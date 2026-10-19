// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Actors/ModularTransformGizmo.h"
#include "Blueprint/UserWidget.h"
#include "StageViewportToolbar.generated.h"

/**
 * The viewport header toolbar (Docs/ADR/0008-viewport-toolbar-projections-and-view-modes.md §4.6), shown in the viewport tab
 * above the 3D view: projection dropdown, Lit / Unlit / Lines, grid toggle, Move / Rotate / Scale, grid snap and angle snap.
 *
 * A view in the project's binding pattern:
 *  - It writes only through the owners' setters: UStageViewportComponent (projection, view mode, grid), UStageSnappingComponent
 *    (grid and angle snap) and AModularTransformGizmo (mode). These are local tool preferences, not game state, so no
 *    GameMode rule is involved.
 *  - It never shows its own copy: after every write it reads back, and it redraws from OnViewChanged, OnSnapSettingsChanged
 *    and OnModeChanged, so changes from the keyboard (Space) or the console show here too.
 *  - Bound in NativeConstruct, unbound in NativeDestruct. No tick. Its buttons never take keyboard focus, so viewport
 *    shortcuts keep working after a click.
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

	void HandleViewModePicked(int32 Index);
	void HandleGizmoModePicked(int32 Index);
	void HandleGridTogglePicked(int32 Index);

	UFUNCTION()
	void HandleProjectionSelected(FString SelectedItem, ESelectInfo::Type SelectionType);

	UFUNCTION()
	void HandleGridSnapSelected(FString SelectedItem, ESelectInfo::Type SelectionType);

	UFUNCTION()
	void HandleAngleSnapSelected(FString SelectedItem, ESelectInfo::Type SelectionType);

	UFUNCTION()
	void HandleViewChanged();

	UFUNCTION()
	void HandleSnapSettingsChanged();

	UFUNCTION()
	void HandleGizmoModeChanged(EGizmoMode NewMode);

	UPROPERTY(Transient)
	TObjectPtr<class UStageThemedComboBox> ProjectionCombo = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UStageThemedComboBox> GridSnapCombo = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UStageThemedComboBox> AngleSnapCombo = nullptr;

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> ViewModeButtons;

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> GizmoModeButtons;

	/** One toggle button, kept as a one-option segment so it styles like the others. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> GridToggleButtons;

	TWeakObjectPtr<class UStageViewportComponent> BoundViewport;
	TWeakObjectPtr<class UStageSnappingComponent> BoundSnapping;
	TWeakObjectPtr<class AModularTransformGizmo> BoundGizmo;
};
