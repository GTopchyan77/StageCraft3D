// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "Data/StageParameterTypes.h"
#include "Economy/StageEconomyTypes.h"
#include "History/StageEditCommand.h"
#include "Placement/StagePlacementTypes.h"
#include "Scene/StageSceneTypes.h"
#include "ModularPlayerController.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStageRequestRejected, const FStageEconomyResultInfo&, Result);

/** What a left click on a stage item does to the selection. */
enum class EStageSelectionClick : uint8
{
	/** Plain click: the item becomes the only selection. */
	Replace,
	/** Ctrl+Click: the item is added to the selection, or removed if it was already in it. */
	Toggle,
};

/**
 * Stage editing controller. Its only jobs are Enhanced Input, cursor tracing and deciding which
 * system an input belongs to; the actual work lives in focused components/actors:
 *   UStagePlacementToolComponent (edit mode, ghost preview, single-click placement),
 *   USpawnSystemComponent (spawn/delete executor), USelectionComponent (selection),
 *   AModularTransformGizmo (move/rotate/scale), AStageCameraPawn (fly camera),
 *   UStageEditorAudioFeedbackComponent (feedback cues), UStageEditHistoryComponent (undo/redo),
 *   UStageSnappingComponent (object snapping for placement and gizmo moves).
 *
 * Mouse buttons have exclusive jobs, as in the Unreal Editor viewport:
 *  - Left, Place mode: places exactly one armed item where the ghost shows it; Place mode stays active for the next copy.
 *    Holding the button never places more.
 *  - Left, Select mode: gizmo handle (drag) > placed item (select; it never follows the mouse) > empty
 *    (deselect). Ctrl+Click adds an item to the selection or removes it, and never deselects on empty space.
 *    A gizmo drag or numeric transform edit of one selected item moves the whole selection with it
 *    (UStageGroupTransformComponent). Ignored entirely while the right button is held.
 *  - Right: navigation only. While held: mouse = look, W/S = forward/back along the view, A/D =
 *    strafe, E/Q = straight up/down world Z. It never selects, places or deletes, and is ignored
 *    while a left-button drag is in progress. Cursor hide/capture/restore is done by
 *    UStageCraftGameViewportClient, so this class never changes the input mode mid-press.
 *  - Wheel: fly speed, with or without the right button held. It scales every fly axis alike.
 *
 * Keys: P toggles Place mode, Esc leaves Place mode (or, in Select mode, deselects and disarms),
 * Delete removes every selected item (one undo step), Space cycles the gizmo Move -> Rotate -> Scale, Ctrl+Z undoes and
 * Ctrl+Y / Ctrl+Shift+Z redo (text fields keep these keys while they have focus).
 *
 * The placement preview follows the cursor without any tick: cursor moves (viewport client), camera
 * moves (pawn root) and view rotation (SetControlRotation) request one coalesced trace on the next tick,
 * and only while in Place mode.
 *
 * Request bridge: widgets never write to the game directly. Parameter edits, purchases and
 * placements go through RequestParameterChange / RequestPurchase / RequestPlaceItem and the spawn
 * component's PlacementValidator, which ask the GameMode (rules + ownership) and only then let the
 * session or economy subsystem apply them. Refusals are broadcast on OnRequestRejected.
 *
 * Input assets are designer-assignable in a Blueprint subclass. If any slot is empty, an equivalent
 * mapping is built in code, so the project works with no content.
 */
