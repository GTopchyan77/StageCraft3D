// Copyright Epic Games, Inc. All Rights Reserved.

#include "Mvr/StageMvrPlacement.h"

#include "Actors/LightingFixtureActor.h"
#include "Actors/ModularBaseActor.h"
#include "Actors/StageSpanActor.h"
#include "Components/SpawnSystemComponent.h"
#include "Data/StageSpanData.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "ModularSceneBuilder.h"
#include "Subsystems/StageSessionSubsystem.h"

#define LOCTEXT_NAMESPACE "StageMvrPlacement"

FStageMvrPlacementJob::FStageMvrPlacementJob(StageMvrImport::FPlan InPlan, USpawnSystemComponent& InSpawner, FCheck InCheck)
	: Plan(MoveTemp(InPlan))
	, Spawner(&InSpawner)
	, Check(MoveTemp(InCheck))
{
	Placed.Reserve(Plan.Fixtures.Num() + Plan.Trusses.Num());
	PlacedNewFixtureLayer.Reserve(Plan.Fixtures.Num() + Plan.Trusses.Num());
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

AModularBaseActor* FStageMvrPlacementJob::FindItem(const FGuid& InstanceId) const
{
	const USpawnSystemComponent* Spawn = Spawner.Get();
	const UStageSessionSubsystem* Session = Spawn ? UWorld::GetSubsystem<UStageSessionSubsystem>(Spawn->GetWorld()) : nullptr;
	return Session && InstanceId.IsValid() ? Session->FindItemById(InstanceId) : nullptr;
}

const FStageItemSnapshot* FStageMvrPlacementJob::TakeOff(const FGuid& InstanceId)
{
	USpawnSystemComponent* Spawn = Spawner.Get();
	AModularBaseActor* Item = FindItem(InstanceId);
	if (!Spawn || !Item)
	{
		return nullptr;
	}
	FStageItemSnapshot Snapshot = Item->CaptureSnapshot();
	if (!Spawn->TryDeleteActor(Item))
	{
		return nullptr;
	}
	return &Removed.Add_GetRef(MoveTemp(Snapshot));
}

bool FStageMvrPlacementJob::PlaceOne(int32 Index)
{
	USpawnSystemComponent* Spawn = Spawner.Get();
	if (!Spawn)
	{
		Result = FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoSpawner", "The stage was closed while the rig was placed."));
		return false;
	}

	if (Index < Plan.Removals.Num())
	{
		// Gone already (the user deleted it meanwhile): nothing to remove, and nothing to restore on a rollback.
		if (!TakeOff(Plan.Removals[Index].InstanceId))
		{
			UE_LOG(LogStageCraft, Log, TEXT("MVR import: %s was no longer on the stage to remove."), *Plan.Removals[Index].Label);
		}
		return true;
	}
	Index -= Plan.Removals.Num();

	UBaseItemData* Item = nullptr;
	int32 Quantity = 1;
	TFunction<void(AModularBaseActor&)> Configure;
	FTransform Transform;
	FGuid Replaces;
	int32 NewFixtureLayer = INDEX_NONE;
	if (Index < Plan.Trusses.Num())
	{
		const StageMvrImport::FTrussPlacement& Truss = Plan.Trusses[Index];
		Item = Truss.Item.Get();
		Quantity = Truss.Sections;
		Transform = Truss.Transform;
		Replaces = Truss.Replaces;
		const bool bUpdate = Replaces.IsValid();
		Configure = [&Truss, bUpdate](AModularBaseActor& Actor)
		{
			const bool bRename = !bUpdate || EnumHasAnyFlags(Truss.Changes, StageMvrImport::EChange::Name);
			Actor.PrepareImported(bRename ? TOptional<FText>(FText::FromString(Truss.Label)) : TOptional<FText>(), Truss.MvrId, Truss.Origin);
			AStageSpanActor* Run = Cast<AStageSpanActor>(&Actor);
			if (Run && Truss.bSetPath)
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
		Replaces = Fixture.Replaces;
		const bool bUpdate = Replaces.IsValid();
		NewFixtureLayer = bUpdate ? INDEX_NONE : Fixture.Layer;
		Configure = [&Fixture, bUpdate](AModularBaseActor& Actor)
		{
			const bool bRename = !bUpdate || EnumHasAnyFlags(Fixture.Changes, StageMvrImport::EChange::Name);
			Actor.PrepareImported(bRename ? TOptional<FText>(FText::FromString(Fixture.Label)) : TOptional<FText>(), Fixture.MvrId, Fixture.Origin);
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

	// An update takes the old item off first, so the rules judge the stage without it and its fixture ID and address are free.
	const FStageItemSnapshot* Old = Replaces.IsValid() ? TakeOff(Replaces) : nullptr;
	if (Replaces.IsValid() && !Old)
	{
		UE_LOG(LogStageCraft, Log, TEXT("MVR import: the item to update was no longer on the stage; it is placed again."));
	}

	const FStageEconomyResultInfo Verdict = Check ? Check(*Item, Quantity) : FStageEconomyResultInfo::Ok();
	if (!Verdict.IsSuccess())
	{
		Result = Verdict;
		return false;
	}
	// The old state carries over to the same kind of item (another mode of a fixture, the same truss); a UUID that went from a truss to
	// a fixture or back starts afresh. Old points into Removed, which nothing changes during this spawn.
	const UBaseItemData* OldItem = Old ? Old->Item.Get() : nullptr;
	const bool bKeepState = OldItem && OldItem->GetClass() == Item->GetClass();
	AModularBaseActor* Actor = Spawn->SpawnConfiguredItem(Item, Transform, [&Configure, Old, bKeepState](AModularBaseActor& Deferred)
	{
		if (Old && bKeepState)
		{
			// The same item, updated: its identity and everything it carries, then the file's changes on top.
			Deferred.AssignInstanceId(Old->InstanceId);
			Deferred.RestoreSnapshotState(*Old);
		}
		Configure(Deferred);
	});
	if (!Actor)
	{
		Result = FStageEconomyResultInfo::Fail(EStageEconomyResult::SessionRuleViolation, FText::Format(LOCTEXT("SpawnFailed", "{0} could not be placed."), Item->DisplayName));
		return false;
	}
	Placed.Add(Actor);
	PlacedNewFixtureLayer.Add(NewFixtureLayer);
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
	PlacedNewFixtureLayer.Reset();
	// Then what this job took off comes back, newest first, as undo would bring it back.
	for (int32 Index = Removed.Num() - 1; Index >= 0; --Index)
	{
		if (!Spawn || !Spawn->RestoreItem(Removed[Index]))
		{
			UE_LOG(LogStageCraft, Warning, TEXT("MVR import: could not restore %s while rolling back."), *Removed[Index].DisplayName.ToString());
		}
	}
	Removed.Reset();
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
	for (int32 Index = 0; Index < Placed.Num(); ++Index)
	{
		const ALightingFixtureActor* Light = Cast<ALightingFixtureActor>(Placed[Index].Get());
		const int32 Layer = PlacedNewFixtureLayer[Index];
		if (Light && Layer != INDEX_NONE)
		{
			ByLayer.FindOrAdd(Layer).Add(Light->GetFixtureId());
		}
	}
	return ByLayer;
}

#undef LOCTEXT_NAMESPACE
