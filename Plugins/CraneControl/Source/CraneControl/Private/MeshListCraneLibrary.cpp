// Copyright DamenTools internal. Crane Control plugin.

#include "MeshListCraneLibrary.h"
#include "CraneControl.h"
#include "CraneMeshMountComponent.h"
#include "CraneRegistrySubsystem.h"
#include "PreciseTransformLibrary.h"
#include "DynamicDatatable/Public/DynamicDatatableBPLibrary.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "UObject/UnrealType.h"

namespace
{
	const FName HudPropertyName(TEXT("WB Demo UI"));
	const FName CraneOnPropertyName(TEXT("bIsCraneOn"));

	/**
	 * Row-struct member by its Blueprint name. User-defined struct members are
	 * stored internally as "<Name>_<N>_<GUID>" (e.g. MeshMesh_123_ABC...), and
	 * the authored-name lookup isn't guaranteed outside the editor, so match
	 * either form.
	 */
	const FProperty* FindRowMember(const UScriptStruct* RowStruct, const TCHAR* AuthoredName)
	{
		const FString Prefix = FString(AuthoredName) + TEXT("_");
		for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
		{
			if (It->GetAuthoredName() == AuthoredName || It->GetName() == AuthoredName || It->GetName().StartsWith(Prefix))
			{
				return *It;
			}
		}
		return nullptr;
	}
}

bool UMeshListCraneLibrary::IsCraneModeOn(const UObject* WorldContextObject)
{
	UWorld* World = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
	APlayerController* PlayerController = World ? UGameplayStatics::GetPlayerController(World, 0) : nullptr;
	if (!PlayerController)
	{
		UE_LOG(LogCraneControl, Error, TEXT("IsCraneModeOn: no local PlayerController - treating crane mode as OFF."));
		return false;
	}

	const FObjectProperty* HudProperty = FindFProperty<FObjectProperty>(PlayerController->GetClass(), HudPropertyName);
	UObject* Hud = HudProperty ? HudProperty->GetObjectPropertyValue_InContainer(PlayerController) : nullptr;
	if (!Hud)
	{
		UE_LOG(LogCraneControl, Error, TEXT("IsCraneModeOn: '%s' has no set '%s' (%s) - treating crane mode as OFF. See MeshListCraneLibrary.h."),
			*PlayerController->GetClass()->GetName(), *HudPropertyName.ToString(), HudProperty ? TEXT("not assigned yet") : TEXT("property missing"));
		return false;
	}

	const FBoolProperty* CraneOnProperty = FindFProperty<FBoolProperty>(Hud->GetClass(), CraneOnPropertyName);
	if (!CraneOnProperty)
	{
		UE_LOG(LogCraneControl, Error, TEXT("IsCraneModeOn: '%s' has no bool '%s' - treating crane mode as OFF. See MeshListCraneLibrary.h."),
			*Hud->GetClass()->GetName(), *CraneOnPropertyName.ToString());
		return false;
	}
	return CraneOnProperty->GetPropertyValue_InContainer(Hud);
}

UStaticMesh* UMeshListCraneLibrary::GetEntryMesh(const FDynamicDataTableRowHandle& Entry)
{
	// Rows live in DynamicTable.Map - the store every DynamicDatatable Blueprint
	// node (and so the list and the placers) reads and writes. The table's legacy
	// RowMap only holds rows serialized with the asset, so it misses runtime rows.
	const UDynamicDataTable* Table = Entry.DynamicDataTable;
	const FInstancedStruct* RowData = Table ? Table->DynamicTable.Map.Find(Entry.RowName) : nullptr;
	const uint8* Row = RowData ? RowData->GetMemory() : nullptr;
	const UScriptStruct* RowStruct = RowData ? RowData->GetScriptStruct() : nullptr;
	if (!Row || !RowStruct)
	{
		UE_LOG(LogCraneControl, Error, TEXT("GetEntryMesh: row '%s' not found in '%s'."), *Entry.RowName.ToString(), *GetNameSafe(Table));
		return nullptr;
	}

	if (const FObjectProperty* MeshProperty = CastField<FObjectProperty>(FindRowMember(RowStruct, TEXT("MeshMesh"))))
	{
		if (UStaticMesh* Imported = Cast<UStaticMesh>(MeshProperty->GetObjectPropertyValue_InContainer(Row)))
		{
			return Imported;
		}
	}

	if (const FClassProperty* ClassProperty = CastField<FClassProperty>(FindRowMember(RowStruct, TEXT("PlaceableObjectClass"))))
	{
		if (UClass* PlaceableClass = Cast<UClass>(ClassProperty->GetObjectPropertyValue_InContainer(Row)))
		{
			return UPreciseTransformLibrary::GetClassDefaultStaticMesh(PlaceableClass);
		}
	}

	UE_LOG(LogCraneControl, Error, TEXT("GetEntryMesh: row '%s' has neither a MeshMesh nor a PlaceableObjectClass with a mesh."), *Entry.RowName.ToString());
	return nullptr;
}

bool UMeshListCraneLibrary::TryMountEntryOnCrane(const UObject* WorldContextObject, const FDynamicDataTableRowHandle& Entry)
{
	if (!IsCraneModeOn(WorldContextObject))
	{
		return false;
	}

	// From here on crane mode is ON: the click belongs to the crane even if the
	// mount fails, so no ghost/placement is started as a side effect.
	UWorld* World = WorldContextObject->GetWorld();
	UCraneRegistrySubsystem* Registry = World ? World->GetSubsystem<UCraneRegistrySubsystem>() : nullptr;
	UCraneMeshMountComponent* Mount = Registry ? Registry->GetCraneMount() : nullptr;
	if (!Mount)
	{
		UE_LOG(LogCraneControl, Error, TEXT("TryMountEntryOnCrane: crane mode is ON but no crane is registered - is CraneMeshMount on WareHouseMeshBP2? '%s' NOT mounted."),
			*Entry.RowName.ToString());
		return true;
	}

	UStaticMesh* Mesh = GetEntryMesh(Entry);
	if (!Mesh)
	{
		return true; // logged by GetEntryMesh
	}

	Mount->SetMountedMesh(Mesh);
	return true;
}
