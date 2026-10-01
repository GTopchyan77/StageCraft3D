// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "StageCameraPawn.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStageCameraFlySpeedChanged, float, NewFlySpeed);

/**
 * Editor-viewport fly camera. AModularPlayerController routes RMB-held input here; this pawn turns
 * it into motion that matches the Unreal Editor viewport:
 *  - look: yaw/pitch from mouse deltas, pitch clamped short of vertical so the view never flips;
 *  - W/S along the full look vector, A/D along the view's horizontal right vector;
 *  - E/Q strictly along world Z, on their own axis so vertical speed never depends on pitch or on
 *    what else is held;
 *  - the mouse wheel steps fly speed, which scales every axis (forward/back, strafe, up/down) alike, and look
 *    speed follows it on a square-root curve (see GetLookSpeedScale).
 *
 * Both rotation and velocity are critically damped toward their targets with frame-rate-independent
 * exponential smoothing, so starts and stops are soft and identical at 30 or 240 fps. Raw mouse
 * deltas arrive in uneven per-frame bursts; smoothing the view (not the input) hides that jitter.
 *
 * There is deliberately no movement component and no collision: motion is integrated directly,
 * so nothing (speed clamps, input normalisation, sweeps) can distort it. The camera passes through
 * stage geometry like the editor camera.
 */
UCLASS()
class MODULARSCENEBUILDER_API AStageCameraPawn : public APawn
{
	GENERATED_BODY()

public:
	AStageCameraPawn();

	/** Mouse delta in raw counts (X = yaw right, Y = pitch up). Scaled by LookSensitivity * GetLookSpeedScale(). */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Camera")
	void AddLookInput(const FVector2D& MouseDelta);

	/** Fly intent for this frame, each axis in [-1, 1]: X = forward, Y = right, Z = world up. Cleared every tick. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Camera")
	void AddFlyInput(const FVector& Input);

	/**
	 * Multiplies fly speed by FlySpeedStepFactor^Steps (mouse wheel notches), clamped to
	 * [MinFlySpeed, MaxFlySpeed]. Geometric steps feel even across the whole range. The change reaches
	 * the camera through the normal velocity easing, so adjusting mid-flight never jerks. Returns the new speed.
	 */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Camera")
	float AdjustFlySpeed(float Steps);

	/** Points the view somewhere new, either instantly or through the normal rotation smoothing. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Camera")
	void SetViewRotation(FRotator NewRotation, bool bSnap = false);

	UFUNCTION(BlueprintPure, Category = "StageCraft|Camera")
	float GetFlySpeed() const { return FlySpeed; }

	/**
	 * Multiplier on LookSensitivity derived from the current fly speed, so looking around speeds up
	 * with the wheel together with flying: (FlySpeed / LookSpeedReferenceFlySpeed)^LookSpeedScaleExponent,
	 * clamped to [MinLookSpeedScale, MaxLookSpeedScale]. 1 when bScaleLookWithFlySpeed is off.
	 */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Camera")
	float GetLookSpeedScale() const;

	/** Fired whenever fly speed actually changes, e.g. for a HUD speed readout. */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Camera")
	FOnStageCameraFlySpeedChanged OnFlySpeedChanged;

	//~ Begin AActor Interface
	virtual void Tick(float DeltaSeconds) override;
	//~ End AActor Interface

	//~ Begin APawn Interface
	virtual void PossessedBy(class AController* NewController) override;
	virtual void UnPossessed() override;
	//~ End APawn Interface

protected:
	/** Degrees of rotation per raw mouse count at the reference fly speed (before GetLookSpeedScale). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StageCraft|Camera|Look", meta = (ClampMin = "0.001"))
	float LookSensitivity = 0.2f;

	/**
	 * Ties look speed to fly speed, so a fast flight across the stage also turns fast and a slow
	 * close-up gets finer aim. (The stock Unreal Editor keeps look speed fixed; turn this off for that.)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StageCraft|Camera|Look")
	bool bScaleLookWithFlySpeed = true;

	/** Fly speed at which look runs at exactly LookSensitivity (the default FlySpeed). */
	UPROPERTY(EditAnywhere, Category = "StageCraft|Camera|Look", meta = (EditCondition = "bScaleLookWithFlySpeed", ClampMin = "1.0", Units = "cm/s"))
	float LookSpeedReferenceFlySpeed = 1200.f;

