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
