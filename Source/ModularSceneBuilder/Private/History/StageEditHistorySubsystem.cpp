// Copyright Epic Games, Inc. All Rights Reserved.

#include "History/StageEditHistorySubsystem.h"

#include "Engine/World.h"
#include "History/StageBatchCommand.h"
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
	Group.Reset();
	OnHistoryChanged.Clear();
	Super::Deinitialize();
}

void UStageEditHistorySubsystem::Record(TSharedRef<IStageEditCommand> Command)
{
	if (Group.IsSet())
	{
		UE_LOG(LogStageCraft, Verbose, TEXT("History: \"%s\" collected into \"%s\"."), *Command->GetDescription().ToString(), *Group->Description.ToString());
		Group->Commands.Add(MoveTemp(Command));
		return;
	}
	const FText Description = Command->GetDescription();
	if (History.Record(MoveTemp(Command)))
	{
		UE_LOG(LogStageCraft, Verbose, TEXT("History: recorded \"%s\" (%d undo steps)."), *Description.ToString(), History.GetUndoCount());
		OnHistoryChanged.Broadcast(EStageHistoryChange::Recorded, Description);
	}
}

bool UStageEditHistorySubsystem::BeginGroup(const FText& Description)
{
	if (Group.IsSet())
	{
		return false;
	}
	Group.Emplace();
	Group->Description = Description;
	return true;
}

TSharedRef<IStageEditCommand> UStageEditHistorySubsystem::MakeGroupCommand(FGroup&& Closed)
{
	check(!Closed.Commands.IsEmpty());
	if (Closed.Commands.Num() == 1)
	{
		return MoveTemp(Closed.Commands[0]);
	}
	return MakeShared<FStageBatchCommand>(MoveTemp(Closed.Description), MoveTemp(Closed.Commands));
}

void UStageEditHistorySubsystem::EndGroup()
{
	if (!Group.IsSet())
	{
		return;
	}
	FGroup Closed = MoveTemp(*Group);
	Group.Reset();
	if (!Closed.Commands.IsEmpty())
	{
		Record(MakeGroupCommand(MoveTemp(Closed)));
	}
}

EStageCommandResult UStageEditHistorySubsystem::AbortGroup(IStageItemEditor& Editor)
{
	if (!Group.IsSet())
	{
		return EStageCommandResult::NothingToDo;
	}
	FGroup Closed = MoveTemp(*Group);
	Group.Reset();
	if (Closed.Commands.IsEmpty())
	{
		return EStageCommandResult::Succeeded;
	}
	const FText Description = Closed.Description;
	const int32 Count = Closed.Commands.Num();
	const TSharedRef<IStageEditCommand> Collected = MakeGroupCommand(MoveTemp(Closed));
	const EStageCommandResult Result = Collected->Undo(Editor);
	if (Result == EStageCommandResult::Succeeded)
	{
		UE_LOG(LogStageCraft, Log, TEXT("History: \"%s\" rolled back (%d steps)."), *Description.ToString(), Count);
		return Result;
	}
	// The stage still holds what was done: keep it undoable rather than lose track of it.
	UE_LOG(LogStageCraft, Warning, TEXT("History: \"%s\" could not be rolled back (%s); kept as one step."), *Description.ToString(), *UEnum::GetValueAsString(Result));
	if (Result != EStageCommandResult::Invalid)
	{
		Record(Collected);
	}
	return Result;
}

void UStageEditHistorySubsystem::ClearHistory()
{
	// A scene load replaces the stage the open group's steps describe.
	Group.Reset();
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
	if (Group.IsSet())
	{
		UE_LOG(LogStageCraft, Log, TEXT("History: %s refused while \"%s\" is being applied."), bUndo ? TEXT("undo") : TEXT("redo"), *Group->Description.ToString());
		return EStageCommandResult::Refused;
	}
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
