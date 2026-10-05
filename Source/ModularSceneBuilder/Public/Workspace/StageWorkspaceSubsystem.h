// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Workspace/StageWorkspaceTypes.h"
#include "StageWorkspaceSubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnStagePanelHostChanged, FGameplayTag, PanelTag, EStagePanelHost, NewHost, EStagePanelHost, PreviousHost);

/**
 * Editor-style dockable workspace: panels in Slate dock tabs, and the 3D viewport in a tab that can be
 * floated to another monitor. Design: Docs/ADR/0001-dockable-workspace.md.
 *
 * Lives on the GameInstance because OS windows and layouts outlive level travel. Holds no game state;
 * panels keep writing through the controller's request bridge.
 *
 * Supported only in Standalone Game and packaged builds running UStageCraftGameEngine.
 * - In the editor and PIE the subsystem is not created.
 * - In an unsupported game run it stays Unsupported.
 * In both cases the controller's plain HUD is the UI.
 *
 * Game thread only. Not network-relevant: purely local presentation.
 */
UCLASS(Config = Game)
class MODULARSCENEBUILDER_API UStageWorkspaceSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem Interface
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	/**
	 * Called by the local AModularPlayerController in BeginPlay, before it decides whether to show its HUD.
	 * - The first call decides support and installs the workspace. The main window and viewport exist by then.
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

	/** Opens or focuses a panel. Returns false when the workspace is not active or the panel is unknown. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Workspace")
	bool OpenPanel(FGameplayTag PanelTag);

	UFUNCTION(BlueprintCallable, Category = "StageCraft|Workspace")
	EStagePanelHost GetPanelHost(FGameplayTag PanelTag) const;

	UFUNCTION(BlueprintCallable, Category = "StageCraft|Workspace")
	void ResetToDefaultLayout();

	/** A panel was docked into the main window, floated into its own window, or closed. */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Workspace")
	FOnStagePanelHostChanged OnPanelHostChanged;

#if !UE_BUILD_SHIPPING
	/** For the development console commands only (StageCraft.Workspace.*). */
	TSharedPtr<class FStageWorkspaceShell> GetShellForDiagnostics() const { return Shell; }
#endif

private:
	/** Uninitialized -> Ready or Unsupported. No-op in any other state. */
	void InstallIfSupported();
	TSharedRef<class SWidget> CreatePanelContent(const FGameplayTag& PanelTag);
	TSubclassOf<class UUserWidget> ResolvePanelWidgetClass(const FGameplayTag& PanelTag) const;
	void HandlePanelHostChanged(const FGameplayTag& PanelTag, EStagePanelHost NewHost, EStagePanelHost PreviousHost);

	/**
	 * Interim panel content (Phase 0/1). UStagePanelDefinition data assets replace these in Phase 1.
	 * Loaded synchronously on first spawn. That is acceptable for two small widgets; it moves to an Asset Manager bundle later.
	 */
	UPROPERTY(Config)
	TSoftClassPtr<class UUserWidget> InspectorPanelClass;

	UPROPERTY(Config)
	TSoftClassPtr<class UUserWidget> FaderBankPanelClass;

	TSharedPtr<class FStageWorkspaceShell> Shell;

	/** The local UI bridge the panels are built against. Owned by the world; null between levels. */
	TWeakObjectPtr<class AModularPlayerController> LocalController;

	EStageWorkspaceState State = EStageWorkspaceState::Uninitialized;
};
