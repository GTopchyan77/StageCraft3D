// Copyright Epic Games, Inc. All Rights Reserved.

#include "Placement/StageSpanMath.h"

namespace StageSpanMath
{
	namespace
	{
		/** A stretched single segment may shrink to half its authored length; shorter would distort it beyond recognition. */
		constexpr double MinStretchFactor = 0.5;

		/** Horizontal distances below this (cm) have no direction. */
		constexpr double DirectionTolerance = 0.01;

		bool AreRulesValid(const FRules& Rules)
		{
			return FMath::IsFinite(Rules.SegmentLength) && Rules.SegmentLength > 0.0 && Rules.MaxSegments >= 1;
		}

		int32 GetSegmentCap(const FRules& Rules)
		{
			return FMath::Min(Rules.MaxSegments, UStageSpanData::AbsoluteMaxSegments);
		}
	}

	FRules MakeRules(const UStageSpanData& Data)
	{
		FRules Rules;
		Rules.Fit = Data.Fit;
		Rules.SegmentLength = Data.SegmentLength;
		Rules.MaxSegments = Data.MaxSegments;
		Rules.MaxSagFraction = FMath::Clamp(Data.MaxSagPercent, 0.f, 50.f) * 0.01;
		return Rules;
	}

	double GetMinLength(const FRules& Rules)
	{
		if (!AreRulesValid(Rules))
		{
			return 0.0;
		}
		return Rules.Fit == EStageSpanFit::Stretch ? Rules.SegmentLength * MinStretchFactor : Rules.SegmentLength;
	}

	double GetMaxLength(const FRules& Rules)
	{
		return AreRulesValid(Rules) ? Rules.SegmentLength * GetSegmentCap(Rules) : 0.0;
	}

	FStageSpanLayout ComputeLayout(const FRules& Rules, double RequestedLength)
	{
		FStageSpanLayout Layout;
		if (!AreRulesValid(Rules))
		{
			return Layout;
		}

		// Untrusted lengths (scene files, console) never produce more than the cap or less than one segment.
		const double Requested = FMath::Clamp(FMath::IsFinite(RequestedLength) ? RequestedLength : Rules.SegmentLength,
			GetMinLength(Rules), GetMaxLength(Rules));
		Layout.SegmentCount = FMath::Clamp(FMath::RoundToInt32(Requested / Rules.SegmentLength), 1, GetSegmentCap(Rules));

		if (Rules.Fit == EStageSpanFit::Stretch)
		{
			Layout.Length = Requested;
			Layout.SegmentPitch = Requested / Layout.SegmentCount;
		}
		else
		{
			Layout.SegmentPitch = Rules.SegmentLength;
			Layout.Length = Rules.SegmentLength * Layout.SegmentCount;
		}
		return Layout;
	}

	FVector ConstrainEndPoint(const FVector& Start, const FVector& Cursor, double AngleStepDegrees)
	{
		const FVector Delta(Cursor.X - Start.X, Cursor.Y - Start.Y, 0.0);
		const double Distance = Delta.Size2D();
		if (Distance < DirectionTolerance)
		{
			return Start;
		}

		if (!(AngleStepDegrees > 0.0) || !FMath::IsFinite(AngleStepDegrees))
		{
			return Start + Delta;
		}

		const double Yaw = FMath::RadiansToDegrees(FMath::Atan2(Delta.Y, Delta.X));
		const double SnappedYaw = FMath::GridSnap(Yaw, AngleStepDegrees);
		const FVector Direction(FMath::Cos(FMath::DegreesToRadians(SnappedYaw)), FMath::Sin(FMath::DegreesToRadians(SnappedYaw)), 0.0);
		// Projected, not the raw distance: the end stays where the cursor is along the snapped line.
		return Start + Direction * FMath::Max(0.0, FVector::DotProduct(Delta, Direction));
	}

