// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UI/StageParameterControlWidget.h"
#include "StageFaderWidget.generated.h"

/**
 * Console-style channel strip for one parameter: name, vertical fader, value readout.
 *
 *  - Float / Integer with a range: the fader spans the range in display units (Dimmer 0..100 %,
 *    Gain -60..12 dB). The mouse wheel over the strip nudges by 1 % of the range (Shift: 0.1 %).
 *  - Bool: a toggle button replaces the fader (Mute, Polarity).
 *  - Color, with SetColorComponent(0..2): the fader edits one channel (R, G or B, 0..100 %) of the
 *    color value and the bar is tinted in that channel's color. A bank shows three strips per color.
 *
 * Works without a Widget Blueprint: if the class has no designed tree, a default strip is built.
 * A Blueprint subclass can supply its own layout using the optional names below:
 *   NameText, ValueSlider, ValueToggle, ValueText, GroupColorStrip.
 */
UCLASS(Blueprintable)
class MODULARSCENEBUILDER_API UStageFaderWidget : public UStageParameterControlWidget
{
	GENERATED_BODY()

public:
	/** Makes the strip edit one channel of a Color parameter. Call before InitializeControl. */
	void SetColorComponent(int32 InComponent) { ColorComponent = InComponent; }

	UFUNCTION(BlueprintPure, Category = "StageCraft|Faders")
	int32 GetColorComponent() const { return ColorComponent; }

protected:
	//~ Begin UUserWidget Interface
	virtual void NativeOnInitialized() override;
	virtual FReply NativeOnMouseWheel(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	//~ End UUserWidget Interface

	//~ Begin UStageParameterControlWidget Interface
	virtual void OnInitializeControl() override;
	virtual void ApplyValue(const FStageParameterValue& InValue, bool bForce) override;
	//~ End UStageParameterControlWidget Interface

	/** Default-tree layout in px, used only when no Widget Blueprint layout exists. */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Faders", meta = (ClampMin = "24.0"))
	float StripWidth = 58.f;

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Faders", meta = (ClampMin = "40.0"))
	float FaderHeight = 140.f;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Faders", meta = (BindWidgetOptional))
	TObjectPtr<class UTextBlock> NameText = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Faders", meta = (BindWidgetOptional))
	TObjectPtr<class USlider> ValueSlider = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Faders", meta = (BindWidgetOptional))
	TObjectPtr<class UCheckBox> ValueToggle = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Faders", meta = (BindWidgetOptional))
	TObjectPtr<class UTextBlock> ValueText = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Faders", meta = (BindWidgetOptional))
	TObjectPtr<class UImage> GroupColorStrip = nullptr;

private:
	void BuildDefaultTree();
	bool IsColorChannel() const { return Descriptor.GetType() == EStageParameterType::Color && ColorComponent != INDEX_NONE; }
	bool IsNumeric() const;

	/** Fader position in display units (percent for a color channel). */
	double ReadDisplayValue(const FStageParameterValue& InValue) const;
	void CommitDisplayValue(double DisplayValue);
	void GetDisplayRange(double& OutMin, double& OutMax) const;

	UFUNCTION()
	void HandleSliderChanged(float InValue);

	UFUNCTION()
	void HandleSliderCaptureEnd();

	UFUNCTION()
	void HandleToggleChanged(bool bIsChecked);

	int32 ColorComponent = INDEX_NONE;
};