UCLASS()
class MODULARSCENEBUILDER_API AModularPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	AModularPlayerController();

	UFUNCTION(BlueprintPure, Category = "StageCraft")
	class USpawnSystemComponent* GetSpawnSystem() const { return SpawnSystem; }

	UFUNCTION(BlueprintPure, Category = "StageCraft")
	class USelectionComponent* GetSelection() const { return Selection; }

	UFUNCTION(BlueprintPure, Category = "StageCraft")
	class UStagePlacementToolComponent* GetPlacementTool() const { return PlacementTool; }

	UFUNCTION(BlueprintPure, Category = "StageCraft")
	class AModularTransformGizmo* GetGizmo() const { return Gizmo; }

	UFUNCTION(BlueprintPure, Category = "StageCraft")
	class UStageEditHistoryComponent* GetEditHistory() const { return EditHistory; }

	UFUNCTION(BlueprintPure, Category = "StageCraft")
	class UStageSnappingComponent* GetSnapping() const { return Snapping; }

	UFUNCTION(BlueprintPure, Category = "StageCraft")
	class UStageGroupTransformComponent* GetGroupTransform() const { return GroupTransform; }

	UFUNCTION(BlueprintPure, Category = "StageCraft")
	class UStageSceneComponent* GetSceneFiles() const { return SceneFiles; }

	UFUNCTION(BlueprintPure, Category = "StageCraft|UI")
	class UUserWidget* GetHUDWidget() const { return HUDWidget; }

	/** True while the right mouse button is held and the camera is in fly navigation. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Camera")
	bool IsNavigatingCamera() const { return bIsNavigatingCamera; }

	/** Cursor trace against surfaces items can be placed on (floor, level geometry, other items). */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Trace")
	bool GetPlacementHitUnderCursor(FHitResult& OutHit) const;

	/** Cursor trace that only hits placed stage items. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Trace")
	bool GetStageItemHitUnderCursor(FHitResult& OutHit) const;

	/**
	 * Cursor ray against the gizmo handles only. Each handle component is tested directly instead of
	 * through the world, so a handle buried inside another mesh is still hit, matching its always-on-top rendering.
	 */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Trace")
	bool GetGizmoHitUnderCursor(FHitResult& OutHit) const;

	//~ Begin AController Interface
	/** Also refreshes the placement preview: the fly camera turns the view through here. */
	virtual void SetControlRotation(const FRotator& NewRotation) override;
	//~ End AController Interface

	// --- Request bridge (UI -> GameMode rules -> subsystems) ---

	/** Validates with the GameMode, then applies through the session. The single write path for UI edits. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Requests")
	FStageEconomyResultInfo RequestParameterChange(UObject* Target, FGameplayTag ParameterId, const FStageParameterValue& Value);

	/** Starts a purchase. The result is the validation outcome; completion is reported on OnRequestRejected (failure) or the economy's OnPurchaseCompleted. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Requests")
	FStageEconomyResultInfo RequestPurchase(class UStageProductData* Product);

	/**
	 * Library entry point: arms Item (its spawn assets stream in first) and enters Place mode, so the
	 * ghost follows the cursor as soon as the item is ready. Refused (and reported) when the rules do not
	 * allow placing Item now, e.g. it is locked. Placing still validates again on the click.
	 */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Requests")
	FStageEconomyResultInfo RequestPlaceItem(class UBaseItemData* Item);

	/** Whether Item may be placed now (for graying out catalog entries). */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Requests")
	FStageEconomyResultInfo CanPlaceItem(const class UBaseItemData* Item) const;

	/**
	 * Marks what this player may not edit before a panel builds controls: locked parameters become
	 * read-only with bLocked set, and locked Type options are labelled. Display only; every write
	 * is still validated by RequestParameterChange.
	 */
	void DecorateParameterSections(const UObject* Target, TArray<FStageParameterSection>& Sections) const;

	/**
	 * Reverts the latest placement, deletion or transform edit (Ctrl+Z, Edit menu). Ignored while a gizmo
	 * drag or camera navigation is in progress. A step the rules refuse now (e.g. restoring an item past the
	 * session limit) stays in the history and is reported like any refused request.
	 */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Requests")
	EStageCommandResult RequestUndo();

	/** Re-applies the latest undone step (Ctrl+Y or Ctrl+Shift+Z, Edit menu). Same guards as RequestUndo. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Requests")
	EStageCommandResult RequestRedo();

	/**
	 * Deletes every selected item as one undoable step (Delete key, Edit menu). Ignored during a gizmo drag or camera
	 * navigation. Returns how many items were deleted.
	 */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Requests")
	int32 RequestDeleteSelection();

	/** Selects every placed item (Edit > Select All). Leaves Place mode first, like any selection. Same guards as delete. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Requests")
	void RequestSelectAll();

	/**
	 * Removes every placed item as one undoable step. The caller must have asked the user to confirm (the Edit menu's
	 * Clear Stage does); Ctrl+Z restores the whole stage. Same guards as delete. Returns how many items were removed.
	 */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Requests")
	int32 RequestClearStage();

	/**
	 * Saves the stage as Name (File > Save / Save As), replacing a scene of that name: the caller confirms an overwrite.
	 * Returns Success when the save started; refusals and failures are reported on OnRequestRejected, success on the scene
	 * component's OnSceneOperationFinished.
	 */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Requests")
	EStageSceneResult RequestSaveScene(const FString& Name);

	/**
	 * Replaces the stage with the saved scene Name (File > Open). Unsaved changes are lost and the undo history is cleared,
	 * so the caller confirms first when the stage is dirty. Ignored during a gizmo drag or camera navigation. Reported like Save.
	 */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Requests")
	EStageSceneResult RequestLoadScene(const FString& Name);

	/** Deletes the saved scene Name from disk (the caller confirms). The stage on screen is not changed. Reported like Save. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Requests")
	EStageSceneResult RequestDeleteScene(const FString& Name);

	/**
	 * Renders a still image of the current view with the user's render settings (Render panel, Render menu), leaving out
	 * the gizmo, the placement ghost and selection overlays. Returns true when the render started; the image arrives on
	 * UStageRenderSubsystem::OnRenderFinished. A refusal (already rendering) is reported on OnRequestRejected.
	 */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Requests")
	bool RequestRender();

	/** Every refused request (UI toasts, shop prompts, error cue). */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Requests")
	FOnStageRequestRejected OnRequestRejected;

