// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UI/StageParameterControlWidget.h"
#include "StageEncoderWidget.generated.h"

/**
 * Console encoder for one ranged numeric parameter (Pan, Tilt, Zoom...): a painted 270 degree dial
 * with the value arc in the feature-group color, name above, readout below.
 *
 * Interaction, built for a mouse rather than for tracing the arc:
 *  - Press anywhere on the strip and drag vertically: up increases, PixelsPerFullRange px for the
 *    whole range. Only the movement delta counts, so where you press never matters and the value
 *    never jumps on press. The drag starts after the platform drag threshold, which keeps plain
 *    clicks and double-clicks clean. While dragging, the cursor is hidden (high-precision mouse,
 *    so screen edges never stop a drag) and put back exactly where the press happened.
 *  - Shift while dragging or scrolling: FineScale (default 1/5) of the speed, switchable mid-drag
 *    without a jump.
 *  - Mouse wheel: one Step per notch (1 % of the range when the parameter has no step).
 *  - Double-click: reset to the parameter's default (Pan/Tilt home), else 0 clamped into range.
 * The drag accumulates its own clamped value, so pushing past a limit never builds up a dead zone
 * and reversing responds immediately. The final value is always read back from the object.
 *
 * Feedback: hover brightens the ring, an active drag thickens and lights the arc, and a floating
 * popup above the dial shows "Name: value" while dragging and briefly after a wheel step or reset.
 * The popup escapes the parent's clipping (the fader bank scrolls) and draws above neighbours. The
 * needle eases toward the value (frame-rate independent), so cue and DMX jumps read as motion;
 * the readout text is always exact.
 *
 * Works without a Widget Blueprint (a default tree is built). A Blueprint subclass may provide
 * NameText, ValueText and DialArea (any widget; the dial is painted over its geometry).
 */
UCLASS(Blueprintable)
class MODULARSCENEBUILDER_API UStageEncoderWidget : public UStageParameterControlWidget
{
	GENERATED_BODY()

public:
	/** Resets to the default value (what a double-click does). Returns false when the encoder is read-only or unranged. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Encoders")
	bool ResetToDefault();

	/** True between the drag threshold being crossed and the button being released. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Encoders")
	bool IsDragging() const { return DragState == EDragState::Dragging; }

	/** The text the value popup shows, e.g. "Pan: 45.0°". */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Encoders")
	FText GetPopupText() const;

	/** Whether the value popup is currently drawn. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Encoders")
	bool IsPopupVisible() const { return IsDragging() || PopupTimeRemaining > 0.f; }

protected:
	//~ Begin UUserWidget Interface
	virtual void NativeOnInitialized() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual int32 NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseMove(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseButtonUp(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseButtonDoubleClick(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual void NativeOnMouseCaptureLost(const FCaptureLostEvent& CaptureLostEvent) override;
	virtual FReply NativeOnMouseWheel(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual void NativeOnMouseEnter(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual void NativeOnMouseLeave(const FPointerEvent& InMouseEvent) override;
	//~ End UUserWidget Interface

	//~ Begin UStageParameterControlWidget Interface
	virtual void OnInitializeControl() override;
	virtual void ApplyValue(const FStageParameterValue& InValue, bool bForce) override;
	//~ End UStageParameterControlWidget Interface

	/** Vertical drag distance, in px, that sweeps the whole range. */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Encoders", meta = (ClampMin = "20.0"))
	float PixelsPerFullRange = 300.f;

	/** Speed multiplier while Shift is held (drag and wheel). 0.2 = five times finer. */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Encoders", meta = (ClampMin = "0.01", ClampMax = "1.0"))
	float FineScale = 0.2f;

	/** How fast the painted needle follows the value; higher is snappier. Zero disables easing. */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Encoders", meta = (ClampMin = "0.0"))
	float NeedleSmoothing = 28.f;

	/** Seconds the value popup stays up after a wheel step or reset. */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Encoders", meta = (ClampMin = "0.0"))
	float PopupLingerTime = 0.9f;

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
	enum class EDragState : uint8
	{
		None,
		/** Button down, threshold not crossed yet: still a click (or the first half of a double-click). */
		Pending,
		Dragging
	};

	void BuildDefaultTree();
	bool CanEdit() const;
	void GetDisplayRange(double& OutMin, double& OutMax) const;
	void Nudge(double DisplayDelta);
	void ShowPopupBriefly();

	/** Ends a drag or pending press; restores the cursor when a drag ran. Safe to call in any state. */
	FReply EndInteraction(bool bReleaseCapture);

	/** 0..1 position of the current value inside the range. */
	float GetNormalizedValue() const;

	void PaintPopup(const FGeometry& AllottedGeometry, const FGeometry& DialGeometry, FSlateWindowElementList& OutDrawElements, int32 LayerId) const;

	EDragState DragState = EDragState::None;

	/** Accumulated drag value in display units, clamped every step. */
	double DragValue = 0.0;

	/** Unapplied movement while Pending, to detect the drag threshold. */
	FVector2D PendingTravel = FVector2D::ZeroVector;

	/** Cursor position, in screen pixels, when the drag started and the cursor was hidden: it reappears here. */
	FIntPoint DragAnchorScreenPosition = FIntPoint::ZeroValue;

	/** Set while switching to high-precision mouse: the re-capture reports our own capture as lost. */
	bool bIgnoreNextCaptureLost = false;

	/** Painted needle position, easing toward GetNormalizedValue(). Negative until the first tick snaps it. */
	float DisplayedNormalized = -1.f;

	float PopupTimeRemaining = 0.f;
	bool bHovered = false;
};
