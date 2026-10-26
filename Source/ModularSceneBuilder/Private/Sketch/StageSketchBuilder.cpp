// Copyright Epic Games, Inc. All Rights Reserved.

#include "Sketch/StageSketchBuilder.h"

#include "Actors/ModularBaseActor.h"
#include "Actors/StageArcTrussActor.h"
#include "Actors/StageDeckActor.h"
#include "Actors/StageSpanActor.h"
#include "Actors/StageStairsActor.h"
#include "Algo/Find.h"
#include "Components/SpawnSystemComponent.h"
#include "Data/StageArcTrussData.h"
#include "Data/StageDeckData.h"
#include "Data/StageSpanData.h"
#include "Data/StageStairsData.h"
#include "ModularSceneBuilder.h"

#define LOCTEXT_NAMESPACE "StageSketchBuilder"

namespace StageSketchBuilder
{
	namespace
	{
		/** Deletes what this build spawned, newest first, so the stage is as it was before it. */
		void RollBack(USpawnSystemComponent& Spawner, TArray<AModularBaseActor*>& Placed)
		{
			for (int32 Index = Placed.Num() - 1; Index >= 0; --Index)
			{
				if (IsValid(Placed[Index]) && !Spawner.TryDeleteActor(Placed[Index]))
				{
					UE_LOG(LogStageCraft, Error, TEXT("Sketch build: could not remove %s while rolling back."), *GetNameSafe(Placed[Index]));
				}
			}
			Placed.Reset();
		}

		FStageEconomyResultInfo MissingItem(const FText& What)
		{
			return FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest,
				FText::Format(LOCTEXT("MissingItem", "The {0} item is no longer in the catalog."), What));
		}

