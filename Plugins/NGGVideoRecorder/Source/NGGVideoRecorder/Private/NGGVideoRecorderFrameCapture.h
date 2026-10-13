// Copyright NGG. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "NGGVideoRecorderTypes.h"
#include "NGGVideoRecorderWriter.h"
#include "RHI.h"
#include "RHIGPUReadback.h"

#include <atomic>

class FNGGRecorderAudioCapture;
class SWindow;
class UGameViewportClient;

/**
 * Pulls finished frames off the GPU and feeds them to the encoder.
 *
 * The capture hooks Slate's "back buffer ready to present" callback, so it records exactly the image the
 * player sees, including UMG and any debug overlays, and it works the same in PIE and in a packaged build.
 *
 * Per presented frame, on the render thread:
 *   1. skip the frame unless the target frame interval has elapsed (not done when locked to a fixed rate);
 *   2. draw the back buffer into a fixed size BGRA8 target, which also handles rescaling and letterboxing;
 *   3. enqueue an asynchronous copy into a staging texture;
 *   4. hand over any staging texture whose copy has landed, in submission order.
 *
 * Nothing here ever blocks on the GPU: readbacks are polled, so a frame costs a copy and no sync point.
 */
class FNGGRecorderFrameCapture
{
public:
	FNGGRecorderFrameCapture();
	~FNGGRecorderFrameCapture();

	/**
	 * Starts capturing. Must be called on the game thread with a live game viewport.
	 * On failure nothing is left registered and OutError explains why.
	 */
	bool Start(const FNGGVideoRecorderOptions& Options, UGameViewportClient* ViewportClient, const FString& FilePath, FString& OutError);

	/** Stops capturing, drains in-flight frames and finalises the file. Blocks. Safe to call twice. */
	bool Stop(FString& OutError);

	bool IsActive() const { return bActive; }

	FIntPoint GetResolution() const { return TargetSize; }
	int32 GetFrameRate() const { return FrameRate; }
	/** Counts survive Stop, which releases the writer, so the finished recording can still be reported on. */
	int32 GetFramesCaptured() const { return Writer.IsValid() ? Writer->GetFramesWritten() : FinalFramesCaptured; }
	int32 GetFramesDropped() const { return Writer.IsValid() ? Writer->GetFramesDropped() : FinalFramesDropped; }

	bool HasFailed() const { return Writer.IsValid() && Writer->HasFailed(); }
	FString GetError() const { return Writer.IsValid() ? Writer->GetError() : FString(); }

	FNGGRecorderWriter* GetWriter() const { return Writer.Get(); }

private:
	void OnBackBufferReady(SWindow& SlateWindow, const FTextureRHIRef& BackBufferRef);

	/** Draws SourceTexture into Slot.Target and enqueues the readback. Render thread. */
	void ResolveAndEnqueue_RenderThread(FRHICommandListImmediate& RHICmdList, FRHITexture* SourceTexture, int32 SlotIndex, int64 TimeNs);

	/** Hands over every readback that has completed, oldest first. Render thread. */
	void DrainReadbacks_RenderThread(bool bBlockUntilEmpty);

	void ReleaseResources_RenderThread();

	/** One in-flight GPU readback plus the render target it reads from. */
	struct FReadbackSlot
	{
		TUniquePtr<FRHIGPUTextureReadback> Readback;
		FTextureRHIRef Target;
		int64 TimeNs = 0;
		bool bPending = false;
	};

	static constexpr int32 NumReadbackSlots = 3;
	FReadbackSlot Slots[NumReadbackSlots];

	/** Ring cursors. Written and read only on the render thread. */
	int32 NextSlotToFill = 0;
	int32 OldestPendingSlot = 0;
	int32 NumPendingSlots = 0;

	/**
	 * Shared rather than unique because the audio listener also holds a reference: the audio engine can
	 * still be inside a callback when capture stops, and a strong reference keeps the queue it writes into
	 * alive until the engine lets the listener go.
	 */
	TSharedPtr<FNGGRecorderWriter> Writer;
	TSharedPtr<FNGGRecorderAudioCapture> AudioCapture;

	FDelegateHandle BackBufferHandle;

	/**
	 * Compared by address only, never dereferenced, so it is safe to read from the render thread even if
	 * the window has since been destroyed.
	 */
	const SWindow* TargetWindow = nullptr;

	FIntPoint TargetSize = FIntPoint::ZeroValue;

	/** Letterbox instead of stretching when the window aspect does not match the output aspect. */
	bool bPreserveAspectRatio = true;

	/** Resolved on the game thread in Start; the module manager is not thread safe. */
	class IRendererModule* RendererModule = nullptr;

	int32 FrameRate = 60;
	bool bFixedFrameRate = false;

	/** Real-time pacing state, render thread only. */
	double StartTimeSeconds = 0.0;
	double FrameIntervalSeconds = 0.0;
	double NextCaptureTimeSeconds = 0.0;
	int64 FixedFrameIndex = 0;

	std::atomic<bool> bActive{ false };
	bool bStopped = false;

	/** Final tallies, captured in Stop before the writer is released. */
	int32 FinalFramesCaptured = 0;
	int32 FinalFramesDropped = 0;

	/** Set once so a per-frame failure does not spam the log. */
	bool bLoggedFormatWarning = false;
};
