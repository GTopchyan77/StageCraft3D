// Copyright Epic Games, Inc. All Rights Reserved.

#include "Show/StageCueTracking.h"

namespace StageCueTracking
{
	void ApplyEntry(FShowCueFixtureState& Into, const FShowCueFixtureState& Entry)
	{
		for (const TPair<FGameplayTag, float>& Pair : Entry.Attributes)
		{
			Into.Attributes.Add(Pair.Key, Pair.Value);
			if (const FGuid* Reference = Entry.PaletteRefs.Find(Pair.Key))
			{
				Into.PaletteRefs.Add(Pair.Key, *Reference);
			}
			else
			{
				Into.PaletteRefs.Remove(Pair.Key);
			}
		}
	}

	void StateAt(TConstArrayView<FShowCue> Cues, int32 Index, FFixtureStates& OutState)
	{
		OutState.Reset();
		if (!Cues.IsValidIndex(Index))
		{
			return;
		}
		// From the nearest block: nothing before it reaches this cue, so a list of blocked (snapshot) cues costs one cue.
		int32 First = Index;
		while (First > 0 && !Cues[First].bBlock)
		{
			--First;
		}
		for (int32 CueIndex = First; CueIndex <= Index; ++CueIndex)
		{
			for (const FShowCueFixtureState& Entry : Cues[CueIndex].Fixtures)
			{
				FShowCueFixtureState& State = OutState.FindOrAdd(Entry.FixtureId);
				State.FixtureId = Entry.FixtureId;
				ApplyEntry(State, Entry);
			}
		}
	}

	void MergeInto(FShowCue& Cue, TConstArrayView<FShowCueFixtureState> Entries)
	{
		for (const FShowCueFixtureState& Entry : Entries)
		{
			FShowCueFixtureState* Existing = Cue.Fixtures.FindByPredicate([&Entry](const FShowCueFixtureState& State) { return State.FixtureId == Entry.FixtureId; });
			if (!Existing)
			{
				Existing = &Cue.Fixtures.AddDefaulted_GetRef();
				Existing->FixtureId = Entry.FixtureId;
			}
			ApplyEntry(*Existing, Entry);
		}
	}

	TArray<FShowCueFixtureState> ToEntries(const FFixtureStates& States)
	{
		TArray<FShowCueFixtureState> Entries;
		States.GenerateValueArray(Entries);
		Entries.Sort([](const FShowCueFixtureState& A, const FShowCueFixtureState& B) { return A.FixtureId < B.FixtureId; });
		return Entries;
	}

	int32 KeepNextCue(TArray<FShowCue>& Cues, int32 Index, const FFixtureStates& Before, TConstArrayView<FShowCueFixtureState> Entries,
		TFunctionRef<bool(int32, const FGameplayTag&, float&)> HomeValue)
	{
		if (!Cues.IsValidIndex(Index + 1) || Cues[Index + 1].bBlock)
		{
			return 0;
		}
		FShowCue& Next = Cues[Index + 1];
		int32 Written = 0;
		for (const FShowCueFixtureState& Entry : Entries)
		{
			FShowCueFixtureState Kept;
			Kept.FixtureId = Entry.FixtureId;
			const FShowCueFixtureState* Old = Before.Find(Entry.FixtureId);
			const FShowCueFixtureState* NextOwn = Next.Fixtures.FindByPredicate([&Entry](const FShowCueFixtureState& State) { return State.FixtureId == Entry.FixtureId; });
			for (const TPair<FGameplayTag, float>& Pair : Entry.Attributes)
			{
				if (NextOwn && NextOwn->Attributes.Contains(Pair.Key))
				{
					continue; // The next cue sets it itself: the change never reached it.
				}
				const float* OldValue = Old ? Old->Attributes.Find(Pair.Key) : nullptr;
				float Value = 0.f;
				if (OldValue)
				{
					Value = *OldValue;
					if (const FGuid* Reference = Old->PaletteRefs.Find(Pair.Key))
					{
						Kept.PaletteRefs.Add(Pair.Key, *Reference);
					}
				}
				else if (!HomeValue(Entry.FixtureId, Pair.Key, Value))
				{
					continue; // Nothing known to go back to (the fixture is not on stage).
				}
				Kept.Attributes.Add(Pair.Key, Value);
			}
			if (!Kept.Attributes.IsEmpty())
			{
				Written += Kept.Attributes.Num();
				const FShowCueFixtureState KeptEntries[] = { Kept };
				MergeInto(Next, KeptEntries);
			}
		}
		return Written;
	}

