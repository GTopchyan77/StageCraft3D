// Copyright Epic Games, Inc. All Rights Reserved.

#include "Audio/StageAudioFileDecoder.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/RunnableThread.h"
#include "Misc/Paths.h"
#include "ModularSceneBuilder.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

#define LOCTEXT_NAMESPACE "StageAudioFileDecoder"

namespace
{
	/** The decoder stays this far ahead of playback, and sleeps while it is. */
	constexpr double AheadSeconds = 1.5;

	/** The ring holds this much, so a long decoded buffer still fits next to what is waiting. */
	constexpr double RingSeconds = 3.0;

	constexpr int64 MaxFileBytes = int64(2) * 1024 * 1024 * 1024;

	/** USoundWaveProcedural plays up to 8 channels. */
	constexpr int32 MaxChannels = 8;

	constexpr float IdleSleepSeconds = 0.010f;

	const TCHAR* const SupportedExtensions[] = { TEXT("wav"), TEXT("wave"), TEXT("flac"), TEXT("mp3"), TEXT("m4a"), TEXT("aac"), TEXT("wma") };
}

TConstArrayView<const TCHAR*> FStageAudioFileDecoder::GetSupportedExtensions()
{
	return MakeArrayView(SupportedExtensions);
}

bool FStageAudioFileDecoder::ValidatePath(const FString& InPath, FText& OutError)
{
	const FString Extension = FPaths::GetExtension(InPath).ToLower();
	if (!GetSupportedExtensions().ContainsByPredicate([&Extension](const TCHAR* Supported) { return Extension == Supported; }))
	{
		OutError = FText::Format(LOCTEXT("Extension", "\"{0}\" is not a music file StageCraft can play. Use WAV, FLAC, MP3, AAC/M4A or WMA."),
			FText::FromString(FPaths::GetCleanFilename(InPath)));
		return false;
	}
	const int64 Size = IFileManager::Get().FileSize(*InPath);
	if (Size < 0)
	{
		OutError = FText::Format(LOCTEXT("Missing", "The file \"{0}\" was not found."), FText::FromString(InPath));
		return false;
	}
	if (Size == 0 || Size > MaxFileBytes)
	{
		OutError = FText::Format(LOCTEXT("Size", "\"{0}\" is empty or larger than 2 GB."), FText::FromString(FPaths::GetCleanFilename(InPath)));
		return false;
	}
	return true;
}

FStageAudioFileDecoder::FStageAudioFileDecoder() = default;

FStageAudioFileDecoder::~FStageAudioFileDecoder()
{
	Shutdown();
}

bool FStageAudioFileDecoder::Start(const FString& InPath, double InStartSeconds, bool bLoop, FOnOpened InOnOpened, FText& OutError)
{
	check(IsInGameThread());
	Shutdown();
#if PLATFORM_WINDOWS
	if (!ValidatePath(InPath, OutError))
	{
		return false;
	}
	Path = FPaths::ConvertRelativePathToFull(InPath);
	StartSeconds = FMath::Max(0.0, FMath::IsFinite(InStartSeconds) ? InStartSeconds : 0.0);
	OnOpened = MoveTemp(InOnOpened);
	bLooping = bLoop;
	bStopping = false;
	Thread.Reset(FRunnableThread::Create(this, TEXT("StageCraft Audio File"), 0, TPri_AboveNormal));
	if (!Thread)
	{
		OutError = LOCTEXT("NoThread", "The music file could not be opened: no decoder thread.");
		OnOpened = nullptr;
		return false;
	}
	return true;
#else
	OutError = LOCTEXT("Unsupported", "Playing music files is not available on this platform.");
	return false;
#endif
}

void FStageAudioFileDecoder::Shutdown()
{
	if (Thread)
	{
		bStopping = true;
		Thread->WaitForCompletion();
		Thread.Reset();
	}
	OnOpened = nullptr;
}

int64 FStageAudioFileDecoder::GetBuffered() const
{
	return WriteIndex.load(std::memory_order_acquire) - ReadIndex.load(std::memory_order_acquire);
}

bool FStageAudioFileDecoder::HasEnded() const
{
	return bEndOfFile.load(std::memory_order_acquire) && !bLooping && GetBuffered() == 0;
}

