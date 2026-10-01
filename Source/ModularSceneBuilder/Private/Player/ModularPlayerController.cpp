// Copyright Epic Games, Inc. All Rights Reserved.

#include "Player/ModularPlayerController.h"

#include "Actors/ModularTransformGizmo.h"
#include "Blueprint/UserWidget.h"
#include "Components/SelectionComponent.h"
#include "Components/SpawnSystemComponent.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/FloatingPawnMovement.h"
#include "GameFramework/Pawn.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "Interaction/StageCraftCollision.h"
#include "ModularSceneBuilder.h"
#include "Subsystems/StageItemSubsystem.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(ModularPlayerController)

namespace ModularPlayerController
{
	// Matches the Unreal Editor viewport: a few degrees short of vertical so yaw stays well defined.
	constexpr double MaxLookPitch = 89.0;

	// Snappy start/stop like the editor camera, scaled with speed so every speed step feels the same.
	constexpr float FlyAccelerationPerSpeed = 8.f;

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
	GizmoClass = AModularTransformGizmo::StaticClass();
}

void AModularPlayerController::BeginPlay()
{
	Super::BeginPlay();

	if (!IsLocalController())
	{
		return;
	}

	ApplyEditorInputMode();

	// Engine look input (the default pawn's mouse bindings) stays off: rotation only ever comes from
	// fly navigation, which sets the control rotation directly.
	SetIgnoreLookInput(true);

	if (GizmoClass)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.Owner = this; // The gizmo reads the owner's camera to keep a constant screen size.
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Gizmo = GetWorld()->SpawnActor<AModularTransformGizmo>(GizmoClass, FTransform::Identity, SpawnParams);
	}

	Selection->OnSelectionChanged.AddDynamic(this, &ThisClass::HandleSelectionChanged);

	// Created after the selection binding so panels that read the current selection in NativeConstruct see a ready controller.
	if (HUDWidgetClass)
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
	if (bIsNavigatingCamera)
	{
		EndCameraNavigation();
	}

	Selection->OnSelectionChanged.RemoveDynamic(this, &ThisClass::HandleSelectionChanged);

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

void AModularPlayerController::ApplyEditorInputMode()
{
	FInputModeGameAndUI InputMode;
	InputMode.SetHideCursorDuringCapture(false);
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	SetInputMode(InputMode);
}

void AModularPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	if (!EditorMappingContext || !PlaceAction || !SecondaryAction || !ToggleGizmoModeAction)
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
	}

	UEnhancedInputComponent* EnhancedInput = Cast<UEnhancedInputComponent>(InputComponent);
	if (!EnhancedInput)
	{
		UE_LOG(LogStageCraft, Error, TEXT("%s requires UEnhancedInputComponent as the default input component class."), *GetName());
		return;
	}

	// With no explicit trigger, Started fires on press, Triggered every frame while held, Completed on release.
	EnhancedInput->BindAction(PlaceAction, ETriggerEvent::Started, this, &ThisClass::HandlePrimaryStarted);
	EnhancedInput->BindAction(PlaceAction, ETriggerEvent::Triggered, this, &ThisClass::HandlePrimaryTriggered);
	EnhancedInput->BindAction(PlaceAction, ETriggerEvent::Completed, this, &ThisClass::HandlePrimaryCompleted);
	EnhancedInput->BindAction(PlaceAction, ETriggerEvent::Canceled, this, &ThisClass::HandlePrimaryCompleted);
	EnhancedInput->BindAction(SecondaryAction, ETriggerEvent::Started, this, &ThisClass::HandleSecondaryStarted);
	EnhancedInput->BindAction(SecondaryAction, ETriggerEvent::Completed, this, &ThisClass::HandleSecondaryCompleted);
	EnhancedInput->BindAction(SecondaryAction, ETriggerEvent::Canceled, this, &ThisClass::HandleSecondaryCompleted);
	EnhancedInput->BindAction(ToggleGizmoModeAction, ETriggerEvent::Started, this, &ThisClass::HandleToggleGizmoMode);

	// Bound permanently; they only fire while CameraNavigationMappingContext is applied.
	EnhancedInput->BindAction(CameraLookAction, ETriggerEvent::Triggered, this, &ThisClass::HandleCameraLook);
	EnhancedInput->BindAction(CameraMoveAction, ETriggerEvent::Triggered, this, &ThisClass::HandleCameraMove);
	EnhancedInput->BindAction(CameraSpeedAction, ETriggerEvent::Triggered, this, &ThisClass::HandleCameraSpeed);
}

