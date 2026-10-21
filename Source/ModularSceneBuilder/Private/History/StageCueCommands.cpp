// Copyright Epic Games, Inc. All Rights Reserved.

#include "History/StageCueCommands.h"

FStageCueListCommand::FStageCueListCommand(const FText& InDescription, TArray<FShowCue> InBefore, TArray<FShowCue> InAfter)
	: Description(InDescription)
	, Before(MoveTemp(InBefore))
	, After(MoveTemp(InAfter))
{
}

EStageCommandResult FStageCueListCommand::Undo(IStageItemEditor& Editor)
{
	return Editor.SetCueList(Before);
}

EStageCommandResult FStageCueListCommand::Redo(IStageItemEditor& Editor)
{
	return Editor.SetCueList(After);
}
