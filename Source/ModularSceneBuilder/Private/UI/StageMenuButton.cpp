// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageMenuButton.h"

#include "UI/StageCraftUITheme.h"
#include "UI/StageCraftWidgetStyle.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Text/STextBlock.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageMenuButton)

void UStageMenuButton::ApplyTheme(const UStageCraftUITheme& Theme)
{
	// The same button as the theme's dropdown fields (the projection selector), so the toolbar's two dropdowns match.
	ComboStyle = StageCraftWidgetStyle::MakeComboBox(Theme).ComboButtonStyle;
	LabelFont = StageCraftWidgetStyle::MakeFont(9);
	LabelColor = FSlateColor(Theme.TextPrimary);
}

void UStageMenuButton::SetLabel(const FText& InLabel)
{
	Label = InLabel;
	if (MyLabel)
	{
		MyLabel->SetText(Label);
	}
}

TSharedRef<SWidget> UStageMenuButton::RebuildWidget()
{
	MyComboButton = SNew(SComboButton)
		.ComboButtonStyle(&ComboStyle)
		.IsFocusable(false)
		.HasDownArrow(true)
		.ForegroundColor(LabelColor)
		.ContentPadding(FMargin(6.f, 1.f))
		.OnGetMenuContent(FOnGetContent::CreateUObject(this, &ThisClass::HandleGetMenuContent))
		.ButtonContent()
		[
			SAssignNew(MyLabel, STextBlock)
			.Text(Label)
			.Font(LabelFont)
			.ColorAndOpacity(LabelColor)
		];
	return MyComboButton.ToSharedRef();
}

void UStageMenuButton::ReleaseSlateResources(bool bReleaseChildren)
{
	Super::ReleaseSlateResources(bReleaseChildren);
	MyComboButton.Reset();
	MyLabel.Reset();
}

TSharedRef<SWidget> UStageMenuButton::HandleGetMenuContent()
{
	if (OnGetMenuContent.IsBound())
	{
		return OnGetMenuContent.Execute();
	}
	// Unbound only if the owner was torn down while the button was still clickable; an empty popup is harmless.
	return SNew(SSpacer);
}
