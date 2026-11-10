// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Scene/StageSceneTypes.h"
#include "Startup/StageStartupTypes.h"
#include "Containers/Ticker.h"
#include "Engine/TimerHandle.h"
#include "Tasks/Task.h"
#include "Workspace/StageWorkspaceTypes.h"
#include "StageWorkspaceSubsystem.generated.h"

/** The answers to "Save changes before closing?" (UStageWorkspaceSubsystem::AnswerExitPrompt). */
enum class EStageExitChoice : uint8
{
	/** Save the stage (under its scene name, or the name typed for an untitled stage), then quit once it is written. */
	Save,
	/** Quit without saving; the stage's autosave is deleted too. */
	Discard,
	/** Keep the app open. */
	Cancel,
};

/** Why the save prompt (SStageUnsavedChangesDialog) is showing, which decides its wording, its buttons and what a save leads to. */
enum class EStageSavePromptPurpose : uint8
{
	/** "Save changes before closing?": Save, Don't Save, Cancel; a save closes the app once written. */
	CloseApp,
	/** Ctrl+S on a stage that was never saved (ADR 0013 addendum): a name, Save, Cancel; the app stays open. */
	NameAndSave,
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnStagePanelHostChanged, FGameplayTag, PanelTag, EStagePanelHost, NewHost, EStagePanelHost, PreviousHost);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStageLayoutApplied, const FString&, LayoutName);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnStageLayoutsChanged);

