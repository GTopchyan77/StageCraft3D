// Copyright DamenTools internal. Spline Route Tool plugin.

#include "SplineRoutePlayerController.h"
#include "SplineFollowerVessel.h"
#include "SplineRouteAuthoringSubsystem.h"
#include "SplineRouteTool.h"

#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "EngineUtils.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PawnMovementComponent.h"

namespace
{
	// Logs only — on-screen messages disabled. Color/Duration kept in the signature so callers don't change.
	static void PCScreenMsg(const FColor& /*Color*/, float /*Duration*/, const FString& Msg)
	{
		UE_LOG(LogSplineRoute, Log, TEXT("%s"), *Msg);
	}
}

ASplineRoutePlayerController::ASplineRoutePlayerController()
{
	bShowMouseCursor = true;
	bEnableClickEvents = true;
	bEnableMouseOverEvents = true;
	DefaultMouseCursor = EMouseCursor::Crosshairs;
}

void ASplineRoutePlayerController::BeginPlay()
{
	Super::BeginPlay();

	if (ULocalPlayer* LocalPlayer = GetLocalPlayer())
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>())
		{
			if (InputMappingContext)
			{
				Subsystem->AddMappingContext(InputMappingContext, 0);
				PCScreenMsg(FColor::Green, 6.0f, FString::Printf(TEXT("[SplineRoute] IMC bound: %s"), *InputMappingContext->GetName()));
			}
			else
			{
				PCScreenMsg(FColor::Red, 10.0f, TEXT("[SplineRoute] ERROR: no InputMappingContext assigned on PC!"));
			}
		}
		else
		{
			PCScreenMsg(FColor::Red, 10.0f, TEXT("[SplineRoute] ERROR: EnhancedInputLocalPlayerSubsystem not available."));
		}
	}
	else
	{
		PCScreenMsg(FColor::Red, 10.0f, TEXT("[SplineRoute] ERROR: no LocalPlayer in BeginPlay."));
	}

	// Make sure UI/world input both work — UI Only would block the world trace under the cursor.
	FInputModeGameAndUI InputMode;
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	InputMode.SetHideCursorDuringCapture(false);
	SetInputMode(InputMode);

	// Panning (PanForward/Right/UpAction) drives the pawn via direct position deltas (see PanPawn),
	// not AddMovementInput, so UFloatingPawnMovement's acceleration/deceleration model is unused —
	// disable it outright rather than just raising MaxSpeed, so ADefaultPawn's own legacy WASD
	// bindings (which still call AddMovementInput) can't sneak in a second, physics-coasting source
	// of movement alongside ours.
	if (APawn* P = GetPawn())
	{
		if (UPawnMovementComponent* MC = P->GetMovementComponent())
		{
			MC->Velocity = FVector::ZeroVector;
			MC->SetComponentTickEnabled(false);
		}
	}

	PCScreenMsg(FColor::Cyan, 6.0f, TEXT("[SplineRoute] PlayerController ready — F to place, LMB to select, hold Left Shift + drag to move, Delete/RMB to remove, HUD arrows to cycle."));
}

void ASplineRoutePlayerController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (ULocalPlayer* LocalPlayer = GetLocalPlayer())
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>())
		{
			if (InputMappingContext)
			{
				Subsystem->RemoveMappingContext(InputMappingContext);
			}
		}
	}
	Super::EndPlay(EndPlayReason);
}

void ASplineRoutePlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	if (UEnhancedInputComponent* EIC = Cast<UEnhancedInputComponent>(InputComponent))
	{
		int32 Bound = 0;
		if (PlaceAction)
		{
			EIC->BindAction(PlaceAction, ETriggerEvent::Started, this, &ASplineRoutePlayerController::OnPlacePressed);
			++Bound;
		}
		else { UE_LOG(LogSplineRoute, Warning, TEXT("[SplineRoute] PlaceAction is null on PlayerController.")); }
		if (SelectAction)
		{
			EIC->BindAction(SelectAction, ETriggerEvent::Started, this, &ASplineRoutePlayerController::OnSelectPressed);
			++Bound;
		}
		else { UE_LOG(LogSplineRoute, Warning, TEXT("[SplineRoute] SelectAction is null on PlayerController.")); }
		if (ReleaseAction)
		{
			EIC->BindAction(ReleaseAction, ETriggerEvent::Completed, this, &ASplineRoutePlayerController::OnReleased);
			EIC->BindAction(ReleaseAction, ETriggerEvent::Canceled, this, &ASplineRoutePlayerController::OnReleased);
			++Bound;
		}
		else { UE_LOG(LogSplineRoute, Warning, TEXT("[SplineRoute] ReleaseAction is null on PlayerController.")); }
		if (DragAction)
		{
			EIC->BindAction(DragAction, ETriggerEvent::Triggered, this, &ASplineRoutePlayerController::OnDragTriggered);
			++Bound;
		}
		else { UE_LOG(LogSplineRoute, Warning, TEXT("[SplineRoute] DragAction is null on PlayerController.")); }
		if (DeleteAction)
		{
			EIC->BindAction(DeleteAction, ETriggerEvent::Started, this, &ASplineRoutePlayerController::OnDeletePressed);
			++Bound;
		}
		else { UE_LOG(LogSplineRoute, Warning, TEXT("[SplineRoute] DeleteAction is null on PlayerController.")); }
		if (ZoomAction)
		{
			EIC->BindAction(ZoomAction, ETriggerEvent::Triggered, this, &ASplineRoutePlayerController::OnZoomTriggered);
			++Bound;
		}
		else { UE_LOG(LogSplineRoute, Warning, TEXT("[SplineRoute] ZoomAction is null on PlayerController.")); }
		if (CycleVesselCameraAction)
		{
			EIC->BindAction(CycleVesselCameraAction, ETriggerEvent::Started, this, &ASplineRoutePlayerController::OnCycleVesselCameraPressed);
			++Bound;
		}
		else { UE_LOG(LogSplineRoute, Warning, TEXT("[SplineRoute] CycleVesselCameraAction is null on PlayerController.")); }
		if (PanForwardAction)
		{
			EIC->BindAction(PanForwardAction, ETriggerEvent::Triggered, this, &ASplineRoutePlayerController::OnPanForwardTriggered);
			++Bound;
		}
		else { UE_LOG(LogSplineRoute, Warning, TEXT("[SplineRoute] PanForwardAction is null on PlayerController.")); }
		if (PanRightAction)
		{
			EIC->BindAction(PanRightAction, ETriggerEvent::Triggered, this, &ASplineRoutePlayerController::OnPanRightTriggered);
			++Bound;
		}
		else { UE_LOG(LogSplineRoute, Warning, TEXT("[SplineRoute] PanRightAction is null on PlayerController.")); }
		if (PanUpAction)
		{
			EIC->BindAction(PanUpAction, ETriggerEvent::Triggered, this, &ASplineRoutePlayerController::OnPanUpTriggered);
			++Bound;
		}
		else { UE_LOG(LogSplineRoute, Warning, TEXT("[SplineRoute] PanUpAction is null on PlayerController.")); }

		UE_LOG(LogSplineRoute, Log, TEXT("[SplineRoute] Bound %d of 10 input actions."), Bound);
	}
	else
	{
		UE_LOG(LogSplineRoute, Error, TEXT("[SplineRoute] InputComponent is not a UEnhancedInputComponent. Actions not bound."));
	}
}

