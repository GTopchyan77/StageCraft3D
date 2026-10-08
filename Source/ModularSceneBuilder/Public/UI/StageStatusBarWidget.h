// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Economy/StageEconomyTypes.h"
#include "History/StageEditCommand.h"
#include "Placement/StagePlacementTypes.h"
#include "Render/StageRenderTypes.h"
#include "Scene/StageSceneTypes.h"
#include "StageStatusBarWidget.generated.h"

namespace StageStatusBar
{
	/**
	 * The context hint for an edit mode: what a click does and which keys apply. ArmedItemName is empty when nothing is armed.
	 * With more than one item selected (Select mode), the hint names the group actions instead.
	 */
	MODULARSCENEBUILDER_API FText MakeHint(EStageEditMode Mode, const FText& ArmedItemName, int32 SelectedCount = 0);
}

/**
 * The workspace status bar along the bottom of the main window, the place for instructions and feedback instead of
 * text drawn over the viewport:
 * - left: the edit mode as a chip (SELECT / PLACE) and a hint for what a click does now;
 * - right: a short-lived message: refused requests (warning color), purchases, camera speed changes, undo / redo,
 *   scene saves and loads, render progress and results.
 *
 * A view only. It is owned by the local AModularPlayerController (CreateWidget) and binds, in NativeConstruct, to the
 * placement tool, the controller's OnRequestRejected, the economy's OnPurchaseCompleted and the camera pawn's
 * OnFlySpeedChanged, the edit history's OnHistoryChanged, the selection's OnSelectionSetChanged, the scene component's
 * OnSceneOperationFinished and the render subsystem's state and results; everything is unbound in NativeDestruct. No tick: the message
 * clears on a one-shot timer.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageStatusBarWidget : public UUserWidget
{
	GENERATED_BODY()

protected:
	//~ Begin UUserWidget Interface
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	//~ End UUserWidget Interface

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Status Bar")
	TObjectPtr<class UStageCraftUITheme> Theme = nullptr;

	/** How long a message stays before the right side clears. */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Status Bar", meta = (ClampMin = "0.5", Units = "s"))
	float MessageDuration = 4.f;

private:
	enum class EMessageSeverity : uint8
	{
		Info,
		Warning,
	};

	void BuildTree();
	void BindSources();
	void UnbindSources();
	void BindCameraPawn(class APawn* Pawn);
	void RefreshMode();
	void ShowMessage(const FText& Message, EMessageSeverity Severity);
	void ClearMessage();
	const class UStageCraftUITheme& GetTheme() const;

	UFUNCTION()
	void HandleEditModeChanged(EStageEditMode NewMode, EStageEditMode PreviousMode);

	UFUNCTION()
	void HandleArmedItemChanged(class UBaseItemData* ArmedItem);

	UFUNCTION()
	void HandleRequestRejected(const FStageEconomyResultInfo& Result);

	UFUNCTION()
	void HandlePurchaseCompleted(class UStageProductData* Product, FStageEconomyResultInfo Result);

	UFUNCTION()
	void HandlePossessedPawnChanged(class APawn* PreviousPawn, class APawn* NewPawn);

	UFUNCTION()
	void HandleFlySpeedChanged(float NewFlySpeed);

	UFUNCTION()
	void HandleHistoryChanged(EStageHistoryChange Change, const FText& Description);

	UFUNCTION()
	void HandleSelectionSetChanged(int32 Count);

	UFUNCTION()
	void HandleSceneOperationFinished(const FStageSceneOperationResult& Result);

	UFUNCTION()
	void HandleRenderStateChanged(EStageRenderState NewState);

	UFUNCTION()
	void HandleRenderFinished(const FStageRenderResult& Result);

	UPROPERTY(Transient)
	TObjectPtr<class UBorder> ModeChip = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> ModeText = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> HintText = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> MessageText = nullptr;

	TWeakObjectPtr<class AModularPlayerController> Controller;
	TWeakObjectPtr<class UStagePlacementToolComponent> PlacementTool;
	TWeakObjectPtr<class UStageEconomySubsystem> Economy;
	TWeakObjectPtr<class AStageCameraPawn> CameraPawn;
	TWeakObjectPtr<class UStageEditHistorySubsystem> History;
	TWeakObjectPtr<class USelectionComponent> Selection;
	TWeakObjectPtr<class UStageSceneComponent> SceneFiles;
	TWeakObjectPtr<class UStageRenderSubsystem> Render;

	FTimerHandle MessageTimer;
};
