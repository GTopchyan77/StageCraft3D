// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Data/StageParameterTypes.h"
#include "StageParameterSectionWidget.generated.h"

/**
 * Titled, color-coded group of rows in the inspector (Info, Transform, Patch, Dimmer, ...).
 * The Widget Blueprint must contain a panel named RowContainer (usually a Vertical Box); the
 * inspector adds rows there. HeaderText and GroupColorStrip are optional.
 */
UCLASS(Abstract, Blueprintable)
class MODULARSCENEBUILDER_API UStageParameterSectionWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	void InitializeSection(const FStageParameterSection& InSection, const FLinearColor& InGroupColor);

	void AddRow(class UStageParameterRowWidget& Row);

	UFUNCTION(BlueprintPure, Category = "StageCraft|Inspector")
	FGameplayTag GetFeatureGroup() const { return FeatureGroup; }

protected:
	UFUNCTION(BlueprintImplementableEvent, Category = "StageCraft|Inspector", meta = (DisplayName = "On Section Initialized"))
	void BP_OnSectionInitialized(const FText& Title, FGameplayTag InFeatureGroup, FLinearColor GroupColor);

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Inspector", meta = (BindWidget))
	TObjectPtr<class UPanelWidget> RowContainer = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Inspector", meta = (BindWidgetOptional))
	TObjectPtr<class UTextBlock> HeaderText = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Inspector", meta = (BindWidgetOptional))
	TObjectPtr<class UImage> GroupColorStrip = nullptr;

private:
	FGameplayTag FeatureGroup;
};
