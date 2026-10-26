// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/GameViewportClient.h"
#include "Player/StageCameraNavigation.h"
#include "Viewport/StageViewportTypes.h"
#include "StageCraftGameViewportClient.generated.h"

/** Will the press of these buttons move the camera? Bound by the local AModularPlayerController. */
DECLARE_DELEGATE_RetVal_OneParam(bool, FStageCursorCapturePolicy, const FStageCameraButtons&);

/**
 * Gives the game viewport the Unreal Editor's mouse capture: while a press moves the camera, Slate hides the cursor,
 * switches to raw high-precision mouse deltas (so the view never stops at the screen edge) and puts the cursor back where it
 * was on release.
 *
 * The engine decides this inside its mouse-down handler (FSceneViewport::AcquireFocusAndCapture), right after InputKey and
 * before gameplay input runs, so it is answered here: by CursorCapturePolicy when bound (the controller knows whether a left
 * press grabs a gizmo handle, places an item or may drag the camera), otherwise for the right button alone. Gizmo drags and
 * placing keep a visible, moving cursor.
 *
 * Registered as GameViewportClientClassName in DefaultEngine.ini (also used by PIE).
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageCraftGameViewportClient : public UGameViewportClient
{
	GENERATED_BODY()

public:
	//~ Begin UGameViewportClient Interface
	virtual bool InputKey(const FInputKeyEventArgs& EventArgs) override;
	virtual bool HideCursorDuringCapture() const override;
	virtual void LostFocus(FViewport* InViewport) override;
	virtual void MouseMove(FViewport* InViewport, int32 X, int32 Y) override;
	virtual void CapturedMouseMove(FViewport* InViewport, int32 X, int32 Y) override;
	//~ End UGameViewportClient Interface

	/**
	 * Shades the viewport for a stage view mode (Docs/ADR/0008 §4.2-4.3). Sets ViewModeIndex and the show flags directly:
	 * UGameViewportClient::SetViewMode forces Lit in Shipping and cooked builds, while Draw only reads ViewModeIndex (F1).
	 * Each call starts again from the flags the viewport had before the first one, so returning to Lit restores everything.
	 * - Lit: the engine's lit view.
	 * - Unlit: VMI_Unlit (base colour, no lights, shadows or fog).
	 * - Lines: Unlit without the base colour, with post-processing on for the line-view material, and without temporal AA,
	 *   eye adaptation, bloom or motion blur, so the material sees stable, unjittered buffers at a fixed exposure.
	 * Renders are unaffected: scene captures have their own show flags.
	 */
	void ApplyStageViewMode(EStageViewMode Mode);

	EStageViewMode GetStageViewMode() const { return StageViewMode; }

	/** True while the navigation button is held (as seen by the viewport, before gameplay input). */
	bool IsNavigationButtonDown() const { return Buttons.bRight; }

	/** While the left button is held: where it went down, in viewport pixels (the cursor may be hidden and frozen since). */
	bool GetLeftPressPosition(FVector2D& OutPosition) const;

	/** Asked at each mouse-down whether to hide the cursor and use raw deltas; see the class comment. */
	FStageCursorCapturePolicy CursorCapturePolicy;

	/** Focus was lost with buttons held: their release will never be delivered (alt-tab mid-drag). */
	FSimpleMulticastDelegate OnMouseButtonsLost;

	/**
	 * The cursor moved over this viewport (captured or not), in viewport pixels. Fires from Slate's mouse
	 * events, possibly several times per frame; listeners that do expensive work should coalesce. It does
	 * not fire while the cursor is over another widget, so the placement preview costs nothing there.
	 */
	FSimpleMulticastDelegate OnCursorMoved;

private:
	/** The viewport's own flags before the first ApplyStageViewMode; every mode is derived from them. */
	TOptional<FEngineShowFlags> BaseShowFlags;

	EStageViewMode StageViewMode = EStageViewMode::Lit;

	/** Mouse buttons and Alt as of the last mouse-button event. */
	FStageCameraButtons Buttons;
	FVector2D LeftPressPosition = FVector2D::ZeroVector;
};
