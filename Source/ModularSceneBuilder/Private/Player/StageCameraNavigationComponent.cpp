// Copyright Epic Games, Inc. All Rights Reserved.

#include "Player/StageCameraNavigationComponent.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/SelectionComponent.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Input/StageInputPreferencesSubsystem.h"
#include "ModularSceneBuilder.h"
#include "Player/ModularPlayerController.h"
#include "Player/StageCameraPawn.h"
#include "UnrealClient.h"
#include "Viewport/StageViewportComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageCameraNavigationComponent)

namespace StageCameraNavigationComponent
{
	/** A camera this close to where a focus left it has not been moved since. */
	constexpr double PivotReuseTolerance = 1.0;
}

UStageCameraNavigationComponent::UStageCameraNavigationComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UStageCameraNavigationComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	CancelAll();
	Super::EndPlay(EndPlayReason);
}

AStageCameraPawn* UStageCameraNavigationComponent::GetCameraPawn() const
{
	const APlayerController* Controller = Cast<APlayerController>(GetOwner());
	return Controller ? Cast<AStageCameraPawn>(Controller->GetPawn()) : nullptr;
}

UStageViewportComponent* UStageCameraNavigationComponent::GetViewport() const
{
	const AModularPlayerController* Controller = Cast<AModularPlayerController>(GetOwner());
	return Controller ? Controller->GetViewportView() : nullptr;
}

bool UStageCameraNavigationComponent::IsOrthographic() const
{
	const UStageViewportComponent* Viewport = GetViewport();
	return Viewport && Viewport->IsOrthographic();
}

bool UStageCameraNavigationComponent::IsAltDown() const
{
	// The viewport's key state is current the moment Slate delivers a key; the player input's only after its next tick.
	const UWorld* World = GetWorld();
	const UGameViewportClient* ViewportClient = World ? World->GetGameViewport() : nullptr;
	if (const FViewport* Viewport = ViewportClient ? ViewportClient->Viewport : nullptr)
	{
		return Viewport->KeyState(EKeys::LeftAlt) || Viewport->KeyState(EKeys::RightAlt);
	}
	const APlayerController* Controller = Cast<APlayerController>(GetOwner());
	return Controller && (Controller->IsInputKeyDown(EKeys::LeftAlt) || Controller->IsInputKeyDown(EKeys::RightAlt));
}

bool UStageCameraNavigationComponent::IsFlying() const
{
	return Held.bRight && !Held.bAlt;
}

bool UStageCameraNavigationComponent::ShouldHideCursorForPress(const FStageCameraButtons& Buttons, bool bLeftMayDrag) const
{
	if (IsOrthographic())
	{
		// Orthographic views pan with the right or middle button; a left press is a click with a visible cursor.
		return !Buttons.bLeft && (Buttons.bRight || Buttons.bMiddle);
	}
	if (Buttons.bAlt || Buttons.bMiddle || Buttons.bRight)
	{
		// A right press during a gizmo drag (left held, not a camera drag) must not hide the cursor the drag follows.
		return !(Buttons.bLeft && !bLeftMayDrag && !Buttons.bAlt);
	}
	return Buttons.bLeft && bLeftMayDrag;
}

void UStageCameraNavigationComponent::BeginButton(EStageCameraButton Button, bool bCanStartDrag)
{
	switch (Button)
	{
	case EStageCameraButton::Left:
	{
		const bool bAlt = IsAltDown() && !IsOrthographic();
		if (!bAlt && !bCanStartDrag)
		{
			return; // Not a camera button this time (placing, a gizmo handle, Ctrl+Click, an orthographic click).
		}
		Held.bLeft = true;
		// Alt+LMB orbits at once; a plain press waits to see whether it is a click.
		bLeftPending = !bAlt;
		LeftPendingTravel = FVector2D::ZeroVector;
		break;
	}
	case EStageCameraButton::Middle:
		Held.bMiddle = true;
		bLeftPending = false; // With a second button held the left press is part of a drag, never a click.
		break;
	case EStageCameraButton::Right:
		Held.bRight = true;
		bLeftPending = false;
		break;
	}
	UpdateDrag();
}

bool UStageCameraNavigationComponent::EndButton(EStageCameraButton Button)
{
	bool bWasClick = false;
	switch (Button)
	{
	case EStageCameraButton::Left:
		bWasClick = bLeftPending;
		bLeftPending = false;
		Held.bLeft = false;
		break;
	case EStageCameraButton::Middle:
		Held.bMiddle = false;
		break;
	case EStageCameraButton::Right:
		Held.bRight = false;
		break;
	}
	UpdateDrag();
	if (!Held.AnyButton())
	{
		StoreCameraSpeed();
	}
	return bWasClick;
}

void UStageCameraNavigationComponent::CancelAll()
{
	Held = FStageCameraButtons();
	bLeftPending = false;
	LeftPendingTravel = FVector2D::ZeroVector;
	ActiveDrag = EStageCameraDrag::None;
	StoreCameraSpeed();
}

