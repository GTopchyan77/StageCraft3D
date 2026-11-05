// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageValueSlider.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageValueSlider)

UStageValueSlider::UStageValueSlider(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Read when the Slate slider is built, so it must be set before then.
	IsFocusable = false;
}

void UStageValueSlider::PostInitProperties()
{
	Super::PostInitProperties();
	// After property initialization, so the binding is never copied from (or onto) the class default object.
	if (!HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
	{
		OnValueChanged.AddUniqueDynamic(this, &ThisClass::HandleValueChanged);
		OnMouseCaptureBegin.AddUniqueDynamic(this, &ThisClass::HandleCaptureBegin);
		OnMouseCaptureEnd.AddUniqueDynamic(this, &ThisClass::HandleCaptureEnd);
	}
}

void UStageValueSlider::HandleValueChanged(float NewValue)
{
	// SetValue (a panel showing the current value) broadcasts too; only the user's own moves are reported.
	if (bUserMoving)
	{
		OnMoved.ExecuteIfBound(SliderIndex, NewValue);
	}
}

void UStageValueSlider::HandleCaptureBegin()
{
	// Fires before the click's own value change, so a click on the bar counts as a move.
	bUserMoving = true;
}

void UStageValueSlider::HandleCaptureEnd()
{
	bUserMoving = false;
}
