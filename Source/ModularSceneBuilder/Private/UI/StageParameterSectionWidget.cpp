// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageParameterSectionWidget.h"

#include "Components/Image.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "UI/StageParameterRowWidget.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageParameterSectionWidget)

void UStageParameterSectionWidget::InitializeSection(const FStageParameterSection& InSection, const FLinearColor& InGroupColor)
{
	FeatureGroup = InSection.FeatureGroup;

	if (HeaderText)
	{
		// Console convention: section headers in capitals.
		HeaderText->SetText(InSection.Title.ToUpper());
	}
	if (GroupColorStrip)
	{
		GroupColorStrip->SetColorAndOpacity(InGroupColor);
	}
	if (RowContainer)
	{
		RowContainer->ClearChildren();
	}

	BP_OnSectionInitialized(InSection.Title, FeatureGroup, InGroupColor);
}

void UStageParameterSectionWidget::AddRow(UStageParameterRowWidget& Row)
{
	if (RowContainer)
	{
		RowContainer->AddChild(&Row);
	}
}
