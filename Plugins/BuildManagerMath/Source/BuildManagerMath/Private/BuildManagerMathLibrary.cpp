// Copyright DamenTools internal. Build Manager Math plugin.

#include "BuildManagerMathLibrary.h"
#include "BuildManagerMath.h"
#include "GameFramework/PlayerController.h"

bool UBuildManagerMathLibrary::ComputeCursorPlaneIntersection(APlayerController* PlayerController, float PlaneZ, float Epsilon, FVector& OutLocation)
{
	OutLocation = FVector::ZeroVector;

	if (!IsValid(PlayerController))
	{
		return false;
	}

	FVector WorldLocation;
	FVector WorldDirection;
	if (!PlayerController->DeprojectMousePositionToWorld(WorldLocation, WorldDirection))
	{
		return false;
	}

	if (FMath::Abs(WorldDirection.Z) < Epsilon)
	{
		return false;
	}

	const double T = (PlaneZ - WorldLocation.Z) / WorldDirection.Z;
	if (T <= Epsilon)
	{
		return false;
	}

	const FVector HitPoint = WorldLocation + WorldDirection * T;
	OutLocation = FVector(HitPoint.X, HitPoint.Y, PlaneZ);
	return true;
}
