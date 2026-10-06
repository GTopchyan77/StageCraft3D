// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Framework/Docking/TabManager.h"
#include "GameplayTagContainer.h"
#include "Workspace/StageLayoutStore.h"
#include "Workspace/StageWorkspaceTypes.h"

/** Callbacks the shell needs from its owner. Bound by UStageWorkspaceSubsystem with weak captures. */
struct FStageWorkspaceShellCallbacks
{
	/** Every widget panel the shell may spawn (not the built-in viewport). Read whenever spawners are registered. */
	TFunction<TArray<FGameplayTag>()> GetWidgetPanels;

	/** Tab label of a panel. Read live by the tab, so it updates when a definition finishes loading. */
	TFunction<FText(const FGameplayTag& PanelTag)> GetPanelLabel;

	/** Builds the content of a widget panel. Called on spawn and on RefreshPanelContent. Must not return null. */
	TFunction<TSharedRef<class SWidget>(const FGameplayTag& PanelTag)> CreatePanelContent;

	/** Builds the status bar under the panels. Called on install and on RefreshPanelContent. Must not return null. */
	TFunction<TSharedRef<class SWidget>()> CreateStatusBarContent;

	/** A panel was docked, floated or closed. Not called while a layout is being restored. */
	TFunction<void(const FGameplayTag& PanelTag, EStagePanelHost NewHost, EStagePanelHost PreviousHost)> PanelHostChanged;

	/** The user changed the arrangement (split, tab move, resize, open, close). Slate defers this by a few seconds and coalesces it. */
	TFunction<void()> LayoutChanged;

	/** The user is closing the main window, which quits the app (ADR F7). Every window still exists, so the layout can be captured here. */
	TFunction<void()> MainWindowClosing;

	/** Makes the widget the main window's content (UStageCraftGameEngine::SetMainWindowContent). */
	TFunction<void(const TSharedRef<class SWidget>& Content)> SetMainWindowContent;

	/** Gives the main window back its bare game viewport (UStageCraftGameEngine::RestoreMainWindowViewport). */
	TFunction<void()> RestoreMainWindowViewport;
};

/**
 * All Slate for the workspace, structured the way Slate expects standalone apps to be (Trace Insights, the level editor):
 * - The global tab manager's primary area in the main window holds one major tab, the Workspace, with its tab well hidden.
 * - The Workspace tab shows the menu bar above a panel area owned by a panel tab manager. Panels (viewport, inspector,
 *   faders...) are panel tabs in it. Panel layouts, splits, tab stacks and floating panel windows all belong to that manager.
 * - The viewport panel hosts the engine's game SViewport.
 *
 * Ownership: created and owned (shared) by UStageWorkspaceSubsystem. Slate holds only weak references back
 * (CreateSP), so tabs that outlive the shell never call into a dead object. The main window and viewport
 * widget are owned by the engine and held weakly here.
 *
 * Game thread only.
 */
class FStageWorkspaceShell : public TSharedFromThis<FStageWorkspaceShell>
{
public:
	/** Name of every panel FLayout. It versions the panel structure: bump it when tab IDs or nesting rules change incompatibly, and saved layouts fall back to the default. */
	static const FName PanelLayoutVersion;

	FStageWorkspaceShell(const TSharedRef<class SWindow>& InMainWindow, const TSharedRef<class SViewport>& InViewportWidget, FStageWorkspaceShellCallbacks InCallbacks);

	/** Installs the Workspace in the main window with PanelLayout as its panel arrangement and MenuBar above it. Call once, before anything else. */
	void Install(const TSharedRef<FTabManager::FLayout>& PanelLayout, const TSharedRef<class SWidget>& MenuBar);

	/** Closes every panel and floating window and gives the main window back its bare viewport. Safe to call twice. */
	void Shutdown();

	bool IsInstalled() const { return bInstalled; }

	/** Closes every panel and floating panel window, then restores PanelLayout. */
	void ApplyLayout(const TSharedRef<FTabManager::FLayout>& PanelLayout);

	/** The live panel arrangement, including floating window rectangles. Null when not installed. */
	TSharedPtr<FTabManager::FLayout> CaptureLayout() const;

	/** The main window's restored rectangle (the rectangle it returns to when un-maximized), and whether it is maximized. */
	TOptional<FStageWindowPlacement> GetMainWindowPlacement() const;

	/** Moves and resizes the main window. Ignored in fullscreen modes, where the window size is the display mode (UGameUserSettings). */
	void ApplyMainWindowPlacement(const FStageWindowPlacement& Placement);

	/** Registers a spawner for every panel that does not have one yet, e.g. when panel definitions are discovered after Install. */
	void RegisterPanelSpawners();

