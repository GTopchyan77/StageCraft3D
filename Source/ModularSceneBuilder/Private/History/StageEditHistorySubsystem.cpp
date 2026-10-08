// Copyright Epic Games, Inc. All Rights Reserved.

#include "History/StageEditHistorySubsystem.h"

#include "Engine/World.h"
#include "ModularSceneBuilder.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageEditHistorySubsystem)

bool UStageEditHistorySubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// Only worlds where a player builds a stage; editor and preview worlds have nothing to undo.
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UStageEditHistorySubsystem::Deinitialize()
{
	History.Clear();
	OnHistoryChanged.Clear();
	Super::Deinitialize();
}

void UStageEditHistorySubsystem::Record(TSharedRef<IStageEditCommand> Command)
{
	const FText Description = Command->GetDescription();
	if (History.Record(MoveTemp(Command)))
	{
		UE_LOG(LogStageCraft, Verbose, TEXT("History: recorded \"%s\" (%d undo steps)."), *Description.ToString(), History.GetUndoCount());
		OnHistoryChanged.Broadcast(EStageHistoryChange::Recorded, Description);
	}
}

void UStageEditHistorySubsystem::ClearHistory()
{
	if (!History.CanUndo() && !History.CanRedo())
	{
		return;
	}
	History.Clear();
	UE_LOG(LogStageCraft, Log, TEXT("History: cleared."));
	OnHistoryChanged.Broadcast(EStageHistoryChange::Cleared, FText::GetEmpty());
}

EStageCommandResult UStageEditHistorySubsystem::Undo(IStageItemEditor& Editor)
{
	return ApplyStep(/*bUndo*/ true, Editor);
}

EStageCommandResult UStageEditHistorySubsystem::Redo(IStageItemEditor& Editor)
{
	return ApplyStep(/*bUndo*/ false, Editor);
}

EStageCommandResult UStageEditHistorySubsystem::ApplyStep(bool bUndo, IStageItemEditor& Editor)
{
	const FText Description = bUndo ? History.GetUndoDescription() : History.GetRedoDescription();
	const EStageCommandResult Result = bUndo ? History.Undo(Editor) : History.Redo(Editor);

	UE_LOG(LogStageCraft, Log, TEXT("History: %s \"%s\" -> %s (%d undo / %d redo)."), bUndo ? TEXT("undo") : TEXT("redo"),
		*Description.ToString(), *UEnum::GetValueAsString(Result), History.GetUndoCount(), History.GetRedoCount());

	switch (Result)
	{
	case EStageCommandResult::Succeeded:
		OnHistoryChanged.Broadcast(bUndo ? EStageHistoryChange::Undone : EStageHistoryChange::Redone, Description);
		break;
	case EStageCommandResult::Invalid:
		OnHistoryChanged.Broadcast(EStageHistoryChange::Cleared, Description);
		break;
	case EStageCommandResult::Refused:
	case EStageCommandResult::NothingToDo:
		break;
	}
	return Result;
}
