// Copyright DamenTools internal. Spline Route Tool plugin.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "TimerManager.h"
#include "SplineRouteActor.generated.h"

class USplineComponent;
class USplineMeshComponent;
class UStaticMeshComponent;
class UStaticMesh;
class UMaterialInterface;
class UMaterialInstanceDynamic;

/** The medium a route's vehicles travel on. Drives placement constraints (water vs land zones) and handle tint. */
UENUM(BlueprintType)
enum class ERouteVehicleType : uint8
{
	/** Waterborne vessels — nodes constrained to Water zones. */
	Vessel UMETA(DisplayName = "Vessel (water)"),
	/** Terrestrial logistics (trucks, forklifts, terminal equipment) — nodes constrained to land/Ground zones. */
	Ground UMETA(DisplayName = "Ground vehicle (land)")
};

/** Broadcast whenever the spline's control points are added / moved / removed / cleared. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnSplineGeometryChanged);

/** Broadcast whenever the selected control-point index changes (incl. cleared to INDEX_NONE). */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnSelectedControlPointChanged, int32, NewIndex);

/**
 * Runtime spline route. Hosts a USplineComponent plus a per-control-point handle mesh.
 * Multiple instances can live in the same level — each represents an independent route.
 */
UCLASS(BlueprintType, Blueprintable)
class SPLINEROUTETOOL_API ASplineRouteActor : public AActor
{
	GENERATED_BODY()

public:
	ASplineRouteActor();

	/** Fires whenever the spline's control points are added / moved / removed / cleared. Followers use this to re-anchor. */
	UPROPERTY(BlueprintAssignable, Category = "Spline Route")
	FOnSplineGeometryChanged OnSplineGeometryChanged;

	/** Fires whenever the selected control point changes. */
	UPROPERTY(BlueprintAssignable, Category = "Spline Route")
	FOnSelectedControlPointChanged OnSelectedControlPointChanged;

	/** The spline that defines the route. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Spline Route")
	TObjectPtr<USplineComponent> Spline;

	/** Whether this route carries waterborne vessels or land vehicles. Governs placement constraints + handle tint. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spline Route")
	ERouteVehicleType VehicleType = ERouteVehicleType::Vessel;

	/** Change the route's vehicle type at runtime and re-tint its handles. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route")
	void SetVehicleType(ERouteVehicleType NewType);

	/** Mesh used for each visible control-point handle. Small box / sphere. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Appearance")
	TObjectPtr<UStaticMesh> HandleMesh;

	/** Material applied to handles when they are not selected. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Appearance")
	TObjectPtr<UMaterialInterface> HandleMaterial;

	/** Material applied to a handle when it is the currently selected control point. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Appearance")
	TObjectPtr<UMaterialInterface> SelectedHandleMaterial;

	/** Material applied to a handle that is flagged as a reverse node (and not currently selected). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Appearance")
	TObjectPtr<UMaterialInterface> ReverseHandleMaterial;

	/** Uniform scale of each handle mesh. Bumped from 150 -> 300 so waypoints read clearly and are easy to click. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Appearance", meta = (ClampMin = "1.0"))
	float HandleSize = 300.0f;

	/** Update HandleSize at runtime and re-apply scale to every existing handle (including the currently selected one's 1.4x bump). */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Appearance")
	void SetHandleSize(float NewSize);

