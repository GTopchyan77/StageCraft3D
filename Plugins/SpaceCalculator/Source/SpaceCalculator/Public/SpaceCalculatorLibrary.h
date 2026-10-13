// Copyright DamenTools internal. Space Calculator plugin.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "SpaceCalculatorLibrary.generated.h"

UCLASS()
class SPACECALCULATOR_API USpaceCalculatorLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Computes Warehouse Planner floor-space usage for a BP_BuildManager instance.
	 *
	 * BP_BuildManager is a Blueprint-only class (no native base) — see the
	 * WB_Demo_UI C++ conversion investigation. Reading its state therefore
	 * requires this deliberate reflection bridge (FindFProperty) rather than
	 * a direct member access. It expects EXACTLY these three UPROPERTYs to
	 * exist on BuildManagerRef's class, by these exact names and types
	 * (confirmed against BP_BuildManager as of this writing):
	 *   - GridCellSizeXY : float or double           (grid cell edge length, cm — Blueprint
	 *                                                  "float" vars can be FFloatProperty or
	 *                                                  FDoubleProperty depending on their Real
	 *                                                  Number setting; both are accepted)
	 *   - CellsCount     : int32                     (GridSize.X * GridSize.Y)
	 *   - OccupancyData  : TMap<FIntPoint, int32>     (key present == cell occupied)
	 *
	 * If BP_BuildManager is ever refactored and any of these three properties
	 * is renamed, retyped, or removed, this function fails LOUDLY: it returns
	 * false and zeroes all three out params, rather than silently returning
	 * stale or garbage numbers. Update the property names below (and the
	 * FindFProperty calls in the .cpp) if BP_BuildManager changes.
	 *
	 * @param BuildManagerRef  A BP_BuildManager_C instance (or any object exposing the three properties above).
	 * @param UsedAreaM2       Out: occupied cell count * cell area, in square meters.
	 * @param AvailableAreaM2  Out: total grid cell count * cell area, in square meters.
	 * @param PercentUsed      Out: UsedAreaM2 / AvailableAreaM2 * 100, or 0 if AvailableAreaM2 is 0.
	 * @return true if all three properties were found and read successfully.
	 */
	UFUNCTION(BlueprintCallable, Category = "SpaceCalculator")
	static bool GetSpaceUsage(UObject* BuildManagerRef, float& UsedAreaM2, float& AvailableAreaM2, float& PercentUsed);

	/**
	 * HUD convenience wrapper around GetSpaceUsage — formats its three out
	 * params into a single ready-to-display line ("Used: X m^2 / Available:
	 * Y m^2 (Z%)"). Exists purely to keep Blueprint-side wiring to one call +
	 * one SetText; it relies on the exact same three BP_BuildManager
	 * properties documented on GetSpaceUsage above and shares its fail-loud
	 * behavior (returns a visibly-broken "n/a" string, not a blank/garbage
	 * one, if those properties are missing).
	 *
	 * @param BuildManagerRef  A BP_BuildManager_C instance (or any object exposing GetSpaceUsage's three properties).
	 * @return Formatted display text.
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "SpaceCalculator")
	static FText GetSpaceUsageText(UObject* BuildManagerRef);

	/**
	 * Per-object-type breakdown of currently placed objects for a BP_BuildManager
	 * instance. Reuses the exact reflection pattern already proven by
	 * WarehousePlannerSaveLibrary::GetUniquePlacedObjects (PreciseTransform
	 * plugin) - reimplemented locally rather than depending on that module, to
	 * keep SpaceCalculator fully self-contained: reads BuildManagerRef's
	 * ObjectsData (TMap<FIntPoint, BP_PlaceableObject_Base_Class_C*>) via
	 * reflection, de-duplicates by actor pointer (a multi-cell object occupies
	 * several map entries but must only be counted once), then buckets each
	 * unique object by its class's display name with the generated-class "_C"
	 * suffix and a leading "BP_" stripped (e.g. class "BP_Demo_RefMan_C"
	 * becomes key "Demo_RefMan").
	 *
	 * Expects EXACTLY this on BuildManagerRef's class (confirmed against
	 * BP_BuildManager as of this writing): ObjectsData (as above). Fails
	 * loudly (returns false, OutCounts left empty) if that property is
	 * missing/retyped - same "BP_BuildManager may have been refactored"
	 * contract as GetSpaceUsage above.
	 *
	 * @param BuildManagerRef  A BP_BuildManager_C instance.
	 * @param OutCounts        Out: class display name -> count of currently placed unique objects of that class.
	 * @return true if ObjectsData was found and read successfully (even if 0 objects are currently placed).
	 */
	UFUNCTION(BlueprintCallable, Category = "SpaceCalculator")
	static bool GetPlacedObjectCounts(UObject* BuildManagerRef, TMap<FString, int32>& OutCounts);

	/**
	 * HUD convenience wrapper around GetPlacedObjectCounts - formats the
	 * per-type counts into a single ready-to-display line ("Type: N | Type: N"),
	 * sorted alphabetically by type name for a stable display order. Exists for
	 * the same reason as GetSpaceUsageText: keeps Blueprint-side wiring to one
	 * call + one SetText.
	 *
	 * @param BuildManagerRef  A BP_BuildManager_C instance.
	 * @return Formatted display text ("Placed: n/a" if ObjectsData couldn't be read; "Placed: (none)" if valid but empty).
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "SpaceCalculator")
	static FText GetPlacedObjectCountsText(UObject* BuildManagerRef);
};
