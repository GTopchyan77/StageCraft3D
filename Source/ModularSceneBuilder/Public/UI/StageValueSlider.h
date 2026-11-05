// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/Slider.h"
#include "StageValueSlider.generated.h"

/** Reports which slider of a panel moved, and its new value. */
DECLARE_DELEGATE_TwoParams(FOnStageSliderMoved, int32 /*SliderIndex*/, float /*Value*/);

/**
 * A slider in a code-built panel (e.g. the Environment panel). A plain USlider's value event does not say which slider
 * moved, so each one carries its index and reports it on OnMoved, like UStageChoiceButton does for buttons.
 *
 * Never keyboard-focusable: a drag leaves focus on the viewport, so viewport shortcuts keep working right after it.
 * OnMoved fires only for user changes: between the mouse capture's begin and end (a click on the bar or a drag). USlider::SetValue
 * broadcasts OnValueChanged on a changed value (UE 5.8), so a panel's read-back would otherwise come back as a user move; the
 * capture window keeps it out, so there is no feedback loop.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageValueSlider : public USlider
{
	GENERATED_BODY()

public:
	UStageValueSlider(const FObjectInitializer& ObjectInitializer);

	//~ Begin UObject Interface
	virtual void PostInitProperties() override;
	//~ End UObject Interface

	void SetSliderIndex(int32 InSliderIndex) { SliderIndex = InSliderIndex; }
	int32 GetSliderIndex() const { return SliderIndex; }

	/** Bound by the panel that owns the slider; unbound when the panel is destroyed with it. */
	FOnStageSliderMoved OnMoved;

private:
	UFUNCTION()
	void HandleValueChanged(float NewValue);

	UFUNCTION()
	void HandleCaptureBegin();

	UFUNCTION()
	void HandleCaptureEnd();

	/** The user holds the slider: its value changes are theirs. */
	bool bUserMoving = false;

	int32 SliderIndex = INDEX_NONE;
};
