// Copyright DamenTools internal. Precise Transform plugin.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "PreciseTransformLibrary.generated.h"

class UDynamicDataTable;
class UDataTable;
class UStaticMesh;

/**
 * P1 Item 4 (Precise Transform Input) for the Warehouse Planner: lets the
 * user type exact X/Y/Z coordinates instead of dragging the mouse to place
 * an object.
 *
 * BP_BuildManager and the active object placer (BP_Damen_Placer /
 * BP_ObjectPlacer_Base_Class and siblings) are Blueprint-only classes with
 * no native base - same constraint documented on SpaceCalculatorLibrary and
 * CameraPresetComponent::ComputeGridFraming. Reading/writing their state and
 * calling their functions therefore goes through reflection
 * (FindFProperty / ProcessEvent) rather than direct member access.
 *
 * Design note on WHY this calls BuildManagerRef's own GetCellFromWorldLocation
 * / GetCellCenter functions via reflection instead of reimplementing the
 * cell<->world math here: that math was reverse-engineered from
 * BP_BuildManager's node graph while designing this plugin and does not
 * simply add BuildManagerRef's own actor location as a grid origin (unlike
 * CameraPresets::ComputeGridFraming's camera-framing approximation, which
 * only needs to be roughly centered, not exact). Re-deriving it independently
 * in C++ risked a subtle, hard-to-notice mismatch between "where the user
 * typed" and "where the object actually lands." Calling the Blueprint's own
 * functions guarantees this stays correct even if that internal math changes.
 */
