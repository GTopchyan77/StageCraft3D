// Copyright DamenTools internal. Spline Route Tool plugin.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "SplineRouteActor.h" // ERouteVehicleType
#include "SplineRouteSaveGame.generated.h"

/** One control point of a saved route: world location + its per-point speed override + reverse flag. */
USTRUCT()
struct FSavedRoutePoint
{
	GENERATED_BODY()

	UPROPERTY()
	FVector Location = FVector::ZeroVector;

	/** Speed override at this point, or -1 for "use base speed". */
	UPROPERTY()
	float SpeedOverride = -1.0f;

	UPROPERTY()
	bool bReverse = false;
};

/** A saved route: its medium + ordered control points. */
USTRUCT()
struct FSavedRoute
{
	GENERATED_BODY()

	UPROPERTY()
	ERouteVehicleType VehicleType = ERouteVehicleType::Vessel;

	UPROPERTY()
	TArray<FSavedRoutePoint> Points;
};

/** A saved vehicle assignment: which catalog entry, attached to which saved route, at what size. */
USTRUCT()
struct FSavedVehicle
{
	GENERATED_BODY()

	UPROPERTY()
	int32 CatalogIndex = 0;

	/** Index into the saved Routes array this vehicle follows (-1 = none). */
	UPROPERTY()
	int32 RouteIndex = INDEX_NONE;

	UPROPERTY()
	float SizeMultiplier = 1.0f;

	/** Resume position: how far along the route's spline this vessel had travelled when saved. */
	UPROPERTY()
	float DistanceAlongSpline = 0.0f;
};

/** A saved yard obstruction: catalog index + full transform. */
USTRUCT()
struct FSavedObstruction
{
	GENERATED_BODY()

	UPROPERTY()
	int32 CatalogIndex = INDEX_NONE;

	UPROPERTY()
	FName TypeId;

	UPROPERTY()
	FTransform Transform = FTransform::Identity;
};

/**
 * Runtime scenario snapshot: routes (with per-point speed/reverse + medium), vehicle assignments,
 * obstructions, and global playback settings. Written/read via UGameplayStatics save slots.
 */
UCLASS()
class SPLINEROUTETOOL_API USplineRouteSaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	UPROPERTY()
	TArray<FSavedRoute> Routes;

	UPROPERTY()
	TArray<FSavedVehicle> Vehicles;

	UPROPERTY()
	TArray<FSavedObstruction> Obstructions;

	UPROPERTY()
	float TimeScale = 1.0f;

	UPROPERTY()
	float VesselSizeMultiplier = 1.0f;

	UPROPERTY()
	float VesselTurnRate = 90.0f;

	/**
	 * Convoy spacing captured from the authoring subsystem. -1 means "not present in this slot"
	 * (slots written before these fields existed), in which case the loader leaves the live
	 * mesh-derived values alone instead of stomping them with a bogus default.
	 */
	UPROPERTY()
	float MinFollowDistanceMeters = -1.0f;

	UPROPERTY()
	float MinSpawnGapMeters = -1.0f;
};
