// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/GameViewportClient.h"
#include "StageCraftGameViewportClient.generated.h"

/**
 * Gives the game viewport the Unreal Editor's right-mouse capture: while the navigation button is
 * the only mouse button held, Slate hides the cursor, switches to raw high-precision mouse deltas
 * (so the view never stops at the screen edge) and puts the cursor back where it was on release.
 *
 * The engine decides this inside its mouse-down handler (FSceneViewport::AcquireFocusAndCapture),
 * before gameplay input runs, so it has to be answered here rather than from the player controller.
 * Left-button drags (gizmo) keep a visible, moving cursor.
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

	/** True while the navigation button is held (as seen by the viewport, before gameplay input). */
	bool IsNavigationButtonDown() const { return bNavigationButtonDown; }

	/**
	 * The cursor moved over this viewport (captured or not), in viewport pixels. Fires from Slate's mouse
	 * events, possibly several times per frame; listeners that do expensive work should coalesce. It does
	 * not fire while the cursor is over another widget, so the placement preview costs nothing there.
	 */
	FSimpleMulticastDelegate OnCursorMoved;

private:
	bool bNavigationButtonDown = false;
	bool bOtherMouseButtonDown = false;
};
