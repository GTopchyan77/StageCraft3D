// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageEffectPreview.h"

#include "UI/SStageEffectPreview.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageEffectPreview)

void UStageEffectPreview::SetStateSource(FGetState InGetState)
{
	GetState = MoveTemp(InGetState);
	PushToSlate();
}

void UStageEffectPreview::SetPreviewColors(const FLinearColor& Background, const FLinearColor& Grid, const FLinearColor& Curve, const FLinearColor& Dot)
{
	BackgroundColor = Background;
	GridColor = Grid;
	CurveColor = Curve;
	DotColor = Dot;
	PushToSlate();
}

TSharedRef<SWidget> UStageEffectPreview::RebuildWidget()
{
	Preview = SNew(SStageEffectPreview);
	PushToSlate();
	return Preview.ToSharedRef();
}

void UStageEffectPreview::ReleaseSlateResources(bool bReleaseChildren)
{
	Super::ReleaseSlateResources(bReleaseChildren);
	Preview.Reset();
}

void UStageEffectPreview::PushToSlate()
{
	if (!Preview.IsValid())
	{
		return;
	}
	SStageEffectPreview::FColors Colors;
	Colors.Background = BackgroundColor;
	Colors.Grid = GridColor;
	Colors.Curve = CurveColor;
	Colors.Dot = DotColor;
	Preview->SetColors(Colors);
	Preview->SetStateSource(GetState);
}
