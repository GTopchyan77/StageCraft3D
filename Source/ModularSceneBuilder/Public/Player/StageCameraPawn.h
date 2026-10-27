// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "StageCameraPawn.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStageCameraSpeedChanged, float, NewCameraSpeed);

/**
 * Editor-viewport camera. UStageCameraNavigationComponent turns mouse and key input into calls here, with the Unreal Editor's
 * rules (StageCameraNavigation, Docs/ADR/0018 §2); this pawn applies them the way FEditorViewportClient does:
 *  - look, drags, the wheel dolly and orbit change the view the moment the input arrives, with no easing (the editor's
 *    MoveViewportPerspectiveCamera; its rotation physics are off by default). Pitch is clamped short of vertical;
 *  - flying (fly keys while RMB is held) runs the editor's own integrator (StageCameraNavigation::StepFlightVelocity):
 *    W/S along the full look vector, A/D along the view's right, E/Q along world Z, accelerating and braking as in the editor;
 *  - focus: a 0.25 s cubic-out glide to a framing location (the editor's FocusViewportOnBox transition).
 *
 * The view rotation has one source of truth, the controller's control rotation: every change reads it, changes it and writes
 * it back, so a rotation someone else set (spawn, a game-mode restart, a projection switch) is simply where the next input
 * starts. There is deliberately no movement component and no collision: the camera passes through stage geometry like the
 * editor's.
 *
 * Tick: only while a flight or a focus glide is running; it switches itself off once both have settled, so a camera at rest,
 * or one being dragged with the mouse, costs nothing per frame.
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

	/** A world translation (drag, wheel dolly), applied at once. Ignored in a fixed view and while orbiting. */
	void AddTranslation(const FVector& Offset);

	/** Turns the view by YawDegrees (the left-button drag). */
	void AddYaw(double YawDegrees);

	/**
	 * Starts orbiting Pivot from where the camera is now (Alt + drag). The distance is the camera's distance to the pivot, at
	 * least StageCameraNavigation::MinOrbitDistance, and the view turns to face the pivot. Stops any flight and focus glide.
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

	/** Nominal flight speed in cm/s at the current camera speed. */
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

	/** Points the view somewhere new at once (pitch limited, no roll). Ignored in a fixed view. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Camera")
	void SetViewRotation(FRotator NewRotation);

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

	/** True while a flight or focus glide is running (its tick is on). */
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

	/** Stops short of straight up/down: at exactly 90 degrees yaw becomes undefined and the view flips. */
	UPROPERTY(EditAnywhere, Category = "StageCraft|Camera|Look", meta = (ClampMin = "0.0", ClampMax = "89.9", Units = "deg"))
	float MaxPitch = 89.f;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<class USceneComponent> CameraRoot = nullptr;

private:
	/** The view rotation now: the controller's control rotation (the actor's when unpossessed), pitch limited, no roll. */
	FRotator GetCurrentViewRotation() const;

	/** Writes the view rotation (pitch limited, roll 0) to the controller. */
	void ApplyViewRotation(const FRotator& NewRotation);

	/** Puts the camera on the orbit sphere for the current view rotation. */
	void PlaceOnOrbit();

	void StartTicking();

	/** Each returns true while it still has motion to apply. */
	bool TickFlight(float DeltaSeconds);
	bool TickFocus(float DeltaSeconds);

	void CancelGlides();

	/** Orthographic axis view: rotation is held exactly and motion is planar (EnterFixedView). */
	bool bFixedView = false;

	float CameraSpeed = 1.f;
	bool bLinkLookToCameraSpeed = false;

	FVector PendingFlyInput = FVector::ZeroVector;
	FVector Velocity = FVector::ZeroVector;

	bool bOrbiting = false;
	FVector OrbitPivot = FVector::ZeroVector;
	double OrbitDistance = 0.0;

	/** Focus glide: from FocusStart to FocusTarget over FocusTransitionSeconds; FocusElapsed < 0 when idle. */
	FVector FocusStart = FVector::ZeroVector;
	FVector FocusTarget = FVector::ZeroVector;
	float FocusElapsed = -1.f;
};
