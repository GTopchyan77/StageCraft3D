// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
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
	};

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

	/**
	 * Actor-local transforms of one segment part, once per segment. Part is authored for a segment of SegmentLength starting at
	 * X = 0; stretched runs scale it along the run's X. OutTransforms is replaced.
	 */
	MODULARSCENEBUILDER_API void MakeSegmentInstances(const FStageSpanLayout& Layout, double SegmentLength, const FTransform& Part, TArray<FTransform>& OutTransforms);

	/** Actor-local transforms of one joint part at every joint, both ends included (SegmentCount + 1). OutTransforms is replaced. */
	MODULARSCENEBUILDER_API void MakeJointInstances(const FStageSpanLayout& Layout, const FTransform& Part, TArray<FTransform>& OutTransforms);
}