	void EffectsAt(TConstArrayView<FShowCue> Cues, int32 Index, TArray<FStageEffectEntry>& OutEffects)
	{
		OutEffects.Reset();
		if (!Cues.IsValidIndex(Index))
		{
			return;
		}
		int32 First = Index;
		while (First > 0 && !Cues[First].bBlock)
		{
			--First;
		}
		for (int32 CueIndex = First; CueIndex <= Index; ++CueIndex)
		{
			const FShowCue& Cue = Cues[CueIndex];
			for (const FGuid& Stop : Cue.EffectStops)
			{
				OutEffects.RemoveAll([&Stop](const FStageEffectEntry& Running) { return Running.Id == Stop; });
			}
			for (const FStageEffectEntry& Entry : Cue.Effects)
			{
				OutEffects.RemoveAll([&Entry](const FStageEffectEntry& Running) { return Running.Id == Entry.Id; });
				OutEffects.Add(Entry);
			}
		}
	}

	void MergeEffectsInto(FShowCue& Cue, TConstArrayView<FStageEffectEntry> Effects, TConstArrayView<FGuid> Stops)
	{
		for (const FStageEffectEntry& Entry : Effects)
		{
			Cue.EffectStops.Remove(Entry.Id);
			if (FStageEffectEntry* Existing = Cue.Effects.FindByPredicate([&Entry](const FStageEffectEntry& Own) { return Own.Id == Entry.Id; }))
			{
				*Existing = Entry;
			}
			else
			{
				Cue.Effects.Add(Entry);
			}
		}
		for (const FGuid& Stop : Stops)
		{
			Cue.Effects.RemoveAll([&Stop](const FStageEffectEntry& Own) { return Own.Id == Stop; });
			Cue.EffectStops.AddUnique(Stop);
		}
	}

	int32 KeepNextCueEffects(TArray<FShowCue>& Cues, int32 Index, TConstArrayView<FStageEffectEntry> Before,
		TConstArrayView<FStageEffectEntry> Effects, TConstArrayView<FGuid> Stops)
	{
		if (!Cues.IsValidIndex(Index + 1) || Cues[Index + 1].bBlock)
		{
			return 0;
		}
		FShowCue& Next = Cues[Index + 1];
		TArray<FGuid> Stored;
		for (const FStageEffectEntry& Entry : Effects)
		{
			Stored.AddUnique(Entry.Id);
		}
		for (const FGuid& Stop : Stops)
		{
			Stored.AddUnique(Stop);
		}

		TArray<FStageEffectEntry> KeptEffects;
		TArray<FGuid> KeptStops;
		for (const FGuid& Id : Stored)
		{
			const auto HasId = [&Id](const FStageEffectEntry& Entry) { return Entry.Id == Id; };
			if (Next.Effects.ContainsByPredicate(HasId) || Next.EffectStops.Contains(Id))
			{
				continue; // The next cue decides this effect itself: the change never reached it.
			}
			if (const FStageEffectEntry* Old = Before.FindByPredicate(HasId))
			{
				KeptEffects.Add(*Old);
			}
			else if (Effects.ContainsByPredicate(HasId))
			{
				KeptStops.Add(Id); // New at Index: it ends there.
			}
		}
		MergeEffectsInto(Next, KeptEffects, KeptStops);
		return KeptEffects.Num() + KeptStops.Num();
	}
}

namespace StageCueTiming
{
	void GetAttributeTiming(const FShowCue& Cue, const FGameplayTag& Attribute, bool bGoingDown, float& OutDelay, float& OutDuration)
	{
		GetFeatureTiming(Cue, StagePaletteRules::GetAttributeKind(Attribute), bGoingDown, OutDelay, OutDuration);
	}

	void GetFeatureTiming(const FShowCue& Cue, EStagePaletteKind Kind, bool bGoingDown, float& OutDelay, float& OutDuration)
	{
		const FStageCueTiming& Timing = Cue.Timing;
		OutDelay = Timing.Delay;
		if (Kind == EStagePaletteKind::Intensity && bGoingDown && Timing.bHasOutTime)
		{
			OutDuration = Timing.OutTime;
			return;
		}
		const float* FeatureTime = Timing.FeatureTimes.Find(Kind);
		OutDuration = FeatureTime ? *FeatureTime : Cue.FadeTime;
	}

	float GetProgress(float Elapsed, float Delay, float Duration)
	{
		if (Elapsed < Delay)
		{
			return 0.f;
		}
		return Duration > UE_KINDA_SMALL_NUMBER ? FMath::Clamp((Elapsed - Delay) / Duration, 0.f, 1.f) : 1.f;
	}

	float ApplyCurve(EStageCueCurve Curve, float Progress)
	{
		const float Clamped = FMath::Clamp(Progress, 0.f, 1.f);
		return Curve == EStageCueCurve::SCurve ? Clamped * Clamped * (3.f - 2.f * Clamped) : Clamped;
	}
}
