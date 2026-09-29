// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Data/StageParameterTypes.h"
#include "StageParameterRowWidget.generated.h"

DECLARE_DELEGATE_TwoParams(FOnStageParameterRowCommitted, const FGameplayTag& /*ParameterId*/, const FStageParameterValue& /*Value*/);

/**
 * One inspector row. The C++ base does all the wiring; a Widget Blueprint only supplies the
 * layout and style. Name any of the optional widgets below exactly and they are driven
 * automatically (ranges, display units, live drag, read-only state):
 *
 *   LabelText, UnitsText, ValueText (read-only/value display), GroupColorStrip,
 *   ValueSpinBox (Float/Integer), SpinX/SpinY/SpinZ (Vector/Rotator), ValueCheckBox (Bool),
 *   ValueTextBox (Text), ColorSwatch (Color preview).
 *
 * Types without a native editor (Color today) call CommitValue from Blueprint, e.g. from a
 * color wheel popup, and receive updates through BP_OnValueRefreshed.
 */
UCLASS(Abstract, Blueprintable)
class MODULARSCENEBUILDER_API UStageParameterRowWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	void InitializeRow(const FStageParameterDescriptor& InDescriptor, const FLinearColor& InGroupColor);

	/** Pushes a new value from the inspected object into the row without echoing it back as a commit. */
	void RefreshValue(const FStageParameterValue& InValue);

	/** Sends a user edit to the inspected object. Values are in stored units (not display units). */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Inspector")
	void CommitValue(const FStageParameterValue& InValue);

	UFUNCTION(BlueprintPure, Category = "StageCraft|Inspector")
	const FStageParameterDescriptor& GetDescriptor() const { return Descriptor; }

	/** Human-readable value in display units, e.g. "75.0 %", "(120, 0, 350) cm". */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Inspector")
	static FText FormatValue(const FStageParameterDescriptor& InDescriptor, const FStageParameterValue& InValue);

	FOnStageParameterRowCommitted OnCommitted;

protected:
	UFUNCTION(BlueprintImplementableEvent, Category = "StageCraft|Inspector", meta = (DisplayName = "On Row Initialized"))
	void BP_OnRowInitialized(const FStageParameterDescriptor& InDescriptor, FLinearColor GroupColor);

	UFUNCTION(BlueprintImplementableEvent, Category = "StageCraft|Inspector", meta = (DisplayName = "On Value Refreshed"))
	void BP_OnValueRefreshed(const FStageParameterValue& InValue);

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

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Inspector", meta = (BindWidgetOptional))
	TObjectPtr<class UImage> ColorSwatch = nullptr;

private:
	void ConfigureSpinBox(class USpinBox& SpinBox, bool bUseRange) const;
	void SetSpinValue(class USpinBox* SpinBox, double StoredValue);
	void CommitComponents();

	UFUNCTION()
	void HandleSpinValueChanged(float InValue);

	UFUNCTION()
	void HandleSpinValueCommitted(float InValue, ETextCommit::Type CommitMethod);

	UFUNCTION()
	void HandleComponentSpinChanged(float InValue);

	UFUNCTION()
	void HandleComponentSpinCommitted(float InValue, ETextCommit::Type CommitMethod);

	UFUNCTION()
	void HandleCheckStateChanged(bool bIsChecked);

	UFUNCTION()
	void HandleTextCommitted(const FText& InText, ETextCommit::Type CommitMethod);

	FStageParameterDescriptor Descriptor;

	/** Set while the row writes widget values itself, so widget change events are not sent back as edits. */
	bool bSuppressCommit = false;
};
