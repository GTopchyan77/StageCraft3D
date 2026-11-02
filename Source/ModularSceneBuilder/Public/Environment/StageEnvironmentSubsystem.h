// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Environment/StageEnvironmentTypes.h"
#include "Subsystems/WorldSubsystem.h"
#include "StageEnvironmentSubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStageEnvironmentChanged, const FStageEnvironmentSettings&, Settings);

/**
 * The environment of the stage being shown: what lights it, time of day, sun path, weather and the camera look
 * (Docs/ADR/0009-outdoor-environment.md, Docs/ADR/0024-startup-atmospheres-starter-stages.md §3-5).
 *
 * Source of truth for FStageEnvironmentSettings. SetSettings is the single write path: it sanitizes, stores and broadcasts
 * OnEnvironmentChanged, which views read back from. What is shown follows the stored settings:
 *  - Level: the level's own lights, untouched.
 *  - OutdoorSky: the sky backend (config: BackendClass) replaces them.
 *  - Studio: AStageStudioRig replaces them (a soft HDRI sky light, a key light, a light backdrop haze).
 *  - The look (exposure, bloom, AO, grading) goes to one unbound post-process component owned here, whatever lights the stage.
 *
 * A Blend transition eases what is shown from the old settings to the new ones over BlendSeconds; the stored settings (and
 * so undo, saving and the broadcast) are the new ones at once. Only a running blend ticks (IsTickable); at rest nothing
 * ticks or polls. The backend's sky actor animates its own clouds and precipitation.
 *
 * World subsystem (Game and PIE): the environment belongs to the stage being shown and ends with its world.
 */
UCLASS(Config = Game)
class MODULARSCENEBUILDER_API UStageEnvironmentSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem Interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	//~ Begin UWorldSubsystem Interface
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	//~ End UWorldSubsystem Interface

	//~ Begin FTickableGameObject Interface
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override;
	virtual TStatId GetStatId() const override;
	//~ End FTickableGameObject Interface

	UFUNCTION(BlueprintPure, Category = "StageCraft|Environment")
	FStageEnvironmentSettings GetSettings() const { return Settings; }

	/**
	 * Stores NewSettings, sanitized, and shows them, at once or eased over BlendSeconds (Transition). Turning the outdoor sky
	 * on without a backend is refused: the other values are still stored, the lighting falls back to the level's and a warning
	 * is logged. Broadcasts OnEnvironmentChanged when anything changed, so callers read back the stored values rather than
	 * what they asked for. A new change during a blend starts the next blend from what is on screen.
	 */
	void SetSettings(const FStageEnvironmentSettings& NewSettings, EStageEnvironmentTransition Transition = EStageEnvironmentTransition::Instant);

	/** Blueprint entry of SetSettings: always shown at once. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Environment", meta = (DisplayName = "Set Settings"))
	void SetSettingsInstant(const FStageEnvironmentSettings& NewSettings) { SetSettings(NewSettings, EStageEnvironmentTransition::Instant); }

	/** Sets the weather values of Preset (StageEnvironment::ApplyWeatherPreset) through SetSettings. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Environment")
	void ApplyWeatherPreset(EStageWeatherPreset Preset);

	/** Whether a sky backend is installed, i.e. whether the outdoor sky can be turned on. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Environment")
	bool IsSkyAvailable() const { return Backend != nullptr; }

	/** The sky system's name, or empty without a backend. */
	FText GetSkySystemName() const;

	/** True while a Blend transition is on screen. */
	bool IsBlending() const { return bBlending; }

	/** What is on screen right now: the stored settings, or a point of the running blend. */
	const FStageEnvironmentSettings& GetDisplayedSettings() const { return Displayed; }

	/** Whether the studio rig lights with its HDRI cubemap (false: missing or not loaded yet, a flat fallback light is used). */
	bool IsStudioHdriAvailable() const;

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Environment")
	FOnStageEnvironmentChanged OnEnvironmentChanged;

protected:
	//~ Begin UWorldSubsystem Interface
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	//~ End UWorldSubsystem Interface

private:
	/** Puts Show on screen: the backend, the studio rig and the post-process look. */
	void Present(const FStageEnvironmentSettings& Show);
	void ApplyLook(const FStageEnvironmentLook& Look);
	void EnsureStudioRig(UWorld& World);
	void RemoveStudioRig();
	void HandleStudioCubemapLoaded();

	/** Sky backend to create. A native class from another module, or empty for none. */
	UPROPERTY(Config)
	TSoftClassPtr<class UStageEnvironmentBackend> BackendClass;

	/** The studio rig's HDRI sky (ADR 0024 §4). Loaded asynchronously the first time the studio is shown; empty or missing: flat light. */
	UPROPERTY(Config)
	TSoftObjectPtr<class UTextureCube> StudioCubemap;

	/** Seconds a Blend transition takes. */
	UPROPERTY(Config)
	float BlendSeconds = 2.f;

	UPROPERTY(Transient)
	TObjectPtr<class UStageEnvironmentBackend> Backend = nullptr;

	/** Holds the post-process look. Spawned at world BeginPlay, transient, destroyed with the subsystem. */
	TWeakObjectPtr<class AActor> LookActor;
	TWeakObjectPtr<class UPostProcessComponent> LookComponent;

	/** Exists only while the studio lights the stage. */
	TWeakObjectPtr<class AStageStudioRig> StudioRig;

	/** The studio cubemap's load in flight or done; kept so the cubemap stays in memory while the rig may use it. */
	TSharedPtr<struct FStreamableHandle> StudioCubemapHandle;

	FStageEnvironmentSettings Settings;

	/** What is on screen; equals Settings except during a blend. */
	FStageEnvironmentSettings Displayed;
	FStageEnvironmentSettings BlendFrom;
	float BlendElapsed = 0.f;
	bool bBlending = false;

	/** The backend and the rig are only touched once the world has begun play: before that, there is no world to light. */
	bool bWorldBegunPlay = false;
};