UCLASS()
class PRECISETRANSFORM_API UPreciseTransformLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Parses three user-typed strings into a world-space FVector (centimeters).
	 * Unlike Blueprint's native string-to-float conversion (which silently
	 * defaults to 0 on bad input), this fails loudly: returns false and logs
	 * exactly which field(s) failed to parse, doesn't guess a "close enough"
	 * value for a malformed field.
	 *
	 * @param XStr, YStr, ZStr  Raw text from the three input fields.
	 * @param OutLocation       Parsed (X, Y, Z) in centimeters if successful; zeroed otherwise.
	 * @return true if all three fields parsed as valid numbers.
	 */
	UFUNCTION(BlueprintCallable, Category = "PreciseTransform")
	static bool ParseCoordinateInput(const FString& XStr, const FString& YStr, const FString& ZStr, FVector& OutLocation);

	/**
	 * Inverse of ParseCoordinateInput: formats a world-space FVector into
	 * three separate strings (one per axis). Output is round-trip compatible
	 * with ParseCoordinateInput - feeding OutXStr/OutYStr/OutZStr back into
	 * it will reproduce the original vector exactly.
	 *
	 * Used by WB_ObjectDescription to populate the three coordinate display
	 * fields when the user selects an already-placed object.
	 *
	 * @param Location           World-space location to format (centimeters).
	 * @param OutXStr, OutYStr, OutZStr  Formatted strings for each axis.
	 */
	UFUNCTION(BlueprintPure, Category = "PreciseTransform")
	static void FormatLocationAsStrings(FVector Location, FString& OutXStr, FString& OutYStr, FString& OutZStr);

	/**
	 * Validates a typed world-space location against BuildManagerRef's grid
	 * and snaps its X/Y to the nearest cell. Z passes through unmodified -
	 * height is the user's explicit call (this feature is for manual, precise
	 * placement; it deliberately does not override Z with a ground trace the
	 * way normal mouse-driven placement does via BP_BuildManager::GetCellLocation).
	 *
	 * Expects EXACTLY these UFUNCTIONs on BuildManagerRef's class (confirmed
	 * against BP_BuildManager as of this writing):
	 *   - GetCellFromWorldLocation(FVector Location) -> FIntPoint Cell
	 *   - GetCellCenter(FIntPoint Cell) -> FVector2D Location
	 *   - CheckCellInBuilableBoundary(FIntPoint Cell) -> bool Success
	 * Cells are signed world-space cells (no grid origin), so validity is the
	 * Build Tool's own boundary check, never 0..GridSize.
	 * If BP_BuildManager is ever refactored and any of these three go
	 * missing/renamed, this fails loudly (returns false, logs which one)
	 * rather than silently snapping to a wrong or garbage location.
	 *
	 * @param BuildManagerRef       A BP_BuildManager_C instance.
	 * @param TypedLocation         World-space location to validate/snap (from ParseCoordinateInput).
	 * @param OutCell               The grid cell TypedLocation's X/Y resolved to.
	 * @param OutSnappedWorldLocation  World-space location snapped to that cell's center, Z passed through.
	 * @return true if TypedLocation's cell passes CheckCellInBuilableBoundary.
	 */
	UFUNCTION(BlueprintCallable, Category = "PreciseTransform")
	static bool ValidateAndSnapToGrid(UObject* BuildManagerRef, FVector TypedLocation, FIntPoint& OutCell, FVector& OutSnappedWorldLocation);

	/**
	 * The main entry point: validates+snaps TypedLocation, then commits
	 * placement exactly as a real mouse click there would - by setting
	 * BuildManagerRef.CellUnderCursor to the resolved cell and immediately
	 * calling ActivePlacer's own BuildObject(), synchronously, in the same
	 * call. This ordering matters: CellUnderCursor is normally recomputed
	 * from live mouse position every Tick (BP_BuildManager::UpdateBuildingManagerValues),
	 * so setting it and returning would just get overwritten next frame.
	 * Calling BuildObject() immediately, before any Tick can run, avoids that -
	 * and BuildObject() already force-refreshes its own placement-validity
	 * state (ForceUpdatePlaceIndicators + DrawPlacementIndicators) as its
	 * first step, so it validates against the NEW cell, not a stale one.
	 *
	 * Expects a public, no-parameter UFUNCTION BuildObject() on ActivePlacer's
	 * class (confirmed against BP_Damen_Placer as of this writing) and a
	 * CellUnderCursor : FIntPoint UPROPERTY on BuildManagerRef's class.
	 *
	 * @param BuildManagerRef     A BP_BuildManager_C instance.
	 * @param ActivePlacer        The currently-active object placer actor (BuildManagerRef.ActiveObjectPlacer).
	 * @param TypedLocation       World-space location to place at (from ParseCoordinateInput).
	 * @param OutAppliedLocation  The actual snapped location placement was attempted at.
	 * @return true if the location was valid and BuildObject() was called. Does NOT guarantee
	 *         the placement itself succeeded (e.g. the cell may be occupied) - BuildObject()'s
	 *         own existing CanBuild logic governs that, exactly as it does for a mouse click.
	 */
	UFUNCTION(BlueprintCallable, Category = "PreciseTransform")
	static bool ApplyPreciseLocation(UObject* BuildManagerRef, AActor* ActivePlacer, FVector TypedLocation, FVector& OutAppliedLocation);

	/**
	 * Convenience wrapper combining ParseCoordinateInput + ApplyPreciseLocation
	 * into a single call, so the three input fields' OnTextCommitted handlers
	 * need only this one node. See both functions above for behavior/failure modes.
	 */
	UFUNCTION(BlueprintCallable, Category = "PreciseTransform")
	static bool ApplyPreciseLocationFromStrings(UObject* BuildManagerRef, AActor* ActivePlacer, const FString& XStr, const FString& YStr, const FString& ZStr, FVector& OutAppliedLocation);

	/**
	 * Moves an ALREADY-PLACED object to a new typed location, re-registering
	 * its grid occupancy. Unlike ApplyPreciseLocation (which places a NEW
	 * object still queued in ActivePlacer), this operates on a real,
	 * already-spawned BP_PlaceableObject_Base_Class_C instance already
	 * registered in BuildManagerRef's OccupancyData/ObjectsData maps.
	 *
	 * Mirrors the exact release/register sequence BP_PlaceableObject_Base_Class
	 * itself uses internally (DestroyPlaceableObject's release loop /
	 * SetupPlaceableObject's register loop - both traced directly from their
	 * node graphs, not assumed) via the same BuildManagerRef UFUNCTIONs, so
	 * grid occupancy state stays byte-for-byte consistent with normal
	 * placement/removal - nothing here is reimplemented independently.
	 *
	 * Expects EXACTLY these UPROPERTYs (confirmed against BP_BuildManager /
	 * BP_PlaceableObject_Base_Class as of this writing):
	 *   BuildManagerRef : OccupancyData (TMap<IntPoint, int32>)
	 *   PlacedObject    : ObjectSize (IntPoint), OccupiedCells (array<IntPoint>),
	 *                     OccupiedCenterCell (IntPoint)
	 * and these UFUNCTIONs on BuildManagerRef's class:
	 *   GetCellsInRectangularArea(CenterAreaCell, AreaSize) -> Cells
	 *   GetCenterOfRectangularArea(AreaCenterCell, AreaSize) -> Center
	 *   SetOccupancyData(Cell, Occupied)
	 *   SetObjectData(Cell, PlacealeObject)   [sic - matches BP_BuildManager's actual, misspelled param name]
	 *   RemoveObjectData(Cell)
	 *   CheckCellInBuilableBoundary(Cell) -> Success
	 * If any of these is renamed/retyped/removed, this fails loudly (returns
	 * false, logs which one) rather than silently corrupting grid state.
	 *
	 * Validates the ENTIRE new footprint (buildable boundary, then no conflicting
	 * occupancy from another object - a cell the object already occupies
	 * itself is fine) BEFORE mutating anything. A rejected move leaves both
	 * the object and the grid's occupancy maps completely untouched.
	 *
	 * @param BuildManagerRef     A BP_BuildManager_C instance.
	 * @param PlacedObject        The already-placed object to move (a BP_PlaceableObject_Base_Class_C instance).
	 * @param TypedLocation       World-space location to move to (from ParseCoordinateInput). Z passes through unmodified, same design choice as ValidateAndSnapToGrid.
	 * @param OutAppliedLocation  The actual location the object was moved to (its new footprint's true center, not just the typed cell's center - correct for multi-cell objects).
	 * @return true if the move was valid and applied. False (object and grid state untouched) if the target footprint falls outside the buildable boundary or overlaps another object's occupied cells.
	 */
	UFUNCTION(BlueprintCallable, Category = "PreciseTransform")
	static bool RepositionPlacedObject(UObject* BuildManagerRef, AActor* PlacedObject, FVector TypedLocation, FVector& OutAppliedLocation);

	/**
	 * Convenience wrapper combining ParseCoordinateInput + RepositionPlacedObject
	 * into a single call, so the three input fields' OnTextCommitted handlers
	 * need only this one node for the already-placed case. See both functions
	 * above for behavior/failure modes.
	 */
	UFUNCTION(BlueprintCallable, Category = "PreciseTransform")
	static bool RepositionPlacedObjectFromStrings(UObject* BuildManagerRef, AActor* PlacedObject, const FString& XStr, const FString& YStr, const FString& ZStr, FVector& OutAppliedLocation);

	/**
	 * Buildable-area boundary, computed automatically instead of hand-typed.
	 *
	 * The boundary previously enabled on BP_BuildManager (BuildableBoundaryEnabled
	 * + BuilableBoundaryMin/Max) was set once by hand from a single "Floor" actor's
	 * collision box. Since then more actors (crane/berth/dock surfaces) were tagged
	 * "Floor" too (see SM_Crane_Berth/Dock_* tagging), so that fixed box no longer
	 * covers every valid placement surface — it was left disabled rather than
	 * rejecting placement on those newer surfaces. This function replaces the
	 * hand-typed box with the combined world-space AABB of every actor currently
	 * tagged FloorTag, so it stays correct as more Floor-tagged surfaces are added.
	 *
	 * Also includes any individual PrimitiveComponent tagged FloorTag, even on an
	 * actor that is not itself tagged - lets a single component on an otherwise
	 * untagged actor mark its own footprint as buildable floor. Same collision
	 * requirement as the actor path (IsCollisionEnabled()), so this doesn't loosen
	 * what counts as "real floor" - it just adds a second, independent way to
	 * mark it.
	 *
	 * Uses AActor::GetComponentsBoundingBox (engine collision/render bounds), not
	 * a hand-rolled bounds calculation - same "call the engine's own math, don't
	 * re-derive it" approach as the rest of this plugin.
	 *
	 * Expects EXACTLY these on BuildManagerRef's class (confirmed against
	 * BP_BuildManager as of this writing): BuilableBoundaryMin/BuilableBoundaryMax
	 * (FVector2D) and BuildableBoundaryEnabled (bool) [sic - matches BP_BuildManager's
	 * actual, misspelled variable names]. Fails loudly (returns false, boundary left
	 * untouched) if any is missing/retyped, or if no actor tagged FloorTag exists in
	 * BuildManagerRef's world.
	 *
	 * @param BuildManagerRef  A BP_BuildManager_C instance.
	 * @param OutMin, OutMax   The computed/applied boundary (world-space X/Y, cm).
	 * @param FloorTag         Actor tag identifying valid floor surfaces. Defaults to "Floor".
	 * @return true if at least one FloorTag actor or FloorTag-tagged component was found and the boundary was applied and enabled.
	 */
	UFUNCTION(BlueprintCallable, Category = "PreciseTransform")
	static bool ApplyBuildableBoundaryFromFloorTags(UObject* BuildManagerRef, FVector2D& OutMin, FVector2D& OutMax, FName FloorTag = "Floor");

	/**
	 * One-off data-population bridge for the `DynamicDatatable` plugin's tables
	 * (e.g. DynDataTable2): merges a native UDataTable's rows into a
	 * UDynamicDataTable, reusing UDynamicDatatableBPLibrary::AddRowsFromDataTable
	 * as-is - the row-merge logic itself is not reimplemented here.
	 *
	 * This exists solely because of a gap in that plugin's own Blueprint API:
	 * AddRowsFromDataTable takes an FDynamicTable& (a plain USTRUCT), which only
	 * exists as a live value while a Blueprint graph is EXECUTING - there is no
	 * way to read or write it as a nested asset property from outside a running
	 * graph (confirmed: the editor bridge's generic property-path setter refuses
	 * to navigate into the TMap it's built from). This function instead accesses
	 * Target->DynamicTable directly as a real C++ member, so calling it once from
	 * a Blueprint's Construction Script (which runs in the actual editor, not
	 * PIE) mutates and persists real asset data once the caller saves it -
	 * exactly like manually editing the row in this plugin's own row-editor UI
	 * would, just scriptable.
	 *
	 * @param Target     The DynamicDataTable asset to merge rows into (e.g. DynDataTable2).
	 * @param Source     A native DataTable (matching or compatible row struct) whose rows get merged in.
	 * @param Overwrite  If true, a Target row with the same name as a Source row is replaced; if false, it's left untouched.
	 * @return true if both Target and Source were valid and the merge ran.
	 */
	UFUNCTION(BlueprintCallable, Category = "DynamicDataTable")
	static bool MergeDataTableIntoDynamicDataTable(UDynamicDataTable* Target, UDataTable* Source, bool Overwrite);

	/**
	 * Diagnostic: logs every field of one row in a live UDynamicDataTable, by
	 * name and value, via LogPreciseTransform. Exists because this plugin's
	 * only prior readback (in MergeDataTableIntoDynamicDataTable) only ever
	 * logged 2 fields (PlaceableObjectClass, Icon) - every other field of a
	 * populated row had never actually been confirmed correct on the live
	 * asset, only on its native UDataTable import source. Reusable for any
	 * future DynamicDataTable row-data investigation, not a throwaway.
	 *
	 * @param Target   The DynamicDataTable asset to read from (e.g. DynDataTable2).
	 * @param RowName  The row to dump.
	 * @return true if Target was valid and RowName exists in it.
	 */
	UFUNCTION(BlueprintCallable, Category = "DynamicDataTable")
	static bool DumpDynamicDataTableRow(UDynamicDataTable* Target, FName RowName);

	/**
	 * Diagnostic: logs a named property's value on any UObject via reflection,
	 * regardless of its declaring class - for comparing a Blueprint variable
	 * (e.g. CanBuild) across sibling classes at runtime without adding a
	 * temporary node to each one's own graph.
	 *
	 * @param Target        Any object (e.g. a spawned object placer instance).
	 * @param PropertyName  The property to read (matches its Blueprint variable name).
	 * @return true if Target was valid and PropertyName was found and logged.
	 */
	UFUNCTION(BlueprintCallable, Category = "PreciseTransform")
	static bool DumpObjectProperty(UObject* Target, FName PropertyName);

	/**
	 * Diagnostic: reads a UObject*-typed property off Target via reflection
	 * and returns it - a cross-class "Get <Property>" that works from any
	 * Blueprint, unlike this bridge's abstracted variable_get node type
	 * (which can only target a variable on the Blueprint currently being
	 * edited). Used to read a variable like BuildManagerRef.ActiveObjectPlacer
	 * from a throwaway diagnostic actor without editing BP_BuildManager itself.
	 *
	 * @param Target        Any object.
	 * @param PropertyName  The object-typed property to read.
	 * @return The property's value, or nullptr if Target/PropertyName was invalid.
	 */
	UFUNCTION(BlueprintCallable, Category = "PreciseTransform")
	static UObject* GetObjectProperty(UObject* Target, FName PropertyName);

	/**
	 * Walkway Warning (informational-only placement feedback): checks whether
	 * Location falls within the world-space bounds of any actor tagged
	 * WalkwayTag. Mirrors ApplyBuildableBoundaryFromFloorTags's exact query
	 * pattern (TActorIterator + ActorHasTag + GetComponentsBoundingBox) - same
	 * "Floor" tag mechanism already used elsewhere in this plugin, just a
	 * point-in-bounds test instead of a combined-bounds one. No existing
	 * "walkway"/"path" concept exists in the project as of this writing; this
	 * is a new, simple Actor Tag ("Walkway" by default) that can be applied to
	 * any floor-section actor, same as "Floor" already is.
	 *
	 * Unlike ApplyBuildableBoundaryFromFloorTags (which passes bNonColliding=
	 * false to GetComponentsBoundingBox, deliberately requiring real floor
	 * collision), this passes bNonColliding=true - a Walkway marker is a pure
	 * tag/bounds check with no physical role, and MUST NOT need collision:
	 * BP_BuildManager::GetCellLocation does its own ground-height line trace
	 * (Visibility channel) looking for the first hit tagged "Floor" - a
	 * Walkway marker that also blocks that trace would mask the real Floor
	 * beneath it and wrongly fail placement validity for every cell under it
	 * (confirmed root cause of a real bug: a Walkway_Test_Marker actor with
	 * default BlockAll collision terminated that trace before it ever reached
	 * the real Floor mesh). Keeping this check collision-independent means a
	 * Walkway marker can safely use NoCollision.
	 *
	 * Does NOT affect placement validity/occupancy - purely a query for
	 * DrawPlacementIndicators to pick a different (still non-blocking) ghost
	 * material when an otherwise-valid placement's cell overlaps a
	 * Walkway-tagged actor's bounds.
	 *
	 * @param WorldContextObject  Any object with a valid World (e.g. the calling placer actor).
	 * @param Location            World-space location to test (e.g. a cell's GetCellLocation output).
	 * @param WalkwayTag          Actor tag identifying walkway surfaces. Defaults to "Walkway".
	 * @return true if Location falls within the XY bounds of at least one WalkwayTag actor.
	 */
	UFUNCTION(BlueprintCallable, Category = "PreciseTransform")
	static bool IsLocationOverWalkway(UObject* WorldContextObject, FVector Location, FName WalkwayTag = "Walkway");

	/**
	 * Door Fit Check (P2 Item 15): informational-only ghost-material warning
	 * when a placement footprint would block a DoorFrame-tagged actor's
	 * clearance path. Same collision-independent, tag/bounds-only pattern as
	 * IsLocationOverWalkway above - deliberately does NOT reuse GetCellLocation's
	 * ground trace or any Floor-tag state, so this check is unaffected by
	 * whether the underlying cell is over valid floor.
	 *
	 * A door marker's own mesh bounds are usually just the frame itself, not
	 * the swing/walk-through clearance it needs kept clear - so unlike Walkway
	 * (which tests raw actor bounds), this expands the DoorFrame actor's XY
	 * bounds by ClearanceRadius before testing Location against it.
	 *
	 * Does NOT affect placement validity/occupancy - purely a query for
	 * DrawPlacementIndicators to pick a different (still non-blocking) ghost
	 * material when an otherwise-valid placement's cell falls within a
	 * DoorFrame-tagged actor's clearance zone.
	 *
	 * @param WorldContextObject  Any object with a valid World (e.g. the calling placer actor).
	 * @param Location            World-space location to test (e.g. a cell's GetCellLocation output).
	 * @param DoorTag             Actor tag identifying door frames. Defaults to "DoorFrame".
	 * @param ClearanceRadius     XY expansion applied to the door actor's bounds, in cm. Defaults to 150 (1.5m assumed standard clearance).
	 * @return true if Location falls within the expanded XY bounds of at least one DoorTag actor.
	 */
	UFUNCTION(BlueprintCallable, Category = "PreciseTransform")
	static bool IsLocationBlockingDoorClearance(UObject* WorldContextObject, FVector Location, FName DoorTag = "DoorFrame", float ClearanceRadius = 150.0f);

	/**
	 * Returns the StaticMesh an actor class shows by default, read from its
	 * component templates without spawning it. Exists so object placers can
	 * build their placement ghost from the placeable they are about to place
	 * (BP_ObjectPlacer_Base_Class.PlaceableObjectClass) instead of a mesh
	 * hardcoded per placer: any BP_PlaceableObject_Base_Class subclass that
	 * just sets a mesh on a StaticMeshComponent gets a matching ghost.
	 *
	 * Blueprint can't do this itself - Get Class Defaults exposes variables
	 * but not components, and spawning a throwaway placeable would run its
	 * BeginPlay (grid occupancy registration). Walks most-derived class first,
	 * so a subclass's own mesh (or its override of an inherited component)
	 * wins over a parent's. Native CDO components are checked last.
	 *
	 * @param ActorClass  Class to inspect (e.g. BP_Demo_Bank_C).
	 * @return The first StaticMeshComponent template's mesh that is set, or nullptr (logged) if none.
	 */
	UFUNCTION(BlueprintPure, Category = "PreciseTransform")
	static UStaticMesh* GetClassDefaultStaticMesh(TSubclassOf<AActor> ActorClass);

	/**
	 * Turns a just-placed object's visible mesh by Yaw degrees about world Z,
	 * so it lands at the rotation its placement ghost showed. The mesh
	 * component is picked with the same rule the ghost uses
	 * (GetClassDefaultStaticMesh on the actor's class), so ghost and result
	 * always refer to the same geometry. Additive (preserves any authored
	 * rotation on the component); a no-op for Yaw ~= 0.
	 *
	 * @param Placeable  The placed actor (a BP_PlaceableObject_Base_Class instance).
	 * @param Yaw        Ghost yaw in degrees (BP_PlaceableObject_Base_Class.ObjectFreeYaw).
	 * @return true if at least one component was rotated (or Yaw was ~0).
	 */
	UFUNCTION(BlueprintCallable, Category = "PreciseTransform")
	static bool ApplyPlacementYaw(AActor* Placeable, float Yaw);
	/**
	 * Shows the placement ghost as ONE mesh, centred on the footprint.
	 *
	 * The standard placer (BP_Demo_Bank_Placer) used to put GhostMesh on every
	 * footprint cell's indicator (one squashed copy per cell) - invisible while
	 * every catalog object was 1x1, wrong for anything larger (an imported
	 * container spans many cells). This creates/reuses a single
	 * StaticMeshComponent named "GhostMeshComponent" attached to the placer's
	 * "RotationPivot" (which the placer moves to the footprint centre and turns
	 * for free rotation), so the ghost sits centred, rotates with C, and keeps
	 * its real proportions. NoCollision: a ghost must never become a trace or
	 * overlap target.
	 *
	 * @param Placer  The placer actor (needs a scene component named RotationPivot).
	 * @param Mesh    The mesh to show (BP_ObjectPlacer_Base_Class.GhostMesh); null hides the ghost.
	 * @return true if the ghost component exists and shows Mesh.
	 */
	UFUNCTION(BlueprintCallable, Category = "PreciseTransform")
	static bool ShowPlacerGhostMesh(AActor* Placer, UStaticMesh* Mesh);

	/**
	 * World yaw of the ghost shown by ShowPlacerGhostMesh - what the placed
	 * object must be turned by so it lands as the ghost showed it (free
	 * rotation and the actor-level Shift+C/V rotation both included). Falls
	 * back to the actor's own yaw if there is no ghost component.
	 */
	UFUNCTION(BlueprintPure, Category = "PreciseTransform")
	static float GetPlacerGhostYaw(AActor* Placer);
};

