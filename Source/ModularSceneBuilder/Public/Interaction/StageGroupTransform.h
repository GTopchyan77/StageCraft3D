// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * How the rest of a multi-selection follows the item being edited (the leader: the gizmo target or the inspected
 * item). Pure; covered by the StageCraft.Selection.GroupTransform tests (Docs/ADR/0004-selection-scenes-and-rendering.md §3.2).
 *
 * The leader's change from LeaderStart to LeaderNow is split into its parts and applied to each follower:
 *  - move: followers move by the same world offset;
 *  - rotate: followers orbit the leader's pivot by the same world rotation and turn with it, so the group keeps its shape;
 *  - scale: each follower scales in place by the leader's per-axis ratio (its position does not spread or shrink), clamped
 *    by StageTransformRules like every other scale write.
 * A follower's result depends only on its own start transform, never on earlier frames, so a drag never accumulates error.
 */
namespace StageGroupTransform
{
	MODULARSCENEBUILDER_API FTransform ApplyLeaderChange(const FTransform& LeaderStart, const FTransform& LeaderNow, const FTransform& FollowerStart);
}
