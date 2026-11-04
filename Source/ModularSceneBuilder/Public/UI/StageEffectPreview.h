// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/Widget.h"
#include "Show/StageEffectTypes.h"
#include "StageEffectPreview.generated.h"

/**
 * UMG host for the Effects panel's live preview curve (SStageEffectPreview; Docs/ADR/0027-effects-engine.md §6). The panel gives it
 * a state source (the selected effect, its fixture count and its clock); the preview reads it each frame it paints and owns nothing.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageEffectPreview : public UWidget
{
	GENERATED_BODY()

public:
	/** The effect to draw, its fixture count and its clock (cycles). False: nothing to draw. */
	using FGetState = TFunction<bool(FStageEffect& /*OutEffect*/, int32& /*OutCount*/, double& /*OutCycles*/)>;

	/** Bound by the owning panel; the function must not outlive the panel (both live in the same widget tree). */
	void SetStateSource(FGetState InGetState);

	void SetPreviewColors(const FLinearColor& Background, const FLinearColor& Grid, const FLinearColor& Curve, const FLinearColor& Dot);

	//~ Begin UVisual Interface
	virtual void ReleaseSlateResources(bool bReleaseChildren) override;
	//~ End UVisual Interface

protected:
	//~ Begin UWidget Interface
	virtual TSharedRef<SWidget> RebuildWidget() override;
	//~ End UWidget Interface

private:
	void PushToSlate();

	TSharedPtr<class SStageEffectPreview> Preview;
	FGetState GetState;
	FLinearColor BackgroundColor = FLinearColor(0.02f, 0.02f, 0.025f);
	FLinearColor GridColor = FLinearColor(1.f, 1.f, 1.f, 0.08f);
	FLinearColor CurveColor = FLinearColor(0.3f, 0.7f, 1.f);
	FLinearColor DotColor = FLinearColor::White;
};
