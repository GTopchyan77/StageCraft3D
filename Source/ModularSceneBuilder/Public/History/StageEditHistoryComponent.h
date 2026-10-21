// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Data/StageParameterTypes.h"
#include "History/StageEditCommand.h"
#include "StageEditHistoryComponent.generated.h"

/** One item's transform before an edit; its current transform is the "after". Held only for the length of one edit. */
struct FStageTransformChange
{
	TWeakObjectPtr<class AActor> Item;
	FTransform Before = FTransform::Identity;
};

/**
 * Writes a parameter during undo / redo through the rules, without recording it again. Bound by the owning controller to the
 * GameMode and the session; unbound means no rules apply (tests, tools) and the value is written straight to the item.
 */
DECLARE_DELEGATE_RetVal_ThreeParams(EStageCommandResult, FStageParameterApplier, class AModularBaseActor& /*Item*/, const FGameplayTag& /*ParameterId*/, const FStageParameterValue& /*Value*/);

/**
 * Decides whether undo / redo may put Item at Transform (sanitized, scale clamped) and reports a refusal itself. Bound by the
 * owning controller to AStageCraftGameModeBase::EvaluateTransformChange, so a locked item cannot be moved by Ctrl+Z either.
 * Unbound means no rules apply (tests, tools); only the sanity check (StageTransformRules::IsSaneTransform) remains.
 */
DECLARE_DELEGATE_RetVal_TwoParams(bool, FStageTransformValidator, const class AModularBaseActor& /*Item*/, const FTransform& /*Transform*/);

/**
 * The local player's link to the stage's undo history (Docs/ADR/0003-undo-redo-and-object-snapping.md):
 * it records what this player's tools did, and runs undo/redo against the live world.
 *
 * Recording (each user action becomes exactly one step, after it succeeded):
 *  - Place: UStagePlacementToolComponent::OnItemPlaced and UStageSpanToolComponent::OnItemPlaced (bound here).
 *  - Parameter: RecordParameterChange, called by the controller after a validated edit that changes what an item is made of
 *    (a span's length); undo / redo write it back through ParameterApplier, so the rules decide again.
 *  - Delete: DeleteItem, which the controller's Delete key goes through.
 *  - Transform: RecordTransformChange, called by the controller when a gizmo drag ends and after a
 *    validated Location / Rotation / Scale edit.
 *  - Cues: RecordCueListChange, called by the controller after a validated cue store / update / delete.
 * Undo and redo never go through those entry points, so they can never record themselves.
 *
 * Transactions (BeginTransaction / EndTransaction): a continuous edit (dragging an inspector field) changes the stage many
 * times. Transform and parameter changes recorded while a transaction is open are merged per item and parameter, keeping
 * the first "before", and become ONE step when it ends. Only one transaction is open at a time.
 *
 * Applying: restores go through USpawnSystemComponent::RestoreItem (the placement rules still decide),
 * removals release the selection first. A transform must be sane and is checked by TransformValidator (lock, scale support)
 * for every item, so a group step whose items include a locked one is refused as a whole (FStageBatchCommand rolls back).
 * Parameters go through ParameterApplier. Items are found by instance id in UStageSessionSubsystem.
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
	 * Deletes every stage item in Targets as ONE undoable step ("Delete 3 items"); undo brings them all back.
	 * Anything that is not a deletable stage item is skipped. Returns how many items were deleted.
	 */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|History")
	int32 DeleteItems(const TArray<AActor*>& Targets);

	/**
	 * Removes every placed item from the stage as one undoable step ("Clear Stage (12 items)"). Callers confirm
	 * with the user first (the Edit menu's Clear Stage asks); Ctrl+Z restores the whole stage. Returns the count removed.
	 */
	int32 ClearStage();

	/**
	 * Records that Target moved from Before to its current transform. Ignored when nothing changed
	 * meaningfully or Target is not a stage item. Call once per finished edit, not per drag frame.
	 */
	void RecordTransformChange(AActor* Target, const FTransform& Before);

	/**
	 * Records one edit that moved several items (a group drag, a group numeric edit) as a single step. Items that did
	 * not change meaningfully, or are no longer stage items, are left out; nothing is recorded if none changed.
	 */
	void RecordTransformChanges(TConstArrayView<FStageTransformChange> Changes);

	/**
	 * Records that ParameterId of Target changed from Before to its current value (read back from the item, so a clamped value is
	 * what is recorded). Ignored when nothing changed meaningfully or Target is not a stage item. ParameterName names the step.
	 */
	void RecordParameterChange(AActor* Target, const FGameplayTag& ParameterId, const FText& ParameterName, const FStageParameterValue& Before);

	/**
	 * Records a cue edit (store, update, delete) as one step holding the whole cue list before and after it
	 * (Docs/ADR/0011-cue-list-and-patch-sheet-export.md §2.4). An open transaction is ended first, so the two never merge.
	 * Description names the step ("Store Cue 3").
	 */
	void RecordCueListChange(const FText& Description, const TArray<struct FShowCue>& Before, const TArray<struct FShowCue>& After);

	/** How undo / redo write parameters. See FStageParameterApplier. */
	FStageParameterApplier ParameterApplier;

	/** Whether undo / redo may write a transform. See FStageTransformValidator. */
	FStageTransformValidator TransformValidator;

	/**
	 * Opens a transaction: until EndTransaction, RecordTransformChange(s) and RecordParameterChange collect instead of
	 * recording. Returns false, and changes nothing, if one is already open (the caller then must not end it).
	 */
	bool BeginTransaction();

	/**
	 * Closes the open transaction and records everything it collected as one step (each item's first "before" against its
	 * current state; items that ended where they started are left out). No-op when none is open. Undo / redo end it first.
	 */
	void EndTransaction();

	bool IsTransactionOpen() const { return Transaction.IsSet(); }

	/** Ends an open transaction, then undoes / redoes the latest step. */
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

	/** Records Commands as one step: the command itself when there is one, a batch named Description otherwise. */
	void RecordAsOneStep(TArray<TSharedRef<IStageEditCommand>> Commands, const FText& BatchDescription) const;

	/** Deletes the deletable items among Targets and records them as one step named by MakeDescription(Count). */
	int32 DeleteAsOneStep(const TArray<AActor*>& Targets, TFunctionRef<FText(int32)> MakeBatchDescription);

	EStageCommandResult Step(bool bUndo);

	/** Commands for Changes, as one step would record them: unchanged and dead items are left out. */
	static TArray<TSharedRef<IStageEditCommand>> MakeTransformCommands(TConstArrayView<FStageTransformChange> Changes);

	/** A parameter change of one item, collected by an open transaction. */
	struct FPendingParameterChange
	{
		TWeakObjectPtr<class AModularBaseActor> Item;
		FGameplayTag ParameterId;
		FText ParameterName;
		FStageParameterValue Before;
	};

	/** What an open transaction collected: the first "before" per item (transforms) and per item and parameter. */
	struct FPendingTransaction
	{
		TArray<FStageTransformChange> Transforms;
		TArray<FPendingParameterChange> Parameters;
	};

	TOptional<FPendingTransaction> Transaction;

	UPROPERTY(Transient)
	TObjectPtr<class USpawnSystemComponent> SpawnSystem = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class USelectionComponent> Selection = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UStagePlacementToolComponent> PlacementTool = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UStageSpanToolComponent> SpanTool = nullptr;
};
