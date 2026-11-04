// Copyright Epic Games, Inc. All Rights Reserved.

#include "Show/StageEffectRunner.h"

#include "Actors/LightingFixtureActor.h"
#include "Data/LightingFixtureData.h"
#include "Data/StageParameterTypes.h"
#include "Show/StageCueTracking.h"
#include "Show/StageEffects.h"

namespace
{
	/** The feature whose cue time an effect fades with (Docs/ADR/0027 §5). */
	EStagePaletteKind GetEffectFeature(const FStageEffect& Effect)
	{
		switch (Effect.Target)
		{
		case EStageEffectTarget::Intensity: return EStagePaletteKind::Intensity;
		case EStageEffectTarget::Position:  return EStagePaletteKind::Position;
		case EStageEffectTarget::Color:     return EStagePaletteKind::Color;
		case EStageEffectTarget::Beam:      return StagePaletteRules::GetAttributeKind(Effect.Attribute);
		}
		return EStagePaletteKind::All;
	}

	/** Same effects on the same fixtures in the same order: the drives (indices into the running list) still hold. */
	template <typename TRunning>
	bool HasSameLayout(const TArray<TRunning>& A, const TArray<TRunning>& B)
	{
		if (A.Num() != B.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < A.Num(); ++Index)
		{
			if (A[Index].Source.Entry.Id != B[Index].Source.Entry.Id || A[Index].Source.Entry.FixtureIds != B[Index].Source.Entry.FixtureIds)
			{
				return false;
			}
		}
		return true;
	}
}

float FStageEffectRunner::FTransition::GetAlpha() const
{
	return StageCueTiming::ApplyCurve(Curve, StageCueTiming::GetProgress(Elapsed, Delay, Duration));
}

float FStageEffectRunner::FRunning::GetWeight() const
{
	if (Transition.IsSet())
	{
		return FMath::Lerp(Transition->From, Transition->To, Transition->GetAlpha());
	}
	return bStopping ? 0.f : 1.f;
}

bool FStageEffectRunner::StartTransition(FRunning& Item, const FShowCue& TimedBy, float From, float To, TOptional<FStageEffect> MorphFrom)
{
	FTransition Transition;
	Transition.From = From;
	Transition.To = To;
	Transition.Curve = TimedBy.Timing.Curve;
	// Going down (a stop) takes the Out time for intensity, as a cue's dimmers going down do.
	StageCueTiming::GetFeatureTiming(TimedBy, GetEffectFeature(Item.Source.Entry.Effect), To < From, Transition.Delay, Transition.Duration);
	Transition.MorphFrom = MoveTemp(MorphFrom);
	if (Transition.Delay + Transition.Duration <= UE_KINDA_SMALL_NUMBER)
	{
		Item.Transition.Reset();
		return false;
	}
	Item.Transition = MoveTemp(Transition);
	return true;
}

bool FStageEffectRunner::SetEffects(TConstArrayView<FSourcedEntry> Entries, FFindFixture FindFixture, const FShowCue* TimedBy)
{
	bool bAnyChange = false;
	const auto IsListed = [&Entries](const FGuid& Id)
	{
		return Entries.ContainsByPredicate([&Id](const FSourcedEntry& Entry) { return Entry.Entry.Id == Id; });
	};

	TArray<FRunning> Next;
	Next.Reserve(Entries.Num() + Running.Num());
	// Effects that leave go first: under the ones that stay, as the older looks. A cue fades them out; anything else drops them,
	// except one already fading out, which finishes.
	for (const FRunning& Old : Running)
	{
		if (IsListed(Old.Source.Entry.Id))
		{
			continue;
		}
		bAnyChange |= !Old.bStopping;
		if (Old.bStopping)
		{
			Next.Add(Old);
			continue;
		}
		const float Weight = Old.GetWeight();
		FRunning Stopping = Old;
		if (TimedBy && Weight > 0.f && StartTransition(Stopping, *TimedBy, Weight, 0.f, {}))
		{
			Stopping.bStopping = true;
			Next.Add(MoveTemp(Stopping));
		}
	}

	for (const FSourcedEntry& Source : Entries)
	{
		const FRunning* Existing = Running.FindByPredicate([&Source](const FRunning& Old) { return Old.Source.Entry.Id == Source.Entry.Id; });
		FRunning Item = Existing ? *Existing : FRunning();
		const bool bNew = !Existing || Existing->bStopping;
		const bool bChanged = Existing && !Existing->Source.Entry.Effect.Equals(Source.Entry.Effect);
		const float Weight = Existing ? Existing->GetWeight() : 0.f;
		bAnyChange |= bNew || bChanged || Existing->Source.Source != Source.Source || Existing->Source.Playback != Source.Playback;
		Item.Source = Source;
		Item.bStopping = false;
		if (bNew || bChanged)
		{
			Item.Transition.Reset();
			if (TimedBy)
			{
				// A change morphs from the settings it had; a start (or a stopping effect that comes back) fades up from where it is.
				TOptional<FStageEffect> MorphFrom;
				if (!bNew)
				{
					MorphFrom = Existing->Source.Entry.Effect;
				}
				StartTransition(Item, *TimedBy, Weight, 1.f, MoveTemp(MorphFrom));
			}
		}
		Next.Add(MoveTemp(Item));
	}

	const bool bSameLayout = HasSameLayout(Running, Next);
	Running = MoveTemp(Next);
	if (!bSameLayout)
	{
		RebuildDrives(&FindFixture);
	}
	return bAnyChange || !bSameLayout;
}

