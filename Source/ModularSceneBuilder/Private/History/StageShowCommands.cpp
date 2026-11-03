// Copyright Epic Games, Inc. All Rights Reserved.

#include "History/StageShowCommands.h"

namespace
{
	/** The entries as one map per fixture ID, for an order-independent comparison. */
	TMap<int32, const TMap<FGameplayTag, float>*> ByFixture(const TArray<FShowCueFixtureState>& Entries)
	{
		TMap<int32, const TMap<FGameplayTag, float>*> Map;
		for (const FShowCueFixtureState& Entry : Entries)
		{
			Map.Add(Entry.FixtureId, &Entry.Attributes);
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
	const TMap<int32, const TMap<FGameplayTag, float>*> BeforeMap = ByFixture(InBefore);
	const TMap<int32, const TMap<FGameplayTag, float>*> AfterMap = ByFixture(InAfter);
	if (BeforeMap.Num() != AfterMap.Num())
	{
		return true;
	}
	for (const TPair<int32, const TMap<FGameplayTag, float>*>& Pair : BeforeMap)
	{
		const TMap<FGameplayTag, float>* const* Other = AfterMap.Find(Pair.Key);
		if (!Other || !AreSameValues(*Pair.Value, **Other))
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
