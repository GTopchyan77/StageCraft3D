// Copyright Epic Games, Inc. All Rights Reserved.

#include "Player/ModularPlayerController.h"

#include "Actors/ModularBaseActor.h"
#include "Actors/ModularTransformGizmo.h"
#include "Algo/AllOf.h"
#include "Audio/StageEditorAudioFeedbackComponent.h"
#include "Blueprint/UserWidget.h"
#include "Components/SceneComponent.h"
#include "Components/SelectionComponent.h"
#include "Components/SpawnSystemComponent.h"
#include "Components/StageGroupTransformComponent.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Framework/Application/SlateApplication.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Economy/StageProductData.h"
#include "Game/StageCraftGameModeBase.h"
#include "Interaction/StageParameterInterface.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputTriggers.h"
#include "Misc/Paths.h"
#include "History/StageEditHistoryComponent.h"
#include "Import/StageFileDialog.h"
#include "Import/StageMeshImportSubsystem.h"
#include "Interaction/StageCraftCollision.h"
#include "ModularSceneBuilder.h"
#include "Placement/StagePlacementToolComponent.h"
#include "Placement/StageSnappingComponent.h"
#include "Placement/StageSpanToolComponent.h"
#include "Player/StageCameraPawn.h"
#include "Player/StageCraftGameViewportClient.h"
#include "Render/StageRenderSubsystem.h"
#include "Report/StageBillOfMaterialsSubsystem.h"
#include "Report/StagePatchSheetSubsystem.h"
#include "Scene/StageSceneComponent.h"
#include "Subsystems/ShowControlSubsystem.h"
#include "Subsystems/StageEconomySubsystem.h"
#include "Subsystems/StageItemSubsystem.h"
#include "Subsystems/StageSessionSubsystem.h"
#include "TimerManager.h"
#include "Viewport/StageViewportComponent.h"
#include "Workspace/StageWorkspaceSubsystem.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(ModularPlayerController)

namespace ModularPlayerController
{
	void MapAxisKey(UInputMappingContext* Context, const UInputAction* Action, const FKey& Key, TOptional<EInputAxisSwizzle> Swizzle = {}, bool bNegate = false)
	{
		FEnhancedActionKeyMapping& Mapping = Context->MapKey(Action, Key);
		if (Swizzle.IsSet())
		{
			UInputModifierSwizzleAxis* SwizzleModifier = NewObject<UInputModifierSwizzleAxis>(Context);
			SwizzleModifier->Order = Swizzle.GetValue();
			Mapping.Modifiers.Add(SwizzleModifier);
		}
		if (bNegate)
		{
			Mapping.Modifiers.Add(NewObject<UInputModifierNegate>(Context));
		}
	}
}

AModularPlayerController::AModularPlayerController()
	: PlacementTraceChannel(ECC_Visibility)
	, StageItemTraceChannel(StageCraftCollision::StageItemChannel)
{
	bShowMouseCursor = true;
	DefaultMouseCursor = EMouseCursor::Crosshairs;

	SpawnSystem = CreateDefaultSubobject<USpawnSystemComponent>(TEXT("SpawnSystem"));
	Selection = CreateDefaultSubobject<USelectionComponent>(TEXT("Selection"));
	PlacementTool = CreateDefaultSubobject<UStagePlacementToolComponent>(TEXT("PlacementTool"));
	AudioFeedback = CreateDefaultSubobject<UStageEditorAudioFeedbackComponent>(TEXT("AudioFeedback"));
	EditHistory = CreateDefaultSubobject<UStageEditHistoryComponent>(TEXT("EditHistory"));
	Snapping = CreateDefaultSubobject<UStageSnappingComponent>(TEXT("Snapping"));
	GroupTransform = CreateDefaultSubobject<UStageGroupTransformComponent>(TEXT("GroupTransform"));
	SceneFiles = CreateDefaultSubobject<UStageSceneComponent>(TEXT("SceneFiles"));
	SpanTool = CreateDefaultSubobject<UStageSpanToolComponent>(TEXT("SpanTool"));
	ViewportView = CreateDefaultSubobject<UStageViewportComponent>(TEXT("ViewportView"));
	GizmoClass = AModularTransformGizmo::StaticClass();
}

void AModularPlayerController::BeginPlay()
{
	Super::BeginPlay();

	if (!IsLocalController())
	{
		return;
	}

	// The only input mode this controller ever uses. RMB cursor capture is handled below Slate's input-mode
	// level by UStageCraftGameViewportClient, so nothing here changes mode while a button is held.
	FInputModeGameAndUI InputMode;
	InputMode.SetHideCursorDuringCapture(false);
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	SetInputMode(InputMode);

	if (!Cast<UStageCraftGameViewportClient>(GetWorld()->GetGameViewport()))
	{
		UE_LOG(LogStageCraft, Warning, TEXT("%s: GameViewportClientClassName is not StageCraftGameViewportClient; RMB navigation will turn the view but cannot hide or capture the cursor, and the placement preview only follows camera moves."), *GetName());
	}

	// Engine look input stays off: the view is owned by AStageCameraPawn, which sets the control rotation itself.
	SetIgnoreLookInput(true);

	if (GizmoClass)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.Owner = this; // The gizmo reads the owner's camera to keep a constant screen size.
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Gizmo = GetWorld()->SpawnActor<AModularTransformGizmo>(GizmoClass, FTransform::Identity, SpawnParams);
	}

	if (Gizmo)
	{
		// Drags snap onto neighbours and become one undo step each when they end.
		Gizmo->TranslationSnapper.BindUObject(Snapping.Get(), &UStageSnappingComponent::SnapMove);
		Gizmo->OnDragStarted.AddUniqueDynamic(this, &ThisClass::HandleGizmoDragStarted);
		Gizmo->OnDragFinished.AddUniqueDynamic(this, &ThisClass::HandleGizmoDragFinished);
	}

	Selection->OnSelectionChanged.AddUniqueDynamic(this, &ThisClass::HandleSelectionChanged);

	// Every spawn is decided by the GameMode through this controller; the preview colour asks the same rules without reporting.
	SpawnSystem->PlacementValidator.BindUObject(this, &ThisClass::ValidatePlacement);
	PlacementTool->PreviewEvaluator.BindUObject(this, &ThisClass::EvaluatePlacementPreview);
	PlacementTool->PlacementSnapper.BindUObject(Snapping.Get(), &UStageSnappingComponent::SnapPlacement);
	PlacementTool->OnEditModeChanged.AddUniqueDynamic(this, &ThisClass::HandleEditModeChanged);
	PlacementTool->OnArmedItemChanged.AddUniqueDynamic(this, &ThisClass::HandleArmedItemChanged);
	SpanTool->PreviewEvaluator.BindUObject(this, &ThisClass::EvaluatePlacementPreview);
	BindPreviewRefreshSources();

	// The toolbar's global grid and angle snap reach the tools from here, now and on every change (ADR 0008 §4.5).
	Snapping->OnSnapSettingsChanged.AddUniqueDynamic(this, &ThisClass::ApplySnapSettings);
	ApplySnapSettings();
	ViewportView->OnViewChanged.AddUniqueDynamic(this, &ThisClass::HandleViewChanged);

	// Undo / redo write lengths and transforms back through the same rules as the inspector: a locked item stays put.
	EditHistory->ParameterApplier.BindUObject(this, &ThisClass::ApplyHistoryParameter);
	EditHistory->TransformValidator.BindUObject(this, &ThisClass::ValidateHistoryTransform);

	// Loading a scene asks the same rules per item, silently, and reports one summary instead of a refusal per item.
	SceneFiles->PlacementEvaluator.BindUObject(this, &ThisClass::EvaluatePlacementPreview);
	SceneFiles->OnSceneOperationFinished.AddUniqueDynamic(this, &ThisClass::HandleSceneOperationFinished);

	if (UStageEconomySubsystem* Economy = GetEconomy())
	{
		Economy->OnPurchaseCompleted.AddUniqueDynamic(this, &ThisClass::HandlePurchaseCompleted);
	}

	// Both happen after the selection binding, so panels that read the current selection in NativeConstruct see a ready controller.
	// In the dockable workspace (Standalone/packaged) the panels live in dock tabs. The fixed HUD holds only those same panels today,
	// so it is not shown there. It returns as the viewport-overlay layer in Phase 1.
	UStageWorkspaceSubsystem* Workspace = GetGameInstance() ? GetGameInstance()->GetSubsystem<UStageWorkspaceSubsystem>() : nullptr;
	if (Workspace)
	{
		Workspace->RegisterLocalController(this);
	}
	if (HUDWidgetClass && !(Workspace && Workspace->IsWorkspaceActive()))
	{
		HUDWidget = CreateWidget<UUserWidget>(this, HUDWidgetClass);
		if (HUDWidget)
		{
			HUDWidget->AddToViewport();
		}
	}
}

