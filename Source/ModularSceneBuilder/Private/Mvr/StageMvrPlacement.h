// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Economy/StageEconomyTypes.h"
#include "Mvr/StageMvrImportPlan.h"

/**
 * Places an MVR import plan (Docs/ADR/0034 §6, MV2) through a player's spawn system, a slice per frame, so a rig of thousands of
 * fixtures never stalls a frame for long: Step places items until its time budget is used, then returns.
 *
 * All or nothing, like StageSketchBuilder: each item is checked with Check (the GameMode's placement rules, not reported) and
 * spawned with SpawnConfiguredItem (whose validator asks the rules again); the deferred actor gets its label, MVR UUID and, for a
 * fixture, its ID, patch and aim before its data is applied. The first refusal or failed spawn deletes everything this job placed
 * and ends it with the reason. A spawner that goes away (level change) ends it the same way. Records nothing: the owner records the
 * placed items as one undo step.
 *
 * Game thread only. Owned by UStageMvrImportSubsystem; tests drive Step directly.
 */
class FStageMvrPlacementJob
{
public:
	using FCheck = TFunction<FStageEconomyResultInfo(const class UBaseItemData& /*Item*/, int32 /*Quantity*/)>;

	FStageMvrPlacementJob(StageMvrImport::FPlan InPlan, class USpawnSystemComponent& InSpawner, FCheck InCheck);

	/** Places items for at most BudgetSeconds (always at least one). True once the job has finished, either way. */
	bool Step(double BudgetSeconds);

	/** Stops the job and deletes what it placed (Ok becomes a failure with Reason). Safe when finished. */
	void Abort(const FText& Reason);

	bool IsFinished() const { return bFinished; }
	bool Succeeded() const { return bFinished && Result.IsSuccess(); }
	const FStageEconomyResultInfo& GetResult() const { return Result; }

	int32 GetDone() const { return Next; }
	int32 GetTotal() const { return Plan.Num(); }

	/** Placed items in plan order (trusses, then fixtures), valid until the stage changes them. Empty after a failure. */
	const TArray<TWeakObjectPtr<class AModularBaseActor>>& GetPlaced() const { return Placed; }

	/** Fixture IDs placed per layer (by FStageMvrObject::Layer), for the fixture groups. */
	TMap<int32, TArray<int32>> GetFixtureIdsByLayer() const;

	const StageMvrImport::FPlan& GetPlan() const { return Plan; }

	/** Frames (Step calls) used and time spent placing, for the log. */
	int32 GetSteps() const { return Steps; }
	double GetPlacingSeconds() const { return PlacingSeconds; }

private:
	/** Places the item at Index (trusses first, then fixtures). False with Result set on refusal. */
	bool PlaceOne(int32 Index);
	void RollBack();
	void Finish(const FStageEconomyResultInfo& InResult);

	StageMvrImport::FPlan Plan;
	TWeakObjectPtr<class USpawnSystemComponent> Spawner;
	FCheck Check;

	TArray<TWeakObjectPtr<class AModularBaseActor>> Placed;
	int32 Next = 0;
	bool bFinished = false;
	FStageEconomyResultInfo Result;
	int32 Steps = 0;
	double PlacingSeconds = 0.0;
};