	/**
	 * How strongly look follows fly speed. 1 would be strictly proportional, which over the 2000x fly
	 * range is unusable (frozen at 10 cm/s, a full spin per twitch at 200 m/s). 0.5 (square root):
	 * each doubling of fly speed turns ~1.41x faster, so look clearly tracks the wheel and stays controllable.
	 */
	UPROPERTY(EditAnywhere, Category = "StageCraft|Camera|Look", meta = (EditCondition = "bScaleLookWithFlySpeed", ClampMin = "0.0", ClampMax = "1.0"))
	float LookSpeedScaleExponent = 0.5f;

	/** Floor for slow close-up work: reached at and below 300 cm/s with the defaults. */
	UPROPERTY(EditAnywhere, Category = "StageCraft|Camera|Look", meta = (EditCondition = "bScaleLookWithFlySpeed", ClampMin = "0.05"))
	float MinLookSpeedScale = 0.5f;

	/** Ceiling so very fast flight never turns faster than a hand can aim: reached at 10,800 cm/s with the defaults. */
	UPROPERTY(EditAnywhere, Category = "StageCraft|Camera|Look", meta = (EditCondition = "bScaleLookWithFlySpeed", ClampMin = "1.0"))
	float MaxLookSpeedScale = 3.f;

	/**
	 * How quickly the view catches up with the mouse, per second. Around 25 gives ~40 ms of glide:
	 * enough to hide raw-input bursts while still feeling 1:1. Zero disables smoothing.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StageCraft|Camera|Look", meta = (ClampMin = "0.0"))
	float RotationSmoothing = 25.f;

	/** Stops short of straight up/down: at exactly 90 degrees yaw becomes undefined and the view flips. */
	UPROPERTY(EditAnywhere, Category = "StageCraft|Camera|Look", meta = (ClampMin = "0.0", ClampMax = "89.9", Units = "deg"))
	float MaxPitch = 89.f;

	/** Applies uniformly to every fly axis. Changed at runtime by the mouse wheel. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StageCraft|Camera|Fly", meta = (ClampMin = "1.0", Units = "cm/s"))
	float FlySpeed = 1200.f;

	/** Slow enough to nudge the view around a single truss clamp. */
	UPROPERTY(EditAnywhere, Category = "StageCraft|Camera|Fly", meta = (ClampMin = "1.0", Units = "cm/s"))
	float MinFlySpeed = 10.f;

	/** Fast enough to cross an arena-sized layout in a couple of seconds. */
	UPROPERTY(EditAnywhere, Category = "StageCraft|Camera|Fly", meta = (ClampMin = "1.0", Units = "cm/s"))
	float MaxFlySpeed = 20000.f;

	/** Speed multiplier per mouse-wheel notch. At 1.25 the full 10 cm/s to 200 m/s range is about 34 notches. */
	UPROPERTY(EditAnywhere, Category = "StageCraft|Camera|Fly", meta = (ClampMin = "1.01"))
	float FlySpeedStepFactor = 1.25f;

	/**
	 * How quickly velocity reaches its target, per second (acceleration and braking alike). Around 8
	 * gives a ~0.12 s ease in/out; higher is snappier. Zero means instant start/stop.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StageCraft|Camera|Fly", meta = (ClampMin = "0.0"))
	float MovementSmoothing = 8.f;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<class USceneComponent> CameraRoot = nullptr;

private:
	void TickRotation(class AController& ViewController, float DeltaSeconds);
	void TickMovement(float DeltaSeconds);

	/** Where the mouse has asked the view to point; the rendered view eases toward it. */
	FRotator TargetRotation = FRotator::ZeroRotator;
	FRotator CurrentRotation = FRotator::ZeroRotator;
	bool bRotationInitialized = false;

	FVector PendingFlyInput = FVector::ZeroVector;
	FVector Velocity = FVector::ZeroVector;
};