void AModularPlayerController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	bIsNavigatingCamera = false;

	UnbindPreviewRefreshSources();
	GetWorldTimerManager().ClearAllTimersForObject(this);
	bPreviewRefreshQueued = false;

	Selection->OnSelectionChanged.RemoveDynamic(this, &ThisClass::HandleSelectionChanged);
	SpawnSystem->PlacementValidator.Unbind();
	PlacementTool->PreviewEvaluator.Unbind();
	PlacementTool->PlacementSnapper.Unbind();
	PlacementTool->OnEditModeChanged.RemoveDynamic(this, &ThisClass::HandleEditModeChanged);
	PlacementTool->OnArmedItemChanged.RemoveDynamic(this, &ThisClass::HandleArmedItemChanged);
	SpanTool->PreviewEvaluator.Unbind();
	Snapping->OnSnapSettingsChanged.RemoveDynamic(this, &ThisClass::ApplySnapSettings);
	ViewportView->OnViewChanged.RemoveDynamic(this, &ThisClass::HandleViewChanged);
	EditHistory->ParameterApplier.Unbind();
	EditHistory->TransformValidator.Unbind();
	SceneFiles->PlacementEvaluator.Unbind();
	SceneFiles->OnSceneOperationFinished.RemoveDynamic(this, &ThisClass::HandleSceneOperationFinished);
	if (UStageEconomySubsystem* Economy = GetEconomy())
	{
		Economy->OnPurchaseCompleted.RemoveDynamic(this, &ThisClass::HandlePurchaseCompleted);
	}

	if (UStageWorkspaceSubsystem* Workspace = GetGameInstance() ? GetGameInstance()->GetSubsystem<UStageWorkspaceSubsystem>() : nullptr)
	{
		Workspace->UnregisterLocalController(this);
	}

	if (HUDWidget)
	{
		HUDWidget->RemoveFromParent();
		HUDWidget = nullptr;
	}

	if (Gizmo)
	{
		Gizmo->TranslationSnapper.Unbind();
		Gizmo->OnDragStarted.RemoveDynamic(this, &ThisClass::HandleGizmoDragStarted);
		Gizmo->OnDragFinished.RemoveDynamic(this, &ThisClass::HandleGizmoDragFinished);
		Gizmo->Destroy();
		Gizmo = nullptr;
	}

	Super::EndPlay(EndPlayReason);
}

void AModularPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	if (!EditorMappingContext || !PlaceAction || !NavigateAction || !DeleteAction || !CancelAction || !ToggleGizmoModeAction || !TogglePlaceModeAction
		|| !FinishPathAction || !RemovePathPointAction || !UndoAction || !RedoAction)
	{
		BuildDefaultInputMapping();
	}

	if (!CameraNavigationMappingContext || !CameraLookAction || !CameraMoveAction || !CameraSpeedAction)
	{
		BuildDefaultCameraMapping();
	}

	if (UEnhancedInputLocalPlayerSubsystem* InputSubsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()))
	{
		InputSubsystem->AddMappingContext(EditorMappingContext, MappingContextPriority);
		InputSubsystem->AddMappingContext(CameraNavigationMappingContext, CameraNavigationContextPriority);
	}

	UEnhancedInputComponent* EnhancedInput = Cast<UEnhancedInputComponent>(InputComponent);
	if (!EnhancedInput)
	{
		UE_LOG(LogStageCraft, Error, TEXT("%s requires UEnhancedInputComponent as the default input component class."), *GetName());
		return;
	}

	// With no explicit trigger, Started fires on press, Triggered every frame while held, Completed on release.
	// Only Started can place; Triggered only continues a gizmo drag.
	EnhancedInput->BindAction(PlaceAction, ETriggerEvent::Started, this, &ThisClass::HandlePrimaryStarted);
	EnhancedInput->BindAction(PlaceAction, ETriggerEvent::Triggered, this, &ThisClass::HandlePrimaryTriggered);
	EnhancedInput->BindAction(PlaceAction, ETriggerEvent::Completed, this, &ThisClass::HandlePrimaryCompleted);
	EnhancedInput->BindAction(PlaceAction, ETriggerEvent::Canceled, this, &ThisClass::HandlePrimaryCompleted);
	EnhancedInput->BindAction(NavigateAction, ETriggerEvent::Started, this, &ThisClass::HandleNavigateStarted);
	EnhancedInput->BindAction(NavigateAction, ETriggerEvent::Completed, this, &ThisClass::HandleNavigateCompleted);
	EnhancedInput->BindAction(NavigateAction, ETriggerEvent::Canceled, this, &ThisClass::HandleNavigateCompleted);
	EnhancedInput->BindAction(DeleteAction, ETriggerEvent::Started, this, &ThisClass::HandleDelete);
	EnhancedInput->BindAction(CancelAction, ETriggerEvent::Started, this, &ThisClass::HandleCancel);
	EnhancedInput->BindAction(ToggleGizmoModeAction, ETriggerEvent::Started, this, &ThisClass::HandleToggleGizmoMode);
	EnhancedInput->BindAction(TogglePlaceModeAction, ETriggerEvent::Started, this, &ThisClass::HandleTogglePlaceMode);
	EnhancedInput->BindAction(FinishPathAction, ETriggerEvent::Started, this, &ThisClass::HandleFinishPath);
	EnhancedInput->BindAction(RemovePathPointAction, ETriggerEvent::Started, this, &ThisClass::HandleRemovePathPoint);
	EnhancedInput->BindAction(UndoAction, ETriggerEvent::Started, this, &ThisClass::HandleUndo);
	EnhancedInput->BindAction(RedoAction, ETriggerEvent::Started, this, &ThisClass::HandleRedo);

	// Always mapped. Look and move act only while NavigateAction is held; speed (wheel) acts any time.
	EnhancedInput->BindAction(CameraLookAction, ETriggerEvent::Triggered, this, &ThisClass::HandleCameraLook);
	EnhancedInput->BindAction(CameraMoveAction, ETriggerEvent::Triggered, this, &ThisClass::HandleCameraMove);
	EnhancedInput->BindAction(CameraSpeedAction, ETriggerEvent::Triggered, this, &ThisClass::HandleCameraSpeed);
}

void AModularPlayerController::BuildDefaultInputMapping()
{
	UE_LOG(LogStageCraft, Log, TEXT("%s: input assets not fully assigned, using built-in LMB/RMB/Delete/Esc/Space/P mapping."), *GetName());

	if (!PlaceAction)
	{
		PlaceAction = NewObject<UInputAction>(this, TEXT("IA_Place_Default"));
	}
	if (!NavigateAction)
	{
		NavigateAction = NewObject<UInputAction>(this, TEXT("IA_Navigate_Default"));
	}
	if (!DeleteAction)
	{
		DeleteAction = NewObject<UInputAction>(this, TEXT("IA_Delete_Default"));
	}
	if (!CancelAction)
	{
		CancelAction = NewObject<UInputAction>(this, TEXT("IA_Cancel_Default"));
	}
	if (!ToggleGizmoModeAction)
	{
		ToggleGizmoModeAction = NewObject<UInputAction>(this, TEXT("IA_ToggleGizmoMode_Default"));
	}
	if (!TogglePlaceModeAction)
	{
		TogglePlaceModeAction = NewObject<UInputAction>(this, TEXT("IA_TogglePlaceMode_Default"));
	}
	if (!FinishPathAction)
	{
		FinishPathAction = NewObject<UInputAction>(this, TEXT("IA_FinishPath_Default"));
	}
	if (!RemovePathPointAction)
	{
		RemovePathPointAction = NewObject<UInputAction>(this, TEXT("IA_RemovePathPoint_Default"));
	}
	if (!UndoAction)
	{
		UndoAction = NewObject<UInputAction>(this, TEXT("IA_Undo_Default"));
	}
	if (!RedoAction)
	{
		RedoAction = NewObject<UInputAction>(this, TEXT("IA_Redo_Default"));
	}

	// A fresh context, because an assigned one cannot be trusted to map actions it was not authored with.
	// P, Z, Y, Enter and Backspace are free in the camera context (which maps W/A/S/D/Q/E), so they are never consumed by navigation.
	EditorMappingContext = NewObject<UInputMappingContext>(this, TEXT("IMC_StageEditor_Default"));
	MapUndoRedoChords(*EditorMappingContext);
	EditorMappingContext->MapKey(PlaceAction, EKeys::LeftMouseButton);
	EditorMappingContext->MapKey(NavigateAction, EKeys::RightMouseButton);
	EditorMappingContext->MapKey(DeleteAction, EKeys::Delete);
	EditorMappingContext->MapKey(CancelAction, EKeys::Escape);
	EditorMappingContext->MapKey(ToggleGizmoModeAction, EKeys::SpaceBar);
	EditorMappingContext->MapKey(TogglePlaceModeAction, EKeys::P);
	EditorMappingContext->MapKey(FinishPathAction, EKeys::Enter);
	EditorMappingContext->MapKey(RemovePathPointAction, EKeys::BackSpace);
}

