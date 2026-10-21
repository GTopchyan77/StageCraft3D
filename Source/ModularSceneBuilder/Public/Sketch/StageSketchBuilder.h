// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/SpawnSystemComponent.h"
#include "Economy/StageEconomyTypes.h"
#include "Sketch/StageSketchTypes.h"

/** What executing a plan did. On failure nothing it spawned is left on the stage. */
struct FStageSketchBuildOutcome
{
	/** Every item spawned, decks first, then truss runs, then fixtures. Empty on failure. */
	TArray<class AModularBaseActor*> Placed;

	int32 DeckCount = 0;
	int32 TrussCount = 0;
	int32 FixtureCount = 0;

	/** Ok, or why the build was refused (the first element the rules or the spawn turned down). */
	FStageEconomyResultInfo Result;

	bool IsSuccess() const { return Result.IsSuccess(); }
};

/**
 * Decides whether one more item of Item, made of Quantity catalog units, may be placed now, without reporting the refusal. The
 * controller passes its placement preview (the GameMode's EvaluatePlacement, not reported); tests pass their own rules.
 */
using FStageSketchPlacementCheck = TFunctionRef<FStageEconomyResultInfo(const class UBaseItemData& /*Item*/, int32 /*Quantity*/)>;

/**
 * Spawns a sketch plan through a player's spawn system (Docs/ADR/0015-sketch-to-stage.md §2.5). All or nothing: each element is
 * checked with Check against the stage as it is after the elements before it, then spawned with USpawnSystemComponent::
 * SpawnConfiguredItem / SpawnItem (whose PlacementValidator asks the rules again with the real quantity). If any element is
 * refused or fails to spawn, every item this call spawned is deleted again and the outcome carries the reason.
 *
 * Records nothing: the caller records the outcome as one undo step. Game thread only.
 */
namespace StageSketchBuilder
{
	MODULARSCENEBUILDER_API FStageSketchBuildOutcome Execute(const FStageSketchBuildPlan& Plan, USpawnSystemComponent& Spawner, FStageSketchPlacementCheck Check);
}