void AModularPlayerController::BuildDefaultInputMapping()
{
	UE_LOG(LogStageCraft, Log, TEXT("%s: input assets not fully assigned, using built-in LMB/RMB/Space mapping."), *GetName());

	if (!PlaceAction)
	{
		PlaceAction = NewObject<UInputAction>(this, TEXT("IA_Place_Default"));
	}
	if (!SecondaryAction)
	{
		SecondaryAction = NewObject<UInputAction>(this, TEXT("IA_Secondary_Default"));
	}
	if (!ToggleGizmoModeAction)
	{
		ToggleGizmoModeAction = NewObject<UInputAction>(this, TEXT("IA_ToggleGizmoMode_Default"));
		// Stop Space from also driving any legacy "fly up" axis mapping while held.
		ToggleGizmoModeAction->bConsumesActionAndAxisMappings = true;
		ToggleGizmoModeAction->TriggerEventsThatConsumeLegacyKeys = static_cast<int32>(ETriggerEvent::Started | ETriggerEvent::Ongoing | ETriggerEvent::Triggered);
	}

	// A fresh context, because an assigned one cannot be trusted to map actions it was not authored with.
	EditorMappingContext = NewObject<UInputMappingContext>(this, TEXT("IMC_StageEditor_Default"));
	EditorMappingContext->MapKey(PlaceAction, EKeys::LeftMouseButton);
	EditorMappingContext->MapKey(SecondaryAction, EKeys::RightMouseButton);
	EditorMappingContext->MapKey(ToggleGizmoModeAction, EKeys::SpaceBar);
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

bool AModularPlayerController::GetPlacementHitUnderCursor(FHitResult& OutHit) const
{
	return GetHitResultUnderCursor(PlacementTraceChannel, false, OutHit);
}

bool AModularPlayerController::GetStageItemHitUnderCursor(FHitResult& OutHit) const
{
	return GetHitResultUnderCursor(StageItemTraceChannel, false, OutHit);
}

bool AModularPlayerController::GetGizmoHitUnderCursor(FHitResult& OutHit) const
{
	FVector RayOrigin, RayDirection;
	return Gizmo
		&& GetCursorRay(RayOrigin, RayDirection)
		&& Gizmo->TraceHandles(RayOrigin, RayDirection, OutHit);
}

bool AModularPlayerController::GetCursorRay(FVector& OutOrigin, FVector& OutDirection) const
{
	return DeprojectMousePositionToWorld(OutOrigin, OutDirection);
}

void AModularPlayerController::HandlePrimaryStarted()
{
	// The cursor is hidden and frozen while flying, so a left click there has no meaningful target.
	if (bIsNavigatingCamera)
	{
		return;
	}

	if (TryBeginGizmoDrag())
	{
		return;
	}

	FHitResult ItemHit;
	if (GetStageItemHitUnderCursor(ItemHit) && Selection->SelectActor(ItemHit.GetActor()))
	{
		return;
	}

	Selection->ClearSelection();

	FHitResult PlacementHit;
	if (GetPlacementHitUnderCursor(PlacementHit))
	{
		SpawnSystem->BeginPlacement(PlacementHit);
	}
}

void AModularPlayerController::HandlePrimaryTriggered()
{
	// Per-frame work only while something is actually being dragged; an idle hold costs nothing.
	if (Gizmo && Gizmo->IsDragging())
	{
		FVector RayOrigin, RayDirection;
		if (GetCursorRay(RayOrigin, RayDirection))
		{
			Gizmo->UpdateDrag(RayOrigin, RayDirection);
		}
		return;
	}

	if (SpawnSystem->IsPlacementStrokeActive())
	{
		FHitResult Hit;
		if (GetPlacementHitUnderCursor(Hit))
		{
			SpawnSystem->UpdatePlacement(Hit);
		}
	}
}

void AModularPlayerController::HandlePrimaryCompleted()
{
	if (Gizmo)
	{
		Gizmo->EndDrag();
	}
	SpawnSystem->EndPlacement();
}

void AModularPlayerController::HandleSecondaryStarted()
{
	// Moving the view mid-drag would drag the target or paint along the camera path, not the user's intent.
	const bool bPrimaryBusy = (Gizmo && Gizmo->IsDragging()) || SpawnSystem->IsPlacementStrokeActive();
	if (bIsNavigatingCamera || bPrimaryBusy)
	{
		return;
	}

	// Click vs. hold is only known on release, so every press starts navigating, as in the Unreal Editor.
	BeginCameraNavigation();
}

void AModularPlayerController::HandleSecondaryCompleted()
{
	if (!bIsNavigatingCamera)
	{
		return; // The press was ignored (primary drag in progress).
	}

	const double HeldSeconds = GetWorld()->GetRealTimeSeconds() - NavigationStartTime;
	const bool bWasClick = !bNavigationUsedFlyKeys
		&& NavigationMouseTravel <= SecondaryClickDragThreshold
		&& HeldSeconds <= MaxSecondaryClickDuration;

	// Restores the cursor first, so the click traces from where the user pressed.
	EndCameraNavigation();

	if (bWasClick)
	{
		HandleSecondaryClick();
	}
}

void AModularPlayerController::HandleSecondaryClick()
{
	FHitResult Hit;
	if (GetStageItemHitUnderCursor(Hit) && SpawnSystem->TryDeleteActor(Hit.GetActor()))
	{
		return; // Selection releases itself through the actor's OnDestroyed.
	}

	// Right-click on empty space backs out of everything: actor selection and the armed catalog item.
	Selection->ClearSelection();

	if (UStageItemSubsystem* ItemSubsystem = GetGameInstance() ? GetGameInstance()->GetSubsystem<UStageItemSubsystem>() : nullptr)
	{
		ItemSubsystem->ClearSelection();
	}
}

void AModularPlayerController::BeginCameraNavigation()
{
	bIsNavigatingCamera = true;
	bNavigationUsedFlyKeys = false;
	NavigationMouseTravel = 0.f;
	NavigationStartTime = GetWorld()->GetRealTimeSeconds();

	float CursorX = 0.f, CursorY = 0.f;
	bHasNavigationCursorPosition = GetMousePosition(CursorX, CursorY);
	NavigationCursorPosition = FVector2D(CursorX, CursorY);

	// Game-only mode switches Slate to high-precision (raw) mouse input with the cursor locked, so the
	// view keeps turning when the cursor would otherwise hit the screen edge. The button-down already
	// gave the viewport mouse capture; game-only keeps it until the release restores the editor mode.
	bShowMouseCursor = false;
	SetInputMode(FInputModeGameOnly());

	if (UEnhancedInputLocalPlayerSubsystem* InputSubsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()))
	{
		// Keys already held when RMB goes down (e.g. W first, then RMB) should fly immediately.
		FModifyContextOptions Options;
		Options.bIgnoreAllPressedKeysUntilRelease = false;
		InputSubsystem->AddMappingContext(CameraNavigationMappingContext, CameraNavigationContextPriority, Options);
	}

	ApplyFlySpeed();
}

