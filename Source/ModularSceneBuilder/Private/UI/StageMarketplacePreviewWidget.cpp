// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageMarketplacePreviewWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Image.h"
#include "Engine/Texture.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageMarketplacePreviewWidget)

void UStageMarketplacePreviewWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		Image = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("PreviewImage"));
		Image->SetColorAndOpacity(FLinearColor::Black);
		WidgetTree->RootWidget = Image;
	}
}

void UStageMarketplacePreviewWidget::SetImage(UTexture* Target)
{
	if (!Image)
	{
		return;
	}
	FSlateBrush Brush;
	Brush.SetResourceObject(Target);
	Brush.ImageSize = Target ? FVector2D(Target->GetSurfaceWidth(), Target->GetSurfaceHeight()) : FVector2D(16.0, 9.0);
	Image->SetBrush(Brush);
	// Black until there is a picture, so an empty frame reads as a screen, not a white box.
	Image->SetColorAndOpacity(Target ? FLinearColor::White : FLinearColor::Black);
}

FReply UStageMarketplacePreviewWidget::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	bDragging = true;
	return FReply::Handled().CaptureMouse(TakeWidget());
}

FReply UStageMarketplacePreviewWidget::NativeOnMouseButtonUp(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	bDragging = false;
	return FReply::Handled().ReleaseMouseCapture();
}

FReply UStageMarketplacePreviewWidget::NativeOnMouseMove(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (!bDragging || !HasMouseCapture())
	{
		return FReply::Unhandled();
	}
	const FVector2D Delta = InMouseEvent.GetCursorDelta();
	if (!Delta.IsNearlyZero())
	{
		OnOrbit.ExecuteIfBound(Delta);
	}
	return FReply::Handled();
}

FReply UStageMarketplacePreviewWidget::NativeOnMouseWheel(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	OnZoom.ExecuteIfBound(InMouseEvent.GetWheelDelta());
	return FReply::Handled();
}

void UStageMarketplacePreviewWidget::NativeOnMouseCaptureLost(const FCaptureLostEvent& CaptureLostEvent)
{
	bDragging = false;
	Super::NativeOnMouseCaptureLost(CaptureLostEvent);
}
