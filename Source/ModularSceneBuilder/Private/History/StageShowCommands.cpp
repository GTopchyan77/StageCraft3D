// Copyright Epic Games, Inc. All Rights Reserved.

#include "History/StageShowCommands.h"

namespace
{
	/** The entries by fixture ID, for an order-independent comparison. */
	TMap<int32, const FShowCueFixtureState*> ByFixture(const TArray<FShowCueFixtureState>& Entries)
	{
		TMap<int32, const FShowCueFixtureState*> Map;
		for (const FShowCueFixtureState& Entry : Entries)
		{
			Map.Add(Entry.FixtureId, &Entry);
		}
		return Map;
	}

	bool AreSameValues(const TMap<FGameplayTag, float>& A, const TMap<FGameplayTag, float>& B)
	{
		if (A.Num() != B.Num())
		{
			return false;
		}
		for (const TPair<FGameplayTag, float>& Pair : A)
		{
			const float* Other = B.Find(Pair.Key);
			if (!Other || !FMath::IsNearlyEqual(*Other, Pair.Value))
			{
				return false;
			}
		}
		return true;
	}

	/** Same values and same palette references (applying a palette over its own values still changes what the fixture follows). */
	bool AreSameEntries(const FShowCueFixtureState& A, const FShowCueFixtureState& B)
	{
		return AreSameValues(A.Attributes, B.Attributes) && A.PaletteRefs.OrderIndependentCompareEqual(B.PaletteRefs);
	}
}

FStageProgrammerCommand::FStageProgrammerCommand(const FText& InDescription, TArray<int32> InFixtureIds, TArray<FShowCueFixtureState> InBefore, TArray<FShowCueFixtureState> InAfter)
	: Description(InDescription)
	, FixtureIds(MoveTemp(InFixtureIds))
	, Before(MoveTemp(InBefore))
	, After(MoveTemp(InAfter))
{
}

bool FStageProgrammerCommand::IsMeaningfulChange(const TArray<FShowCueFixtureState>& InBefore, const TArray<FShowCueFixtureState>& InAfter)
{
	const TMap<int32, const FShowCueFixtureState*> BeforeMap = ByFixture(InBefore);
	const TMap<int32, const FShowCueFixtureState*> AfterMap = ByFixture(InAfter);
	if (BeforeMap.Num() != AfterMap.Num())
	{
		return true;
	}
	for (const TPair<int32, const FShowCueFixtureState*>& Pair : BeforeMap)
	{
		const FShowCueFixtureState* const* Other = AfterMap.Find(Pair.Key);
		if (!Other || !AreSameEntries(*Pair.Value, **Other))
		{
			return true;
		}
	}
	return false;
}

EStageCommandResult FStageProgrammerCommand::Undo(IStageItemEditor& Editor)
{
	return Editor.SetProgrammerValues(FixtureIds, Before);
}

EStageCommandResult FStageProgrammerCommand::Redo(IStageItemEditor& Editor)
{
	return Editor.SetProgrammerValues(FixtureIds, After);
}

FStageFixtureGroupsCommand::FStageFixtureGroupsCommand(const FText& InDescription, TArray<FStageFixtureGroup> InBefore, TArray<FStageFixtureGroup> InAfter)
	: Description(InDescription)
	, Before(MoveTemp(InBefore))
	, After(MoveTemp(InAfter))
{
}

EStageCommandResult FStageFixtureGroupsCommand::Undo(IStageItemEditor& Editor)
{
	return Editor.SetFixtureGroups(Before);
}

EStageCommandResult FStageFixtureGroupsCommand::Redo(IStageItemEditor& Editor)
{
	return Editor.SetFixtureGroups(After);
}

FStagePlaybacksCommand::FStagePlaybacksCommand(const FText& InDescription, TArray<FStagePlayback> InBefore, TArray<FStagePlayback> InAfter)
	: Description(InDescription)
	, Before(MoveTemp(InBefore))
	, After(MoveTemp(InAfter))
{
}

EStageCommandResult FStagePlaybacksCommand::Undo(IStageItemEditor& Editor)
{
	return Editor.SetPlaybacks(Before);
}

EStageCommandResult FStagePlaybacksCommand::Redo(IStageItemEditor& Editor)
{
	return Editor.SetPlaybacks(After);
}

FStagePalettesCommand::FStagePalettesCommand(const FText& InDescription, TArray<FStagePalette> InBefore, TArray<FStagePalette> InAfter)
	: Description(InDescription)
	, Before(MoveTemp(InBefore))
	, After(MoveTemp(InAfter))
{
}

EStageCommandResult FStagePalettesCommand::Undo(IStageItemEditor& Editor)
{
	return Editor.SetPalettes(Before);
}

EStageCommandResult FStagePalettesCommand::Redo(IStageItemEditor& Editor)
{
	return Editor.SetPalettes(After);
}

FStageProgrammerEffectsCommand::FStageProgrammerEffectsCommand(const FText& InDescription, TArray<FStageEffectEntry> InBefore, TArray<FStageEffectEntry> InAfter)
	: Description(InDescription)
	, Before(MoveTemp(InBefore))
	, After(MoveTemp(InAfter))
{
}

EStageCommandResult FStageProgrammerEffectsCommand::Undo(IStageItemEditor& Editor)
{
	return Editor.SetProgrammerEffects(Before);
}

EStageCommandResult FStageProgrammerEffectsCommand::Redo(IStageItemEditor& Editor)
{
	return Editor.SetProgrammerEffects(After);
}
