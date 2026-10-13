// Copyright NGG. All Rights Reserved.

#include "NGGVideoRecorderWriter.h"

#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/RunnableThread.h"
#include "NGGVideoRecorderLog.h"

FNGGRecorderWriter::FNGGRecorderWriter()
{
}

FNGGRecorderWriter::~FNGGRecorderWriter()
{
	FString Unused;
	FinishWriting(Unused);
}

bool FNGGRecorderWriter::StartWriting(const FNGGEncoderConfig& InConfig, int32 InMaxQueuedFrames, FString& OutError)
{
	check(IsInGameThread());

	Config = InConfig;
	MaxQueuedFrames = FMath::Clamp(InMaxQueuedFrames, 2, 64);
	FrameSizeBytes = Config.Width * Config.Height * 4;

	// Two frames at the same timestamp would be rejected by the muxer, so timestamps are nudged apart by a
	// fraction of a frame when the game presents faster than the target rate.
	MinFrameDeltaNs = FMath::Max<int64>(1, 1000000000LL / (FMath::Max(1, Config.FrameRate) * 8));

	Encoder = CreateNGGVideoEncoder(OutError);
	if (!Encoder.IsValid())
	{
		return false;
	}

	if (!Encoder->Initialize(Config, OutError))
	{
		Encoder.Reset();
		return false;
	}

	// Pre-allocate the pool up front so the render thread never allocates while recording.
	{
		FScopeLock Lock(&FramePoolLock);
		FramePool.Reserve(MaxQueuedFrames);
		for (int32 Index = 0; Index < MaxQueuedFrames; ++Index)
		{
			TUniquePtr<FNGGCapturedFrame> Frame = MakeUnique<FNGGCapturedFrame>();
			Frame->Pixels.SetNumUninitialized(FrameSizeBytes);
			FramePool.Add(MoveTemp(Frame));
		}
	}

	WakeEvent = FPlatformProcess::GetSynchEventFromPool(false);
	bStopRequested = false;
	bFailed = false;

	Thread = FRunnableThread::Create(this, TEXT("NGGVideoRecorderEncoder"), 0, TPri_AboveNormal);
	if (!Thread)
	{
		OutError = TEXT("Could not create the encoder thread.");
		FString Ignored;
		Encoder->Finalize(Ignored);
		Encoder.Reset();
		FPlatformProcess::ReturnSynchEventToPool(WakeEvent);
		WakeEvent = nullptr;
		return false;
	}

	return true;
}

bool FNGGRecorderWriter::FinishWriting(FString& OutError)
{
	if (Thread)
	{
		bStopRequested = true;
		if (WakeEvent)
		{
			WakeEvent->Trigger();
		}

		// Run() drains both queues and finalises the file before returning.
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
	}

	if (WakeEvent)
	{
		FPlatformProcess::ReturnSynchEventToPool(WakeEvent);
		WakeEvent = nullptr;
	}

	Encoder.Reset();

	{
		FScopeLock Lock(&FramePoolLock);
		FramePool.Empty();
	}

	if (bFailed)
	{
		OutError = GetError();
		return false;
	}
	return true;
}

TUniquePtr<FNGGCapturedFrame> FNGGRecorderWriter::AcquireFrameBuffer()
{
	if (bStopRequested || bFailed)
	{
		return nullptr;
	}

	if (InFlightFrames.GetValue() >= MaxQueuedFrames)
	{
		return nullptr;
	}

	TUniquePtr<FNGGCapturedFrame> Frame;
	{
		FScopeLock Lock(&FramePoolLock);
		if (FramePool.Num() > 0)
		{
			Frame = MoveTemp(FramePool.Last());
			FramePool.Pop(EAllowShrinking::No);
		}
	}

	if (!Frame.IsValid())
	{
		return nullptr;
	}

	// The pool is sized for the recording, but stay defensive in case the resolution ever changes.
	if (Frame->Pixels.Num() != FrameSizeBytes)
	{
		Frame->Pixels.SetNumUninitialized(FrameSizeBytes);
	}

	InFlightFrames.Increment();
	return Frame;
}

TUniquePtr<FNGGCapturedFrame> FNGGRecorderWriter::AcquireFrameBufferBlocking(double TimeoutSeconds)
{
	const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;

	for (;;)
	{
		TUniquePtr<FNGGCapturedFrame> Frame = AcquireFrameBuffer();
		if (Frame.IsValid())
		{
			return Frame;
		}

		if (bStopRequested || bFailed || FPlatformTime::Seconds() >= Deadline)
		{
			return nullptr;
		}

		// The encoder is the bottleneck here, which is expected when the sim renders faster than it encodes.
		FPlatformProcess::SleepNoStats(0.001f);
	}
}

void FNGGRecorderWriter::SubmitFrame(TUniquePtr<FNGGCapturedFrame> Frame)
{
	if (!Frame.IsValid())
	{
		return;
	}

	VideoQueue.Enqueue(MoveTemp(Frame));
	if (WakeEvent)
	{
		WakeEvent->Trigger();
	}
}

