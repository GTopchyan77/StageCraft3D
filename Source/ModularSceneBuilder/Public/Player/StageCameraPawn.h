// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "StageCameraPawn.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStageCameraSpeedChanged, float, NewCameraSpeed);

/**
 * Editor-viewport camera. UStageCameraNavigationComponent turns mouse and key input into calls here, with the Unreal Editor's
 * rules (StageCameraNavigation, Docs/ADR/0018 §2); this pawn turns them into smooth motion:
 *  - look: yaw/pitch, pitch clamped short of vertical so the view never flips;
 *  - flying (fly keys while RMB is held): W/S along the full look vector, A/D along the view's horizontal right, E/Q strictly
 *    along world Z on their own axis, all at the camera speed's fly rate (2000 cm/s per unit, as in the editor);
 *  - drags and the wheel dolly: world translations, eased in over a few frames so uneven mouse bursts never judder;
 *  - orbit: the view turns about a pivot and the camera stays on the sphere around it; dolly changes the distance;
 *  - focus: a 0.25 s cubic-out glide to a framing location (the editor's FocusViewportOnBox transition).
 *
 * Rotation and flight velocity are eased with frame-rate-independent exponential smoothing, identical at 30 or 240 fps.
 * There is deliberately no movement component and no collision: the camera passes through stage geometry like the editor's.
 *
 * Tick: on only while something moves. Every input wakes it; it switches itself off again once rotation, velocity, drag
 * translation and focus have all settled, so an idle camera costs nothing per frame.
 */
UCLASS()
class MODULARSCENEBUILDER_API AStageCameraPawn : public APawn
{
	GENERATED_BODY()

public:
	AStageCameraPawn();

	/** Mouse delta in raw counts (X = yaw right, Y = pitch up), already multiplied by the user's look preferences. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Camera")
	void AddLookInput(const FVector2D& MouseDelta);

	/** Fly intent for this frame, each axis in [-1, 1]: X = forward, Y = right, Z = world up. Cleared every tick. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Camera")
	void AddFlyInput(const FVector& Input);

	/** A world translation (drag, wheel dolly), applied over the next few frames. Ignored in a fixed view and while orbiting. */
	void AddTranslation(const FVector& Offset);

	/** Turns the view by YawDegrees (the left-button drag). */
	void AddYaw(double YawDegrees);

	/**
	 * Starts orbiting Pivot from where the camera is now (Alt + drag). The distance is the camera's distance to the pivot, at
	 * least StageCameraNavigation::MinOrbitDistance. Stops any flight and focus glide.
	 */
	void BeginOrbit(const FVector& Pivot);

	/** Orbit by a mouse delta (Alt + LMB). No effect unless orbiting. */
	void AddOrbitInput(const FVector2D& MouseDelta);

	/** Dolly toward / away from the pivot (Alt + RMB). No effect unless orbiting. */
	void AddOrbitDolly(const FVector2D& MouseDelta);

	/** Moves the pivot and the camera together (Alt + MMB). No effect unless orbiting. */
	void AddOrbitPan(const FVector& Offset);

	/** Back to free flight; the camera keeps where the orbit left it. Idempotent. */
	void EndOrbit();

	bool IsOrbiting() const { return bOrbiting; }
	const FVector& GetOrbitPivot() const { return OrbitPivot; }

	/** Glides to NewLocation in StageCameraNavigation::FocusTransitionSeconds, keeping the view direction. */
	void FocusOn(const FVector& NewLocation);

	/** Camera speed (editor units, 1 = 20 m/s flight), clamped. Broadcasts OnCameraSpeedChanged when it changes. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Camera")
	void SetCameraSpeed(float NewCameraSpeed);

	/** RMB + wheel: StageCameraNavigation::StepCameraSpeed. Returns the new speed. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Camera")
	float StepCameraSpeed(float Notches);

	UFUNCTION(BlueprintPure, Category = "StageCraft|Camera")
	float GetCameraSpeed() const { return CameraSpeed; }

	/** Settled flight speed in cm/s at the current camera speed. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Camera")
	float GetFlySpeed() const;

	/**
	 * Look speed follows camera speed (STATE.md #15): (FlySpeed / LookSpeedReferenceFlySpeed)^LookSpeedScaleExponent, clamped
	 * to [MinLookSpeedScale, MaxLookSpeedScale]. Off by default, as in the Unreal Editor (a Preferences option).
	 */
	void SetLookLinkedToCameraSpeed(bool bLinked) { bLinkLookToCameraSpeed = bLinked; }

	/** 1 unless look is linked to camera speed. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Camera")
	float GetLookSpeedScale() const;

	/** Points the view somewhere new, either instantly or through the normal rotation smoothing. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Camera")
	void SetViewRotation(FRotator NewRotation, bool bSnap = false);

	/**
	 * Fixed view for orthographic projections (Docs/ADR/0008 §4.1): the view snaps to ViewRotation exactly (no pitch limit, so
	 * Top can look straight down), look and orbit input are ignored, and fly input moves in the view plane (forward = screen up,
	 * right = screen right, up/down ignored). Calling it again switches to another fixed rotation.
	 */
	void EnterFixedView(const FRotator& ViewRotation);

