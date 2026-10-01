// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UI/StageParameterControlWidget.h"
#include "StageParameterRowWidget.generated.h"

/**
 * One inspector row. The C++ base does all the wiring; a Widget Blueprint only supplies the
 * layout and style. Name any of the optional widgets below exactly and they are driven
 * automatically (ranges, display units, live drag, read-only state):
 *
 *   LabelText, UnitsText, ValueText (read-only/value display), GroupColorStrip,
 *   ValueSpinBox (Float/Integer), SpinX/SpinY/SpinZ (Vector/Rotator/Color), ValueCheckBox (Bool),
 *   ValueTextBox (Text), ValueComboBox (Enum dropdown), ColorSwatch (Color preview).
 *
 * Any editor the Blueprint does not bind itself is created at runtime inside EditorSlot (a
 * Horizontal Box is ideal), so one generic row Blueprint serves every parameter type. Color
 * gets a swatch plus R/G/B percent boxes. When an editable editor exists, ValueText is
 * collapsed; read-only rows show only ValueText.
 *
 * Live values never fight the user (see UStageParameterControlWidget): a box being typed into or
 * dragged keeps the user's value until the edit ends, then settles on the object's real value.
 */
UCLASS(Abstract, Blueprintable)
class MODULARSCENEBUILDER_API UStageParameterRowWidget : public UStageParameterControlWidget
{
	GENERATED_BODY()

protected:
	//~ Begin UStageParameterControlWidget Interface
	virtual void OnInitializeControl() override;
	virtual void ApplyValue(const FStageParameterValue& InValue, bool bForce) override;
	//~ End UStageParameterControlWidget Interface

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Inspector", meta = (BindWidgetOptional))
	TObjectPtr<class UTextBlock> LabelText = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Inspector", meta = (BindWidgetOptional))
	TObjectPtr<class UTextBlock> UnitsText = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Inspector", meta = (BindWidgetOptional))
	TObjectPtr<class UTextBlock> ValueText = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Inspector", meta = (BindWidgetOptional))
	TObjectPtr<class UImage> GroupColorStrip = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Inspector", meta = (BindWidgetOptional))
	TObjectPtr<class USpinBox> ValueSpinBox = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Inspector", meta = (BindWidgetOptional))
	TObjectPtr<class USpinBox> SpinX = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Inspector", meta = (BindWidgetOptional))
	TObjectPtr<class USpinBox> SpinY = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Inspector", meta = (BindWidgetOptional))
	TObjectPtr<class USpinBox> SpinZ = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Inspector", meta = (BindWidgetOptional))
	TObjectPtr<class UCheckBox> ValueCheckBox = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Inspector", meta = (BindWidgetOptional))
	TObjectPtr<class UEditableTextBox> ValueTextBox = nullptr;

	/** Enum parameters: filled from the descriptor's Options. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Inspector", meta = (BindWidgetOptional))
	TObjectPtr<class UComboBoxString> ValueComboBox = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Inspector", meta = (BindWidgetOptional))
	TObjectPtr<class UImage> ColorSwatch = nullptr;

	/** Receives auto-created editors for value widgets the Blueprint did not bind. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Inspector", meta = (BindWidgetOptional))
	TObjectPtr<class UPanelWidget> EditorSlot = nullptr;

private:
	void CreateMissingEditors();
	class USpinBox* CreateSpinBox(const TCHAR* Name);
	void AddToEditorSlot(class UWidget& Widget, bool bFill);
	bool HasEditor() const;
	void ConfigureSpinBox(class USpinBox& SpinBox, bool bUseRange) const;
	void ConfigurePercentSpinBox(class USpinBox& SpinBox) const;
	void SetSpinValue(class USpinBox* SpinBox, double DisplayValue, bool bForce);
	void CommitComponents();

	UFUNCTION()
	void HandleSpinValueChanged(float InValue);

	UFUNCTION()
	void HandleSpinValueCommitted(float InValue, ETextCommit::Type CommitMethod);

	UFUNCTION()
	void HandleSpinEndSliderMovement(float InValue);

	UFUNCTION()
	void HandleComponentSpinChanged(float InValue);

	UFUNCTION()
	void HandleComponentSpinCommitted(float InValue, ETextCommit::Type CommitMethod);

	UFUNCTION()
	void HandleCheckStateChanged(bool bIsChecked);

	UFUNCTION()
	void HandleTextCommitted(const FText& InText, ETextCommit::Type CommitMethod);

	UFUNCTION()
	void HandleComboSelectionChanged(FString SelectedItem, ESelectInfo::Type SelectionType);
};