	double GetRunYaw(const FVector& Start, const FVector& End, double FallbackYawDegrees)
	{
		const FVector2D Delta(End.X - Start.X, End.Y - Start.Y);
		return Delta.Size() < DirectionTolerance ? FallbackYawDegrees : FMath::RadiansToDegrees(FMath::Atan2(Delta.Y, Delta.X));
	}

	FTransform MakeRunTransform(const FVector& Start, const FVector& End, double FallbackYawDegrees)
	{
		return FTransform(FRotator(0.0, GetRunYaw(Start, End, FallbackYawDegrees), 0.0), Start, FVector::OneVector);
	}

	// --- Paths ---

	namespace
	{
		bool IsFinitePoint(const FVector& Point)
		{
			return FMath::IsFinite(Point.X) && FMath::IsFinite(Point.Y) && FMath::IsFinite(Point.Z);
		}

		/** The rules with only SegmentsLeft segments available, so a path's legs share the item's cap. */
		FRules LimitTo(const FRules& Rules, int32 SegmentsLeft)
		{
			FRules Limited = Rules;
			Limited.MaxSegments = FMath::Min(GetSegmentCap(Rules), SegmentsLeft);
			return Limited;
		}

		/**
		 * The closing leg from the last vertex to the first, or an invalid layout when the path cannot close: too few vertices,
		 * no segments left, or (whole segments) a closing distance that is not a whole number of segments.
		 */
		FStageSpanLayout LayoutClosingLeg(const FRules& Rules, TConstArrayView<FVector> Vertices, int32 SegmentsLeft)
		{
			if (Vertices.Num() < 3 || SegmentsLeft <= 0)
			{
				return FStageSpanLayout();
			}
			const double Distance = FVector::Dist(Vertices.Last(), Vertices[0]);
			if (Distance < MinLegLength)
			{
				return FStageSpanLayout();
			}
			const FStageSpanLayout Leg = ComputeLayout(LimitTo(Rules, SegmentsLeft), Distance);
			// A clamped leg (too short to stretch, over the cap) or a whole-segment gap would leave the loop visibly open.
			return Leg.IsValid() && FMath::IsNearlyEqual(Leg.Length, Distance, ClosureTolerance) ? Leg : FStageSpanLayout();
		}

		/** Direction of a leg at parameter T, including the slope of its sag. */
		FVector GetLegTangent(const FStageSpanPathLayout& Layout, int32 Leg, double T)
		{
			const double Sag = Layout.GetLegSag(Leg);
			return (Layout.GetLegEnd(Leg) - Layout.GetLegStart(Leg)) - FVector::UpVector * (4.0 * Sag * (1.0 - 2.0 * T));
		}

		FTransform MakeFrame(const FVector& Location, const FVector& Direction)
		{
			// MakeFromX keeps world Z as up: sloped chords pitch, they never roll.
			return FTransform(FRotationMatrix::MakeFromX(Direction).Rotator(), Location);
		}
	}

