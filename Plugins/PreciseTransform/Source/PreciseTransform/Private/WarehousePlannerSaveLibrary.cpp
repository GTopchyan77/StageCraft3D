// Copyright DamenTools internal. Precise Transform plugin.

#include "WarehousePlannerSaveLibrary.h"
#include "WarehousePlannerSaveGame.h"
#include "PreciseTransform.h"
#include "UObject/UnrealType.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"

namespace
{
	/**
	 * Reads BuildManagerRef.ObjectsData (map<FIntPoint, BP_PlaceableObject_Base_Class_C>)
	 * via reflection and returns the de-duplicated set of placed object actors -
	 * a multi-cell object occupies several map entries but must only be
	 * processed once. See WarehousePlannerSaveLibrary.h for the expected property.
	 */
	bool GetUniquePlacedObjects(UObject* BuildManagerRef, const TCHAR* CallerName, TArray<AActor*>& OutObjects)
	{
		OutObjects.Reset();

		if (!IsValid(BuildManagerRef))
		{
			UE_LOG(LogPreciseTransform, Warning, TEXT("%s: BuildManagerRef is null/invalid."), CallerName);
			return false;
		}

		UClass* Class = BuildManagerRef->GetClass();
		const FMapProperty* ObjectsDataProp = FindFProperty<FMapProperty>(Class, TEXT("ObjectsData"));
		if (!ObjectsDataProp)
		{
			UE_LOG(LogPreciseTransform, Error,
				TEXT("%s: BuildManagerRef (class %s) is missing ObjectsData(map) - BP_BuildManager may have been renamed/refactored."),
				CallerName, *Class->GetName());
			return false;
		}

		const FObjectProperty* ValueProp = CastField<FObjectProperty>(ObjectsDataProp->ValueProp);
		if (!ValueProp)
		{
			UE_LOG(LogPreciseTransform, Error,
				TEXT("%s: ObjectsData's value type is not an object reference - BP_BuildManager may have been refactored."), CallerName);
			return false;
		}

		FScriptMapHelper MapHelper(ObjectsDataProp, ObjectsDataProp->ContainerPtrToValuePtr<void>(BuildManagerRef));
		TSet<AActor*> Unique;
		for (int32 Index = 0; Index < MapHelper.GetMaxIndex(); ++Index)
		{
			if (!MapHelper.IsValidIndex(Index))
			{
				continue;
			}
			UObject* Obj = ValueProp->GetObjectPropertyValue(MapHelper.GetValuePtr(Index));
			if (AActor* Actor = Cast<AActor>(Obj))
			{
				if (IsValid(Actor))
				{
					Unique.Add(Actor);
				}
			}
		}

		OutObjects = Unique.Array();
		return true;
	}
}

bool UWarehousePlannerSaveLibrary::SaveWarehouseLayout(UObject* BuildManagerRef, FString SlotName, int32 UserIndex)
{
	TArray<AActor*> PlacedActors;
	if (!GetUniquePlacedObjects(BuildManagerRef, TEXT("SaveWarehouseLayout"), PlacedActors))
	{
		return false;
	}

	UWarehousePlannerSaveGame* SaveGame = Cast<UWarehousePlannerSaveGame>(
		UGameplayStatics::CreateSaveGameObject(UWarehousePlannerSaveGame::StaticClass()));
	if (!SaveGame)
	{
		UE_LOG(LogPreciseTransform, Error, TEXT("SaveWarehouseLayout: failed to create UWarehousePlannerSaveGame instance."));
		return false;
	}

	SaveGame->PlacedObjects.Reserve(PlacedActors.Num());
	for (AActor* Actor : PlacedActors)
	{
		FWarehousePlacedObjectSaveData Entry;
		Entry.ObjectClass = Actor->GetClass();
		Entry.Location = Actor->GetActorLocation();
		Entry.Rotation = Actor->GetActorRotation();
		Entry.Scale = Actor->GetActorScale3D();
		SaveGame->PlacedObjects.Add(Entry);
	}

	const bool bSaved = UGameplayStatics::SaveGameToSlot(SaveGame, SlotName, UserIndex);
	UE_LOG(LogPreciseTransform, Log,
		TEXT("SaveWarehouseLayout: saved %d object(s) to slot '%s' (UserIndex=%d) - %s."),
		SaveGame->PlacedObjects.Num(), *SlotName, UserIndex, bSaved ? TEXT("OK") : TEXT("FAILED"));

	return bSaved;
}