int32 FStageAudioFileDecoder::Read(float* Out, int32 Frames)
{
	if (!bOpened.load(std::memory_order_acquire) || !Out || Frames <= 0)
	{
		return 0;
	}
	const int32 Channels = Info.Channels;
	const int64 Read = ReadIndex.load(std::memory_order_relaxed);
	const int64 Available = WriteIndex.load(std::memory_order_acquire) - Read;
	const int32 FramesOut = int32(FMath::Min<int64>(Frames, Available / Channels));
	const int64 Count = int64(FramesOut) * Channels;
	const int64 Capacity = Mask + 1;
	const int64 Start = Read & Mask;
	const int64 First = FMath::Min(Count, Capacity - Start);
	FMemory::Memcpy(Out, Ring.GetData() + Start, First * sizeof(float));
	if (Count > First)
	{
		FMemory::Memcpy(Out + First, Ring.GetData(), (Count - First) * sizeof(float));
	}
	ReadIndex.store(Read + Count, std::memory_order_release);
	return FramesOut;
}

bool FStageAudioFileDecoder::FlushPending()
{
	const int64 Capacity = Mask + 1;
	const int64 Write = WriteIndex.load(std::memory_order_relaxed);
	const int64 Free = Capacity - (Write - ReadIndex.load(std::memory_order_acquire));
	// Whole frames only, so the reader never sees half a frame.
	const int64 Count = FMath::Min<int64>(Pending.Num() - PendingOffset, Free) / Info.Channels * Info.Channels;
	for (int64 Index = 0; Index < Count; ++Index)
	{
		Ring[(Write + Index) & Mask] = Pending[PendingOffset + Index];
	}
	WriteIndex.store(Write + Count, std::memory_order_release);
	PendingOffset += int32(Count);
	if (PendingOffset >= Pending.Num())
	{
		Pending.Reset();
		PendingOffset = 0;
		return true;
	}
	return false;
}

