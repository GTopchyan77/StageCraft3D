// Copyright Epic Games, Inc. All Rights Reserved.

#include "Player/StageCameraPawn.h"

#include "Components/SceneComponent.h"
#include "GameFramework/Controller.h"
#include "Player/StageCameraNavigation.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageCameraPawn)

namespace StageCameraPawn
{
	/** Below this a coasting flight is invisible; stopping there keeps the tick off at rest. */
	constexpr double VelocityRestTolerance = 0.5; // cm/s
}

AStageCameraPawn::AStageCameraPawn()
{
	PrimaryActorTick.bCanEverTick = true;
	// Off until a flight or a focus glide needs it: mouse input is applied the moment it arrives.
	PrimaryActorTick.bStartWithTickEnabled = false;

	CameraRoot = CreateDefaultSubobject<USceneComponent>(TEXT("CameraRoot"));
	CameraRoot->SetMobility(EComponentMobility::Movable);
	SetRootComponent(CameraRoot);

	// The view point is the actor origin; APawn's default eye height would offset it from where it flies.
	BaseEyeHeight = 0.f;

	// The view is the controller's control rotation; the actor itself never needs to turn.
	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;

	SetCanBeDamaged(false);
	CameraSpeed = StageCameraNavigation::DefaultCameraSpeed;
}

void AStageCameraPawn::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);

	// Fly keys are gathered during the controller's tick; ticking after it applies them in the same frame.
	if (NewController)
	{
		AddTickPrerequisiteActor(NewController);
	}
}

void AStageCameraPawn::UnPossessed()
{
	if (AController* OldController = GetController())
	{
		RemoveTickPrerequisiteActor(OldController);
	}
	Super::UnPossessed();
}

void AStageCameraPawn::StartTicking()
{
	if (!IsActorTickEnabled())
	{
		SetActorTickEnabled(true);
	}
}

FRotator AStageCameraPawn::GetCurrentViewRotation() const
{
	const AController* ViewController = GetController();
	const FRotator Rotation = ViewController ? ViewController->GetControlRotation() : GetActorRotation();
	if (bFixedView)
	{
		return Rotation; // Exact, so Top keeps its -90 pitch.
	}
	return FRotator(FMath::Clamp(FRotator::NormalizeAxis(Rotation.Pitch), -MaxPitch, MaxPitch), FRotator::NormalizeAxis(Rotation.Yaw), 0.0);
}

void AStageCameraPawn::ApplyViewRotation(const FRotator& NewRotation)
{
	AController* ViewController = GetController();
	if (!ViewController || NewRotation.ContainsNaN())
	{
		return;
	}
	ViewController->SetControlRotation(FRotator(FMath::Clamp(FRotator::NormalizeAxis(NewRotation.Pitch), -MaxPitch, MaxPitch),
		FRotator::NormalizeAxis(NewRotation.Yaw), 0.0));
}

float AStageCameraPawn::GetFlySpeed() const
{
	return StageCameraNavigation::GetFlySpeed(CameraSpeed);
}

float AStageCameraPawn::GetLookSpeedScale() const
{
	if (!bLinkLookToCameraSpeed)
	{
		return 1.f;
	}
	const float Ratio = GetFlySpeed() / FMath::Max(LookSpeedReferenceFlySpeed, 1.f);
	return FMath::Clamp(FMath::Pow(Ratio, LookSpeedScaleExponent), MinLookSpeedScale, FMath::Max(MinLookSpeedScale, MaxLookSpeedScale));
}

void AStageCameraPawn::AddLookInput(const FVector2D& MouseDelta)
{
	if (bFixedView || MouseDelta.IsZero() || MouseDelta.ContainsNaN())
	{
		return; // An orthographic view is defined by its axis; turning it would make it a different view.
	}
	const double DegreesPerCount = LookSensitivity * GetLookSpeedScale();
	const FRotator View = GetCurrentViewRotation();
	ApplyViewRotation(FRotator(View.Pitch + MouseDelta.Y * DegreesPerCount, View.Yaw + MouseDelta.X * DegreesPerCount, 0.0));
}

