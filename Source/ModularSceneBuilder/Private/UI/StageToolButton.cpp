// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageToolButton.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageToolButton)

UStageToolButton::UStageToolButton(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	InitIsFocusable(false);
}

UStageChoiceButton::UStageChoiceButton(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void UStageChoiceButton::PostInitProperties()
{
	Super::PostInitProperties();
	// After property initialization, so the binding is never copied from (or onto) the class default object.
	// The button listens to itself for its whole life; the binding dies with it.
	if (!HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
	{
		OnClicked.AddUniqueDynamic(this, &ThisClass::HandleClicked);
	}
}

void UStageChoiceButton::HandleClicked()
{
	OnPicked.ExecuteIfBound(ChoiceIndex);
}