#if !UE_BUILD_SHIPPING
	// Development verification only (StageCraft.Edit.* console commands): drive the same handlers as
	// the mouse, at a simulated cursor position in viewport pixels. Compiled out of Shipping.
	void DevSetSimulatedCursor(const FVector2D& ViewportPosition);
	void DevClearSimulatedCursor();
	void DevSimulatePrimaryPressed(EStageSelectionClick Click = EStageSelectionClick::Replace);
	void DevSimulatePrimaryHeld();
	void DevSimulatePrimaryReleased();
#endif

protected:
	//~ Begin APlayerController Interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void SetupInputComponent() override;
	//~ End APlayerController Interface

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<class USpawnSystemComponent> SpawnSystem = nullptr;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<class USelectionComponent> Selection = nullptr;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<class UStagePlacementToolComponent> PlacementTool = nullptr;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<class UStageEditorAudioFeedbackComponent> AudioFeedback = nullptr;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<class UStageEditHistoryComponent> EditHistory = nullptr;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<class UStageSnappingComponent> Snapping = nullptr;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<class UStageGroupTransformComponent> GroupTransform = nullptr;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<class UStageSceneComponent> SceneFiles = nullptr;

	/** Spawned once for the local player and re-targeted on every selection change. */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Gizmo")
	TSubclassOf<class AModularTransformGizmo> GizmoClass;

	/**
	 * Root editor UI (catalog, inspector, show control panels), created once for the local player.
	 * Clicks on its visible panels are consumed by UMG and never reach placement/selection input.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|UI")
	TSubclassOf<class UUserWidget> HUDWidgetClass;

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input")
	TObjectPtr<class UInputMappingContext> EditorMappingContext = nullptr;

	/** Primary click: place (Place mode) or gizmo drag / select (Select mode). Holding never places more. */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input")
	TObjectPtr<class UInputAction> PlaceAction = nullptr;

	/** Hold to navigate (right mouse button). Navigation only: it never selects, places or deletes. */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input")
	TObjectPtr<class UInputAction> NavigateAction = nullptr;

	/** Deletes the selected item (Delete key). */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input")
	TObjectPtr<class UInputAction> DeleteAction = nullptr;

	/** Backs out one level: leaves Place mode, or clears the selection and disarms the catalog item (Esc). */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input")
	TObjectPtr<class UInputAction> CancelAction = nullptr;

	/** Cycles the gizmo Move -> Rotate -> Scale (Space). */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input")
	TObjectPtr<class UInputAction> ToggleGizmoModeAction = nullptr;

	/** Toggles Place mode (P). */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input")
	TObjectPtr<class UInputAction> TogglePlaceModeAction = nullptr;

	/** Undo (Ctrl+Z). An assigned action must be mapped with its own Ctrl chord in EditorMappingContext. */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input")
	TObjectPtr<class UInputAction> UndoAction = nullptr;

	/** Redo (Ctrl+Y and Ctrl+Shift+Z). An assigned action must be mapped with its own chords in EditorMappingContext. */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input")
	TObjectPtr<class UInputAction> RedoAction = nullptr;

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input")
	int32 MappingContextPriority = 0;

	/**
	 * Applied for the whole session; the handlers act only while NavigateAction is held. Never
	 * added/removed per press, because rebuilding mappings while keys are held makes press/release
	 * pairing unreliable. Must map CameraLookAction, CameraMoveAction and CameraSpeedAction.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input|Camera")
	TObjectPtr<class UInputMappingContext> CameraNavigationMappingContext = nullptr;

	/** Axis2D look delta in raw mouse counts (X = yaw, Y = pitch). */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input|Camera")
	TObjectPtr<class UInputAction> CameraLookAction = nullptr;

	/** Axis3D fly input (X = view forward, Y = view right, Z = world up). */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input|Camera")
	TObjectPtr<class UInputAction> CameraMoveAction = nullptr;

	/** Axis1D fly speed steps (mouse wheel), like the Unreal Editor's camera speed. */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input|Camera")
	TObjectPtr<class UInputAction> CameraSpeedAction = nullptr;

	/** Above the editor context, so the fly keys win when both map the same key. */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input|Camera")
	int32 CameraNavigationContextPriority = 1;

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Trace")
	TEnumAsByte<ECollisionChannel> PlacementTraceChannel;

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Trace")
	TEnumAsByte<ECollisionChannel> StageItemTraceChannel;

