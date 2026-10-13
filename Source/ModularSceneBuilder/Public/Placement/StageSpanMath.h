// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Data/StageSpanData.h"

/** A run laid out for a requested length: what is built, counted and billed. */
struct FStageSpanLayout
{
	int32 SegmentCount = 0;

	/** Distance between joints in cm: the segment length, or length / count when stretched. */
	double SegmentPitch = 0.0;

	/** The run's real length in cm (SegmentCount * SegmentPitch). */
	double Length = 0.0;

	bool IsValid() const { return SegmentCount > 0; }

	bool operator==(const FStageSpanLayout& Other) const
	{
		return SegmentCount == Other.SegmentCount && SegmentPitch == Other.SegmentPitch && Length == Other.Length;
	}
};

/**
 * A drawn path laid out (Docs/ADR/0007-lighting-patch-paths-and-pipeline-hardening.md §4): straight legs between vertices,
 * each a run of whole (or stretched) segments. What a path item builds, counts and bills.
 */
struct FStageSpanPathLayout
{
	/** Normalised vertices in the path's local space; Points[0] is the origin. A closed path does not repeat its first point. */
	TArray<FVector> Points;

	/** One run per leg: Points[i] -> Points[i + 1], plus Points.Last() -> Points[0] when closed. */
	TArray<FStageSpanLayout> Legs;

	bool bClosed = false;

	/** Requested mid-span sag of every leg in cm (each leg is limited to MaxSagFraction of its length). */
	double SagDepth = 0.0;

	/** Deepest sag a leg may have, as a fraction of its length; 0 = the item does not sag. */
	double MaxSagFraction = 0.0;

	bool IsValid() const { return !Legs.IsEmpty(); }

	int32 GetSegmentCount() const;

	/** Total length along the legs' chords, in cm. */
	double GetLength() const;

	/** Start and end vertex of a leg (the end wraps to Points[0] on a closed path's last leg). */
	FVector GetLegStart(int32 Leg) const { return Points[Leg]; }
	FVector GetLegEnd(int32 Leg) const { return Points[(Leg + 1) % Points.Num()]; }

	/** The sag of one leg after its length limit, in cm. */
	double GetLegSag(int32 Leg) const;

	bool operator==(const FStageSpanPathLayout& Other) const
	{
		return Points == Other.Points && Legs == Other.Legs && bClosed == Other.bClosed && SagDepth == Other.SagDepth && MaxSagFraction == Other.MaxSagFraction;
	}
};

/**
 * Pure geometry of spans (Docs/ADR/0005-spans-outliner-and-bill-of-materials.md §2): layout of a run, its placement
 * constraints and the local transforms of every repeated part. No world, no UObjects beyond reading an asset's rules;
 * covered by the StageCraft.Span tests. Deterministic: the same inputs always give the same run.
 */
namespace StageSpanMath
{
	/** The layout rules of a span asset, as plain values so the math is testable without assets. */
	struct FRules
	{
		EStageSpanFit Fit = EStageSpanFit::WholeSegments;
		double SegmentLength = 0.0;
		int32 MaxSegments = 0;

		/** Deepest sag as a fraction of a leg's length (0 = rigid). Only stretched items sag. */
		double MaxSagFraction = 0.0;
	};

	/** Most vertices a path may have (drawn or loaded); later points are dropped. */
	constexpr int32 MaxPathPoints = 64;

	/** A closed path's closing leg must come within this (cm) of a whole number of segments for whole-segment items. */
	constexpr double ClosureTolerance = 1.0;

	/** Vertices closer than this (cm) to the previous one are dropped: the leg would have no direction. */
	constexpr double MinLegLength = 1.0;

	MODULARSCENEBUILDER_API FRules MakeRules(const UStageSpanData& Data);

	/** Shortest and longest run the rules allow, in cm. Zero for invalid rules. */
	MODULARSCENEBUILDER_API double GetMinLength(const FRules& Rules);
	MODULARSCENEBUILDER_API double GetMaxLength(const FRules& Rules);

