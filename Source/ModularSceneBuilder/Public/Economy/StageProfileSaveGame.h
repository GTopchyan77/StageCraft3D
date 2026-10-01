// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "Economy/StageEconomyTypes.h"
#include "StageProfileSaveGame.generated.h"

/** On-disk form of the player profile. Written and read only by UStageProfileSubsystem. */
UCLASS()
class MODULARSCENEBUILDER_API UStageProfileSaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	/** Bump when the layout changes incompatibly; older saves are then migrated or reset on load. */
	static constexpr int32 CurrentVersion = 1;

	UPROPERTY(SaveGame)
	int32 Version = CurrentVersion;

	UPROPERTY(SaveGame)
	FStagePlayerProfile Profile;

	/** Detects edits to the file outside the game (see UStageProfileSubsystem::ComputeIntegrityHash). */
	UPROPERTY(SaveGame)
	FString IntegrityHash;
};
