// Copyright Epic Games, Inc. All Rights Reserved.

#include "Subsystems/StageProgrammerSubsystem.h"

#include "Data/StageParameterTypes.h"
#include "Engine/World.h"
#include "ModularSceneBuilder.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageProgrammerSubsystem)

bool UStageProgrammerSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// A programmer belongs to a stage being played, like the show control it feeds.
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UStageProgrammerSubsystem::Deinitialize()
{
	Values.Reset();
	Highlighted.Reset();
	OnFixturesChanged.Clear();
	Super::Deinitialize();
}

TMap<FGameplayTag, float> UStageProgrammerSubsystem::SanitizeValues(const TMap<FGameplayTag, float>& InValues)
{
	TMap<FGameplayTag, float> Kept;
	for (const TPair<FGameplayTag, float>& Pair : InValues)
	{
		if (Kept.Num() >= MaxAttributesPerFixture)
		{
			break;
		}
		if (Pair.Key.MatchesTag(StageCraftTags::Attribute) && Pair.Key != StageCraftTags::Attribute && FMath::IsFinite(Pair.Value))
		{
			Kept.Add(Pair.Key, Pair.Value);
		}
	}
	return Kept;
}

void UStageProgrammerSubsystem::BroadcastValues(TConstArrayView<int32> FixtureIds)
{
	OnFixturesChanged.Broadcast(FixtureIds);
	OnValuesChanged.Broadcast();
}

void UStageProgrammerSubsystem::SetValues(int32 FixtureId, const TMap<FGameplayTag, float>& InValues)
{
	const TMap<FGameplayTag, float> Sanitized = SanitizeValues(InValues);
	if (FixtureId <= 0 || Sanitized.IsEmpty() || (!Values.Contains(FixtureId) && Values.Num() >= MaxFixtures))
	{
		return;
	}
	TMap<FGameplayTag, float>& Fixture = Values.FindOrAdd(FixtureId);
	for (const TPair<FGameplayTag, float>& Pair : Sanitized)
	{
		if (Fixture.Contains(Pair.Key) || Fixture.Num() < MaxAttributesPerFixture)
		{
			Fixture.Add(Pair.Key, Pair.Value);
		}
	}
	const int32 Changed[] = { FixtureId };
	BroadcastValues(Changed);
}

void UStageProgrammerSubsystem::ClearFixtures(TConstArrayView<int32> FixtureIds)
{
	TArray<int32> Cleared;
	for (const int32 FixtureId : FixtureIds)
	{
		if (Values.Remove(FixtureId) > 0)
		{
			Cleared.Add(FixtureId);
		}
	}
	if (!Cleared.IsEmpty())
	{
		BroadcastValues(Cleared);
	}
}

void UStageProgrammerSubsystem::ClearAll()
{
	TArray<int32> Cleared;
	Values.GetKeys(Cleared);
	ClearFixtures(Cleared);
}

TArray<FShowCueFixtureState> UStageProgrammerSubsystem::CaptureFixtures(TConstArrayView<int32> FixtureIds) const
{
	TArray<FShowCueFixtureState> Entries;
	for (const int32 FixtureId : FixtureIds)
	{
		if (const TMap<FGameplayTag, float>* Fixture = Values.Find(FixtureId))
		{
			FShowCueFixtureState& Entry = Entries.AddDefaulted_GetRef();
			Entry.FixtureId = FixtureId;
			Entry.Attributes = *Fixture;
		}
	}
	return Entries;
}

void UStageProgrammerSubsystem::RestoreFixtures(TConstArrayView<int32> FixtureIds, TConstArrayView<FShowCueFixtureState> Entries)
{
	TArray<int32> Changed;
	for (const int32 FixtureId : FixtureIds)
	{
		Values.Remove(FixtureId);
		Changed.AddUnique(FixtureId);
	}
	for (const FShowCueFixtureState& Entry : Entries)
	{
		// Only the fixtures the step names: an entry for any other fixture would be a malformed step, not part of the edit.
		const TMap<FGameplayTag, float> Sanitized = SanitizeValues(Entry.Attributes);
		if (Entry.FixtureId > 0 && FixtureIds.Contains(Entry.FixtureId) && !Sanitized.IsEmpty() && Values.Num() < MaxFixtures)
		{
			Values.Add(Entry.FixtureId, Sanitized);
		}
	}
	if (!Changed.IsEmpty())
	{
		BroadcastValues(Changed);
	}
}

TArray<FShowCueFixtureState> UStageProgrammerSubsystem::GetEntries() const
{
	TArray<int32> FixtureIds;
	Values.GetKeys(FixtureIds);
	FixtureIds.Sort();
	return CaptureFixtures(FixtureIds);
}

void UStageProgrammerSubsystem::ReplaceAll(TConstArrayView<FShowCueFixtureState> Entries)
{
	TArray<int32> Changed;
	Values.GetKeys(Changed);
	Values.Reset();

	int32 Dropped = 0;
	for (const FShowCueFixtureState& Entry : Entries)
	{
		const TMap<FGameplayTag, float> Sanitized = SanitizeValues(Entry.Attributes);
		if (Entry.FixtureId <= 0 || Sanitized.IsEmpty() || Values.Contains(Entry.FixtureId) || Values.Num() >= MaxFixtures)
		{
			++Dropped;
			continue;
		}
		Values.Add(Entry.FixtureId, Sanitized);
		Changed.AddUnique(Entry.FixtureId);
	}
	if (Dropped > 0)
	{
		UE_LOG(LogStageCraft, Warning, TEXT("Programmer: %d entr%s without a valid fixture ID or value were dropped."), Dropped, Dropped == 1 ? TEXT("y") : TEXT("ies"));
	}
	if (!Changed.IsEmpty())
	{
		BroadcastValues(Changed);
	}
}

void UStageProgrammerSubsystem::SetHighlightedFixtures(TConstArrayView<int32> FixtureIds)
{
	TSet<int32> NewSet;
	for (const int32 FixtureId : FixtureIds)
	{
		if (FixtureId > 0)
		{
			NewSet.Add(FixtureId);
		}
	}
	// Both the fixtures that leave the highlight and the ones that join it are re-mixed.
	TArray<int32> Changed = Highlighted.Difference(NewSet).Array();
	Changed.Append(NewSet.Difference(Highlighted).Array());
	Highlighted = MoveTemp(NewSet);
	if (!Changed.IsEmpty())
	{
		OnFixturesChanged.Broadcast(Changed);
	}
}
