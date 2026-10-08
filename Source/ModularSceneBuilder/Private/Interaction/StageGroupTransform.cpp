// Copyright Epic Games, Inc. All Rights Reserved.

#include "Interaction/StageGroupTransform.h"

#include "Interaction/StageTransformRules.h"

namespace StageGroupTransform
{
	namespace
	{
		/** Leader scale ratio per axis. An axis whose start scale is (near) zero has no meaningful ratio and stays 1. */
		FVector ScaleRatio(const FVector& Start, const FVector& Now)
		{
			auto Ratio = [](double From, double To)
			{
				return FMath::IsNearlyZero(From) ? 1.0 : To / From;
			};
			return FVector(Ratio(Start.X, Now.X), Ratio(Start.Y, Now.Y), Ratio(Start.Z, Now.Z));
		}
	}

	FTransform ApplyLeaderChange(const FTransform& LeaderStart, const FTransform& LeaderNow, const FTransform& FollowerStart)
	{
		// World-space rotation that turns the leader's start orientation into its current one.
		const FQuat DeltaRotation = (LeaderNow.GetRotation() * LeaderStart.GetRotation().Inverse()).GetNormalized();

		const FVector OffsetFromLeader = FollowerStart.GetLocation() - LeaderStart.GetLocation();
		const FVector Location = LeaderNow.GetLocation() + DeltaRotation.RotateVector(OffsetFromLeader);
		const FQuat Rotation = (DeltaRotation * FollowerStart.GetRotation()).GetNormalized();
		const FVector Scale = StageTransformRules::ClampScale(FollowerStart.GetScale3D() * ScaleRatio(LeaderStart.GetScale3D(), LeaderNow.GetScale3D()));

		return FTransform(Rotation, Location, Scale);
	}
}
