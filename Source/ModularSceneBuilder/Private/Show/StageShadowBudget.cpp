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

	FStageShadowSelection SelectCastersWithHold(TConstArrayView<FStageShadowCandidate> Candidates, int32 Budget, float Hysteresis, double MinHoldSeconds)
	{
		FStageShadowSelection Selection;
		const TArray<bool> Ranked = SelectCasters(Candidates, Budget, Hysteresis);
		if (!FMath::IsFinite(MinHoldSeconds) || MinHoldSeconds <= 0.0)
		{
			Selection.Casting = Ranked;
			return Selection;
		}

		auto IsLit = [](const FStageShadowCandidate& Candidate) { return FMath::IsFinite(Candidate.Importance) && Candidate.Importance > 0.f; };
		auto IsHeld = [MinHoldSeconds, &IsLit](const FStageShadowCandidate& Candidate)
		{
			return IsLit(Candidate) && FMath::IsFinite(Candidate.SecondsSinceSwitch) && Candidate.SecondsSinceSwitch < MinHoldSeconds;
		};

		// Held candidates keep their state; the free ones are ranked for whatever budget the held casters leave.
		TArray<FStageShadowCandidate> Free(Candidates.GetData(), Candidates.Num());
		int32 HeldCasters = 0;
		for (int32 Index = 0; Index < Candidates.Num(); ++Index)
		{
			if (IsHeld(Candidates[Index]))
			{
				HeldCasters += Candidates[Index].bCasting ? 1 : 0;
				Free[Index].Importance = 0.f; // Out of the free ranking.
			}
		}
		const int32 FreeBudget = Budget < 0 ? Budget : FMath::Max(0, Budget - HeldCasters);
		Selection.Casting = SelectCasters(Free, FreeBudget, Hysteresis);

		for (int32 Index = 0; Index < Candidates.Num(); ++Index)
		{
			const FStageShadowCandidate& Candidate = Candidates[Index];
			if (!IsHeld(Candidate))
			{
				continue;
			}
			Selection.Casting[Index] = Candidate.bCasting;
			if (Candidate.bCasting != Ranked[Index])
			{
				const double Remaining = MinHoldSeconds - Candidate.SecondsSinceSwitch;
				Selection.RetryAfterSeconds = Selection.RetryAfterSeconds > 0.0 ? FMath::Min(Selection.RetryAfterSeconds, Remaining) : Remaining;
			}
		}

		// The budget is a hard limit: when it shrank below the held casters, the lowest ranked of them drop now.
		if (Budget >= 0 && HeldCasters > Budget)
		{
			TArray<int32> Casters;
			for (int32 Index = 0; Index < Candidates.Num(); ++Index)
			{
				if (Selection.Casting[Index])
				{
					Casters.Add(Index);
				}
			}
			Casters.Sort([&Candidates](int32 A, int32 B)
			{
				return Candidates[A].Importance != Candidates[B].Importance ? Candidates[A].Importance > Candidates[B].Importance : A < B;
			});
			for (int32 Rank = Budget; Rank < Casters.Num(); ++Rank)
			{
				Selection.Casting[Casters[Rank]] = false;
			}
		}
		return Selection;
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

float UStageShowPerformanceSettings::GetMinHoldSeconds() const
{
	return FMath::IsFinite(MinCasterHoldSeconds) ? FMath::Clamp(MinCasterHoldSeconds, 0.f, 10.f) : 1.f;
}