void AModularPlayerController::MapUndoRedoChords(UInputMappingContext& Context)
{
	// Modifier keys are actions of their own, used only as chords. They come first so their state is
	// evaluated before the chorded keys in the same frame.
	UInputAction* ControlChord = NewObject<UInputAction>(&Context, TEXT("IA_ModifierControl_Default"));
	UInputAction* ShiftChord = NewObject<UInputAction>(&Context, TEXT("IA_ModifierShift_Default"));
	Context.MapKey(ControlChord, EKeys::LeftControl);
	Context.MapKey(ControlChord, EKeys::RightControl);
	Context.MapKey(ShiftChord, EKeys::LeftShift);
	Context.MapKey(ShiftChord, EKeys::RightShift);

	const auto AddChord = [&Context](FEnhancedActionKeyMapping& Mapping, const UInputAction* Chord)
	{
		UInputTriggerChordAction* Trigger = NewObject<UInputTriggerChordAction>(&Context);
		Trigger->ChordAction = Chord;
		Mapping.Triggers.Add(Trigger);
	};

	// Ctrl+Shift+Z is mapped before Ctrl+Z on purpose: Enhanced Input then blocks the later Z mapping
	// while the earlier chorded one triggers (IEnhancedInputSubsystemInterface::InjectChordBlockers),
	// so Ctrl+Shift+Z redoes without also undoing.
	FEnhancedActionKeyMapping& RedoShiftZ = Context.MapKey(RedoAction, EKeys::Z);
	AddChord(RedoShiftZ, ControlChord);
	AddChord(RedoShiftZ, ShiftChord);
	AddChord(Context.MapKey(UndoAction, EKeys::Z), ControlChord);
	AddChord(Context.MapKey(RedoAction, EKeys::Y), ControlChord);
}

void AModularPlayerController::BuildDefaultCameraMapping()
{
	using namespace ModularPlayerController;

	UE_LOG(LogStageCraft, Log, TEXT("%s: camera input assets not fully assigned, using built-in RMB fly mapping (mouse look, WASD, Q/E, wheel)."), *GetName());

	if (!CameraLookAction)
	{
		CameraLookAction = NewObject<UInputAction>(this, TEXT("IA_CameraLook_Default"));
		CameraLookAction->ValueType = EInputActionValueType::Axis2D;
	}
	if (!CameraMoveAction)
	{
		CameraMoveAction = NewObject<UInputAction>(this, TEXT("IA_CameraMove_Default"));
		CameraMoveAction->ValueType = EInputActionValueType::Axis3D;
	}
	if (!CameraSpeedAction)
	{
		CameraSpeedAction = NewObject<UInputAction>(this, TEXT("IA_CameraSpeed_Default"));
		CameraSpeedAction->ValueType = EInputActionValueType::Axis1D;
	}

	CameraNavigationMappingContext = NewObject<UInputMappingContext>(this, TEXT("IMC_CameraNavigation_Default"));
	UInputMappingContext* Context = CameraNavigationMappingContext;

	Context->MapKey(CameraLookAction, EKeys::Mouse2D);
	Context->MapKey(CameraSpeedAction, EKeys::MouseWheelAxis);

	// A digital key yields (1,0,0); swizzle routes it onto Y (right) or Z (up), negate flips the direction.
	// Space/Ctrl are not mapped: Space cycles the gizmo and Ctrl is reserved for multi-select.
	MapAxisKey(Context, CameraMoveAction, EKeys::W);
	MapAxisKey(Context, CameraMoveAction, EKeys::S, {}, /*bNegate*/ true);
	MapAxisKey(Context, CameraMoveAction, EKeys::D, EInputAxisSwizzle::YXZ);
	MapAxisKey(Context, CameraMoveAction, EKeys::A, EInputAxisSwizzle::YXZ, /*bNegate*/ true);
	MapAxisKey(Context, CameraMoveAction, EKeys::E, EInputAxisSwizzle::ZYX);
	MapAxisKey(Context, CameraMoveAction, EKeys::Q, EInputAxisSwizzle::ZYX, /*bNegate*/ true);
}

void AModularPlayerController::HandleSelectionChanged(AActor* NewSelection, AActor* PreviousSelection)
{
	if (Gizmo)
	{
		Gizmo->AttachToTarget(NewSelection);
	}
}

void AModularPlayerController::HandleEditModeChanged(EStageEditMode NewMode, EStageEditMode PreviousMode)
{
	if (NewMode == EStageEditMode::Place)
	{
		// Placing and transforming are separate tools: the gizmo hides while the ghost is out.
		Selection->ClearSelection();
		RequestPreviewRefresh();
	}
}

void AModularPlayerController::HandleArmedItemChanged(UBaseItemData* ArmedItem)
{
	RequestPreviewRefresh();
}

// --- Traces ---

bool AModularPlayerController::GetCursorViewportPosition(FVector2D& OutPosition) const
{
#if !UE_BUILD_SHIPPING
	if (SimulatedCursor.IsSet())
	{
		OutPosition = SimulatedCursor.GetValue();
		return true;
	}
#endif
	float X = 0.f;
	float Y = 0.f;
	if (!GetMousePosition(X, Y))
	{
		return false;
	}
	OutPosition = FVector2D(X, Y);
	return true;
}

bool AModularPlayerController::GetRayAt(const FVector2D& ViewportPosition, FVector& OutOrigin, FVector& OutDirection) const
{
	return DeprojectScreenPositionToWorld(ViewportPosition.X, ViewportPosition.Y, OutOrigin, OutDirection);
}

bool AModularPlayerController::GetPlacementHitAt(const FVector2D& ViewportPosition, FHitResult& OutHit) const
{
	return GetHitResultAtScreenPosition(ViewportPosition, PlacementTraceChannel, /*bTraceComplex*/ false, OutHit);
}

bool AModularPlayerController::GetStageItemHitAt(const FVector2D& ViewportPosition, FHitResult& OutHit) const
{
	return GetHitResultAtScreenPosition(ViewportPosition, StageItemTraceChannel, /*bTraceComplex*/ false, OutHit);
}

bool AModularPlayerController::GetGizmoHitAt(const FVector2D& ViewportPosition, FHitResult& OutHit) const
{
	FVector RayOrigin, RayDirection;
	return Gizmo
		&& GetRayAt(ViewportPosition, RayOrigin, RayDirection)
		&& Gizmo->TraceHandles(RayOrigin, RayDirection, OutHit);
}

bool AModularPlayerController::GetPlacementHitUnderCursor(FHitResult& OutHit) const
{
	FVector2D Position;
	return GetCursorViewportPosition(Position) && GetPlacementHitAt(Position, OutHit);
}

bool AModularPlayerController::GetStageItemHitUnderCursor(FHitResult& OutHit) const
{
	FVector2D Position;
	return GetCursorViewportPosition(Position) && GetStageItemHitAt(Position, OutHit);
}

bool AModularPlayerController::GetGizmoHitUnderCursor(FHitResult& OutHit) const
{
	FVector2D Position;
	return GetCursorViewportPosition(Position) && GetGizmoHitAt(Position, OutHit);
}

AStageCameraPawn* AModularPlayerController::GetCameraPawn() const
{
	return Cast<AStageCameraPawn>(GetPawn());
}

bool AModularPlayerController::IsGizmoDragging() const
{
	return Gizmo && Gizmo->IsDragging();
}

// --- Primary button ---

EStageSelectionClick AModularPlayerController::GetSelectionClickFromKeyboard()
{
	return FSlateApplication::IsInitialized() && FSlateApplication::Get().GetModifierKeys().IsControlDown()
		? EStageSelectionClick::Toggle
		: EStageSelectionClick::Replace;
}

