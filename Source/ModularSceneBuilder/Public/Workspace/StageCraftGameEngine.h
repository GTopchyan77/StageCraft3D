// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/GameEngine.h"
#include "StageCraftGameEngine.generated.h"

/**
 * Makes the game viewport dockable. Registered as GameEngine= in DefaultEngine.ini.
 * Standalone Game and packaged builds only; the editor and PIE use UEditorEngine.
 *
 * Two engine behaviours stand in the way of a docked viewport (Docs/ADR/0001-dockable-workspace.md):
 * - F4: UGameEngine creates the game SViewport with RenderDirectlyToWindow(true), which draws into the
 *   window's backbuffer and cannot live in a tab. This class creates it with a separate render target,
 *   as the editor's level viewports do.
 * - F12: every frame, FDefaultGameMoviePlayer::WaitForMovieToFinish calls SwitchGameWindowToUseGameViewport
 *   (LaunchEngineLoop.cpp:5902, also in Shipping). That puts UGameEngine::GameViewportWidget back as the main
 *   window's content whenever the content is something else. SetMainWindowContent therefore installs content
 *   through a host widget and points GameViewportWidget at that host, so the engine keeps *our* content there.
 *   GetGameViewportWidget() still returns the real scene viewport for every consumer.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageCraftGameEngine : public UGameEngine
{
	GENERATED_BODY()

public:
	//~ Begin UGameEngine Interface
	virtual TSharedRef<class SViewport> CreateGameViewportWidget() override;
	virtual void Start() override;
	virtual void Tick(float DeltaSeconds, bool bIdleMode) override;
	/** The real game SViewport (the one the scene renders into), wherever it is parented. */
	virtual TSharedPtr<class SViewport> GetGameViewportWidget() const override;
	//~ End UGameEngine Interface

	/** True when the game viewport has a separate render target and can therefore be docked. */
	bool CanDockGameViewport() const;

	/**
	 * Makes Content the main window's content instead of the bare game viewport, and keeps it there across the
	 * engine's per-frame window switch and loading movies. The caller is responsible for parenting the game
	 * viewport (GetGameViewportWidget) somewhere inside Content if it should stay visible.
	 * Game thread only. Ignored, with an ensure, when the viewport cannot be docked.
	 */
	void SetMainWindowContent(const TSharedRef<class SWidget>& Content);

	/** Undoes SetMainWindowContent: the bare game viewport becomes the main window's content again. Safe to call twice. */
	void RestoreMainWindowViewport();

private:
	/** The real game viewport. Held strongly: while a layout is rebuilt, no tab holds it and FSceneViewport only holds it weakly. */
	TSharedPtr<class SViewport> SceneViewportWidget;

	/** The main window's content while SetMainWindowContent is active. Also what UGameEngine::GameViewportWidget points at then. */
	TSharedPtr<class SViewport> MainWindowHost;

	/** UGameEngine registers GameViewportWidget as Slate's game viewport once, on its first tick (GameEngine.cpp:2022). */
	bool bFirstTickDone = false;
};
