// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Framework/Docking/TabManager.h"
#include "GameplayTagContainer.h"
#include "Workspace/StageWorkspaceTypes.h"

/** Callbacks the shell needs from its owner. Bound by UStageWorkspaceSubsystem with weak captures. */
struct FStageWorkspaceShellCallbacks
{
	/** Builds the content of a widget panel. Called on spawn and on RefreshPanelContent. Must not return null. */
	TFunction<TSharedRef<class SWidget>(const FGameplayTag& PanelTag)> CreatePanelContent;

	/** A panel was docked, floated or closed. Not called while a layout is being restored. */
	TFunction<void(const FGameplayTag& PanelTag, EStagePanelHost NewHost, EStagePanelHost PreviousHost)> PanelHostChanged;

	/** Makes the widget the main window's content (UStageCraftGameEngine::SetMainWindowContent). */
	TFunction<void(const TSharedRef<class SWidget>& Content)> SetMainWindowContent;

	/** Gives the main window back its bare game viewport (UStageCraftGameEngine::RestoreMainWindowViewport). */
	TFunction<void()> RestoreMainWindowViewport;
};

/**
 * All Slate for the workspace, structured the way Slate expects standalone apps to be (Trace Insights, the level editor):
 * - The global tab manager's primary area in the main window holds one major tab, the Workspace, with its tab well hidden.
 * - The Workspace tab owns a panel tab manager. Panels (viewport, inspector, faders) are panel tabs in it.
 *   Panel layouts, splits, tab stacks and floating panel windows all belong to that manager.
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
	FStageWorkspaceShell(const TSharedRef<class SWindow>& InMainWindow, const TSharedRef<class SViewport>& InViewportWidget, FStageWorkspaceShellCallbacks InCallbacks);

	/** Installs the Workspace in the main window with PanelLayout as its panel arrangement. Call once, before anything else. */
	void Install(const TSharedRef<FTabManager::FLayout>& PanelLayout);

	/** Closes every panel and floating window and gives the main window back its bare viewport. Safe to call twice. */
	void Shutdown();

	/** Closes every panel and floating panel window, then restores PanelLayout. */
	void ApplyLayout(const TSharedRef<FTabManager::FLayout>& PanelLayout);

	/** Rebuilds the content of every open widget panel, e.g. for a new local controller after level travel. */
	void RefreshPanelContent();

	/** Opens (or brings to front) a panel. Returns false for an unknown panel. */
	bool OpenPanel(const FGameplayTag& PanelTag);

	EStagePanelHost GetPanelHost(const FGameplayTag& PanelTag) const;

	/** The OS window that currently shows the 3D viewport, or null before Install. */
	TSharedPtr<class SWindow> GetViewportWindow() const;

	static TSharedRef<FTabManager::FLayout> MakeDefaultLayout();

	/** Every panel this shell can spawn, viewport first. */
	static TArray<FGameplayTag> GetKnownPanels();

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

	TSharedRef<class SWidget> RestorePanels(const TSharedRef<FTabManager::FLayout>& PanelLayout, const TSharedPtr<class SWindow>& OwnerWindow);
	void SyncReportedHosts();
	void HandleTabRelocated(FGameplayTag PanelTag);
	void HandleTabClosed(TSharedRef<class SDockTab> Tab, FGameplayTag PanelTag);
	void HandleViewportMoved();
	void NotifyHostChanged(const FGameplayTag& PanelTag, EStagePanelHost NewHost);
	TSharedPtr<class SDockTab> FindLiveTab(const FGameplayTag& PanelTag) const;

	TWeakPtr<class SWindow> MainWindow;
	TWeakPtr<class SViewport> ViewportWidget;
	FStageWorkspaceShellCallbacks Callbacks;

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