void AModularPlayerController::HandlePrimaryStarted()
{
	FVector2D Position;
	if (GetCursorViewportPosition(Position))
	{
		HandlePrimaryPressedAt(Position, GetSelectionClickFromKeyboard());
	}
}

void AModularPlayerController::HandlePrimaryTriggered()
{
	// Per-frame work only while a gizmo drag is in progress; an idle hold costs nothing and never places.
	FVector2D Position;
	if (IsGizmoDragging() && GetCursorViewportPosition(Position))
	{
		HandlePrimaryHeldAt(Position);
	}
}

void AModularPlayerController::HandlePrimaryCompleted()
{
	HandlePrimaryReleased();
}

void AModularPlayerController::HandlePrimaryPressedAt(const FVector2D& ViewportPosition, EStageSelectionClick Click)
{
	// The cursor is hidden and frozen while flying; a left click there has no meaningful target.
	if (bIsNavigatingCamera)
	{
		return;
	}

	if (PlacementTool->IsPlaceMode())
	{
		// A miss is passed on as an empty hit, so the tool reports "no surface" (error cue) instead of silence.
		FHitResult Hit;
		GetPlacementHitAt(ViewportPosition, Hit);
		if (SpanTool->IsToolActive())
		{
			// First click sets the start, each further click adds a point; a click on the last or the first point finishes the path.
			SpanTool->HandleClick(Hit);
			RequestPreviewRefresh();
			return;
		}
		if (PlacementTool->TryPlace(Hit))
		{
			// Stamping keeps the ghost out: re-trace so it lands on the new surface (the placed item) and shows the fresh verdict.
			RequestPreviewRefresh();
		}
		return;
	}

	if (TryBeginGizmoDragAt(ViewportPosition))
	{
		return;
	}

	SelectOrDeselectAt(ViewportPosition, Click);
}

void AModularPlayerController::HandlePrimaryHeldAt(const FVector2D& ViewportPosition)
{
	FVector RayOrigin, RayDirection;
	if (IsGizmoDragging() && GetRayAt(ViewportPosition, RayOrigin, RayDirection))
	{
		Gizmo->UpdateDrag(RayOrigin, RayDirection);
		// The rest of the selection follows the target's final (snapped, clamped) transform for this frame.
		GroupTransform->FollowLeader();
	}
}

void AModularPlayerController::HandlePrimaryReleased()
{
	if (Gizmo)
	{
		Gizmo->EndDrag();
	}
}

void AModularPlayerController::SelectOrDeselectAt(const FVector2D& ViewportPosition, EStageSelectionClick Click)
{
	// Selecting never moves the item: only an explicit gizmo handle drag or a numeric edit does.
	FHitResult ItemHit;
	const bool bHitItem = GetStageItemHitAt(ViewportPosition, ItemHit);

	if (Click == EStageSelectionClick::Toggle)
	{
		// Ctrl+Click on empty space keeps the selection, as in the Unreal Editor, so a missed click never loses a group.
		if (bHitItem)
		{
			Selection->ToggleActorSelection(ItemHit.GetActor());
		}
		return;
	}

	if (bHitItem && Selection->SelectActor(ItemHit.GetActor()))
	{
		return;
	}
	Selection->ClearSelection();
}

bool AModularPlayerController::TryBeginGizmoDragAt(const FVector2D& ViewportPosition)
{
	if (!Gizmo || !Gizmo->GetTarget())
	{
		return false;
	}

	FHitResult GizmoHit;
	FVector RayOrigin, RayDirection;
	if (!GetGizmoHitAt(ViewportPosition, GizmoHit) || !GetRayAt(ViewportPosition, RayOrigin, RayDirection))
	{
		return false;
	}

	// The drag moves the target and every selected item with it, so each must allow it (a locked item, a span's scale).
	// A refused press still lands on a handle: it is consumed, so it never falls through to selecting what lies behind.
	const FGameplayTag TransformParameter = Gizmo->GetMode() == EGizmoMode::Translate ? StageCraftTags::Param_Transform_Location
		: (Gizmo->GetMode() == EGizmoMode::Rotate ? StageCraftTags::Param_Transform_Rotation : StageCraftTags::Param_Transform_Scale);
	const FStageEconomyResultInfo Verdict = EvaluateSelectionTransform(TransformParameter);
	if (!Verdict.IsSuccess())
	{
		ReportRejection(Verdict);
		return true;
	}
	return Gizmo->TryBeginDrag(GizmoHit, RayOrigin, RayDirection);
}

// --- Keys ---

void AModularPlayerController::HandleNavigateStarted()
{
	// Turning the view mid-drag would drag the target along the camera path.
	if (IsGizmoDragging())
	{
		return;
	}
	bIsNavigatingCamera = true;
}

void AModularPlayerController::HandleNavigateCompleted()
{
	// Releasing only stops feeding input; the pawn eases to a stop on its own. Nothing is selected,
	// placed or deleted here, however short the press was.
	bIsNavigatingCamera = false;
}

void AModularPlayerController::HandleDelete()
{
	RequestDeleteSelection();
}

void AModularPlayerController::HandleUndo()
{
	RequestUndo();
}

void AModularPlayerController::HandleRedo()
{
	RequestRedo();
}

void AModularPlayerController::HandleGizmoDragStarted(AActor* Target)
{
	if (Target)
	{
		// The rest of the selection travels with the target, so it is neither left behind nor a snap neighbour.
		GroupTransform->BeginGroupEdit(*Target);
		Snapping->BeginMove(*Target, GroupTransform->GetFollowers());
	}
}

void AModularPlayerController::HandleGizmoDragFinished(AActor* Target, const FTransform& StartTransform)
{
	Snapping->EndMove();

	// The whole drag is one step, however many frames it took and however many items moved.
	const TArray<FStageTransformChange> Changes = GroupTransform->EndGroupEdit();
	if (Changes.IsEmpty())
	{
		EditHistory->RecordTransformChange(Target, StartTransform);
		return;
	}
	EditHistory->RecordTransformChanges(Changes);
}

void AModularPlayerController::HandleCancel()
{
	if (bIsNavigatingCamera)
	{
		return;
	}

	// One level at a time: first drop the span path being drawn, then leave Place mode (the item stays armed for P), then clear everything.
	if (PlacementTool->IsPlaceMode())
	{
		if (SpanTool->CancelPath())
		{
			RequestPreviewRefresh();
			return;
		}
		PlacementTool->EnterSelectMode();
		return;
	}

	Selection->ClearSelection();

	if (UStageItemSubsystem* ItemSubsystem = GetGameInstance() ? GetGameInstance()->GetSubsystem<UStageItemSubsystem>() : nullptr)
	{
		ItemSubsystem->ClearSelection();
	}
}

void AModularPlayerController::HandleToggleGizmoMode()
{
	if (Gizmo && !IsGizmoDragging())
	{
		Gizmo->CycleMode();
	}
}

void AModularPlayerController::HandleFinishPath()
{
	if (!bIsNavigatingCamera && SpanTool->IsToolActive() && SpanTool->FinishPath())
	{
		RequestPreviewRefresh();
	}
}

void AModularPlayerController::HandleRemovePathPoint()
{
	if (!bIsNavigatingCamera && SpanTool->IsToolActive() && SpanTool->RemoveLastPoint())
	{
		RequestPreviewRefresh();
	}
}

void AModularPlayerController::HandleTogglePlaceMode()
{
	if (bIsNavigatingCamera || IsGizmoDragging())
	{
		return;
	}
	PlacementTool->TogglePlaceMode();
}

// --- Camera ---

void AModularPlayerController::HandleCameraLook(const FInputActionValue& Value)
{
	if (bIsNavigatingCamera)
	{
		// An orthographic view does not turn: the right-button drag pans it instead, as in the Unreal Editor.
		if (ViewportView->IsOrthographic())
		{
			ViewportView->PanOrtho(Value.Get<FVector2D>());
		}
		else if (AStageCameraPawn* CameraPawn = GetCameraPawn())
		{
			CameraPawn->AddLookInput(Value.Get<FVector2D>());
		}
	}
}

void AModularPlayerController::HandleCameraMove(const FInputActionValue& Value)
{
	if (bIsNavigatingCamera)
	{
		if (AStageCameraPawn* CameraPawn = GetCameraPawn())
		{
			CameraPawn->AddFlyInput(Value.Get<FVector>());
		}
	}
}

