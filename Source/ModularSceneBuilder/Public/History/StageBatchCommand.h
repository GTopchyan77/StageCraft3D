// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "History/StageEditCommand.h"

/**
 * Several commands that one user action produced (deleting a multi-selection, moving a group with the gizmo,
 * clearing the stage), undone and redone as a single history step (Docs/ADR/0004-selection-scenes-and-rendering.md §3.3).
 *
 * All or nothing: undo runs the children's Undo newest first, redo runs their Redo oldest first. If a child does not
 * succeed, the children already applied in this step are reverted again in reverse order, so the stage is exactly as it
 * was before the step, and the child's result (Refused or Invalid) is returned for the history to act on. A rollback
 * that itself fails is logged as an error; the step is then reported Invalid, because its state can no longer be trusted.
 *
 * Pure logic against IStageItemEditor; covered by the StageCraft.History.Batch tests.
 */
class MODULARSCENEBUILDER_API FStageBatchCommand final : public IStageEditCommand
{
public:
	/** InCommands must hold at least one command, in the order they were applied. */
	FStageBatchCommand(FText InDescription, TArray<TSharedRef<IStageEditCommand>> InCommands);

	virtual FText GetDescription() const override { return Description; }
	virtual EStageCommandResult Undo(IStageItemEditor& Editor) override;
	virtual EStageCommandResult Redo(IStageItemEditor& Editor) override;

	int32 Num() const { return Commands.Num(); }

private:
	FText Description;
	TArray<TSharedRef<IStageEditCommand>> Commands;
};
