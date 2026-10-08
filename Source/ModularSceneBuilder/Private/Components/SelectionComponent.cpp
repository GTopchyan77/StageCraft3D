// Copyright Epic Games, Inc. All Rights Reserved.

#include "Components/SelectionComponent.h"

#include "GameFramework/Actor.h"
#include "Interaction/InteractableInterface.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(SelectionComponent)

USelectionComponent::USelectionComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void USelectionComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ClearSelection();
	Super::EndPlay(EndPlayReason);
}

bool USelectionComponent::IsSelectable(const AActor* Actor)
{
	// An actor already being destroyed would be selected after its OnDestroyed fired, leaving a dangling selection.
	return IsValid(Actor) && !Actor->IsActorBeingDestroyed() && Actor->Implements<UInteractableInterface>();
}

bool USelectionComponent::SelectActor(AActor* Target)
{
	if (!IsSelectable(Target))
	{
		return false;
	}
	ApplySelection({ Target });
	return true;
}

bool USelectionComponent::ToggleActorSelection(AActor* Target)
{
	if (!IsSelectable(Target))
	{
		return false;
	}

	TArray<AActor*> NewSelection = GetSelectedActors();
	const bool bWasSelected = NewSelection.Remove(Target) > 0;
	if (!bWasSelected)
	{
		NewSelection.Add(Target);
	}
	ApplySelection(NewSelection);
	return !bWasSelected;
}

void USelectionComponent::DeselectActor(AActor* Target)
{
	TArray<AActor*> NewSelection = GetSelectedActors();
	if (Target && NewSelection.Remove(Target) > 0)
	{
		ApplySelection(NewSelection);
	}
}

void USelectionComponent::SelectActors(const TArray<AActor*>& Targets)
{
	TArray<AActor*> NewSelection;
	NewSelection.Reserve(Targets.Num());
	for (AActor* Target : Targets)
	{
		if (IsSelectable(Target))
		{
			NewSelection.AddUnique(Target);
		}
	}
	ApplySelection(NewSelection);
}

void USelectionComponent::ClearSelection()
{
	ApplySelection({});
}

TArray<AActor*> USelectionComponent::GetSelectedActors() const
{
	TArray<AActor*> Result;
	Result.Reserve(SelectedActors.Num());
	for (const TObjectPtr<AActor>& Actor : SelectedActors)
	{
		Result.Add(Actor.Get());
	}
	return Result;
}

bool USelectionComponent::IsActorSelected(const AActor* Actor) const
{
	return Actor && SelectedActors.Contains(Actor);
}

void USelectionComponent::ApplySelection(const TArray<AActor*>& NewSelection)
{
	const TArray<AActor*> PreviousSelection = GetSelectedActors();
	if (PreviousSelection == NewSelection)
	{
		return;
	}

	AActor* PreviousPrimary = GetSelectedActor();

	// The stored set changes before any callback runs, so a listener that reads the selection sees the final state.
	SelectedActors.Reset(NewSelection.Num());
	for (AActor* Actor : NewSelection)
	{
		SelectedActors.Add(Actor);
	}

	for (AActor* Actor : PreviousSelection)
	{
		if (!NewSelection.Contains(Actor) && Actor)
		{
			Actor->OnDestroyed.RemoveDynamic(this, &ThisClass::HandleSelectedActorDestroyed);
			IInteractableInterface::Execute_OnDeselect(Actor);
		}
	}
	for (AActor* Actor : NewSelection)
	{
		if (!PreviousSelection.Contains(Actor))
		{
			Actor->OnDestroyed.AddUniqueDynamic(this, &ThisClass::HandleSelectedActorDestroyed);
			IInteractableInterface::Execute_OnSelect(Actor);
		}
	}

	AActor* NewPrimary = GetSelectedActor();
	if (NewPrimary != PreviousPrimary)
	{
		OnSelectionChanged.Broadcast(NewPrimary, PreviousPrimary);
	}
	OnSelectionSetChanged.Broadcast(SelectedActors.Num());
}

void USelectionComponent::HandleSelectedActorDestroyed(AActor* DestroyedActor)
{
	DeselectActor(DestroyedActor);
}
