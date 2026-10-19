// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Data/StageItemTypes.h"

/**
 * Pure placement rules shared by the placement preview and the spawn, so what the ghost shows is
 * exactly where the item lands. No world access; covered by StageCraft.Placement.Math tests.
 */
namespace StagePlacementMath
{
	/**
	 * Snaps ImpactPoint to the item's grid (per axis; a zero cell size disables that axis), optionally
	 * aligns the item's up axis to ImpactNormal, then applies PlacementOffset in the item's local space.
	 */
	MODULARSCENEBUILDER_API FTransform ComputePlacementTransform(const FStageItemPlacementRules& Rules, const FVector& ImpactPoint, const FVector& ImpactNormal);

	/** True when at least one axis of the rules snaps to a grid. Snap feedback is only meaningful then. */
	MODULARSCENEBUILDER_API bool UsesGridSnapping(const FStageItemPlacementRules& Rules);

	/**
	 * The item's rules under the toolbar's global grid (Docs/ADR/0008 §4.5): X and Y snap to GridCm, or not at all when
	 * GridCm <= 0 (Off). Z, the pivot offset and surface alignment stay the item's own. Non-finite GridCm counts as Off.
	 */
	MODULARSCENEBUILDER_API FStageItemPlacementRules WithGlobalGrid(const FStageItemPlacementRules& Rules, double GridCm);
}
