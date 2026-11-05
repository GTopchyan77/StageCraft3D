// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Sound/SoundWaveProcedural.h"
#include "Templates/SharedPointer.h"

#include <atomic>

#include "StageAudioFileSound.generated.h"

/**
 * Plays a decoded music file (Docs/ADR/0029-audio-analysis-and-audio-reactive-effects.md §2.1, §3 File). The mixer pulls the
 * samples on the audio render thread (OnGeneratePCMAudio). The same samples go to the analysis tap at that moment, which is what
 * the mixer renders; the audience hears them after the engine's output buffering (FileOutputDelayMs), so the analysis runs
 * ahead of the sound and can be released in time with it.
 *
 * Plays at the file's own rate and channel count; the mixer converts for the device. Silence is played (and counted as an
 * underrun) when the decoder is behind, and after the end of a file that does not loop.
 *
 * Threads: Setup runs on the game thread before the sound plays; OnGeneratePCMAudio on the audio render thread; the counters
 * are atomics. The decoder and the tap are shared, so they live as long as this sound, whatever stops first. Created per file
 * by UStageAudioReactiveSubsystem and never saved.
 */
UCLASS(Transient)
class UStageAudioFileSound : public USoundWaveProcedural
{
	GENERATED_BODY()

public:
	UStageAudioFileSound(const FObjectInitializer& ObjectInitializer);

	void Setup(const TSharedRef<class FStageAudioFileDecoder>& InDecoder, const TSharedRef<class FStageAudioTap>& InTap, int32 InSampleRate, int32 InChannels);

	/** Frames handed to the mixer since Setup, silence included. */
	int64 GetFramesRendered() const { return FramesRendered.load(std::memory_order_relaxed); }

	/** Frames of the file handed to the mixer since Setup (silence excluded): the playback position. */
	int64 GetFramesPlayed() const { return FramesPlayed.load(std::memory_order_relaxed); }

	int64 GetUnderruns() const { return Underruns.load(std::memory_order_relaxed); }

	//~ Begin USoundWave Interface
	virtual int32 OnGeneratePCMAudio(TArray<uint8>& OutAudio, int32 NumSamples) override;
	virtual Audio::EAudioMixerStreamDataFormat::Type GetGeneratedPCMDataFormat() const override;
	//~ End USoundWave Interface

private:
	TSharedPtr<class FStageAudioFileDecoder> Decoder;
	TSharedPtr<class FStageAudioTap> Tap;
	int32 FileChannels = 2;

	std::atomic<int64> FramesRendered{ 0 };
	std::atomic<int64> FramesPlayed{ 0 };
	std::atomic<int64> Underruns{ 0 };
};
