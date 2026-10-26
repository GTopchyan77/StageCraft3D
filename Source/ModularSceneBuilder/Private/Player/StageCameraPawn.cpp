// Copyright Epic Games, Inc. All Rights Reserved.

#include "Player/StageCameraPawn.h"

#include "Components/SceneComponent.h"
#include "GameFramework/Controller.h"
#include "Player/StageCameraNavigation.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageCameraPawn)

namespace StageCameraPawn
{
	/** Fraction of the remaining gap closed this frame. Exponential, so the result is frame-rate independent. */
	float SmoothingAlpha(float Sharpness, float DeltaSeconds)
	{
		return Sharpness > 0.f ? 1.f - FMath::Exp(-Sharpness * DeltaSeconds) : 1.f;
	}

	// Below these the motion is invisible; snapping avoids an endless sub-pixel tail and keeps the tick off at rest.
	constexpr double RotationRestToleranceDeg = 0.001;
	constexpr double VelocityRestTolerance = 0.5; // cm/s
	constexpr double TranslationRestTolerance = 0.01; // cm
}

AStageCameraPawn::AStageCameraPawn()
{
	PrimaryActorTick.bCanEverTick = true;
	// On for the first frames only: it adopts the spawn rotation, then switches itself off until input arrives.
	PrimaryActorTick.bStartWithTickEnabled = true;

	CameraRoot = CreateDefaultSubobject<USceneComponent>(TEXT("CameraRoot"));
	CameraRoot->SetMobility(EComponentMobility::Movable);
	SetRootComponent(CameraRoot);

	// The view point is the actor origin; APawn's default eye height would offset it from where it flies.
	BaseEyeHeight = 0.f;

	// Rotation is applied to the controller in TickRotation; the actor itself never needs to turn.
	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;

	SetCanBeDamaged(false);
	CameraSpeed = StageCameraNavigation::DefaultCameraSpeed;
}

void AStageCameraPawn::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);

	// Input is gathered during the controller's tick; ticking after it applies that input in the same frame.
	if (NewController)
	{
		AddTickPrerequisiteActor(NewController);
	}
	bRotationInitialized = false;
	SetActorTickEnabled(true);
}

void AStageCameraPawn::UnPossessed()
{
	if (AController* OldController = GetController())
	{
		RemoveTickPrerequisiteActor(OldController);
	}
	Super::UnPossessed();
}

void AStageCameraPawn::Wake()
{
	if (IsActorTickEnabled())
	{
		return;
	}
	if (const AController* ViewController = GetController())
	{
		AdoptControlRotation(*ViewController);
	}
	SetActorTickEnabled(true);
}

void AStageCameraPawn::AdoptControlRotation(const AController& ViewController)
{
	using namespace StageCameraPawn;
	if (bFixedView)
	{
		return; // Set exactly once by EnterFixedView; adopting the control rotation would clamp Top's -90 pitch.
	}
	// The spawn rotation, or one someone else set while the camera was idle (a game mode restart): take it instead of fighting it.
	const FRotator ControlRotation = ViewController.GetControlRotation();
	const bool bAtRest = CurrentRotation.Equals(TargetRotation, RotationRestToleranceDeg);
	if (!bRotationInitialized || (bAtRest && !ControlRotation.Equals(CurrentRotation, RotationRestToleranceDeg)))
	{
		TargetRotation = CurrentRotation = FRotator(FMath::Clamp(FRotator::NormalizeAxis(ControlRotation.Pitch), -MaxPitch, MaxPitch),
			FRotator::NormalizeAxis(ControlRotation.Yaw), 0.0);
		bRotationInitialized = true;
	}
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
	if (bFixedView || MouseDelta.IsZero())
	{
		return; // An orthographic view is defined by its axis; turning it would make it a different view.
	}
	Wake();
	// Scales the target, not the easing: RotationSmoothing still glides the view toward it.
	const float DegreesPerCount = LookSensitivity * GetLookSpeedScale();
	TargetRotation.Yaw = FRotator::NormalizeAxis(TargetRotation.Yaw + MouseDelta.X * DegreesPerCount);
	TargetRotation.Pitch = FMath::Clamp(TargetRotation.Pitch + MouseDelta.Y * DegreesPerCount, -MaxPitch, MaxPitch);
}

void AStageCameraPawn::AddFlyInput(const FVector& Input)
{
	if (Input.IsZero())
	{
		return;
	}
	Wake();
	PendingFlyInput += Input;
}

void AStageCameraPawn::AddTranslation(const FVector& Offset)
{
	if (bFixedView || bOrbiting || Offset.IsZero() || Offset.ContainsNaN())
	{
		return;
	}
	Wake();
	FocusElapsed = -1.f; // A drag takes over from a focus glide, as in the editor.
	PendingTranslation += Offset;
}

