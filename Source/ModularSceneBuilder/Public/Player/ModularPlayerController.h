// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "Data/StageParameterTypes.h"
#include "Economy/StageEconomyTypes.h"
#include "Environment/StageEnvironmentTypes.h"
#include "History/StageEditCommand.h"
#include "Input/StageKeyBindings.h"
#include "Placement/StagePlacementTypes.h"
#include "Scene/StageSceneTypes.h"
#include "Show/ShowControlTypes.h"
#include "Sketch/StageSketchTypes.h"
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
 *   UStageSnappingComponent (object snapping for placement and gizmo moves),
 *   UStageSpanToolComponent (two-click placement of spans: truss runs, railings, cables).
 *
 * In Place mode with a span armed, clicks, cursor moves and the first Esc go to the span tool instead of the point placement tool.
 *
 * A gizmo drag or numeric transform edit is checked by the GameMode for every item it moves (the target and the rest of the
 * selection), so a locked item, or a span being scaled, refuses the whole edit instead of being left behind.
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
 * Keys are the user's key map (UStageInputPreferencesSubsystem, Docs/ADR/0013-preferences-key-bindings-and-audit-fixes.md §2.3).
 * InputKey looks up each key press (exact chord: key plus Ctrl/Shift/Alt) and runs that command: undo, redo, delete, select
 * all, cancel, the transform tools, Place mode, finishing or shortening a span path, the view projections and the grid.
 * Commands are suspended while the right button is held, so the same key can fly (W) and pick a tool (W) as in the Unreal
 * Editor. The fly keys are an Enhanced Input context rebuilt from the key map when it changes. Text fields keep their keys
 * while they have focus.
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
 * The mouse actions (select/place, navigate) and the camera actions are designer-assignable in a Blueprint subclass; empty
 * slots are built in code, so the project works with no content. The fly key context is always built from the key map.
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

	/** The AI assistant's voice prompts (Docs/ADR/0017 §2.5): MIC in the assistant panel records and transcribes through it. */
	class UStageVoiceInputComponent* GetVoiceInput() const { return VoiceInput; }

	UFUNCTION(BlueprintPure, Category = "StageCraft")
	class UStageSnappingComponent* GetSnapping() const { return Snapping; }

	UFUNCTION(BlueprintPure, Category = "StageCraft")
	class UStageGroupTransformComponent* GetGroupTransform() const { return GroupTransform; }

	UFUNCTION(BlueprintPure, Category = "StageCraft")
	class UStageSceneComponent* GetSceneFiles() const { return SceneFiles; }

	UFUNCTION(BlueprintPure, Category = "StageCraft")
	class UStageSpanToolComponent* GetSpanTool() const { return SpanTool; }

	UFUNCTION(BlueprintPure, Category = "StageCraft")
	class UStageViewportComponent* GetViewportView() const { return ViewportView; }

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
	 * Starts one continuous edit (a slider drag in the inspector): every RequestParameterChange until EndEditTransaction is
	 * still validated and applied at once, but together they become ONE undo step. Returns false if an edit is already open;
	 * only the caller that got true ends it. Callers end it when the interaction ends or their view goes away.
	 */
	bool BeginEditTransaction();

	/** Ends the continuous edit started by BeginEditTransaction and records it. No-op when none is open. */
	void EndEditTransaction();

	/**
	 * Deletes every selected item as one undoable step (Delete key, Edit menu). Ignored during a gizmo drag or camera
	 * navigation. Returns how many items were deleted.
	 */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Requests")
	int32 RequestDeleteSelection();

	/** Selects every visible placed item (Edit > Select All). Leaves Place mode first, like any selection. Same guards as delete. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Requests")
	void RequestSelectAll();

	/**
	 * Selects Items as a group (outliner rows and discipline headers). Replace: exactly these. Toggle (Ctrl): removes them when all are
	 * already selected, otherwise adds them. Hidden items can be selected here, as in the Unreal Editor's outliner, so they can be found
	 * and shown again. Leaves Place mode first; same guards as delete.
	 */
	void RequestSelectItems(const TArray<class AModularBaseActor*>& Items, EStageSelectionClick Click);

	/**
	 * Exports the bill of materials as PDF and CSV to Saved/StageCraft/Reports (Bill of Materials panel, File menu). Returns true when the
	 * export started; the result arrives on UStageBillOfMaterialsSubsystem::OnExportFinished. A refusal (empty stage, already
	 * exporting) is reported on OnRequestRejected.
	 */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Requests")
	bool RequestExportBillOfMaterials();

	/**
	 * Exports the DMX patch sheet as PDF and CSV to Saved/StageCraft/Reports (Patch panel, File menu;
	 * Docs/ADR/0011-cue-list-and-patch-sheet-export.md §3). Returns true when the export started; the result arrives on
	 * UStagePatchSheetSubsystem::OnExportFinished. A refusal (no show control, already exporting) is reported on OnRequestRejected.
	 */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Requests")
	bool RequestExportPatchSheet();

	/**
	 * Exports the cable schedule and power distribution plan as PDF and CSV to Saved/StageCraft/Reports (File menu;
	 * Docs/ADR/0012-production-reports-and-fixture-benchmark.md §2.3). Returns true when the export started; the result arrives on
	 * UStageCableScheduleSubsystem::OnExportFinished. A refusal (nothing to plan, already exporting) is reported on OnRequestRejected.
	 */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Requests")
	bool RequestExportCableSchedule();

	/**
	 * Builds the Sketch window's drawing as decks, truss runs and fixtures (Docs/ADR/0015-sketch-to-stage.md §2.5). Returns true when
	 * the request started: the plan's items stream in, then every element is placed through the GameMode rules, all or nothing,
	 * and recorded as ONE undo step. The result arrives on UStageSketchSubsystem::OnGenerationFinished. Refused (and reported on
	 * OnRequestRejected) during a gizmo drag or camera flight, while a generation is in flight, or when the sketch builds nothing.
	 */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Requests")
	bool RequestGenerateFromSketch();

	/**
	 * Places a build plan (decks, truss paths, fixtures) as ONE undo step named StepName: every element through the GameMode rules, all
	 * or nothing (StageSketchBuilder, Docs/ADR/0016-ai-assistant.md §2.4). The plan's items should be in memory already. Used by
	 * the Sketch window and the AI assistant. A refusal (gizmo drag or camera flight, empty plan, a rule) is reported on
	 * OnRequestRejected; the result says what was placed (Message: the plan's description) or why nothing was (Message: the reason).
	 */
	FStageSketchGenerateResult RequestBuildPlan(const FStageSketchBuildPlan& Plan, const FText& StepName);

	/**
	 * Replaces the outdoor environment (time of day, sun path, weather, sky on/off) and records the change as ONE undo step named
	 * StepName (Docs/ADR/0016-ai-assistant.md §2.4). The environment sanitizes the values and refuses the sky without a sky system,
	 * so callers read the stored settings back. False, reported on OnRequestRejected, when the level has no environment.
	 */
	bool RequestSetEnvironment(const FStageEnvironmentSettings& Settings, const FText& StepName);

	/**
	 * The AI Assistant panel's single entry point (Docs/ADR/0016-ai-assistant.md §2.2): hands Prompt to UStageAICommandSubsystem,
	 * whose action commands run back through this controller's requests, so they obey the same rules and undo history as the
	 * buttons. Replies arrive on the subsystem's OnConversationChanged. False, reported on OnRequestRejected, when there is no
	 * assistant, the prompt is empty, or the assistant is still answering the previous one.
	 */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Requests")
	bool RequestAssistantPrompt(const FString& Prompt);

	/**
	 * Runs Work and returns every refusal it reported, in order (they are still broadcast on OnRequestRejected). Lets the assistant
	 * quote the reason a request it made was refused. Not re-entrant: a nested call returns its own refusals only.
	 */
	TArray<FStageEconomyResultInfo> RunCapturingRejections(TFunctionRef<void()> Work);

	// --- Cues (Docs/ADR/0011-cue-list-and-patch-sheet-export.md §2.2) ---
	// Each asks AStageCraftGameModeBase::EvaluateCueRequest, then calls UShowControlSubsystem. Store, Update and Delete are
	// recorded as one undo step each. A refusal is reported on OnRequestRejected and returns false.

	/** Records the current look as cue CueNumber (re-records it if it exists). */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Requests")
	bool RequestStoreCue(float CueNumber, const FText& Label, float FadeTime);

	/** Changes an existing cue's label and fade time. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Requests")
	bool RequestUpdateCue(float CueNumber, const FText& Label, float FadeTime);

	UFUNCTION(BlueprintCallable, Category = "StageCraft|Requests")
	bool RequestDeleteCue(float CueNumber);

	UFUNCTION(BlueprintCallable, Category = "StageCraft|Requests")
	bool RequestGoToCue(float CueNumber);

	/** Plays the next cue. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Requests")
	bool RequestCueGo();

	/** Plays the previous cue. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Requests")
	bool RequestCueBack();

	UFUNCTION(BlueprintCallable, Category = "StageCraft|Requests")
	bool RequestSetControlSource(EStageControlSource Source);

	/**
	 * Imports model files into the Library (files dropped on a panel, Import Model...). The GameMode decides whether imports are
	 * allowed; files that cannot be imported are reported on OnRequestRejected. Accepted files import in the background and report
	 * through UStageMeshImportSubsystem::OnImportFinished. Returns how many files were queued.
	 */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Requests")
	int32 RequestImportModelFiles(const TArray<FString>& Files);

	/**
	 * Lets the user pick model files in the operating system's file dialog, then imports them as RequestImportModelFiles does.
	 * The dialog is modal. Returns how many files were queued (0 when cancelled or when this build has no file dialog).
	 */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Requests")
	int32 RequestBrowseModelFiles();

	/** True when this build can show the file dialog of RequestBrowseModelFiles (builds with developer tools). */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Requests")
	static bool CanBrowseModelFiles();

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

	/**
	 * Replaces the stage with the autosave Name (File > Recover Autosave), for after a crash. The stage keeps its scene name and
	 * stays marked as unsaved until the user saves it. Same guards and confirmation duty as RequestLoadScene.
	 */
	EStageSceneResult RequestRecoverAutosave(const FString& Name);

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
	virtual bool InputKey(const FInputKeyEventArgs& Params) override;
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

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<class UStageSpanToolComponent> SpanTool = nullptr;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<class UStageViewportComponent> ViewportView = nullptr;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<class UStageVoiceInputComponent> VoiceInput = nullptr;

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

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input")
	int32 MappingContextPriority = 0;

	/**
	 * Look, wheel and the fly keys from the key map. Applied for the whole session; the handlers act only while NavigateAction
	 * is held. Rebuilt only when the fly keys change in the preferences, never per press, because rebuilding mappings while
	 * keys are held makes press/release pairing unreliable.
	 */
	UPROPERTY(Transient)
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

	/** Above the editor context (which maps only the mouse buttons). */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input|Camera")
	int32 CameraNavigationContextPriority = 1;

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Trace")
	TEnumAsByte<ECollisionChannel> PlacementTraceChannel;

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Trace")
	TEnumAsByte<ECollisionChannel> StageItemTraceChannel;

