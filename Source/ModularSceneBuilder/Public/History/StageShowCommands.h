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

/** The playbacks' recorded content changed (record, clear, rename). All playbacks before and after (Docs/ADR/0026 §6); live state is not part of it. */
class MODULARSCENEBUILDER_API FStagePlaybacksCommand final : public IStageEditCommand
{
public:
	FStagePlaybacksCommand(const FText& InDescription, TArray<FStagePlayback> InBefore, TArray<FStagePlayback> InAfter);

	virtual FText GetDescription() const override { return Description; }
	virtual EStageCommandResult Undo(IStageItemEditor& Editor) override;
	virtual EStageCommandResult Redo(IStageItemEditor& Editor) override;

private:
	FText Description;
	TArray<FStagePlayback> Before;
	TArray<FStagePlayback> After;
};

/**
 * The palettes changed (record, update, rename, delete). Whole list before and after, like groups (Docs/ADR/0028-palettes-and-references.md
 * §4). A delete is batched with the cue and programmer steps that turned its references into values, so it undoes as one.
 */
class MODULARSCENEBUILDER_API FStagePalettesCommand final : public IStageEditCommand
{
public:
	FStagePalettesCommand(const FText& InDescription, TArray<FStagePalette> InBefore, TArray<FStagePalette> InAfter);

	virtual FText GetDescription() const override { return Description; }
	virtual EStageCommandResult Undo(IStageItemEditor& Editor) override;
	virtual EStageCommandResult Redo(IStageItemEditor& Editor) override;

private:
	FText Description;
	TArray<FStagePalette> Before;
	TArray<FStagePalette> After;
};

/**
 * The programmer's effects and stops changed (start, edit, stop, take, Clear, a store). Both before and after (Docs/ADR/0027-effects-engine.md §4); a
 * Clear is batched with the programmer's values so it undoes as one. Covered by StageCraft.Show.Effects.
 */
class MODULARSCENEBUILDER_API FStageProgrammerEffectsCommand final : public IStageEditCommand
{
public:
	FStageProgrammerEffectsCommand(const FText& InDescription, FStageProgrammerEffects InBefore, FStageProgrammerEffects InAfter);

	virtual FText GetDescription() const override { return Description; }
	virtual EStageCommandResult Undo(IStageItemEditor& Editor) override;
	virtual EStageCommandResult Redo(IStageItemEditor& Editor) override;

private:
	FText Description;
	FStageProgrammerEffects Before;
	FStageProgrammerEffects After;
};
