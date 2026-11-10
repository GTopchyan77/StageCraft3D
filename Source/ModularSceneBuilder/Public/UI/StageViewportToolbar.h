// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Actors/ModularTransformGizmo.h"
#include "Blueprint/UserWidget.h"
#include "StageViewportToolbar.generated.h"

/**
 * The viewport header toolbar (Docs/ADR/0008-viewport-toolbar-projections-and-view-modes.md §4.6), shown in the viewport tab
 * above the 3D view, as one horizontal row of five dropdowns separated by thin dividers with equal gaps, as in the Unreal
 * Editor's viewport bar (STATE.md #87): the projection dropdown; View (view modes, Show > Grid); Snap (grid snap Off / 10 / 50 /
 * 100 cm, angle snap Off / 5 / 15 / 45 degrees, Snap to Items), whose label shows both steps; Transform (Move / Rotate /
 * Scale), whose label shows the active tool; and Tools (Sketch to Stage, ADR 0015, and the AI Assistant, ADR 0016).
 * Every option lives in a popup, so the bar stays one short row at any window width.
 * A « toggle at the left collapses the row to a thin strip with only a » expand button (Ctrl+Shift+T by default, as in the
 * Unreal Editor); the state is a per-machine preference owned by UStageViewportComponent (Docs/ADR/0018 §3).
 *
 * A view in the project's binding pattern:
 *  - It writes only through the owners' setters: UStageViewportComponent (projection, view mode, grid), UStageSnappingComponent
 *    (grid and angle snap) and AModularTransformGizmo (mode). These are local tool preferences, not game state, so no
 *    GameMode rule is involved.
 *  - It never shows its own copy: after every write it reads back, and it redraws from OnViewChanged, OnSnapSettingsChanged
 *    and OnModeChanged, so changes from the keyboard (Space) or the console show here too.
 *  - Bound in NativeConstruct, unbound in NativeDestruct. No tick. Its buttons never take keyboard focus, so viewport
 *    shortcuts keep working after a click.
 *  - The menus are built fresh on every open from the current state, so they hold no bindings and need no redraw.
 *
 * The tree is built in code from the theme; there is no Widget Blueprint. Created by UStageWorkspaceSubsystem for the local
 * controller.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageViewportToolbar : public UUserWidget
{
	GENERATED_BODY()

protected:
	//~ Begin UUserWidget Interface
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	//~ End UUserWidget Interface

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Viewport")
	TObjectPtr<class UStageCraftUITheme> Theme = nullptr;

private:
	void BuildTree();
	class UStageThemedComboBox* MakeCombo(FName Name, TConstArrayView<FText> Options, const FText& Tip);
	class UWidget* MakeSegment(TConstArrayView<FText> Labels, TConstArrayView<FText> Tips, TArray<TObjectPtr<class UStageChoiceButton>>& OutButtons,
		void (UStageViewportToolbar::*Handler)(int32));
	class UStageMenuButton* MakeMenuButton(FName Name, const FText& Tip, TSharedRef<class SWidget> (UStageViewportToolbar::*Builder)());
	class UWidget* MakeSeparator() const;
	class UTextBlock* MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color) const;
	const class UStageCraftUITheme& GetTheme() const;
	void StyleChoice(class UStageChoiceButton& Button, bool bSelected) const;

	/** Shows the owners' current state; the only way the controls change outside a click. */
	void Refresh();

	class AModularPlayerController* GetStageController() const;

	/** The View menu's popup: the view modes (radio) and Show > Grid (toggle), from the viewport's state at this moment. */
	TSharedRef<class SWidget> BuildViewMenu();

	/** Grid snap and angle snap presets (radio) and Snap to Items (toggle). */
	TSharedRef<class SWidget> BuildSnapMenu();

	/** Move / Rotate / Scale (radio). */
	TSharedRef<class SWidget> BuildTransformMenu();

	/** Opens (or brings forward) the Sketch to Stage and AI Assistant panels. */
	TSharedRef<class SWidget> BuildToolsMenu();

	void HandleCollapsePicked(int32 Index);

	UFUNCTION()
	void HandleProjectionSelected(FString SelectedItem, ESelectInfo::Type SelectionType);

	UFUNCTION()
	void HandleViewChanged();

	UFUNCTION()
	void HandleSnapSettingsChanged();

	UFUNCTION()
	void HandleGizmoModeChanged(EGizmoMode NewMode);

	UPROPERTY(Transient)
	TObjectPtr<class UStageThemedComboBox> ProjectionCombo = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UStageMenuButton> ViewMenuButton = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UStageMenuButton> SnapMenuButton = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UStageMenuButton> TransformMenuButton = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UStageMenuButton> ToolsMenuButton = nullptr;

	/** The collapse / expand toggle (a one-button segment). */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> CollapseButtons;

	/** The five sections, hidden while the toolbar is collapsed. */
	UPROPERTY(Transient)
	TObjectPtr<class UHorizontalBox> Sections = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class USizeBox> BarHeightBox = nullptr;

	TWeakObjectPtr<class UStageViewportComponent> BoundViewport;
	TWeakObjectPtr<class UStageSnappingComponent> BoundSnapping;
	TWeakObjectPtr<class AModularTransformGizmo> BoundGizmo;
};
