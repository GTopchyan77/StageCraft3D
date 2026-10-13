// Copyright NGG. All Rights Reserved.

#include "NGGVideoRecorderFrameCapture.h"

#include "CommonRenderResources.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GlobalShader.h"
#include "HAL/PlatformProcess.h"
#include "Modules/ModuleManager.h"
#include "NGGVideoRecorderAudioCapture.h"
#include "NGGVideoRecorderLog.h"
#include "NGGVideoRecorderSettings.h"
#include "PipelineStateCache.h"
#include "RHIStaticStates.h"
#include "RenderingThread.h"
#include "RendererInterface.h"
#include "ScreenRendering.h"
#include "Shader.h"
#include "ShaderParameterUtils.h"
#include "UnrealClient.h"
#include "Widgets/SWindow.h"

namespace NGGVideoRecorderCapture
{
	/** H.264 needs even dimensions, and nothing sensible comes out below a handful of pixels. */
	static FIntPoint MakeEncodable(FIntPoint Size)
	{
		Size.X = FMath::Max(16, Size.X);
		Size.Y = FMath::Max(16, Size.Y);
		Size.X &= ~1;
		Size.Y &= ~1;
		return Size;
	}

	static FIntPoint ResolveTargetSize(const FNGGVideoRecorderOptions& Options, FIntPoint ViewportSize)
	{
		switch (Options.Resolution)
		{
		case ENGGRecorderResolution::HD720:   return FIntPoint(1280, 720);
		case ENGGRecorderResolution::HD1080:  return FIntPoint(1920, 1080);
		case ENGGRecorderResolution::QHD1440: return FIntPoint(2560, 1440);
		case ENGGRecorderResolution::UHD2160: return FIntPoint(3840, 2160);
		case ENGGRecorderResolution::Custom:  return MakeEncodable(Options.CustomResolution);
		case ENGGRecorderResolution::MatchViewport:
		default:
			return MakeEncodable(ViewportSize);
		}
	}

	/** Bits per pixel per frame, so a preset means the same thing at any resolution or frame rate. */
	static float QualityToBitsPerPixel(ENGGRecorderQuality Quality)
	{
		switch (Quality)
		{
		case ENGGRecorderQuality::Low:      return 0.04f;
		case ENGGRecorderQuality::Medium:   return 0.08f;
		case ENGGRecorderQuality::VeryHigh: return 0.22f;
		case ENGGRecorderQuality::High:
		default:                            return 0.14f;
		}
	}

	static int32 ResolveBitrate(const FNGGVideoRecorderOptions& Options, FIntPoint Size, int32 FrameRate)
	{
		if (Options.Quality == ENGGRecorderQuality::Custom)
		{
			return FMath::RoundToInt(FMath::Max(0.5f, Options.CustomBitrateMbps) * 1000000.0f);
		}

		const double Bits = (double)Size.X * (double)Size.Y * (double)FrameRate * QualityToBitsPerPixel(Options.Quality);
		return (int32)FMath::Clamp(Bits, 1000000.0, 200000000.0);
	}

	/** Rect inside the target that the source image is drawn into, centred and letterboxed if asked for. */
	static FIntRect ComputeDestRect(FIntPoint SourceSize, FIntPoint TargetSize, bool bPreserveAspect)
	{
		if (!bPreserveAspect || SourceSize.X <= 0 || SourceSize.Y <= 0)
		{
			return FIntRect(0, 0, TargetSize.X, TargetSize.Y);
		}

		const float SourceAspect = (float)SourceSize.X / (float)SourceSize.Y;
		const float TargetAspect = (float)TargetSize.X / (float)TargetSize.Y;

		FIntPoint Fitted = TargetSize;
		if (SourceAspect > TargetAspect)
		{
			// Source is wider: full width, bars top and bottom.
			Fitted.Y = FMath::RoundToInt((float)TargetSize.X / SourceAspect);
		}
		else if (SourceAspect < TargetAspect)
		{
			// Source is taller: full height, bars left and right.
			Fitted.X = FMath::RoundToInt((float)TargetSize.Y * SourceAspect);
		}

		Fitted.X = FMath::Clamp(Fitted.X, 2, TargetSize.X);
		Fitted.Y = FMath::Clamp(Fitted.Y, 2, TargetSize.Y);

		const FIntPoint Offset((TargetSize.X - Fitted.X) / 2, (TargetSize.Y - Fitted.Y) / 2);
		return FIntRect(Offset, Offset + Fitted);
	}
}

