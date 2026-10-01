// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "UI/StageParameterViewWidget.h"
#include "StageFaderBankWidget.generated.h"

/**
 * Quick-access playback bar for the selected item, in the spirit of a GrandMA3 fader/encoder bar:
 * one channel strip per live-controllable parameter, grouped and colored by feature group.
 *
 * Built from the same parameter sections as the inspector (so new equipment needs no UI code):
 *  - ranged Float / Integer: UStageFaderWidget, or UStageEncoderWidget for EncoderParameters (Pan, Tilt);
 *  - Bool: a toggle strip (Mute, Polarity);
 *  - Color: three strips, R / G / B, all bound to the one color parameter.
 * Read-only rows, text, vectors and enums stay in the inspector. Only sections whose feature group
 * is in FeatureGroups are shown, in the order the item lists them, so Patch and Transform never
 * clutter the bar.
 *
 * Edits from the bar, the inspector, the gizmo, cues or DMX all meet in the item's parameter
 * model, and every view refreshes from its change event (UStageParameterViewWidget).
 *
 * Works without a Widget Blueprint (a default dark bar is built). A Blueprint subclass may provide
 * FaderContainer (required for a custom layout, e.g. a Horizontal Box), TitleText and EmptyState.
 */
UCLASS(Blueprintable)
class MODULARSCENEBUILDER_API UStageFaderBankWidget : public UStageParameterViewWidget
{
	GENERATED_BODY()

public:
	UStageFaderBankWidget(const FObjectInitializer& ObjectInitializer);

protected:
	//~ Begin UUserWidget Interface
	virtual void NativeOnInitialized() override;
	//~ End UUserWidget Interface

	//~ Begin UStageParameterViewWidget Interface
	virtual void ClearView() override;
	virtual void BuildView(UObject& Target, const TArray<FStageParameterSection>& Sections) override;
	virtual void OnViewUpdated(UObject* Target) override;
	virtual void OnParameterRefreshed(const FGameplayTag& ParameterId, const FStageParameterValue& Value) override;
	//~ End UStageParameterViewWidget Interface

	/** Feature groups that get strips, matched exactly. Empty shows every group. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "StageCraft|Faders", meta = (Categories = "StageCraft.FeatureGroup"))
	TArray<FGameplayTag> FeatureGroups;

	/** Parameters shown as encoders instead of faders (console convention: position on encoders). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "StageCraft|Faders", meta = (Categories = "StageCraft.Attribute"))
	FGameplayTagContainer EncoderParameters;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "StageCraft|Faders")
	TSubclassOf<class UStageFaderWidget> FaderWidgetClass;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "StageCraft|Faders")
	TSubclassOf<class UStageEncoderWidget> EncoderWidgetClass;

	/** Gap between feature groups, in px. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "StageCraft|Faders", meta = (ClampMin = "0.0"))
	float GroupSpacing = 10.f;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Faders", meta = (BindWidgetOptional))
	TObjectPtr<class UPanelWidget> FaderContainer = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Faders", meta = (BindWidgetOptional))
	TObjectPtr<class UTextBlock> TitleText = nullptr;

	/** Shown when nothing is selected or the selection has no controllable parameters. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Faders", meta = (BindWidgetOptional))
	TObjectPtr<class UTextBlock> EmptyState = nullptr;

private:
	void BuildDefaultTree();
	void AddStrip(class UStageParameterControlWidget& Control, float LeftPadding);
	void RefreshTitle(UObject* Target);
};