/**
 * Editor-style dockable workspace and its window manager (Docs/ADR/0001-dockable-workspace.md):
 * - panels in Slate dock tabs that can be split, tabbed, floated to other monitors and closed;
 * - the 3D viewport in a tab that can float but never close;
 * - named layouts saved to disk, and the live arrangement restored on the next launch.
 *
 * Lives on the GameInstance because OS windows and layouts outlive level travel. Holds no game state:
 * panels keep writing through the controller's request bridge, and layout files are local presentation only.
 *
 * Views (the Window/Layout menu bar, any UMG layout panel) follow the project's binding pattern:
 * - read the state once with the getters;
 * - bind OnPanelHostChanged / OnLayoutApplied / OnLayoutsChanged;
 * - commit through the validated operations here, which return an EStageLayoutResult and never throw away user data.
 *
 * Supported only in Standalone Game and packaged builds running UStageCraftGameEngine.
 * - In the editor and PIE the subsystem is not created.
 * - In an unsupported game run it stays Unsupported.
 * In both cases the controller's plain HUD is the UI, and every operation returns WorkspaceInactive / false.
 *
 * Game thread only. Not network-relevant: purely local presentation.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageWorkspaceSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	/** Built-in layouts are defined in C++, never written to disk, and cannot be overwritten or deleted, so Reset always works. */
	static const FString DefaultLayoutName;
	static const FString ViewportOnlyLayoutName;
	/** Built-in layout for running a show (Docs/ADR/0026 §6): viewport, playbacks, cues, groups, palettes and the inspector. */
	static const FString LightingDeskLayoutName;

	//~ Begin USubsystem Interface
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	/**
	 * Called by the local AModularPlayerController in BeginPlay, before it decides whether to show its HUD.
	 * - The first call decides support and installs the workspace with the last session's layout (or Default).
	 *   The main window and viewport exist by then.
	 * - After level travel, the new controller's call rebuilds the panel content against it.
	 * Null or non-local controllers are ignored.
	 */
	void RegisterLocalController(class AModularPlayerController* Controller);

	/** Called by the same controller in EndPlay. Panels show an empty state until the next registration. */
	void UnregisterLocalController(class AModularPlayerController* Controller);

	UFUNCTION(BlueprintPure, Category = "StageCraft|Workspace")
	EStageWorkspaceState GetState() const { return State; }

	/** True when panels are hosted in dock tabs. The controller then keeps only viewport overlays on screen. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Workspace")
	bool IsWorkspaceActive() const { return State == EStageWorkspaceState::Ready; }

	/** True when panels are, or are about to be, hosted in dock tabs (the install may wait for the engine's loading screen). */
	bool WillHostPanels() const { return State == EStageWorkspaceState::Ready || State == EStageWorkspaceState::WaitingForLoadingScreen; }

	// --- Panels ---

	/** Whether every panel definition and its UI bundle has loaded (also true when there are none). */
	bool IsPanelCatalogLoaded() const { return bPanelCatalogLoaded; }

	/** Load progress of the panel definitions in [0, 1]: 1 once loaded. For the startup loader (Docs/ADR/0024 §2). */
	float GetPanelCatalogLoadProgress() const;

	/** Pushed while the panel definitions stream in. */
	FOnStageLoadProgress OnPanelCatalogLoadProgress;

	/** Fires once the panel definitions have loaded. */
	FSimpleMulticastDelegate OnPanelCatalogLoaded;


	/** The viewport first, then every panel found by the Asset Manager (StagePanel), sorted by tag. Valid in any state. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Workspace")
	TArray<FGameplayTag> GetAvailablePanels() const;

	/** Tab label of a panel. Falls back to the tag's last segment while the definition is still loading. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Workspace")
	FText GetPanelDisplayName(FGameplayTag PanelTag) const;

	/** Opens or focuses a panel. Returns false when the workspace is not active or the panel is unknown. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Workspace")
	bool OpenPanel(FGameplayTag PanelTag);

	/** Closes a panel. Returns false for the viewport (it cannot close), a panel that is not open, or an inactive workspace. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Workspace")
	bool ClosePanel(FGameplayTag PanelTag);

	/**
	 * SHOW ME (Docs/ADR/0030 §7): opens PanelTag and pulses its anchored control ControlId (IStageAnchoredPanel), scrolled into
	 * view. A panel builds its widgets as it opens, so the control is looked for over the next half second; one request at a time,
	 * a newer one replaces it. False when the workspace is not active or the panel cannot open. The panel still opens when the
	 * control is not shown now (a collapsed section); that is logged.
	 */
	bool ShowControl(FGameplayTag PanelTag, FName ControlId);

	/** The widget PanelTag was last built with, while it lives; null for a panel never opened or the viewport. */
	class UUserWidget* FindPanelWidget(FGameplayTag PanelTag) const;

	UFUNCTION(BlueprintCallable, Category = "StageCraft|Workspace")
	EStagePanelHost GetPanelHost(FGameplayTag PanelTag) const;

	/**
	 * A starter stage was opened (welcome screen, File > New from Template, the assistant or a console command): the stage is shown
	 * on its own, with nothing popping up over it (STATE.md #76). Closes the welcome screen and every panel in a floating window;
	 * panels docked in the main window stay as they are, and nothing is opened. Returns the floating panels closed. Idempotent.
	 */
	int32 PresentStarterStage();

	// --- Layouts ---

	/**
	 * Replaces the whole arrangement with a built-in or saved user layout. A user layout also moves the main window, clamped to the current monitors.
	 * A corrupt or incompatible file is renamed to .bak, OnLayoutsChanged fires, and the current arrangement is kept.
	 */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Workspace")
	EStageLayoutResult ApplyLayout(const FString& LayoutName);

	/** Saves the live arrangement and main window rectangle as a user layout, replacing one with the same name. Synchronous (a few KB). */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Workspace")
	EStageLayoutResult SaveCurrentLayoutAs(const FString& LayoutName);

	/** Deletes a saved user layout file. Built-in layouts are refused (ReservedName). */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Workspace")
	EStageLayoutResult DeleteLayout(const FString& LayoutName);

	/** ApplyLayout(DefaultLayoutName). The main window stays where it is. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Workspace")
	void ResetToDefaultLayout();

	UFUNCTION(BlueprintPure, Category = "StageCraft|Workspace")
	TArray<FString> GetBuiltInLayouts() const;

	/** Saved user layouts, sorted. Reads the layouts folder: call it when a view opens, not every frame. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Workspace")
	TArray<FString> GetUserLayouts() const;

	UFUNCTION(BlueprintPure, Category = "StageCraft|Workspace")
	bool IsBuiltInLayout(const FString& LayoutName) const;

	/** The layout last applied or saved: what the live arrangement is based on. Empty when that layout was deleted. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Workspace")
	const FString& GetActiveLayoutName() const { return ActiveLayoutName; }

	/** A panel was docked into the main window, floated into its own window, or closed by the user. */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Workspace")
	FOnStagePanelHostChanged OnPanelHostChanged;

	/** A whole layout replaced the arrangement (ApplyLayout, Reset). */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Workspace")
	FOnStageLayoutApplied OnLayoutApplied;

	/** The list of user layouts changed (saved, deleted, or a bad file moved aside). */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Workspace")
	FOnStageLayoutsChanged OnLayoutsChanged;

#if !UE_BUILD_SHIPPING
	/** For the development console commands only (StageCraft.Workspace.*). */
	TSharedPtr<class FStageWorkspaceShell> GetShellForDiagnostics() const { return Shell; }