void ASplineRoutePlayerController::OnPlacePressed(const FInputActionValue& /*Value*/)
{
	PCScreenMsg(FColor::Yellow, 1.5f, TEXT("[SplineRoute] PLACE pressed"));
	if (UWorld* World = GetWorld())
	{
		if (USplineRouteAuthoringSubsystem* Sub = World->GetSubsystem<USplineRouteAuthoringSubsystem>())
		{
			// F now means "place" only (with a built-in over-handle guard). Selection lives on LMB.
			Sub->HandlePlaceAtCursor(this);
		}
		else { PCScreenMsg(FColor::Red, 5.0f, TEXT("[SplineRoute] Authoring subsystem missing.")); }
	}
}

void ASplineRoutePlayerController::OnSelectPressed(const FInputActionValue& /*Value*/)
{
	if (UWorld* World = GetWorld())
	{
		if (USplineRouteAuthoringSubsystem* Sub = World->GetSubsystem<USplineRouteAuthoringSubsystem>())
		{
			// LMB selects the nearest waypoint — selection only. Movement is Spacebar-gated (OnDragTriggered).
			Sub->HandleSelectAtCursor(this);
		}
	}
}

void ASplineRoutePlayerController::OnReleased(const FInputActionValue& /*Value*/)
{
	UE_LOG(LogSplineRoute, Log, TEXT("[SplineRoute] RELEASE"));
	if (UWorld* World = GetWorld())
	{
		if (USplineRouteAuthoringSubsystem* Sub = World->GetSubsystem<USplineRouteAuthoringSubsystem>())
		{
			Sub->HandleReleaseDrag();
		}
	}
}

void ASplineRoutePlayerController::OnDragTriggered(const FInputActionValue& /*Value*/)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	USplineRouteAuthoringSubsystem* Sub = World->GetSubsystem<USplineRouteAuthoringSubsystem>();
	if (!Sub)
	{
		return;
	}

	// Move ONLY while the Left Shift modifier is held. Plain LMB-drag must not move waypoints —
	// that conflation ("drag and select were the same") is exactly what operators complained about.
	// NB: NOT Spacebar — the DefaultPawn binds Space to "move camera up", so it would double-fire.
	if (IsInputKeyDown(EKeys::LeftShift))
	{
		Sub->HandleDrag(this);
	}
	else
	{
		// Modifier released mid-hold: stop any in-progress drag so the point freezes where it is.
		Sub->HandleReleaseDrag();
	}
}

void ASplineRoutePlayerController::OnDeletePressed(const FInputActionValue& /*Value*/)
{
	PCScreenMsg(FColor::Orange, 1.5f, TEXT("[SplineRoute] DELETE pressed"));
	if (UWorld* World = GetWorld())
	{
		if (USplineRouteAuthoringSubsystem* Sub = World->GetSubsystem<USplineRouteAuthoringSubsystem>())
		{
			Sub->HandleDeleteSelected();
		}
	}
}

void ASplineRoutePlayerController::OnZoomTriggered(const FInputActionValue& Value)
{
	if (APawn* P = GetPawn())
	{
		if (APlayerCameraManager* CM = PlayerCameraManager)
		{
			// Move along camera view direction = true perspective zoom
			FVector CameraForward = CM->GetCameraRotation().Vector();
			FVector ZoomDelta = CameraForward * Value.Get<float>() * ZoomSpeed;
			FVector NewLocation = P->GetActorLocation() + ZoomDelta;
			P->SetActorLocation(NewLocation);
		}
	}
}

void ASplineRoutePlayerController::OnPanForwardTriggered(const FInputActionValue& Value)
{
	// Full control rotation (pitch included), matching ADefaultPawn::MoveForward's own convention.
	PanPawn(GetControlRotation().Vector(), Value.Get<float>());
}

void ASplineRoutePlayerController::OnPanRightTriggered(const FInputActionValue& Value)
{
	const FVector Right = FRotationMatrix(GetControlRotation()).GetScaledAxis(EAxis::Y);
	PanPawn(Right, Value.Get<float>());
}

void ASplineRoutePlayerController::OnPanUpTriggered(const FInputActionValue& Value)
{
	PanPawn(FVector::UpVector, Value.Get<float>());
}

