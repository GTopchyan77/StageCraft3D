// Copyright Epic Games, Inc. All Rights Reserved.

#include "Player/ModularPlayerController.h"

#include "Actors/ModularTransformGizmo.h"
#include "Audio/StageEditorAudioFeedbackComponent.h"
#include "Blueprint/UserWidget.h"
#include "Components/SceneComponent.h"
#include "Components/SelectionComponent.h"
#include "Components/SpawnSystemComponent.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
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
#include "Interaction/StageCraftCollision.h"
#include "ModularSceneBuilder.h"
#include "Placement/StagePlacementToolComponent.h"
#include "Player/StageCameraPawn.h"
#include "Player/StageCraftGameViewportClient.h"
#include "Subsystems/StageEconomySubsystem.h"
#include "Subsystems/StageItemSubsystem.h"
#include "Subsystems/StageSessionSubsystem.h"
#include "TimerManager.h"
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

	Selection->OnSelectionChanged.AddUniqueDynamic(this, &ThisClass::HandleSelectionChanged);

	// Every spawn is decided by the GameMode through this controller; the preview colour asks the same rules without reporting.
	SpawnSystem->PlacementValidator.BindUObject(this, &ThisClass::ValidatePlacement);
	PlacementTool->PreviewEvaluator.BindUObject(this, &ThisClass::EvaluatePlacementPreview);
	PlacementTool->OnEditModeChanged.AddUniqueDynamic(this, &ThisClass::HandleEditModeChanged);
	PlacementTool->OnArmedItemChanged.AddUniqueDynamic(this, &ThisClass::HandleArmedItemChanged);
	BindPreviewRefreshSources();

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
	PlacementTool->OnEditModeChanged.RemoveDynamic(this, &ThisClass::HandleEditModeChanged);
	PlacementTool->OnArmedItemChanged.RemoveDynamic(this, &ThisClass::HandleArmedItemChanged);
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
		Gizmo->Destroy();
		Gizmo = nullptr;
	}

	Super::EndPlay(EndPlayReason);
}

void AModularPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	if (!EditorMappingContext || !PlaceAction || !NavigateAction || !DeleteAction || !CancelAction || !ToggleGizmoModeAction || !TogglePlaceModeAction)
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

	// A fresh context, because an assigned one cannot be trusted to map actions it was not authored with.
	// P is free in the camera context (which maps W/A/S/D/Q/E), so it is never consumed by navigation.
	EditorMappingContext = NewObject<UInputMappingContext>(this, TEXT("IMC_StageEditor_Default"));
	EditorMappingContext->MapKey(PlaceAction, EKeys::LeftMouseButton);
	EditorMappingContext->MapKey(NavigateAction, EKeys::RightMouseButton);
	EditorMappingContext->MapKey(DeleteAction, EKeys::Delete);
	EditorMappingContext->MapKey(CancelAction, EKeys::Escape);
	EditorMappingContext->MapKey(ToggleGizmoModeAction, EKeys::SpaceBar);
	EditorMappingContext->MapKey(TogglePlaceModeAction, EKeys::P);
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

void AModularPlayerController::HandlePrimaryStarted()
{
	FVector2D Position;
	if (GetCursorViewportPosition(Position))
	{
		HandlePrimaryPressedAt(Position);
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

void AModularPlayerController::HandlePrimaryPressedAt(const FVector2D& ViewportPosition)
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
		PlacementTool->TryPlace(Hit);
		return;
	}

	if (TryBeginGizmoDragAt(ViewportPosition))
	{
		return;
	}

	SelectOrDeselectAt(ViewportPosition);
}

void AModularPlayerController::HandlePrimaryHeldAt(const FVector2D& ViewportPosition)
{
	FVector RayOrigin, RayDirection;
	if (IsGizmoDragging() && GetRayAt(ViewportPosition, RayOrigin, RayDirection))
	{
		Gizmo->UpdateDrag(RayOrigin, RayDirection);
	}
}

void AModularPlayerController::HandlePrimaryReleased()
{
	if (Gizmo)
	{
		Gizmo->EndDrag();
	}
}

