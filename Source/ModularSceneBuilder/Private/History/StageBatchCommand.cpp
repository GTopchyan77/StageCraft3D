// Copyright Epic Games, Inc. All Rights Reserved.

#include "History/StageBatchCommand.h"

#include "ModularSceneBuilder.h"

namespace StageBatchCommand
{
	/** Runs one direction over Commands in order; on a failure, reverts the ones already applied. */
	EStageCommandResult RunAllOrNothing(TConstArrayView<TSharedRef<IStageEditCommand>> Commands, IStageItemEditor& Editor, bool bUndo, const FText& Description)
	{
		const int32 Count = Commands.Num();
		for (int32 Step = 0; Step < Count; ++Step)
		{
			// Undo walks newest first, redo oldest first: each is the mirror of how the step was built.
			const int32 Index = bUndo ? Count - 1 - Step : Step;
			IStageEditCommand& Command = *Commands[Index];
			const EStageCommandResult Result = bUndo ? Command.Undo(Editor) : Command.Redo(Editor);
			if (Result == EStageCommandResult::Succeeded)
			{
				continue;
			}

			bool bRolledBack = true;
			for (int32 Back = Step - 1; Back >= 0; --Back)
			{
				const int32 AppliedIndex = bUndo ? Count - 1 - Back : Back;
				IStageEditCommand& Applied = *Commands[AppliedIndex];
				bRolledBack &= (bUndo ? Applied.Redo(Editor) : Applied.Undo(Editor)) == EStageCommandResult::Succeeded;
			}
			if (!bRolledBack)
			{
				UE_LOG(LogStageCraft, Error, TEXT("History: \"%s\" failed part-way (%s) and could not be fully reverted; the step is dropped."),
					*Description.ToString(), *UEnum::GetValueAsString(Result));
				return EStageCommandResult::Invalid;
			}
			return Result;
		}
		return EStageCommandResult::Succeeded;
	}
}

FStageBatchCommand::FStageBatchCommand(FText InDescription, TArray<TSharedRef<IStageEditCommand>> InCommands)
	: Description(MoveTemp(InDescription))
	, Commands(MoveTemp(InCommands))
{
	ensureMsgf(!Commands.IsEmpty(), TEXT("FStageBatchCommand \"%s\" was built without commands."), *Description.ToString());
}

EStageCommandResult FStageBatchCommand::Undo(IStageItemEditor& Editor)
{
	return StageBatchCommand::RunAllOrNothing(Commands, Editor, /*bUndo*/ true, Description);
}

EStageCommandResult FStageBatchCommand::Redo(IStageItemEditor& Editor)
{
	return StageBatchCommand::RunAllOrNothing(Commands, Editor, /*bUndo*/ false, Description);
}