FNGGRecorderFrameCapture::FNGGRecorderFrameCapture()
{
}

FNGGRecorderFrameCapture::~FNGGRecorderFrameCapture()
{
	FString Unused;
	Stop(Unused);
}

bool FNGGRecorderFrameCapture::Start(
	const FNGGVideoRecorderOptions& Options,
	UGameViewportClient* ViewportClient,
	const FString& FilePath,
	FString& OutError)
{
	using namespace NGGVideoRecorderCapture;

	check(IsInGameThread());

	if (bActive)
	{
		OutError = TEXT("A recording is already running.");
		return false;
	}

	if (!FSlateApplication::IsInitialized())
	{
		OutError = TEXT("Slate is not available, so there is no viewport to record.");
		return false;
	}

	if (!ViewportClient || !ViewportClient->Viewport)
	{
		OutError = TEXT("There is no game viewport to record.");
		return false;
	}

	TSharedPtr<SWindow> Window = ViewportClient->GetWindow();
	if (!Window.IsValid())
	{
		OutError = TEXT("The game viewport is not attached to a window.");
		return false;
	}

	const FIntPoint ViewportSize = ViewportClient->Viewport->GetSizeXY();
	if (ViewportSize.X <= 0 || ViewportSize.Y <= 0)
	{
		OutError = TEXT("The game viewport has no size yet.");
		return false;
	}

	FrameRate = FMath::Clamp(Options.FrameRate, 1, 240);
	bFixedFrameRate = Options.bLockToFixedFrameRate;
	bPreserveAspectRatio = Options.bPreserveAspectRatio;
	TargetSize = ResolveTargetSize(Options, ViewportSize);
	TargetWindow = Window.Get();
	RendererModule = &FModuleManager::GetModuleChecked<IRendererModule>(TEXT("Renderer"));

	Writer = MakeShared<FNGGRecorderWriter>();

	FNGGEncoderConfig EncoderConfig;
	EncoderConfig.FilePath = FilePath;
	EncoderConfig.Width = TargetSize.X;
	EncoderConfig.Height = TargetSize.Y;
	EncoderConfig.FrameRate = FrameRate;
	EncoderConfig.VideoBitrate = ResolveBitrate(Options, TargetSize, FrameRate);
	EncoderConfig.bHasAudio = false;

	// Audio has to be resolved before the encoder opens the file, because the streams cannot be added
	// afterwards. Locking to a fixed frame rate detaches game time from wall time, which the audio engine
	// does not follow, so the two would drift apart over the recording.
	TSharedPtr<FNGGRecorderAudioCapture> PendingAudio;
	if (Options.bCaptureAudio && !bFixedFrameRate)
	{
		int32 SampleRate = 48000;
		FString AudioError;

		// The listener is registered here but only starts producing once the writer is running; anything it
		// hands over before then is queued, not lost.
		PendingAudio = FNGGRecorderAudioCapture::CreateAndRegister(ViewportClient->GetWorld(), Writer, SampleRate, AudioError);
		if (PendingAudio.IsValid())
		{
			EncoderConfig.bHasAudio = true;
			EncoderConfig.AudioSampleRate = SampleRate;
			EncoderConfig.AudioChannels = FNGGRecorderAudioCapture::OutputChannels;
			EncoderConfig.AudioBitrate = 192000;
		}
		else
		{
			UE_LOG(LogNGGVideoRecorder, Warning, TEXT("Recording without audio: %s"), *AudioError);
		}
	}
	else if (Options.bCaptureAudio && bFixedFrameRate)
	{
		UE_LOG(LogNGGVideoRecorder, Warning,
			TEXT("Recording without audio because the capture is locked to a fixed frame rate; game time and the audio clock would drift apart."));
	}

	const int32 MaxQueued = UNGGVideoRecorderSettings::Get().MaxQueuedFrames;
	if (!Writer->StartWriting(EncoderConfig, MaxQueued, OutError))
	{
		if (PendingAudio.IsValid())
		{
			PendingAudio->Unregister();
		}
		Writer.Reset();
		return false;
	}

	AudioCapture = PendingAudio;

	// GPU resources live on the render thread; make sure they exist before the first callback can fire.
	{
		FNGGRecorderFrameCapture* Self = this;
		const FIntPoint Size = TargetSize;

		ENQUEUE_RENDER_COMMAND(NGGVideoRecorderCreateTargets)(
			[Self, Size](FRHICommandListImmediate& RHICmdList)
			{
				for (int32 Index = 0; Index < NumReadbackSlots; ++Index)
				{
					FReadbackSlot& Slot = Self->Slots[Index];

					const FRHITextureCreateDesc Desc =
						FRHITextureCreateDesc::Create2D(TEXT("NGGVideoRecorder.Target"), Size.X, Size.Y, PF_B8G8R8A8)
						.SetClearValue(FClearValueBinding::Black)
						.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource);

					Slot.Target = RHICmdList.CreateTexture(Desc);
					Slot.Readback = MakeUnique<FRHIGPUTextureReadback>(TEXT("NGGVideoRecorder.Readback"));
					Slot.bPending = false;
					Slot.TimeNs = 0;
				}

				Self->NextSlotToFill = 0;
				Self->OldestPendingSlot = 0;
				Self->NumPendingSlots = 0;
			});

		FlushRenderingCommands();
	}

	StartTimeSeconds = FPlatformTime::Seconds();
	FrameIntervalSeconds = 1.0 / (double)FrameRate;
	NextCaptureTimeSeconds = StartTimeSeconds;
	FixedFrameIndex = 0;
	bLoggedFormatWarning = false;
	bStopped = false;

	BackBufferHandle = FSlateApplication::Get().GetRenderer()->OnBackBufferReadyToPresent()
		.AddRaw(this, &FNGGRecorderFrameCapture::OnBackBufferReady);

	bActive = true;
	return true;
}

