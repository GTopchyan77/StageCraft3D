// Copyright Epic Games, Inc. All Rights Reserved.

#include "Audio/StageAudioCaptureSources.h"

#include "Audio/StageAudioTap.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/RunnableThread.h"
#include "Misc/ScopeLock.h"
#include "ModularSceneBuilder.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

#define LOCTEXT_NAMESPACE "StageAudioCapture"

namespace
{
	/** Room in the tap: 2.7 s at 48 kHz, far more than the analyser ever lags. */
	constexpr int32 TapCapacitySamples = 1 << 17;

	/**
	 * How often the capture thread drains the device. Packets arrive once per device period (10 ms on most PCs) and WASAPI keeps
	 * BufferDuration100ns of slack, so half a period loses no audio and adds at most that much latency, with 40% of the wake-ups of
	 * the fixed 2 ms poll it replaces (STATE.md #88 measures both).
	 */
	TAutoConsoleVariable<float> CVarLoopbackPollMs(TEXT("StageCraft.Audio.LoopbackPollMs"), 0.f,
		TEXT("How often the System audio thread drains the playback device, in ms (1-20). 0 = half the device period. Read when System audio starts. Development tuning only."),
		ECVF_Cheat);

	/** The poll interval for a device whose default period is DevicePeriod100ns. */
	float GetPollSeconds(int64 DevicePeriod100ns)
	{
		const float Override = CVarLoopbackPollMs.GetValueOnAnyThread();
		const float Ms = Override > 0.f ? Override : (DevicePeriod100ns > 0 ? float(DevicePeriod100ns) * 1e-4f * 0.5f : 5.f);
		return FMath::Clamp(Ms, 1.f, 20.f) * 1e-3f;
	}

	/** WASAPI's own buffer: 200 ms of slack if the thread is held up. */
	constexpr int64 BufferDuration100ns = 2000000;
}

// --- System audio (WASAPI loopback) ---

FStageLoopbackCapture::FStageLoopbackCapture() = default;

FStageLoopbackCapture::~FStageLoopbackCapture()
{
	Shutdown();
}

bool FStageLoopbackCapture::Start(const TSharedRef<FStageAudioTap>& InTap, FOnOpened InOnOpened, FText& OutError)
{
	check(IsInGameThread());
	Shutdown();
#if PLATFORM_WINDOWS
	Tap = InTap;
	OnOpened = MoveTemp(InOnOpened);
	bStopping = false;
	bFailed = false;
	Thread.Reset(FRunnableThread::Create(this, TEXT("StageCraft System Audio"), 0, TPri_AboveNormal));
	if (!Thread)
	{
		OutError = LOCTEXT("NoThread", "System audio could not be captured: no capture thread.");
		OnOpened = nullptr;
		Tap.Reset();
		return false;
	}
	return true;
#else
	OutError = LOCTEXT("Unsupported", "Listening to system audio is only available on Windows.");
	return false;
#endif
}

void FStageLoopbackCapture::Shutdown()
{
	if (Thread)
	{
		bStopping = true;
		Thread->WaitForCompletion();
		Thread.Reset();
	}
	OnOpened = nullptr;
	Tap.Reset();
}

FText FStageLoopbackCapture::GetError() const
{
	FScopeLock Guard(&ErrorLock);
	return Error;
}

void FStageLoopbackCapture::SetFailed(const FText& InError)
{
	{
		FScopeLock Guard(&ErrorLock);
		Error = InError;
	}
	bFailed.store(true, std::memory_order_release);
}

