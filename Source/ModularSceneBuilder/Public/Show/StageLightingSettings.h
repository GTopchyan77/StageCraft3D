// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "StageLightingSettings.generated.h"

/** Project-wide lighting content (Project Settings > Game > StageCraft Lighting). */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "StageCraft Lighting"))
class MODULARSCENEBUILDER_API UStageLightingSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetCategoryName() const override { return TEXT("Game"); }

	/** DataTable of FStageGelRow: the gels the inspector's Gel picker and the assistant offer. */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Gels", meta = (RequiredAssetDataTags = "RowStructure=/Script/ModularSceneBuilder.StageGelRow"))
	TSoftObjectPtr<class UDataTable> GelTable;
};
