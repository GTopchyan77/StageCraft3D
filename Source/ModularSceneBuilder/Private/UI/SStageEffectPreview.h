// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Show/StageEffectTypes.h"
#include "Widgets/SLeafWidget.h"

/**
 * The Effects panel's live preview (Docs/ADR/0027-effects-engine.md §6): one cycle of the effect's wave, and a dot per fixture where
 * the pattern has it now. Circle and Figure 8 on position draw their path in pan (across) and tilt (up). Reads its state through
 * GetState every frame it paints; owns nothing. Repaints itself on an active timer while it has something to show.
 */
class SStageEffectPreview : public SLeafWidget
{
public:
	/** The effect to draw, its fixture count and its clock now (cycles). False: nothing to draw. */
	using FGetState = TFunction<bool(FStageEffect& /*OutEffect*/, int32& /*OutCount*/, double& /*OutCycles*/)>;

	struct FColors
	{
		FLinearColor Background = FLinearColor(0.02f, 0.02f, 0.025f);
		FLinearColor Grid = FLinearColor(1.f, 1.f, 1.f, 0.08f);
		FLinearColor Curve = FLinearColor(0.3f, 0.7f, 1.f);
		FLinearColor Dot = FLinearColor::White;
	};

	SLATE_BEGIN_ARGS(SStageEffectPreview) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	void SetStateSource(FGetState InGetState);
	void SetColors(const FColors& InColors) { Colors = InColors; }

	//~ Begin SWidget Interface
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
		int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override { return FVector2D(240.0, 96.0); }
	//~ End SWidget Interface

private:
	EActiveTimerReturnType Animate(double InCurrentTime, float InDeltaTime);

	FGetState GetState;
	FColors Colors;
	TSharedPtr<FActiveTimerHandle> AnimateTimer;
};
