// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Environment/StageEnvironmentBackend.h"
#include "StageSkyCreatorWeather.h"
#include "StageSkyCreatorBackend.generated.h"

/**
 * Sky backend on the Sky Creator plugin (Docs/ADR/0009-outdoor-environment.md §3).
 *
 * Sky on: spawns one transient ASkyCreator (sun, moon, sky atmosphere, volumetric clouds, sky light, height fog, rain and
 * snow particles) and hides the level's own sky components while it exists, so the stage is never lit twice. Each Apply
 * sets the sun path, the time of day and the weather on that actor; StageSkyCreator::MakeWeather maps the normalized
 * settings onto Sky Creator's weather settings.
 * Sky off: destroys the actor and makes the hidden components visible again.
 *
 * The sky actor ticks (cloud wind, occlusion captures, particle simulation); that is Sky Creator's own work and only
 * runs while the outdoor sky is on. Nothing here ticks.
 */
UCLASS()
class UStageSkyCreatorBackend : public UStageEnvironmentBackend
{
	GENERATED_BODY()

public:
	//~ Begin UStageEnvironmentBackend Interface
	virtual void Apply(UWorld& World, const struct FStageEnvironmentSettings& Settings) override;
	virtual void Shutdown() override;
	virtual FText GetDisplayName() const override;
	//~ End UStageEnvironmentBackend Interface

private:
	class ASkyCreator* SpawnSky(UWorld& World);
	void HideLevelSky(UWorld& World, const AActor& SkyActor);
	void RemoveSky();

	/** The sky actor's own default weather, read once when it is spawned: the base StageSkyCreator::MakeWeather maps onto. */
	UPROPERTY(Transient)
	FSkyCreatorWeatherSettings BaseWeather;

	/** The sky actor this backend spawned; owned by the world, destroyed by RemoveSky. */
	TWeakObjectPtr<class ASkyCreator> Sky;

	/** Level components hidden while the sky is on, made visible again when it goes. Weak: the level may unload first. */
	TArray<TWeakObjectPtr<class USceneComponent>> HiddenLevelComponents;
};