void UStageCameraNavigationComponent::UpdateDrag()
{
	FStageCameraButtons Effective = Held;
	Effective.bAlt = Held.AnyButton() && IsAltDown();
	// A left press that has not travelled yet is not a drag: alone it does nothing, with the right button it waits too.
	if (bLeftPending)
	{
		Effective.bLeft = false;
	}
	const EStageCameraDrag NewDrag = IsOrthographic()
		? ((Effective.bRight || Effective.bMiddle) ? EStageCameraDrag::Pan : EStageCameraDrag::None)
		: StageCameraNavigation::ClassifyDrag(Effective);
	Held.bAlt = Effective.bAlt;

	if (NewDrag == ActiveDrag)
	{
		return;
	}
	AStageCameraPawn* Pawn = GetCameraPawn();
	const bool bOrbitMode = NewDrag == EStageCameraDrag::Orbit || NewDrag == EStageCameraDrag::OrbitDolly || NewDrag == EStageCameraDrag::OrbitPan;
	if (Pawn && !IsOrthographic())
	{
		if (bOrbitMode && !Pawn->IsOrbiting())
		{
			Pawn->BeginOrbit(FindOrbitPivot(*Pawn));
		}
		else if (!bOrbitMode && NewDrag != EStageCameraDrag::None)
		{
			// Leaving the orbit only when another kind of motion starts: on release it stays, so the eased turn still circles
			// the pivot and the next Alt-drag continues around the same point.
			Pawn->EndOrbit();
		}
	}
	ActiveDrag = NewDrag;
}

FVector UStageCameraNavigationComponent::FindOrbitPivot(const AStageCameraPawn& Pawn) const
{
	using namespace StageCameraNavigationComponent;
	const FVector Location = Pawn.GetActorLocation();
	if (RememberedPivot.IsSet() && FVector::Dist(Location, RememberedPivotCameraLocation) <= PivotReuseTolerance)
	{
		return RememberedPivot.GetValue();
	}

	// Whatever the view centre is looking at, as the editor's look-at point usually is.
	const FVector Forward = Pawn.GetTargetViewRotation().Vector();
	if (const UWorld* World = GetWorld())
	{
		FCollisionQueryParams Params(SCENE_QUERY_STAT(StageCameraOrbitPivot), /*bTraceComplex*/ false, &Pawn);
		FHitResult Hit;
		if (World->LineTraceSingleByChannel(Hit, Location, Location + Forward * PivotTraceDistance, ECC_Visibility, Params))
		{
			return Hit.ImpactPoint;
		}
	}
	return Location + Forward * StageCameraNavigation::DefaultPivotDistance;
}

void UStageCameraNavigationComponent::HandleMouseDelta(const FVector2D& Delta)
{
	if (Delta.IsZero())
	{
		return;
	}
	FVector2D Step = Delta;
	if (bLeftPending)
	{
		LeftPendingTravel += Delta;
		if (LeftPendingTravel.Size() < StageCameraNavigation::ClickDragThreshold)
		{
			return;
		}
		// The press became a drag: no click on release. All the travel so far is applied, so nothing is lost.
		bLeftPending = false;
		Step = LeftPendingTravel;
		UpdateDrag();
	}
	else if (Held.AnyButton() && Held.bAlt != IsAltDown())
	{
		UpdateDrag(); // Alt pressed or released mid-drag switches mode, as in the editor.
	}

	AStageCameraPawn* Pawn = GetCameraPawn();
	if (!Pawn || ActiveDrag == EStageCameraDrag::None)
	{
		return;
	}

	if (IsOrthographic())
	{
		if (UStageViewportComponent* Viewport = GetViewport())
		{
			Viewport->PanOrtho(Step);
		}
		return;
	}

	const FRotator View = Pawn->GetTargetViewRotation();
	const float Speed = Pawn->GetCameraSpeed();
	switch (ActiveDrag)
	{
	case EStageCameraDrag::FlyAndTurn:
		Pawn->AddTranslation(StageCameraNavigation::GetFlyAndTurnTranslation(Step, View.Yaw, Speed));
		Pawn->AddYaw(StageCameraNavigation::GetFlyAndTurnYaw(Step));
		break;
	case EStageCameraDrag::Look:
	{
		// The user's preferences scale and flip the look; they are read per event, never cached.
		FVector2D Look = Step;
		const AModularPlayerController* Controller = Cast<AModularPlayerController>(GetOwner());
		if (const UStageInputPreferencesSubsystem* Preferences = Controller ? ULocalPlayer::GetSubsystem<UStageInputPreferencesSubsystem>(Controller->GetLocalPlayer()) : nullptr)
		{
			Look *= Preferences->GetLookSensitivity();
			Look.Y *= Preferences->IsLookInverted() ? -1.0 : 1.0;
		}
		Pawn->AddLookInput(Look);
		break;
	}
	case EStageCameraDrag::Pan:
		Pawn->AddTranslation(StageCameraNavigation::GetPanTranslation(Step, View, Speed));
		break;
	case EStageCameraDrag::PanVertical:
		Pawn->AddTranslation(StageCameraNavigation::GetVerticalPanTranslation(Step, View.Yaw, Speed));
		break;
	case EStageCameraDrag::Orbit:
		Pawn->AddOrbitInput(Step);
		break;
	case EStageCameraDrag::OrbitDolly:
		Pawn->AddOrbitDolly(Step);
		break;
	case EStageCameraDrag::OrbitPan:
		Pawn->AddOrbitPan(StageCameraNavigation::GetPanTranslation(Step, View, Speed));
		break;
	case EStageCameraDrag::None:
	default:
		break;
	}
}

