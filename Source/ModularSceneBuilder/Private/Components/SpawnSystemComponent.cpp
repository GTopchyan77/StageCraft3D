// Copyright Epic Games, Inc. All Rights Reserved.

#include "Components/SpawnSystemComponent.h"

#include "Actors/ModularBaseActor.h"
#include "Data/BaseItemData.h"
#include "Engine/World.h"
#include "Interaction/InteractableInterface.h"
#include "ModularSceneBuilder.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(SpawnSystemComponent)

USpawnSystemComponent::USpawnSystemComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

AModularBaseActor* USpawnSystemComponent::SpawnItem(UBaseItemData* Item, const FTransform& Transform)
{
	UWorld* World = GetWorld();
	if (!World || !Item)
	{
		UE_LOG(LogStageCraft, Warning, TEXT("%s: cannot spawn %s: missing world or item."), *GetNameSafe(GetOwner()), *GetNameSafe(Item));
		return nullptr;
	}

	if (PlacementValidator.IsBound() && !PlacementValidator.Execute(*Item).IsSuccess())
	{
		// The validator's owner reports the refusal (toast, error cue); nothing to add here.
		return nullptr;
	}

	// Normally already resident: UStageItemSubsystem streams the Game bundle before an item is armed.
	UClass* ActorClass = Item->ActorClass.LoadSynchronous();
	if (!ActorClass)
	{
		UE_LOG(LogStageCraft, Warning, TEXT("%s: cannot spawn %s: ActorClass is not set."), *GetNameSafe(GetOwner()), *Item->GetName());
		return nullptr;
	}

	// Deferred so construction scripts and BeginPlay already see the item data.
	AModularBaseActor* Actor = World->SpawnActorDeferred<AModularBaseActor>(ActorClass, Transform, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Actor)
	{
		return nullptr;
	}

	Actor->InitializeFromItemData(Item);
	Actor->FinishSpawning(Transform);

	UE_LOG(LogStageCraft, Log, TEXT("Placed %s (%s) at %s."), *Actor->GetName(), *Item->GetName(), *Transform.GetLocation().ToCompactString());
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
