// Copyright Epic Games, Inc. All Rights Reserved.

#include "Render/StageRenderSubsystem.h"

#include "Actors/ModularBaseActor.h"
#include "Async/Async.h"
#include "CanvasItem.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ModularSceneBuilder.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"
#include "Settings/StageCraftUserSettings.h"
#include "Subsystems/StageSessionSubsystem.h"
#include "Tasks/Task.h"
#include "TextureResource.h"
#include "TimerManager.h"

#include <atomic>

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageRenderSubsystem)

#define LOCTEXT_NAMESPACE "StageRender"

/**
 * The GPU -> CPU hand-off of one render. The readback object is created, polled, mapped and released on the render thread
 * only; the game thread looks at the atomics and, once bComplete is set, owns Pixels.
 */
struct FStageRenderReadback
{
	TUniquePtr<FRHIGPUTextureReadback> GpuReadback;
	FIntPoint Size = FIntPoint::ZeroValue;

	/** BGRA8, tightly packed rows. Written by the render thread before bComplete is published. */
	TArray64<uint8> Pixels;

	std::atomic<bool> bComplete{ false };
	std::atomic<bool> bSucceeded{ false };
	/** A poll command is queued on the render thread; the game thread queues at most one at a time. */
	std::atomic<bool> bPollQueued{ false };
};

namespace StageRenderSubsystem
{
	/** A GPU that has not returned the image after this many frames has a problem; the job fails instead of waiting forever. */
	constexpr int32 MaxReadbackPolls = 600;

	/** Stops auto exposure from lagging behind the warm-up: the capture adapts within a few frames. */
	constexpr float WarmupExposureSpeed = 20.f;

	/** Footer height as a fraction of the image height, and the text height as a fraction of the footer. */
	constexpr double WatermarkBarFraction = 0.04;
	constexpr double WatermarkTextFraction = 0.55;

	int32 GetConsoleInt(const TCHAR* Name, int32 Fallback)
	{
		const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
		return Variable ? Variable->GetInt() : Fallback;
	}

	/** Writes Bytes to Path through a temporary file, so a crash never leaves a truncated image behind. Any thread. */
	bool WriteFileAtomic(const FString& Path, TConstArrayView64<uint8> Bytes)
	{
		const FString TempPath = Path + TEXT(".tmp");
		if (!FFileHelper::SaveArrayToFile(Bytes, *TempPath))
		{
			return false;
		}
		if (!IFileManager::Get().Move(*Path, *TempPath, /*bReplace*/ false, /*bEvenIfReadOnly*/ false, /*bAttributes*/ false, /*bDoNotRetryOrError*/ true))
		{
			IFileManager::Get().Delete(*TempPath, false, false, true);
			return false;
		}
		return true;
	}

	/** Path in Directory for FileName that does not exist yet ("..._2.png", "..._3.png" when needed). */
	FString MakeUniquePath(const FString& Directory, const FString& FileName)
	{
		const FString Base = FPaths::GetBaseFilename(FileName);
		const FString Extension = FPaths::GetExtension(FileName, /*bIncludeDot*/ true);
		FString Path = FPaths::Combine(Directory, FileName);
		for (int32 Suffix = 2; IFileManager::Get().FileExists(*Path); ++Suffix)
		{
			Path = FPaths::Combine(Directory, FString::Printf(TEXT("%s_%d%s"), *Base, Suffix, *Extension));
		}
		return Path;
	}
}

bool UStageRenderSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UStageRenderSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	// PNG compression runs on a worker thread, which may only use modules that are already loaded (FImageUtils looks it up without loading off the game thread).
	FModuleManager::Get().LoadModule(TEXT("ImageWrapper"));
}

void UStageRenderSubsystem::Deinitialize()
{
	CancelJob();
	OnRenderStateChanged.Clear();
	OnRenderFinished.Clear();
	OnSettingsChanged.Clear();
	Super::Deinitialize();
}

// --- Settings ---

FStageRenderSettings UStageRenderSubsystem::GetSettings() const
{
	const UStageCraftUserSettings* UserSettings = UStageCraftUserSettings::Get();
	return UserSettings ? UserSettings->GetRenderSettings() : FallbackSettings;
}