void FStageEffectRunner::RefreshFixtures(FFindFixture FindFixture)
{
	RebuildDrives(&FindFixture);
}

void FStageEffectRunner::RebuildDrives(const FFindFixture* FindFixture)
{
	ByFixture.Reset();
	for (int32 EffectIndex = 0; EffectIndex < Running.Num(); ++EffectIndex)
	{
		const TArray<int32>& FixtureIds = Running[EffectIndex].Source.Entry.FixtureIds;
		for (int32 Index = 0; Index < FixtureIds.Num(); ++Index)
		{
			ByFixture.FindOrAdd(FixtureIds[Index]).Add({ EffectIndex, Index });
		}
	}
	if (!FindFixture)
	{
		// Only effects left: every fixture still driven was already found.
		DrivenFixtures.RemoveAll([this](const FDrivenFixture& Driven) { return !ByFixture.Contains(Driven.FixtureId); });
		return;
	}
	DrivenFixtures.Reset();
	DrivenFixtures.Reserve(ByFixture.Num());
	for (const TPair<int32, TArray<FDrive, TInlineAllocator<2>>>& Pair : ByFixture)
	{
		if (ALightingFixtureActor* Fixture = (*FindFixture)(Pair.Key))
		{
			DrivenFixtures.Add({ Fixture, Pair.Key });
		}
	}
}

void FStageEffectRunner::Advance(float DeltaSeconds, TArray<int32>& OutReleased)
{
	if (!FMath::IsFinite(DeltaSeconds) || DeltaSeconds <= 0.f)
	{
		return;
	}
	bool bAnyEnded = false;
	for (FRunning& Item : Running)
	{
		float Rate = Item.Source.Entry.Effect.RateHz;
		if (Item.Transition.IsSet() && Item.Transition->MorphFrom.IsSet())
		{
			// A morph blends the rate too, so the pattern speeds up or slows down smoothly.
			Rate = FMath::Lerp(Item.Transition->MorphFrom->RateHz, Rate, Item.Transition->GetAlpha());
		}
		// Wrapped every 2^20 cycles (days at the top rate): keeps the double's fraction exact, and every waveform repeats by then
		// except Random, whose sequence simply carries on from another point.
		Item.Cycles = FMath::Fmod(Item.Cycles + static_cast<double>(Rate) * DeltaSeconds, 1048576.0);

		if (Item.Transition.IsSet())
		{
			Item.Transition->Elapsed += DeltaSeconds;
			if (Item.Transition->IsDone())
			{
				Item.Transition.Reset();
				bAnyEnded |= Item.bStopping;
			}
		}
	}
	if (!bAnyEnded)
	{
		return;
	}
	for (const FRunning& Item : Running)
	{
		if (Item.bStopping && !Item.Transition.IsSet())
		{
			for (const int32 FixtureId : Item.Source.Entry.FixtureIds)
			{
				OutReleased.AddUnique(FixtureId);
			}
		}
	}
	Running.RemoveAll([](const FRunning& Item) { return Item.bStopping && !Item.Transition.IsSet(); });
	RebuildDrives(nullptr);
}

double FStageEffectRunner::GetCycles(const FGuid& Id) const
{
	const FRunning* Item = Running.FindByPredicate([&Id](const FRunning& Candidate) { return Candidate.Source.Entry.Id == Id; });
	return Item ? Item->Cycles : 0.0;
}

float FStageEffectRunner::GetWeight(const FGuid& Id) const
{
	const FRunning* Item = Running.FindByPredicate([&Id](const FRunning& Candidate) { return Candidate.Source.Entry.Id == Id; });
	return Item ? Item->GetWeight() : 0.f;
}

void FStageEffectRunner::GetRunning(TArray<FStageRunningEffect>& OutEffects) const
{
	OutEffects.Reset(Running.Num());
	for (const FRunning& Item : Running)
	{
		FStageRunningEffect& Effect = OutEffects.AddDefaulted_GetRef();
		Effect.Entry = Item.Source.Entry;
		Effect.Source = Item.Source.Source;
		Effect.Playback = Item.Source.Playback;
		Effect.bStopping = Item.bStopping;
	}
}

bool FStageEffectRunner::Evaluate(const ALightingFixtureActor& Fixture, StageShowMix::FEffectMap& OutValues) const
{
	OutValues.Reset();
	const TArray<FDrive, TInlineAllocator<2>>* Drives = ByFixture.Find(Fixture.GetFixtureId());
	if (!Drives)
	{
		return false;
	}
	for (const FDrive& Drive : *Drives)
	{
		EvaluateDrive(Fixture, Drive, OutValues);
	}
	return !OutValues.IsEmpty();
}

