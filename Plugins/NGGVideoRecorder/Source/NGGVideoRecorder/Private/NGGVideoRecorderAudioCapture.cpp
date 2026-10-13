// Copyright NGG. All Rights Reserved.

#include "NGGVideoRecorderAudioCapture.h"

#include "AudioDevice.h"
#include "AudioDeviceHandle.h"
#include "Engine/World.h"
#include "NGGVideoRecorderLog.h"
#include "NGGVideoRecorderWriter.h"
#include "Sound/SoundSubmix.h"

TSharedPtr<FNGGRecorderAudioCapture> FNGGRecorderAudioCapture::CreateAndRegister(
	UWorld* World,
	const TSharedPtr<FNGGRecorderWriter>& InWriter,
	int32& OutSampleRate,
	FString& OutError)
{
	if (!World || !InWriter.IsValid())
	{
		OutError = TEXT("No world or encoder to attach audio capture to.");
		return nullptr;
	}

	FAudioDeviceHandle DeviceHandle = World->GetAudioDevice();
	if (!DeviceHandle.IsValid())
	{
		OutError = TEXT("This world has no audio device (audio may be disabled with -nosound).");
		return nullptr;
	}

	const int32 SampleRate = FMath::RoundToInt(DeviceHandle->GetSampleRate());

	// Media Foundation's AAC encoder accepts only these two rates, and resampling is out of scope here.
	if (SampleRate != 44100 && SampleRate != 48000)
	{
		OutError = FString::Printf(
			TEXT("The audio device runs at %d Hz; the system AAC encoder only accepts 44100 or 48000 Hz."), SampleRate);
		return nullptr;
	}

	TSharedPtr<FNGGRecorderAudioCapture> Capture = MakeShared<FNGGRecorderAudioCapture>();
	Capture->Writer = InWriter;
	Capture->CapturedWorld = World;
	Capture->ExpectedSampleRate = SampleRate;
	Capture->FramesSubmitted = 0;
	Capture->bRunning = true;

	DeviceHandle->RegisterSubmixBufferListener(Capture.ToSharedRef(), DeviceHandle->GetMainSubmixObject());

	OutSampleRate = SampleRate;
	return Capture;
}

void FNGGRecorderAudioCapture::Unregister()
{
	// Stop feeding the encoder first. The audio engine may still call back while it detaches the listener,
	// and those callbacks now return immediately.
	const bool bWasRunning = bRunning.exchange(false);

	if (UWorld* World = CapturedWorld.Get())
	{
		FAudioDeviceHandle DeviceHandle = World->GetAudioDevice();
		if (DeviceHandle.IsValid() && bWasRunning)
		{
			DeviceHandle->UnregisterSubmixBufferListener(AsShared(), DeviceHandle->GetMainSubmixObject());
		}
	}

	CapturedWorld.Reset();
	Writer.Reset();
}

const FString& FNGGRecorderAudioCapture::GetListenerName() const
{
	static const FString Name = TEXT("NGGVideoRecorder");
	return Name;
}

void FNGGRecorderAudioCapture::OnNewSubmixBuffer(
	const USoundSubmix* /*OwningSubmix*/,
	float* AudioData,
	int32 NumSamples,
	int32 NumChannels,
	const int32 SampleRate,
	double /*AudioClock*/)
{
	if (!bRunning || !AudioData || NumSamples <= 0 || NumChannels <= 0 || !Writer.IsValid())
	{
		return;
	}

	// The encoder was configured for one rate; if the device switched underneath us the track would drift,
	// so stop contributing rather than write audio that slowly desynchronises.
	if (SampleRate != ExpectedSampleRate)
	{
		return;
	}

	const int32 NumFrames = NumSamples / NumChannels;
	if (NumFrames <= 0)
	{
		return;
	}

	TUniquePtr<FNGGAudioChunk> Chunk = MakeUnique<FNGGAudioChunk>();
	Chunk->Samples.SetNumUninitialized(NumFrames * OutputChannels);
	Chunk->TimeNs = (FramesSubmitted * 1000000000LL) / ExpectedSampleRate;

	int16* Out = Chunk->Samples.GetData();

	for (int32 Frame = 0; Frame < NumFrames; ++Frame)
	{
		const float* In = AudioData + (int64)Frame * NumChannels;

		float Left;
		float Right;

		if (NumChannels == 1)
		{
			Left = Right = In[0];
		}
		else
		{
			Left = In[0];
			Right = In[1];

			// Fold the centre channel in at -3 dB so dialogue and engine sound survive a surround mix.
			// Surround channels are dropped; a full downmix matrix is more than this needs.
			if (NumChannels >= 3)
			{
				const float Centre = In[2] * 0.7071f;
				Left += Centre;
				Right += Centre;
			}
		}

		Out[Frame * OutputChannels + 0] = (int16)(FMath::Clamp(Left, -1.0f, 1.0f) * 32767.0f);
		Out[Frame * OutputChannels + 1] = (int16)(FMath::Clamp(Right, -1.0f, 1.0f) * 32767.0f);
	}

	FramesSubmitted += NumFrames;

	Writer->SubmitAudio(MoveTemp(Chunk));
}