	FStageSpanPathLayout ComputePathLayout(const FRules& Rules, TConstArrayView<FVector> Points, bool bClosed, double SagDepth)
	{
		FStageSpanPathLayout Path;
		if (!AreRulesValid(Rules))
		{
			return Path;
		}

		TArray<FVector, TInlineAllocator<16>> Drawn;
		for (const FVector& Point : Points)
		{
			if (Drawn.Num() >= MaxPathPoints)
			{
				break;
			}
			if (IsFinitePoint(Point) && (Drawn.IsEmpty() || FVector::Dist(Point, Drawn.Last()) >= MinLegLength))
			{
				Drawn.Add(Point);
			}
		}
		if (Drawn.Num() < 2)
		{
			return Path;
		}

		// Each leg keeps its drawn direction and gets its normalised length; later points follow by the same offset.
		Path.Points.Add(Drawn[0]);
		FVector Offset = FVector::ZeroVector;
		int32 SegmentsLeft = GetSegmentCap(Rules);
		for (int32 Index = 1; Index < Drawn.Num() && SegmentsLeft > 0; ++Index)
		{
			const FVector Start = Path.Points.Last();
			const FVector Delta = Drawn[Index] + Offset - Start;
			const double Distance = Delta.Size();
			if (Distance < MinLegLength)
			{
				continue;
			}
			const FStageSpanLayout Leg = ComputeLayout(LimitTo(Rules, SegmentsLeft), Distance);
			if (!Leg.IsValid())
			{
				break;
			}
			const FVector End = Start + Delta / Distance * Leg.Length;
			Path.Legs.Add(Leg);
			Path.Points.Add(End);
			Offset = End - Drawn[Index];
			SegmentsLeft -= Leg.SegmentCount;
		}
		if (Path.Legs.IsEmpty())
		{
			return FStageSpanPathLayout();
		}

		if (bClosed)
		{
			const FStageSpanLayout Closing = LayoutClosingLeg(Rules, Path.Points, SegmentsLeft);
			if (Closing.IsValid())
			{
				Path.Legs.Add(Closing);
				Path.bClosed = true;
			}
		}

		Path.MaxSagFraction = Rules.Fit == EStageSpanFit::Stretch ? FMath::Clamp(Rules.MaxSagFraction, 0.0, 0.5) : 0.0;
		if (Path.MaxSagFraction > 0.0 && FMath::IsFinite(SagDepth))
		{
			// Stored no deeper than the longest leg allows, so the value saved is the deepest sag actually shown.
			double LongestLeg = 0.0;
			for (int32 Leg = 0; Leg < Path.Legs.Num(); ++Leg)
			{
				LongestLeg = FMath::Max(LongestLeg, FVector::Dist(Path.GetLegStart(Leg), Path.GetLegEnd(Leg)));
			}
			Path.SagDepth = FMath::Clamp(SagDepth, 0.0, Path.MaxSagFraction * LongestLeg);
		}
		return Path;
	}

	bool CanClosePath(const FRules& Rules, TConstArrayView<FVector> Points)
	{
		return ComputePathLayout(Rules, Points, /*bClosed*/ true, 0.0).bClosed;
	}

	FVector ConstrainPathPoint(const FRules& Rules, const FVector& Start, const FVector& Cursor, double AngleStepDegrees)
	{
		const FVector End = ConstrainEndPoint(Start, Cursor, AngleStepDegrees);
		const double Distance = FVector::Dist(Start, End);
		if (Distance < DirectionTolerance || !AreRulesValid(Rules))
		{
			return End;
		}
		// The same normalisation ComputePathLayout applies, so the point the user sees is the vertex that is stored.
		const FStageSpanLayout Leg = ComputeLayout(Rules, Distance);
		return Start + (End - Start) / Distance * Leg.Length;
	}

	FVector GetPathLegPoint(const FStageSpanPathLayout& Layout, int32 Leg, double T)
	{
		const FVector Start = Layout.GetLegStart(Leg);
		const FVector End = Layout.GetLegEnd(Leg);
		// Parabolic sag, depth Sag at mid-span: close to a catenary for the shallow sags of cables between supports.
		return FMath::Lerp(Start, End, T) - FVector::UpVector * (4.0 * Layout.GetLegSag(Leg) * T * (1.0 - T));
	}

