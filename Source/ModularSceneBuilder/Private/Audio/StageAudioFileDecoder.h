// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "Templates/Function.h"
#include "Templates/UniquePtr.h"

#include <atomic>

/** What the decoder found in a file once it opened it. */
struct FStageAudioFileInfo
{
	FString Path;
	int32 SampleRate = 0;
	int32 Channels = 0;
	/** 0 when the file does not say. */
	double DurationSeconds = 0.0;
};

/**
 * Decodes a music file for the File source (Docs/ADR/0029-audio-analysis-and-audio-reactive-effects.md §2.1, §3) with Windows
 * Media Foundation's source reader: WAV, FLAC, MP3, AAC/M4A and WMA to interleaved float, on a thread of its own, into a ring that
 * it keeps about 1.5 s ahead of playback. Phase G0 measured it at 150-370 times real time in Development and Shipping.
 *
 * Threads:
 * - Start, Shutdown and SetLoop run on the game thread.
 * - The decoder thread opens the file and fills the ring. It reports the result once through OnOpened, on the decoder thread;
 *   the owner moves the result to the game thread.
 * - Read runs on the audio render thread (UStageAudioFileSound), the ring's only reader. It never blocks: it returns what is
 *   decoded, and the caller plays silence for the rest.
 * Shutdown joins the thread. The ring stays allocated until the decoder is destroyed, so a render callback that is still running
 * reads nothing harmful. The owner and the sound share the decoder (TSharedRef), so it outlives both.
 *
 * Other platforms: Start fails with "not available on this platform".
 */
class FStageAudioFileDecoder : private FRunnable
{
public:
	using FOnOpened = TFunction<void(bool bOk, const FText& Error, const FStageAudioFileInfo& Info)>;

	/** Lower-case extensions the File source accepts. */
	static TConstArrayView<const TCHAR*> GetSupportedExtensions();

	/** The checks made before any decoding: the file exists, is not empty or over 2 GB, and has a supported extension. */
	static bool ValidatePath(const FString& Path, FText& OutError);

	FStageAudioFileDecoder();
	virtual ~FStageAudioFileDecoder() override;
	FStageAudioFileDecoder(const FStageAudioFileDecoder&) = delete;
	FStageAudioFileDecoder& operator=(const FStageAudioFileDecoder&) = delete;

	/**
	 * Validates Path and starts the thread, which opens the file at StartSeconds and calls OnOpened once. False, with OutError,
	 * when the path is refused or the thread cannot start (OnOpened is then never called).
	 */
	bool Start(const FString& Path, double StartSeconds, bool bLoop, FOnOpened OnOpened, FText& OutError);

	/** Joins the thread. Safe when not started and when called twice. */
	void Shutdown();

	void SetLoop(bool bLoop) { bLooping = bLoop; }

	/** Audio render thread: up to Frames frames of interleaved float into Out; returns the frames written. */
	int32 Read(float* Out, int32 Frames);

	/** True once the file has been opened (Info is then valid). */
	bool IsOpen() const { return bOpened.load(std::memory_order_acquire); }

	/** Valid once IsOpen. */
	const FStageAudioFileInfo& GetInfo() const { return Info; }

	/** The end was reached without looping and everything decoded has been read. */
	bool HasEnded() const;

	/** Decoding failed after the file opened (a corrupt frame, a removed drive). */
	bool HasFailed() const { return bFailed.load(std::memory_order_acquire); }

private:
	//~ Begin FRunnable Interface
	virtual uint32 Run() override;
	virtual void Stop() override { bStopping = true; }
	//~ End FRunnable Interface

	/** Appends as much of Pending as fits; true when all of it did. */
	bool FlushPending();
	int64 GetBuffered() const;

	FString Path;
	double StartSeconds = 0.0;
	FOnOpened OnOpened;
	FStageAudioFileInfo Info;

	TUniquePtr<class FRunnableThread> Thread;
	std::atomic<bool> bStopping{ false };
	std::atomic<bool> bLooping{ true };
	std::atomic<bool> bOpened{ false };
	std::atomic<bool> bEndOfFile{ false };
	std::atomic<bool> bFailed{ false };

	/** Interleaved samples; the decoder thread writes, the render thread reads. Allocated by the thread before bOpened. */
	TArray<float> Ring;
	int64 Mask = 0;
	std::atomic<int64> WriteIndex{ 0 };
	std::atomic<int64> ReadIndex{ 0 };

	/** The decoder thread's: a decoded buffer that did not fit in the ring yet. */
	TArray<float> Pending;
	int32 PendingOffset = 0;
};
