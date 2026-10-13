// Copyright DamenTools internal. Spline Route Tool plugin.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "SplineRouteActor.h" // ERouteVehicleType used in UFUNCTION signatures
#include "SplineRouteAuthoringSubsystem.generated.h"

class ASplineRouteActor;
class APlayerController;
class UVesselCatalog;
class ASplineFollowerVessel;
class USplineFollowerComponent;
class ASplineRouteObstruction;
class UUserWidget;
class UWidget;

/** Fires when the active route reference changes. Useful for the HUD to re-bind to selection events. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnActiveRouteChanged, ASplineRouteActor*, NewRoute);

/**
 * Per-world authoring state for the runtime spline editor.
 *
 * - Tracks the "active" route (where new clicks go).
 * - Tracks the currently selected control point.
 * - Performs mouse-to-world tracing onto a configurable horizontal plane.
 * - Implements add / move / delete primitives invoked by the player controller.
 *
 * Designed to be reachable from any actor or widget via GetWorld()->GetSubsystem<...>().
 */
UCLASS(config=Game)
class SPLINEROUTETOOL_API USplineRouteAuthoringSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Z value of the authoring plane (world space). */
	UPROPERTY(config, EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Authoring")
	float PlaneZ = 50.0f;

	/**
	 * Screen-space radius (pixels) around the cursor used to pick a waypoint handle for selection / drag.
	 * Generous on purpose — the old hit-test relied on the tiny handle mesh, which made clicking a point
	 * to select it land on empty water instead. Bump this up if operators still report missed selections.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Authoring", meta = (ClampMin = "4.0"))
	float HandleSelectRadiusPx = 60.0f;

	/** Class spawned when no active route exists and the user clicks. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Authoring")
	TSubclassOf<ASplineRouteActor> DefaultRouteClass;

	/** Catalog of pre-made vessels for the runtime picker. If null at first use, the subsystem tries to load /Game/SplineRouteTool/Data/DA_VesselCatalog. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Authoring")
	TObjectPtr<UVesselCatalog> Catalog;

	/** Currently selected index in Catalog->Entries. */
	UPROPERTY(BlueprintReadWrite, Category = "Spline Route|Authoring")
	int32 SelectedVesselIndex = 0;

	/** Uniform multiplier applied to every vessel's catalog scale. Re-applies to existing vessels via SetVesselSizeMultiplier. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Authoring", meta = (ClampMin = "0.05"))
	float VesselSizeMultiplier = 1.0f;

	/** Turn rate (deg/sec) applied to every vessel. New vessels adopt this; SetTurnRateForAllVessels re-applies to live ones. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Authoring", meta = (ClampMin = "0.0"))
	float VesselTurnRate = 90.0f;

	/** Minimum spline distance the last-spawned vessel must have travelled before another spawn is allowed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Authoring", meta = (ClampMin = "0.0"))
	float MinSpawnGapMeters = 5000.0f;

	/** Minimum spline-distance gap each vessel's follower keeps from its leader. Scales with VesselSizeMultiplier. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Authoring", meta = (ClampMin = "0.0"))
	float MinFollowDistanceMeters = 5000.0f;

	/** Multiplier applied on top of a vessel's own current scale while selected (see SetSelectedVessel).
	 *  1.4 (the waypoint handle's own SelectedHandleScaleBump) reads as an absurd jump on a full-size ship
	 *  mesh, so this defaults smaller than that convention — tune here if the highlight still looks wrong. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Authoring", meta = (ClampMin = "1.0"))
	float VesselSelectionHighlightScale = 1.15f;

	/** Broadcast whenever SetActiveRoute / GetOrSpawnActiveRoute changes the active route reference. */
	UPROPERTY(BlueprintAssignable, Category = "Spline Route|Authoring")
	FOnActiveRouteChanged OnActiveRouteChanged;

	/** Lazily resolves the catalog if it's null. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Authoring")
	UVesselCatalog* GetOrLoadCatalog();

	/**
	 * Spawn a vessel from Catalog->Entries[VesselIndex] and attach its follower to the active route.
	 * Returns AActor, not ASplineFollowerVessel — VesselClass may resolve to either ASplineFollowerVessel
	 * or a plain Blueprint carrying its own USplineFollowerComponent (e.g. BP_Ship_Cargo_2/3/4/6).
	 */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Authoring")
	AActor* SpawnVesselOnActiveRoute(int32 VesselIndex);

	/** Update VesselSizeMultiplier and re-apply scale to every live ASplineFollowerVessel in the world. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Authoring")
	void SetVesselSizeMultiplier(float NewMultiplier);

	/** Update VesselTurnRate and re-apply it to every live vessel's follower. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Authoring")
	void SetTurnRateForAllVessels(float NewTurnRate);

	/** Update MinSpawnGapMeters, the minimum spline distance required between consecutive vessel spawns. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Authoring")
	void SetMinSpawnGap(float NewGap) { MinSpawnGapMeters = FMath::Max(0.0f, NewGap); }

	/** Apply NewSpeed to the currently selected control point. No-op if nothing selected or no active route. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Authoring")
	void SetSpeedForSelectedSegment(float NewSpeed);

	/** Returns the speed override on the selected point, or -1.f if no override / nothing selected. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Authoring")
	float GetSpeedForSelectedSegment() const;

	/** Flag/unflag the selected control point as a reverse node. No-op if nothing selected. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Authoring")
	void SetReverseForSelectedPoint(bool bReverse);

	/** Flip the reverse flag on the selected control point. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Authoring")
	void ToggleReverseForSelectedPoint();

	/** Whether the selected control point is a reverse node (false if nothing selected). */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Authoring")
	bool GetReverseForSelectedPoint() const;

	// --- Yard obstruction placement ----------------------------------------------------------

	/** When true, the place action drops an obstruction at the cursor instead of a route waypoint. */
	UPROPERTY(BlueprintReadWrite, Category = "Spline Route|Obstruction")
	bool bObstructionPlaceMode = false;

	/** Selected obstruction catalog index used when placing. */
	UPROPERTY(BlueprintReadWrite, Category = "Spline Route|Obstruction")
	int32 SelectedObstructionIndex = 0;

	/** Toggle obstruction placement mode on/off. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Obstruction")
	void SetObstructionPlaceMode(bool bEnabled);

	UFUNCTION(BlueprintCallable, Category = "Spline Route|Obstruction")
	bool IsObstructionPlaceMode() const { return bObstructionPlaceMode; }

	/** Choose which obstruction type is placed. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Obstruction")
	void SetSelectedObstructionIndex(int32 Index);

	/** Spawn the selected obstruction at the cursor's plane intersection. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Obstruction")
	ASplineRouteObstruction* SpawnObstructionAtCursor(APlayerController* PC);

	/** Number of obstruction catalog entries (at least 1 — a default Container is offered when empty). */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Obstruction")
	int32 GetNumObstructionTypes() const;

	/** Display label for an obstruction catalog index. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Obstruction")
	FText GetObstructionLabel(int32 Index) const;

	// --- Ground vehicle / route medium -------------------------------------------------------

	/** Set the active route's vehicle medium (Vessel/Ground). Ensures a route exists first. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Authoring")
	void SetActiveRouteVehicleType(ERouteVehicleType NewType);

	/** Flip the active route between Vessel (water) and Ground (land). Drives the HUD toggle. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Authoring")
	void ToggleActiveRouteVehicleType();

	/** The active route's vehicle type (Vessel if no active route). */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Authoring")
	ERouteVehicleType GetActiveRouteVehicleType() const;

	// --- Multi-vehicle playback orchestrator -------------------------------------------------

	/** Resume all motion at the last non-zero play rate (or 1.0 if never set). */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Playback")
	void PlayAll();

	/** Freeze all motion (global time scale -> 0), remembering the current rate so PlayAll can resume it. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Playback")
	void PauseAll();

	/** Pause, then rewind every follower in the world to the start of its route. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Playback")
	void StopAll();

	// --- Scenario save / load ----------------------------------------------------------------

	/** Default save slot used by the HUD Save/Load buttons. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Scenario")
	FString ScenarioSlotName = TEXT("SplineRouteScenario");

	/** Capture all routes, vehicles, obstructions, and playback settings into a save slot. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Scenario")
	bool SaveScenario(const FString& SlotName);

	/** Clear the world and rebuild routes/vehicles/obstructions from a save slot. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Scenario")
	bool LoadScenario(const FString& SlotName);

	/** Destroy all tool-authored routes, vehicles, and obstructions in the world. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Scenario")
	void ClearScenario();

	/**
	 * Destroy every live vessel actor (either kind — ASplineFollowerVessel or a generic
	 * BP_Ship_Cargo_*-style actor carrying its own USplineFollowerComponent), clearing selection
	 * and spawn-gate state. Leaves routes/waypoints and obstructions untouched.
	 */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Scenario")
	void ClearAllVessels();

	/**
	 * Clear every control point on every route in the world (ClearControlPoints per route), leaving
	 * the route actors themselves — and any vessels following them — intact. Leaves vessels and
	 * obstructions untouched.
	 */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Scenario")
	void ClearAllWaypoints();

	/**
	 * The HUD calls this in NativeConstruct so the place-input handler knows what screen rect to ignore.
	 * Without this, Enhanced Input fires IA_Place even when the click lands on the HUD widget.
	 * Pass the INNER panel widget (e.g. the dark Border), not the root canvas, otherwise the entire
	 * viewport will be considered "over HUD".
	 */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Authoring")
	void RegisterHUDPanel(UWidget* InHUDPanel);

	/** True if the cursor is currently inside the registered HUD panel's screen rect. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Authoring")
	bool IsCursorOverHUD(APlayerController* PC) const;

	/** Returns the currently active route. May be null until first click. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Authoring")
	ASplineRouteActor* GetActiveRoute() const { return ActiveRoute; }

	/**
	 * Most recently spawned vessel, or null. Only used by the spawn gate now — camera cycling targets
	 * GetSelectedVessel() instead. AActor, not ASplineFollowerVessel, since the spawn gate now tracks
	 * either kind (see LastSpawnedFollower for the actual gate/convoy data).
	 */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Authoring")
	AActor* GetLastSpawnedVessel() const { return LastSpawnedVessel.Get(); }

	/**
	 * The vessel last clicked via HandleSelectAtCursor, or null if none has been clicked (or the selected
	 * one was destroyed), narrowed to the ASplineFollowerVessel kind. Drives the camera-cycle input ("C"),
	 * which is itself ASplineFollowerVessel-specific (ActivateCameraSlot) — see GetSelectedVesselActor()
	 * below for the kind-agnostic selection (highlight/delete) that also covers BP_Ship_Cargo_2/3/4/6.
	 */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Authoring")
	ASplineFollowerVessel* GetSelectedVessel() const;

	/**
	 * The vessel actor last clicked via HandleSelectAtCursor, of EITHER kind — an ASplineFollowerVessel or
	 * a plain Blueprint carrying its own USplineFollowerComponent (e.g. BP_Ship_Cargo_2/3/4/6). Null if
	 * none has been clicked, or the selected one was destroyed. Backs the selection scale-highlight and
	 * Delete-key removal, both of which are generic across vessel kind.
	 */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Authoring")
	AActor* GetSelectedVesselActor() const { return SelectedVessel.Get(); }

	/** Force a specific route to be the active one (e.g. for the demo widget). */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Authoring")
	void SetActiveRoute(ASplineRouteActor* NewRoute);

	/**
	 * Return the active route, claiming a level-placed ASplineRouteActor if one exists,
	 * or spawning a new one from DefaultRouteClass if not. Mirrors what HandlePlaceAtCursor
	 * does on the first click — but callable from UI so the HUD slider can drive a route
	 * before the user has clicked to place a point.
	 */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Authoring")
	ASplineRouteActor* EnsureActiveRoute();

	/**
	 * Handle a "place / select" press from the player controller.
	 * - If the cursor is over an existing control point, that point becomes selected and is the drag target.
	 * - Otherwise a new control point is appended to the active route at the plane intersection.
	 */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Authoring")
	void HandlePlaceOrSelect(APlayerController* PC);

	/**
	 * Always place a new control point at the cursor's plane intersection.
	 * Used by the "F key places" workflow. Sets the new point as both selected and drag target,
	 * so the user can immediately move it with a held mouse drag if desired.
	 */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Authoring")
	void HandlePlaceAtCursor(APlayerController* PC);

	/**
	 * Select the waypoint nearest the cursor (within HandleSelectRadiusPx), if any; otherwise no-op.
	 * Selection ONLY — it does not arm a drag. Moving a point now requires holding the Spacebar
	 * modifier while dragging (see HandleDrag), which is what fixes "clicking to select moved my point".
	 */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Authoring")
	void HandleSelectAtCursor(APlayerController* PC);

	/** Select the next waypoint on the active route (wraps). Drives the HUD's cycle button. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Authoring")
	void SelectNextPoint();

	/** Select the previous waypoint on the active route (wraps). Drives the HUD's cycle button. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Authoring")
	void SelectPrevPoint();

	/** Update the dragged point (if any) to the plane intersection under the cursor. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Authoring")
	void HandleDrag(APlayerController* PC);

	/** End an in-progress drag (if any). */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Authoring")
	void HandleReleaseDrag();

	/** Delete the currently selected control point (if any). */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Authoring")
	void HandleDeleteSelected();

	/** Project the mouse cursor onto the authoring plane (Z = PlaneZ). Returns false if PC is null. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Authoring")
	bool ProjectMouseToPlane(APlayerController* PC, FVector& OutWorldLocation) const;

	/**
	 * True if a control point may be placed (or dragged to) WorldLocation, based on the
	 * level's ASplineRouteZone actors. See ASplineRouteZone for the policy.
	 */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Authoring")
	bool CanPlaceAt(const FVector& WorldLocation) const;

private:
	UPROPERTY(Transient)
	TObjectPtr<ASplineRouteActor> ActiveRoute;

	UPROPERTY(Transient)
	TObjectPtr<UWidget> HUDPanel;

	UPROPERTY(Transient)
	int32 DraggedPointIndex = INDEX_NONE;

	/** Most recently spawned vessel actor — checked by the spawn gate against MinSpawnGapMeters. Either
	 *  kind (ASplineFollowerVessel or a generic BP_Ship_Cargo_*-style actor); see LastSpawnedFollower
	 *  for the follower component itself, which is what the gate/convoy logic actually reads. */
	TWeakObjectPtr<AActor> LastSpawnedVessel;

	/** The most recently spawned vessel's USplineFollowerComponent, found generically via
	 *  FindComponentByClass so the spawn gate and convoy chaining work identically for both vessel kinds. */
	TWeakObjectPtr<USplineFollowerComponent> LastSpawnedFollower;

	/** Vessel last clicked via HandleSelectAtCursor, either kind. Independent of LastSpawnedVessel — set by
	 *  clicking, not by spawning. Widened to AActor (from ASplineFollowerVessel) so the selection highlight
	 *  and Delete-key removal work uniformly for the generic BP_Ship_Cargo_*-style kind too; see
	 *  GetSelectedVessel() for the narrowed accessor camera-cycling still relies on. */
	TWeakObjectPtr<AActor> SelectedVessel;

	/** SelectedVessel's actual world scale immediately before the selection highlight was applied to it —
	 *  captured fresh on every new selection (never a fixed "base" scale) so restoring it on deselect is
	 *  exact even if SetSizeMultiplier() changed the vessel's scale independently while selected. Only
	 *  meaningful while SelectedVessel is valid. */
	FVector PreHighlightVesselScale = FVector::OneVector;

	/** Time scale to restore on PlayAll after a PauseAll/StopAll. */
	float LastPlayRate = 1.0f;

	bool IsDragging() const { return DraggedPointIndex != INDEX_NONE; }

	/**
	 * Find the waypoint whose handle projects closest to the cursor, within RadiusPx pixels.
	 * Screen-space on purpose: a world line-trace is occluded by large background meshes (e.g. the
	 * cadastral plane) even when the handle is plainly visible. Returns false on miss.
	 */
	bool FindClosestControlPointOnScreen(APlayerController* PC, float RadiusPx, ASplineRouteActor*& OutRoute, int32& OutIndex) const;

	/** Shared cycle implementation for SelectNextPoint / SelectPrevPoint. Delta is +1 / -1. */
	void CycleSelection(int32 Delta);

	/**
	 * Centralizes vessel-selection bookkeeping so it's identical from every call site (click-select,
	 * waypoint-select clearing it, and delete). If a different vessel was previously selected (and still
	 * exists), restores its exact PreHighlightVesselScale first. If NewVessel is non-null, captures its
	 * current scale into PreHighlightVesselScale BEFORE scaling it up, so the eventual restore is exact.
	 * Pass nullptr to deselect with no new selection.
	 */
	void SetSelectedVessel(AActor* NewVessel);

	ASplineRouteActor* GetOrSpawnActiveRoute();

	/** Internal — assigns ActiveRoute and fires OnActiveRouteChanged when the value changes. */
	void AssignActiveRoute(ASplineRouteActor* NewRoute);
};
