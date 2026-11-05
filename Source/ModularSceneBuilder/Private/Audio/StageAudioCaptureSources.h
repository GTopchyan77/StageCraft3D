// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AudioCaptureCore.h"
#include "HAL/CriticalSection.h"
#include "HAL/Runnable.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"

#include <atomic>

/**
 * The SystemAudio source (Docs/ADR/0029-audio-analysis-and-audio-reactive-effects.md §2 A4, §3): captures what this PC plays
 * through WASAPI loopback of the default playback device, in shared mode, on a thread of its own. This is how YouTube, Spotify or
 * any player drives the lights without StageCraft touching them. Analysed only: nothing is played back, recorded or kept.
 *
 * Threads:
 * - Start and Shutdown run on the game thread.
 * - The capture thread opens the device. It sizes the tap for the device's rate (FStageAudioTap::Init) before writing to it,
 *   then reports once through OnOpened, on the capture thread; the owner moves the result to the game thread.
 * - The capture thread then writes 10 ms packets into the tap, stamped with the device's own capture time (QPC position).
 * If the device goes away (unplugged, the default device changed), HasFailed turns true with an error and capture stops.
 * Shutdown joins the thread.
 *
 * Other platforms: Start fails with "not available on this platform". G0 measured it (ADR 0029 §2.1): 48 kHz float, 10 ms packets,
 * steady timestamps, in Development and Shipping.
 */
class FStageLoopbackCapture : private FRunnable
{
public:
	using FOnOpened = TFunction<void(bool bOk, const FText& Error, const FString& Description, int32 SampleRate, int32 Channels)>;

	FStageLoopbackCapture();
	virtual ~FStageLoopbackCapture() override;
	FStageLoopbackCapture(const FStageLoopbackCapture&) = delete;
	FStageLoopbackCapture& operator=(const FStageLoopbackCapture&) = delete;

	/** False, with OutError, when the platform has no loopback or the thread cannot start (OnOpened is then never called). */
	bool Start(const TSharedRef<class FStageAudioTap>& Tap, FOnOpened OnOpened, FText& OutError);

	/** Joins the thread. Safe when not started and when called twice. */
	void Shutdown();

	bool HasFailed() const { return bFailed.load(std::memory_order_acquire); }

	/** The reason HasFailed. */
	FText GetError() const;

private:
	//~ Begin FRunnable Interface
	virtual uint32 Run() override;
	virtual void Stop() override { bStopping = true; }
	//~ End FRunnable Interface

	void SetFailed(const FText& Error);

	TSharedPtr<class FStageAudioTap> Tap;
	FOnOpened OnOpened;
	TUniquePtr<class FRunnableThread> Thread;
	std::atomic<bool> bStopping{ false };
	std::atomic<bool> bFailed{ false };
	mutable FCriticalSection ErrorLock;
	FText Error;
};

/**
 * The Input source (ADR 0029 §3): a capture device (line in, audio interface, microphone) through the engine's AudioCaptureCore,
 * as StageVoiceRecorder records voice prompts. Analysed only, never played back, so there is no feedback loop.
 *
 * Threads: Start and Shutdown run on the game thread. The device's callback thread writes into the tap until Shutdown; a callback
 * delivered while the stream is closing is ignored.
 */
class FStageAudioInputCapture
{
public:
	~FStageAudioInputCapture();

	/**
	 * Opens DeviceId (AudioCaptureCore's id; empty or not found = the default recording device) and starts writing into Tap,
	 * which this sizes for the device's rate. GainDb is applied before analysis. False, with a user-facing reason, when it cannot.
	 */
	bool Start(const TSharedRef<class FStageAudioTap>& Tap, const FString& DeviceId, float GainDb, FString& OutDescription, FText& OutError);

	/** Safe when not started and when called twice. */
	void Shutdown();

	bool IsRunning() const { return Capture.IsValid(); }

	/** "Microphone (USB Audio)" for each capture device, with its id. */
	static TArray<TPair<FString, FString>> ListDevices();

private:
	TUniquePtr<Audio::FAudioCapture> Capture;
	TSharedPtr<class FStageAudioTap> Tap;
	float Gain = 1.f;
	std::atomic<bool> bAccepting{ false };
};