void FStageEffectRunner::EvaluateDrive(const ALightingFixtureActor& Fixture, const FDrive& Drive, StageShowMix::FEffectMap& OutValues) const
{
	const FRunning& Item = Running[Drive.Effect];
	const float Weight = Item.GetWeight();
	if (Weight <= 0.f)
	{
		return;
	}
	const FStageEffectEntry& Entry = Item.Source.Entry;
	const int32 Count = Entry.FixtureIds.Num();

	// A morph evaluates the old settings on the same clock and blends each value towards the new ones.
	StageShowMix::FEffectMap MorphFromValues;
	float MorphAlpha = 1.f;
	if (Item.Transition.IsSet() && Item.Transition->MorphFrom.IsSet())
	{
		MorphAlpha = Item.Transition->GetAlpha();
		if (MorphAlpha < 1.f)
		{
			EvaluateEffect(Fixture, *Item.Transition->MorphFrom, Drive.Index, Count, Item.Cycles,
				[&MorphFromValues](const FGameplayTag& Attribute, float Value, bool bRelative) { MorphFromValues.Add(Attribute, { Value, bRelative, 1.f }); });
		}
	}

	// A playback's fader scales its intensity effects as it scales its intensity (Docs/ADR/0027 §5).
	const float Scale = Entry.Effect.Target == EStageEffectTarget::Intensity ? Item.Source.IntensityScale : 1.f;
	EvaluateEffect(Fixture, Entry.Effect, Drive.Index, Count, Item.Cycles, [&](const FGameplayTag& Attribute, float Value, bool bRelative)
	{
		if (const StageShowMix::FEffectValue* From = MorphFromValues.Find(Attribute); From && From->bRelative == bRelative)
		{
			Value = FMath::Lerp(From->Value, Value, MorphAlpha);
		}
		const StageShowMix::FEffectValue Over{ Value * Scale, bRelative, Weight };
		if (StageShowMix::FEffectValue* Below = OutValues.Find(Attribute))
		{
			StageShowMix::ComposeEffect(*Below, Over);
		}
		else
		{
			OutValues.Add(Attribute, Over);
		}
	});
}

void FStageEffectRunner::EvaluateEffect(const ALightingFixtureActor& Fixture, const FStageEffect& Effect, int32 Index, int32 Count, double Cycles,
	TFunctionRef<void(const FGameplayTag&, float, bool)> Emit)
{
	const StageEffects::FSample Sample = StageEffects::Evaluate(Effect, Index, Count, Cycles);
	const float SizeFactor = StageEffects::GetSizeFactor(Effect, Index, Count);
	const TMap<FGameplayTag, float>& Attributes = Fixture.GetAttributes();

	// A value between Low and High of the fixture's own range, so one effect suits every fixture type.
	const auto EmitLevel = [&](const FGameplayTag& Attribute)
	{
		const ULightingFixtureData* Data = Fixture.GetFixtureData();
		float Min = 0.f;
		float Max = 1.f;
		if (Data && !Data->GetAttributeRange(Attribute, Min, Max))
		{
			return;
		}
		Emit(Attribute, FMath::Lerp(Min, Max, StageEffects::GetLevel(Effect, Sample.Unit, SizeFactor)), false);
	};

	switch (Effect.Target)
	{
	case EStageEffectTarget::Intensity:
		for (const TPair<FGameplayTag, float>& Pair : Attributes)
		{
			if (StagePaletteRules::GetAttributeKind(Pair.Key) == EStagePaletteKind::Intensity)
			{
				EmitLevel(Pair.Key);
			}
		}
		break;

	case EStageEffectTarget::Beam:
		if (Attributes.Contains(Effect.Attribute))
		{
			EmitLevel(Effect.Attribute);
		}
		break;

	case EStageEffectTarget::Position:
	{
		const bool bTwoAxis = Effect.Waveform == EStageEffectWaveform::Circle || Effect.Waveform == EStageEffectWaveform::Figure8;
		const float Size = Effect.Size * SizeFactor;
		if ((bTwoAxis || Effect.Axis != EStageEffectAxis::Tilt) && Attributes.Contains(StageCraftTags::Attribute_Pan))
		{
			Emit(StageCraftTags::Attribute_Pan, Size * Sample.Pan, true);
		}
		if ((bTwoAxis || Effect.Axis != EStageEffectAxis::Pan) && Attributes.Contains(StageCraftTags::Attribute_Tilt))
		{
			Emit(StageCraftTags::Attribute_Tilt, Size * Sample.Tilt, true);
		}
		break;
	}

	case EStageEffectTarget::Color:
		// The fixture mixes the colour its own way (emitters, CMY, the nearest wheel slot), as a colour palette does.
		for (const TPair<FGameplayTag, float>& Pair : Fixture.MatchColor(StageEffects::GetColor(Effect, Sample.Unit)))
		{
			Emit(Pair.Key, Pair.Value, false);
		}
		break;
	}
}