	/** Leaves the fixed view and points the view at RestoreRotation instantly (pitch limited as usual). Idempotent. */
	void ExitFixedView(const FRotator& RestoreRotation);

	bool IsFixedView() const { return bFixedView; }

	/** Moves the camera instantly (projection switches, orthographic pan and zoom) and stops any glide, orbit or flight. */
	void TeleportView(const FVector& NewLocation);

	/** The rotation the view is easing toward (what the camera will show once settled). */
	const FRotator& GetTargetViewRotation() const { return TargetRotation; }

	/** True while the pawn is integrating motion (its tick is on). */
	bool IsMoving() const { return IsActorTickEnabled(); }

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Camera")
	FOnStageCameraSpeedChanged OnCameraSpeedChanged;

	//~ Begin AActor Interface
	virtual void Tick(float DeltaSeconds) override;
	//~ End AActor Interface

	//~ Begin APawn Interface
	virtual void PossessedBy(class AController* NewController) override;
	virtual void UnPossessed() override;
	//~ End APawn Interface

protected:
	/** Degrees of rotation per raw mouse count (the editor's MouseSensitivty), before the user's look sensitivity. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "StageCraft|Camera|Look", meta = (ClampMin = "0.001"))
	float LookSensitivity = 0.2f;

	/** Fly speed at which a linked look runs at exactly LookSensitivity. */
	UPROPERTY(EditAnywhere, Category = "StageCraft|Camera|Look", meta = (ClampMin = "1.0", Units = "cm/s"))
	float LookSpeedReferenceFlySpeed = 1200.f;

	/** How strongly a linked look follows fly speed; 0.5 (square root) keeps it controllable over the whole range. */
	UPROPERTY(EditAnywhere, Category = "StageCraft|Camera|Look", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float LookSpeedScaleExponent = 0.5f;

	UPROPERTY(EditAnywhere, Category = "StageCraft|Camera|Look", meta = (ClampMin = "0.05"))
	float MinLookSpeedScale = 0.5f;

	UPROPERTY(EditAnywhere, Category = "StageCraft|Camera|Look", meta = (ClampMin = "1.0"))
	float MaxLookSpeedScale = 3.f;

	/**
	 * How quickly the view catches up with the mouse, per second. Around 25 gives ~40 ms of glide: enough to hide raw-input
	 * bursts while still feeling 1:1. Zero disables smoothing.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "StageCraft|Camera|Look", meta = (ClampMin = "0.0"))
	float RotationSmoothing = 25.f;

	/** Stops short of straight up/down: at exactly 90 degrees yaw becomes undefined and the view flips. */
	UPROPERTY(EditAnywhere, Category = "StageCraft|Camera|Look", meta = (ClampMin = "0.0", ClampMax = "89.9", Units = "deg"))
	float MaxPitch = 89.f;

	/**
	 * How quickly flight velocity reaches its target, per second (acceleration and braking alike). Around 8 gives a ~0.12 s
	 * ease in/out, close to the editor's damping of 10. Zero means instant start/stop.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "StageCraft|Camera|Fly", meta = (ClampMin = "0.0"))
	float MovementSmoothing = 8.f;

	/** How quickly drag and wheel translations are taken up, per second; like RotationSmoothing for motion. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "StageCraft|Camera|Fly", meta = (ClampMin = "0.0"))
	float TranslationSmoothing = 25.f;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<class USceneComponent> CameraRoot = nullptr;

private:
	/** Switches the tick on. When it was off, first adopts a rotation someone else set meanwhile, so input never fights it. */
	void Wake();
	void AdoptControlRotation(const class AController& ViewController);

	/** Each returns true while it still has motion to apply. */
	bool TickRotation(class AController& ViewController, float DeltaSeconds);
	bool TickMovement(float DeltaSeconds);
	bool TickFocus(float DeltaSeconds);
	bool TickOrbit(float DeltaSeconds);

	void CancelGlides();

	/** Where the mouse has asked the view to point; the rendered view eases toward it. */
	FRotator TargetRotation = FRotator::ZeroRotator;
	FRotator CurrentRotation = FRotator::ZeroRotator;
	bool bRotationInitialized = false;

	/** Orthographic axis view: rotation is held exactly and motion is planar (EnterFixedView). */
	bool bFixedView = false;

	float CameraSpeed = 1.f;
	bool bLinkLookToCameraSpeed = false;

	FVector PendingFlyInput = FVector::ZeroVector;
	FVector Velocity = FVector::ZeroVector;

	/** Drag and wheel translation not yet applied (or, while orbiting, pivot movement). */
	FVector PendingTranslation = FVector::ZeroVector;

	bool bOrbiting = false;
	FVector OrbitPivot = FVector::ZeroVector;
	double OrbitDistance = 0.0;
	double TargetOrbitDistance = 0.0;

	/** Focus glide: from FocusStart to FocusTarget over FocusTransitionSeconds; FocusElapsed < 0 when idle. */
	FVector FocusStart = FVector::ZeroVector;
	FVector FocusTarget = FVector::ZeroVector;
	float FocusElapsed = -1.f;
};
