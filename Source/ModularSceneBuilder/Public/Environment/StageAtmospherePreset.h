// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Environment/StageEnvironmentTypes.h"
#include "StageAtmospherePreset.generated.h"

/**
 * One atmosphere of the Environment panel (Docs/ADR/0024-startup-atmospheres-starter-stages.md §3): a complete environment,
 * lighting source, time, weather and camera look together, picked with one click and blended in.
 *
 * Content only: a data asset of this class under /Game/StageCraft/Atmospheres is found by the Asset Manager (StageAtmosphere
 * in DefaultGame.ini) and listed by UStageAtmosphereSubsystem in SortOrder. Applying one goes through the controller's
 * environment request like any other environment edit (one undo step); the settings are sanitized there, so an asset with
 * out-of-range values can never reach the sky.
 */
UCLASS(BlueprintType, Const)
class MODULARSCENEBUILDER_API UStageAtmospherePreset : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	static const FPrimaryAssetType AtmosphereAssetType;

	//~ Begin UObject Interface
	virtual FPrimaryAssetId GetPrimaryAssetId() const override;
	//~ End UObject Interface

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Display")
	FText DisplayName;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Display", meta = (MultiLine = true))
	FText Description;

	/** Order in the panel, lowest first. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Display")
	int32 SortOrder = 0;

	/** The card's colour swatch: the mood at a glance. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Display")
	FLinearColor SwatchColor = FLinearColor(0.2f, 0.2f, 0.2f);

	/** The whole environment this atmosphere sets. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Environment")
	FStageEnvironmentSettings Settings;
};