void AModularPlayerController::HandleCameraSpeed(const FInputActionValue& Value)
{
	// Not gated on RMB: the wheel has no other job in the stage view, so speed can be dialled in
	// before flying as well as during. Wheel input over a UMG panel never reaches here (UMG consumes it).
	AStageCameraPawn* CameraPawn = GetCameraPawn();
	const float Steps = Value.Get<float>();
	if (!CameraPawn || FMath::IsNearlyZero(Steps))
	{
		return;
	}

	// Orthographic: the wheel zooms around the cursor (there is no depth to fly through).
	float CursorX = 0.f;
	float CursorY = 0.f;
	if (ViewportView->IsOrthographic() && GetMousePosition(CursorX, CursorY))
	{
		ViewportView->ZoomOrtho(Steps, FVector2D(CursorX, CursorY));
		return;
	}

	// The status bar shows the new speed through AStageCameraPawn::OnFlySpeedChanged.
	CameraPawn->AdjustFlySpeed(Steps);
}

void AModularPlayerController::ApplySnapSettings()
{
	if (Gizmo)
	{
		Gizmo->SetDragSnapping(Snapping->GetGridSnap(), Snapping->GetAngleSnap());
	}
	PlacementTool->SetGridSnap(Snapping->GetGridSnap());
	RequestPreviewRefresh(); // The ghost may land on a different cell now.
}

void AModularPlayerController::HandleViewChanged()
{
	RequestPreviewRefresh();
}

void AModularPlayerController::SetControlRotation(const FRotator& NewRotation)
{
	Super::SetControlRotation(NewRotation);
	RequestPreviewRefresh();
}

// --- Placement preview refresh ---

void AModularPlayerController::BindPreviewRefreshSources()
{
	if (UStageCraftGameViewportClient* ViewportClient = Cast<UStageCraftGameViewportClient>(GetWorld()->GetGameViewport()))
	{
		BoundViewportClient = ViewportClient;
		CursorMovedHandle = ViewportClient->OnCursorMoved.AddUObject(this, &ThisClass::HandleCursorMoved);
	}

	OnPossessedPawnChanged.AddUniqueDynamic(this, &ThisClass::HandlePossessedPawnChanged);
	BindCameraPawn(GetPawn());
}

void AModularPlayerController::UnbindPreviewRefreshSources()
{
	if (UStageCraftGameViewportClient* ViewportClient = BoundViewportClient.Get())
	{
		ViewportClient->OnCursorMoved.Remove(CursorMovedHandle);
	}
	BoundViewportClient.Reset();
	CursorMovedHandle.Reset();

	OnPossessedPawnChanged.RemoveDynamic(this, &ThisClass::HandlePossessedPawnChanged);
	BindCameraPawn(nullptr);
}

void AModularPlayerController::HandlePossessedPawnChanged(APawn* PreviousPawn, APawn* NewPawn)
{
	BindCameraPawn(NewPawn);
}

void AModularPlayerController::BindCameraPawn(APawn* NewPawn)
{
	if (USceneComponent* PreviousRoot = BoundCameraRoot.Get())
	{
		PreviousRoot->TransformUpdated.Remove(CameraTransformHandle);
	}
	BoundCameraRoot.Reset();
	CameraTransformHandle.Reset();

	if (USceneComponent* NewRoot = NewPawn ? NewPawn->GetRootComponent() : nullptr)
	{
		BoundCameraRoot = NewRoot;
		CameraTransformHandle = NewRoot->TransformUpdated.AddUObject(this, &ThisClass::HandleCameraTransformUpdated);
	}
}

void AModularPlayerController::HandleCursorMoved()
{
	RequestPreviewRefresh();
}

void AModularPlayerController::HandleCameraTransformUpdated(USceneComponent* UpdatedComponent, EUpdateTransformFlags UpdateTransformFlags, ETeleportType Teleport)
{
	RequestPreviewRefresh();
}

void AModularPlayerController::RequestPreviewRefresh()
{
	// Several sources can fire in one frame (cursor, camera move, camera turn): one trace on the next tick
	// covers them all, and runs after the camera has settled for this frame.
	if (bPreviewRefreshQueued || !PlacementTool || !PlacementTool->IsPlaceMode() || !GetWorld())
	{
		return;
	}
	bPreviewRefreshQueued = true;
	GetWorldTimerManager().SetTimerForNextTick(this, &ThisClass::RefreshPlacementPreview);
}

void AModularPlayerController::RefreshPlacementPreview()
{
	bPreviewRefreshQueued = false;
	if (!PlacementTool->IsPlaceMode())
	{
		return;
	}

	FVector2D Position;
	FHitResult Hit;
	const bool bHasSurface = GetCursorViewportPosition(Position) && GetPlacementHitAt(Position, Hit);
	if (SpanTool->IsToolActive())
	{
		if (bHasSurface)
		{
			SpanTool->UpdateTarget(Hit);
		}
		else
		{
			SpanTool->ClearTarget();
		}
		return;
	}

	if (bHasSurface)
	{
		PlacementTool->UpdateTarget(Hit);
	}
	else
	{
		PlacementTool->ClearTarget();
	}
}

// --- Development simulation ---

#if !UE_BUILD_SHIPPING
void AModularPlayerController::DevSetSimulatedCursor(const FVector2D& ViewportPosition)
{
	SimulatedCursor = ViewportPosition;
	RequestPreviewRefresh();
}

void AModularPlayerController::DevClearSimulatedCursor()
{
	SimulatedCursor.Reset();
	RequestPreviewRefresh();
}

void AModularPlayerController::DevSimulatePrimaryPressed(EStageSelectionClick Click)
{
	FVector2D Position;
	if (GetCursorViewportPosition(Position))
	{
		HandlePrimaryPressedAt(Position, Click);
	}
}

void AModularPlayerController::DevSimulatePrimaryHeld()
{
	HandlePrimaryTriggered();
}

void AModularPlayerController::DevSimulatePrimaryReleased()
{
	HandlePrimaryCompleted();
}
#endif

// --- Request bridge ---

#define LOCTEXT_NAMESPACE "StageCraftPlayerController"

AStageCraftGameModeBase* AModularPlayerController::GetStageGameMode() const
{
	// Null on clients in a networked session: the decision then has to go to the server (future RPC).
	return GetWorld() ? GetWorld()->GetAuthGameMode<AStageCraftGameModeBase>() : nullptr;
}

UStageEconomySubsystem* AModularPlayerController::GetEconomy() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UStageEconomySubsystem>() : nullptr;
}

FStageEconomyResultInfo AModularPlayerController::RequestParameterChange(UObject* Target, FGameplayTag ParameterId, const FStageParameterValue& Value)
{
	const AStageCraftGameModeBase* GameMode = GetStageGameMode();
	UStageSessionSubsystem* Session = UWorld::GetSubsystem<UStageSessionSubsystem>(GetWorld());
	if (!GameMode || !Session)
	{
		// Deny by default: without the rules authority there is nobody to approve the edit.
		const FStageEconomyResultInfo Result = FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest,
			LOCTEXT("NoAuthority", "Editing needs the StageCraft game mode."));
		ReportRejection(Result);
		return Result;
	}

	const FStageEconomyResultInfo Verdict = GameMode->EvaluateParameterChange(this, Target, ParameterId, Value);
	if (!Verdict.IsSuccess())
	{
		ReportRejection(Verdict);
		return Verdict;
	}

	// Transform edits are undoable; the value actually stored (after clamping) is what gets recorded. A transform edit of a
	// selected item moves the rest of the selection with it, as a gizmo drag does. Mid-drag the gizmo owns the group edit.
	AActor* TransformTarget = StageCraftTags::IsTransformParameter(ParameterId) ? Cast<AActor>(Target) : nullptr;
	const FTransform TransformBefore = TransformTarget ? TransformTarget->GetActorTransform() : FTransform::Identity;
	const bool bGroupEdit = TransformTarget && !IsGizmoDragging();

	// The rest of the selection moves too, so every selected item must allow the edit, as for a gizmo drag.
	if (bGroupEdit && Selection->IsActorSelected(TransformTarget))
	{
		const FStageEconomyResultInfo GroupVerdict = EvaluateSelectionTransform(ParameterId);
		if (!GroupVerdict.IsSuccess())
		{
			ReportRejection(GroupVerdict);
			return GroupVerdict;
		}
	}

	// Edits that reshape the item (a span's length or sag) are undoable steps, like placing or deleting.
	FStageParameterValue QuantityBefore;
	const bool bRecordParameter = StageCraftTags::IsShapeParameter(ParameterId) && Target->Implements<UStageParameterInterface>()
		&& IStageParameterInterface::Execute_GetParameterValue(Target, ParameterId, QuantityBefore);

	if (bGroupEdit)
	{
		GroupTransform->BeginGroupEdit(*TransformTarget);
	}

	// A value the target cannot take (wrong type, read-only id) is not a rule violation, so it is
	// returned but not reported; the panel snaps back on read-back anyway.
	if (!Session->ApplyParameterChange(Target, ParameterId, Value))
	{
		if (bGroupEdit)
		{
			GroupTransform->EndGroupEdit();
		}
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NotAccepted", "The value was not accepted."));
	}

	if (bGroupEdit)
	{
		GroupTransform->FollowLeader();
		EditHistory->RecordTransformChanges(GroupTransform->EndGroupEdit());
	}
	else if (TransformTarget)
	{
		EditHistory->RecordTransformChange(TransformTarget, TransformBefore);
	}

	if (bRecordParameter)
	{
		EditHistory->RecordParameterChange(Cast<AActor>(Target), ParameterId, FindParameterName(*Target, ParameterId), QuantityBefore);
	}

	// A hidden item can be neither seen nor clicked, so it must not keep the gizmo or the inspector.
	if (ParameterId == StageCraftTags::Param_Info_Visible && Value.Type == EStageParameterType::Bool && !Value.bBool)
	{
		Selection->DeselectActor(Cast<AActor>(Target));
	}
	return FStageEconomyResultInfo::Ok();
}