#endif

	/** Whether "Save changes before closing?" is showing (or saving before it quits). */
	bool IsExitPromptOpen() const { return ExitPromptState != EExitPromptState::Closed && PromptPurpose == EStageSavePromptPurpose::CloseApp; }

	/** Whether any save prompt is showing (the exit prompt, or Ctrl+S naming a new stage). Shortcuts stand aside meanwhile. */
	bool IsSavePromptOpen() const { return ExitPromptState != EExitPromptState::Closed; }

	/** True while anything covers the workspace and owns the keys: a save prompt, the welcome screen or the startup loader. */
	bool IsModalOverlayOpen() const { return IsSavePromptOpen() || WelcomeDialog.IsValid() || StartupLoader.IsValid(); }

	/**
	 * Ctrl+S on a stage that was never saved: asks for a name over the workspace and saves under it; the app stays open.
	 * False (nothing shown) without the workspace or while a prompt is already open; the caller then reports how to save instead.
	 */
	bool ShowNameAndSavePrompt();

	/**
	 * Answers the open exit prompt, exactly as its buttons do. SceneName is the name to save an untitled stage under; it is
	 * ignored for a stage that already has one. Save quits only after the file is written; a failed save keeps the prompt
	 * open with the reason. Ignored when no prompt is open, or while its save is running.
	 */
	void AnswerExitPrompt(EStageExitChoice Choice, const FString& SceneName);