void AStageCameraPawn::AddFlyInput(const FVector& Input)
{
	if (Input.IsZero() || Input.ContainsNaN())
	{
		return;
	}
	FocusElapsed = -1.f; // Flying takes over from a focus glide.
	PendingFlyInput += Input;
	StartTicking();
}

void AStageCameraPawn::AddTranslation(const FVector& Offset)
{
	if (bFixedView || bOrbiting || Offset.IsZero() || Offset.ContainsNaN())
	{
		return;
	}
	FocusElapsed = -1.f; // A drag takes over from a focus glide, as in the editor.
	AddActorWorldOffset(Offset, /*bSweep*/ false);
}

void AStageCameraPawn::AddYaw(double YawDegrees)
{
	if (bFixedView || FMath::IsNearlyZero(YawDegrees) || !FMath::IsFinite(YawDegrees))
	{
		return;
	}
	const FRotator View = GetCurrentViewRotation();
	ApplyViewRotation(FRotator(View.Pitch, View.Yaw + YawDegrees, 0.0));
}

void AStageCameraPawn::BeginOrbit(const FVector& Pivot)
{
	if (bFixedView || Pivot.ContainsNaN())
	{
		return;
	}
	CancelGlides();
	// The camera keeps its place: the orbit starts on the sphere it is already on, and the view turns to face the pivot.
	const FVector ToPivot = Pivot - GetActorLocation();
	OrbitPivot = Pivot;
	OrbitDistance = FMath::Max(ToPivot.Size(), StageCameraNavigation::MinOrbitDistance);
	if (!ToPivot.IsNearlyZero())
	{
		ApplyViewRotation(ToPivot.Rotation());
	}
	bOrbiting = true;
	PlaceOnOrbit();
}

void AStageCameraPawn::AddOrbitInput(const FVector2D& MouseDelta)
{
	if (!bOrbiting || MouseDelta.IsZero() || MouseDelta.ContainsNaN())
	{
		return;
	}
	ApplyViewRotation(StageCameraNavigation::GetOrbitRotation(GetCurrentViewRotation(), MouseDelta));
	PlaceOnOrbit();
}

void AStageCameraPawn::AddOrbitDolly(const FVector2D& MouseDelta)
{
	if (!bOrbiting || MouseDelta.IsZero() || MouseDelta.ContainsNaN())
	{
		return;
	}
	OrbitDistance = StageCameraNavigation::GetDollyDistance(OrbitDistance, MouseDelta, CameraSpeed);
	PlaceOnOrbit();
}

void AStageCameraPawn::AddOrbitPan(const FVector& Offset)
{
	if (!bOrbiting || Offset.IsZero() || Offset.ContainsNaN())
	{
		return;
	}
	OrbitPivot += Offset;
	PlaceOnOrbit();
}

void AStageCameraPawn::PlaceOnOrbit()
{
	// From the rotation just written, so the pivot stays exactly at the view centre.
	SetActorLocation(StageCameraNavigation::GetOrbitLocation(OrbitPivot, GetCurrentViewRotation(), OrbitDistance), /*bSweep*/ false);
}

void AStageCameraPawn::EndOrbit()
{
	bOrbiting = false;
}

void AStageCameraPawn::FocusOn(const FVector& NewLocation)
{
	if (bFixedView || NewLocation.ContainsNaN())
	{
		return;
	}
	EndOrbit();
	Velocity = FVector::ZeroVector;
	FocusStart = GetActorLocation();
	FocusTarget = NewLocation;
	FocusElapsed = 0.f;
	StartTicking();
}

void AStageCameraPawn::SetCameraSpeed(float NewCameraSpeed)
{
	const float Sanitized = StageCameraNavigation::SanitizeCameraSpeed(NewCameraSpeed);
	if (Sanitized != CameraSpeed)
	{
		// Only the acceleration changes; a flight in progress speeds up or slows down through the integrator, never with a jerk.
		CameraSpeed = Sanitized;
		OnCameraSpeedChanged.Broadcast(CameraSpeed);
	}
}

float AStageCameraPawn::StepCameraSpeed(float Notches)
{
	SetCameraSpeed(StageCameraNavigation::StepCameraSpeed(CameraSpeed, Notches));
	return CameraSpeed;
}

