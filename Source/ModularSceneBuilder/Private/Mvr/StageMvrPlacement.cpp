// Copyright Epic Games, Inc. All Rights Reserved.

#include "Mvr/StageMvrPlacement.h"

#include "Actors/LightingFixtureActor.h"
#include "Actors/ModularBaseActor.h"
#include "Actors/StageSpanActor.h"
#include "Components/SpawnSystemComponent.h"
#include "Data/StageSpanData.h"
#include "HAL/PlatformTime.h"
#include "ModularSceneBuilder.h"

#define LOCTEXT_NAMESPACE "StageMvrPlacement"

FStageMvrPlacementJob::FStageMvrPlacementJob(StageMvrImport::FPlan InPlan, USpawnSystemComponent& InSpawner, FCheck InCheck)
	: Plan(MoveTemp(InPlan))
	, Spawner(&InSpawner)
	, Check(MoveTemp(InCheck))
{
	Placed.Reserve(Plan.Num());
	if (Plan.Num() == 0)
	{
		Finish(FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("Empty", "There is nothing to place.")));
	}
}

bool FStageMvrPlacementJob::Step(double BudgetSeconds)
{
	if (bFinished)
	{
		return true;
	}
	++Steps;
	const double Start = FPlatformTime::Seconds();
	do
	{
		if (!PlaceOne(Next))
		{
			RollBack();
			Finish(Result);
			break;
		}
		if (++Next >= Plan.Num())
		{
			Finish(FStageEconomyResultInfo::Ok());
			break;
		}
	}
	while (FPlatformTime::Seconds() - Start < BudgetSeconds);
	PlacingSeconds += FPlatformTime::Seconds() - Start;
	return bFinished;
}

bool FStageMvrPlacementJob::PlaceOne(int32 Index)
{
	USpawnSystemComponent* Spawn = Spawner.Get();
	if (!Spawn)
	{
		Result = FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoSpawner", "The stage was closed while the rig was placed."));
		return false;
	}

	UBaseItemData* Item = nullptr;
	int32 Quantity = 1;
	TFunction<void(AModularBaseActor&)> Configure;
	FTransform Transform;
	if (Index < Plan.Trusses.Num())
	{
		const StageMvrImport::FTrussPlacement& Truss = Plan.Trusses[Index];
		Item = Plan.TrussItem.Get();
		Quantity = Truss.Sections;
		Transform = Truss.Transform;
		Configure = [&Truss](AModularBaseActor& Actor)
		{
			Actor.PrepareImported(FText::FromString(Truss.Label), Truss.MvrId);
			if (AStageSpanActor* Run = Cast<AStageSpanActor>(&Actor))
			{
				Run->InitializePath(Truss.LocalPoints, false);
			}
		};
	}
	else
	{
		const StageMvrImport::FFixturePlacement& Fixture = Plan.Fixtures[Index - Plan.Trusses.Num()];
		Item = Fixture.Item.Get();
		Transform = Fixture.Transform;
		Configure = [&Fixture](AModularBaseActor& Actor)
		{
			Actor.PrepareImported(FText::FromString(Fixture.Label), Fixture.MvrId);
			if (ALightingFixtureActor* Light = Cast<ALightingFixtureActor>(&Actor))
			{
				Light->PrepareImportedPatch(Fixture.FixtureId, Fixture.Patch, Fixture.PanTilt);
			}
		};
	}
	if (!Item)
	{
		Result = FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("MissingItem", "An item of the rig is no longer in the catalog."));
		return false;
	}

	const FStageEconomyResultInfo Verdict = Check ? Check(*Item, Quantity) : FStageEconomyResultInfo::Ok();
	if (!Verdict.IsSuccess())
	{
		Result = Verdict;
		return false;
	}
	AModularBaseActor* Actor = Spawn->SpawnConfiguredItem(Item, Transform, [&Configure](AModularBaseActor& Deferred) { Configure(Deferred); });
	if (!Actor)
	{
		Result = FStageEconomyResultInfo::Fail(EStageEconomyResult::SessionRuleViolation, FText::Format(LOCTEXT("SpawnFailed", "{0} could not be placed."), Item->DisplayName));
		return false;
	}
	Placed.Add(Actor);
	return true;
}

void FStageMvrPlacementJob::RollBack()
{
	USpawnSystemComponent* Spawn = Spawner.Get();
	for (int32 Index = Placed.Num() - 1; Index >= 0; --Index)
	{
		AModularBaseActor* Actor = Placed[Index].Get();
		if (IsValid(Actor) && (!Spawn || !Spawn->TryDeleteActor(Actor)))
		{
			// Without its spawner (the level is closing) the actor goes with the level.
			UE_LOG(LogStageCraft, Warning, TEXT("MVR import: could not remove %s while rolling back."), *GetNameSafe(Actor));
		}
	}
	Placed.Reset();
}

void FStageMvrPlacementJob::Finish(const FStageEconomyResultInfo& InResult)
{
	Result = InResult;
	bFinished = true;
}

void FStageMvrPlacementJob::Abort(const FText& Reason)
{
	if (bFinished)
	{
		return;
	}
	RollBack();
	Finish(FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, Reason));
}

TMap<int32, TArray<int32>> FStageMvrPlacementJob::GetFixtureIdsByLayer() const
{
	TMap<int32, TArray<int32>> ByLayer;
	for (int32 Index = Plan.Trusses.Num(); Index < Placed.Num(); ++Index)
	{
		const ALightingFixtureActor* Light = Cast<ALightingFixtureActor>(Placed[Index].Get());
		const int32 Layer = Plan.Fixtures[Index - Plan.Trusses.Num()].Layer;
		if (Light && Layer != INDEX_NONE)
		{
			ByLayer.FindOrAdd(Layer).Add(Light->GetFixtureId());
		}
	}
	return ByLayer;
}

#undef LOCTEXT_NAMESPACE