	/**
	 * The run for RequestedLength (cm), always at least one segment. Whole segments: the nearest whole count of exact segments.
	 * Stretch: the nearest whole count, each segment stretched to fill the length exactly (at most 50 % for a single segment).
	 * The length is clamped to the rules' range; a non-finite length is treated as one segment. Invalid rules give an invalid layout.
	 */
	MODULARSCENEBUILDER_API FStageSpanLayout ComputeLayout(const FRules& Rules, double RequestedLength);

	/**
	 * Where a run from Start toward Cursor ends: in the horizontal plane through Start (runs are drawn level and tilted afterwards
	 * with the gizmo), with the direction snapped to multiples of AngleStepDegrees (0 = free) and the length projected onto it,
	 * like AutoCAD's polar tracking. Returns Start when the cursor is (horizontally) on the start point.
	 */
	MODULARSCENEBUILDER_API FVector ConstrainEndPoint(const FVector& Start, const FVector& Cursor, double AngleStepDegrees);

	/** Actor transform of a run from Start toward End: located at Start, +X pointing at End, level, unit scale. FallbackYawDegrees when End is on Start. */
	MODULARSCENEBUILDER_API FTransform MakeRunTransform(const FVector& Start, const FVector& End, double FallbackYawDegrees);

	/** The yaw a run from Start toward End points at, or FallbackYawDegrees when End is (horizontally) on Start. */
	MODULARSCENEBUILDER_API double GetRunYaw(const FVector& Start, const FVector& End, double FallbackYawDegrees);

	// --- Paths ---

	/**
	 * Lays out a drawn path. Points are local, the first at the origin. Untrusted input is made safe: non-finite points and
	 * points closer than MinLegLength to the previous one are dropped, at most MaxPathPoints are kept, and the total segment
	 * count is capped by the rules (later legs are dropped).
	 *  - Whole segments: each leg is shortened or lengthened to a whole number of segments along its own direction, and every
	 *    later point moves by the same offset, so the drawn shape is kept.
	 *  - Closed: needs three or more vertices; for whole-segment items the closing leg must also be whole (within
	 *    ClosureTolerance), otherwise the path stays open.
	 *  - Sag is kept only for stretched items with MaxSagFraction > 0, clamped to 0 or more.
	 * Fewer than two usable points give an invalid layout. Deterministic.
	 */
	MODULARSCENEBUILDER_API FStageSpanPathLayout ComputePathLayout(const FRules& Rules, TConstArrayView<FVector> Points, bool bClosed, double SagDepth);

	/** True when Points (already normalised by ComputePathLayout) can be closed: three or more vertices and a whole closing leg. */
	MODULARSCENEBUILDER_API bool CanClosePath(const FRules& Rules, TConstArrayView<FVector> Points);

	/**
	 * Where a leg from Start toward Cursor ends for these rules: polar-snapped like ConstrainEndPoint, then shortened or
	 * lengthened to a whole number of segments (whole-segment items). Returns Start when the cursor is on it.
	 */
	MODULARSCENEBUILDER_API FVector ConstrainPathPoint(const FRules& Rules, const FVector& Start, const FVector& Cursor, double AngleStepDegrees);

	/** A point on leg Leg at parameter T (0 start .. 1 end), sagging by the leg's sag. */
	MODULARSCENEBUILDER_API FVector GetPathLegPoint(const FStageSpanPathLayout& Layout, int32 Leg, double T);

	/**
	 * Local transforms of one segment part, once per segment of every leg. Part is authored for a segment of SegmentLength
	 * starting at X = 0 along +X. Each segment spans the chord between consecutive
	 * joints (straight legs: along the leg; sagging legs: between points on the curve), stretched to the chord length, so
	 * neighbouring segments meet exactly at their joints. OutTransforms is replaced.
	 */
	MODULARSCENEBUILDER_API void MakePathSegmentInstances(const FStageSpanPathLayout& Layout, double SegmentLength, const FTransform& Part, TArray<FTransform>& OutTransforms);

	/**
	 * Local frames of every joint (location + orientation along the run): OutJoints gets the joints between segments and the two
	 * ends of an open path; OutCorners gets the interior vertices (every vertex of a closed path), turned to the bisector of the
	 * two legs. No joint is listed twice. Either array is replaced.
	 */
	MODULARSCENEBUILDER_API void MakePathJointFrames(const FStageSpanPathLayout& Layout, TArray<FTransform>& OutJoints, TArray<FTransform>& OutCorners);
}
