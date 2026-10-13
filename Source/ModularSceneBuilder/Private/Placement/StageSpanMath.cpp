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

	void MakeSegmentInstances(const FStageSpanLayout& Layout, double SegmentLength, const FTransform& Part, TArray<FTransform>& OutTransforms)
	{
		OutTransforms.Reset(FMath::Max(Layout.SegmentCount, 0));
		if (!Layout.IsValid() || !(SegmentLength > 0.0))
		{
			return;
		}

		// The stretch is applied in the run's frame, after the part's own rotation: a part lying along the run (a chord, a cable)
		// gets longer, not thicker. Rotated parts (diagonal lacing) are approximated, since FTransform cannot hold shear.
		const double Stretch = Layout.SegmentPitch / SegmentLength;
		const FTransform Base = FMath::IsNearlyEqual(Stretch, 1.0)
			? Part
			: FTransform(Part.ToMatrixWithScale() * FScaleMatrix(FVector(Stretch, 1.0, 1.0)));

		for (int32 Segment = 0; Segment < Layout.SegmentCount; ++Segment)
		{
			FTransform& Instance = OutTransforms.Add_GetRef(Base);
			Instance.AddToTranslation(FVector(Segment * Layout.SegmentPitch, 0.0, 0.0));
		}
	}

	void MakeJointInstances(const FStageSpanLayout& Layout, const FTransform& Part, TArray<FTransform>& OutTransforms)
	{
		OutTransforms.Reset(Layout.IsValid() ? Layout.SegmentCount + 1 : 0);
		if (!Layout.IsValid())
		{
			return;
		}

		for (int32 Joint = 0; Joint <= Layout.SegmentCount; ++Joint)
		{
			FTransform& Instance = OutTransforms.Add_GetRef(Part);
			Instance.AddToTranslation(FVector(Joint * Layout.SegmentPitch, 0.0, 0.0));
		}
	}
}
