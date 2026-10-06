// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Pure bounding-box snapping between stage items (Docs/ADR/0003-undo-redo-and-object-snapping.md).
 * No world access; covered by the StageCraft.Snap.Math tests.
 *
 * Per axis, the moving box locks onto the closest neighbour face within Threshold:
 *  - flush: the moving box's face touches the neighbour's opposite face (side by side, or stacked on Z);
 *  - aligned: both boxes share a min face, a max face or their centre (edges line up).
 * A neighbour only counts on an axis when the two boxes overlap, or are within Threshold, on both other
 * axes, so items across the stage never pull on each other.
 */
namespace StageSnapMath
{
	inline constexpr uint8 AxisBitX = 1 << 0;
	inline constexpr uint8 AxisBitY = 1 << 1;
	inline constexpr uint8 AxisBitZ = 1 << 2;
	/** Placement snaps sideways only: the surface trace already decides the height. */
	inline constexpr uint8 HorizontalAxes = AxisBitX | AxisBitY;
	inline constexpr uint8 AllAxes = AxisBitX | AxisBitY | AxisBitZ;

	inline constexpr uint8 AxisBit(int32 Axis) { return static_cast<uint8>(1u << Axis); }

	/** Which faces lined up. Listed in priority order: on equal distance, the earlier anchor wins. */
	enum class ESnapAnchor : uint8
	{
		FlushAfter,		// moving min touches neighbour max
		FlushBefore,	// moving max touches neighbour min
		AlignMin,
		AlignMax,
		AlignCenter,
	};

	struct FAxisSnap
	{
		bool bSnapped = false;
		/** Offset to add on this axis. */
		double Delta = 0.0;
		/** The world coordinate the two boxes share after snapping (where the guide is drawn). */
		double Plane = 0.0;
		/** Index into the neighbour list. */
		int32 Neighbour = INDEX_NONE;
		ESnapAnchor Anchor = ESnapAnchor::FlushAfter;
	};

	struct FSnapResult
	{
		FAxisSnap Axes[3];

		bool AnySnapped() const;
		FVector GetOffset() const;

		/** Identifies which faces snapped (not how far), to detect a newly engaged snap. Zero when nothing snapped. */
		uint32 GetSignature() const;
	};

	/** True when Neighbour may pull Moving along Axis (overlap or within Threshold on both other axes). */
	MODULARSCENEBUILDER_API bool IsNeighbourRelevant(const FBox& Moving, const FBox& Neighbour, int32 Axis, double Threshold);

	/**
	 * Closest snap per axis in AxisMask. Invalid neighbour boxes are skipped, a non-positive Threshold
	 * snaps nothing. Ties keep the earlier neighbour and anchor, so the result is deterministic for a
	 * given neighbour order.
	 */
	MODULARSCENEBUILDER_API FSnapResult ComputeSnap(const FBox& Moving, TConstArrayView<FBox> Neighbours, double Threshold, uint8 AxisMask);

	/** Per axis: snapped axes take FreeLocation + delta (the snap was measured there), the others keep GridLocation. */
	MODULARSCENEBUILDER_API FVector MergeWithGrid(const FVector& FreeLocation, const FVector& GridLocation, const FSnapResult& Snap);

	/**
	 * Guide segment for a snap on Axis at Plane: lies in that plane and spans both boxes along the next
	 * axis (X -> Y, Y -> X, Z -> X). Side snaps sit at the bottom of the shared height; Z snaps at the
	 * middle of the shared Y range.
	 */
	MODULARSCENEBUILDER_API void MakeGuide(int32 Axis, double Plane, const FBox& SnappedMoving, const FBox& Neighbour, FVector& OutStart, FVector& OutEnd);
}
