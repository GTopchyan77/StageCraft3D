// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Sketch/StageSketchTypes.h"
#include "StageSketchSurfaceWidget.generated.h"

/**
 * The drawing area of the Sketch window (Docs/ADR/0015-sketch-to-stage.md §2.3, §2.7): a top view of the stage (screen up = +X,
 * screen right = +Y, like the viewport's Top projection) with a grid, the sketch's strokes and the stroke being drawn.
 *
 *  - Tools (SetTool): Freehand (drag; stabilised while drawing), Shape and Truss (click points; the first point, a double-click
 *    or Enter finishes; Backspace removes the last point, Esc drops the stroke), Fixture (click), Erase (click a stroke).
 *  - View: the wheel zooms about the cursor, the right or middle button pans, FrameSketch fits the strokes.
 *  - Clicked points snap to the sketch grid when snapping is on; the generator snaps again by its own rules.
 *
 * Only the stroke in progress and the view live here (transient UI state). Finished strokes go to UStageSketchSubsystem::AddStroke;
 * everything drawn is read from the subsystem, and a repaint is requested on OnSketchChanged and on input. No tick
 * (DisableNativeTick); bound in NativeConstruct, unbound in NativeDestruct. It takes keyboard focus on a click so Enter, Esc and
 * Backspace reach it while drawing.
 */
UCLASS(meta = (DisableNativeTick))
class MODULARSCENEBUILDER_API UStageSketchSurfaceWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	void SetTool(EStageSketchTool NewTool);
	EStageSketchTool GetTool() const { return Tool; }

	/** Zooms and centres the view on the strokes (the default view when there are none). */
	void FrameSketch();

	/** Drops the stroke being drawn. */
	void CancelStroke();

	void SetTheme(const class UStageCraftUITheme* InTheme) { Theme = InTheme; }

protected:
	//~ Begin UUserWidget Interface
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual int32 NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
		int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseButtonUp(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseMove(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseButtonDoubleClick(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseWheel(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual void NativeOnMouseLeave(const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;
	virtual void NativeOnMouseCaptureLost(const FCaptureLostEvent& CaptureLostEvent) override;
	//~ End UUserWidget Interface

private:
	/** What the left button is doing. */
	enum class EGesture : uint8
	{
		None,
		/** Dragging a freehand stroke. */
		Drawing,
		/** Right or middle drag. */
		Panning,
	};

	class UStageSketchSubsystem* GetSketch() const;
	const class UStageCraftUITheme& GetTheme() const;

	FVector2D LocalToWorld(const FVector2D& Local, const FVector2D& Size) const;
	FVector2D WorldToLocal(const FVector2D& World, const FVector2D& Size) const;

	/** The plan point under a local position, snapped to the sketch grid when snapping is on and the tool clicks points. */
	FVector2D PickPoint(const FVector2D& Local, const FVector2D& Size) const;

	/** Shape / Truss: adds a click, or closes the stroke when it is on the first point. */
	void HandlePointClick(const FVector2D& Local, const FVector2D& Size);

	/** Hands the stroke in progress to the sketch (closed for shapes and loops) and starts a new one. */
	void CommitStroke(EStageSketchStrokeKind Kind, bool bClosed);

	/** Finishes a Shape (closed) or Truss (open) stroke: Enter or a double-click. */
	void FinishPointStroke();

	EStageSketchStrokeKind GetStrokeKind() const;

	void Repaint();

	UFUNCTION()
	void HandleSketchChanged();

	void PaintGrid(const FGeometry& Geometry, FSlateWindowElementList& Elements, int32 Layer, const FVector2D& Size) const;
	void PaintStroke(const FGeometry& Geometry, FSlateWindowElementList& Elements, int32 Layer, const FVector2D& Size,
		const FStageSketchStroke& Stroke, bool bHighlighted) const;
	void PaintFill(const FGeometry& Geometry, FSlateWindowElementList& Elements, int32 Layer, TConstArrayView<FVector2D> LocalPolygon, const FLinearColor& Color) const;
	void PaintMarker(const FGeometry& Geometry, FSlateWindowElementList& Elements, int32 Layer, const FVector2D& Local, float Radius, const FLinearColor& Color) const;

	UPROPERTY(Transient)
	TObjectPtr<const class UStageCraftUITheme> Theme = nullptr;

	TWeakObjectPtr<class UStageSketchSubsystem> BoundSketch;

	EStageSketchTool Tool = EStageSketchTool::Shape;
	EGesture Gesture = EGesture::None;

	/** The stroke being drawn, in plan cm. */
	TArray<FVector2D> Pending;

	/** Freehand: the stabilised pen position, and where the last sample was taken (local px). */
	FVector2D PenWorld = FVector2D::ZeroVector;
	FVector2D LastSampleLocal = FVector2D::ZeroVector;

	/** Panning: the last cursor position (local px). */
	FVector2D LastPanLocal = FVector2D::ZeroVector;

	/** Cursor in plan cm (snapped as a click would be), while over the canvas. */
	TOptional<FVector2D> Hover;

	/** Erase: the stroke under the cursor. */
	int32 HoverStroke = INDEX_NONE;

	/** View: the plan point at the canvas centre, and the zoom. */
	FVector2D ViewCentre = FVector2D::ZeroVector;
	double PixelsPerCm = 0.2;
};