void UStageCameraNavigationComponent::HandleWheel(float Notches, const FVector2D& CursorPosition)
{
	AStageCameraPawn* Pawn = GetCameraPawn();
	if (!Pawn || FMath::IsNearlyZero(Notches) || !FMath::IsFinite(Notches))
	{
		return;
	}
	// Orthographic: the wheel zooms around the cursor (there is no depth to dolly through).
	if (UStageViewportComponent* Viewport = GetViewport(); Viewport && Viewport->IsOrthographic())
	{
		Viewport->ZoomOrtho(Notches, CursorPosition);
		return;
	}
	if (Held.bRight)
	{
		// RMB + wheel: camera speed, shown in the status bar (AStageCameraPawn::OnCameraSpeedChanged), stored on release.
		Pawn->StepCameraSpeed(Notches);
		return;
	}
	if (Pawn->IsOrbiting())
	{
		Pawn->EndOrbit();
	}
	Pawn->AddTranslation(StageCameraNavigation::GetWheelDolly(Pawn->GetTargetViewRotation(), Notches));
}

void UStageCameraNavigationComponent::HandleFlyInput(const FVector& Input)
{
	if (!IsFlying() || Input.IsZero())
	{
		return;
	}
	if (AStageCameraPawn* Pawn = GetCameraPawn())
	{
		if (Pawn->IsOrbiting())
		{
			Pawn->EndOrbit();
		}
		Pawn->AddFlyInput(Input);
	}
}

bool UStageCameraNavigationComponent::FocusSelection()
{
	const AModularPlayerController* Controller = Cast<AModularPlayerController>(GetOwner());
	const USelectionComponent* Selection = Controller ? Controller->GetSelection() : nullptr;
	if (!Selection || !Selection->HasSelection())
	{
		return false;
	}
	FBox Bounds(ForceInit);
	for (const AActor* Actor : Selection->GetSelectedActors())
	{
		if (IsValid(Actor))
		{
			// Colliding components only: a fixture's beam or a helper sphere would frame empty space.
			const FBox ActorBounds = Actor->GetComponentsBoundingBox(/*bNonColliding*/ false);
			Bounds += ActorBounds.IsValid ? ActorBounds : FBox(Actor->GetActorLocation(), Actor->GetActorLocation());
		}
	}
	return FocusBounds(Bounds);
}

bool UStageCameraNavigationComponent::FocusBounds(const FBox& Bounds)
{
	AStageCameraPawn* Pawn = GetCameraPawn();
	const APlayerController* Controller = Cast<APlayerController>(GetOwner());
	if (!Pawn || !Controller || !Bounds.IsValid || Bounds.ContainsNaN())
	{
		return false;
	}
	if (UStageViewportComponent* Viewport = GetViewport(); Viewport && Viewport->IsOrthographic())
	{
		return Viewport->FocusOrtho(Bounds);
	}

	int32 SizeX = 0;
	int32 SizeY = 0;
	Controller->GetViewportSize(SizeX, SizeY);
	const float Aspect = SizeY > 0 ? static_cast<float>(SizeX) / static_cast<float>(SizeY) : 16.f / 9.f;
	const float FOV = Controller->PlayerCameraManager ? Controller->PlayerCameraManager->GetFOVAngle() : 90.f;
	const FVector Target = StageCameraNavigation::GetFocusLocation(Bounds, Pawn->GetTargetViewRotation(), FOV, Aspect, Pawn->GetActorLocation());
	Pawn->FocusOn(Target);

	// The next Alt-drag orbits the framed items, as in the editor.
	RememberedPivot = Bounds.GetCenter();
	RememberedPivotCameraLocation = Target;
	UE_LOG(LogStageCraft, Verbose, TEXT("Camera focus: bounds %s, camera to %s."), *Bounds.ToString(), *Target.ToString());
	return true;
}

void UStageCameraNavigationComponent::StoreCameraSpeed()
{
	const AStageCameraPawn* Pawn = GetCameraPawn();
	const APlayerController* Controller = Cast<APlayerController>(GetOwner());
	UStageInputPreferencesSubsystem* Preferences = Controller ? ULocalPlayer::GetSubsystem<UStageInputPreferencesSubsystem>(Controller->GetLocalPlayer()) : nullptr;
	if (!Pawn || !Preferences || Pawn->GetCameraSpeed() == StoredCameraSpeed)
	{
		return;
	}
	StoredCameraSpeed = Pawn->GetCameraSpeed();
	Preferences->StoreCameraSpeed(StoredCameraSpeed);
}
