// Copyright Epic Games, Inc. All Rights Reserved.

#include "Show/StageShadowBudget.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageShadowBudget)

namespace StageShadowBudget
{
	TArray<bool> SelectCasters(TConstArrayView<FStageShadowCandidate> Candidates, int32 Budget, float Hysteresis)
	{
		TArray<bool> Casting;
		Casting.Init(false, Candidates.Num());
		const float Bonus = 1.f + (FMath::IsFinite(Hysteresis) ? FMath::Clamp(Hysteresis, 0.f, 2.f) : 0.f);

		TArray<TPair<float, int32>> Ranked;
		Ranked.Reserve(Candidates.Num());
		for (int32 Index = 0; Index < Candidates.Num(); ++Index)
		{
			const float Importance = Candidates[Index].Importance;
			if (FMath::IsFinite(Importance) && Importance > 0.f)
			{
				Ranked.Emplace(Candidates[Index].bCasting ? Importance * Bonus : Importance, Index);
			}
		}
		if (Budget < 0 || Ranked.Num() <= Budget)
		{
			for (const TPair<float, int32>& Entry : Ranked)
			{
				Casting[Entry.Value] = true;
			}
			return Casting;
		}
		// Brightest first; equal scores keep the registration order, so the result never depends on the sort's stability.
		Ranked.Sort([](const TPair<float, int32>& A, const TPair<float, int32>& B)
		{
			return A.Key != B.Key ? A.Key > B.Key : A.Value < B.Value;
		});
		for (int32 Rank = 0; Rank < Budget; ++Rank)
		{
			Casting[Ranked[Rank].Value] = true;
		}
		return Casting;
	}

	float GetImportance(float Flux, double DistanceCm)
	{
		if (!FMath::IsFinite(Flux) || Flux <= 0.f || !FMath::IsFinite(DistanceCm))
		{
			return 0.f;
		}
		const double Metres = FMath::Max(DistanceCm / 100.0, 1.0);
		return static_cast<float>(Flux / (Metres * Metres));
	}
}

int32 UStageShowPerformanceSettings::GetShadowBudget() const
{
	return bLimitFixtureShadows ? FMath::Clamp(MaxShadowedFixtures, 0, 2000) : -1;
}

float UStageShowPerformanceSettings::GetUpdateSeconds() const
{
	return FMath::IsFinite(UpdateIntervalSeconds) ? FMath::Clamp(UpdateIntervalSeconds, 0.05f, 2.f) : 0.25f;
}

float UStageShowPerformanceSettings::GetHysteresis() const
{
	return FMath::IsFinite(Hysteresis) ? FMath::Clamp(Hysteresis, 0.f, 2.f) : 0.25f;
}
