// Copyright Epic Games, Inc. All Rights Reserved.

#include "Audio/StageAudioFileSound.h"

#include "Audio/StageAudioFileDecoder.h"
#include "Audio/StageAudioTap.h"
#include "HAL/PlatformTime.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageAudioFileSound)

UStageAudioFileSound::UStageAudioFileSound(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bLooping = false; // the decoder loops the file; the wave itself never ends while playing
	bProcedural = true;
	Duration = INDEFINITELY_LOOPING_DURATION;
	SoundGroup = SOUNDGROUP_Music;
	VirtualizationMode = EVirtualizationMode::PlayWhenSilent; // a quiet passage must not stop the music (or the analysis)
}

void UStageAudioFileSound::Setup(const TSharedRef<FStageAudioFileDecoder>& InDecoder, const TSharedRef<FStageAudioTap>& InTap, int32 InSampleRate, int32 InChannels)
{
	check(IsInGameThread());
	Decoder = InDecoder;
	Tap = InTap;
	FileChannels = FMath::Clamp(InChannels, 1, 8);
	SetSampleRate(uint32(FMath::Max(InSampleRate, 8000)));
	NumChannels = FileChannels;
}

Audio::EAudioMixerStreamDataFormat::Type UStageAudioFileSound::GetGeneratedPCMDataFormat() const
{
	return Audio::EAudioMixerStreamDataFormat::Float;
}

int32 UStageAudioFileSound::OnGeneratePCMAudio(TArray<uint8>& OutAudio, int32 NumSamples)
{
	const int32 Frames = NumSamples / FileChannels;
	if (Frames <= 0)
	{
		return 0;
	}
	const int32 Samples = Frames * FileChannels;
	// Grows once to the mixer's callback size and is then reused (the engine never shrinks it).
	OutAudio.SetNumUninitialized(Samples * int32(sizeof(float)), EAllowShrinking::No);
	float* Out = reinterpret_cast<float*>(OutAudio.GetData());

	const double Now = FPlatformTime::Seconds();
	const int32 Got = Decoder.IsValid() ? Decoder->Read(Out, Frames) : 0;
	if (Got < Frames)
	{
		FMemory::Memzero(Out + int64(Got) * FileChannels, int64(Frames - Got) * FileChannels * sizeof(float));
		if (Decoder.IsValid() && !Decoder->HasEnded())
		{
			Underruns.fetch_add(1, std::memory_order_relaxed);
		}
	}
	if (Tap.IsValid())
	{
		Tap->Write(Out, Frames, FileChannels, Now);
	}
	FramesRendered.fetch_add(Frames, std::memory_order_relaxed);
	FramesPlayed.fetch_add(Got, std::memory_order_relaxed);
	return Samples;
}
