// Copyright Epic Games, Inc. All Rights Reserved.

#include "Show/StageEffectRunner.h"

#include "Actors/LightingFixtureActor.h"
#include "Data/LightingFixtureData.h"
#include "Data/StageParameterTypes.h"
#include "Show/ShowControlTypes.h"
#include "Show/StageEffects.h"

void FStageEffectRunner::SetEffects(TConstArrayView<FStageEffectEntry> Entries, FFindFixture FindFixture)
{
	TArray<FRunning> Next;
	Next.Reserve(Entries.Num());
	for (const FStageEffectEntry& Entry : Entries)
	{
		FRunning& Item = Next.AddDefaulted_GetRef();
		Item.Entry = Entry;
		if (const FRunning* Existing = Running.FindByPredicate([&Entry](const FRunning& Old) { return Old.Entry.Id == Entry.Id; }))
		{
			Item.Cycles = Existing->Cycles;
		}
	}
	Running = MoveTemp(Next);
	RefreshFixtures(FindFixture);
}

void FStageEffectRunner::RefreshFixtures(FFindFixture FindFixture)
{
	ByFixture.Reset();
	DrivenFixtures.Reset();
	for (int32 EffectIndex = 0; EffectIndex < Running.Num(); ++EffectIndex)
	{
		const TArray<int32>& FixtureIds = Running[EffectIndex].Entry.FixtureIds;
		for (int32 Index = 0; Index < FixtureIds.Num(); ++Index)
		{
			ByFixture.FindOrAdd(FixtureIds[Index]).Add({ EffectIndex, Index });
		}
	}
	DrivenFixtures.Reserve(ByFixture.Num());
	for (const TPair<int32, TArray<FDrive, TInlineAllocator<2>>>& Pair : ByFixture)
	{
		if (ALightingFixtureActor* Fixture = FindFixture(Pair.Key))
		{
			DrivenFixtures.Add({ Fixture, Pair.Key });
		}
	}
}

void FStageEffectRunner::Advance(float DeltaSeconds)
{
	if (!FMath::IsFinite(DeltaSeconds) || DeltaSeconds <= 0.f)
	{
		return;
	}
	for (FRunning& Item : Running)
	{
		// Wrapped every 2^20 cycles (days at the top rate): keeps the double's fraction exact, and every waveform repeats by then
		// except Random, whose sequence simply carries on from another point.
		Item.Cycles = FMath::Fmod(Item.Cycles + static_cast<double>(Item.Entry.Effect.RateHz) * DeltaSeconds, 1048576.0);
	}
}

double FStageEffectRunner::GetCycles(const FGuid& Id) const
{
	const FRunning* Item = Running.FindByPredicate([&Id](const FRunning& Candidate) { return Candidate.Entry.Id == Id; });
	return Item ? Item->Cycles : 0.0;
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
	const FStageEffect& Effect = Item.Entry.Effect;
	const int32 Count = Item.Entry.FixtureIds.Num();
	const StageEffects::FSample Sample = StageEffects::Evaluate(Effect, Drive.Index, Count, Item.Cycles);
	const float SizeFactor = StageEffects::GetSizeFactor(Effect, Drive.Index, Count);
	const TMap<FGameplayTag, float>& Attributes = Fixture.GetAttributes();

	// A value between Low and High of the fixture's own range, so one effect suits every fixture type.
	const auto SetLevel = [&](const FGameplayTag& Attribute)
	{
		const ULightingFixtureData* Data = Fixture.GetFixtureData();
		float Min = 0.f;
		float Max = 1.f;
		if (Data && !Data->GetAttributeRange(Attribute, Min, Max))
		{
			return;
		}
		OutValues.Add(Attribute, { FMath::Lerp(Min, Max, StageEffects::GetLevel(Effect, Sample.Unit, SizeFactor)), false, 1.f });
	};

	switch (Effect.Target)
	{
	case EStageEffectTarget::Intensity:
		for (const TPair<FGameplayTag, float>& Pair : Attributes)
		{
			if (StagePaletteRules::GetAttributeKind(Pair.Key) == EStagePaletteKind::Intensity)
			{
				SetLevel(Pair.Key);
			}
		}
		break;

	case EStageEffectTarget::Beam:
		if (Attributes.Contains(Effect.Attribute))
		{
			SetLevel(Effect.Attribute);
		}
		break;

	case EStageEffectTarget::Position:
	{
		const bool bTwoAxis = Effect.Waveform == EStageEffectWaveform::Circle || Effect.Waveform == EStageEffectWaveform::Figure8;
		const float Size = Effect.Size * SizeFactor;
		if ((bTwoAxis || Effect.Axis != EStageEffectAxis::Tilt) && Attributes.Contains(StageCraftTags::Attribute_Pan))
		{
			OutValues.Add(StageCraftTags::Attribute_Pan, { Size * Sample.Pan, true, 1.f });
		}
		if ((bTwoAxis || Effect.Axis != EStageEffectAxis::Pan) && Attributes.Contains(StageCraftTags::Attribute_Tilt))
		{
			OutValues.Add(StageCraftTags::Attribute_Tilt, { Size * Sample.Tilt, true, 1.f });
		}
		break;
	}

	case EStageEffectTarget::Color:
		// The fixture mixes the colour its own way (emitters, CMY, the nearest wheel slot), as a colour palette does.
		for (const TPair<FGameplayTag, float>& Pair : Fixture.MatchColor(StageEffects::GetColor(Effect, Sample.Unit)))
		{
			OutValues.Add(Pair.Key, { Pair.Value, false, 1.f });
		}
		break;
	}
}
