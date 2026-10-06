// Copyright Epic Games, Inc. All Rights Reserved.

#include "Placement/StagePlacementMath.h"

namespace StagePlacementMath
{
	namespace
	{
		double SnapAxis(double Value, double CellSize)
		{
			return CellSize > 0.0 ? FMath::GridSnap(Value, CellSize) : Value;
		}
	}

	FTransform ComputePlacementTransform(const FStageItemPlacementRules& Rules, const FVector& ImpactPoint, const FVector& ImpactNormal)
	{
		FVector Location(
			SnapAxis(ImpactPoint.X, Rules.GridSize.X),
			SnapAxis(ImpactPoint.Y, Rules.GridSize.Y),
			SnapAxis(ImpactPoint.Z, Rules.GridSize.Z));

		// A degenerate normal (zero-length hit normal) falls back to world up instead of producing NaNs.
		const FVector SafeNormal = ImpactNormal.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
		const FQuat Rotation = Rules.bAlignToSurfaceNormal
			? FRotationMatrix::MakeFromZ(SafeNormal).ToQuat()
			: FQuat::Identity;

		Location += Rotation.RotateVector(Rules.PlacementOffset);
		return FTransform(Rotation, Location);
	}

	bool UsesGridSnapping(const FStageItemPlacementRules& Rules)
	{
		return Rules.GridSize.X > 0.0 || Rules.GridSize.Y > 0.0 || Rules.GridSize.Z > 0.0;
	}
}
