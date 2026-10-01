// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "StageSessionTypes.generated.h"

/**
 * Rules of one stage session, owned by the GameMode and handed to UStageSessionSubsystem when
 * play starts. Different modes (free sandbox, budgeted challenge, venue with a power limit) are
 * different GameMode defaults, not different code.
 */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageSessionRules
{
	GENERATED_BODY()

	/** Off: a sandbox where every item and parameter is usable regardless of ownership. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Session")
	bool bEnforceEntitlements = true;

	/** Placed items allowed at once. 0 = unlimited. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Session", meta = (ClampMin = "0"))
	int32 MaxPlacedItems = 0;

	/** Venue power budget across all placed gear. 0 = unlimited. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Session", meta = (ClampMin = "0.0", DisplayName = "Max Total Power (W)"))
	float MaxTotalPowerWatts = 0.f;

	/** Total weight of all placed gear. 0 = unlimited. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Session", meta = (ClampMin = "0.0", Units = "kg"))
	float MaxTotalWeightKg = 0.f;
};

/** Live totals of the stage, recomputed whenever an item is added, removed or changes type. */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageSessionStats
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Session")
	int32 PlacedItems = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Session", meta = (DisplayName = "Total Power (W)"))
	float TotalPowerWatts = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Session", meta = (Units = "kg"))
	float TotalWeightKg = 0.f;
};
