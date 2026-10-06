// Copyright Epic Games, Inc. All Rights Reserved.

#include "Audio/StageEditorAudioFeedbackComponent.h"

#include "Audio/StageAudioSubsystem.h"
#include "Audio/StageAudioTypes.h"
#include "Components/SelectionComponent.h"
#include "Engine/GameInstance.h"
#include "ModularSceneBuilder.h"
#include "Placement/StageSnappingComponent.h"
#include "Player/ModularPlayerController.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageEditorAudioFeedbackComponent)

UStageEditorAudioFeedbackComponent::UStageEditorAudioFeedbackComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UStageEditorAudioFeedbackComponent::BeginPlay()
{
	Super::BeginPlay();

	Controller = Cast<AModularPlayerController>(GetOwner());
	if (!ensureMsgf(Controller, TEXT("%s must be owned by an AModularPlayerController."), *GetName()) || !Controller->IsLocalController())
	{
		Controller = nullptr;
		return;
	}

	const UGameInstance* GameInstance = Controller->GetGameInstance();
	Audio = GameInstance ? GameInstance->GetSubsystem<UStageAudioSubsystem>() : nullptr;

	Controller->GetSelection()->OnSelectionChanged.AddUniqueDynamic(this, &ThisClass::HandleSelectionChanged);
	Controller->OnRequestRejected.AddUniqueDynamic(this, &ThisClass::HandleRequestRejected);

	UStagePlacementToolComponent* Placement = Controller->GetPlacementTool();
	Placement->OnItemPlaced.AddUniqueDynamic(this, &ThisClass::HandleItemPlaced);
	Placement->OnPlacementSnapped.AddUniqueDynamic(this, &ThisClass::HandlePlacementSnapped);
	Placement->OnPlacementFailed.AddUniqueDynamic(this, &ThisClass::HandlePlacementFailed);

	Controller->GetSnapping()->OnSnapEngaged.AddUniqueDynamic(this, &ThisClass::HandleObjectSnapEngaged);
}

void UStageEditorAudioFeedbackComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (Controller)
	{
		Controller->GetSelection()->OnSelectionChanged.RemoveDynamic(this, &ThisClass::HandleSelectionChanged);
		Controller->OnRequestRejected.RemoveDynamic(this, &ThisClass::HandleRequestRejected);

		UStagePlacementToolComponent* Placement = Controller->GetPlacementTool();
		Placement->OnItemPlaced.RemoveDynamic(this, &ThisClass::HandleItemPlaced);
		Placement->OnPlacementSnapped.RemoveDynamic(this, &ThisClass::HandlePlacementSnapped);
		Placement->OnPlacementFailed.RemoveDynamic(this, &ThisClass::HandlePlacementFailed);
		Controller->GetSnapping()->OnSnapEngaged.RemoveDynamic(this, &ThisClass::HandleObjectSnapEngaged);
		Controller = nullptr;
	}

	Super::EndPlay(EndPlayReason);
}

void UStageEditorAudioFeedbackComponent::HandleSelectionChanged(AActor* NewSelection, AActor* PreviousSelection)
{
	// Deselection is silent: it happens implicitly (Esc, entering Place mode) far more often than by intent.
	if (NewSelection)
	{
		Play(StageCraftTags::Sound_Select);
	}
}

void UStageEditorAudioFeedbackComponent::HandleItemPlaced(AModularBaseActor* PlacedActor)
{
	Play(StageCraftTags::Sound_Place);
}

void UStageEditorAudioFeedbackComponent::HandlePlacementSnapped(FVector LandingPoint)
{
	Play(StageCraftTags::Sound_Snap);
}

void UStageEditorAudioFeedbackComponent::HandleObjectSnapEngaged()
{
	Play(StageCraftTags::Sound_Snap);
}

void UStageEditorAudioFeedbackComponent::HandlePlacementFailed(EStagePlacementFailure Reason)
{
	Play(StageCraftTags::Sound_Error);
}

void UStageEditorAudioFeedbackComponent::HandleRequestRejected(const FStageEconomyResultInfo& Result)
{
	Play(StageCraftTags::Sound_Error);
}

void UStageEditorAudioFeedbackComponent::Play(const FGameplayTag& CueTag) const
{
	if (UStageAudioSubsystem* AudioSubsystem = Audio.Get())
	{
		AudioSubsystem->PlayCue(CueTag);
	}
}
