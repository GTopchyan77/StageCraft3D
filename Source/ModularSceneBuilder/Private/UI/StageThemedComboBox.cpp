// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageThemedComboBox.h"

#include "UI/StageCraftUITheme.h"
#include "UI/StageCraftWidgetStyle.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageThemedComboBox)

void UStageThemedComboBox::ApplyTheme(const UStageCraftUITheme& Theme)
{
	SetWidgetStyle(StageCraftWidgetStyle::MakeComboBox(Theme));
	SetItemStyle(StageCraftWidgetStyle::MakeListRow(Theme));
	SetContentPadding(FMargin(6.f, 1.f));
	InitScrollBarStyle(StageCraftWidgetStyle::MakeScrollBar(Theme));
	InitFont(StageCraftWidgetStyle::MakeFont(9));
	InitForegroundColor(Theme.TextPrimary);
}