	/** Mesh tiled along each spline segment to make the curve visible at runtime. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Appearance")
	TObjectPtr<UStaticMesh> SegmentMesh;

	/** Material applied to each segment mesh. Falls back to a runtime-generated MID if null. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Appearance")
	TObjectPtr<UMaterialInterface> SegmentMaterial;

	/** XY thickness of each segment mesh in world units (cylinder radius scaling). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Appearance", meta = (ClampMin = "0.1"))
	float SegmentThickness = 8.0f;

	/**
	 * Whether the tiled segment meshes (the tube visualising each spline segment) are actually visible.
	 * Component visibility, not colour/alpha — an Opaque material ignores Opacity entirely, so driving
	 * this via alpha=0 renders solid black instead of invisible. Default false: the route reads through
	 * waypoint handles + direction arrows, with the tube available as an opt-in visual.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spline Route|Appearance")
	bool bShowSegmentMeshes = false;

	// ---------------------------------------------------------------------------------------------
	// Direction arrows
	// ---------------------------------------------------------------------------------------------

	/** Draw travel-direction arrows along the route (start point -> end point). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spline Route|Direction")
	bool bShowDirectionArrows = true;

	/** Distance in world units between consecutive direction arrows along the spline. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spline Route|Direction", meta = (ClampMin = "50.0"))
	float ArrowSpacing = 2000.0f;

	/** Arrow size as a fraction of HandleSize, so arrows stay proportional to the waypoint handles. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Direction", meta = (ClampMin = "0.01"))
	float ArrowScale = 0.5f;

	/** Mesh used for each direction arrow. Must point along its local +Z (engine cone does). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Direction")
	TObjectPtr<UStaticMesh> ArrowMesh;

	/** Material applied to each direction arrow. Falls back to a runtime-generated MID if null. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Direction")
	TObjectPtr<UMaterialInterface> ArrowMaterial;

	/** Extra rotation applied to each direction arrow on top of its spline-tangent alignment. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Appearance|Direction")
	FRotator ArrowRotationOffset = FRotator(0.0f, 0.0f, 0.0f);

	/** Extra world-space offset applied to each direction arrow's computed spline position. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Appearance|Direction")
	FVector ArrowLocationOffset = FVector(0.0f, 0.0f, 0.0f);

	/** Colour of arrows on segments within the "start -> selected waypoint" gradient span (start endpoint). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Appearance|Direction")
	FLinearColor ArrowGradientStartColor = FLinearColor(0.0f, 1.0f, 1.0f, 1.0f);

	/** Colour of arrows at the segment reaching the currently selected waypoint. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Appearance|Direction")
	FLinearColor ArrowGradientEndColor = FLinearColor(1.0f, 0.5f, 0.0f, 1.0f);

	/** Colour of arrows on segments beyond the selected waypoint (and of all arrows when nothing is selected). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Appearance|Direction")
	FLinearColor ArrowDefaultColor = FLinearColor::White;

	/** Toggle direction arrows at runtime and rebuild them. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Direction")
	void SetShowDirectionArrows(bool bShow);

	/** Change arrow spacing at runtime and rebuild the arrows. Clamped to >= 50 units. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Direction")
	void SetArrowSpacing(float NewSpacing);

	// ---------------------------------------------------------------------------------------------
	// Selection gradient
	// ---------------------------------------------------------------------------------------------

	/** Colour of the route start (control point 0) in the "start -> selected waypoint" gradient. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Gradient")
	FLinearColor GradientStartColor = FLinearColor(0.0f, 0.9f, 1.0f, 1.0f);

	/** Colour reached at the currently selected waypoint in the gradient. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Gradient")
	FLinearColor GradientEndColor = FLinearColor(1.0f, 0.25f, 0.03f, 1.0f);

	/** Colour of segments beyond the selected waypoint (and of the whole route when nothing is selected). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Gradient")
	FLinearColor SegmentDefaultColor = FLinearColor(0.75f, 0.75f, 0.75f, 1.0f);

	/**
	 * Seconds between each step of the start ("A") -> selected waypoint "chase light" reveal. Every
	 * arrow/segment in the span ends up the same GradientEndColor / ArrowGradientEndColor — this only
	 * controls how fast the reveal travels from one to the next.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Gradient", meta = (ClampMin = "0.01"))
	float ChaseStepDelay = 0.06f;

	/** Add a control point at WorldLocation. Returns the new point index. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route")
	int32 AddControlPoint(FVector WorldLocation);

	/** Move an existing control point in world space. No-op if PointIndex is invalid. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route")
	void MoveControlPoint(int32 PointIndex, FVector NewWorldLocation);

	/** Remove a control point. No-op if PointIndex is invalid. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route")
	void RemoveControlPoint(int32 PointIndex);

	/** Remove every control point. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route")
	void ClearControlPoints();

	/** Returns the world-space location of every control point's handle mesh. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route")
	int32 GetNumControlPoints() const;

	/** Resolve a hit primitive to a control-point index on this route. Returns INDEX_NONE on miss. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route")
	int32 FindControlPointForComponent(const UPrimitiveComponent* HitComponent) const;

	/** Mark a point as visually selected. Pass INDEX_NONE to clear selection. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route")
	void SetSelectedControlPoint(int32 PointIndex);

	/** Currently selected control point index, or INDEX_NONE. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route")
	int32 GetSelectedControlPoint() const { return SelectedIndex; }

	/** Get the world transform of a point along the spline at the given distance. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route")
	FTransform GetTransformAtDistance(float Distance) const;

	/** Total spline length in world units. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route")
	float GetTotalLength() const;

	/** Direct access to the underlying spline. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route")
	USplineComponent* GetSpline() const { return Spline; }

	/**
	 * Override the cruise speed AT the given control point. NewSpeed < 0 clears the override
	 * (the point falls back to the follower's base Speed). Speeds are interpolated between
	 * adjacent control points by GetSpeedAtDistance for a smooth per-point profile.
	 */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Speed")
	void SetPointSpeedOverride(int32 PointIndex, float NewSpeed);

