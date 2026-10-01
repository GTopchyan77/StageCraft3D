// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UI/StageParameterControlWidget.h"
#include "StageEncoderWidget.generated.h"

/**
 * Console encoder for one ranged numeric parameter (Pan, Tilt, Zoom...): a painted dial with the
 * value arc in the feature-group color, name above, readout below.
 *
 * Drag anywhere on it (right/up increases), or use the mouse wheel; hold Shift for fine control,
 * like a console's encoder push-and-turn. Dragging accumulates its own value, so clamping never
 * makes it sticky, and the final value is always read back from the object.
 *
 * Works without a Widget Blueprint (a default tree is built). A Blueprint subclass may provide
 * NameText, ValueText and DialArea (any widget; the dial is painted over its geometry).
 */
UCLASS(Blueprintable)
class MODULARSCENEBUILDER_API UStageEncoderWidget : public UStageParameterControlWidget
{
	GENERATED_BODY()

protected:
	//~ Begin UUserWidget Interface
	virtual void NativeOnInitialized() override;
	virtual int32 NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseMove(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseButtonUp(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual void NativeOnMouseCaptureLost(const FCaptureLostEvent& CaptureLostEvent) override;
	virtual FReply NativeOnMouseWheel(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	//~ End UUserWidget Interface

	//~ Begin UStageParameterControlWidget Interface
	virtual void OnInitializeControl() override;
	virtual void ApplyValue(const FStageParameterValue& InValue, bool bForce) override;
	//~ End UStageParameterControlWidget Interface

	/** Pixels of drag for the full range. Shift divides the speed by 10. */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Encoders", meta = (ClampMin = "20.0"))
	float PixelsPerFullRange = 300.f;

	/** Default-tree layout in px, used only when no Widget Blueprint layout exists. */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Encoders", meta = (ClampMin = "24.0"))
	float DialSize = 52.f;

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Encoders", meta = (ClampMin = "24.0"))
	float StripWidth = 70.f;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Encoders", meta = (BindWidgetOptional))
	TObjectPtr<class UTextBlock> NameText = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Encoders", meta = (BindWidgetOptional))
	TObjectPtr<class UTextBlock> ValueText = nullptr;

	/** The dial is painted inside this widget's geometry. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Encoders", meta = (BindWidgetOptional))
	TObjectPtr<class UWidget> DialArea = nullptr;

private:
	void BuildDefaultTree();
	void GetDisplayRange(double& OutMin, double& OutMax) const;
	void Nudge(double DisplayDelta);
	void EndDrag();

	/** 0..1 position of the current value inside the range. */
	float GetNormalizedValue() const;

	double DragValue = 0.0;
	bool bDragging = false;
};
