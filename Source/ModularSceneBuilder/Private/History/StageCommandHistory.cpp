// Copyright Epic Games, Inc. All Rights Reserved.

#include "History/StageCommandHistory.h"

FStageCommandHistory::FStageCommandHistory(int32 InMaxDepth)
	: MaxDepth(FMath::Max(InMaxDepth, 1))
{
}

bool FStageCommandHistory::Record(TSharedRef<IStageEditCommand> Command)
{
	// A command that records while it runs would push itself onto the stack it is being moved between.
	if (!ensureMsgf(!bStepping, TEXT("FStageCommandHistory: a command recorded history while it was being undone or redone; ignored.")))
	{
		return false;
	}

	RedoStack.Reset();
	UndoStack.Add(MoveTemp(Command));
	if (UndoStack.Num() > MaxDepth)
	{
		UndoStack.RemoveAt(0, UndoStack.Num() - MaxDepth);
	}
	return true;
}

EStageCommandResult FStageCommandHistory::Undo(IStageItemEditor& Editor)
{
	return Step(UndoStack, RedoStack, Editor, /*bUndo*/ true);
}

EStageCommandResult FStageCommandHistory::Redo(IStageItemEditor& Editor)
{
	return Step(RedoStack, UndoStack, Editor, /*bUndo*/ false);
}

EStageCommandResult FStageCommandHistory::Step(FCommandStack& From, FCommandStack& To, IStageItemEditor& Editor, bool bUndo)
{
	if (From.IsEmpty() || bStepping)
	{
		return EStageCommandResult::NothingToDo;
	}

	// Held by value: the stacks are not touched while the command runs, but the reference must outlive any reallocation.
	const TSharedRef<IStageEditCommand> Command = From.Last();

	bStepping = true;
	const EStageCommandResult Result = bUndo ? Command->Undo(Editor) : Command->Redo(Editor);
	bStepping = false;

	switch (Result)
	{
	case EStageCommandResult::Succeeded:
		From.Pop(EAllowShrinking::No);
		To.Add(Command);
		break;
	case EStageCommandResult::Invalid:
		From.Pop(EAllowShrinking::No);
		break;
	case EStageCommandResult::Refused:
	case EStageCommandResult::NothingToDo:
		break;
	}
	return Result;
}

void FStageCommandHistory::Clear()
{
	UndoStack.Reset();
	RedoStack.Reset();
}

FText FStageCommandHistory::GetUndoDescription() const
{
	return UndoStack.IsEmpty() ? FText::GetEmpty() : UndoStack.Last()->GetDescription();
}

FText FStageCommandHistory::GetRedoDescription() const
{
	return RedoStack.IsEmpty() ? FText::GetEmpty() : RedoStack.Last()->GetDescription();
}