	/** Returns the speed override at the given control point, or -1.f if unset / index invalid. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Speed")
	float GetPointSpeedOverride(int32 PointIndex) const;

	/**
	 * Effective speed at a distance along the spline: linear interpolation between the effective
	 * speeds of the two control points bounding that distance (each point uses its override, or
	 * BaseSpeed when unset). Produces a smooth speed ramp instead of per-segment step changes.
	 */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Speed")
	float GetSpeedAtDistance(float Distance, float BaseSpeed) const;

	/** Number of segments (Spline points - 1, or NumPoints if closed). */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Speed")
	int32 GetNumSegments() const;

	/**
	 * Mark a control point as a REVERSE node. The segment leaving a reverse node is traversed
	 * stern-first — the vessel keeps progressing along the route but faces backwards — to simulate
	 * docking, K-turns, and shipyard parking. No-op on invalid index.
	 */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Reverse")
	void SetPointReverse(int32 PointIndex, bool bReverse);

	/** Whether the given control point is flagged as a reverse node. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Reverse")
	bool GetPointReverse(int32 PointIndex) const;

	/** True if the segment containing Distance leaves a reverse-flagged control point. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Reverse")
	bool IsReverseAtDistance(float Distance) const;

	/** Re-spawn segment/arrow meshes and re-apply materials. Call after changing SegmentMaterial/ArrowMaterial at runtime. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Appearance")
	void RebuildGeometry();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	/** Per-control-point speed overrides. Sentinel -1.f means "use follower base speed". Sized to NumControlPoints. */
	UPROPERTY(Transient)
	TArray<float> PointSpeedOverrides;

	/** Per-control-point reverse flags. True => the segment leaving this point is driven stern-first. Sized to NumControlPoints. */
	UPROPERTY(Transient)
	TArray<bool> PointReverseFlags;

	/** Resize the per-point arrays to the current control-point count (speed padded -1.f, reverse padded false). */
	void ResizePerPointArrays();

	/** Per-control-point visual handles. Index matches the spline point index. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> HandleMeshes;

	/** Per-segment spline mesh components that visualise the curve at runtime. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<USplineMeshComponent>> SegmentMeshes;

	/** One MID per entry in SegmentMeshes, so each segment can carry its own gradient colour. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> SegmentMIDs;

	/** Direction-arrow meshes distributed along the spline. Rebuilt whenever the geometry changes. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> DirectionArrows;

	/** One MID per entry in DirectionArrows, so each arrow can carry its own gradient colour. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> ArrowMIDs;

	/** Spline distance each DirectionArrows entry was placed at — used to map arrows back to a segment for gradient tinting. */
	UPROPERTY(Transient)
	TArray<float> ArrowDistances;

	/** Runtime fallback materials, created in BeginPlay from WorldGridMaterial when explicit assets are not set. */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> FallbackHandleMID;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> FallbackSelectedHandleMID;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> FallbackReverseHandleMID;

	/** Tint for normal (unselected, non-reverse) handles on a Ground-vehicle route. */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> FallbackGroundHandleMID;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> FallbackSegmentMID;

	/** Tint for direction arrows when no explicit ArrowMaterial is assigned. */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> FallbackArrowMID;

