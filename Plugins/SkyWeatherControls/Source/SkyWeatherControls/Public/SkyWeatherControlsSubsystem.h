// Copyright DamenTools internal. Sky & Weather Controls plugin.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "SkyWeatherControlsSubsystem.generated.h"

class USkyWeatherControlsWidget;

/**
 * Puts the Time of Day / Weather dropdowns on screen for any game world that contains
 * Ultra Dynamic Sky.
 *
 * Implemented as a world subsystem specifically so no level, GameMode or HUD Blueprint
 * has to be edited to opt in -- levels without UDS simply never spawn the panel.
 *
 * Console:
 *   SkyWeather.AutoShow 0|1   suppress the automatic panel (read at world begin play)
 *   SkyWeather.Toggle         show/hide the panel in the current world
 */
UCLASS()
class SKYWEATHERCONTROLS_API USkyWeatherControlsSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;

	/** Creates the panel on demand and shows or hides it. */
	void SetControlsVisible(bool bVisible);
	bool AreControlsVisible() const;

private:
	/** Retried on a timer: UDS and the local player may not exist on the first frame. */
	void TrySpawnControls();

	UPROPERTY(Transient)
	TObjectPtr<USkyWeatherControlsWidget> Controls;

	FTimerHandle SpawnTimerHandle;
	double SpawnDeadlineSeconds = 0.0;
};
