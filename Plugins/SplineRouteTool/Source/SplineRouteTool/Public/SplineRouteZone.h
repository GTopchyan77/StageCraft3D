// Copyright DamenTools internal. Spline Route Tool plugin.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SplineRouteZone.generated.h"

class UBoxComponent;

/** What a zone marks the area as for spline-route placement. */
UENUM(BlueprintType)
enum class ESplineRouteZoneType : uint8
{
	/** Placement is allowed inside this zone. If any Water zone exists in the level, points may only be placed inside one. */
	Water UMETA(DisplayName = "Water (placement allowed)"),

	/** Placement is blocked inside this zone, regardless of any Water zones. */
	Ground UMETA(DisplayName = "Ground (placement blocked)")
};

/**
 * Authoring zone for the spline-route tool. Place one or more in a level to restrict where
 * the user can drop / drag spline control points.
 *
 * Policy (implemented in USplineRouteAuthoringSubsystem::CanPlaceAt):
 *   1. Inside any Ground zone  -> blocked.
 *   2. Any Water zone present in level -> point must be inside one to be allowed.
 *   3. No zones in level at all -> placement allowed everywhere (back-compat).
 */
UCLASS(BlueprintType, Blueprintable)
class SPLINEROUTETOOL_API ASplineRouteZone : public AActor
{
	GENERATED_BODY()

public:
	ASplineRouteZone();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Spline Route|Zone")
	TObjectPtr<UBoxComponent> Box;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Zone")
	ESplineRouteZoneType ZoneType = ESplineRouteZoneType::Water;

	/** XY-only containment test by default. Set bUseZ true to clip on Z too. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Zone")
	bool ContainsLocation(const FVector& WorldLocation, bool bUseZ = false) const;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	void ApplyShapeColor();
};
