// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Economy/StageEconomyTypes.h"
#include "Placement/StagePlacementToolComponent.h"
#include "StageEditorAudioFeedbackComponent.generated.h"

/**
 * Maps editor events of its owning AModularPlayerController to feedback cues, and nothing else:
 *   selection of an item -> Select, item placed -> Place, preview snapped -> Snap,
 *   placement failed or any request refused -> Error.
 *
 * Keeps the audio subsystem free of gameplay knowledge and the controller free of audio knowledge.
 * Binds in BeginPlay (local controllers only), unbinds in EndPlay. No tick.
 */
UCLASS(ClassGroup = (StageCraft), meta = (BlueprintSpawnableComponent))
class MODULARSCENEBUILDER_API UStageEditorAudioFeedbackComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UStageEditorAudioFeedbackComponent();

protected:
	//~ Begin UActorComponent Interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	//~ End UActorComponent Interface

private:
	UFUNCTION()
	void HandleSelectionChanged(AActor* NewSelection, AActor* PreviousSelection);

	UFUNCTION()
	void HandleItemPlaced(class AModularBaseActor* PlacedActor);

	UFUNCTION()
	void HandlePlacementSnapped(FVector LandingPoint);

	UFUNCTION()
	void HandlePlacementFailed(EStagePlacementFailure Reason);

	UFUNCTION()
	void HandleRequestRejected(const FStageEconomyResultInfo& Result);

	void Play(const FGameplayTag& CueTag) const;

	UPROPERTY(Transient)
	TObjectPtr<class AModularPlayerController> Controller = nullptr;

	TWeakObjectPtr<class UStageAudioSubsystem> Audio;
};