private:
	void BuildDefaultInputMapping();
	void EnsureCameraActions();

	/** Builds the camera context from the key map's fly keys; when it replaces one in use, swaps it in the input subsystem. */
	void RebuildCameraMapping();

	/** The local user's key map and navigation preferences; null before the local player exists. */
	class UStageInputPreferencesSubsystem* GetInputPreferences() const;

	/** The key map or the fly keys changed: rebuild the fly mapping. Commands read the map on each press, so need nothing. */
	UFUNCTION()
	void HandleInputPreferencesChanged();

	/** Runs one key-map command. Each command keeps its own guards (gizmo drag, span tool state). */
	void ExecuteCommand(EStageInputCommand Command);

	/** Ctrl, Shift or Alt (either side) is down for the event from Viewport (or, without one, in this controller's input state). */
	bool IsModifierDown(const class FViewport* Viewport, const FKey& Left, const FKey& Right) const;

	/** The preferences this controller is bound to, for unbinding in EndPlay. */
	TWeakObjectPtr<class UStageInputPreferencesSubsystem> BoundInputPreferences;

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

	/** Pushes the toolbar's grid and angle snap to the gizmo and the placement tool (which the span tool reads). */
	UFUNCTION()
	void ApplySnapSettings();

	/** Projection or zoom changed: the cursor now points elsewhere, so the placement preview re-traces. */
	UFUNCTION()
	void HandleViewChanged();

	FStageEconomyResultInfo ValidatePlacement(const class UBaseItemData& Item, int32 Quantity);
	FStageEconomyResultInfo EvaluatePlacementPreview(const class UBaseItemData& Item, int32 Quantity);
	FStageEconomyResultInfo EvaluatePlacementRules(const class UBaseItemData* Item, int32 Quantity) const;

	/**
	 * Asks the GameMode whether every selected item may take a TransformParameter edit (Location, Rotation or Scale), as a gizmo
	 * drag or group numeric edit would apply to all of them. The first refusal is returned; Ok when nothing is selected.
	 */
	FStageEconomyResultInfo EvaluateSelectionTransform(const FGameplayTag& TransformParameter) const;

	/** The inspector label of ParameterId on Target ("Length"), for naming history steps; the tag name if Target does not list it. */
	static FText FindParameterName(const UObject& Target, const FGameplayTag& ParameterId);

	/** Undo / redo of a recorded parameter step: validated by the GameMode and applied by the session, never recorded again. */
	EStageCommandResult ApplyHistoryParameter(class AModularBaseActor& Item, const FGameplayTag& ParameterId, const FStageParameterValue& Value);

	/** Undo / redo of a recorded transform, per item: the GameMode's EvaluateTransformChange decides; a refusal is reported. */
	bool ValidateHistoryTransform(const class AModularBaseActor& Item, const FTransform& Transform);

	/** The world's show control when the GameMode allows Request on CueNumber; null (refusal reported) otherwise. */
	class UShowControlSubsystem* ApproveCueRequest(EStageCueRequest Request, float CueNumber);

	/** The sketch plan's items are in memory: places the plan (all or nothing), records it as one step, reports the result. */
	void HandleSketchAssetsReady();

	/** Records the cue list change from Before to the current list as one undo step named Description. */
	void RecordCueEdit(const FText& Description, const TArray<FShowCue>& Before, const class UShowControlSubsystem& Show) const;

	void ReportRejection(const FStageEconomyResultInfo& Result);
	class AStageCraftGameModeBase* GetStageGameMode() const;
	class UStageEconomySubsystem* GetEconomy() const;

	// Enhanced Input handlers: read the cursor once and forward to the position-based handlers below.
	void HandlePrimaryStarted();
	void HandlePrimaryTriggered();
	void HandlePrimaryCompleted();
	void HandleNavigateStarted();
	void HandleNavigateCompleted();

	// Key-map commands (ExecuteCommand).
	void HandleCancel();
	void HandleTogglePlaceMode();
	void HandleFinishPath();
	void HandleRemovePathPoint();

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

	/** While RunCapturingRejections runs: where ReportRejection also copies each refusal. */
	TArray<FStageEconomyResultInfo>* RejectionSink = nullptr;

	/** Folder the import dialog opens in: where the user last picked models, this session. */
	FString LastImportDirectory;

#if !UE_BUILD_SHIPPING
	TOptional<FVector2D> SimulatedCursor;
#endif
};