void AModularPlayerController::SelectOrDeselectAt(const FVector2D& ViewportPosition)
{
	// Selecting never moves the item: only an explicit gizmo handle drag or a numeric edit does.
	FHitResult ItemHit;
	if (GetStageItemHitAt(ViewportPosition, ItemHit) && Selection->SelectActor(ItemHit.GetActor()))
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
	return GetGizmoHitAt(ViewportPosition, GizmoHit)
		&& GetRayAt(ViewportPosition, RayOrigin, RayDirection)
		&& Gizmo->TryBeginDrag(GizmoHit, RayOrigin, RayDirection);
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
	AActor* Target = Selection->GetSelectedActor();
	if (!Target || bIsNavigatingCamera || IsGizmoDragging())
	{
		return;
	}

	// Released before destruction rather than via OnDestroyed, so the gizmo and inspector never
	// observe a selected actor that is being torn down.
	Selection->ClearSelection();
	SpawnSystem->TryDeleteActor(Target);
}

void AModularPlayerController::HandleCancel()
{
	if (bIsNavigatingCamera)
	{
		return;
	}

	// One level at a time: first leave Place mode (the item stays armed for P), then clear everything.
	if (PlacementTool->IsPlaceMode())
	{
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
		if (AStageCameraPawn* CameraPawn = GetCameraPawn())
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

	const float NewSpeed = CameraPawn->AdjustFlySpeed(Steps);

#if !UE_BUILD_SHIPPING
	// Interim readout until the HUD binds AStageCameraPawn::OnFlySpeedChanged; a fixed key replaces the previous line.
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(static_cast<uint64>(GetUniqueID()), 1.5f, FColor(120, 200, 255),
			FString::Printf(TEXT("Camera speed: %.2f m/s  |  look x%.2f"), NewSpeed / 100.f, CameraPawn->GetLookSpeedScale()));
	}
#endif
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
	if (GetCursorViewportPosition(Position) && GetPlacementHitAt(Position, Hit))
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

void AModularPlayerController::DevSimulatePrimaryPressed()
{
	HandlePrimaryStarted();
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

	// A value the target cannot take (wrong type, read-only id) is not a rule violation, so it is
	// returned but not reported; the panel snaps back on read-back anyway.
	if (!Session->ApplyParameterChange(Target, ParameterId, Value))
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NotAccepted", "The value was not accepted."));
	}
	return FStageEconomyResultInfo::Ok();
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

	UE_LOG(LogStageCraft, Log, TEXT("%s: purchased %s."), *GetName(), *GetNameSafe(Product));
	// Interim feedback until a shop widget binds UStageEconomySubsystem::OnPurchaseCompleted.
	if (GEngine && Product)
	{
		GEngine->AddOnScreenDebugMessage(static_cast<uint64>(GetUniqueID()) + 1, 3.f, FColor(120, 230, 140),
			FText::Format(LOCTEXT("Purchased", "Purchased {0}"), Product->DisplayName).ToString());
	}
}

FStageEconomyResultInfo AModularPlayerController::CanPlaceItem(const UBaseItemData* Item) const
{
	const AStageCraftGameModeBase* GameMode = GetStageGameMode();
	return GameMode
		? GameMode->EvaluatePlacement(this, Item)
		: FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoAuthorityPlace", "Placement needs the StageCraft game mode."));
}

FStageEconomyResultInfo AModularPlayerController::ValidatePlacement(const UBaseItemData& Item)
{
	const FStageEconomyResultInfo Result = CanPlaceItem(&Item);
	if (!Result.IsSuccess())
	{
		ReportRejection(Result);
	}
	return Result;
}

FStageEconomyResultInfo AModularPlayerController::EvaluatePlacementPreview(const UBaseItemData& Item)
{
	// Same rules as a commit, without reporting: hovering a locked item must not raise toasts or error cues.
	return CanPlaceItem(&Item);
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

	const FStageEconomyResultInfo Verdict = ValidatePlacement(*Item);
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

	// Interim feedback until a HUD toast binds OnRequestRejected. One key, so repeats replace each other.
	if (GEngine && IsLocalController())
	{
		GEngine->AddOnScreenDebugMessage(static_cast<uint64>(GetUniqueID()) + 1, 3.f, FColor(255, 170, 90), Result.Message.ToString());
	}
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
