// Copyright Epic Games, Inc. All Rights Reserved.

#include "Player/ModularPlayerController.h"

#include "Components/SpawnSystemComponent.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "Interaction/StageCraftCollision.h"
#include "ModularSceneBuilder.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(ModularPlayerController)

AModularPlayerController::AModularPlayerController()
	: PlacementTraceChannel(ECC_Visibility)
	, StageItemTraceChannel(StageCraftCollision::StageItemChannel)
{
	bShowMouseCursor = true;
	DefaultMouseCursor = EMouseCursor::Crosshairs;

	SpawnSystem = CreateDefaultSubobject<USpawnSystemComponent>(TEXT("SpawnSystem"));
}

void AModularPlayerController::BeginPlay()
{
	Super::BeginPlay();

	if (IsLocalController())
	{
		FInputModeGameAndUI InputMode;
		InputMode.SetHideCursorDuringCapture(false);
		InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		SetInputMode(InputMode);

		// The default pawn turns the camera on mouse movement while a button is held, which would
		// swing the view during every place/delete click. Camera navigation gets its own bindings later.
		SetIgnoreLookInput(true);
	}
}

void AModularPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	if (!EditorMappingContext || !PlaceAction || !DeleteAction)
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
	EnhancedInput->BindAction(PlaceAction, ETriggerEvent::Started, this, &ThisClass::HandlePlaceStarted);
	EnhancedInput->BindAction(PlaceAction, ETriggerEvent::Triggered, this, &ThisClass::HandlePlaceTriggered);
	EnhancedInput->BindAction(PlaceAction, ETriggerEvent::Completed, this, &ThisClass::HandlePlaceCompleted);
	EnhancedInput->BindAction(PlaceAction, ETriggerEvent::Canceled, this, &ThisClass::HandlePlaceCompleted);
	EnhancedInput->BindAction(DeleteAction, ETriggerEvent::Started, this, &ThisClass::HandleDeleteStarted);
}

void AModularPlayerController::BuildDefaultInputMapping()
{
	UE_LOG(LogStageCraft, Log, TEXT("%s: input assets not fully assigned, using built-in LMB/RMB mapping."), *GetName());

	if (!PlaceAction)
	{
		PlaceAction = NewObject<UInputAction>(this, TEXT("IA_Place_Default"));
	}
	if (!DeleteAction)
	{
		DeleteAction = NewObject<UInputAction>(this, TEXT("IA_Delete_Default"));
	}

	// A fresh context, because an assigned one cannot be trusted to map actions it was not authored with.
	EditorMappingContext = NewObject<UInputMappingContext>(this, TEXT("IMC_StageEditor_Default"));
	EditorMappingContext->MapKey(PlaceAction, EKeys::LeftMouseButton);
	EditorMappingContext->MapKey(DeleteAction, EKeys::RightMouseButton);
}

bool AModularPlayerController::GetPlacementHitUnderCursor(FHitResult& OutHit) const
{
	return GetHitResultUnderCursor(PlacementTraceChannel, false, OutHit);
}

bool AModularPlayerController::GetStageItemHitUnderCursor(FHitResult& OutHit) const
{
	return GetHitResultUnderCursor(StageItemTraceChannel, false, OutHit);
}

void AModularPlayerController::HandlePlaceStarted()
{
	FHitResult Hit;
	if (GetPlacementHitUnderCursor(Hit))
	{
		SpawnSystem->BeginPlacement(Hit);
	}
}

void AModularPlayerController::HandlePlaceTriggered()
{
	// Only continuous strokes need a per-frame trace; single placements and idle holds cost nothing.
	if (!SpawnSystem->IsPlacementStrokeActive())
	{
		return;
	}

	FHitResult Hit;
	if (GetPlacementHitUnderCursor(Hit))
	{
		SpawnSystem->UpdatePlacement(Hit);
	}
}

void AModularPlayerController::HandlePlaceCompleted()
{
	SpawnSystem->EndPlacement();
}

void AModularPlayerController::HandleDeleteStarted()
{
	FHitResult Hit;
	if (GetStageItemHitUnderCursor(Hit))
	{
		SpawnSystem->TryDeleteActor(Hit.GetActor());
	}
}