void AStageCameraPawn::AddYaw(double YawDegrees)
{
	if (bFixedView || FMath::IsNearlyZero(YawDegrees) || !FMath::IsFinite(YawDegrees))
	{
		return;
	}
	Wake();
	TargetRotation.Yaw = FRotator::NormalizeAxis(TargetRotation.Yaw + YawDegrees);
}

void AStageCameraPawn::BeginOrbit(const FVector& Pivot)
{
	if (bFixedView || Pivot.ContainsNaN())
	{
		return;
	}
	Wake();
	CancelGlides();
	// The camera keeps its place: the orbit starts on the sphere it is already on, and the view turns to face the pivot.
	const FVector ToPivot = Pivot - GetActorLocation();
	OrbitPivot = Pivot;
	OrbitDistance = TargetOrbitDistance = FMath::Max(ToPivot.Size(), StageCameraNavigation::MinOrbitDistance);
	if (!ToPivot.IsNearlyZero())
	{
		const FRotator Facing = ToPivot.Rotation();
		TargetRotation = FRotator(FMath::Clamp(FRotator::NormalizeAxis(Facing.Pitch), -MaxPitch, MaxPitch), FRotator::NormalizeAxis(Facing.Yaw), 0.0);
	}
	bOrbiting = true;
}

void AStageCameraPawn::AddOrbitInput(const FVector2D& MouseDelta)
{
	if (!bOrbiting || MouseDelta.IsZero())
	{
		return;
	}
	Wake();
	const FRotator Orbited = StageCameraNavigation::GetOrbitRotation(TargetRotation, MouseDelta);
	TargetRotation = FRotator(FMath::Clamp(Orbited.Pitch, -MaxPitch, MaxPitch), Orbited.Yaw, 0.0);
}

void AStageCameraPawn::AddOrbitDolly(const FVector2D& MouseDelta)
{
	if (!bOrbiting || MouseDelta.IsZero())
	{
		return;
	}
	Wake();
	TargetOrbitDistance = StageCameraNavigation::GetDollyDistance(TargetOrbitDistance, MouseDelta, CameraSpeed);
}

void AStageCameraPawn::AddOrbitPan(const FVector& Offset)
{
	if (!bOrbiting || Offset.IsZero() || Offset.ContainsNaN())
	{
		return;
	}
	Wake();
	PendingTranslation += Offset;
}

void AStageCameraPawn::EndOrbit()
{
	if (!bOrbiting)
	{
		return;
	}
	bOrbiting = false;
	// Whatever pivot movement is still pending is dropped with the pivot.
	PendingTranslation = FVector::ZeroVector;
}

void AStageCameraPawn::FocusOn(const FVector& NewLocation)
{
	if (bFixedView || NewLocation.ContainsNaN())
	{
		return;
	}
	Wake();
	EndOrbit();
	Velocity = FVector::ZeroVector;
	PendingTranslation = FVector::ZeroVector;
	FocusStart = GetActorLocation();
	FocusTarget = NewLocation;
	FocusElapsed = 0.f;
}

