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
 * Left-button drags (gizmo, placement painting) keep a visible, moving cursor.
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
	//~ End UGameViewportClient Interface

	/** True while the navigation button is held (as seen by the viewport, before gameplay input). */
	bool IsNavigationButtonDown() const { return bNavigationButtonDown; }

private:
	bool bNavigationButtonDown = false;
	bool bOtherMouseButtonDown = false;
};