uint32 FStageAudioFileDecoder::Run()
{
#if PLATFORM_WINDOWS
	const HRESULT CoResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const bool bMediaFoundation = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
	IMFSourceReader* Reader = nullptr;
	IMFMediaType* Requested = nullptr;
	IMFMediaType* Current = nullptr;
	FText Error;
	auto Fail = [&Error](FText Reason) { Error = MoveTemp(Reason); };

	do
	{
		if (!bMediaFoundation)
		{
			Fail(LOCTEXT("NoMF", "Windows Media Foundation is not available (on Windows N editions install the Media Feature Pack)."));
			break;
		}
		if (FAILED(MFCreateSourceReaderFromURL(*Path, nullptr, &Reader)))
		{
			Fail(FText::Format(LOCTEXT("Open", "\"{0}\" could not be opened as music. It may be damaged or in a format Windows cannot decode."),
				FText::FromString(FPaths::GetCleanFilename(Path))));
			break;
		}
		Reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), 0);
		Reader->SetStreamSelection(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 1);
		// Ask for 32-bit float; the reader inserts the decoder and converter (the file's rate and channels are kept).
		if (FAILED(MFCreateMediaType(&Requested))
			|| FAILED(Requested->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio))
			|| FAILED(Requested->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float))
			|| FAILED(Reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, Requested))
			|| FAILED(Reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &Current)))
		{
			Fail(FText::Format(LOCTEXT("NoAudio", "\"{0}\" has no audio Windows can decode."), FText::FromString(FPaths::GetCleanFilename(Path))));
			break;
		}
		UINT32 Rate = 0;
		UINT32 Channels = 0;
		Current->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &Rate);
		Current->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &Channels);
		if (Rate < 8000 || Rate > 384000 || Channels < 1 || Channels > UINT32(MaxChannels))
		{
			Fail(FText::Format(LOCTEXT("Format", "\"{0}\" has an unsupported format ({1} Hz, {2} channels)."),
				FText::FromString(FPaths::GetCleanFilename(Path)), FText::AsNumber(Rate), FText::AsNumber(Channels)));
			break;
		}

		Info.Path = Path;
		Info.SampleRate = int32(Rate);
		Info.Channels = int32(Channels);
		PROPVARIANT Duration;
		PropVariantInit(&Duration);
		if (SUCCEEDED(Reader->GetPresentationAttribute(DWORD(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &Duration)) && Duration.vt == VT_UI8)
		{
			Info.DurationSeconds = double(Duration.uhVal.QuadPart) * 1e-7; // 100 ns units
		}
		PropVariantClear(&Duration);

		if (StartSeconds > 0.0)
		{
			PROPVARIANT Position;
			PropVariantInit(&Position);
			Position.vt = VT_I8;
			Position.hVal.QuadPart = LONGLONG(StartSeconds * 1e7);
			Reader->SetCurrentPosition(GUID_NULL, Position);
			PropVariantClear(&Position);
		}

		const int32 Capacity = int32(FMath::RoundUpToPowerOfTwo(uint32(RingSeconds * Rate * Channels)));
		Ring.SetNumZeroed(Capacity);
		Mask = int64(Capacity) - 1;
		WriteIndex.store(0, std::memory_order_relaxed);
		ReadIndex.store(0, std::memory_order_relaxed);
		bOpened.store(true, std::memory_order_release);
	}
	while (false);

	if (OnOpened)
	{
		OnOpened(Error.IsEmpty(), Error, Info);
	}

	const int64 AheadSamples = Error.IsEmpty() ? int64(AheadSeconds * Info.SampleRate * Info.Channels) : 0;
	int64 DecodedSinceSeek = 0;
	while (Error.IsEmpty() && !bStopping)
	{
		if (!Pending.IsEmpty() && !FlushPending())
		{
			FPlatformProcess::SleepNoStats(IdleSleepSeconds); // the ring is full: playback catches up
			continue;
		}
		if (GetBuffered() >= AheadSamples)
		{
			FPlatformProcess::SleepNoStats(IdleSleepSeconds);
			continue;
		}
		if (bEndOfFile.load(std::memory_order_acquire))
		{
			if (!bLooping || DecodedSinceSeek == 0)
			{
				FPlatformProcess::SleepNoStats(IdleSleepSeconds); // ended (or a file that yields nothing): wait to be shut down
				continue;
			}
			// Loop: back to the start, seamlessly (the ring still holds the end of the file).
			PROPVARIANT Position;
			PropVariantInit(&Position);
			Position.vt = VT_I8;
			Position.hVal.QuadPart = 0;
			const bool bRewound = SUCCEEDED(Reader->SetCurrentPosition(GUID_NULL, Position));
			PropVariantClear(&Position);
			if (!bRewound)
			{
				bLooping = false;
				continue;
			}
			DecodedSinceSeek = 0;
			bEndOfFile.store(false, std::memory_order_release);
		}

		DWORD Flags = 0;
		LONGLONG Timestamp = 0;
		IMFSample* Sample = nullptr;
		if (FAILED(Reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr, &Flags, &Timestamp, &Sample)))
		{
			UE_LOG(LogStageCraft, Warning, TEXT("Audio file: decoding \"%s\" failed."), *FPaths::GetCleanFilename(Path));
			bFailed.store(true, std::memory_order_release);
			bEndOfFile.store(true, std::memory_order_release);
			bLooping = false;
			break;
		}
		if (Sample)
		{
			IMFMediaBuffer* Buffer = nullptr;
			if (SUCCEEDED(Sample->ConvertToContiguousBuffer(&Buffer)))
			{
				BYTE* Data = nullptr;
				DWORD Length = 0;
				if (SUCCEEDED(Buffer->Lock(&Data, nullptr, &Length)))
				{
					const int32 Floats = int32(Length / sizeof(float));
					Pending.Append(reinterpret_cast<const float*>(Data), Floats);
					DecodedSinceSeek += Floats;
					Buffer->Unlock();
				}
				Buffer->Release();
			}
			Sample->Release();
			FlushPending();
		}
		if (Flags & MF_SOURCE_READERF_ENDOFSTREAM)
		{
			bEndOfFile.store(true, std::memory_order_release);
		}
	}

	if (Current) { Current->Release(); }
	if (Requested) { Requested->Release(); }
	if (Reader) { Reader->Release(); }
	if (bMediaFoundation) { MFShutdown(); }
	if (SUCCEEDED(CoResult)) { CoUninitialize(); }
#endif
	return 0;
}

#undef LOCTEXT_NAMESPACE