bool FNGGRecorderFrameCapture::Stop(FString& OutError)
{
	check(IsInGameThread());

	if (bStopped)
	{
		return true;
	}
	bStopped = true;

	// Stop accepting new frames before unhooking, so a callback already in flight does nothing.
	bActive = false;

	if (BackBufferHandle.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().GetRenderer()->OnBackBufferReadyToPresent().Remove(BackBufferHandle);
	}
	BackBufferHandle.Reset();

	if (AudioCapture.IsValid())
	{
		AudioCapture->Unregister();
		AudioCapture.Reset();
	}

	// Hand over everything the GPU still owes us, then drop the GPU resources. The flush below guarantees
	// this has finished before the raw 'this' capture could dangle.
	if (Writer.IsValid())
	{
		FNGGRecorderFrameCapture* Self = this;
		ENQUEUE_RENDER_COMMAND(NGGVideoRecorderFlush)(
			[Self](FRHICommandListImmediate& RHICmdList)
			{
				Self->DrainReadbacks_RenderThread(true);
				Self->ReleaseResources_RenderThread();
			});
	}
	else
	{
		FNGGRecorderFrameCapture* Self = this;
		ENQUEUE_RENDER_COMMAND(NGGVideoRecorderRelease)(
			[Self](FRHICommandListImmediate& RHICmdList)
			{
				Self->ReleaseResources_RenderThread();
			});
	}
	FlushRenderingCommands();

	bool bSuccess = true;
	if (Writer.IsValid())
	{
		bSuccess = Writer->FinishWriting(OutError);

		// Read the tallies before dropping the writer, otherwise the summary reports zero.
		FinalFramesCaptured = Writer->GetFramesWritten();
		FinalFramesDropped = Writer->GetFramesDropped();

		Writer.Reset();
	}

	TargetWindow = nullptr;
	return bSuccess;
}

void FNGGRecorderFrameCapture::ReleaseResources_RenderThread()
{
	for (int32 Index = 0; Index < NumReadbackSlots; ++Index)
	{
		Slots[Index].Readback.Reset();
		Slots[Index].Target.SafeRelease();
		Slots[Index].bPending = false;
	}

	NextSlotToFill = 0;
	OldestPendingSlot = 0;
	NumPendingSlots = 0;
}

