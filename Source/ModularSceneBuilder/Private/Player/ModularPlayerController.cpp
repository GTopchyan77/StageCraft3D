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
#include "InputAction.h"
#include "InputMappingContext.h"
#include "Interaction/StageCraftCollision.h"
#include "ModularSceneBuilder.h"
#include "Subsystems/StageItemSubsystem.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(ModularPlayerController)

AModularPlayerController::AModularPlayerController()
	: PlacementTraceChannel(ECC_Visibility)
	, StageItemTraceChannel(StageCraftCollision::StageItemChannel)
	, GizmoTraceChannel(StageCraftCollision::GizmoChannel)
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

	FInputModeGameAndUI InputMode;
	InputMode.SetHideCursorDuringCapture(false);
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	SetInputMode(InputMode);

	// The default pawn turns the camera on mouse movement while a button is held, which would
	// swing the view during every click. Camera navigation gets its own bindings later.
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

void AModularPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	if (!EditorMappingContext || !PlaceAction || !DeleteAction || !ToggleGizmoModeAction)
	{
		BuildDefaultInputMapping();
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
	EnhancedInput->BindAction(DeleteAction, ETriggerEvent::Started, this, &ThisClass::HandleDeleteStarted);
	EnhancedInput->BindAction(ToggleGizmoModeAction, ETriggerEvent::Started, this, &ThisClass::HandleToggleGizmoMode);
}

void AModularPlayerController::BuildDefaultInputMapping()
{
	UE_LOG(LogStageCraft, Log, TEXT("%s: input assets not fully assigned, using built-in LMB/RMB/Space mapping."), *GetName());

	if (!PlaceAction)
	{
		PlaceAction = NewObject<UInputAction>(this, TEXT("IA_Place_Default"));
	}
	if (!DeleteAction)
	{
		DeleteAction = NewObject<UInputAction>(this, TEXT("IA_Delete_Default"));
	}
	if (!ToggleGizmoModeAction)
	{
		ToggleGizmoModeAction = NewObject<UInputAction>(this, TEXT("IA_ToggleGizmoMode_Default"));
		// Stop Space from also driving the default pawn's legacy "fly up" axis while held.
		ToggleGizmoModeAction->bConsumesActionAndAxisMappings = true;
		ToggleGizmoModeAction->TriggerEventsThatConsumeLegacyKeys = static_cast<int32>(ETriggerEvent::Started | ETriggerEvent::Ongoing | ETriggerEvent::Triggered);
	}

	// A fresh context, because an assigned one cannot be trusted to map actions it was not authored with.
	EditorMappingContext = NewObject<UInputMappingContext>(this, TEXT("IMC_StageEditor_Default"));
	EditorMappingContext->MapKey(PlaceAction, EKeys::LeftMouseButton);
	EditorMappingContext->MapKey(DeleteAction, EKeys::RightMouseButton);
	EditorMappingContext->MapKey(ToggleGizmoModeAction, EKeys::SpaceBar);
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
	return GetHitResultUnderCursor(GizmoTraceChannel, false, OutHit);
}

bool AModularPlayerController::GetCursorRay(FVector& OutOrigin, FVector& OutDirection) const
{
	return DeprojectMousePositionToWorld(OutOrigin, OutDirection);
}

void AModularPlayerController::HandlePrimaryStarted()
{
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

void AModularPlayerController::HandleDeleteStarted()
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
