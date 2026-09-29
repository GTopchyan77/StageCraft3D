// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "StageCraftUITheme.generated.h"

/**
 * Central palette for every StageCraft panel, so widgets never hard-code colors. The defaults
 * are a dark, low-glare console look (GrandMA3 / ChamSys-inspired): near-black surfaces,
 * amber accent, and one strong color per feature group used as a section strip.
 *
 * Create one asset (e.g. /Game/StageCraft/UI/DA_StageCraftTheme) and assign it to the panels.
 * Colors are linear; the defaults are authored as sRGB hex and converted.
 */
UCLASS(BlueprintType, Const)
class MODULARSCENEBUILDER_API UStageCraftUITheme : public UDataAsset
{
	GENERATED_BODY()

public:
	UStageCraftUITheme();

	/** Color for a feature group; falls back to the nearest parent tag, then to AccentColor. */
	UFUNCTION(BlueprintPure, Category = "Theme")
	FLinearColor GetFeatureGroupColor(FGameplayTag FeatureGroup) const;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surfaces")
	FLinearColor WindowBackground;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surfaces")
	FLinearColor PanelBackground;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surfaces")
	FLinearColor PanelHeader;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surfaces")
	FLinearColor RowBackground;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surfaces")
	FLinearColor RowHover;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surfaces")
	FLinearColor Divider;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Text")
	FLinearColor TextPrimary;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Text")
	FLinearColor TextSecondary;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Text")
	FLinearColor TextDisabled;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "State")
	FLinearColor AccentColor;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "State")
	FLinearColor SelectionColor;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "State")
	FLinearColor WarningColor;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Feature Groups", meta = (Categories = "StageCraft.FeatureGroup"))
	TMap<FGameplayTag, FLinearColor> FeatureGroupColors;
};
