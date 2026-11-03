// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "History/StageEditCommand.h"
#include "Show/ShowControlTypes.h"

/**
 * Programmer values changed for some fixtures (an edit from the inspector, the fader bank or the assistant, a Clear, a cue store that
 * took them in). Holds those fixtures' values before and after, so the step has an exact inverse; a fixture without an entry had no
 * values (Docs/ADR/0025-programmer-and-output-mix.md §3). Pure logic against IStageItemEditor; covered by StageCraft.Show.Programmer.
 */
class MODULARSCENEBUILDER_API FStageProgrammerCommand final : public IStageEditCommand
{
public:
	FStageProgrammerCommand(const FText& InDescription, TArray<int32> InFixtureIds, TArray<FShowCueFixtureState> InBefore, TArray<FShowCueFixtureState> InAfter);

	/** False when Before and After hold the same values, so recording it would add a step that does nothing. */
	static bool IsMeaningfulChange(const TArray<FShowCueFixtureState>& Before, const TArray<FShowCueFixtureState>& After);

	virtual FText GetDescription() const override { return Description; }
	virtual EStageCommandResult Undo(IStageItemEditor& Editor) override;
	virtual EStageCommandResult Redo(IStageItemEditor& Editor) override;

private:
	FText Description;
	TArray<int32> FixtureIds;
	TArray<FShowCueFixtureState> Before;
	TArray<FShowCueFixtureState> After;
};

/** The fixture groups changed (record, update, rename, delete). Whole list before and after, like cue steps (Docs/ADR/0025 §5). */
class MODULARSCENEBUILDER_API FStageFixtureGroupsCommand final : public IStageEditCommand
{
public:
	FStageFixtureGroupsCommand(const FText& InDescription, TArray<FStageFixtureGroup> InBefore, TArray<FStageFixtureGroup> InAfter);

	virtual FText GetDescription() const override { return Description; }
	virtual EStageCommandResult Undo(IStageItemEditor& Editor) override;
	virtual EStageCommandResult Redo(IStageItemEditor& Editor) override;

private:
	FText Description;
	TArray<FStageFixtureGroup> Before;
	TArray<FStageFixtureGroup> After;
};
