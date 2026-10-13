// Copyright DamenTools internal. Spline Route Tool plugin.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "SplineRouteTimeSubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnSplineRouteTimeScaleChanged, float, NewScale);

/**
 * Global time-scale used by every USplineFollowerComponent.
 * GameInstance-scoped so it survives level transitions and is reachable from UMG widgets.
 */
UCLASS()
class SPLINEROUTETOOL_API USplineRouteTimeSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;

	/** Current time-scale multiplier. 1.0 = real-time, 0.0 = paused. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Time")
	float GetTimeScale() const { return TimeScale; }

	/** Set the multiplier (clamped to >= 0). Broadcasts OnTimeScaleChanged. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Time")
	void SetTimeScale(float NewScale);

	/** Broadcasts whenever SetTimeScale changes the value. */
	UPROPERTY(BlueprintAssignable, Category = "Spline Route|Time")
	FOnSplineRouteTimeScaleChanged OnTimeScaleChanged;

	/** Maximum slider value the UI clamps to. Tweakable. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spline Route|Time", meta = (ClampMin = "0.1"))
	float MaxTimeScale = 5.0f;

private:
	UPROPERTY()
	float TimeScale = 1.0f;
};