FText AModularPlayerController::FindParameterName(const UObject& Target, const FGameplayTag& ParameterId)
{
	if (Target.Implements<UStageParameterInterface>())
	{
		for (const FStageParameterSection& Section : IStageParameterInterface::Execute_GetParameterSections(&Target))
		{
			if (const FStageParameterDescriptor* Descriptor = Section.Parameters.FindByPredicate([&ParameterId](const FStageParameterDescriptor& Candidate) { return Candidate.Id == ParameterId; }))
			{
				return Descriptor->DisplayName;
			}
		}
	}
	return FText::FromName(ParameterId.GetTagName());
}

FStageEconomyResultInfo AModularPlayerController::EvaluateSelectionTransform(const FGameplayTag& TransformParameter) const
{
	const AStageCraftGameModeBase* GameMode = GetStageGameMode();
	if (!GameMode)
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoAuthorityMove", "Editing needs the StageCraft game mode."));
	}

	for (AActor* Selected : Selection->GetSelectedActors())
	{
		// The current value: the rules judge whether this item may be moved, rotated or scaled at all, not where to.
		FStageParameterValue Current;
		if (!IsValid(Selected) || !Selected->Implements<UStageParameterInterface>()
			|| !IStageParameterInterface::Execute_GetParameterValue(Selected, TransformParameter, Current))
		{
			continue;
		}
		const FStageEconomyResultInfo Verdict = GameMode->EvaluateParameterChange(this, Selected, TransformParameter, Current);
		if (!Verdict.IsSuccess())
		{
			return Verdict;
		}
	}
	return FStageEconomyResultInfo::Ok();
}

EStageCommandResult AModularPlayerController::ApplyHistoryParameter(AModularBaseActor& Item, const FGameplayTag& ParameterId, const FStageParameterValue& Value)
{
	const AStageCraftGameModeBase* GameMode = GetStageGameMode();
	UStageSessionSubsystem* Session = UWorld::GetSubsystem<UStageSessionSubsystem>(GetWorld());
	if (!GameMode || !Session)
	{
		ReportRejection(FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoAuthorityUndo", "Undo needs the StageCraft game mode.")));
		return EStageCommandResult::Refused;
	}

	const FStageEconomyResultInfo Verdict = GameMode->EvaluateParameterChange(this, &Item, ParameterId, Value);
	if (!Verdict.IsSuccess())
	{
		ReportRejection(Verdict);
		return EStageCommandResult::Refused;
	}
	return Session->ApplyParameterChange(&Item, ParameterId, Value) ? EStageCommandResult::Succeeded : EStageCommandResult::Invalid;
}

bool AModularPlayerController::ValidateHistoryTransform(const AModularBaseActor& Item, const FTransform& Transform)
{
	const AStageCraftGameModeBase* GameMode = GetStageGameMode();
	if (!GameMode)
	{
		ReportRejection(FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoAuthorityUndo", "Undo needs the StageCraft game mode.")));
		return false;
	}

	const FStageEconomyResultInfo Verdict = GameMode->EvaluateTransformChange(this, Item, Transform);
	if (!Verdict.IsSuccess())
	{
		ReportRejection(Verdict);
		return false;
	}
	return true;
}

bool AModularPlayerController::BeginEditTransaction()
{
	return EditHistory->BeginTransaction();
}

void AModularPlayerController::EndEditTransaction()
{
	EditHistory->EndTransaction();
}

bool AModularPlayerController::IsBusyWithPointer() const
{
	return bIsNavigatingCamera || IsGizmoDragging();
}

int32 AModularPlayerController::RequestDeleteSelection()
{
	if (IsBusyWithPointer() || !Selection->HasSelection())
	{
		return 0;
	}

	// One undoable step; the history component releases each item's selection before the actor is torn down.
	const int32 Deleted = EditHistory->DeleteItems(Selection->GetSelectedActors());
	RequestPreviewRefresh();
	return Deleted;
}

void AModularPlayerController::RequestSelectAll()
{
	const UStageSessionSubsystem* Session = UWorld::GetSubsystem<UStageSessionSubsystem>(GetWorld());
	if (IsBusyWithPointer() || !Session)
	{
		return;
	}

	// Selecting is a Select-mode activity; the ghost would otherwise sit on top of a selected group.
	PlacementTool->EnterSelectMode();
	TArray<AActor*> VisibleItems;
	for (AModularBaseActor* Item : Session->GetPlacedItems())
	{
		// As in the Unreal Editor: Select All picks what can be seen. Hidden items are selected from the outliner.
		if (!Item->IsHiddenOnStage())
		{
			VisibleItems.Add(Item);
		}
	}
	Selection->SelectActors(VisibleItems);
}

void AModularPlayerController::RequestSelectItems(const TArray<AModularBaseActor*>& Items, EStageSelectionClick Click)
{
	if (IsBusyWithPointer())
	{
		return;
	}

	PlacementTool->EnterSelectMode();
	const TArray<AActor*> Targets(Items);
	if (Click == EStageSelectionClick::Replace)
	{
		Selection->SelectActors(Targets);
		return;
	}

	const bool bAllSelected = !Targets.IsEmpty() && Algo::AllOf(Targets, [this](const AActor* Target) { return Selection->IsActorSelected(Target); });
	if (bAllSelected)
	{
		Selection->RemoveActorsFromSelection(Targets);
	}
	else
	{
		Selection->AddActorsToSelection(Targets);
	}
}

bool AModularPlayerController::RequestExportBillOfMaterials()
{
	UStageBillOfMaterialsSubsystem* Report = UWorld::GetSubsystem<UStageBillOfMaterialsSubsystem>(GetWorld());
	FText Reason = LOCTEXT("NoReport", "The bill of materials is not available in this level.");
	if (Report && Report->StartExport(Reason))
	{
		return true;
	}
	ReportRejection(FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, Reason));
	return false;
}

bool AModularPlayerController::RequestExportPatchSheet()
{
	UStagePatchSheetSubsystem* PatchSheet = UWorld::GetSubsystem<UStagePatchSheetSubsystem>(GetWorld());
	FText Reason = LOCTEXT("NoPatchSheet", "The patch sheet is not available in this level.");
	if (PatchSheet && PatchSheet->StartExport(Reason))
	{
		return true;
	}
	ReportRejection(FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, Reason));
	return false;
}

UShowControlSubsystem* AModularPlayerController::ApproveCueRequest(EStageCueRequest Request, float CueNumber)
{
	const AStageCraftGameModeBase* GameMode = GetStageGameMode();
	UShowControlSubsystem* Show = UWorld::GetSubsystem<UShowControlSubsystem>(GetWorld());
	const FStageEconomyResultInfo Verdict = GameMode && Show
		? GameMode->EvaluateCueRequest(this, Request, CueNumber)
		: FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoCues", "Cues are not available in this level."));
	if (!Verdict.IsSuccess())
	{
		ReportRejection(Verdict);
		return nullptr;
	}
	return Show;
}

