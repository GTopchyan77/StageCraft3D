// Copyright DamenTools internal. Crane Control plugin.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "CraneControlLibrary.generated.h"

UCLASS()
class CRANECONTROL_API UCraneControlLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	/** Integrates two independent velocities (units/sec) into a clamped X/Y offset pair, one Tick's worth at a time.
	 *  X clamps to [MinX, MaxX] (an existing rail bound). Y clamps to [-BoundsY, +BoundsY] (a new, symmetric rail bound).
	 *  Called every Tick from a Blueprint (WareHouseMeshBP2) that owns and reads/writes all of the state itself —
	 *  this function is pure math with plain params, no reflection needed. */
	UFUNCTION(BlueprintCallable, Category = "CraneControl")
	static void IntegrateCraneVelocity(
		float VelocityX, float VelocityY, float DeltaSeconds,
		float CurrentOffsetX, float CurrentOffsetY,
		float MinX, float MaxX, float BoundsY,
		float& OutOffsetX, float& OutOffsetY);
};
