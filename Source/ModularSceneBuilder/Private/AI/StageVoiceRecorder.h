// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AI/StageSpeechTypes.h"
#include "AudioCaptureCore.h"
#include "HAL/CriticalSection.h"

/**
 * Records one voice prompt from the default microphone (Docs/ADR/0017-multilingual-voice-and-photo-import.md §2.5), through the
 * engine's capture backend (AudioCaptureCore; the AudioCapture plugin registers WASAPI on Windows).
 *
 * Threads: Start, Stop and Abort run on the game thread. The capture callback runs on the device's thread and only appends mono
 * samples under Lock, up to the length given to Start; past that it drops audio until Stop. Stop and Abort first stop accepting
 * audio (under Lock), then close the stream, so a callback that is still delivered afterwards changes nothing; the destructor
 * aborts. Owned by UStageVoiceInputComponent.
 */
class FStageVoiceRecorder
{
public:
	~FStageVoiceRecorder();

	/** Opens the default microphone and starts recording, at most MaxSeconds. False, with a user-facing reason, when it cannot. */
	bool Start(float MaxSeconds, FText& OutError);

	/** Stops recording and hands over what was recorded. Empty when nothing was recording. */
	FStageVoiceClip Stop();

	/** Stops recording and drops it. Safe when nothing is recording. */
	void Abort();

	bool IsRecording() const { return Capture.IsValid(); }

private:
	void HandleAudio(const void* Audio, int32 NumFrames, int32 NumChannels, int32 SampleRate);
	void CloseStream();

	TUniquePtr<Audio::FAudioCapture> Capture;

	/** Guards everything below: the device thread writes it, the game thread starts, stops and reads it. */
	FCriticalSection Lock;
	TArray<float> Samples;
	int32 SampleRate = 0;
	double MaxSeconds = 0.0;

	/** Cleared before the stream is stopped, so a late callback never touches the samples being handed over. */
	bool bAccepting = false;
};
