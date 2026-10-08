// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Camera/CameraTypes.h"
#include "Render/StageRenderTypes.h"
#include "Subsystems/WorldSubsystem.h"
#include "StageRenderSubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStageRenderStateChanged, EStageRenderState, NewState);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStageRenderFinished, const FStageRenderResult&, Result);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStageRenderSettingsChanged, const FStageRenderSettings&, Settings);

/** What a render needs from the requester, taken at the moment of the request. */
struct FStageRenderRequest
{
	/** The view to render: the local player's camera position, rotation, field of view and camera post-processing. */
	FMinimalViewInfo View;

	/** Editor helpers that must never appear in a delivered image (gizmo, placement ghost and marker). */
	TArray<TWeakObjectPtr<class AActor>> HiddenActors;
};

/**
 * High-resolution still renders of the stage (Docs/ADR/0004-selection-scenes-and-rendering.md §5).
 *
 * A render is a short job with explicit states (EStageRenderState), one at a time:
 *  1. Capturing: a transient scene capture renders the requested view into a render target at the internal size
 *     (the output size, or larger for supersampling) for a few frames, so temporal anti-aliasing, auto exposure and
 *     Lumen settle. The capture is configured to match the viewport's lighting (scene captures default to no Lumen and
 *     no temporal AA), and hides editor helpers and selection overlays.
 *  2. Reading: the finished image is copied to a GPU readback buffer; the render thread maps it only once the GPU fence
 *     has passed, so the game thread never waits on the GPU.
 *  3. Encoding: a background task filters a supersampled image down, compresses the PNG and writes it atomically to
 *     Saved/Renders. The game thread keeps running; it only learns the result.
 * Frame steps use next-tick timers that exist only while a job runs: nothing ticks or polls while idle.
 *
 * The viewport keeps rendering throughout. The GPU work of the capture frames does lower the frame rate briefly
 * (well under a second for Full HD; more for 4K Supersampled).
 *
 * Settings are the user's (UStageCraftUserSettings, saved on change); this subsystem is their only writer.
 * World subsystem (Game and PIE): a render belongs to the stage being shown and is cancelled with its world.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageRenderSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem Interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	UFUNCTION(BlueprintPure, Category = "StageCraft|Render")
	FStageRenderSettings GetSettings() const;

	/** Stores (sanitized) and saves the user's render settings, then broadcasts OnSettingsChanged. Allowed while rendering; affects the next render. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Render")
	void SetSettings(const FStageRenderSettings& NewSettings);

	/**
	 * Starts a render of Request.View with the current settings. Returns false, with a user-facing reason, when it cannot
	 * start (a render is already running, no world). The result arrives on OnRenderFinished.
	 */
	bool StartRender(const FStageRenderRequest& Request, FText& OutReason);

	UFUNCTION(BlueprintPure, Category = "StageCraft|Render")
	EStageRenderState GetState() const { return State; }

	UFUNCTION(BlueprintPure, Category = "StageCraft|Render")
	bool IsRendering() const { return State != EStageRenderState::Idle; }

	UFUNCTION(BlueprintPure, Category = "StageCraft|Render")
	const FStageRenderResult& GetLastResult() const { return LastResult; }

	/** Where renders are written: <Project>/Saved/Renders. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Render")
	FString GetOutputDirectory() const;

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Render")
	FOnStageRenderStateChanged OnRenderStateChanged;

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Render")
	FOnStageRenderFinished OnRenderFinished;

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Render")
	FOnStageRenderSettingsChanged OnSettingsChanged;

protected:
	//~ Begin UWorldSubsystem Interface
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	//~ End UWorldSubsystem Interface

private:
	using FJobStep = void (UStageRenderSubsystem::*)();

	void SetState(EStageRenderState NewState);
	void ScheduleNextFrame(FJobStep Step);

	bool CreateCaptureResources(const FStageRenderRequest& Request);
	void ConfigureCapture(class USceneCaptureComponent2D& Capture, const FStageRenderRequest& Request) const;
	void SetItemHighlightsSuppressed(bool bSuppressed);

	void StepCapture();
	void DrawWatermark();
	void BeginReadback();
	void PollReadback();
	void BeginEncode();
	void HandleEncodeFinished(uint32 ForJob, bool bSucceeded, const FString& FilePath, const FText& Error);

	void Finish(bool bSucceeded, const FString& FilePath, const FText& Message);
	void ReleaseCaptureResources();
	void CancelJob();

	UPROPERTY(Transient)
	TObjectPtr<class ASceneCapture2D> CaptureActor = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextureRenderTarget2D> RenderTarget = nullptr;

	/** Shared with render-thread commands, which may still hold it after a job was cancelled. */
	TSharedPtr<struct FStageRenderReadback, ESPMode::ThreadSafe> Readback;

	/** Items whose overlay is hidden for this job; restored when capturing ends. */
	TArray<TWeakObjectPtr<class AModularBaseActor>> SuppressedItems;

	// The running job. Valid while State != Idle.
	FStageRenderSettings JobSettings;
	FString JobSceneName;
	int32 JobItemCount = 0;
	FIntPoint JobOutputSize = FIntPoint::ZeroValue;
	FIntPoint JobInternalSize = FIntPoint::ZeroValue;
	FDateTime JobLocalTime;
	double JobStartSeconds = 0.0;
	int32 FramesRemaining = 0;
	int32 ReadbackPolls = 0;

	/** Increases with every job; late results of a cancelled job carry an older value and are dropped. */
	uint32 JobSerial = 0;

	EStageRenderState State = EStageRenderState::Idle;
	FStageRenderResult LastResult;

	/** Used only when the engine's settings object is not UStageCraftUserSettings (misconfigured project). */
	FStageRenderSettings FallbackSettings;
};
