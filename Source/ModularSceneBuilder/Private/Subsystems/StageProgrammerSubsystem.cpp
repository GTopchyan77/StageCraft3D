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
	Entries.Reset();
	Highlighted.Reset();
	Effects.Reset();
	OnFixturesChanged.Clear();
	OnEffectsChanged.Clear();
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

FShowCueFixtureState UStageProgrammerSubsystem::SanitizeEntry(const FShowCueFixtureState& InEntry)
{
	FShowCueFixtureState Entry;
	Entry.FixtureId = InEntry.FixtureId;
	Entry.Attributes = SanitizeValues(InEntry.Attributes);
	Entry.PaletteRefs = InEntry.PaletteRefs;
	StagePaletteRules::SanitizeReferences(Entry);
	return Entry;
}

void UStageProgrammerSubsystem::BroadcastValues(TConstArrayView<int32> FixtureIds)
{
	OnFixturesChanged.Broadcast(FixtureIds);
	OnValuesChanged.Broadcast();
}

void UStageProgrammerSubsystem::BroadcastEffects()
{
	OnEffectsChanged.Broadcast();
	OnValuesChanged.Broadcast();
}

const FStageEffectEntry* UStageProgrammerSubsystem::FindEffect(const FGuid& EffectId) const
{
	return Effects.FindByPredicate([&EffectId](const FStageEffectEntry& Entry) { return Entry.Id == EffectId; });
}

FGuid UStageProgrammerSubsystem::AddEffect(const FStageEffect& Effect, TConstArrayView<int32> FixtureIds)
{
	if (Effects.Num() >= StageEffectRules::MaxEffects)
	{
		return FGuid();
	}
	TArray<FStageEffectEntry> Candidate;
	FStageEffectEntry& Entry = Candidate.AddDefaulted_GetRef();
	Entry.Id = FGuid::NewGuid();
	Entry.Effect = Effect;
	Entry.FixtureIds = TArray<int32>(FixtureIds);
	StageEffectRules::SanitizeEntries(Candidate);
	if (Candidate.IsEmpty())
	{
		return FGuid();
	}
	const FGuid Id = Candidate[0].Id;
	Effects.Add(MoveTemp(Candidate[0]));
	BroadcastEffects();
	return Id;
}

bool UStageProgrammerSubsystem::UpdateEffect(const FGuid& EffectId, const FStageEffect& Effect)
{
	FStageEffectEntry* Entry = Effects.FindByPredicate([&EffectId](const FStageEffectEntry& Candidate) { return Candidate.Id == EffectId; });
	const FStageEffect Sanitized = StageEffectRules::SanitizeEffect(Effect);
	if (!Entry || Entry->Effect.Equals(Sanitized))
	{
		return false;
	}
	Entry->Effect = Sanitized;
	BroadcastEffects();
	return true;
}

bool UStageProgrammerSubsystem::RemoveEffect(const FGuid& EffectId)
{
	if (Effects.RemoveAll([&EffectId](const FStageEffectEntry& Entry) { return Entry.Id == EffectId; }) == 0)
	{
		return false;
	}
	BroadcastEffects();
	return true;
}

void UStageProgrammerSubsystem::SetEffects(TArray<FStageEffectEntry> InEffects)
{
	const int32 Dropped = StageEffectRules::SanitizeEntries(InEffects);
	UE_CLOG(Dropped > 0, LogStageCraft, Warning, TEXT("Programmer: dropped %d invalid effect entries or fixture IDs."), Dropped);
	if (AreEffectsEqual(Effects, InEffects))
	{
		return;
	}
	Effects = MoveTemp(InEffects);
	BroadcastEffects();
}

bool UStageProgrammerSubsystem::AreEffectsEqual(TConstArrayView<FStageEffectEntry> A, TConstArrayView<FStageEffectEntry> B)
{
	if (A.Num() != B.Num())
	{
		return false;
	}
	for (int32 Index = 0; Index < A.Num(); ++Index)
	{
		if (A[Index].Id != B[Index].Id || A[Index].FixtureIds != B[Index].FixtureIds || !A[Index].Effect.Equals(B[Index].Effect))
		{
			return false;
		}
	}
	return true;
}

const TMap<FGameplayTag, float>* UStageProgrammerSubsystem::FindValues(int32 FixtureId) const
{
	const FShowCueFixtureState* Entry = Entries.Find(FixtureId);
	return Entry ? &Entry->Attributes : nullptr;
}