void UStageRenderSubsystem::SetSettings(const FStageRenderSettings& NewSettings)
{
	bool bChanged = false;
	if (UStageCraftUserSettings* UserSettings = UStageCraftUserSettings::Get())
	{
		bChanged = UserSettings->SetRenderSettings(NewSettings);
		if (bChanged)
		{
			// One small ini write per click in the Render panel; nothing drags these values continuously.
			UserSettings->SaveSettings();
		}
	}
	else
	{
		const FStageRenderSettings Sanitized = StageRender::Sanitize(NewSettings);
		bChanged = !(FallbackSettings == Sanitized);
		FallbackSettings = Sanitized;
	}

	if (bChanged)
	{
		OnSettingsChanged.Broadcast(GetSettings());
	}
}

FString UStageRenderSubsystem::GetOutputDirectory() const
{
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Renders")));
}

// --- Job ---

void UStageRenderSubsystem::SetState(EStageRenderState NewState)
{
	if (State != NewState)
	{
		State = NewState;
		OnRenderStateChanged.Broadcast(State);
	}
}

void UStageRenderSubsystem::ScheduleNextFrame(FJobStep Step)
{
	// One-shot, only while a job runs: the subsystem has no tick and no timer while idle.
	GetWorld()->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateUObject(this, Step));
}

bool UStageRenderSubsystem::StartRender(const FStageRenderRequest& Request, FText& OutReason)
{
	if (IsRendering())
	{
		OutReason = LOCTEXT("Busy", "A render is already in progress.");
		return false;
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		OutReason = LOCTEXT("NoWorld", "There is no stage to render.");
		return false;
	}

	++JobSerial;
	JobSettings = GetSettings();
	JobOutputSize = StageRender::GetOutputSize(JobSettings.Resolution);
	JobInternalSize = StageRender::GetInternalSize(JobSettings);
	JobLocalTime = FDateTime::Now();
	JobStartSeconds = FPlatformTime::Seconds();
	FramesRemaining = StageRender::GetWarmupFrames(JobSettings);
	ReadbackPolls = 0;

	const UStageSessionSubsystem* Session = UWorld::GetSubsystem<UStageSessionSubsystem>(World);
	JobSceneName = Session && !Session->GetSceneName().IsEmpty() ? Session->GetSceneName() : UWorld::RemovePIEPrefix(World->GetMapName());
	JobItemCount = Session ? Session->GetStats().PlacedItems : 0;

	if (!CreateCaptureResources(Request))
	{
		ReleaseCaptureResources();
		OutReason = LOCTEXT("NoCapture", "The render could not be set up (the scene capture or its render target could not be created).");
		return false;
	}

	UE_LOG(LogStageCraft, Log, TEXT("Render started: %dx%d (internal %dx%d), AA %s, post %s, watermark %s, %d warm-up frames, scene \"%s\"."),
		JobOutputSize.X, JobOutputSize.Y, JobInternalSize.X, JobInternalSize.Y, *UEnum::GetValueAsString(JobSettings.AntiAliasing),
		*UEnum::GetValueAsString(JobSettings.PostProcess), JobSettings.bWatermark ? TEXT("on") : TEXT("off"), FramesRemaining, *JobSceneName);

	SetItemHighlightsSuppressed(true);
	SetState(EStageRenderState::Capturing);
	ScheduleNextFrame(&ThisClass::StepCapture);
	return true;
}

bool UStageRenderSubsystem::CreateCaptureResources(const FStageRenderRequest& Request)
{
	UWorld* World = GetWorld();

	RenderTarget = NewObject<UTextureRenderTarget2D>(this, NAME_None, RF_Transient);
	// RGBA8 is linear PF_B8G8R8A8: the capture writes display-ready (tonemapped, gamma-encoded) colour, read back byte for byte.
	RenderTarget->RenderTargetFormat = RTF_RGBA8;
	RenderTarget->ClearColor = FLinearColor::Black;
	RenderTarget->bAutoGenerateMips = false;
	RenderTarget->InitAutoFormat(JobInternalSize.X, JobInternalSize.Y);
	RenderTarget->UpdateResourceImmediate(/*bClearRenderTarget*/ true);

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.ObjectFlags = RF_Transient;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	CaptureActor = World->SpawnActor<ASceneCapture2D>(Request.View.Location, Request.View.Rotation, SpawnParameters);
	USceneCaptureComponent2D* Capture = CaptureActor ? CaptureActor->GetCaptureComponent2D() : nullptr;
	if (!Capture || !RenderTarget->GameThread_GetRenderTargetResource())
	{
		return false;
	}

	ConfigureCapture(*Capture, Request);
	return true;
}

