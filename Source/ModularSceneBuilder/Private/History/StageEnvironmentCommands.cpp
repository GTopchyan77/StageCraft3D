// Copyright Epic Games, Inc. All Rights Reserved.

#include "History/StageEnvironmentCommands.h"

FStageEnvironmentCommand::FStageEnvironmentCommand(const FText& InDescription, const FStageEnvironmentSettings& InBefore, const FStageEnvironmentSettings& InAfter)
	: Description(InDescription)
	, Before(InBefore)
	, After(InAfter)
{
}

EStageCommandResult FStageEnvironmentCommand::Undo(IStageItemEditor& Editor)
{
	return Editor.SetEnvironment(Before);
}

EStageCommandResult FStageEnvironmentCommand::Redo(IStageItemEditor& Editor)
{
	return Editor.SetEnvironment(After);
}
