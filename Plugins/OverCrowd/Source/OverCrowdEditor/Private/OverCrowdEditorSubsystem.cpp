// This file is Copyright (C) 2025 Overcrowd Development Engineering Ltd, all rights reserved


#include "OverCrowdEditorSubsystem.h"
#include "CrowdMemberPlacement.h"
#include "Kismet/GameplayStatics.h"
#include "CrowdInstancer.h"
//
void UOverCrowdEditorSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// FWorldDelegates::LevelAddedToWorld.AddUObject(this, &UOverCrowdEditorSubsystem::OnLevelAddedToWorld);
	// FWorldDelegates::OnCurrentLevelChanged.AddUObject(this, &UOverCrowdEditorSubsystem::OnLevelChanged);
	// FCoreUObjectDelegates::OnObjectModified.AddUObject(this, &UOverCrowdEditorSubsystem::OnObjectModified);
	FCoreUObjectDelegates::OnObjectPropertyChanged.AddUObject(this, &UOverCrowdEditorSubsystem::OnPropertyChanged);

	if (GEngine)
	{
		GEngine->OnLevelActorDeleted().AddUObject(this, &UOverCrowdEditorSubsystem::OnLevelActorsRemoved);
	}
}

void UOverCrowdEditorSubsystem::Deinitialize()
{
}
//
// void UOverCrowdEditorSubsystem::OnLevelAddedToWorld(ULevel* InLevel, UWorld* World)
// {
// }
//
// void UOverCrowdEditorSubsystem::OnLevelChanged(ULevel* InNewLevel, ULevel* InOldLevel, UWorld* InWorld)
// {
// }
//
// void UOverCrowdEditorSubsystem::OnObjectModified(UObject* Object)
// {
// }

void UOverCrowdEditorSubsystem::OnPropertyChanged(UObject* ObjectBeingModified, FPropertyChangedEvent& PropertyChangedEvent)
{
	ACrowdMemberPlacement *Placement = Cast<ACrowdMemberPlacement>(ObjectBeingModified);
	if (Placement && PropertyChangedEvent.GetPropertyName() != NAME_None)
	{
		Placement->OnPropertyEdited(PropertyChangedEvent.GetPropertyName());
		Placement->bPlacementVisibility = true;
	}
}

void UOverCrowdEditorSubsystem::OnLevelActorsRemoved(AActor* InActor)
{
	ACrowdMemberPlacement* Placement = Cast<ACrowdMemberPlacement>(InActor);

	if (Placement)
	{
		UWorld* WorldContext = InActor->GetWorld();
		
		TArray<AActor*> FoundActors;
		UGameplayStatics::GetAllActorsOfClass(WorldContext, AOverCrowdManager::StaticClass(), FoundActors);

		for (auto Actor : FoundActors)
		{
			if (AOverCrowdManager* CrowdInstancer = Cast<AOverCrowdManager>(Actor))
			{
				//TODO::Uncomment
				//if(Placement->PlacementGroup == CrowdInstancer->PlacementGroup)
					CrowdInstancer->bRegenerateNextTick = true;
			}
		}
	}
}
