// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "History/StageEditCommand.h"
#include "Show/ShowControlTypes.h"

/**
 * The cue list changed (store, update, delete). Holds the whole list before and after, so every kind of cue edit has an exact
 * inverse (Docs/ADR/0011-cue-list-and-patch-sheet-export.md §2.4). Playback is not part of the history.
 * Pure logic against IStageItemEditor; covered by the StageCraft.History.Cues test.
 */
class MODULARSCENEBUILDER_API FStageCueListCommand final : public IStageEditCommand
{
public:
	/** Description is user-facing, e.g. "Store Cue 3". */
	FStageCueListCommand(const FText& InDescription, TArray<FShowCue> InBefore, TArray<FShowCue> InAfter);

	virtual FText GetDescription() const override { return Description; }
	virtual EStageCommandResult Undo(IStageItemEditor& Editor) override;
	virtual EStageCommandResult Redo(IStageItemEditor& Editor) override;

private:
	FText Description;
	TArray<FShowCue> Before;
	TArray<FShowCue> After;
};