void FNGGRecorderFrameCapture::OnBackBufferReady(SWindow& SlateWindow, const FTextureRHIRef& BackBufferRef)
{
	if (!bActive)
	{
		return;
	}

	// Slate raises this for every window it renders, including editor windows during PIE.
	if (&SlateWindow != TargetWindow)
	{
		return;
	}

	if (!IsInRenderingThread())
	{
		return;
	}

	FRHITexture* BackBuffer = BackBufferRef.GetReference();
	if (!BackBuffer || !Writer.IsValid())
	{
		return;
	}

	FRHICommandListImmediate& RHICmdList = FRHICommandListImmediate::Get();

	// Free up ring slots first; this is also where finished frames reach the encoder.
	DrainReadbacks_RenderThread(false);

	int64 TimeNs = 0;

	if (bFixedFrameRate)
	{
		// One rendered frame is exactly one output frame, whatever the wall clock says.
		TimeNs = (FixedFrameIndex * 1000000000LL) / FrameRate;
	}
	else
	{
		const double Now = FPlatformTime::Seconds();

		// A quarter frame of slack: without it, a game running at exactly the target rate would skip every
		// other frame whenever the present time jitters slightly early.
		if (Now < NextCaptureTimeSeconds - FrameIntervalSeconds * 0.25)
		{
			return;
		}

		NextCaptureTimeSeconds += FrameIntervalSeconds;
		if (NextCaptureTimeSeconds < Now)
		{
			// Resynchronise after a hitch instead of trying to catch up on the frames we missed.
			NextCaptureTimeSeconds = Now + FrameIntervalSeconds;
		}

		TimeNs = (int64)((Now - StartTimeSeconds) * 1000000000.0);
	}

	if (NumPendingSlots >= NumReadbackSlots)
	{
		if (bFixedFrameRate)
		{
			// Not real time anyway, so wait for the GPU rather than lose a frame.
			DrainReadbacks_RenderThread(true);
		}
		else
		{
			Writer->NotifyFrameDropped();
			return;
		}
	}

	// Integer back buffers, including the 10 bit ones Windows often hands out for an ordinary SDR swap
	// chain, hold values in [0,1] and convert to 8 bit cleanly. Only a floating point back buffer - which
	// means HDR output is actually on - carries values above 1.0 that this pass would clip.
	if (!bLoggedFormatWarning)
	{
		const EPixelFormat Format = BackBuffer->GetFormat();
		const bool bIsFloatingPoint =
			Format == PF_FloatRGBA || Format == PF_FloatRGB ||
			Format == PF_FloatR11G11B10 || Format == PF_A32B32G32R32F;

		if (bIsFloatingPoint)
		{
			bLoggedFormatWarning = true;
			UE_LOG(LogNGGVideoRecorder, Warning,
				TEXT("The back buffer is floating point (format %d), which means HDR output is on. The recording clips to SDR and highlights will look blown out. Turn HDR display output off while recording."),
				(int32)Format);
		}
	}

	const int32 SlotIndex = NextSlotToFill;
	ResolveAndEnqueue_RenderThread(RHICmdList, BackBuffer, SlotIndex, TimeNs);

	NextSlotToFill = (NextSlotToFill + 1) % NumReadbackSlots;
	++NumPendingSlots;

	if (bFixedFrameRate)
	{
		++FixedFrameIndex;
	}
}