void UStageRenderSubsystem::ConfigureCapture(USceneCaptureComponent2D& Capture, const FStageRenderRequest& Request) const
{
	using namespace StageRenderSubsystem;

	Capture.TextureTarget = RenderTarget;
	Capture.CaptureSource = SCS_FinalColorLDR;
	Capture.FOVAngle = Request.View.FOV;
	// Rendered every frame during the warm-up, with a persistent view state: temporal AA, eye adaptation and Lumen need history.
	Capture.bCaptureEveryFrame = true;
	Capture.bCaptureOnMovement = false;
	Capture.bAlwaysPersistRenderingState = true;

	Capture.HiddenActors.Add(Capture.GetOwner());
	for (const TWeakObjectPtr<AActor>& Hidden : Request.HiddenActors)
	{
		if (AActor* Actor = Hidden.Get())
		{
			Capture.HiddenActors.Add(Actor);
		}
	}

	// Scene captures default to TemporalAA off (FXAA at most) and no motion blur (SceneCaptureComponent.cpp).
	const bool bTemporal = JobSettings.AntiAliasing == EStageRenderAntiAliasing::Temporal || JobSettings.AntiAliasing == EStageRenderAntiAliasing::Supersampled;
	Capture.ShowFlags.SetAntiAliasing(JobSettings.AntiAliasing != EStageRenderAntiAliasing::Off);
	Capture.ShowFlags.SetTemporalAA(bTemporal);
	Capture.ShowFlags.SetMotionBlur(false);

	// The camera's own post-processing first; the level's volumes are blended in by the renderer as for the viewport.
	FPostProcessSettings& Post = Capture.PostProcessSettings;
	Post = Request.View.PostProcessSettings;
	Capture.PostProcessBlendWeight = 1.f;

	// Scene captures turn Lumen off unless overridden (SceneCaptureRendering.cpp); match the project's methods so the render lights like the viewport.
	Post.bOverride_DynamicGlobalIlluminationMethod = true;
	Post.DynamicGlobalIlluminationMethod = static_cast<EDynamicGlobalIlluminationMethod::Type>(GetConsoleInt(TEXT("r.DynamicGlobalIlluminationMethod"), EDynamicGlobalIlluminationMethod::Lumen));
	Post.bOverride_ReflectionMethod = true;
	Post.ReflectionMethod = static_cast<EReflectionMethod::Type>(GetConsoleInt(TEXT("r.ReflectionMethod"), EReflectionMethod::Lumen));
	Post.bOverride_LumenSurfaceCacheResolution = true;
	Post.LumenSurfaceCacheResolution = 1.f;
	Post.bOverride_AutoExposureSpeedUp = true;
	Post.AutoExposureSpeedUp = WarmupExposureSpeed;
	Post.bOverride_AutoExposureSpeedDown = true;
	Post.AutoExposureSpeedDown = WarmupExposureSpeed;
	Post.bOverride_MotionBlurAmount = true;
	Post.MotionBlurAmount = 0.f;

	if (JobSettings.PostProcess == EStageRenderPostProcess::Clean)
	{
		Post.bOverride_VignetteIntensity = true;
		Post.VignetteIntensity = 0.f;
		Post.bOverride_FilmGrainIntensity = true;
		Post.FilmGrainIntensity = 0.f;
		Post.bOverride_SceneFringeIntensity = true;
		Post.SceneFringeIntensity = 0.f;
		Post.bOverride_LensFlareIntensity = true;
		Post.LensFlareIntensity = 0.f;
		Post.bOverride_BloomIntensity = true;
		Post.BloomIntensity = 0.f;
	}
	else if (JobSettings.PostProcess == EStageRenderPostProcess::Cinematic)
	{
		Post.bOverride_LumenFinalGatherQuality = true;
		Post.LumenFinalGatherQuality = 2.f;
		Post.bOverride_LumenSceneLightingQuality = true;
		Post.LumenSceneLightingQuality = 2.f;
		Post.bOverride_LumenReflectionQuality = true;
		Post.LumenReflectionQuality = 2.f;
		Post.bOverride_AmbientOcclusionQuality = true;
		Post.AmbientOcclusionQuality = 100.f;
		Post.bOverride_ScreenSpaceReflectionQuality = true;
		Post.ScreenSpaceReflectionQuality = 100.f;
	}
}