		FStageEconomyResultInfo SpawnFailed(const UBaseItemData& Item)
		{
			return FStageEconomyResultInfo::Fail(EStageEconomyResult::SessionRuleViolation,
				FText::Format(LOCTEXT("SpawnFailed", "{0} could not be placed."), Item.DisplayName));
		}
	}

	FStageSketchBuildOutcome Execute(const FStageSketchBuildPlan& Plan, USpawnSystemComponent& Spawner, FStageSketchPlacementCheck Check)
	{
		FStageSketchBuildOutcome Outcome;
		if (Plan.IsEmpty())
		{
			Outcome.Result = FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("EmptyPlan", "There is nothing to build."));
			return Outcome;
		}

		UStageDeckData* DeckItem = Plan.DeckItem.Get();
		UStageSpanData* TrussItem = Plan.TrussItem.Get();
		UBaseItemData* FixtureItem = Plan.FixtureItem.Get();
		UStageStairsData* StairsItem = Plan.StairsItem.Get();
		UStageArcTrussData* ArcItem = Plan.ArcTrussItem.Get();
		const struct FRequiredItem { bool bMissing; FText Name; } Required[] = {
			{ !Plan.Decks.IsEmpty() && !DeckItem, LOCTEXT("Deck", "deck") },
			{ !Plan.Trusses.IsEmpty() && !TrussItem, LOCTEXT("Truss", "truss") },
			{ !Plan.Fixtures.IsEmpty() && !FixtureItem, LOCTEXT("Fixture", "fixture") },
			{ !Plan.Stairs.IsEmpty() && !StairsItem, LOCTEXT("Stairs", "stairs") },
			{ !Plan.ArcTrusses.IsEmpty() && !ArcItem, LOCTEXT("ArcTruss", "curved truss") } };
		if (const FRequiredItem* Missing = Algo::FindByPredicate(Required, [](const FRequiredItem& Entry) { return Entry.bMissing; }))
		{
			Outcome.Result = MissingItem(Missing->Name);
			return Outcome;
		}

		TArray<AModularBaseActor*>& Placed = Outcome.Placed;
		Placed.Reserve(Plan.Num());

		// Checks one element first (so a refusal has its reason), then spawns it; any failure undoes the whole build.
		const auto Place = [&](UBaseItemData& Item, int32 Quantity, TFunctionRef<AModularBaseActor*()> Spawn)
		{
			const FStageEconomyResultInfo Verdict = Check(Item, Quantity);
			if (!Verdict.IsSuccess())
			{
				Outcome.Result = Verdict;
				return false;
			}
			AModularBaseActor* Actor = Spawn();
			if (!Actor)
			{
				Outcome.Result = SpawnFailed(Item);
				return false;
			}
			Placed.Add(Actor);
			return true;
		};

		bool bOk = true;
		for (int32 Index = 0; bOk && Index < Plan.Decks.Num(); ++Index)
		{
			const FStageSketchDeckPlan& Deck = Plan.Decks[Index];
			bOk = Place(*DeckItem, FMath::Max(1, Deck.Panels), [&]()
			{
				return Spawner.SpawnConfiguredItem(DeckItem, Deck.Transform, [&Deck](AModularBaseActor& Actor)
				{
					if (AStageDeckActor* DeckActor = Cast<AStageDeckActor>(&Actor))
					{
						DeckActor->InitializeShape(Deck.LocalOutline, Deck.Height);
					}
				});
			});
			Outcome.DeckCount += bOk ? 1 : 0;
		}
		for (int32 Index = 0; bOk && Index < Plan.Stairs.Num(); ++Index)
		{
			const FStageSketchStairsPlan& Stairs = Plan.Stairs[Index];
			bOk = Place(*StairsItem, 1, [&]()
			{
				return Spawner.SpawnConfiguredItem(StairsItem, Stairs.Transform, [&Stairs](AModularBaseActor& Actor)
				{
					if (AStageStairsActor* StairsActor = Cast<AStageStairsActor>(&Actor))
					{
						StairsActor->InitializeStairs(Stairs.Rise, Stairs.Width, Stairs.bHandrails);
					}
				});
			});
			Outcome.StairsCount += bOk ? 1 : 0;
		}
		for (int32 Index = 0; bOk && Index < Plan.Trusses.Num(); ++Index)
		{
			const FStageSketchTrussPlan& Truss = Plan.Trusses[Index];
			bOk = Place(*TrussItem, FMath::Max(1, Truss.SegmentCount), [&]()
			{
				return Spawner.SpawnConfiguredItem(TrussItem, Truss.Transform, [&Truss](AModularBaseActor& Actor)
				{
					if (AStageSpanActor* Run = Cast<AStageSpanActor>(&Actor))
					{
						Run->InitializePath(Truss.LocalPoints, Truss.bClosed);
					}
				});
			});
			Outcome.TrussCount += bOk ? 1 : 0;
		}
		for (int32 Index = 0; bOk && Index < Plan.ArcTrusses.Num(); ++Index)
		{
			const FStageSketchArcTrussPlan& Arc = Plan.ArcTrusses[Index];
			bOk = Place(*ArcItem, FMath::Max(1, Arc.Sections), [&]()
			{
				return Spawner.SpawnConfiguredItem(ArcItem, Arc.Transform, [&Arc](AModularBaseActor& Actor)
				{
					if (AStageArcTrussActor* ArcActor = Cast<AStageArcTrussActor>(&Actor))
					{
						ArcActor->InitializeArc(Arc.Radius, Arc.AngleDegrees);
					}
				});
			});
			Outcome.ArcTrussCount += bOk ? 1 : 0;
		}
		for (int32 Index = 0; bOk && Index < Plan.Fixtures.Num(); ++Index)
		{
			const FStageSketchFixturePlan& Fixture = Plan.Fixtures[Index];
			bOk = Place(*FixtureItem, 1, [&]() { return Spawner.SpawnItem(FixtureItem, Fixture.Transform); });
			Outcome.FixtureCount += bOk ? 1 : 0;
		}

		if (!bOk)
		{
			UE_LOG(LogStageCraft, Log, TEXT("Sketch build refused after %d of %d items (%s): %s; rolling back."), Placed.Num(), Plan.Num(),
				*UEnum::GetValueAsString(Outcome.Result.Code), *Outcome.Result.Message.ToString());
			RollBack(Spawner, Placed);
			Outcome.DeckCount = Outcome.TrussCount = Outcome.FixtureCount = Outcome.StairsCount = Outcome.ArcTrussCount = 0;
			return Outcome;
		}
		Outcome.Result = FStageEconomyResultInfo::Ok();
		return Outcome;
	}
}

#undef LOCTEXT_NAMESPACE
