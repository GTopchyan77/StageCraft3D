// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Sketch/StageSketchTypes.h"

/** What a plan is built with: the settings as plain values and the catalog item chosen for each element. */
struct FStageSketchBuildRules
{
	/** Snap step of deck corners, fixtures and freehand truss points, in cm. */
	double GridStep = 50.0;
	bool bSnapToGrid = true;

	/** World Z of the floor decks and floor fixtures stand on, in cm. */
	double FloorHeight = 0.0;

	/** World Z of the truss, in cm. */
	double TrussHeight = 600.0;

	/** A fixture marker this close (cm, horizontally) to a planned truss hangs from it. */
	double TrussAttachDistance = 75.0;

	/** Null means the catalog has none: strokes of that element are skipped with a warning. */
	class UStageDeckData* DeckItem = nullptr;
	class UStageSpanData* TrussItem = nullptr;
	class UBaseItemData* FixtureItem = nullptr;
};

/**
 * Turns sketch strokes into a build plan (Docs/ADR/0015-sketch-to-stage.md §2.4). Pure: reads item assets' rules only, no world;
 * covered by the StageCraft.Sketch.Plan tests. Deterministic: the same strokes and rules always give the same plan.
 *
 *  - Decks: corners snapped to the grid, the outline sanitized (StageDeckMath::SanitizeOutline: a self-crossing or too small
 *    shape is skipped), origin at the centroid on the floor, the stroke's height clamped to the item's range.
 *  - Truss: points at the truss height; truss-tool points snap to the item's section length for whole-section items (so a drawn
 *    rectangle closes) and to the grid otherwise; laid out by StageSpanMath::ComputePathLayout as the actor will be.
 *  - Fixtures: hung from the nearest truss within TrussAttachDistance (projected onto it, rolled 180°), else on the highest
 *    deck that contains them, else on the floor. Two fixtures on the same spot keep the first.
 */
namespace StageSketchGenerator
{
	/** Two fixtures closer than this (cm) are one; the later one is skipped. */
	constexpr double DuplicateFixtureDistance = 1.0;

	MODULARSCENEBUILDER_API FStageSketchBuildPlan BuildPlan(TConstArrayView<FStageSketchStroke> Strokes, const FStageSketchBuildRules& Rules);

	/** "2 decks (12 panels), 1 truss run (14 sections), 8 fixtures", or "Nothing to build". */
	MODULARSCENEBUILDER_API FText DescribePlan(const FStageSketchBuildPlan& Plan);
}