	/** Rebuilds the content of every open widget panel and the status bar, e.g. for a new local controller after level travel. */
	void RefreshPanelContent();

	/** Opens (or brings to front) a panel. Returns false for a panel without a spawner. */
	bool OpenPanel(const FGameplayTag& PanelTag);

	/** Closes a widget panel. Returns false for the viewport (it cannot close) or a panel that is not open. */
	bool ClosePanel(const FGameplayTag& PanelTag);

	EStagePanelHost GetPanelHost(const FGameplayTag& PanelTag) const;

	/** The OS window that currently shows the 3D viewport, or null before Install. */
	TSharedPtr<class SWindow> GetViewportWindow() const;

	/** Every panel this shell can spawn, viewport first. */
	TArray<FGameplayTag> GetPanels() const;

	/** Viewport left, faders below it, inspector on the right: the arrangement of the fixed HUD. */
	static TSharedRef<FTabManager::FLayout> MakeDefaultLayout();

	/** Only the viewport, filling the main window. Other panels stay available from the Window menu. */
	static TSharedRef<FTabManager::FLayout> MakeViewportOnlyLayout();

	/** An empty panel layout with the current PanelLayoutVersion. Every panel layout must start here. */
	static TSharedRef<FTabManager::FLayout> NewPanelLayout();

	static FTabId ToTabId(const FGameplayTag& PanelTag);

private:
	TSharedRef<class SDockTab> SpawnWorkspaceTab(const FSpawnTabArgs& Args);
	TSharedRef<class SDockTab> SpawnViewportTab(const FSpawnTabArgs& Args);
	TSharedRef<class SDockTab> SpawnWidgetPanelTab(const FSpawnTabArgs& Args, FGameplayTag PanelTag);

	/** A fresh panel tab manager with every panel spawner registered. Fresh, so no spawner still tracks a tab from a previous layout. */
	void CreatePanelTabManager(const TSharedRef<class SDockTab>& InWorkspaceTab);

	/**
	 * Destroys every panel tab and floating panel window now, and drops the panel tab manager.
	 * FTabManager::CloseAllAreas is not used: it only requests (deferred) window destruction, so old tabs stay alive and
	 * block their spawners, and for the embedded primary area it would request destroying the main window itself.
	 */
	void TearDownPanels();

	/** The owner's status bar, or nothing when the owner does not provide one. */
	TSharedRef<class SWidget> MakeStatusBarContent() const;

	/** The Workspace tab's content: the menu bar, the restored panel area and the status bar. */
	TSharedRef<class SWidget> BuildWorkspaceContent(const TSharedRef<FTabManager::FLayout>& PanelLayout);

	void SyncReportedHosts();
	void HandleTabRelocated(FGameplayTag PanelTag);
	void HandleTabClosed(TSharedRef<class SDockTab> Tab, FGameplayTag PanelTag);
	void HandlePersistLayout(const TSharedRef<FTabManager::FLayout>& Layout);
	void HandleMainWindowCloseRequested(const TSharedRef<class SWindow>& Window);
	void HandleViewportMoved();
	void NotifyHostChanged(const FGameplayTag& PanelTag, EStagePanelHost NewHost);
	TSharedPtr<class SDockTab> FindLiveTab(const FGameplayTag& PanelTag) const;

	TWeakPtr<class SWindow> MainWindow;
	TWeakPtr<class SViewport> ViewportWidget;
	FStageWorkspaceShellCallbacks Callbacks;

	/** Shown above the panels. Owned here so it survives layout changes. */
	TSharedPtr<class SWidget> MenuBarWidget;

	/** Holds the status bar below the panels. Owned here so its content can be replaced for a new controller. */
	TSharedPtr<class SBox> StatusBarSlot;

	/** The major tab in the main window. Owns PanelTabManager. */
	TWeakPtr<class SDockTab> WorkspaceTab;

	/** Owns the panel tabs, their layout and floating panel windows. Created when the Workspace tab spawns. */
	TSharedPtr<FTabManager> PanelTabManager;

	/** The panel layout the Workspace tab restores when it spawns. */
	TSharedPtr<FTabManager::FLayout> PendingPanelLayout;

	/**
	 * Last host reported per panel. This is derived state: Slate's tab manager is the source of truth.
	 * It is kept only to report PreviousHost and to suppress duplicate notifications. It is
	 * re-synchronised from Slate after every layout restore.
	 */
	TMap<FGameplayTag, EStagePanelHost> ReportedHosts;

	/** Restoring a layout spawns and relocates every tab. Those are not user actions and must not be reported. */
	bool bRestoringLayout = false;

	bool bInstalled = false;
};
