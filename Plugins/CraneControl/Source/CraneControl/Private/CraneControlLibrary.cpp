// Copyright DamenTools internal. Crane Control plugin.

#include "CraneControlLibrary.h"
#include "CraneControl.h"

void UCraneControlLibrary::IntegrateCraneVelocity(
	float VelocityX, float VelocityY, float DeltaSeconds,
	float CurrentOffsetX, float CurrentOffsetY,
	float MinX, float MaxX, float BoundsY,
	float& OutOffsetX, float& OutOffsetY)
{
	OutOffsetX = FMath::Clamp(CurrentOffsetX + VelocityX * DeltaSeconds, MinX, MaxX);
	OutOffsetY = FMath::Clamp(CurrentOffsetY + VelocityY * DeltaSeconds, -BoundsY, BoundsY);
}