uint32 FStageLoopbackCapture::Run()
{
#if PLATFORM_WINDOWS
	const HRESULT CoResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	IMMDeviceEnumerator* Enumerator = nullptr;
	IMMDevice* Device = nullptr;
	IAudioClient* Client = nullptr;
	IAudioCaptureClient* CaptureClient = nullptr;
	WAVEFORMATEX* Format = nullptr;
	FText OpenError;
	bool bFloat = false;
	bool bPcm16 = false;
	int32 Rate = 0;
	int32 Channels = 0;
	float PollSeconds = GetPollSeconds(0);

	do
	{
		if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&Enumerator)))
			|| FAILED(Enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &Device)))
		{
			OpenError = LOCTEXT("NoDevice", "There is no playback device to listen to. Connect speakers or headphones and choose them in Windows.");
			break;
		}
		if (FAILED(Device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&Client)))
			|| FAILED(Client->GetMixFormat(&Format))
			|| FAILED(Client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, BufferDuration100ns, 0, Format, nullptr))
			|| FAILED(Client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&CaptureClient))))
		{
			OpenError = LOCTEXT("NoLoopback", "Windows did not allow listening to the playback device.");
			break;
		}
		// {00000003-0000-0010-8000-00AA00389B71}: KSDATAFORMAT_SUBTYPE_IEEE_FLOAT.
		static const GUID FloatSubFormat = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
		bFloat = Format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT
			|| (Format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && IsEqualGUID(reinterpret_cast<WAVEFORMATEXTENSIBLE*>(Format)->SubFormat, FloatSubFormat) != 0);
		bPcm16 = !bFloat && Format->wBitsPerSample == 16;
		Rate = int32(Format->nSamplesPerSec);
		Channels = int32(Format->nChannels);
		if ((!bFloat && !bPcm16) || Rate < 8000 || Channels < 1)
		{
			OpenError = LOCTEXT("Format", "The playback device uses a sample format StageCraft cannot read.");
			break;
		}
		REFERENCE_TIME DefaultPeriod = 0;
		REFERENCE_TIME MinimumPeriod = 0;
		PollSeconds = GetPollSeconds(SUCCEEDED(Client->GetDevicePeriod(&DefaultPeriod, &MinimumPeriod)) ? int64(DefaultPeriod) : 0);
		Tap->Init(TapCapacitySamples, Rate);
		if (FAILED(Client->Start()))
		{
			OpenError = LOCTEXT("NoStart", "Listening to the playback device could not start.");
			break;
		}
	}
	while (false);

	if (OnOpened)
	{
		OnOpened(OpenError.IsEmpty(), OpenError, FString::Printf(TEXT("Default playback device, %d Hz, %d ch"), Rate, Channels), Rate, Channels);
	}
	if (!OpenError.IsEmpty())
	{
		SetFailed(OpenError);
	}

	LARGE_INTEGER Frequency;
	QueryPerformanceFrequency(&Frequency);
	TArray<float> Converted;
	const double Started = FPlatformTime::Seconds();
	int64 Wakeups = 0;
	ULONG64 CyclesAtStart = 0;
	QueryThreadCycleTime(GetCurrentThread(), &CyclesAtStart);
	while (OpenError.IsEmpty() && !bStopping)
	{
		FPlatformProcess::SleepNoStats(PollSeconds);
		++Wakeups;
		UINT32 Next = 0;
		HRESULT Result = S_OK;
		while (!bStopping && SUCCEEDED(Result = CaptureClient->GetNextPacketSize(&Next)) && Next > 0)
		{
			BYTE* Data = nullptr;
			UINT32 Frames = 0;
			DWORD Flags = 0;
			UINT64 DevicePosition = 0;
			UINT64 QpcPosition = 0;
			Result = CaptureClient->GetBuffer(&Data, &Frames, &Flags, &DevicePosition, &QpcPosition);
			if (FAILED(Result))
			{
				break;
			}
			// QpcPosition is the capture time of the first frame in 100 ns units of the performance counter; place it on our clock.
			LARGE_INTEGER Counter;
			QueryPerformanceCounter(&Counter);
			const double Now100ns = double(Counter.QuadPart) * 1e7 / double(Frequency.QuadPart);
			const double Time = FPlatformTime::Seconds() - FMath::Max(0.0, Now100ns - double(QpcPosition)) * 1e-7;
			if (Flags & AUDCLNT_BUFFERFLAGS_SILENT)
			{
				Tap->Write(nullptr, int32(Frames), Channels, Time);
			}
			else if (bFloat)
			{
				Tap->Write(reinterpret_cast<const float*>(Data), int32(Frames), Channels, Time);
			}
			else
			{
				// Grows to the largest packet once (10 ms of audio), then reused.
				Converted.SetNumUninitialized(int32(Frames) * Channels, EAllowShrinking::No);
				const int16* Pcm = reinterpret_cast<const int16*>(Data);
				for (int32 Index = 0; Index < Converted.Num(); ++Index)
				{
					Converted[Index] = float(Pcm[Index]) / 32768.f;
				}
				Tap->Write(Converted.GetData(), int32(Frames), Channels, Time);
			}
			CaptureClient->ReleaseBuffer(Frames);
		}
		if (FAILED(Result))
		{
			UE_LOG(LogStageCraft, Warning, TEXT("Audio: system audio capture stopped (0x%08x)."), uint32(Result));
			SetFailed(Result == AUDCLNT_E_DEVICE_INVALIDATED
				? LOCTEXT("DeviceChanged", "The playback device changed or was unplugged. Choose System audio again to listen to the new one.")
				: LOCTEXT("CaptureFailed", "Listening to the playback device stopped with an error."));
			break;
		}
	}

	// What listening cost this thread, so the poll interval is chosen on measurements. Cycle counts are exact; GetThreadTimes only
	// samples at the scheduler tick (15.6 ms) and reads 0 for a thread that wakes for microseconds.
	ULONG64 CyclesNow = 0;
	if (OpenError.IsEmpty() && QueryThreadCycleTime(GetCurrentThread(), &CyclesNow))
	{
		const double Seconds = FMath::Max(1e-3, FPlatformTime::Seconds() - Started);
		const double Cycles = double(CyclesNow - CyclesAtStart);
		UE_LOG(LogStageCraft, Log, TEXT("Audio: system audio thread ran %.1f s polling every %.1f ms: %lld wake-ups, %.2f M cycles (%.3f M cycles per second, %.0f cycles per wake-up)."),
			Seconds, PollSeconds * 1000.0, Wakeups, Cycles * 1e-6, Cycles * 1e-6 / Seconds, Wakeups > 0 ? Cycles / double(Wakeups) : 0.0);
	}

	if (Client) { Client->Stop(); }
	if (CaptureClient) { CaptureClient->Release(); }
	if (Client) { Client->Release(); }
	if (Device) { Device->Release(); }
	if (Enumerator) { Enumerator->Release(); }
	if (Format) { CoTaskMemFree(Format); }
	if (SUCCEEDED(CoResult)) { CoUninitialize(); }
