// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Transform limits shared by every write path (gizmo drags, inspector fields), so a value is clamped
 * the same way whichever tool produced it. Pure; covered by StageCraft.Gizmo.ScaleRules tests.
 */
namespace StageTransformRules
{
	/** Per-axis scale limits. Below the minimum an item collapses to an unselectable sliver. */
	inline constexpr double MinScale = 0.01;
	inline constexpr double MaxScale = 100.0;

	/** 10 km in cm: anything farther from the origin is not a stage position but data damage or a runaway edit. */
	inline constexpr double MaxCoordinate = 1.0e6;

	/** Clamps every component into [MinScale, MaxScale]. Non-finite components become 1. */
	inline FVector ClampScale(const FVector& Scale)
	{
		auto ClampAxis = [](double Value)
		{
			return FMath::IsFinite(Value) ? FMath::Clamp(Value, MinScale, MaxScale) : 1.0;
		};
		return FVector(ClampAxis(Scale.X), ClampAxis(Scale.Y), ClampAxis(Scale.Z));
	}

	/**
	 * Whether a transform may be written to a stage item at all: every component finite, the location within MaxCoordinate
	 * of the origin, and a rotation that is not degenerate. Scale is not judged here; writers clamp it with ClampScale.
	 * Used where a transform is computed rather than typed (group followers, undo/redo, files).
	 */
	inline bool IsSaneTransform(const FTransform& Transform)
	{
		const FVector Location = Transform.GetLocation();
		const FQuat Rotation = Transform.GetRotation();
		return !Location.ContainsNaN() && Location.GetAbsMax() <= MaxCoordinate
			&& !Rotation.ContainsNaN() && Rotation.SizeSquared() > UE_KINDA_SMALL_NUMBER
			&& !Transform.GetScale3D().ContainsNaN();
	}

	/**
	 * Multiplier for a scale-handle drag: dragging the handle by its own length outwards doubles the
	 * scale, dragging it back onto the pivot approaches zero (floored at MinScale). A non-positive
	 * handle length means no reference, so the scale is left unchanged (1).
	 */
	inline double ComputeDragScaleFactor(double DragDistance, double HandleLength)
	{
		if (!(HandleLength > 0.0) || !FMath::IsFinite(DragDistance))
		{
			return 1.0;
		}
		return FMath::Max(1.0 + DragDistance / HandleLength, MinScale);
	}
}