void UStageRenderSubsystem::SetItemHighlightsSuppressed(bool bSuppressed)
{
	if (bSuppressed)
	{
		const UStageSessionSubsystem* Session = UWorld::GetSubsystem<UStageSessionSubsystem>(GetWorld());
		if (!Session)
		{
			return;
		}
		for (AModularBaseActor* Item : Session->GetPlacedItems())
		{
			if (Item && (Item->IsSelected() || Item->IsHovered()))
			{
				Item->SetHighlightSuppressed(true);
				SuppressedItems.Add(Item);
			}
		}
		return;
	}

	for (const TWeakObjectPtr<AModularBaseActor>& Item : SuppressedItems)
	{
		if (AModularBaseActor* Actor = Item.Get())
		{
			Actor->SetHighlightSuppressed(false);
		}
	}
	SuppressedItems.Reset();
}

void UStageRenderSubsystem::StepCapture()
{
	if (State != EStageRenderState::Capturing)
	{
		return;
	}
	if (--FramesRemaining > 0)
	{
		ScheduleNextFrame(&ThisClass::StepCapture);
		return;
	}

	// The last warm-up frame has been rendered. Stop capturing and give the viewport its overlays back.
	if (USceneCaptureComponent2D* Capture = CaptureActor ? CaptureActor->GetCaptureComponent2D() : nullptr)
	{
		Capture->bCaptureEveryFrame = false;
	}
	SetItemHighlightsSuppressed(false);

	if (JobSettings.bWatermark)
	{
		DrawWatermark();
	}
	BeginReadback();
}

void UStageRenderSubsystem::DrawWatermark()
{
	using namespace StageRenderSubsystem;

	UFont* Font = GEngine ? GEngine->GetLargeFont() : nullptr;
	if (!Font)
	{
		UE_LOG(LogStageCraft, Warning, TEXT("Render: no engine font is available; the watermark is skipped."));
		return;
	}

	UCanvas* Canvas = nullptr;
	FVector2D CanvasSize;
	FDrawToRenderTargetContext Context;
	UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(this, RenderTarget, Canvas, CanvasSize, Context);
	if (!Canvas)
	{
		return;
	}

	// Sized from the image height, so the footer looks the same at every resolution and after supersampling: the text is
	// measured once at scale 1 and scaled to fill WatermarkTextFraction of the bar, whatever the engine font's size is.
	const double BarHeight = FMath::RoundToDouble(CanvasSize.Y * WatermarkBarFraction);
	const FString Text = StageRender::MakeWatermarkText(JobSceneName, JobLocalTime, JobOutputSize, JobItemCount);
	double UnscaledWidth = 0.0;
	double UnscaledHeight = 0.0;
	Canvas->TextSize(Font, Text, UnscaledWidth, UnscaledHeight, 1.0, 1.0);
	const double TextScale = UnscaledHeight > 0.0 ? BarHeight * WatermarkTextFraction / UnscaledHeight : 1.0;

	FCanvasTileItem Bar(FVector2D(0.0, CanvasSize.Y - BarHeight), FVector2D(CanvasSize.X, BarHeight), FLinearColor(0.f, 0.f, 0.f, 0.55f));
	Bar.BlendMode = SE_BLEND_Translucent;
	Canvas->DrawItem(Bar);

	double TextWidth = 0.0;
	double TextHeight = 0.0;
	Canvas->TextSize(Font, Text, TextWidth, TextHeight, TextScale, TextScale);
	const double Margin = BarHeight * 0.5;
	FCanvasTextItem Label(FVector2D(CanvasSize.X - TextWidth - Margin, CanvasSize.Y - BarHeight + (BarHeight - TextHeight) * 0.5),
		FText::FromString(Text), Font, FLinearColor(0.92f, 0.92f, 0.92f));
	Label.Scale = FVector2D(TextScale, TextScale);
	Canvas->DrawItem(Label);

	UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(this, Context);
}

