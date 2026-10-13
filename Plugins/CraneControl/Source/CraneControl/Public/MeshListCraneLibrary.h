// Copyright DamenTools internal. Crane Control plugin.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "DynamicDatatable/DynamicDataTable/DynamicDataTable.h"
#include "MeshListCraneLibrary.generated.h"

class UStaticMesh;

/**
 * The Crane-ON half of the Mesh List click (WB_Building_List_Item).
 *
 * One list serves both uses; the HUD's Crane On/Off state decides what a
 * click does. WB_Building_List_Item calls TryMountEntryOnCrane first:
 *   - crane OFF -> returns false and the item carries on with the unchanged
 *     BuildManager.EnableBuildingTool ghost/placement flow;
 *   - crane ON  -> hangs the entry's mesh on the crane hook (replacing what
 *     was there) and returns true, so no ghost / placement is started.
 *
 * Reads Blueprint-only state through this project's reflection-bridge
 * convention (FindFProperty by name, fail loudly), matching
 * UPreciseTransformLibrary / UDamenFileManagerWidget:
 *   - PlayerController(0) property "WB Demo UI" (PC_Demo_PlayerCntroller) ->
 *     its bool "bIsCraneOn" (WB_Demo_UI, flipped by the Crane On/Off button) -
 *     the single source of truth for crane mode.
 *   - the row struct's "MeshMesh" (runtime StaticMesh of an imported entry) and
 *     "PlaceableObjectClass" (catalog entries: its class-default mesh, the same
 *     mesh its placement ghost shows).
 * The crane itself comes from UCraneRegistrySubsystem (no world scans).
 */
UCLASS()
class CRANECONTROL_API UMeshListCraneLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** @return true if crane mode is ON (the click is fully handled here, do NOT start placement). */
	UFUNCTION(BlueprintCallable, Category = "Crane|MeshList", meta = (WorldContext = "WorldContextObject"))
	static bool TryMountEntryOnCrane(const UObject* WorldContextObject, const FDynamicDataTableRowHandle& Entry);

	/** The mesh an entry shows everywhere (ghost, placed object, crane): its imported MeshMesh, else its placeable class's default mesh. */
	UFUNCTION(BlueprintPure, Category = "Crane|MeshList")
	static UStaticMesh* GetEntryMesh(const FDynamicDataTableRowHandle& Entry);

	/** Reads WB_Demo_UI.bIsCraneOn via the local PlayerController's "WB Demo UI". Logs and returns false if it can't be read. */
	UFUNCTION(BlueprintPure, Category = "Crane|MeshList", meta = (WorldContext = "WorldContextObject"))
	static bool IsCraneModeOn(const UObject* WorldContextObject);
};
