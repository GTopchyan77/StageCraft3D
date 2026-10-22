// Copyright Epic Games, Inc. All Rights Reserved.

#include "AI/StageVoiceRecorder.h"

#include "Misc/ScopeLock.h"
#include "ModularSceneBuilder.h"

#define LOCTEXT_NAMESPACE "StageVoiceRecorder"

FStageVoiceRecorder::~FStageVoiceRecorder()
{
	Abort();
}

bool FStageVoiceRecorder::Start(float InMaxSeconds, FText& OutError)
{
	check(IsInGameThread());
	Abort();

	TUniquePtr<Audio::FAudioCapture> NewCapture = MakeUnique<Audio::FAudioCapture>();
	Audio::FCaptureDeviceInfo Device;
	if (!NewCapture->GetCaptureDeviceInfo(Device, Audio::DefaultDeviceIndex))
	{
		OutError = LOCTEXT("NoDevice", "No microphone was found. Connect one and choose it as the default recording device in Windows.");
		return false;
	}
	{
		FScopeLock ScopeLock(&Lock);
		Samples.Reset();
		SampleRate = Device.PreferredSampleRate;
		MaxSeconds = FMath::Max(double(InMaxSeconds), 1.0);
		bAccepting = true;
		Samples.Reserve(int32(FMath::Min(MaxSeconds * double(FMath::Max(SampleRate, 8000)), double(MAX_int32 / 2))));
	}

	// The default device and format: 32-bit float, the device's channels and rate.
	Audio::FAudioCaptureDeviceParams Params;
	const bool bOpened = NewCapture->OpenAudioCaptureStream(Params,
		[this](const void* Audio, int32 NumFrames, int32 NumChannels, int32 InSampleRate, double, bool)
		{
			HandleAudio(Audio, NumFrames, NumChannels, InSampleRate);
		}, 1024);
	if (!bOpened || !NewCapture->StartStream())
	{
		NewCapture->AbortStream();
		OutError = LOCTEXT("NotOpened", "The microphone could not be opened. Check that apps may use it (Windows Settings › Privacy › Microphone).");
		return false;
	}
	Capture = MoveTemp(NewCapture);
	UE_LOG(LogStageCraft, Log, TEXT("Voice: recording from \"%s\" (%d Hz, %d channels), at most %.0f s."), *Device.DeviceName, Device.PreferredSampleRate,
		Device.InputChannels, InMaxSeconds);
	return true;
}

void FStageVoiceRecorder::HandleAudio(const void* Audio, int32 NumFrames, int32 NumChannels, int32 InSampleRate)
{
	if (!Audio || NumFrames <= 0 || NumChannels <= 0)
	{
		return;
	}
	// The stream was opened with the default 32-bit float encoding.
	const float* Interleaved = static_cast<const float*>(Audio);
	FScopeLock ScopeLock(&Lock);
	if (!bAccepting)
	{
		return;
	}
	if (InSampleRate > 0)
	{
		SampleRate = InSampleRate;
	}
	const int32 MaxSamples = int32(FMath::Min(MaxSeconds * double(FMath::Max(SampleRate, 1)), double(MAX_int32 / 2)));
	const int32 Frames = FMath::Min(NumFrames, MaxSamples - Samples.Num());
	for (int32 Frame = 0; Frame < Frames; ++Frame)
	{
		float Sum = 0.f;
		for (int32 Channel = 0; Channel < NumChannels; ++Channel)
		{
			Sum += Interleaved[Frame * NumChannels + Channel];
		}
		Samples.Add(Sum / float(NumChannels));
	}
}

void FStageVoiceRecorder::CloseStream()
{
	// A callback the device thread delivers while the stream is being stopped finds this cleared and touches nothing.
	{
		FScopeLock ScopeLock(&Lock);
		bAccepting = false;
	}
	if (Capture.IsValid())
	{
		Capture->StopStream();
		Capture->CloseStream();
		Capture.Reset();
	}
}

FStageVoiceClip FStageVoiceRecorder::Stop()
{
	check(IsInGameThread());
	FStageVoiceClip Clip;
	if (!Capture.IsValid())
	{
		return Clip;
	}
	CloseStream();
	FScopeLock ScopeLock(&Lock);
	Clip.Samples = MoveTemp(Samples);
	Clip.SampleRate = SampleRate;
	Samples.Reset();
	return Clip;
}

void FStageVoiceRecorder::Abort()
{
	CloseStream();
	FScopeLock ScopeLock(&Lock);
	Samples.Empty();
}

#undef LOCTEXT_NAMESPACE
