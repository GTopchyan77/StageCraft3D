// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Sketch/StageSketchTypes.h"

/**
 * Pure 2D helpers of the Sketch window (Docs/ADR/0015-sketch-to-stage.md §2.2-2.3): stroke smoothing and simplification, grid
 * snapping, closing a gesture, picking a stroke, and making untrusted strokes and settings safe. Plan coordinates in cm.
 * No world, no UObjects; covered by the StageCraft.Sketch.Math tests. Deterministic.
 */
namespace StageSketchMath
{
	/** Most strokes a sketch holds. */
	constexpr int32 MaxStrokes = 256;

	/** Most points one stroke keeps (a long freehand drag is simplified well below this). */
	constexpr int32 MaxStrokePoints = 512;

	/** Farthest a point may be from the world origin, in cm (100 m). */
	constexpr double MaxCoordinate = 10000.0;

	/** Consecutive points closer than this (cm) are merged. */
	constexpr double MinPointSpacing = 1.0;

	/** Ramer-Douglas-Peucker tolerance for finished freehand strokes, in cm. */
	constexpr double FreehandTolerance = 15.0;

	/** A freehand stroke that ends this close (cm) to its start is a closed shape. */
	constexpr double CloseDistance = 75.0;

	/** Strongest freehand stabiliser (1 would never move). */
	constexpr float MaxSmoothing = 0.9f;

	/** Setting ranges, in cm. */
	constexpr float MinDeckHeight = 5.f;
	constexpr float MaxDeckHeight = 500.f;
	constexpr float MinTrussHeight = 100.f;
	constexpr float MaxTrussHeight = 2000.f;
	constexpr float MinGridStep = 10.f;
	constexpr float MaxGridStep = 500.f;

	/** Moves the stabilised point toward Raw: Strength 0 jumps to Raw, values near 1 trail behind (a "lazy brush"). */
	MODULARSCENEBUILDER_API FVector2D Stabilize(const FVector2D& Previous, const FVector2D& Raw, double Strength);

	/** Ramer-Douglas-Peucker: drops points within Tolerance of the line through their neighbours. Keeps both ends. */
	MODULARSCENEBUILDER_API TArray<FVector2D> Simplify(TConstArrayView<FVector2D> Points, double Tolerance);

	/** The nearest grid point; Point unchanged for a step that is not positive and finite. */
	MODULARSCENEBUILDER_API FVector2D SnapToGrid(const FVector2D& Point, double Step);

	/** Nearest point of segment A-B to Point; OutAlpha in 0..1 along it. */
	MODULARSCENEBUILDER_API FVector2D ClosestPointOnSegment(const FVector2D& A, const FVector2D& B, const FVector2D& Point, double& OutAlpha);

	/** Distance from Point to the stroke's lines (its closing edge too when closed), or to its only point. Infinite for no points. */
	MODULARSCENEBUILDER_API double DistanceToStroke(const FStageSketchStroke& Stroke, const FVector2D& Point);

	/** True when a freehand gesture of three or more points ends within CloseDistance of its start. */
	MODULARSCENEBUILDER_API bool IsClosedGesture(TConstArrayView<FVector2D> Points);

	/**
	 * Makes an untrusted stroke usable: non-finite points and points beyond MaxCoordinate dropped, close points merged, at most
	 * MaxStrokePoints kept, the deck height clamped to MinDeckHeight..MaxDeckHeight. Then by kind:
	 *  - Freehand: simplified with FreehandTolerance; closed when IsClosedGesture (the end point dropped); needs two points.
	 *  - Shape: always closed; needs three points.
	 *  - Truss: a repeated first point at the end closes it; needs two points (three when closed).
	 *  - Fixture: its first point only.
	 * False when too few points are left; the stroke is then not added.
	 */
	MODULARSCENEBUILDER_API bool SanitizeStroke(FStageSketchStroke& Stroke);

	/** Clamps every setting to its range; non-finite values take the defaults. */
	MODULARSCENEBUILDER_API FStageSketchSettings SanitizeSettings(const FStageSketchSettings& Settings);
}
