// Copyright DamenTools internal. Precise Transform plugin.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "WarehousePlannerSaveLibrary.generated.h"

/**
 * Basic Scenario Save/Load for the Warehouse Planner demo. Mirrors the
 * reflection-based BuildManagerRef access pattern already proven in
 * SpaceCalculatorLibrary/PreciseTransformLibrary (BP_BuildManager is a
 * Blueprint-only class with no native base).
 *
 * Save reads BuildManagerRef.ObjectsData (map<FIntPoint, BP_PlaceableObject_Base_Class_C>)
 * via reflection, de-duplicates by object (a multi-cell object occupies several
 * cells but must only be saved once), and records each unique placed object's
 * class + world transform (location/rotation/scale) into a UWarehousePlannerSaveGame slot.
 *
 * Load destroys every currently-placed object by calling its own
 * DestroyPlaceableObject() UFUNCTION via reflection - traced directly from
 * BP_PlaceableObject_Base_Class's graph, it releases that object's cells from
 * BuildManagerRef's OccupancyData/ObjectsData maps (SetOccupancyData(false) +
 * RemoveObjectData per cell) and then destroys the actor itself, so grid state
 * stays consistent exactly like a normal demolition would - nothing here is
 * reimplemented independently, same "call the Blueprint's own functions"
 * philosophy as the rest of this plugin. It then spawns each saved class at
 * its saved transform via GetWorld()->SpawnActorDeferred, sets the spawned
 * object's private BuildManager backreference via reflection BEFORE calling
 * FinishSpawning - without this, BeginPlay's own SetupPlaceableObject/
 * SetupBorder chain (and later per-tick UpdatePlaceableObjectUnderCursor/
 * EnableObjectOutline, which all read self.BuildManager) throw repeated
 * "Accessed None ... property BuildManager" runtime errors, confirmed via a
 * real PIE test.
 *
 * KNOWN LIMITATION: loaded objects are still NOT re-registered into
 * BuildManagerRef's OccupancyData/ObjectsData maps. BuildManager being set
 * means SetupPlaceableObject's ForEachLoop (over self.OccupiedCells) no
 * longer errors, but OccupiedCells/ObjectSize/OccupiedCenterCell are never
 * computed for a loaded object, so that loop simply iterates zero cells -
 * no grid registration happens, and border/outline sizing may look slightly
 * off (visuals) but no more Accessed-None errors are thrown. Doing full grid
 * re-registration correctly requires deriving the object's real grid
 * footprint, which BP_PlaceableObject_Base_Class only ever receives from
 * BP_ObjectPlacer_Base_Class's placement flow (untraced, out of scope for
 * this basic pass). Until that follow-up lands, cells under loaded objects
 * show as free in BuildManagerRef's grid, so new placements can visually
 * overlap them.
 */
UCLASS()
class PRECISETRANSFORM_API UWarehousePlannerSaveLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Saves every currently-placed object's class + world transform to slot
	 * SlotName. Expects EXACTLY this on BuildManagerRef's class (confirmed
	 * against BP_BuildManager as of this writing):
	 *   ObjectsData : TMap<FIntPoint, BP_PlaceableObject_Base_Class_C*>
	 * Fails loudly (returns false, no slot written) if that property is
	 * missing/retyped.
	 *
	 * @param BuildManagerRef  A BP_BuildManager_C instance.
	 * @param SlotName         Save slot name.
	 * @param UserIndex        Save slot user index (single-user tool - defaults to 0).
	 * @return true if BuildManagerRef was valid and the slot was written (even if 0 objects were placed).
	 */
	UFUNCTION(BlueprintCallable, Category = "WarehousePlanner|Save")
	static bool SaveWarehouseLayout(UObject* BuildManagerRef, FString SlotName = TEXT("WarehouseLayout"), int32 UserIndex = 0);

	/**
	 * Loads slot SlotName, destroys every currently-placed object (via each
	 * object's own DestroyPlaceableObject() - see class comment), then spawns
	 * a new actor of each saved class at its saved transform.
	 *
	 * Expects EXACTLY this on BuildManagerRef's class: ObjectsData (as above).
	 * Expects a public, no-parameter UFUNCTION DestroyPlaceableObject() on
	 * each placed object's class (confirmed against
	 * BP_PlaceableObject_Base_Class as of this writing) - if missing on any
	 * object's class, that object is destroyed directly via Actor->Destroy()
	 * instead (logged loudly) as a fallback, which will leave stale entries in
	 * BuildManagerRef's OccupancyData/ObjectsData for that object's cells.
	 *
	 * @param BuildManagerRef  A BP_BuildManager_C instance.
	 * @param SlotName         Save slot name.
	 * @param UserIndex        Save slot user index.
	 * @return true if the slot existed and was loaded. See class comment for the grid-occupancy known limitation.
	 */
	UFUNCTION(BlueprintCallable, Category = "WarehousePlanner|Save")
	static bool LoadWarehouseLayout(UObject* BuildManagerRef, FString SlotName = TEXT("WarehouseLayout"), int32 UserIndex = 0);

	/**
	 * Destroys every currently-placed object via its own DestroyPlaceableObject()
	 * UFUNCTION (see class comment) - same destroy step LoadWarehouseLayout runs
	 * before respawning, just without the respawn, so OccupancyData/ObjectsData
	 * end up correctly empty instead of stale.
	 *
	 * @param BuildManagerRef  A BP_BuildManager_C instance.
	 * @return true if BuildManagerRef was valid (even if 0 objects were placed).
	 */
	UFUNCTION(BlueprintCallable, Category = "WarehousePlanner|Save")
	static bool ClearAllPlacedObjects(UObject* BuildManagerRef);
};