void AModularPlayerController::EndCameraNavigation()
{
	bIsNavigatingCamera = false;

	if (UEnhancedInputLocalPlayerSubsystem* InputSubsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()))
	{
		InputSubsystem->RemoveMappingContext(CameraNavigationMappingContext);
	}

	bShowMouseCursor = true;
	ApplyEditorInputMode();

	// Raw-input mode does not reliably restore the OS cursor, so put it back explicitly.
	if (bHasNavigationCursorPosition)
	{
		SetMouseLocation(FMath::RoundToInt(NavigationCursorPosition.X), FMath::RoundToInt(NavigationCursorPosition.Y));
	}
}

void AModularPlayerController::HandleCameraLook(const FInputActionValue& Value)
{
	if (!bIsNavigatingCamera)
	{
		return;
	}

	const FVector2D Delta = Value.Get<FVector2D>();
	NavigationMouseTravel += static_cast<float>(Delta.Size());

	FRotator Rotation = GetControlRotation();
	Rotation.Yaw = FRotator::NormalizeAxis(Rotation.Yaw + Delta.X * LookSensitivity);
	Rotation.Pitch = FMath::ClampAngle(Rotation.Pitch + Delta.Y * LookSensitivity, -ModularPlayerController::MaxLookPitch, ModularPlayerController::MaxLookPitch);
	Rotation.Roll = 0.0;
	SetControlRotation(Rotation);
}

