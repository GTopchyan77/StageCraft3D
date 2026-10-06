// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tasks/Task.h"
#include "Workspace/StageWorkspaceTypes.h"
#include "StageWorkspaceSubsystem.generated.h"

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

	// --- Panels ---

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

	UFUNCTION(BlueprintCallable, Category = "StageCraft|Workspace")
	EStagePanelHost GetPanelHost(FGameplayTag PanelTag) const;

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

private:
	/** Uninitialized -> Ready or Unsupported. No-op in any other state. */
	void InstallIfSupported();

	void LoadPanelCatalog();
	void HandlePanelCatalogLoaded();
	TSharedRef<class SWidget> CreatePanelContent(const FGameplayTag& PanelTag);

	/** The status bar for the local controller (UStageStatusBarWidget), or nothing until one registers. */
	TSharedRef<class SWidget> CreateStatusBarContent();

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

	TSharedPtr<class FStageWorkspaceShell> Shell;

	/** Shared only so the header needs no private include; the subsystem is the sole owner. Immutable after Initialize. */
	TSharedPtr<class FStageLayoutStore> LayoutStore;

	/** Loaded panel definitions by tag. Filled when the catalog's UI bundle has loaded. */
	UPROPERTY(Transient)
	TMap<FGameplayTag, TObjectPtr<class UStagePanelDefinition>> PanelDefinitions;

	/** Panels known from the Asset Manager scan (primary asset names are panel tags), sorted. Available before loading. */
	TArray<FGameplayTag> ScannedPanels;

	/** Keeps the definitions and their UI bundle loaded for the run. */
	TSharedPtr<struct FStreamableHandle> PanelCatalogHandle;

	/** The local UI bridge the panels are built against. Owned by the world; null between levels. */
	TWeakObjectPtr<class AModularPlayerController> LocalController;

	FString ActiveLayoutName;

	/** The last background session write. The next one waits for it, so writes never race. */
	UE::Tasks::FTask PendingSessionWrite;

	/** Set once the final session save ran on close. Later saves would capture windows that are being destroyed. */
	bool bSessionSealed = false;

	EStageWorkspaceState State = EStageWorkspaceState::Uninitialized;
};