void AStageCameraPawn::CancelGlides()
{
	Velocity = FVector::ZeroVector;
	PendingFlyInput = FVector::ZeroVector;
	FocusElapsed = -1.f;
}

void AStageCameraPawn::EnterFixedView(const FRotator& ViewRotation)
{
	EndOrbit();
	CancelGlides();
	bFixedView = true;
	if (AController* ViewController = GetController())
	{
		ViewController->SetControlRotation(FRotator(FRotator::NormalizeAxis(ViewRotation.Pitch), FRotator::NormalizeAxis(ViewRotation.Yaw), 0.0));
	}
}

void AStageCameraPawn::ExitFixedView(const FRotator& RestoreRotation)
{
	if (!bFixedView)
	{
		return;
	}
	bFixedView = false;
	CancelGlides();
	SetViewRotation(RestoreRotation);
}

void AStageCameraPawn::TeleportView(const FVector& NewLocation)
{
	EndOrbit();
	CancelGlides();
	SetActorLocation(NewLocation, /*bSweep*/ false, nullptr, ETeleportType::TeleportPhysics);
}

void AStageCameraPawn::SetViewRotation(FRotator NewRotation)
{
	if (bFixedView)
	{
		return; // Held by the fixed view until ExitFixedView.
	}
	ApplyViewRotation(NewRotation);
}

void AStageCameraPawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// Both always run: a focus glide and a coasting flight each settle on their own.
	const bool bFlying = TickFlight(DeltaSeconds);
	const bool bFocusing = TickFocus(DeltaSeconds);
	if (!bFlying && !bFocusing)
	{
		SetActorTickEnabled(false); // At rest: no per-frame work until the next fly key or focus.
	}
}

bool AStageCameraPawn::TickFlight(float DeltaSeconds)
{
	using namespace StageCameraPawn;

	// One impulse per held direction, as the editor counts keys; two bindings of one direction do not fly twice as fast.
	const FVector Input(FMath::Clamp(PendingFlyInput.X, -1.0, 1.0), FMath::Clamp(PendingFlyInput.Y, -1.0, 1.0), FMath::Clamp(PendingFlyInput.Z, -1.0, 1.0));
	PendingFlyInput = FVector::ZeroVector;

	FVector Impulse = FVector::ZeroVector;
	if (!Input.IsZero())
	{
		const FRotationMatrix ViewAxes(GetCurrentViewRotation());
		// A fixed (orthographic) view moves in its own plane: forward means up the screen, and there is no climbing.
		const FVector ForwardAxis = bFixedView ? ViewAxes.GetUnitAxis(EAxis::Z) : ViewAxes.GetUnitAxis(EAxis::X);
		Impulse = ForwardAxis * Input.X + ViewAxes.GetUnitAxis(EAxis::Y) * Input.Y + (bFixedView ? FVector::ZeroVector : FVector::UpVector * Input.Z);
	}
	if (Impulse.IsZero() && Velocity.Size() < VelocityRestTolerance)
	{
		Velocity = FVector::ZeroVector;
		return false;
	}

	Velocity = StageCameraNavigation::StepFlightVelocity(Velocity, Impulse, CameraSpeed, DeltaSeconds);
	const FVector Step = Velocity * FMath::Clamp(static_cast<double>(DeltaSeconds), 0.0, static_cast<double>(StageCameraNavigation::MaxFlightStepSeconds));
	if (!Step.IsZero())
	{
		AddActorWorldOffset(Step, /*bSweep*/ false);
	}
	return true;
}

bool AStageCameraPawn::TickFocus(float DeltaSeconds)
{
	if (FocusElapsed < 0.f)
	{
		return false;
	}
	FocusElapsed = FMath::Min(FocusElapsed + DeltaSeconds, StageCameraNavigation::FocusTransitionSeconds);
	const float Alpha = StageCameraNavigation::EaseFocus(FocusElapsed / StageCameraNavigation::FocusTransitionSeconds);
	SetActorLocation(FMath::Lerp(FocusStart, FocusTarget, static_cast<double>(Alpha)), /*bSweep*/ false);
	if (FocusElapsed >= StageCameraNavigation::FocusTransitionSeconds)
	{
		FocusElapsed = -1.f;
		return false;
	}
	return true;
}
