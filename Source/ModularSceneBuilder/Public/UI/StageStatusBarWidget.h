// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Economy/StageEconomyTypes.h"
#include "History/StageEditCommand.h"
#include "Import/StageMeshImportTypes.h"
#include "Input/StageKeyBindings.h"
#include "Placement/StagePlacementTypes.h"
#include "Placement/StageSpanToolComponent.h"
#include "Render/StageRenderTypes.h"
#include "Report/StageBillOfMaterialsSubsystem.h"
#include "Scene/StageSceneTypes.h"
#include "StageStatusBarWidget.generated.h"

namespace StageStatusBar
{
	/**
	 * The keys the hints name, from the user's key map (Docs/ADR/0013-preferences-key-bindings-and-audit-fixes.md §2.4). An empty
	 * text is an unbound command; the hint then leaves that part out.
	 */
	struct MODULARSCENEBUILDER_API FHintKeys
	{
		FText Cancel;
		FText TogglePlace;
		FText CycleTool;
		FText Delete;
		FText FinishPath;
		FText RemovePoint;
		/** "WASD", or empty when a fly direction is unbound. */
		FText Fly;

		static FHintKeys FromBindings(const FStageKeyBindings& Bindings);
		static FHintKeys Defaults() { return FromBindings(FStageKeyBindings::MakeDefaults()); }
	};

	/**
	 * The context hint for an edit mode: what a click does and which keys apply. ArmedItemName is empty when nothing is armed.
	 * With more than one item selected (Select mode), the hint names the group actions instead.
	 */
	MODULARSCENEBUILDER_API FText MakeHint(EStageEditMode Mode, const FText& ArmedItemName, int32 SelectedCount = 0, const FHintKeys& Keys = FHintKeys::Defaults());

	/** The hint while drawing a span: which point the next click sets and, with a run under the cursor, its length and segment count. */
	MODULARSCENEBUILDER_API FText MakeSpanHint(const FStageSpanToolStatus& Status, const FHintKeys& Keys = FHintKeys::Defaults());
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
 * OnCameraSpeedChanged, the edit history's OnHistoryChanged, the selection's OnSelectionSetChanged, the scene component's
 * OnSceneOperationFinished, the render subsystem's state and results, and the input preferences (the hint names the user's
 * keys); everything is unbound in NativeDestruct. No tick: the message
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
	void HandleCameraSpeedChanged(float NewCameraSpeed);

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

	UFUNCTION()
	void HandleSpanStatusChanged(const FStageSpanToolStatus& Status);

	/** The key map changed: the hint names the new keys. */
	UFUNCTION()
	void HandleInputPreferencesChanged();

	UFUNCTION()
	void HandleReportExportFinished(const FStageReportExportResult& Result);

	UFUNCTION()
	void HandleModelImportStarted(const FString& SourceFile);

	UFUNCTION()
	void HandleModelImportFinished(const FStageMeshImportOutcome& Outcome);

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
	TWeakObjectPtr<class UStageSpanToolComponent> SpanTool;
	TWeakObjectPtr<class UStageBillOfMaterialsSubsystem> BillOfMaterials;
	TWeakObjectPtr<class UStagePatchSheetSubsystem> PatchSheet;
	TWeakObjectPtr<class UStageCableScheduleSubsystem> CableSchedule;
	TWeakObjectPtr<class UStageMeshImportSubsystem> Importer;
	TWeakObjectPtr<class UStageInputPreferencesSubsystem> InputPreferences;

	FTimerHandle MessageTimer;
};