	/** Guards EnsureFallbackMaterials against re-running its LoadObject on every geometry rebuild. */
	UPROPERTY(Transient)
	bool bFallbackMaterialsReady = false;

	/** Index of the currently selected control point, or INDEX_NONE. */
	UPROPERTY(Transient)
	int32 SelectedIndex = INDEX_NONE;

	/**
	 * Control point up to which the route is CONFIRMED fully lit (GradientEndColor/ArrowGradientEndColor),
	 * or INDEX_NONE. Updated incrementally as the chase reveal progresses (see ApplySegmentGradient) so an
	 * interrupted reveal always resumes from wherever it actually got to, and reset by
	 * ResyncSegmentGradientInstant whenever geometry rebuilds invalidate the MIDs it refers to.
	 */
	UPROPERTY(Transient)
	int32 LastRevealedIndex = INDEX_NONE;

	/** Drives the A -> Selected "chase light" reveal started by ApplySegmentGradient. */
	FTimerHandle ChaseRevealTimerHandle;

	/**
	 * If a chase reveal was interrupted mid-group, this is the segment-group index (a control point's
	 * segment mesh + every arrow on it, treated as one atomic unit for confirmation purposes even though
	 * they animate across several timer ticks) that was only partially painted toward its target colour
	 * when that happened. ApplySegmentGradient folds this back into whichever range it's about to
	 * (re)process next, in EITHER direction, so an interrupted chase never leaves a segment's mesh and
	 * its arrows disagreeing about whether they're lit. INDEX_NONE when idle or at a clean boundary.
	 * Reset by ResyncSegmentGradientInstant.
	 */
	UPROPERTY(Transient)
	int32 PendingChaseGroupIndex = INDEX_NONE;

	void RebuildHandleMeshes();
	void RebuildSegmentMeshes();

	/** Destroy and respawn the direction arrows for the current spline geometry + reverse flags. */
	void RebuildDirectionArrows();

	/**
	 * Animated "chase light" reveal: extends or retracts from LastRevealedIndex to SelectedIndex.
	 * Call ONLY in response to an actual selection change (SetSelectedControlPoint) — geometry rebuilds
	 * must use ResyncSegmentGradientInstant instead, since they invalidate LastRevealedIndex's MIDs.
	 */
	void ApplySegmentGradient();

	/**
	 * Instantly (no animation) paints every segment/arrow MID to match the current SelectedIndex —
	 * GradientEndColor/ArrowGradientEndColor up to it, default beyond — and resets LastRevealedIndex to
	 * match. Call after any geometry rebuild (new/moved/removed points, reverse toggle, handle size,
	 * arrow spacing) since those destroy and recreate SegmentMIDs/ArrowMIDs from scratch.
	 */
	void ResyncSegmentGradientInstant();

	void ApplyHandleMaterial(int32 PointIndex, bool bSelected);
	UStaticMeshComponent* CreateHandleAt(int32 PointIndex, const FVector& WorldLocation);
	/**
	 * Bias a control point's raw (on-plane) world location upward so the handle mesh's actual lowest
	 * vertex lands on the plane, instead of its pivot. Reads HandleMesh's real local bounding box
	 * (custom waypoint meshes are neither centered nor a fixed size, unlike the engine cube) rather
	 * than assuming a centered 100-unit cube. AppliedWorldScale is the exact uniform scale about to be
	 * (or already) passed to SetWorldScale3D for this handle, so a selected (bumped) handle stays
	 * bottom-anchored too.
	 */
	FVector GetBiasedHandleLocation(const FVector& SplinePointWorldLocation, float AppliedWorldScale) const;
	void EnsureFallbackMaterials();
	UMaterialInterface* GetEffectiveHandleMaterial(bool bSelected) const;
	/** Material for a handle accounting for selection AND its reverse flag (selected wins over reverse). */
	UMaterialInterface* GetEffectiveHandleMaterialForPoint(int32 PointIndex, bool bSelected) const;
	UMaterialInterface* GetEffectiveSegmentMaterial() const;
	UMaterialInterface* GetEffectiveArrowMaterial() const;
	/** Index of the segment (control point i -> i+1) that contains Distance, or INDEX_NONE if out of range. */
	int32 GetSegmentIndexAtDistance(float Distance) const;
};
