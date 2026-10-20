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
	}
}

void UStageValueSlider::HandleValueChanged(float NewValue)
{
	OnMoved.ExecuteIfBound(SliderIndex, NewValue);
}
