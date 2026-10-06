// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "History/StageEditCommand.h"

/**
 * Undo and redo stacks of already-executed commands. Pure (no world, no UObjects), owned by
 * UStageEditHistorySubsystem and covered by the StageCraft.History.Stack tests.
 *
 * Rules:
 *  - Record pushes onto the undo stack and clears the redo stack (a new action forks history).
 *  - Undo/Redo move a command between the stacks only when it Succeeded. Refused leaves both stacks
 *    unchanged so the user can retry; Invalid drops the command, because it can never apply again.
 *  - The undo stack keeps at most MaxDepth steps; the oldest is discarded first.
 *  - Not re-entrant: recording from inside a command's Undo/Redo is a programming error and is ignored.
 * Game thread only.
 */
class MODULARSCENEBUILDER_API FStageCommandHistory
{
public:
	static constexpr int32 DefaultMaxDepth = 100;

	/** InMaxDepth below 1 is raised to 1. */
	explicit FStageCommandHistory(int32 InMaxDepth = DefaultMaxDepth);

	/** Returns false (and ignores the command) only while an undo or redo is running. */
	bool Record(TSharedRef<IStageEditCommand> Command);

	EStageCommandResult Undo(IStageItemEditor& Editor);
	EStageCommandResult Redo(IStageItemEditor& Editor);

	void Clear();

	bool CanUndo() const { return !UndoStack.IsEmpty(); }
	bool CanRedo() const { return !RedoStack.IsEmpty(); }
	int32 GetUndoCount() const { return UndoStack.Num(); }
	int32 GetRedoCount() const { return RedoStack.Num(); }
	int32 GetMaxDepth() const { return MaxDepth; }

	/** Description of the step Undo would revert, or empty. */
	FText GetUndoDescription() const;

	/** Description of the step Redo would re-apply, or empty. */
	FText GetRedoDescription() const;

private:
	using FCommandStack = TArray<TSharedRef<IStageEditCommand>>;

	/** Runs the top of From (Undo or Redo per bUndo) and moves it to To on success. */
	EStageCommandResult Step(FCommandStack& From, FCommandStack& To, IStageItemEditor& Editor, bool bUndo);

	FCommandStack UndoStack;
	FCommandStack RedoStack;
	int32 MaxDepth = DefaultMaxDepth;
	bool bStepping = false;
};
