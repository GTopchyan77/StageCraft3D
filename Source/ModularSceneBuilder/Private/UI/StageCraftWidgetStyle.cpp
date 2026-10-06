// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageCraftWidgetStyle.h"

#include "Brushes/SlateRoundedBoxBrush.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"

namespace StageCraftWidgetStyle
{
	const UStageCraftUITheme& ResolveTheme(const UStageCraftUITheme* Theme)
	{
		return Theme ? *Theme : *GetDefault<UStageCraftUITheme>();
	}

	FSlateBrush MakeBox(const FLinearColor& Fill, float Radius, const FLinearColor& Outline, float OutlineWidth)
	{
		return FSlateRoundedBoxBrush(Fill, Radius, Outline, OutlineWidth);
	}

	FButtonStyle MakeFlatButton(const FLinearColor& Normal, const FLinearColor& Hovered, const FLinearColor& Pressed)
	{
		FButtonStyle Style;
		Style.SetNormal(MakeBox(Normal, CornerRadius))
			.SetHovered(MakeBox(Hovered, CornerRadius))
			.SetPressed(MakeBox(Pressed, CornerRadius))
			.SetDisabled(MakeBox(WithAlpha(Normal, Normal.A * 0.5f), CornerRadius))
			.SetNormalPadding(FMargin(0.f))
			.SetPressedPadding(FMargin(0.f));
		return Style;
	}

	FEditableTextBoxStyle MakeInputField(const UStageCraftUITheme& Theme)
	{
		FEditableTextBoxStyle Style;
		Style.SetBackgroundImageNormal(MakeBox(Theme.WindowBackground, CornerRadius, Theme.Divider, 1.f))
			.SetBackgroundImageHovered(MakeBox(Theme.WindowBackground, CornerRadius, Theme.TextDisabled, 1.f))
			.SetBackgroundImageFocused(MakeBox(Theme.WindowBackground, CornerRadius, Theme.AccentColor, 1.f))
			.SetBackgroundImageReadOnly(MakeBox(Theme.WindowBackground, CornerRadius, Theme.Divider, 1.f))
			.SetPadding(FMargin(8.f, 5.f))
			.SetFont(MakeFont(9))
			.SetForegroundColor(Theme.TextPrimary)
			.SetFocusedForegroundColor(Theme.TextPrimary)
			.SetReadOnlyForegroundColor(Theme.TextDisabled)
			.SetBackgroundColor(FLinearColor::White)
			.SetScrollBarStyle(MakeScrollBar(Theme));
		return Style;
	}

	FScrollBarStyle MakeScrollBar(const UStageCraftUITheme& Theme)
	{
		const FSlateBrush Track = MakeBox(FLinearColor::Transparent);
		const FSlateBrush Thumb = MakeBox(Theme.Divider, 2.f);
		const FSlateBrush ThumbHot = MakeBox(Theme.TextDisabled, 2.f);

		FScrollBarStyle Style;
		Style.SetVerticalBackgroundImage(Track)
			.SetHorizontalBackgroundImage(Track)
			.SetVerticalTopSlotImage(Track)
			.SetVerticalBottomSlotImage(Track)
			.SetHorizontalTopSlotImage(Track)
			.SetHorizontalBottomSlotImage(Track)
			.SetNormalThumbImage(Thumb)
			.SetHoveredThumbImage(ThumbHot)
			.SetDraggedThumbImage(ThumbHot)
			.SetThickness(6.f);
		return Style;
	}

	FSlateFontInfo MakeFont(int32 Size, FName Typeface)
	{
		return FCoreStyle::GetDefaultFontStyle(Typeface, Size);
	}

	const FSlateBrush* AppBrush(FName BrushName)
	{
		return FAppStyle::GetBrush(BrushName);
	}

	FLinearColor WithAlpha(const FLinearColor& Color, float Alpha)
	{
		return FLinearColor(Color.R, Color.G, Color.B, Alpha);
	}
}
