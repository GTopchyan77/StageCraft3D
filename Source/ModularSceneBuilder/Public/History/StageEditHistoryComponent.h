// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "History/StageEditCommand.h"
#include "StageEditHistoryComponent.generated.h"

/**
 * The local player's link to the stage's undo history (Docs/ADR/0003-undo-redo-and-object-snapping.md):
 * it records what this player's tools did, and runs undo/redo against the live world.
 *
 * Recording (each user action becomes exactly one step, after it succeeded):
 *  - Place: UStagePlacementToolComponent::OnItemPlaced (bound here).
 *  - Delete: DeleteItem, which the controller's Delete key goes through.
 *  - Transform: RecordTransformChange, called by the controller when a gizmo drag ends and after a
 *    validated Location / Rotation / Scale edit.
 * Undo and redo never go through those entry points, so they can never record themselves.
 *
 * Applying: restores go through USpawnSystemComponent::RestoreItem (the placement rules still decide),
 * removals release the selection first, transforms use the shared scale clamp. Items are found by
 * instance id in UStageSessionSubsystem.
 *
 * Requires USpawnSystemComponent and USelectionComponent on the same actor; UStagePlacementToolComponent
 * is optional (without it placements are not recorded). Undo/redo is requested through the controller
 * (AModularPlayerController::RequestUndo / RequestRedo), which owns the input guards. No tick.
 */
UCLASS(ClassGroup = (StageCraft), meta = (BlueprintSpawnableComponent))
class MODULARSCENEBUILDER_API UStageEditHistoryComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UStageEditHistoryComponent();

	/**
	 * Deletes a stage item as an undoable step (its selection is released first). Returns false, and
	 * records nothing, for anything that is not a deletable stage item.
	 */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|History")
	bool DeleteItem(AActor* Target);

	/**
	 * Records that Target moved from Before to its current transform. Ignored when nothing changed
	 * meaningfully or Target is not a stage item. Call once per finished edit, not per drag frame.
	 */
	void RecordTransformChange(AActor* Target, const FTransform& Before);

	EStageCommandResult Undo();
	EStageCommandResult Redo();

	/** This world's history, or null outside Game/PIE worlds. */
	class UStageEditHistorySubsystem* GetHistory() const;

protected:
	//~ Begin UActorComponent Interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	//~ End UActorComponent Interface

private:
	UFUNCTION()
	void HandleItemPlaced(class AModularBaseActor* PlacedActor);

	void Record(TSharedRef<IStageEditCommand> Command) const;
	EStageCommandResult Step(bool bUndo);

	UPROPERTY(Transient)
	TObjectPtr<class USpawnSystemComponent> SpawnSystem = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class USelectionComponent> Selection = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UStagePlacementToolComponent> PlacementTool = nullptr;
};