const TMap<FGameplayTag, FGuid>* UStageProgrammerSubsystem::FindReferences(int32 FixtureId) const
{
	const FShowCueFixtureState* Entry = Entries.Find(FixtureId);
	return Entry && !Entry->PaletteRefs.IsEmpty() ? &Entry->PaletteRefs : nullptr;
}

void UStageProgrammerSubsystem::MergeValues(int32 FixtureId, const TMap<FGameplayTag, float>& InValues, const FGuid& PaletteId)
{
	const TMap<FGameplayTag, float> Sanitized = SanitizeValues(InValues);
	if (FixtureId <= 0 || Sanitized.IsEmpty() || (!Entries.Contains(FixtureId) && Entries.Num() >= MaxFixtures))
	{
		return;
	}
	FShowCueFixtureState& Entry = Entries.FindOrAdd(FixtureId);
	Entry.FixtureId = FixtureId;
	for (const TPair<FGameplayTag, float>& Pair : Sanitized)
	{
		if (!Entry.Attributes.Contains(Pair.Key) && Entry.Attributes.Num() >= MaxAttributesPerFixture)
		{
			continue;
		}
		Entry.Attributes.Add(Pair.Key, Pair.Value);
		// The latest edit of an attribute decides what it follows: a palette, or nothing (a plain value).
		if (PaletteId.IsValid())
		{
			Entry.PaletteRefs.Add(Pair.Key, PaletteId);
		}
		else
		{
			Entry.PaletteRefs.Remove(Pair.Key);
		}
	}
	const int32 Changed[] = { FixtureId };
	BroadcastValues(Changed);
}

void UStageProgrammerSubsystem::SetValues(int32 FixtureId, const TMap<FGameplayTag, float>& InValues)
{
	MergeValues(FixtureId, InValues, FGuid());
}

void UStageProgrammerSubsystem::SetReferencedValues(int32 FixtureId, const TMap<FGameplayTag, float>& InValues, const FGuid& PaletteId)
{
	MergeValues(FixtureId, InValues, PaletteId);
}

void UStageProgrammerSubsystem::ClearFixtures(TConstArrayView<int32> FixtureIds)
{
	TArray<int32> Cleared;
	for (const int32 FixtureId : FixtureIds)
	{
		if (Entries.Remove(FixtureId) > 0)
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
	Entries.GetKeys(Cleared);
	ClearFixtures(Cleared);
}

TArray<FShowCueFixtureState> UStageProgrammerSubsystem::CaptureFixtures(TConstArrayView<int32> FixtureIds) const
{
	TArray<FShowCueFixtureState> Captured;
	for (const int32 FixtureId : FixtureIds)
	{
		if (const FShowCueFixtureState* Entry = Entries.Find(FixtureId))
		{
			Captured.Add(*Entry);
		}
	}
	return Captured;
}

void UStageProgrammerSubsystem::RestoreFixtures(TConstArrayView<int32> FixtureIds, TConstArrayView<FShowCueFixtureState> InEntries)
{
	TArray<int32> Changed;
	for (const int32 FixtureId : FixtureIds)
	{
		Entries.Remove(FixtureId);
		Changed.AddUnique(FixtureId);
	}
	for (const FShowCueFixtureState& InEntry : InEntries)
	{
		// Only the fixtures the step names: an entry for any other fixture would be a malformed step, not part of the edit.
		FShowCueFixtureState Entry = SanitizeEntry(InEntry);
		if (Entry.FixtureId > 0 && FixtureIds.Contains(Entry.FixtureId) && !Entry.Attributes.IsEmpty() && Entries.Num() < MaxFixtures)
		{
			Entries.Add(Entry.FixtureId, MoveTemp(Entry));
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
	Entries.GetKeys(FixtureIds);
	FixtureIds.Sort();
	return CaptureFixtures(FixtureIds);
}

void UStageProgrammerSubsystem::ReplaceAll(TConstArrayView<FShowCueFixtureState> InEntries)
{
	TArray<int32> Changed;
	Entries.GetKeys(Changed);
	Entries.Reset();

	int32 Dropped = 0;
	for (const FShowCueFixtureState& InEntry : InEntries)
	{
		FShowCueFixtureState Entry = SanitizeEntry(InEntry);
		if (Entry.FixtureId <= 0 || Entry.Attributes.IsEmpty() || Entries.Contains(Entry.FixtureId) || Entries.Num() >= MaxFixtures)
		{
			++Dropped;
			continue;
		}
		Changed.AddUnique(Entry.FixtureId);
		Entries.Add(Entry.FixtureId, MoveTemp(Entry));
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
