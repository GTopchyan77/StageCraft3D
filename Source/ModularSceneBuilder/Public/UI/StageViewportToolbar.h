// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Actors/ModularTransformGizmo.h"
#include "Blueprint/UserWidget.h"
#include "StageViewportToolbar.generated.h"

/**
 * The viewport header toolbar (Docs/ADR/0008-viewport-toolbar-projections-and-view-modes.md §4.6), shown in the viewport tab
 * above the 3D view, as one horizontal row of five sections separated by thin dividers with equal gaps: the projection dropdown,
 * the View menu button (an Unreal-style popup with the view modes and the Show > Grid toggle), grid snap (Off / 10 / 50 / 100 cm)
 * and angle snap (Off / 5 / 15 / 45 degrees) as toggling segments, and Move / Rotate / Scale.
 *
 * A view in the project's binding pattern:
 *  - It writes only through the owners' setters: UStageViewportComponent (projection, view mode, grid), UStageSnappingComponent
 *    (grid and angle snap) and AModularTransformGizmo (mode). These are local tool preferences, not game state, so no
 *    GameMode rule is involved.
 *  - It never shows its own copy: after every write it reads back, and it redraws from OnViewChanged, OnSnapSettingsChanged
 *    and OnModeChanged, so changes from the keyboard (Space) or the console show here too.
 *  - Bound in NativeConstruct, unbound in NativeDestruct. No tick. Its buttons never take keyboard focus, so viewport
 *    shortcuts keep working after a click.
 *  - The View menu is built fresh on every open from the current state, so it holds no bindings and needs no redraw.
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
	class UWidget* MakeLabel(const FText& Text) const;
	class UWidget* MakeSeparator() const;
	class UTextBlock* MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color) const;
	const class UStageCraftUITheme& GetTheme() const;
	void StyleChoice(class UStageChoiceButton& Button, bool bSelected) const;

	/** Shows the owners' current state; the only way the controls change outside a click. */
	void Refresh();

	class AModularPlayerController* GetStageController() const;

	/** The View menu's popup: the view modes (radio) and Show > Grid (toggle), from the viewport's state at this moment. */
	TSharedRef<class SWidget> BuildViewMenu();

	void HandleGizmoModePicked(int32 Index);
	void HandleGridSnapPicked(int32 Index);
	void HandleAngleSnapPicked(int32 Index);

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

	/** One button per StageViewMath::GetGridSnapPresets entry; the button index is the preset index. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> GridSnapButtons;

	/** One button per StageViewMath::GetAngleSnapPresets entry; the button index is the preset index. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> AngleSnapButtons;

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> GizmoModeButtons;

	TWeakObjectPtr<class UStageViewportComponent> BoundViewport;
	TWeakObjectPtr<class UStageSnappingComponent> BoundSnapping;
	TWeakObjectPtr<class AModularTransformGizmo> BoundGizmo;
};
