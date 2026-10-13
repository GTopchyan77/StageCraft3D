// Copyright NGG. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "HAL/Runnable.h"
#include "NGGVideoRecorderEncoder.h"

#include <atomic>

class FRunnableThread;

/** One captured frame, BGRA8, top-down, tightly packed. Buffers are pooled and reused. */
struct FNGGCapturedFrame
{
	TArray<uint8> Pixels;

	/** Presentation time in nanoseconds from the start of the recording. */
	int64 TimeNs = 0;
};

/** A block of interleaved 16 bit PCM taken from the main submix. */
struct FNGGAudioChunk
{
	TArray<int16> Samples;

	/** Presentation time in nanoseconds from the start of the recording. */
	int64 TimeNs = 0;
};

/**
 * Owns the encoder and runs it on its own thread.
 *
 * Producers are the render thread (video) and the audio render thread (audio); this class is the only
 * consumer. Both queues are single-producer/single-consumer, so no locks are taken on the hot path -
 * the only mutex guards the frame buffer pool, which is touched twice per frame.
 */
class FNGGRecorderWriter : public FRunnable
{
public:
	FNGGRecorderWriter();
	virtual ~FNGGRecorderWriter() override;

	/** Opens the output file and starts the worker. On failure nothing is left running. */
	bool StartWriting(const FNGGEncoderConfig& Config, int32 InMaxQueuedFrames, FString& OutError);

	/**
	 * Drains the queues, finalises the MP4 and joins the worker. Blocks; typically a few hundred ms.
	 * Safe to call more than once.
	 */
	bool FinishWriting(FString& OutError);

	/**
	 * Takes a recycled buffer from the pool, sized for one frame.
	 * Returns null when the encoder is already this many frames behind, which means the caller should
	 * drop the frame. Render thread only.
	 */
	TUniquePtr<FNGGCapturedFrame> AcquireFrameBuffer();

	/**
	 * Blocking variant used by fixed-time-step recording, where dropping a frame is not acceptable.
	 * Returns null only if the wait timed out or the writer stopped.
	 */
	TUniquePtr<FNGGCapturedFrame> AcquireFrameBufferBlocking(double TimeoutSeconds);

	/** Hands a filled buffer to the encoder. Render thread only. */
	void SubmitFrame(TUniquePtr<FNGGCapturedFrame> Frame);

	/** Hands a block of PCM to the encoder. Audio render thread only. */
	void SubmitAudio(TUniquePtr<FNGGAudioChunk> Chunk);

	/** Called from the render thread when a frame had to be thrown away. */
	void NotifyFrameDropped() { DroppedFrames.Increment(); }

	int32 GetFramesWritten() const { return WrittenFrames.GetValue(); }
	int32 GetFramesDropped() const { return DroppedFrames.GetValue(); }

	/** True once the encoder has hit an error; the recording is no longer usable. */
	bool HasFailed() const { return bFailed; }
	FString GetError() const;

	//~ FRunnable
	virtual uint32 Run() override;
	virtual void Stop() override;
	//~ End FRunnable

private:
	/** Writes whatever is ready. Returns true if it wrote anything. */
	bool DrainOnce();

	void WriteVideo(TUniquePtr<FNGGCapturedFrame> Frame);
	void WriteAudio(TUniquePtr<FNGGAudioChunk> Chunk);

	void RecycleFrame(TUniquePtr<FNGGCapturedFrame> Frame);

	void RecordFailure(const FString& Error);

	FNGGEncoderConfig Config;
	TUniquePtr<FNGGVideoEncoder> Encoder;

	TQueue<TUniquePtr<FNGGCapturedFrame>, EQueueMode::Spsc> VideoQueue;
	TQueue<TUniquePtr<FNGGAudioChunk>, EQueueMode::Spsc> AudioQueue;

	/** Free list of frame-sized buffers, shared between the render thread and the worker. */
	TArray<TUniquePtr<FNGGCapturedFrame>> FramePool;
	FCriticalSection FramePoolLock;

	/** Frames handed out but not yet written. Bounded by MaxQueuedFrames. */
	FThreadSafeCounter InFlightFrames;
	FThreadSafeCounter WrittenFrames;
	FThreadSafeCounter DroppedFrames;

	int32 MaxQueuedFrames = 8;
	int32 FrameSizeBytes = 0;

	/**
	 * The most recent frame, held back by one so its duration can be set from the following frame's
	 * timestamp. A hitch in the game therefore lengthens a frame instead of shifting everything after it.
	 */
	TUniquePtr<FNGGCapturedFrame> PendingFrame;
	int64 MinFrameDeltaNs = 1;

	FRunnableThread* Thread = nullptr;
	FEvent* WakeEvent = nullptr;
	std::atomic<bool> bStopRequested{ false };
	std::atomic<bool> bFailed{ false };

	mutable FCriticalSection ErrorLock;
	FString ErrorMessage;
};
