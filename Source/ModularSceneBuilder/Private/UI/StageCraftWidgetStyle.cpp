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

	FSlateBrush MakeLine(const FLinearColor& Color, const FVector2D& Size)
	{
		FSlateBrush Brush = MakeBox(Color);
		Brush.ImageSize = Size;
		return Brush;
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

	FSpinBoxStyle MakeNumericField(const UStageCraftUITheme& Theme)
	{
		// Starts from the app style so fields this function does not mention (sounds, inset padding) keep sensible values.
		FSpinBoxStyle Style = FAppStyle::Get().GetWidgetStyle<FSpinBoxStyle>(TEXT("SpinBox"));
		Style.SetBackgroundBrush(MakeBox(Theme.WindowBackground, CornerRadius, Theme.Divider, 1.f))
			.SetHoveredBackgroundBrush(MakeBox(Theme.WindowBackground, CornerRadius, Theme.TextDisabled, 1.f))
			.SetActiveBackgroundBrush(MakeBox(Theme.WindowBackground, CornerRadius, Theme.AccentColor, 1.f))
			.SetInactiveFillBrush(MakeBox(WithAlpha(Theme.TextSecondary, 0.10f), CornerRadius))
			.SetHoveredFillBrush(MakeBox(WithAlpha(Theme.TextSecondary, 0.16f), CornerRadius))
			.SetActiveFillBrush(MakeBox(WithAlpha(Theme.AccentColor, 0.22f), CornerRadius))
			.SetArrowsImage(FSlateNoResource())
			.SetForegroundColor(Theme.TextPrimary)
			.SetTextPadding(FMargin(FieldTextInset, 2.f));
		return Style;
	}

	FCheckBoxStyle MakeCheckBox(const UStageCraftUITheme& Theme)
	{
		// SCheckBox sizes its box from the brush, and a rounded box brush has no size of its own.
		const FVector2D BoxSize(16.0, 16.0);
		auto MakeCheckBoxBackground = [&BoxSize](const FLinearColor& Fill, const FLinearColor& Outline)
		{
			FSlateBrush Box = MakeBox(Fill, CornerRadius, Outline, 1.f);
			Box.ImageSize = BoxSize;
			return Box;
		};

		// The check mark takes its colour from the checked foreground colours below.
		FSlateBrush Check = *AppBrush(TEXT("Icons.Check"));
		Check.TintColor = FLinearColor::White;
		Check.ImageSize = BoxSize;

		FCheckBoxStyle Style = FAppStyle::Get().GetWidgetStyle<FCheckBoxStyle>(TEXT("Checkbox"));
		Style.SetCheckBoxType(ESlateCheckBoxType::CheckBox)
			.SetBackgroundImage(MakeCheckBoxBackground(Theme.WindowBackground, Theme.Divider))
			.SetBackgroundHoveredImage(MakeCheckBoxBackground(Theme.WindowBackground, Theme.TextDisabled))
			.SetBackgroundPressedImage(MakeCheckBoxBackground(Theme.WindowBackground, Theme.AccentColor))
			.SetUncheckedImage(FSlateNoResource())
			.SetUncheckedHoveredImage(FSlateNoResource())
			.SetUncheckedPressedImage(FSlateNoResource())
			.SetCheckedImage(Check)
			.SetCheckedHoveredImage(Check)
			.SetCheckedPressedImage(Check)
			.SetUndeterminedImage(FSlateNoResource())
			.SetUndeterminedHoveredImage(FSlateNoResource())
			.SetUndeterminedPressedImage(FSlateNoResource())
			.SetCheckedForegroundColor(Theme.AccentColor)
			.SetCheckedHoveredForegroundColor(Theme.AccentColor)
			.SetCheckedPressedForegroundColor(Theme.AccentColor)
			.SetPadding(FMargin(2.f));
		return Style;
	}

	FComboBoxStyle MakeComboBox(const UStageCraftUITheme& Theme)
	{
		FButtonStyle Button;
		Button.SetNormal(MakeBox(Theme.WindowBackground, CornerRadius, Theme.Divider, 1.f))
			.SetHovered(MakeBox(Theme.WindowBackground, CornerRadius, Theme.TextDisabled, 1.f))
			.SetPressed(MakeBox(Theme.WindowBackground, CornerRadius, Theme.AccentColor, 1.f))
			.SetDisabled(MakeBox(Theme.WindowBackground, CornerRadius, Theme.Divider, 1.f))
			.SetNormalForeground(Theme.TextPrimary)
			.SetHoveredForeground(Theme.TextPrimary)
			.SetPressedForeground(Theme.TextPrimary)
			.SetDisabledForeground(Theme.TextDisabled)
			.SetNormalPadding(FMargin(0.f))
			.SetPressedPadding(FMargin(0.f));

		FSlateBrush Arrow = *AppBrush(TEXT("Icons.ChevronDown"));
		Arrow.TintColor = Theme.TextSecondary;
		Arrow.ImageSize = FVector2D(10.0, 10.0);

		FComboButtonStyle ComboButton = FAppStyle::Get().GetWidgetStyle<FComboBoxStyle>(TEXT("ComboBox")).ComboButtonStyle;
		ComboButton.SetButtonStyle(Button)
			.SetDownArrowImage(Arrow)
			.SetMenuBorderBrush(MakeBox(Theme.PanelHeader, CornerRadius, Theme.Divider, 1.f))
			.SetMenuBorderPadding(FMargin(1.f));

		FComboBoxStyle Style = FAppStyle::Get().GetWidgetStyle<FComboBoxStyle>(TEXT("ComboBox"));
		Style.SetComboButtonStyle(ComboButton)
			.SetContentPadding(FMargin(FieldTextInset, 2.f))
			.SetMenuRowPadding(FMargin(6.f, 3.f));
		return Style;
	}

	FTableRowStyle MakeListRow(const UStageCraftUITheme& Theme)
	{
		const FSlateBrush Clear = MakeBox(FLinearColor::Transparent);
		const FSlateBrush Hover = MakeBox(Theme.RowHover);
		const FSlateBrush Selected = MakeBox(WithAlpha(Theme.SelectionColor, 0.35f));

		// The app style's row supplies drop indicators and focus brushes; every surface colour is replaced.
		FTableRowStyle Style = FAppStyle::Get().GetWidgetStyle<FTableRowStyle>(TEXT("ComboBox.Row"));
		Style.SetEvenRowBackgroundBrush(Clear)
			.SetOddRowBackgroundBrush(Clear)
			.SetEvenRowBackgroundHoveredBrush(Hover)
			.SetOddRowBackgroundHoveredBrush(Hover)
			.SetActiveBrush(Selected)
			.SetActiveHoveredBrush(Selected)
			.SetInactiveBrush(Selected)
			.SetInactiveHoveredBrush(Selected)
			.SetActiveHighlightedBrush(Selected)
			.SetInactiveHighlightedBrush(Selected)
			.SetSelectorFocusedBrush(FSlateNoResource())
			.SetTextColor(Theme.TextPrimary)
			.SetSelectedTextColor(Theme.TextPrimary);
		return Style;
	}

	FSliderStyle MakeSlider(const UStageCraftUITheme& Theme)
	{
		constexpr float ThumbSize = 12.f;
		const auto MakeThumb = [](const FLinearColor& Fill, const FLinearColor& Outline)
		{
			return FSlateRoundedBoxBrush(Fill, ThumbSize * 0.5f, Outline, 1.f, FVector2D(ThumbSize, ThumbSize));
		};

		FSliderStyle Style;
		Style.SetNormalBarImage(MakeBox(Theme.Divider, 1.f))
			.SetHoveredBarImage(MakeBox(Theme.TextDisabled, 1.f))
			.SetDisabledBarImage(MakeBox(WithAlpha(Theme.Divider, 0.5f), 1.f))
			.SetNormalThumbImage(MakeThumb(Theme.TextPrimary, Theme.WindowBackground))
			.SetHoveredThumbImage(MakeThumb(Theme.AccentColor, Theme.WindowBackground))
			.SetDisabledThumbImage(MakeThumb(Theme.TextDisabled, Theme.WindowBackground))
			.SetBarThickness(3.f);
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
