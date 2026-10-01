// Copyright Epic Games, Inc. All Rights Reserved.

#include "Player/StageCameraPawn.h"

#include "Components/SceneComponent.h"
#include "GameFramework/Controller.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageCameraPawn)

namespace StageCameraPawn
{
	/** Fraction of the remaining gap closed this frame. Exponential, so the result is frame-rate independent. */
	float SmoothingAlpha(float Sharpness, float DeltaSeconds)
	{
		return Sharpness > 0.f ? 1.f - FMath::Exp(-Sharpness * DeltaSeconds) : 1.f;
	}

	// Below these the motion is invisible; snapping avoids an endless sub-pixel tail and per-frame work.
	constexpr double RotationRestToleranceDeg = 0.001;
	constexpr double VelocityRestTolerance = 0.5; // cm/s
}

AStageCameraPawn::AStageCameraPawn()
{
	PrimaryActorTick.bCanEverTick = true;

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
}

void AStageCameraPawn::UnPossessed()
{
	if (AController* OldController = GetController())
	{
		RemoveTickPrerequisiteActor(OldController);
	}
	Super::UnPossessed();
}

float AStageCameraPawn::GetLookSpeedScale() const
{
	if (!bScaleLookWithFlySpeed)
	{
		return 1.f;
	}

	const float Ratio = FlySpeed / FMath::Max(LookSpeedReferenceFlySpeed, 1.f);
	return FMath::Clamp(FMath::Pow(Ratio, LookSpeedScaleExponent), MinLookSpeedScale, FMath::Max(MinLookSpeedScale, MaxLookSpeedScale));
}

void AStageCameraPawn::AddLookInput(const FVector2D& MouseDelta)
{
	// Scales the target, not the easing: RotationSmoothing still glides the view toward it, so a
	// faster look is just as smooth, and a wheel notch mid-turn changes the rate without a jump.
	const float DegreesPerCount = LookSensitivity * GetLookSpeedScale();
	TargetRotation.Yaw = FRotator::NormalizeAxis(TargetRotation.Yaw + MouseDelta.X * DegreesPerCount);
	TargetRotation.Pitch = FMath::Clamp(TargetRotation.Pitch + MouseDelta.Y * DegreesPerCount, -MaxPitch, MaxPitch);
}

void AStageCameraPawn::AddFlyInput(const FVector& Input)
{
	PendingFlyInput += Input;
}

float AStageCameraPawn::AdjustFlySpeed(float Steps)
{
	const float NewSpeed = FMath::Clamp(FlySpeed * FMath::Pow(FlySpeedStepFactor, Steps), MinFlySpeed, MaxFlySpeed);
	if (NewSpeed != FlySpeed)
	{
		// Only the target speed changes; TickMovement eases the actual velocity toward it.
		FlySpeed = NewSpeed;
		OnFlySpeedChanged.Broadcast(FlySpeed);
	}
	return FlySpeed;
}

void AStageCameraPawn::SetViewRotation(FRotator NewRotation, bool bSnap)
{
	TargetRotation = FRotator(FMath::Clamp(FRotator::NormalizeAxis(NewRotation.Pitch), -MaxPitch, MaxPitch), FRotator::NormalizeAxis(NewRotation.Yaw), 0.0);
	if (bSnap)
	{
		CurrentRotation = TargetRotation;
		if (AController* ViewController = GetController())
		{
			ViewController->SetControlRotation(CurrentRotation);
		}
	}
	bRotationInitialized = true;
}

void AStageCameraPawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (AController* ViewController = GetController())
	{
		TickRotation(*ViewController, DeltaSeconds);
	}
	TickMovement(DeltaSeconds);
}

void AStageCameraPawn::TickRotation(AController& ViewController, float DeltaSeconds)
{
	using namespace StageCameraPawn;

	const FRotator ControlRotation = ViewController.GetControlRotation();
	const bool bAtRest = CurrentRotation.Equals(TargetRotation, RotationRestToleranceDeg);

	// Adopt the spawn rotation, and any rotation someone else set while we were idle (game mode
	// restart, a future "focus selection"), instead of fighting it.
	if (!bRotationInitialized || (bAtRest && !ControlRotation.Equals(CurrentRotation, RotationRestToleranceDeg)))
	{
		const FRotator Adopted(FMath::Clamp(FRotator::NormalizeAxis(ControlRotation.Pitch), -MaxPitch, MaxPitch), FRotator::NormalizeAxis(ControlRotation.Yaw), 0.0);
		TargetRotation = CurrentRotation = Adopted;
		bRotationInitialized = true;
		return;
	}

	if (bAtRest)
	{
		return;
	}

	// Shortest-path delta, so easing across the +/-180 yaw seam never spins the long way round.
	const FRotator Remaining = (TargetRotation - CurrentRotation).GetNormalized();
	const float Alpha = SmoothingAlpha(RotationSmoothing, DeltaSeconds);
	CurrentRotation = (CurrentRotation + Remaining * Alpha).GetNormalized();
	CurrentRotation.Roll = 0.0;

	if (CurrentRotation.Equals(TargetRotation, RotationRestToleranceDeg))
	{
		CurrentRotation = TargetRotation;
	}

	ViewController.SetControlRotation(CurrentRotation);
}

void AStageCameraPawn::TickMovement(float DeltaSeconds)
{
	using namespace StageCameraPawn;

	const FVector Input = PendingFlyInput;
	PendingFlyInput = FVector::ZeroVector;

	// Horizontal-plane input and vertical input are limited separately: holding E with W or A/D
	// never slows the climb, and the climb never slows the forward/strafe motion.
	const FRotationMatrix ViewAxes(CurrentRotation);
	FVector ViewMove = ViewAxes.GetUnitAxis(EAxis::X) * Input.X + ViewAxes.GetUnitAxis(EAxis::Y) * Input.Y;
	ViewMove = ViewMove.GetClampedToMaxSize(1.0);
	const double Climb = FMath::Clamp(Input.Z, -1.0, 1.0);

	const FVector TargetVelocity = (ViewMove + FVector::UpVector * Climb) * FlySpeed;

	if (TargetVelocity.IsZero() && Velocity.Size() < VelocityRestTolerance)
	{
		Velocity = FVector::ZeroVector;
		return; // Idle: no per-frame transform updates.
	}

	Velocity = FMath::Lerp(Velocity, TargetVelocity, SmoothingAlpha(MovementSmoothing, DeltaSeconds));
	AddActorWorldOffset(Velocity * DeltaSeconds, /*bSweep*/ false);
}