private:
	/** Uninitialized -> Ready or Unsupported. No-op in any other state. */
	void InstallIfSupported();

	/** Covers the freshly installed workspace with the startup loader until the catalogs are ready (Docs/ADR/0024 §2). */
	/** The engine's loading screen has handed the main window back: install now (WaitingForLoadingScreen). */
	void HandleLoadingScreenFinished();
	void ShowStartupLoader();
	void HandleStartupSettled(EStageStartupState NewState);
	/** Fades the loader out and removes it. Idempotent. */
	void DismissStartupLoader();
	void HandleStartupLoaderClosed();
	/** The welcome screen with the starter stages (Docs/ADR/0024 §6), unless switched off or another prompt is up. */
	void ShowWelcome();
	void CloseWelcome();

	void LoadPanelCatalog();
	void HandlePanelCatalogLoaded();
	TSharedRef<class SWidget> CreatePanelContent(const FGameplayTag& PanelTag);

	/** The status bar for the local controller (UStageStatusBarWidget), or nothing until one registers. */
	TSharedRef<class SWidget> CreateStatusBarContent();

	/** The viewport toolbar for the local controller (UStageViewportToolbar), or nothing until one registers. */
	TSharedRef<class SWidget> CreateViewportToolbarContent();

	void HandlePanelHostChanged(const FGameplayTag& PanelTag, EStagePanelHost NewHost, EStagePanelHost PreviousHost);

	/** Clamps, validates and applies a saved layout. Corrupt when Slate cannot read its panel layout. */
	EStageLayoutResult ApplySavedLayout(struct FStageSavedLayout& Layout);
	bool CaptureCurrentLayout(const FString& Name, struct FStageSavedLayout& OutLayout) const;
	void SetActiveLayout(const FString& LayoutName);

	/** Writes the live arrangement to the session file on a background task. Writes run in order. */
	void SaveSessionAsync();
	/** Writes the live arrangement now, after any pending background write. Used when the app closes. Seals the session. */
	void SaveSessionNow();
	void HandleLayoutChanged();
	void HandleMainWindowClosing();

	/**
	 * The main window's close button (ADR 0010 §3.2): true closes now (no unsaved changes, or no stage); false keeps the window
	 * and shows the exit prompt, which later quits through FStageWorkspaceShell::CloseMainWindow.
	 */
	bool ConfirmMainWindowClose();
	void ShowExitPrompt(const FString& SceneName);
	/** Shows the save prompt for Purpose; the exit prompt and Ctrl+S's naming prompt share its state, answers and save handling. */
	void ShowSavePrompt(const FString& SceneName, EStageSavePromptPurpose Purpose);
	void CloseExitPrompt();

	UFUNCTION()
	void HandleExitSaveFinished(const FStageSceneOperationResult& Result);

	enum class EExitPromptState : uint8
	{
		Closed,
		/** Waiting for Save / Don't Save / Cancel. */
		Asking,
		/** Save chosen; waiting for the scene file to be written. */
		Saving,
	};

	EExitPromptState ExitPromptState = EExitPromptState::Closed;
	/** What the open prompt is for; meaningful only while ExitPromptState is not Closed. */
	EStageSavePromptPurpose PromptPurpose = EStageSavePromptPurpose::CloseApp;

	TSharedPtr<class SStageUnsavedChangesDialog> ExitDialog;

	/** The scene component whose save the prompt waits for; bound only while Saving. */
	TWeakObjectPtr<class UStageSceneComponent> ExitSaveScenes;

	/** An untitled stage's name that would replace an existing scene: the second Save confirms it. */
	FString PendingExitOverwrite;

	TSharedPtr<class FStageWorkspaceShell> Shell;

	/** Shared only so the header needs no private include; the subsystem is the sole owner. Immutable after Initialize. */
	TSharedPtr<class FStageLayoutStore> LayoutStore;

	/** Loaded panel definitions by tag. Filled when the catalog's UI bundle has loaded. */
	UPROPERTY(Transient)
	TMap<FGameplayTag, TObjectPtr<class UStagePanelDefinition>> PanelDefinitions;

	/** Panels known from the Asset Manager scan (primary asset names are panel tags), sorted. Available before loading. */
	TArray<FGameplayTag> ScannedPanels;

	/** The widget each panel was last built with, for SHOW ME. Weak: its dock tab owns it. */
	TMap<FGameplayTag, TWeakObjectPtr<class UUserWidget>> LivePanelWidgets;

	/** Looks for the control of the pending SHOW ME; removed when it is found, times out or the subsystem goes. */
	FTSTicker::FDelegateHandle ShowControlTicker;

	/** Pulses ControlId of the open PanelTag; false while the panel has not built a visible widget for it. */
	bool PulseAnchor(const FGameplayTag& PanelTag, FName ControlId) const;

	/** Keeps the definitions and their UI bundle loaded for the run. */
	TSharedPtr<struct FStreamableHandle> PanelCatalogHandle;

	bool bPanelCatalogLoaded = false;

	/** The startup loader over the workspace, while it is shown. */
	TSharedPtr<class SStageBrandedLoader> StartupLoader;
	FDelegateHandle StartupSettledHandle;
	FDelegateHandle LoadingScreenFinishedHandle;

	/** The welcome screen while it is shown. */
	TSharedPtr<class SStageWelcomeDialog> WelcomeDialog;
	FTimerHandle WelcomeTimer;

	// --- GDTF import report (Docs/ADR/0033 §10) ---

	/** The library's reviews changed: shows the oldest one on the next tick, never inside the library's broadcast. */
	UFUNCTION()
	void HandleFixtureReviewsChanged();
	void RequestFixtureReviewRefresh();

	/**
	 * Shows the oldest waiting review in the import report dialog, or closes the dialog when none waits. Waits while the welcome
	 * screen, the save prompt or the startup loader is up (they come back here when they close).
	 */
	void ShowNextFixtureReview();
	void HandleFixtureDialogClosed();

	/** The import report while it is shown (the dialog layer holds its Slate widget; this keeps the UObject). */
	UPROPERTY(Transient)
	TObjectPtr<class UStageFixtureImportDialog> FixtureDialog = nullptr;
	TWeakObjectPtr<class UStageFixtureLibrarySubsystem> FixtureLibrary;
	bool bFixtureReviewRefreshPending = false;

	// --- MVR import review (Docs/ADR/0034 §6, MV3). Shares the dialog layer with the GDTF report: one at a time. ---

	/** Shows, updates or closes the MVR review from the importer's state. Called from ShowNextFixtureReview when no GDTF review waits. */
	void ShowMvrReview();
	void HandleMvrDialogClosed();
	void CloseMvrDialog();

	UPROPERTY(Transient)
	TObjectPtr<class UStageMvrImportDialog> MvrDialog = nullptr;
	TWeakObjectPtr<class UStageMvrImportSubsystem> MvrImporter;
	FDelegateHandle MvrChangedHandle;

	/** The local UI bridge the panels are built against. Owned by the world; null between levels. */
	TWeakObjectPtr<class AModularPlayerController> LocalController;

	FString ActiveLayoutName;

	/** The last background session write. The next one waits for it, so writes never race. */
	UE::Tasks::FTask PendingSessionWrite;

	/** Set once the final session save ran on close. Later saves would capture windows that are being destroyed. */
	bool bSessionSealed = false;

	EStageWorkspaceState State = EStageWorkspaceState::Uninitialized;
};