void AModularPlayerController::HandleCameraMove(const FInputActionValue& Value)
{
	APawn* CameraPawn = GetPawn();
	if (!bIsNavigatingCamera || !CameraPawn)
	{
		return;
	}

	const FVector Input = Value.Get<FVector>();
	if (Input.IsNearlyZero())
	{
		return;
	}
	bNavigationUsedFlyKeys = true;

	// Forward follows the full view direction (pitch included) and up is world Z, as in the editor viewport.
	const FRotationMatrix ViewAxes(GetControlRotation());
	CameraPawn->AddMovementInput(ViewAxes.GetScaledAxis(EAxis::X), Input.X);
	CameraPawn->AddMovementInput(ViewAxes.GetScaledAxis(EAxis::Y), Input.Y);
	CameraPawn->AddMovementInput(FVector::UpVector, Input.Z);
}

void AModularPlayerController::HandleCameraSpeed(const FInputActionValue& Value)
{
	const float Steps = Value.Get<float>();
	if (!bIsNavigatingCamera || FMath::IsNearlyZero(Steps))
	{
		return;
	}

	FlySpeed = FMath::Clamp(FlySpeed * FMath::Pow(FlySpeedStepFactor, Steps), MinFlySpeed, MaxFlySpeed);
	ApplyFlySpeed();
}

void AModularPlayerController::ApplyFlySpeed() const
{
	const APawn* CameraPawn = GetPawn();
	UFloatingPawnMovement* Movement = CameraPawn ? Cast<UFloatingPawnMovement>(CameraPawn->GetMovementComponent()) : nullptr;
	if (!Movement)
	{
		return; // A custom pawn with its own movement keeps its own speed.
	}

	Movement->MaxSpeed = FlySpeed;
	Movement->Acceleration = FlySpeed * ModularPlayerController::FlyAccelerationPerSpeed;
	Movement->Deceleration = FlySpeed * ModularPlayerController::FlyAccelerationPerSpeed;
}

void AModularPlayerController::HandleToggleGizmoMode()
{
	if (Gizmo)
	{
		Gizmo->ToggleMode();
	}
}

bool AModularPlayerController::TryBeginGizmoDrag()
{
	if (!Gizmo || !Gizmo->GetTarget())
	{
		return false;
	}

	FHitResult GizmoHit;
	FVector RayOrigin, RayDirection;
	return GetGizmoHitUnderCursor(GizmoHit)
		&& GetCursorRay(RayOrigin, RayDirection)
		&& Gizmo->TryBeginDrag(GizmoHit, RayOrigin, RayDirection);
}