void AStageCameraPawn::SetCameraSpeed(float NewCameraSpeed)
{
	const float Sanitized = StageCameraNavigation::SanitizeCameraSpeed(NewCameraSpeed);
	if (Sanitized != CameraSpeed)
	{
		// Only the target speed changes; TickMovement eases a flight's velocity toward it, so changing mid-flight never jerks.
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
	PendingTranslation = FVector::ZeroVector;
	FocusElapsed = -1.f;
}

void AStageCameraPawn::EnterFixedView(const FRotator& ViewRotation)
{
	EndOrbit();
	CancelGlides();
	bFixedView = true;
	bRotationInitialized = true;
	TargetRotation = CurrentRotation = FRotator(FRotator::NormalizeAxis(ViewRotation.Pitch), FRotator::NormalizeAxis(ViewRotation.Yaw), 0.0);
	if (AController* ViewController = GetController())
	{
		ViewController->SetControlRotation(CurrentRotation);
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
	SetViewRotation(RestoreRotation, /*bSnap*/ true);
}

void AStageCameraPawn::TeleportView(const FVector& NewLocation)
{
	EndOrbit();
	CancelGlides();
	SetActorLocation(NewLocation, /*bSweep*/ false, nullptr, ETeleportType::TeleportPhysics);
}

void AStageCameraPawn::SetViewRotation(FRotator NewRotation, bool bSnap)
{
	if (bFixedView)
	{
		return; // Held by the fixed view until ExitFixedView.
	}
	TargetRotation = FRotator(FMath::Clamp(FRotator::NormalizeAxis(NewRotation.Pitch), -MaxPitch, MaxPitch), FRotator::NormalizeAxis(NewRotation.Yaw), 0.0);
	bRotationInitialized = true;
	if (bSnap)
	{
		CurrentRotation = TargetRotation;
		if (AController* ViewController = GetController())
		{
			ViewController->SetControlRotation(CurrentRotation);
		}
		return;
	}
	Wake();
}

void AStageCameraPawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	AController* ViewController = GetController();
	if (ViewController && !bRotationInitialized)
	{
		AdoptControlRotation(*ViewController);
	}

	const bool bTurning = ViewController && TickRotation(*ViewController, DeltaSeconds);
	// Orbiting places the camera from the (eased) rotation; otherwise flight, drags and focus move it.
	const bool bMoving = bOrbiting ? TickOrbit(DeltaSeconds) : (TickFocus(DeltaSeconds) | TickMovement(DeltaSeconds));

	if (!bTurning && !bMoving && PendingFlyInput.IsZero())
	{
		SetActorTickEnabled(false); // At rest: no per-frame work until the next input wakes it.
	}
}

bool AStageCameraPawn::TickRotation(AController& ViewController, float DeltaSeconds)
{
	using namespace StageCameraPawn;

	if (bFixedView || CurrentRotation.Equals(TargetRotation, RotationRestToleranceDeg))
	{
		return false;
	}

	// Shortest-path delta, so easing across the +/-180 yaw seam never spins the long way round.
	const FRotator Remaining = (TargetRotation - CurrentRotation).GetNormalized();
	CurrentRotation = (CurrentRotation + Remaining * SmoothingAlpha(RotationSmoothing, DeltaSeconds)).GetNormalized();
	CurrentRotation.Roll = 0.0;
	if (CurrentRotation.Equals(TargetRotation, RotationRestToleranceDeg))
	{
		CurrentRotation = TargetRotation;
	}
	ViewController.SetControlRotation(CurrentRotation);
	return true;
}

bool AStageCameraPawn::TickMovement(float DeltaSeconds)
{
	using namespace StageCameraPawn;

	const FVector Input = PendingFlyInput;
	PendingFlyInput = FVector::ZeroVector;

	// Horizontal-plane input and vertical input are limited separately: holding E with W or A/D never slows the climb.
	const FRotationMatrix ViewAxes(CurrentRotation);
	// A fixed (orthographic) view moves in its own plane: forward means up the screen.
	const FVector ForwardAxis = bFixedView ? ViewAxes.GetUnitAxis(EAxis::Z) : ViewAxes.GetUnitAxis(EAxis::X);
	const FVector ViewMove = (ForwardAxis * Input.X + ViewAxes.GetUnitAxis(EAxis::Y) * Input.Y).GetClampedToMaxSize(1.0);
	const double Climb = bFixedView ? 0.0 : FMath::Clamp(Input.Z, -1.0, 1.0);
	const FVector TargetVelocity = (ViewMove + FVector::UpVector * Climb) * GetFlySpeed();

	FVector Step = FVector::ZeroVector;
	bool bActive = false;
	if (!TargetVelocity.IsZero() || Velocity.Size() >= VelocityRestTolerance)
	{
		Velocity = FMath::Lerp(Velocity, TargetVelocity, SmoothingAlpha(MovementSmoothing, DeltaSeconds));
		Step += Velocity * DeltaSeconds;
		bActive = true;
	}
	else
	{
		Velocity = FVector::ZeroVector;
	}

	if (!PendingTranslation.IsZero())
	{
		const FVector Taken = PendingTranslation.Size() <= TranslationRestTolerance
			? PendingTranslation : PendingTranslation * SmoothingAlpha(TranslationSmoothing, DeltaSeconds);
		PendingTranslation -= Taken;
		Step += Taken;
		bActive = true;
	}

	if (!Step.IsZero())
	{
		AddActorWorldOffset(Step, /*bSweep*/ false);
	}
	return bActive;
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

bool AStageCameraPawn::TickOrbit(float DeltaSeconds)
{
	using namespace StageCameraPawn;
	bool bActive = false;

	if (!PendingTranslation.IsZero())
	{
		const FVector Taken = PendingTranslation.Size() <= TranslationRestTolerance
			? PendingTranslation : PendingTranslation * SmoothingAlpha(TranslationSmoothing, DeltaSeconds);
		PendingTranslation -= Taken;
		OrbitPivot += Taken;
		bActive = true;
	}
	if (!FMath::IsNearlyEqual(OrbitDistance, TargetOrbitDistance, TranslationRestTolerance))
	{
		OrbitDistance = FMath::Lerp(OrbitDistance, TargetOrbitDistance, static_cast<double>(SmoothingAlpha(TranslationSmoothing, DeltaSeconds)));
		bActive = true;
	}
	else
	{
		OrbitDistance = TargetOrbitDistance;
	}

	// Placed from the rendered (eased) rotation, so the pivot stays exactly at the view centre while the turn glides.
	const FVector Location = StageCameraNavigation::GetOrbitLocation(OrbitPivot, CurrentRotation, OrbitDistance);
	if (!Location.Equals(GetActorLocation(), TranslationRestTolerance))
	{
		SetActorLocation(Location, /*bSweep*/ false);
		bActive = true;
	}
	return bActive;
}