void FNGGRecorderWriter::SubmitAudio(TUniquePtr<FNGGAudioChunk> Chunk)
{
	if (!Chunk.IsValid() || bStopRequested || bFailed)
	{
		return;
	}

	AudioQueue.Enqueue(MoveTemp(Chunk));
	if (WakeEvent)
	{
		WakeEvent->Trigger();
	}
}

FString FNGGRecorderWriter::GetError() const
{
	FScopeLock Lock(&ErrorLock);
	return ErrorMessage;
}

void FNGGRecorderWriter::RecordFailure(const FString& Error)
{
	{
		FScopeLock Lock(&ErrorLock);
		if (ErrorMessage.IsEmpty())
		{
			ErrorMessage = Error;
		}
	}

	if (!bFailed.exchange(true))
	{
		UE_LOG(LogNGGVideoRecorder, Error, TEXT("Encoder failed, recording aborted: %s"), *Error);
	}
}

void FNGGRecorderWriter::RecycleFrame(TUniquePtr<FNGGCapturedFrame> Frame)
{
	if (!Frame.IsValid())
	{
		return;
	}

	{
		FScopeLock Lock(&FramePoolLock);
		FramePool.Add(MoveTemp(Frame));
	}
	InFlightFrames.Decrement();
}

void FNGGRecorderWriter::WriteVideo(TUniquePtr<FNGGCapturedFrame> Frame)
{
	// Frames are held back by one so the previous frame's duration can be derived from this one's timestamp.
	if (PendingFrame.IsValid())
	{
		int64 ThisTime = Frame->TimeNs;
		if (ThisTime <= PendingFrame->TimeNs)
		{
			ThisTime = PendingFrame->TimeNs + MinFrameDeltaNs;
			Frame->TimeNs = ThisTime;
		}

		const int64 Duration = ThisTime - PendingFrame->TimeNs;

		FString Error;
		if (!Encoder->WriteVideoFrame(PendingFrame->Pixels.GetData(), PendingFrame->TimeNs, Duration, Error))
		{
			RecordFailure(Error);
		}
		else
		{
			WrittenFrames.Increment();
		}

		RecycleFrame(MoveTemp(PendingFrame));
	}

	PendingFrame = MoveTemp(Frame);
}

void FNGGRecorderWriter::WriteAudio(TUniquePtr<FNGGAudioChunk> Chunk)
{
	const int32 NumFrames = Config.AudioChannels > 0 ? Chunk->Samples.Num() / Config.AudioChannels : 0;

	FString Error;
	if (!Encoder->WriteAudio(Chunk->Samples.GetData(), NumFrames, Chunk->TimeNs, Error))
	{
		RecordFailure(Error);
	}
}

bool FNGGRecorderWriter::DrainOnce()
{
	bool bDidWork = false;

	// Interleave the two streams in timestamp order. The muxer copes with out-of-order writes by buffering,
	// so feeding it in order keeps its memory use flat over a long recording.
	for (;;)
	{
		TUniquePtr<FNGGCapturedFrame>* NextVideo = VideoQueue.Peek();
		TUniquePtr<FNGGAudioChunk>* NextAudio = AudioQueue.Peek();

		if (!NextVideo && !NextAudio)
		{
			break;
		}

		const bool bTakeVideo = NextVideo && (!NextAudio || (*NextVideo)->TimeNs <= (*NextAudio)->TimeNs);

		if (bTakeVideo)
		{
			TUniquePtr<FNGGCapturedFrame> Frame;
			VideoQueue.Dequeue(Frame);
			if (!bFailed)
			{
				WriteVideo(MoveTemp(Frame));
			}
			else
			{
				RecycleFrame(MoveTemp(Frame));
			}
		}
		else
		{
			TUniquePtr<FNGGAudioChunk> Chunk;
			AudioQueue.Dequeue(Chunk);
			if (!bFailed)
			{
				WriteAudio(MoveTemp(Chunk));
			}
		}

		bDidWork = true;
	}

	return bDidWork;
}

uint32 FNGGRecorderWriter::Run()
{
	while (!bStopRequested)
	{
		if (!DrainOnce() && WakeEvent)
		{
			WakeEvent->Wait(FTimespan::FromMilliseconds(5));
		}
	}

	// Stop was requested: take whatever the producers already handed over.
	DrainOnce();

	// The held-back frame has no successor to derive a duration from, so give it one nominal frame.
	if (PendingFrame.IsValid())
	{
		if (!bFailed)
		{
			const int64 Duration = 1000000000LL / FMath::Max(1, Config.FrameRate);

			FString Error;
			if (!Encoder->WriteVideoFrame(PendingFrame->Pixels.GetData(), PendingFrame->TimeNs, Duration, Error))
			{
				RecordFailure(Error);
			}
			else
			{
				WrittenFrames.Increment();
			}
		}
		RecycleFrame(MoveTemp(PendingFrame));
	}

	if (Encoder.IsValid())
	{
		FString Error;
		if (!Encoder->Finalize(Error))
		{
			RecordFailure(Error);
		}
	}

	return 0;
}

void FNGGRecorderWriter::Stop()
{
	bStopRequested = true;
	if (WakeEvent)
	{
		WakeEvent->Trigger();
	}
}
