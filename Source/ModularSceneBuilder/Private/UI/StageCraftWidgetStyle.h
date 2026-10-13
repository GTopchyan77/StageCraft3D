// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Fonts/SlateFontInfo.h"
#include "Styling/SlateBrush.h"
#include "Styling/SlateTypes.h"
#include "UI/StageCraftUITheme.h"

/**
 * Style builders for StageCraft panels that build their widget tree in code (Library, status bar).
 *
 * Every color comes from a UStageCraftUITheme, so code-built panels match the designer-built Inspector and Fader
 * panels. The engine's default UMG styles (light buttons and text boxes) are never used on a StageCraft surface.
 * Pure functions: no state, game thread only (they build Slate brushes).
 */
namespace StageCraftWidgetStyle
{
	/** Corner radius of buttons, chips and fields. */
	constexpr float CornerRadius = 3.f;

	/** The theme a panel should use: its assigned asset, or the palette's class defaults (the authored look). */
	const UStageCraftUITheme& ResolveTheme(const UStageCraftUITheme* Theme);

	/** A flat, optionally rounded and outlined box. */
	FSlateBrush MakeBox(const FLinearColor& Fill, float Radius = 0.f, const FLinearColor& Outline = FLinearColor::Transparent, float OutlineWidth = 0.f);

	/** A borderless button that only shows a surface on hover and press, with a rounded fill. */
	FButtonStyle MakeFlatButton(const FLinearColor& Normal, const FLinearColor& Hovered, const FLinearColor& Pressed);

	/** Dark search / input field: recessed background, accent outline while focused. */
	FEditableTextBoxStyle MakeInputField(const UStageCraftUITheme& Theme);

	/** Height of every value field (numeric, text, dropdown), so rows line up whatever editor they hold. */
	constexpr float FieldHeight = 22.f;

	/** Numeric field (spin box) matching MakeInputField: recessed, accent outline while dragged or typed into, a faint fill for ranged values. */
	FSpinBoxStyle MakeNumericField(const UStageCraftUITheme& Theme);

	/** Small outlined box with an accent check mark. */
	FCheckBoxStyle MakeCheckBox(const UStageCraftUITheme& Theme);

	/** Dropdown button matching MakeInputField, with a dark menu. */
	FComboBoxStyle MakeComboBox(const UStageCraftUITheme& Theme);

	/** Rows of a dropdown menu or list: transparent, hover surface, selection tint. */
	FTableRowStyle MakeListRow(const UStageCraftUITheme& Theme);

	/** Thin scroll bar that blends into the panel. */
	FScrollBarStyle MakeScrollBar(const UStageCraftUITheme& Theme);

	/** Engine Roboto at a given size. Typeface: "Regular", "Bold", "Italic"... */
	FSlateFontInfo MakeFont(int32 Size, FName Typeface = TEXT("Regular"));

	/** A brush from the app style (Starship in game builds), e.g. "Icons.Search". Never null. */
	const FSlateBrush* AppBrush(FName BrushName);

	/** The same color with a different alpha. */
	FLinearColor WithAlpha(const FLinearColor& Color, float Alpha);
}
