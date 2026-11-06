// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "History/StageCommandHistory.h"
#include "History/StageEditCommand.h"
#include "StageEditHistorySubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnStageEditHistoryChanged, EStageHistoryChange, Change, const FText&, Description);

/**
 * The undo/redo history of the stage being built in this world (Docs/ADR/0003-undo-redo-and-object-snapping.md).
 *
 * A world subsystem because the history describes one stage session: it must not survive level
 * travel, where the items it names no longer exist. Game and PIE worlds only, like UStageSessionSubsystem.
 *
 * Owns the stacks only. It never touches the world itself: commands run against the IStageItemEditor
 * passed in by the caller, which is the local player's UStageEditHistoryComponent (reached through the
 * controller's RequestUndo / RequestRedo bridge). Views read the descriptions and bind OnHistoryChanged.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageEditHistorySubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem Interface
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	/** Adds an action that has already been applied. Clears the redo stack. While a group is open it is collected instead. */
	void Record(TSharedRef<IStageEditCommand> Command);

	/**
	 * Undoes / redoes the latest step. Refused, and nothing changes, while a group is open: the group's steps are not on the
	 * stack yet, so an undo would revert an older step underneath a plan that is still running.
	 */
	EStageCommandResult Undo(IStageItemEditor& Editor);
	EStageCommandResult Redo(IStageItemEditor& Editor);

	/**
	 * Opens a group (Docs/ADR/0030 §6, an assistant plan is one undo step): until EndGroup or AbortGroup, every recorded step is
	 * collected, whichever request recorded it (cues, patch, programmer, placements, environment). False, and nothing changes,
	 * when a group is already open. Only one is open at a time; ClearHistory drops it.
	 */
	bool BeginGroup(const FText& Description);

	/**
	 * Closes the open group and records what it collected as ONE step: the command itself when it holds one (keeping its own
	 * name), a batch named by the group otherwise. Nothing is recorded for an empty group. No-op when none is open.
	 */
	void EndGroup();

	/**
	 * Closes the open group and reverts what it collected through Editor, newest first, all or nothing (FStageBatchCommand), and
	 * records nothing. If the revert is refused or fails, what was collected is recorded as one step after all, so the user can
	 * still undo it, and that result is returned. Succeeded for an empty group; NothingToDo when none is open.
	 */
	EStageCommandResult AbortGroup(IStageItemEditor& Editor);

	bool IsGroupOpen() const { return Group.IsSet(); }

	/** How many steps the open group has collected (0 when none is open). */
	int32 GetGroupCount() const { return Group.IsSet() ? Group->Commands.Num() : 0; }

	/**
	 * Forgets every step. Called when the whole stage is replaced (a scene file was loaded): the recorded steps name
	 * items of the previous stage and must not be applied to the new one. Broadcasts Cleared. Idempotent.
	 */
	void ClearHistory();

	UFUNCTION(BlueprintPure, Category = "StageCraft|History")
	bool CanUndo() const { return History.CanUndo(); }

	UFUNCTION(BlueprintPure, Category = "StageCraft|History")
	bool CanRedo() const { return History.CanRedo(); }

	UFUNCTION(BlueprintPure, Category = "StageCraft|History")
	int32 GetUndoCount() const { return History.GetUndoCount(); }

	UFUNCTION(BlueprintPure, Category = "StageCraft|History")
	int32 GetRedoCount() const { return History.GetRedoCount(); }

	UFUNCTION(BlueprintPure, Category = "StageCraft|History")
	FText GetUndoDescription() const { return History.GetUndoDescription(); }

	UFUNCTION(BlueprintPure, Category = "StageCraft|History")
	FText GetRedoDescription() const { return History.GetRedoDescription(); }

	/** Description is the step concerned (empty for Cleared). */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|History")
	FOnStageEditHistoryChanged OnHistoryChanged;

protected:
	//~ Begin UWorldSubsystem Interface
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	//~ End UWorldSubsystem Interface

private:
	EStageCommandResult ApplyStep(bool bUndo, IStageItemEditor& Editor);

	/** The open group's collected steps, oldest first, and its name. */
	struct FGroup
	{
		FText Description;
		TArray<TSharedRef<IStageEditCommand>> Commands;
	};

	/** The collected steps as the one command a step records: the only one itself, or a batch named by the group. */
	static TSharedRef<IStageEditCommand> MakeGroupCommand(FGroup&& Closed);

	FStageCommandHistory History;
	TOptional<FGroup> Group;
};
