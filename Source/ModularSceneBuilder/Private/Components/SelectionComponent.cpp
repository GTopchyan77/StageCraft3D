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

bool USelectionComponent::SelectActor(AActor* Target)
{
	if (!IsValid(Target) || !Target->Implements<UInteractableInterface>())
	{
		return false;
	}

	SetSelection(Target);
	return true;
}

void USelectionComponent::ClearSelection()
{
	SetSelection(nullptr);
}

void USelectionComponent::SetSelection(AActor* NewSelection)
{
	if (NewSelection == SelectedActor)
	{
		return;
	}

	AActor* PreviousSelection = SelectedActor;

	if (PreviousSelection)
	{
		PreviousSelection->OnDestroyed.RemoveDynamic(this, &ThisClass::HandleSelectedActorDestroyed);
		IInteractableInterface::Execute_OnDeselect(PreviousSelection);
	}

	SelectedActor = NewSelection;

	if (SelectedActor)
	{
		SelectedActor->OnDestroyed.AddDynamic(this, &ThisClass::HandleSelectedActorDestroyed);
		IInteractableInterface::Execute_OnSelect(SelectedActor);
	}

	OnSelectionChanged.Broadcast(SelectedActor, PreviousSelection);
}

void USelectionComponent::HandleSelectedActorDestroyed(AActor* DestroyedActor)
{
	if (DestroyedActor == SelectedActor)
	{
		ClearSelection();
	}
}
