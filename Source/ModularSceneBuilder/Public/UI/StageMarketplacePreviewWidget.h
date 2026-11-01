// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "StageMarketplacePreviewWidget.generated.h"

/** Mouse drag over the preview, in pixels. */
DECLARE_DELEGATE_OneParam(FOnStagePreviewOrbit, const FVector2D& /*DragPixels*/);
/** Mouse wheel over the preview; positive is away from the user (zoom in). */
DECLARE_DELEGATE_OneParam(FOnStagePreviewZoom, float /*WheelSteps*/);

/**
 * The image of the Marketplace's read-only 3D preview (Docs/ADR/0021-community-marketplace.md §12.3) and its mouse input: a drag
 * with any button orbits, the wheel zooms. It only reports input; the owning panel moves the preview camera and re-renders, so
 * the image changes only when something happened. No tick (DisableNativeTick).
 */
UCLASS(meta = (DisableNativeTick))
class MODULARSCENEBUILDER_API UStageMarketplacePreviewWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Shows Target (a render target owned by the preview); null shows the empty frame. */
	void SetImage(class UTexture* Target);

	FOnStagePreviewOrbit OnOrbit;
	FOnStagePreviewZoom OnZoom;

protected:
	//~ Begin UUserWidget Interface
	virtual void NativeOnInitialized() override;
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseButtonUp(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseMove(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseWheel(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual void NativeOnMouseCaptureLost(const FCaptureLostEvent& CaptureLostEvent) override;
	//~ End UUserWidget Interface

private:
	UPROPERTY(Transient)
	TObjectPtr<class UImage> Image = nullptr;

	bool bDragging = false;
};
