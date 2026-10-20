// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Environment/StageEnvironmentTypes.h"
#include "Subsystems/WorldSubsystem.h"
#include "StageEnvironmentSubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStageEnvironmentChanged, const FStageEnvironmentSettings&, Settings);

/**
 * The outdoor environment of the stage being shown: time of day, sun path and weather (Docs/ADR/0009-outdoor-environment.md).
 *
 * Source of truth for FStageEnvironmentSettings. SetSettings is the single write path: it sanitizes, stores, hands the
 * settings to the sky backend and broadcasts OnEnvironmentChanged, which views read back from. Nothing ticks or polls;
 * the backend's sky actor animates clouds and precipitation on its own and exists only while the sky is on.
 *
 * The backend class comes from config ([/Script/ModularSceneBuilder.StageEnvironmentSubsystem] BackendClass in
 * DefaultGame.ini), so the sky plugin stays out of this module. Without a backend the settings are still kept, the sky
 * cannot be turned on, and IsSkyAvailable is false so the panel can say why.
 *
 * World subsystem (Game and PIE): the environment belongs to the stage being shown and ends with its world.
 */
UCLASS(Config = Game)
class MODULARSCENEBUILDER_API UStageEnvironmentSubsystem : public UWorldSubsystem
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

	UFUNCTION(BlueprintPure, Category = "StageCraft|Environment")
	FStageEnvironmentSettings GetSettings() const { return Settings; }

	/**
	 * Stores NewSettings, sanitized, and applies them to the sky. Turning the sky on without a backend is refused: the
	 * other values are still stored, bSkyEnabled stays false and a warning is logged. Broadcasts OnEnvironmentChanged when
	 * anything changed, so callers read back the stored values rather than what they asked for.
	 */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Environment")
	void SetSettings(const FStageEnvironmentSettings& NewSettings);

	/** Sets the weather values of Preset (StageEnvironment::ApplyWeatherPreset) through SetSettings. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Environment")
	void ApplyWeatherPreset(EStageWeatherPreset Preset);

	/** Whether a sky backend is installed, i.e. whether the sky can be turned on. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Environment")
	bool IsSkyAvailable() const { return Backend != nullptr; }

	/** The sky system's name, or empty without a backend. */
	FText GetSkySystemName() const;

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Environment")
	FOnStageEnvironmentChanged OnEnvironmentChanged;

protected:
	//~ Begin UWorldSubsystem Interface
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	//~ End UWorldSubsystem Interface

private:
	void ApplyToBackend();

	/** Sky backend to create. A native class from another module, or empty for none. */
	UPROPERTY(Config)
	TSoftClassPtr<class UStageEnvironmentBackend> BackendClass;

	UPROPERTY(Transient)
	TObjectPtr<class UStageEnvironmentBackend> Backend = nullptr;

	FStageEnvironmentSettings Settings;

	/** The backend is only called once the world has begun play: before that, there is no world to put a sky into. */
	bool bWorldBegunPlay = false;
};