void UStageRenderSubsystem::BeginReadback()
{
	FTextureRenderTargetResource* Resource = RenderTarget ? RenderTarget->GameThread_GetRenderTargetResource() : nullptr;
	if (!Resource)
	{
		Finish(false, FString(), LOCTEXT("NoTarget", "The render target was lost before the image could be read."));
		return;
	}

	Readback = MakeShared<FStageRenderReadback, ESPMode::ThreadSafe>();
	Readback->Size = JobInternalSize;
	ENQUEUE_RENDER_COMMAND(StageRenderEnqueueReadback)([Shared = Readback, Resource](FRHICommandListImmediate& RHICmdList)
	{
		FRHITexture* Texture = Resource->GetRenderTargetTexture();
		if (!Texture)
		{
			Shared->bComplete.store(true, std::memory_order_release);
			return;
		}
		Shared->GpuReadback = MakeUnique<FRHIGPUTextureReadback>(TEXT("StageRenderReadback"));
		RHICmdList.Transition(FRHITransitionInfo(Texture, ERHIAccess::Unknown, ERHIAccess::CopySrc));
		Shared->GpuReadback->EnqueueCopy(RHICmdList, Texture, FIntVector::ZeroValue, 0, FIntVector::ZeroValue);
		RHICmdList.Transition(FRHITransitionInfo(Texture, ERHIAccess::CopySrc, ERHIAccess::SRVMask));
	});

	SetState(EStageRenderState::Reading);
	// The capture is finished with; releasing it now frees its view state (temporal history, Lumen caches) early.
	if (CaptureActor)
	{
		CaptureActor->Destroy();
		CaptureActor = nullptr;
	}
	ScheduleNextFrame(&ThisClass::PollReadback);
}

void UStageRenderSubsystem::PollReadback()
{
	if (State != EStageRenderState::Reading || !Readback.IsValid())
	{
		return;
	}

	if (Readback->bComplete.load(std::memory_order_acquire))
	{
		if (!Readback->bSucceeded.load(std::memory_order_acquire))
		{
			Finish(false, FString(), LOCTEXT("ReadFailed", "The image could not be read back from the GPU."));
			return;
		}
		BeginEncode();
		return;
	}

	if (++ReadbackPolls > StageRenderSubsystem::MaxReadbackPolls)
	{
		Finish(false, FString(), LOCTEXT("ReadTimeout", "The GPU did not return the image in time."));
		return;
	}

	// The render thread maps the buffer only once the GPU fence has passed, so neither thread ever waits on the GPU.
	if (!Readback->bPollQueued.exchange(true))
	{
		ENQUEUE_RENDER_COMMAND(StageRenderPollReadback)([Shared = Readback](FRHICommandListImmediate&)
		{
			Shared->bPollQueued.store(false);
			if (Shared->bComplete.load(std::memory_order_acquire) || !Shared->GpuReadback || !Shared->GpuReadback->IsReady())
			{
				return;
			}

			int32 RowPitchInPixels = 0;
			int32 BufferHeight = 0;
			const uint8* Mapped = static_cast<const uint8*>(Shared->GpuReadback->Lock(RowPitchInPixels, &BufferHeight));
			const int32 Width = Shared->Size.X;
			const int32 Height = Shared->Size.Y;
			const bool bValid = Mapped && RowPitchInPixels >= Width && BufferHeight >= Height;
			if (bValid)
			{
				constexpr int32 BytesPerPixel = 4;
				Shared->Pixels.SetNumUninitialized(int64(Width) * Height * BytesPerPixel);
				for (int32 Row = 0; Row < Height; ++Row)
				{
					FMemory::Memcpy(Shared->Pixels.GetData() + int64(Row) * Width * BytesPerPixel,
						Mapped + int64(Row) * RowPitchInPixels * BytesPerPixel, int64(Width) * BytesPerPixel);
				}
			}
			Shared->GpuReadback->Unlock();
			Shared->GpuReadback.Reset();
			Shared->bSucceeded.store(bValid, std::memory_order_release);
			Shared->bComplete.store(true, std::memory_order_release);
		});
	}
	ScheduleNextFrame(&ThisClass::PollReadback);
}

