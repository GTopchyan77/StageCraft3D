// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Environment/StageEnvironmentTypes.h"
#include "History/StageEditCommand.h"

/**
 * The outdoor environment changed (an assistant weather, time or sky request). Holds the whole settings before and after, so
 * the step has an exact inverse (Docs/ADR/0016-ai-assistant.md §2.4). Undo restores everything as it was before the step,
 * including edits made in the Environment panel since, as cue list steps do. Pure logic against IStageItemEditor; covered by
 * the StageCraft.History.Environment test.
 */
class MODULARSCENEBUILDER_API FStageEnvironmentCommand final : public IStageEditCommand
{
public:
	/** Description is user-facing, e.g. "Assistant: weather rain". */
	FStageEnvironmentCommand(const FText& InDescription, const FStageEnvironmentSettings& InBefore, const FStageEnvironmentSettings& InAfter);

	virtual FText GetDescription() const override { return Description; }
	virtual EStageCommandResult Undo(IStageItemEditor& Editor) override;
	virtual EStageCommandResult Redo(IStageItemEditor& Editor) override;

private:
	FText Description;
	FStageEnvironmentSettings Before;
	FStageEnvironmentSettings After;
};