void AModularPlayerController::RecordCueEdit(const FText& Description, const TArray<FShowCue>& Before, const UShowControlSubsystem& Show) const
{
	if (EditHistory)
	{
		EditHistory->RecordCueListChange(Description, Before, Show.GetCues());
	}
}

bool AModularPlayerController::RequestStoreCue(float CueNumber, const FText& Label, float FadeTime)
{
	UShowControlSubsystem* Show = ApproveCueRequest(EStageCueRequest::Store, CueNumber);
	if (!Show)
	{
		return false;
	}
	const FText Description = FText::Format(Show->FindCue(CueNumber) ? LOCTEXT("UpdateCueLook", "Re-record Cue {0}") : LOCTEXT("StoreCue", "Store Cue {0}"),
		FText::AsNumber(StageCueRules::RoundCueNumber(CueNumber)));
	const TArray<FShowCue> Before = Show->GetCues();
	if (!Show->StoreCue(CueNumber, Label, FadeTime))
	{
		return false;
	}
	RecordCueEdit(Description, Before, *Show);
	return true;
}

bool AModularPlayerController::RequestUpdateCue(float CueNumber, const FText& Label, float FadeTime)
{
	UShowControlSubsystem* Show = ApproveCueRequest(EStageCueRequest::Update, CueNumber);
	if (!Show)
	{
		return false;
	}
	const TArray<FShowCue> Before = Show->GetCues();
	if (!Show->UpdateCue(CueNumber, Label, FadeTime))
	{
		return false;
	}
	// An update that changed nothing records nothing.
	const FShowCue* Old = Before.FindByPredicate([CueNumber](const FShowCue& Cue)
	{
		return FMath::IsNearlyEqual(Cue.Number, CueNumber, StageCueRules::NumberResolution * 0.5f);
	});
	const FShowCue* New = Show->FindCue(CueNumber);
	if (Old && New && (!Old->Label.EqualTo(New->Label) || !FMath::IsNearlyEqual(Old->FadeTime, New->FadeTime)))
	{
		RecordCueEdit(FText::Format(LOCTEXT("EditCue", "Edit Cue {0}"), FText::AsNumber(New->Number)), Before, *Show);
	}
	return true;
}

bool AModularPlayerController::RequestDeleteCue(float CueNumber)
{
	UShowControlSubsystem* Show = ApproveCueRequest(EStageCueRequest::Delete, CueNumber);
	if (!Show)
	{
		return false;
	}
	const TArray<FShowCue> Before = Show->GetCues();
	if (!Show->DeleteCue(CueNumber))
	{
		return false;
	}
	RecordCueEdit(FText::Format(LOCTEXT("DeleteCue", "Delete Cue {0}"), FText::AsNumber(StageCueRules::RoundCueNumber(CueNumber))), Before, *Show);
	return true;
}

bool AModularPlayerController::RequestGoToCue(float CueNumber)
{
	UShowControlSubsystem* Show = ApproveCueRequest(EStageCueRequest::Play, CueNumber);
	if (!Show)
	{
		return false;
	}
	if (!Show->GoToCue(CueNumber))
	{
		ReportRejection(FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoCueToGo", "That cue does not exist.")));
		return false;
	}
	return true;
}

bool AModularPlayerController::RequestCueGo()
{
	UShowControlSubsystem* Show = ApproveCueRequest(EStageCueRequest::Play, 0.f);
	if (!Show)
	{
		return false;
	}
	if (!Show->Go())
	{
		ReportRejection(FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest,
			Show->GetCues().IsEmpty() ? LOCTEXT("NoCues_Go", "There are no cues yet. Store one first.") : LOCTEXT("LastCue", "This is the last cue.")));
		return false;
	}
	return true;
}

bool AModularPlayerController::RequestCueBack()
{
	UShowControlSubsystem* Show = ApproveCueRequest(EStageCueRequest::Play, 0.f);
	if (!Show)
	{
		return false;
	}
	if (!Show->Back())
	{
		ReportRejection(FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("FirstCue", "There is no earlier cue to go back to.")));
		return false;
	}
	return true;
}

bool AModularPlayerController::RequestSetControlSource(EStageControlSource Source)
{
	UShowControlSubsystem* Show = ApproveCueRequest(EStageCueRequest::SetControlSource, 0.f);
	if (!Show)
	{
		return false;
	}
	Show->SetControlSource(Source);
	return true;
}

int32 AModularPlayerController::RequestImportModelFiles(const TArray<FString>& Files)
{
	const AStageCraftGameModeBase* GameMode = GetStageGameMode();
	UStageMeshImportSubsystem* Importer = GetGameInstance() ? GetGameInstance()->GetSubsystem<UStageMeshImportSubsystem>() : nullptr;
	const FStageEconomyResultInfo Verdict = GameMode && Importer
		? GameMode->EvaluateModelImport(this)
		: FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoImport", "Model import needs the StageCraft game mode."));
	if (!Verdict.IsSuccess())
	{
		ReportRejection(Verdict);
		return 0;
	}

	const TArray<FStageMeshImportOutcome> Refusals = Importer->RequestImport(Files);
	if (!Refusals.IsEmpty())
	{
		// One message for the whole drop: the first reason, and how many other files were left out.
		const FText Message = Refusals.Num() == 1
			? Refusals[0].Message
			: FText::Format(LOCTEXT("SeveralRefused", "{0} (and {1} more files were not imported)"), Refusals[0].Message, FText::AsNumber(Refusals.Num() - 1));
		ReportRejection(FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, Message));
	}
	return Files.Num() - Refusals.Num();
}

int32 AModularPlayerController::RequestBrowseModelFiles()
{
	const UStageMeshImportSubsystem* Importer = GetGameInstance() ? GetGameInstance()->GetSubsystem<UStageMeshImportSubsystem>() : nullptr;
	if (!Importer || !CanBrowseModelFiles())
	{
		ReportRejection(FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest,
			LOCTEXT("NoDialog", "This build has no file dialog. Drag model files onto the Library or the viewport instead.")));
		return 0;
	}

	// Asked before the dialog opens, so a session without imports does not let the user browse for nothing.
	const AStageCraftGameModeBase* GameMode = GetStageGameMode();
	const FStageEconomyResultInfo Verdict = GameMode ? GameMode->EvaluateModelImport(this)
		: FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoImportMode", "Model import needs the StageCraft game mode."));
	if (!Verdict.IsSuccess())
	{
		ReportRejection(Verdict);
		return 0;
	}

	TArray<FString> Files;
	const FString Filter = StageMeshImport::MakeFileDialogFilter(Importer->GetSupportedExtensions());
	if (!StageFileDialog::OpenFiles(LOCTEXT("ImportTitle", "Import Models into the Library"), LastImportDirectory, Filter, Files))
	{
		return 0;
	}
	LastImportDirectory = FPaths::GetPath(Files[0]);
	return RequestImportModelFiles(Files);
}

bool AModularPlayerController::CanBrowseModelFiles()
{
	return StageFileDialog::IsAvailable();
}

int32 AModularPlayerController::RequestClearStage()
{
	if (IsBusyWithPointer())
	{
		return 0;
	}

	const int32 Removed = EditHistory->ClearStage();
	RequestPreviewRefresh();
	return Removed;
}

EStageSceneResult AModularPlayerController::RequestSaveScene(const FString& Name)
{
	return ReportSceneRefusal(EStageSceneOperation::Save, SceneFiles->SaveScene(Name.TrimStartAndEnd()), Name);
}

EStageSceneResult AModularPlayerController::RequestLoadScene(const FString& Name)
{
	// Loading destroys every item, including one the gizmo may be holding.
	if (IsBusyWithPointer())
	{
		return EStageSceneResult::Busy;
	}
	return ReportSceneRefusal(EStageSceneOperation::Load, SceneFiles->LoadScene(Name), Name);
}

EStageSceneResult AModularPlayerController::RequestRecoverAutosave(const FString& Name)
{
	if (IsBusyWithPointer())
	{
		return EStageSceneResult::Busy;
	}
	return ReportSceneRefusal(EStageSceneOperation::Load, SceneFiles->RecoverAutosave(Name), Name);
}

EStageSceneResult AModularPlayerController::RequestDeleteScene(const FString& Name)
{
	// The component reports the outcome (success or failure) itself; only a refusal to start is reported here.
	const EStageSceneResult Result = SceneFiles->DeleteScene(Name);
	return Result == EStageSceneResult::Busy || Result == EStageSceneResult::Unavailable
		? ReportSceneRefusal(EStageSceneOperation::Delete, Result, Name)
		: Result;
}