void UStageRenderSubsystem::BeginEncode()
{
	SetState(EStageRenderState::Encoding);

	// From here the job owns only CPU data; the GPU resources go now.
	TArray64<uint8> Pixels = MoveTemp(Readback->Pixels);
	Readback.Reset();
	RenderTarget = nullptr;

	const FString Directory = GetOutputDirectory();
	const FString FileName = StageRender::MakeFileName(JobSceneName, JobLocalTime, JobOutputSize);
	UE::Tasks::Launch(UE_SOURCE_LOCATION, [WeakThis = TWeakObjectPtr<UStageRenderSubsystem>(this), ForJob = JobSerial, Pixels = MoveTemp(Pixels),
		InternalSize = JobInternalSize, OutputSize = JobOutputSize, Directory, FileName]() mutable
	{
		FImage Image(InternalSize.X, InternalSize.Y, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		Image.RawData = MoveTemp(Pixels);

		// The final colour's alpha is not coverage; a delivered still is always opaque.
		for (int64 Alpha = 3; Alpha < Image.RawData.Num(); Alpha += 4)
		{
			Image.RawData[Alpha] = 255;
		}

		if (InternalSize != OutputSize)
		{
			// Gamma-correct downsample of the supersampled image (ImageCore filters in linear light).
			FImage Output;
			FImageCore::ResizeImageAllocDest(Image, Output, OutputSize.X, OutputSize.Y, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
			Image = MoveTemp(Output);
		}

		TArray64<uint8> Png;
		FString FilePath;
		bool bSucceeded = FImageUtils::CompressImage(Png, TEXT("png"), Image);
		FText Error;
		if (!bSucceeded)
		{
			Error = LOCTEXT("EncodeFailed", "The image could not be compressed to PNG.");
		}
		else
		{
			IFileManager::Get().MakeDirectory(*Directory, /*Tree*/ true);
			FilePath = StageRenderSubsystem::MakeUniquePath(Directory, FileName);
			bSucceeded = StageRenderSubsystem::WriteFileAtomic(FilePath, Png);
			if (!bSucceeded)
			{
				Error = FText::Format(LOCTEXT("WriteFailed", "The image could not be written to {0}."), FText::FromString(Directory));
			}
		}

		AsyncTask(ENamedThreads::GameThread, [WeakThis, ForJob, bSucceeded, FilePath, Error]()
		{
			if (UStageRenderSubsystem* This = WeakThis.Get())
			{
				This->HandleEncodeFinished(ForJob, bSucceeded, FilePath, Error);
			}
		});
	});
}

void UStageRenderSubsystem::HandleEncodeFinished(uint32 ForJob, bool bSucceeded, const FString& FilePath, const FText& Error)
{
	if (ForJob != JobSerial || State != EStageRenderState::Encoding)
	{
		return;
	}
	Finish(bSucceeded, FilePath, bSucceeded
		? FText::Format(LOCTEXT("Saved", "Render saved: {0}"), FText::FromString(FPaths::GetCleanFilename(FilePath)))
		: Error);
}

void UStageRenderSubsystem::Finish(bool bSucceeded, const FString& FilePath, const FText& Message)
{
	ReleaseCaptureResources();

	LastResult = FStageRenderResult();
	LastResult.bSucceeded = bSucceeded;
	LastResult.FilePath = FilePath;
	LastResult.Size = JobOutputSize;
	LastResult.Seconds = FPlatformTime::Seconds() - JobStartSeconds;
	LastResult.Message = Message;

	UE_LOG(LogStageCraft, Log, TEXT("Render %s in %.2f s: %s"), bSucceeded ? TEXT("finished") : TEXT("failed"), LastResult.Seconds,
		bSucceeded ? *FilePath : *Message.ToString());

	SetState(EStageRenderState::Idle);
	OnRenderFinished.Broadcast(LastResult);
}

void UStageRenderSubsystem::ReleaseCaptureResources()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearAllTimersForObject(this);
	}
	SetItemHighlightsSuppressed(false);
	if (CaptureActor)
	{
		CaptureActor->Destroy();
		CaptureActor = nullptr;
	}
	RenderTarget = nullptr;

	if (Readback.IsValid())
	{
		// The readback buffer is an RHI resource: release it on the render thread, after any poll still queued there.
		ENQUEUE_RENDER_COMMAND(StageRenderReleaseReadback)([Shared = MoveTemp(Readback)](FRHICommandListImmediate&) mutable
		{
			Shared.Reset();
		});
	}
}

void UStageRenderSubsystem::CancelJob()
{
	if (!IsRendering())
	{
		return;
	}
	// A task still encoding reports with this job's serial and is ignored.
	++JobSerial;
	UE_LOG(LogStageCraft, Log, TEXT("Render cancelled (%s): its world is going away."), *UEnum::GetValueAsString(State));
	ReleaseCaptureResources();
	State = EStageRenderState::Idle;
}

#undef LOCTEXT_NAMESPACE
