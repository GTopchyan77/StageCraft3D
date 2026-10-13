// Copyright DamenTools internal. Spline Route Tool plugin.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "SplineRouteActor.h" // ERouteVehicleType
#include "VesselCatalog.generated.h"

class UStaticMesh;
class ADolphinCompanion;
class ASplineFollowerVessel;

/**
 * Optional decorative companion that swims alongside a vessel. Cosmetic only: the vessel spawns,
 * attaches and destroys it, and it is intentionally not part of the scenario save format.
 */
USTRUCT(BlueprintType)
struct FDolphinCompanionSettings
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dolphin Companion")
	bool bSpawnCompanion = false;

	/** Soft class so the catalog points at BP_DolphinCompanion — mesh and swim animation stay authored on the Blueprint. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dolphin Companion", meta = (EditCondition = "bSpawnCompanion"))
	TSoftClassPtr<ADolphinCompanion> CompanionClass;

	/** Station-keeping offset from the vessel root in unscaled root space (X forward, Y right, Z up). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dolphin Companion", meta = (EditCondition = "bSpawnCompanion"))
	FVector RelativeOffset = FVector(-400.0f, 500.0f, -50.0f);

	/** Companion scale. Independent of the vessel's size multiplier, which lives on the vessel mesh rather than its root. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dolphin Companion", meta = (EditCondition = "bSpawnCompanion", ClampMin = "0.01"))
	float Scale = 1.0f;

	/** Peak vertical displacement of the swim bob, in cm. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dolphin Companion", meta = (EditCondition = "bSpawnCompanion", ClampMin = "0.0"))
	float BobAmplitude = 15.0f;

	/** Bob frequency in Hz, measured at time-scale 1.0. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dolphin Companion", meta = (EditCondition = "bSpawnCompanion", ClampMin = "0.0"))
	float BobSpeed = 0.5f;

	/** Corrective mesh rotation for the companion, same DCC up/forward-axis fixup as FVesselCatalogEntry::RotationOffset.
	 *  Default 90 degrees yaw: the imported Dolphin mesh's longest bind-pose axis is Y, not X. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dolphin Companion", meta = (EditCondition = "bSpawnCompanion"))
	FRotator MeshRotationOffset = FRotator(0.0f, 90.0f, 0.0f);
};

/**
 * Per-vessel slider range. Considered "set" if Max > Min — otherwise the HUD
 * uses its built-in fallback (the Min/Max/Default UPROPERTYs on the HUD widget).
 * Default is clamped into [Min, Max] when applied.
 */
USTRUCT(BlueprintType)
struct FVesselSliderRange
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Range")
	float Min = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Range")
	float Max = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Range")
	float Default = 0.0f;

	FVesselSliderRange() = default;
	FVesselSliderRange(float InMin, float InMax, float InDefault)
		: Min(InMin), Max(InMax), Default(InDefault) {}

	bool IsSet() const { return Max > Min; }
};

USTRUCT(BlueprintType)
struct FVesselCatalogEntry
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vessel")
	FText DisplayName;

	/** Whether this entry is a waterborne vessel or a land vehicle. Spawning sets the route's medium to match. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vessel")
	ERouteVehicleType VehicleType = ERouteVehicleType::Vessel;

	/**
	 * Blueprint child of ASplineFollowerVessel with this vessel's mesh baked directly into
	 * MeshComponent's default Static Mesh — replaces the old raw TSoftObjectPtr<UStaticMesh> field.
	 * Every other field below still applies as an authoring-time override on top of the Blueprint's
	 * defaults (applied via Configure() on spawn), so the HUD sliders keep working exactly as before.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vessel")
	TSoftClassPtr<ASplineFollowerVessel> VesselClass;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vessel")
	FVector RelativeScale = FVector(1.0f, 1.0f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vessel", meta = (ClampMin = "0.0"))
	float DefaultSpeed = 200.0f;

	/** Z offset applied to the follower so the vessel sits above the spline plane (matches the mesh's pivot). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vessel")
	float HeightOffset = 50.0f;

	/** Turn rate (deg/sec) the follower eases toward the tangent at. Lower = wider, slower turns for big vessels. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vessel", meta = (ClampMin = "0.0"))
	float TurnRateDegPerSec = 90.0f;

	/**
	 * Fixed corrective rotation applied to the mesh (not the actor/route heading) — for meshes exported
	 * from DCC tools with a different up-axis convention (e.g. Blender's Z-up-but-Y-forward export producing
	 * a 90-degree tilt in UE). Zero for meshes that already import correctly.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vessel")
	FRotator RotationOffset = FRotator::ZeroRotator;

	/** Seconds before/after a reverse waypoint over which speed ramps down/up. Time-based (not distance-based)
	 *  so the slowdown takes the same wall-clock duration regardless of vessel speed or TimeScale. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vessel", meta = (ClampMin = "0.0"))
	float ReverseApproachSeconds = 3.0f;

	/** Speed multiplier applied exactly at a reverse waypoint (1.0 = no slowdown, 0.1 = 10% speed). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vessel", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ReverseMinSpeedFactor = 0.1f;

	/** Optional decorative dolphin swimming alongside this vessel. Off by default. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vessel|Companion")
	FDolphinCompanionSettings DolphinCompanion;
};

/** A placeable yard-layout obstruction type (shipping container, barrier, steel-part stack...). */
USTRUCT(BlueprintType)
struct FObstructionCatalogEntry
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Obstruction")
	FText DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Obstruction")
	TSoftObjectPtr<UStaticMesh> Mesh;

	/** World scale applied to the mesh. With the default engine cube (100u) this reads as size-in-metres x100. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Obstruction")
	FVector Scale = FVector(6.0f, 2.4f, 2.6f);
};

UCLASS(BlueprintType)
class SPLINEROUTETOOL_API UVesselCatalog : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vessels")
	TArray<FVesselCatalogEntry> Entries;

	/** Yard-layout obstruction types for the placement panel. If empty, a single "Container" cube is offered. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Obstructions")
	TArray<FObstructionCatalogEntry> Obstructions;

	/** Ship Size slider — multiplier applied on top of each entry's RelativeScale. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUD|Slider Ranges")
	FVesselSliderRange VesselSizeRange { 0.25f, 10.0f, 1.0f };

	/** Segment Speed slider — units/sec for the selected route segment. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUD|Slider Ranges")
	FVesselSliderRange SegmentSpeedRange { 50.0f, 500.0f, 200.0f };

	/** Waypoint Size slider — authoring handle radius on the active route. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUD|Slider Ranges")
	FVesselSliderRange HandleSizeRange { 20.0f, 400.0f, 150.0f };

	/** Time Scale slider — global playback time multiplier. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUD|Slider Ranges")
	FVesselSliderRange TimeScaleRange { 0.0f, 5.0f, 1.0f };

	/** Turn Rate slider — deg/sec the live vessels ease toward the tangent at (0 = instant snap). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUD|Slider Ranges")
	FVesselSliderRange TurnRateRange { 0.0f, 180.0f, 90.0f };
};
