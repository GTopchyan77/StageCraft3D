// Copyright Epic Games, Inc. All Rights Reserved.

#include "Components/SpawnSystemComponent.h"

#include "Actors/ModularBaseActor.h"
#include "Data/BaseItemData.h"
#include "Engine/World.h"
#include "History/StageItemSnapshot.h"
#include "Interaction/InteractableInterface.h"
#include "ModularSceneBuilder.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(SpawnSystemComponent)

USpawnSystemComponent::USpawnSystemComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

AModularBaseActor* USpawnSystemComponent::SpawnItem(UBaseItemData* Item, const FTransform& Transform)
{
	if (!Item)
	{
		UE_LOG(LogStageCraft, Warning, TEXT("%s: cannot spawn: no item."), *GetNameSafe(GetOwner()));
		return nullptr;
	}
	return SpawnValidated(*Item, Transform, nullptr);
}

AModularBaseActor* USpawnSystemComponent::RestoreItem(const FStageItemSnapshot& Snapshot)
{
	// Catalog items are resident while the catalog is loaded; this only loads if the asset was unloaded since.
	UBaseItemData* Item = Snapshot.Item.LoadSynchronous();
	if (!Item || !Snapshot.InstanceId.IsValid())
	{
		UE_LOG(LogStageCraft, Warning, TEXT("%s: cannot restore %s: its catalog item %s is gone."), *GetNameSafe(GetOwner()),
			*Snapshot.DisplayName.ToString(), *Snapshot.Item.ToString());
		return nullptr;
	}
	return SpawnValidated(*Item, Snapshot.Transform, &Snapshot);
}

AModularBaseActor* USpawnSystemComponent::SpawnValidated(UBaseItemData& Item, const FTransform& Transform, const FStageItemSnapshot* Restore)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		UE_LOG(LogStageCraft, Warning, TEXT("%s: cannot spawn %s: no world."), *GetNameSafe(GetOwner()), *Item.GetName());
		return nullptr;
	}

	if (PlacementValidator.IsBound() && !PlacementValidator.Execute(Item).IsSuccess())
	{
		// The validator's owner reports the refusal (toast, error cue); nothing to add here.
		return nullptr;
	}

	// Normally already resident: UStageItemSubsystem streams the Game bundle before an item is armed.
	UClass* ActorClass = Item.ActorClass.LoadSynchronous();
	if (!ActorClass)
	{
		UE_LOG(LogStageCraft, Warning, TEXT("%s: cannot spawn %s: ActorClass is not set."), *GetNameSafe(GetOwner()), *Item.GetName());
		return nullptr;
	}

	// Deferred so construction scripts and BeginPlay already see the item data (and, when restoring, the old id and state).
	AModularBaseActor* Actor = World->SpawnActorDeferred<AModularBaseActor>(ActorClass, Transform, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Actor)
	{
		return nullptr;
	}

	if (Restore)
	{
		Actor->AssignInstanceId(Restore->InstanceId);
		Actor->RestoreSnapshotState(*Restore);
	}
	Actor->InitializeFromItemData(&Item);
	Actor->FinishSpawning(Transform);

	UE_LOG(LogStageCraft, Log, TEXT("%s %s (%s) at %s."), Restore ? TEXT("Restored") : TEXT("Placed"), *Actor->GetName(), *Item.GetName(),
		*Transform.GetLocation().ToCompactString());
	OnItemSpawned.Broadcast(Actor);
	return Actor;
}

bool USpawnSystemComponent::TryDeleteActor(AActor* Target)
{
	if (!IsValid(Target) || Target->IsActorBeingDestroyed() || !Target->Implements<UInteractableInterface>())
	{
		return false;
	}

	UE_LOG(LogStageCraft, Log, TEXT("Deleting stage item %s."), *Target->GetName());
	OnItemDeleted.Broadcast(Target);
	// Selection listeners are released by AModularBaseActor::EndPlay.
	return Target->Destroy();
}