bool UWarehousePlannerSaveLibrary::LoadWarehouseLayout(UObject* BuildManagerRef, FString SlotName, int32 UserIndex)
{
	if (!IsValid(BuildManagerRef))
	{
		UE_LOG(LogPreciseTransform, Warning, TEXT("LoadWarehouseLayout: BuildManagerRef is null/invalid."));
		return false;
	}

	if (!UGameplayStatics::DoesSaveGameExist(SlotName, UserIndex))
	{
		UE_LOG(LogPreciseTransform, Warning, TEXT("LoadWarehouseLayout: no save found in slot '%s' (UserIndex=%d)."), *SlotName, UserIndex);
		return false;
	}

	UWarehousePlannerSaveGame* SaveGame = Cast<UWarehousePlannerSaveGame>(UGameplayStatics::LoadGameFromSlot(SlotName, UserIndex));
	if (!SaveGame)
	{
		UE_LOG(LogPreciseTransform, Error, TEXT("LoadWarehouseLayout: slot '%s' exists but did not load as UWarehousePlannerSaveGame."), *SlotName);
		return false;
	}

	UWorld* World = BuildManagerRef->GetWorld();
	if (!World)
	{
		UE_LOG(LogPreciseTransform, Error, TEXT("LoadWarehouseLayout: BuildManagerRef has no valid World."));
		return false;
	}

	// Destroy every currently-placed object first - see header comment: each
	// object's own DestroyPlaceableObject() releases its cells from
	// BuildManagerRef's OccupancyData/ObjectsData and destroys itself.
	TArray<AActor*> CurrentActors;
	GetUniquePlacedObjects(BuildManagerRef, TEXT("LoadWarehouseLayout"), CurrentActors);
	for (AActor* Actor : CurrentActors)
	{
		if (!IsValid(Actor))
		{
			continue;
		}
		UFunction* DestroyFunc = Actor->GetClass()->FindFunctionByName(TEXT("DestroyPlaceableObject"));
		if (DestroyFunc)
		{
			Actor->ProcessEvent(DestroyFunc, nullptr);
		}
		else
		{
			UE_LOG(LogPreciseTransform, Error,
				TEXT("LoadWarehouseLayout: '%s' (class %s) has no DestroyPlaceableObject() - destroying directly. BuildManagerRef's OccupancyData/ObjectsData may now hold stale entries for this object's cells."),
				*Actor->GetName(), *Actor->GetClass()->GetName());
			Actor->Destroy();
		}
	}

	// Respawn each saved object at its saved transform. See header comment:
	// these are NOT re-registered into BuildManagerRef's grid occupancy.
	//
	// Spawned DEFERRED (not a plain SpawnActor): BP_PlaceableObject_Base_Class's
	// BeginPlay chain (SetupPlaceableObject/SetupBorder, and later per-tick
	// UpdatePlaceableObjectUnderCursor/EnableObjectOutline) all read its private
	// BuildManager backreference, which is normally set by BP_ObjectPlacer_Base_Class
	// during real placement. A plain SpawnActor runs BeginPlay immediately, before
	// we'd get a chance to set it, producing repeated "Accessed None ... property
	// BuildManager" runtime errors (confirmed via Gevor's PIE test). Deferred spawn
	// lets us set BuildManager via reflection BEFORE FinishSpawning fires BeginPlay,
	// same "give it what real placement would have given it" approach as the rest
	// of this plugin.
	int32 SpawnedCount = 0;

	for (const FWarehousePlacedObjectSaveData& Entry : SaveGame->PlacedObjects)
	{
		UClass* ObjectClass = Entry.ObjectClass.LoadSynchronous();
		if (!ObjectClass)
		{
			UE_LOG(LogPreciseTransform, Warning, TEXT("LoadWarehouseLayout: saved entry has an unresolvable class ('%s') - skipped."), *Entry.ObjectClass.ToString());
			continue;
		}

		const FTransform SpawnTransform(Entry.Rotation, Entry.Location, Entry.Scale);
		AActor* Spawned = World->SpawnActorDeferred<AActor>(ObjectClass, SpawnTransform, nullptr, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!Spawned)
		{
			UE_LOG(LogPreciseTransform, Warning, TEXT("LoadWarehouseLayout: failed to spawn '%s' at (%.1f, %.1f, %.1f)."),
				*ObjectClass->GetName(), Entry.Location.X, Entry.Location.Y, Entry.Location.Z);
			continue;
		}

		if (FObjectProperty* BuildManagerProp = CastField<FObjectProperty>(Spawned->GetClass()->FindPropertyByName(TEXT("BuildManager"))))
		{
			BuildManagerProp->SetObjectPropertyValue_InContainer(Spawned, BuildManagerRef);
		}
		else
		{
			UE_LOG(LogPreciseTransform, Warning,
				TEXT("LoadWarehouseLayout: '%s' has no BuildManager property to initialize before BeginPlay - it may throw Accessed-None runtime errors that reference it."),
				*ObjectClass->GetName());
		}

		Spawned->FinishSpawning(SpawnTransform);
		++SpawnedCount;
	}

	UE_LOG(LogPreciseTransform, Log,
		TEXT("LoadWarehouseLayout: destroyed %d existing object(s), spawned %d of %d saved object(s) from slot '%s'."),
		CurrentActors.Num(), SpawnedCount, SaveGame->PlacedObjects.Num(), *SlotName);

	return true;
}

bool UWarehousePlannerSaveLibrary::ClearAllPlacedObjects(UObject* BuildManagerRef)
{
	TArray<AActor*> CurrentActors;
	if (!GetUniquePlacedObjects(BuildManagerRef, TEXT("ClearAllPlacedObjects"), CurrentActors))
	{
		return false;
	}

	// Same per-object destroy step as LoadWarehouseLayout - see header comment:
	// each object's own DestroyPlaceableObject() releases its cells from
	// BuildManagerRef's OccupancyData/ObjectsData and destroys itself.
	for (AActor* Actor : CurrentActors)
	{
		if (!IsValid(Actor))
		{
			continue;
		}
		UFunction* DestroyFunc = Actor->GetClass()->FindFunctionByName(TEXT("DestroyPlaceableObject"));
		if (DestroyFunc)
		{
			Actor->ProcessEvent(DestroyFunc, nullptr);
		}
		else
		{
			UE_LOG(LogPreciseTransform, Error,
				TEXT("ClearAllPlacedObjects: '%s' (class %s) has no DestroyPlaceableObject() - destroying directly. BuildManagerRef's OccupancyData/ObjectsData may now hold stale entries for this object's cells."),
				*Actor->GetName(), *Actor->GetClass()->GetName());
			Actor->Destroy();
		}
	}

	UE_LOG(LogPreciseTransform, Log, TEXT("ClearAllPlacedObjects: destroyed %d placed object(s)."), CurrentActors.Num());
	return true;
}
