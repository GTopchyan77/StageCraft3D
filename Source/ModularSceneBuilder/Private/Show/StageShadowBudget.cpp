// Copyright Epic Games, Inc. All Rights Reserved.

#include "Show/StageShadowBudget.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageShadowBudget)

namespace StageShadowBudget
{
	namespace
	{
		bool IsLit(const FStageShadowCandidate& Candidate)
		{
			return FMath::IsFinite(Candidate.Importance) && Candidate.Importance > 0.f;
		}

		/** The moving limit as it applies: never more than the total, and the total when there is no limit of its own. */
		int32 GetMovingLimit(const FStageShadowLimits& Limits)
		{
			return Limits.Moving < 0 ? Limits.Total : FMath::Min(Limits.Moving, Limits.Total);
		}

		/** Switches off the lowest ranked casters that Filter selects until at most Limit of them cast. */
		template <typename FilterType>
		void DropLowestOver(TConstArrayView<FStageShadowCandidate> Candidates, TArray<bool>& Casting, int32 Limit, FilterType Filter)
		{
			TArray<int32> Casters;
			for (int32 Index = 0; Index < Candidates.Num(); ++Index)
			{
				if (Casting[Index] && Filter(Candidates[Index]))
				{
					Casters.Add(Index);
				}
			}
			if (Casters.Num() <= Limit)
			{
				return;
			}
			Casters.Sort([&Candidates](int32 A, int32 B)
			{
				return Candidates[A].Importance != Candidates[B].Importance ? Candidates[A].Importance > Candidates[B].Importance : A < B;
			});
			for (int32 Rank = Limit; Rank < Casters.Num(); ++Rank)
			{
				Casting[Casters[Rank]] = false;
			}
		}
	}

	TArray<bool> SelectCasters(TConstArrayView<FStageShadowCandidate> Candidates, const FStageShadowLimits& Limits, float Hysteresis)
	{
		TArray<bool> Casting;
		Casting.Init(false, Candidates.Num());
		const float Bonus = 1.f + (FMath::IsFinite(Hysteresis) ? FMath::Clamp(Hysteresis, 0.f, 2.f) : 0.f);

		TArray<TPair<float, int32>> Ranked;
		Ranked.Reserve(Candidates.Num());
		for (int32 Index = 0; Index < Candidates.Num(); ++Index)
		{
			if (IsLit(Candidates[Index]))
			{
				const float Importance = Candidates[Index].Importance;
				Ranked.Emplace(Candidates[Index].bCasting ? Importance * Bonus : Importance, Index);
			}
		}
		if (Limits.Total < 0)
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
		// A moving candidate over its limit is passed over rather than ending the walk, so a still one further down gets the place.
		const int32 MovingLimit = GetMovingLimit(Limits);
		int32 Cast = 0;
		int32 MovingCast = 0;
		for (const TPair<float, int32>& Entry : Ranked)
		{
			if (Cast >= Limits.Total)
			{
				break;
			}
			const bool bMoving = Candidates[Entry.Value].bMoving;
			if (bMoving && MovingCast >= MovingLimit)
			{
				continue;
			}
			Casting[Entry.Value] = true;
			++Cast;
			MovingCast += bMoving ? 1 : 0;
		}
		return Casting;
	}

	FStageShadowSelection SelectCastersWithHold(TConstArrayView<FStageShadowCandidate> Candidates, const FStageShadowLimits& Limits, float Hysteresis, double MinHoldSeconds)
	{
		FStageShadowSelection Selection;
		const TArray<bool> Ranked = SelectCasters(Candidates, Limits, Hysteresis);
		if (!FMath::IsFinite(MinHoldSeconds) || MinHoldSeconds <= 0.0)
		{
			Selection.Casting = Ranked;
			return Selection;
		}

		auto IsHeld = [MinHoldSeconds](const FStageShadowCandidate& Candidate)
		{
			return IsLit(Candidate) && FMath::IsFinite(Candidate.SecondsSinceSwitch) && Candidate.SecondsSinceSwitch < MinHoldSeconds;
		};

		// Held candidates keep their state; the free ones are ranked for whatever the held casters leave of each limit.
		TArray<FStageShadowCandidate> Free(Candidates.GetData(), Candidates.Num());
		int32 HeldCasters = 0;
		int32 HeldMovingCasters = 0;
		for (int32 Index = 0; Index < Candidates.Num(); ++Index)
		{
			if (IsHeld(Candidates[Index]))
			{
				HeldCasters += Candidates[Index].bCasting ? 1 : 0;
				HeldMovingCasters += Candidates[Index].bCasting && Candidates[Index].bMoving ? 1 : 0;
				Free[Index].Importance = 0.f; // Out of the free ranking.
			}
		}
		FStageShadowLimits FreeLimits = Limits;
		if (Limits.Total >= 0)
		{
			FreeLimits.Total = FMath::Max(0, Limits.Total - HeldCasters);
			FreeLimits.Moving = FMath::Max(0, GetMovingLimit(Limits) - HeldMovingCasters);
		}
		Selection.Casting = SelectCasters(Free, FreeLimits, Hysteresis);

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

		// Both limits are hard: held casters over one of them (a held caster whose head started moving, a shrunken budget) drop,
		// the lowest ranked first. The moving limit first, so a dropped moving caster leaves room under the total.
		if (Limits.Total >= 0)
		{
			DropLowestOver(Candidates, Selection.Casting, GetMovingLimit(Limits), [](const FStageShadowCandidate& Candidate) { return Candidate.bMoving; });
			DropLowestOver(Candidates, Selection.Casting, Limits.Total, [](const FStageShadowCandidate&) { return true; });
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

	bool IsMoving(double SecondsSinceMove, double MovingSeconds)
	{
		// A clock that went backwards (a new world) or invalid input counts as still: a still fixture keeps the wider budget.
		return FMath::IsFinite(SecondsSinceMove) && FMath::IsFinite(MovingSeconds) && SecondsSinceMove >= 0.0 && SecondsSinceMove < MovingSeconds;
	}
}

int32 UStageShowPerformanceSettings::GetShadowBudget() const
{
	return bLimitFixtureShadows ? FMath::Clamp(MaxShadowedFixtures, 0, 2000) : -1;
}

int32 UStageShowPerformanceSettings::GetMovingShadowBudget() const
{
	return bLimitFixtureShadows ? FMath::Clamp(MaxShadowedMovingFixtures, 0, 2000) : -1;
}

FStageShadowLimits UStageShowPerformanceSettings::GetShadowLimits() const
{
	return { GetShadowBudget(), GetMovingShadowBudget() };
}

float UStageShowPerformanceSettings::GetMovingSeconds() const
{
	return FMath::IsFinite(MovingSeconds) ? FMath::Clamp(MovingSeconds, 0.1f, 10.f) : 1.f;
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
