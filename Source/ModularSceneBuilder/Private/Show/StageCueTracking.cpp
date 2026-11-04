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
}

namespace StageCueTiming
{
	void GetAttributeTiming(const FShowCue& Cue, const FGameplayTag& Attribute, bool bGoingDown, float& OutDelay, float& OutDuration)
	{
		const FStageCueTiming& Timing = Cue.Timing;
		const EStagePaletteKind Kind = StagePaletteRules::GetAttributeKind(Attribute);
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