#endif
	return 0;
}

// --- Input (AudioCaptureCore) ---

FStageAudioInputCapture::~FStageAudioInputCapture()
{
	Shutdown();
}

TArray<TPair<FString, FString>> FStageAudioInputCapture::ListDevices()
{
	TArray<TPair<FString, FString>> Out;
	Audio::FAudioCapture Probe;
	TArray<Audio::FCaptureDeviceInfo> Devices;
	Probe.GetCaptureDevicesAvailable(Devices);
	for (const Audio::FCaptureDeviceInfo& Device : Devices)
	{
		Out.Emplace(Device.DeviceName, Device.DeviceId);
	}
	return Out;
}

bool FStageAudioInputCapture::Start(const TSharedRef<FStageAudioTap>& InTap, const FString& DeviceId, float GainDb, FString& OutDescription, FText& OutError)
{
	check(IsInGameThread());
	Shutdown();

	TUniquePtr<Audio::FAudioCapture> NewCapture = MakeUnique<Audio::FAudioCapture>();
	int32 DeviceIndex = Audio::DefaultDeviceIndex;
	if (!DeviceId.IsEmpty())
	{
		TArray<Audio::FCaptureDeviceInfo> Devices;
		NewCapture->GetCaptureDevicesAvailable(Devices);
		DeviceIndex = Devices.IndexOfByPredicate([&DeviceId](const Audio::FCaptureDeviceInfo& Device) { return Device.DeviceId == DeviceId; });
		if (DeviceIndex == INDEX_NONE)
		{
			UE_LOG(LogStageCraft, Log, TEXT("Audio: the chosen input device is not connected; using the default recording device."));
			DeviceIndex = Audio::DefaultDeviceIndex;
		}
	}
	Audio::FCaptureDeviceInfo Device;
	if (!NewCapture->GetCaptureDeviceInfo(Device, DeviceIndex) || Device.PreferredSampleRate < 8000)
	{
		OutError = LOCTEXT("NoInput", "No recording device was found. Connect a line input, audio interface or microphone and choose it in Windows.");
		return false;
	}

	InTap->Init(TapCapacitySamples, Device.PreferredSampleRate);
	Tap = InTap;
	Gain = FMath::Pow(10.f, FMath::Clamp(GainDb, -24.f, 24.f) / 20.f);
	bAccepting = true;

	Audio::FAudioCaptureDeviceParams Params;
	Params.DeviceIndex = DeviceIndex;
	const int32 Rate = Device.PreferredSampleRate;
	const bool bOpened = NewCapture->OpenAudioCaptureStream(Params,
		[this, Rate, TapRef = InTap, GainValue = Gain](const void* Audio, int32 NumFrames, int32 NumChannels, int32 SampleRate, double, bool)
		{
			// The stream was opened with the default 32-bit float encoding. The block ends now; its first frame was NumFrames ago.
			if (!bAccepting.load(std::memory_order_acquire) || !Audio || NumFrames <= 0 || NumChannels <= 0 || SampleRate != Rate)
			{
				return;
			}
			const double Time = FPlatformTime::Seconds() - double(NumFrames) / double(Rate);
			TapRef->Write(static_cast<const float*>(Audio), NumFrames, NumChannels, Time, GainValue);
		}, 480);
	if (!bOpened || !NewCapture->StartStream())
	{
		bAccepting = false;
		NewCapture->AbortStream();
		Tap.Reset();
		OutError = LOCTEXT("InputNotOpened", "The recording device could not be opened. Check that apps may use it (Windows Settings › Privacy › Microphone).");
		return false;
	}
	Capture = MoveTemp(NewCapture);
	OutDescription = FString::Printf(TEXT("%s, %d Hz, %d ch"), *Device.DeviceName, Device.PreferredSampleRate, Device.InputChannels);
	return true;
}

void FStageAudioInputCapture::Shutdown()
{
	// A callback the device thread delivers while the stream closes finds this cleared and writes nothing.
	bAccepting = false;
	if (Capture.IsValid())
	{
		Capture->StopStream();
		Capture->CloseStream();
		Capture.Reset();
	}
	Tap.Reset();
}

#undef LOCTEXT_NAMESPACE