void FNGGRecorderFrameCapture::ResolveAndEnqueue_RenderThread(
	FRHICommandListImmediate& RHICmdList,
	FRHITexture* SourceTexture,
	int32 SlotIndex,
	int64 TimeNs)
{
	using namespace NGGVideoRecorderCapture;

	FReadbackSlot& Slot = Slots[SlotIndex];
	if (!Slot.Target.IsValid() || !Slot.Readback.IsValid() || !RendererModule)
	{
		return;
	}

	const FIntPoint SourceSize(SourceTexture->GetSizeX(), SourceTexture->GetSizeY());
	if (SourceSize.X <= 0 || SourceSize.Y <= 0)
	{
		return;
	}

	// The output size is fixed for the whole recording because the encoder cannot change frame size mid
	// stream. If the player resizes the window, the new back buffer is simply rescaled into the same target.
	const FIntRect Dest = ComputeDestRect(SourceSize, TargetSize, bPreserveAspectRatio);

	RHICmdList.Transition(FRHITransitionInfo(Slot.Target, ERHIAccess::Unknown, ERHIAccess::RTV));

	// Clearing every frame costs almost nothing and keeps the letterbox bars black even when the window
	// aspect changes mid recording.
	FRHIRenderPassInfo RPInfo(Slot.Target, ERenderTargetActions::Clear_Store);
	RHICmdList.BeginRenderPass(RPInfo, TEXT("NGGVideoRecorderResolve"));
	{
		RHICmdList.SetViewport(0, 0, 0.0f, (float)TargetSize.X, (float)TargetSize.Y, 1.0f);

		FGraphicsPipelineStateInitializer PSOInit;
		RHICmdList.ApplyCachedRenderTargets(PSOInit);
		PSOInit.BlendState = TStaticBlendState<>::GetRHI();
		PSOInit.RasterizerState = TStaticRasterizerState<>::GetRHI();
		PSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
		PSOInit.PrimitiveType = PT_TriangleList;

		FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
		TShaderMapRef<FScreenVS> VertexShader(ShaderMap);
		TShaderMapRef<FScreenPS> PixelShader(ShaderMap);

		PSOInit.BoundShaderState.VertexDeclarationRHI = GFilterVertexDeclaration.VertexDeclarationRHI;
		PSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
		PSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();

		SetGraphicsPipelineState(RHICmdList, PSOInit, 0);

		// Point sampling when there is no scaling keeps a native-resolution capture pixel exact.
		FRHISamplerState* Sampler = (SourceSize == Dest.Size())
			? TStaticSamplerState<SF_Point>::GetRHI()
			: TStaticSamplerState<SF_Bilinear>::GetRHI();

		SetShaderParametersLegacyPS(RHICmdList, PixelShader, Sampler, SourceTexture);

		RendererModule->DrawRectangle(
			RHICmdList,
			(float)Dest.Min.X, (float)Dest.Min.Y,			// destination position
			(float)Dest.Width(), (float)Dest.Height(),		// destination size
			0.0f, 0.0f,										// source position in texels
			(float)SourceSize.X, (float)SourceSize.Y,		// source size in texels
			TargetSize,
			SourceSize,
			VertexShader,
			EDRF_Default);
	}
	RHICmdList.EndRenderPass();

	// EnqueueCopy transitions its own staging texture but expects the source to be ready to copy from.
	RHICmdList.Transition(FRHITransitionInfo(Slot.Target, ERHIAccess::RTV, ERHIAccess::CopySrc));

	Slot.Readback->EnqueueCopy(RHICmdList, Slot.Target.GetReference());
	Slot.TimeNs = TimeNs;
	Slot.bPending = true;
}

void FNGGRecorderFrameCapture::DrainReadbacks_RenderThread(bool bBlockUntilEmpty)
{
	if (!Writer.IsValid())
	{
		return;
	}

	while (NumPendingSlots > 0)
	{
		FReadbackSlot& Slot = Slots[OldestPendingSlot];
		if (!Slot.bPending || !Slot.Readback.IsValid())
		{
			break;
		}

		if (!Slot.Readback->IsReady())
		{
			if (!bBlockUntilEmpty)
			{
				// Frames are consumed strictly in order, so a slot that is not ready blocks the ones behind
				// it. That is intentional: it keeps timestamps monotonic without any sorting.
				break;
			}
			FPlatformProcess::SleepNoStats(0.0005f);
			continue;
		}

		int32 RowPitchInPixels = 0;
		int32 BufferHeight = 0;
		void* Mapped = Slot.Readback->Lock(RowPitchInPixels, &BufferHeight);

		if (Mapped)
		{
			// In fixed frame rate mode the sim is not running in real time, so waiting for the encoder is
			// preferable to dropping a frame. Five seconds is a generous ceiling that still fails rather
			// than hanging the render thread forever.
			TUniquePtr<FNGGCapturedFrame> Frame = bFixedFrameRate
				? Writer->AcquireFrameBufferBlocking(5.0)
				: Writer->AcquireFrameBuffer();

			if (Frame.IsValid())
			{
				const int32 DestRowBytes = TargetSize.X * 4;
				const int64 SourceRowBytes = (int64)RowPitchInPixels * 4;
				const int32 RowsToCopy = FMath::Min(TargetSize.Y, BufferHeight > 0 ? BufferHeight : TargetSize.Y);

				const uint8* Source = static_cast<const uint8*>(Mapped);
				uint8* Dest = Frame->Pixels.GetData();

				// The staging texture's row pitch is driver aligned and usually wider than the image.
				for (int32 Row = 0; Row < RowsToCopy; ++Row)
				{
					FMemory::Memcpy(Dest + (int64)Row * DestRowBytes, Source + (int64)Row * SourceRowBytes, DestRowBytes);
				}

				Frame->TimeNs = Slot.TimeNs;
				Writer->SubmitFrame(MoveTemp(Frame));
			}
			else
			{
				Writer->NotifyFrameDropped();
			}

			Slot.Readback->Unlock();
		}

		Slot.bPending = false;
		OldestPendingSlot = (OldestPendingSlot + 1) % NumReadbackSlots;
		--NumPendingSlots;
	}
}