	void MakePathSegmentInstances(const FStageSpanPathLayout& Layout, double SegmentLength, const FTransform& Part, TArray<FTransform>& OutTransforms)
	{
		OutTransforms.Reset(Layout.GetSegmentCount());
		if (!Layout.IsValid() || !(SegmentLength > 0.0))
		{
			return;
		}

		for (int32 Leg = 0; Leg < Layout.Legs.Num(); ++Leg)
		{
			const int32 Count = Layout.Legs[Leg].SegmentCount;
			for (int32 Segment = 0; Segment < Count; ++Segment)
			{
				const FVector From = GetPathLegPoint(Layout, Leg, static_cast<double>(Segment) / Count);
				const FVector To = GetPathLegPoint(Layout, Leg, static_cast<double>(Segment + 1) / Count);
				const FVector Chord = To - From;
				const double ChordLength = Chord.Size();
				if (ChordLength <= UE_DOUBLE_KINDA_SMALL_NUMBER)
				{
					continue;
				}

				// The stretch is applied in the segment's frame, after the part's own rotation: a part lying along the run (a chord, a
				// cable) gets longer, not thicker. Rotated parts (diagonal lacing) are approximated, since FTransform cannot hold shear.
				const FTransform Frame = MakeFrame(From, Chord);
				const double Stretch = ChordLength / SegmentLength;
				OutTransforms.Add(FMath::IsNearlyEqual(Stretch, 1.0)
					? Part * Frame
					: FTransform(Part.ToMatrixWithScale() * FScaleMatrix(FVector(Stretch, 1.0, 1.0)) * Frame.ToMatrixNoScale()));
			}
		}
	}

	void MakePathJointFrames(const FStageSpanPathLayout& Layout, TArray<FTransform>& OutJoints, TArray<FTransform>& OutCorners)
	{
		OutJoints.Reset();
		OutCorners.Reset();
		if (!Layout.IsValid())
		{
			return;
		}

		// Joints inside each leg, between its segments.
		for (int32 Leg = 0; Leg < Layout.Legs.Num(); ++Leg)
		{
			const int32 Count = Layout.Legs[Leg].SegmentCount;
			for (int32 Joint = 1; Joint < Count; ++Joint)
			{
				const double T = static_cast<double>(Joint) / Count;
				OutJoints.Add(MakeFrame(GetPathLegPoint(Layout, Leg, T), GetLegTangent(Layout, Leg, T)));
			}
		}

		// Vertices: the ends of an open path are joints; every other vertex is a corner between its two legs.
		const int32 VertexCount = Layout.Points.Num();
		const int32 LegCount = Layout.Legs.Num();
		for (int32 Vertex = 0; Vertex < VertexCount; ++Vertex)
		{
			const bool bHasIncoming = Layout.bClosed || Vertex > 0;
			const bool bHasOutgoing = Vertex < LegCount;
			const int32 Incoming = (Vertex - 1 + LegCount) % LegCount;
			if (bHasIncoming && bHasOutgoing)
			{
				FVector Bisector = GetLegTangent(Layout, Incoming, 1.0).GetSafeNormal() + GetLegTangent(Layout, Vertex, 0.0).GetSafeNormal();
				if (Bisector.IsNearlyZero())
				{
					// A U-turn has no bisector; face along the incoming leg.
					Bisector = GetLegTangent(Layout, Incoming, 1.0);
				}
				OutCorners.Add(MakeFrame(Layout.Points[Vertex], Bisector));
			}
			else
			{
				OutJoints.Add(MakeFrame(Layout.Points[Vertex], bHasOutgoing ? GetLegTangent(Layout, Vertex, 0.0) : GetLegTangent(Layout, Incoming, 1.0)));
			}
		}
	}
}

int32 FStageSpanPathLayout::GetSegmentCount() const
{
	int32 Count = 0;
	for (const FStageSpanLayout& Leg : Legs)
	{
		Count += Leg.SegmentCount;
	}
	return Count;
}

double FStageSpanPathLayout::GetLength() const
{
	double Length = 0.0;
	for (int32 Leg = 0; Leg < Legs.Num(); ++Leg)
	{
		Length += FVector::Dist(GetLegStart(Leg), GetLegEnd(Leg));
	}
	return Length;
}

double FStageSpanPathLayout::GetLegSag(int32 Leg) const
{
	if (!(MaxSagFraction > 0.0) || !(SagDepth > 0.0) || !Legs.IsValidIndex(Leg))
	{
		return 0.0;
	}
	return FMath::Min(SagDepth, MaxSagFraction * FVector::Dist(GetLegStart(Leg), GetLegEnd(Leg)));
}