bool AModularPlayerController::RequestRender()
{
	UStageRenderSubsystem* Render = UWorld::GetSubsystem<UStageRenderSubsystem>(GetWorld());
	if (!Render || !PlayerCameraManager)
	{
		ReportRejection(FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoRender", "Rendering is not available in this level.")));
		return false;
	}

	FStageRenderRequest Request;
	// Editor helpers (gizmo, ghosts) register with the render subsystem themselves and are left out of every image.
	Request.View = PlayerCameraManager->GetCameraCacheView();

	// The engine's rule (FMinimalViewInfo::CalculateProjectionMatrixGivenViewRectangle): the view keeps its horizontal
	// extent under Maintain X FOV, or Major Axis FOV on a landscape viewport; otherwise its vertical one.
	if (const ULocalPlayer* ViewingPlayer = GetLocalPlayer())
	{
		int32 SizeX = 0;
		int32 SizeY = 0;
		GetViewportSize(SizeX, SizeY);
		const EAspectRatioAxisConstraint Constraint = Request.View.AspectRatioAxisConstraint.Get(ViewingPlayer->AspectRatioAxisConstraint);
		const bool bSpansWidth = Constraint == AspectRatio_MaintainXFOV || (Constraint == AspectRatio_MajorAxisFOV && SizeX > SizeY);
		Request.bViewSpansHeight = !bSpansWidth;
	}

	FText Reason;
	if (!Render->StartRender(Request, Reason))
	{
		ReportRejection(FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, Reason));
		return false;
	}
	return true;
}

EStageSceneResult AModularPlayerController::ReportSceneRefusal(EStageSceneOperation Operation, EStageSceneResult Result, const FString& Name)
{
	if (Result != EStageSceneResult::Success)
	{
		ReportRejection(FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, UStageSceneComponent::DescribeFailure(Operation, Result, Name)));
	}
	return Result;
}

void AModularPlayerController::HandleSceneOperationFinished(const FStageSceneOperationResult& Result)
{
	// Failures take the common refusal path (status bar warning + error cue); successes are announced by the status bar.
	if (!Result.IsSuccess())
	{
		ReportRejection(FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, Result.Message));
		return;
	}
	if (Result.Operation == EStageSceneOperation::Load)
	{
		RequestPreviewRefresh();
	}
}

EStageCommandResult AModularPlayerController::RequestUndo()
{
	return RequestHistoryStep(/*bUndo*/ true);
}

EStageCommandResult AModularPlayerController::RequestRedo()
{
	return RequestHistoryStep(/*bUndo*/ false);
}

EStageCommandResult AModularPlayerController::RequestHistoryStep(bool bUndo)
{
	// Mid-drag the gizmo owns the target's transform; reverting it underneath would fight the drag.
	if (IsBusyWithPointer())
	{
		return EStageCommandResult::NothingToDo;
	}

	const EStageCommandResult Result = bUndo ? EditHistory->Undo() : EditHistory->Redo();
	if (Result == EStageCommandResult::Invalid)
	{
		// Refusals were reported by the placement validator; a step that can never apply is reported here, then dropped.
		ReportRejection(FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, bUndo
			? LOCTEXT("UndoInvalid", "That step can no longer be undone: its item no longer exists.")
			: LOCTEXT("RedoInvalid", "That step can no longer be redone: its item no longer exists.")));
	}

	// The ghost may have been resting on an item that just appeared, moved or disappeared.
	RequestPreviewRefresh();
	return Result;
}

FStageEconomyResultInfo AModularPlayerController::RequestPurchase(UStageProductData* Product)
{
	UStageEconomySubsystem* Economy = GetEconomy();
	const FStageEconomyResultInfo Result = Economy
		? Economy->RequestPurchase(Product)
		: FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoEconomy", "The shop is not available."));
	if (!Result.IsSuccess())
	{
		ReportRejection(Result);
	}
	return Result;
}

void AModularPlayerController::HandlePurchaseCompleted(UStageProductData* Product, FStageEconomyResultInfo Result)
{
	if (!Result.IsSuccess())
	{
		ReportRejection(Result);
		return;
	}

	// The status bar announces it (UStageStatusBarWidget binds UStageEconomySubsystem::OnPurchaseCompleted).
	UE_LOG(LogStageCraft, Log, TEXT("%s: purchased %s."), *GetName(), *GetNameSafe(Product));
}

FStageEconomyResultInfo AModularPlayerController::CanPlaceItem(const UBaseItemData* Item) const
{
	return EvaluatePlacementRules(Item, 1);
}

FStageEconomyResultInfo AModularPlayerController::EvaluatePlacementRules(const UBaseItemData* Item, int32 Quantity) const
{
	const AStageCraftGameModeBase* GameMode = GetStageGameMode();
	return GameMode
		? GameMode->EvaluatePlacement(this, Item, Quantity)
		: FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoAuthorityPlace", "Placement needs the StageCraft game mode."));
}

FStageEconomyResultInfo AModularPlayerController::ValidatePlacement(const UBaseItemData& Item, int32 Quantity)
{
	const FStageEconomyResultInfo Result = EvaluatePlacementRules(&Item, Quantity);
	if (!Result.IsSuccess())
	{
		ReportRejection(Result);
	}
	return Result;
}

FStageEconomyResultInfo AModularPlayerController::EvaluatePlacementPreview(const UBaseItemData& Item, int32 Quantity)
{
	// Same rules as a commit, without reporting: hovering a locked item must not raise toasts or error cues.
	return EvaluatePlacementRules(&Item, Quantity);
}

FStageEconomyResultInfo AModularPlayerController::RequestPlaceItem(UBaseItemData* Item)
{
	UStageItemSubsystem* ItemSubsystem = GetGameInstance() ? GetGameInstance()->GetSubsystem<UStageItemSubsystem>() : nullptr;
	if (!Item || !ItemSubsystem)
	{
		const FStageEconomyResultInfo Result = FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest,
			LOCTEXT("NoItemToPlace", "There is no item to place."));
		ReportRejection(Result);
		return Result;
	}

	const FStageEconomyResultInfo Verdict = ValidatePlacement(*Item, 1);
	if (!Verdict.IsSuccess())
	{
		return Verdict;
	}

	// Arming streams the item's spawn assets; the ghost appears when OnArmedItemChanged fires.
	ItemSubsystem->SelectItem(Item);
	PlacementTool->EnterPlaceMode();
	return Verdict;
}

void AModularPlayerController::ReportRejection(const FStageEconomyResultInfo& Result)
{
	UE_LOG(LogStageCraft, Log, TEXT("%s: request refused (%s): %s"), *GetName(),
		*UEnum::GetValueAsString(Result.Code), *Result.Message.ToString());

	// Shown by the workspace status bar (UStageStatusBarWidget) and answered with the error cue; nothing is drawn over the viewport.
	OnRequestRejected.Broadcast(Result);
}

void AModularPlayerController::DecorateParameterSections(const UObject* Target, TArray<FStageParameterSection>& Sections) const
{
	const AStageCraftGameModeBase* GameMode = GetStageGameMode();
	if (!Target)
	{
		return;
	}

	for (FStageParameterSection& Section : Sections)
	{
		for (FStageParameterDescriptor& Descriptor : Section.Parameters)
		{
			if (Descriptor.bReadOnly)
			{
				continue;
			}

			// Same check a commit of the current value would get, so the display never promises an edit the rules refuse.
			const FStageEconomyResultInfo Verdict = GameMode
				? GameMode->EvaluateParameterChange(this, Target, Descriptor.Id, Descriptor.Value)
				: FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, FText::GetEmpty());
			if (!Verdict.IsSuccess())
			{
				Descriptor.bReadOnly = true;
				Descriptor.bLocked = Verdict.Code == EStageEconomyResult::Locked;
				continue;
			}

			if (Descriptor.GetType() == EStageParameterType::Enum && GameMode)
			{
				for (int32 Option = 0; Option < Descriptor.Options.Num(); ++Option)
				{
					if (Option == Descriptor.Value.Integer)
					{
						continue;
					}
					const FStageEconomyResultInfo OptionVerdict = GameMode->EvaluateParameterChange(this, Target, Descriptor.Id, FStageParameterValue::MakeEnum(Option));
					if (OptionVerdict.Code == EStageEconomyResult::Locked)
					{
						Descriptor.Options[Option] = FText::Format(LOCTEXT("LockedOption", "{0} (locked)"), Descriptor.Options[Option]);
					}
				}
			}
		}
	}
}

#undef LOCTEXT_NAMESPACE
