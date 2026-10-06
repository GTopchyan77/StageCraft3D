// Copyright Epic Games, Inc. All Rights Reserved.

#include "Placement/StageSnapMath.h"

namespace StageSnapMath
{
	namespace
	{
		struct FCandidate
		{
			double Delta;
			double Plane;
			ESnapAnchor Anchor;
		};

		bool IntervalsTouch(double MinA, double MaxA, double MinB, double MaxB, double Margin)
		{
			return MinA - Margin <= MaxB && MinB <= MaxA + Margin;
		}
	}

	bool FSnapResult::AnySnapped() const
	{
		return Axes[0].bSnapped || Axes[1].bSnapped || Axes[2].bSnapped;
	}

	FVector FSnapResult::GetOffset() const
	{
		return FVector(
			Axes[0].bSnapped ? Axes[0].Delta : 0.0,
			Axes[1].bSnapped ? Axes[1].Delta : 0.0,
			Axes[2].bSnapped ? Axes[2].Delta : 0.0);
	}

	uint32 FSnapResult::GetSignature() const
	{
		if (!AnySnapped())
		{
			return 0;
		}

		uint32 Signature = 0;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const FAxisSnap& Snap = Axes[Axis];
			Signature = HashCombine(Signature, Snap.bSnapped ? GetTypeHash(Snap.Neighbour) ^ (static_cast<uint32>(Snap.Anchor) << 24) : 0xFFFFFFFFu);
		}
		// Never zero while snapped, so zero can mean "not snapped".
		return Signature | 1u;
	}

	bool IsNeighbourRelevant(const FBox& Moving, const FBox& Neighbour, int32 Axis, double Threshold)
	{
		for (int32 Other = 0; Other < 3; ++Other)
		{
			if (Other != Axis && !IntervalsTouch(Moving.Min[Other], Moving.Max[Other], Neighbour.Min[Other], Neighbour.Max[Other], Threshold))
			{
				return false;
			}
		}
		return true;
	}

	FSnapResult ComputeSnap(const FBox& Moving, TConstArrayView<FBox> Neighbours, double Threshold, uint8 AxisMask)
	{
		FSnapResult Result;
		if (!Moving.IsValid || !(Threshold > 0.0))
		{
			return Result;
		}

		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (!(AxisMask & AxisBit(Axis)))
			{
				continue;
			}

			FAxisSnap& Best = Result.Axes[Axis];
			for (int32 Index = 0; Index < Neighbours.Num(); ++Index)
			{
				const FBox& Neighbour = Neighbours[Index];
				if (!Neighbour.IsValid || !IsNeighbourRelevant(Moving, Neighbour, Axis, Threshold))
				{
					continue;
				}

				const double NeighbourCenter = 0.5 * (Neighbour.Min[Axis] + Neighbour.Max[Axis]);
				const double MovingCenter = 0.5 * (Moving.Min[Axis] + Moving.Max[Axis]);
				const FCandidate Candidates[] =
				{
					{ Neighbour.Max[Axis] - Moving.Min[Axis], Neighbour.Max[Axis], ESnapAnchor::FlushAfter },
					{ Neighbour.Min[Axis] - Moving.Max[Axis], Neighbour.Min[Axis], ESnapAnchor::FlushBefore },
					{ Neighbour.Min[Axis] - Moving.Min[Axis], Neighbour.Min[Axis], ESnapAnchor::AlignMin },
					{ Neighbour.Max[Axis] - Moving.Max[Axis], Neighbour.Max[Axis], ESnapAnchor::AlignMax },
					{ NeighbourCenter - MovingCenter, NeighbourCenter, ESnapAnchor::AlignCenter },
				};

				for (const FCandidate& Candidate : Candidates)
				{
					const double Distance = FMath::Abs(Candidate.Delta);
					const bool bCloser = Best.bSnapped ? Distance < FMath::Abs(Best.Delta) : Distance <= Threshold;
					if (bCloser)
					{
						Best.bSnapped = true;
						Best.Delta = Candidate.Delta;
						Best.Plane = Candidate.Plane;
						Best.Neighbour = Index;
						Best.Anchor = Candidate.Anchor;
					}
				}
			}
		}
		return Result;
	}

	FVector MergeWithGrid(const FVector& FreeLocation, const FVector& GridLocation, const FSnapResult& Snap)
	{
		FVector Result = GridLocation;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (Snap.Axes[Axis].bSnapped)
			{
				Result[Axis] = FreeLocation[Axis] + Snap.Axes[Axis].Delta;
			}
		}
		return Result;
	}

	void MakeGuide(int32 Axis, double Plane, const FBox& SnappedMoving, const FBox& Neighbour, FVector& OutStart, FVector& OutEnd)
	{
		const int32 SpanAxis = Axis == 0 ? 1 : 0;
		const int32 DepthAxis = 3 - Axis - SpanAxis;

		OutStart[Axis] = Plane;
		OutEnd[Axis] = Plane;
		OutStart[SpanAxis] = FMath::Min(SnappedMoving.Min[SpanAxis], Neighbour.Min[SpanAxis]);
		OutEnd[SpanAxis] = FMath::Max(SnappedMoving.Max[SpanAxis], Neighbour.Max[SpanAxis]);

		const double SharedMin = FMath::Max(SnappedMoving.Min[DepthAxis], Neighbour.Min[DepthAxis]);
		const double SharedMax = FMath::Min(SnappedMoving.Max[DepthAxis], Neighbour.Max[DepthAxis]);
		const bool bShared = SharedMin <= SharedMax;

		double Depth = 0.0;
		if (Axis == 2)
		{
			Depth = bShared ? 0.5 * (SharedMin + SharedMax) : 0.5 * (SnappedMoving.Min[DepthAxis] + SnappedMoving.Max[DepthAxis]);
		}
		else
		{
			// Side snaps: DepthAxis is Z. The bottom of the shared height is on the floor for side-by-side
			// items and on the lower item's top face for stacked ones, where the user is looking.
			Depth = bShared ? SharedMin : SnappedMoving.Min[DepthAxis];
		}
		OutStart[DepthAxis] = Depth;
		OutEnd[DepthAxis] = Depth;
	}
}