private:
	void BuildDefaultInputMapping();
	void BuildDefaultCameraMapping();
	void MapUndoRedoChords(class UInputMappingContext& Context);

	UFUNCTION()
	void HandleSelectionChanged(AActor* NewSelection, AActor* PreviousSelection);

	UFUNCTION()
	void HandlePurchaseCompleted(class UStageProductData* Product, FStageEconomyResultInfo Result);

	UFUNCTION()
	void HandleEditModeChanged(EStageEditMode NewMode, EStageEditMode PreviousMode);

	UFUNCTION()
	void HandleArmedItemChanged(class UBaseItemData* ArmedItem);

	UFUNCTION()
	void HandleSceneOperationFinished(const FStageSceneOperationResult& Result);

	/** Reports a scene request that could not start; returns Result for the caller to pass on. */
	EStageSceneResult ReportSceneRefusal(EStageSceneOperation Operation, EStageSceneResult Result, const FString& Name);

	UFUNCTION()
	void HandlePossessedPawnChanged(APawn* PreviousPawn, APawn* NewPawn);

	FStageEconomyResultInfo ValidatePlacement(const class UBaseItemData& Item);
	FStageEconomyResultInfo EvaluatePlacementPreview(const class UBaseItemData& Item);
	void ReportRejection(const FStageEconomyResultInfo& Result);
	class AStageCraftGameModeBase* GetStageGameMode() const;
	class UStageEconomySubsystem* GetEconomy() const;

	// Enhanced Input handlers: read the cursor once and forward to the position-based handlers below.
	void HandlePrimaryStarted();
	void HandlePrimaryTriggered();
	void HandlePrimaryCompleted();
	void HandleNavigateStarted();
	void HandleNavigateCompleted();
	void HandleDelete();
	void HandleCancel();
	void HandleToggleGizmoMode();
	void HandleTogglePlaceMode();
	void HandleUndo();
	void HandleRedo();

	UFUNCTION()
	void HandleGizmoDragStarted(AActor* Target);

	UFUNCTION()
	void HandleGizmoDragFinished(AActor* Target, const FTransform& StartTransform);

	/** Shared by undo and redo: input guards, the step itself, and reporting a step that can never apply. */
	EStageCommandResult RequestHistoryStep(bool bUndo);

	/** Structural edits (delete, clear, undo) must not run under a gizmo drag or while the camera flies. */
	bool IsBusyWithPointer() const;

	/** Ctrl held (either key), read from Slate's modifier state so it is independent of Enhanced Input mappings. */
	static EStageSelectionClick GetSelectionClickFromKeyboard();

	void HandleCameraLook(const struct FInputActionValue& Value);
	void HandleCameraMove(const struct FInputActionValue& Value);
	void HandleCameraSpeed(const struct FInputActionValue& Value);

	// Position-based handlers, shared by real input and the dev simulation.
	void HandlePrimaryPressedAt(const FVector2D& ViewportPosition, EStageSelectionClick Click);
	void HandlePrimaryHeldAt(const FVector2D& ViewportPosition);
	void HandlePrimaryReleased();
	void SelectOrDeselectAt(const FVector2D& ViewportPosition, EStageSelectionClick Click);

	// Placement preview: event-driven, coalesced to one trace per frame.
	void BindPreviewRefreshSources();
	void UnbindPreviewRefreshSources();
	void BindCameraPawn(APawn* NewPawn);
	void HandleCursorMoved();
	void HandleCameraTransformUpdated(class USceneComponent* UpdatedComponent, EUpdateTransformFlags UpdateTransformFlags, ETeleportType Teleport);
	void RequestPreviewRefresh();
	void RefreshPlacementPreview();

	/** The fly camera, or null when a different pawn is possessed (navigation is then a no-op). */
	class AStageCameraPawn* GetCameraPawn() const;

	bool IsGizmoDragging() const;
	bool TryBeginGizmoDragAt(const FVector2D& ViewportPosition);
	bool GetCursorViewportPosition(FVector2D& OutPosition) const;
	bool GetRayAt(const FVector2D& ViewportPosition, FVector& OutOrigin, FVector& OutDirection) const;
	bool GetPlacementHitAt(const FVector2D& ViewportPosition, FHitResult& OutHit) const;
	bool GetStageItemHitAt(const FVector2D& ViewportPosition, FHitResult& OutHit) const;
	bool GetGizmoHitAt(const FVector2D& ViewportPosition, FHitResult& OutHit) const;

	UPROPERTY(Transient)
	TObjectPtr<class AModularTransformGizmo> Gizmo = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UUserWidget> HUDWidget = nullptr;

	TWeakObjectPtr<class UStageCraftGameViewportClient> BoundViewportClient;
	TWeakObjectPtr<class USceneComponent> BoundCameraRoot;
	FDelegateHandle CursorMovedHandle;
	FDelegateHandle CameraTransformHandle;

	bool bIsNavigatingCamera = false;
	bool bPreviewRefreshQueued = false;

#if !UE_BUILD_SHIPPING
	TOptional<FVector2D> SimulatedCursor;
#endif
};