void ASplineRoutePlayerController::OnCycleVesselCameraPressed(const FInputActionValue& /*Value*/)
{
	UWorld* World = GetWorld();
	USplineRouteAuthoringSubsystem* Sub = World ? World->GetSubsystem<USplineRouteAuthoringSubsystem>() : nullptr;
	if (!Sub)
	{
		return;
	}

	// Click-to-select only matters when there's a real choice to make. With exactly one vessel in the
	// world, requiring a click first is pure friction — use it directly. With 2+, ambiguity is real,
	// so fall back to whichever one was clicked (GetSelectedVessel).
	ASplineFollowerVessel* OnlyVessel = nullptr;
	int32 VesselCount = 0;
	for (TActorIterator<ASplineFollowerVessel> It(World); It; ++It)
	{
		OnlyVessel = *It;
		if (++VesselCount > 1)
		{
			break; // Already ambiguous — no need to keep counting.
		}
	}

	ASplineFollowerVessel* Vessel = nullptr;
	if (VesselCount == 1)
	{
		Vessel = OnlyVessel;
	}
	else if (VesselCount > 1)
	{
		Vessel = Sub->GetSelectedVessel();
		if (!Vessel)
		{
			UE_LOG(LogSplineRoute, Warning, TEXT("[SplineRoute] CYCLE CAMERA pressed but no vessel selected — click a vessel first (multiple vessels exist)."));
			return;
		}
	}
	else
	{
		UE_LOG(LogSplineRoute, Warning, TEXT("[SplineRoute] CYCLE CAMERA pressed but no vessel has been spawned yet."));
		return;
	}

	// A newer vessel was spawned since cycling last ran (or this is the very first press): clean up
	// whatever the previous vessel had active and start this vessel's cycle fresh at slot 0, rather than
	// resuming a stale index that may skip slots or point at a destroyed vessel.
	if (Vessel != CyclingVessel.Get())
	{
		if (ASplineFollowerVessel* PrevVessel = CyclingVessel.Get())
		{
			PrevVessel->ActivateCameraSlot(-1);
		}
		CachedFreeCameraPawn = GetPawn();
		CyclingVessel = Vessel;
		CurrentCameraSlotIndex = -1;
	}

	++CurrentCameraSlotIndex;

	if (CurrentCameraSlotIndex >= Vessel->GetNumCameraSlots())
	{
		// Wrapped past the last slot: back to the free/authoring camera. NB: ASplineFollowerVessel is a
		// plain AActor, never possessed, so GetPawn() never changed during cycling — only the view target
		// did. Pan/zoom/waypoint input (all driven off GetPawn(), not the view target) never lost focus
		// and needs no restoring here.
		CurrentCameraSlotIndex = -1;
		CyclingVessel = nullptr;
		Vessel->ActivateCameraSlot(-1);
		if (APawn* FreeCam = CachedFreeCameraPawn.Get())
		{
			SetViewTargetWithBlend(FreeCam, Vessel->CameraBlendTime, Vessel->CameraBlendFunction, Vessel->CameraBlendExp);
		}
	}
	else
	{
		Vessel->ActivateCameraSlot(CurrentCameraSlotIndex);
		SetViewTargetWithBlend(Vessel, Vessel->CameraBlendTime, Vessel->CameraBlendFunction, Vessel->CameraBlendExp);
	}
}

void ASplineRoutePlayerController::PanPawn(const FVector& Direction, float AxisValue)
{
	if (FMath::IsNearlyZero(AxisValue))
	{
		return;
	}
	APawn* P = GetPawn();
	UWorld* World = GetWorld();
	if (!P || !World)
	{
		return;
	}

	// Direct position delta, not AddMovementInput — constant PanSpeed, zero acceleration ramp,
	// stops the instant AxisValue returns to 0 (i.e. the instant the key is released).
	P->AddActorWorldOffset(Direction.GetSafeNormal() * AxisValue * PanSpeed * World->GetDeltaSeconds());
}
