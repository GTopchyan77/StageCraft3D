// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * What a held mouse-button combination does to the perspective camera, as in the Unreal Editor 5.8 viewport
 * (FEditorViewportClient::ConvertMovementToDragRot / ConvertMovementToOrbitDragRot; Docs/ADR/0018 §2).
 */
enum class EStageCameraDrag : uint8
{
	None,
	/** LMB: forward / back along the horizontal view direction and yaw. */
	FlyAndTurn,
	/** RMB: look around; the fly keys move while it is held. */
	Look,
	/** MMB: pan in the view plane. */
	Pan,
	/** LMB + RMB: pan sideways and straight up / down (world Z). */
	PanVertical,
	/** Alt + LMB: orbit the pivot. */
	Orbit,
	/** Alt + RMB: move toward / away from the pivot. */
	OrbitDolly,
	/** Alt + MMB: pan the pivot and the camera together. */
	OrbitPan,
};

/** Mouse buttons and Alt as the viewport saw them. */
struct FStageCameraButtons
{
	bool bLeft = false;
	bool bMiddle = false;
	bool bRight = false;
	bool bAlt = false;

	bool AnyButton() const { return bLeft || bMiddle || bRight; }
};

/**
 * The Unreal Editor's camera rules, as pure functions so they are testable without a viewport. Numbers are the editor's:
 *  - camera speed: one continuous value (UE 5.8 FEditorViewportCameraSpeedSettings; 1 by default), stepped by 10 % per
 *    wheel notch while the right button is held (OnChangeCameraSpeed);
 *  - flying at speed S uses the editor's own integrator (FEditorCameraController::UpdatePosition: acceleration 20000·S,
 *    damping 10 per second, at most 0.75 per step), settling near 2000·S cm/s; the key impulses are not normalised, so a
 *    diagonal is faster, as in the editor;
 *  - mouse drags move 1 cm per mouse count times S, and turn 0.2° per count (ULevelEditorViewportSettings::MouseSensitivty),
 *    applied at once with no smoothing (the editor's bUsePhysicsBasedRotation is off);
 *  - the wheel alone dollies 96 cm per notch, whatever the speed (MouseScrollCameraSpeed 5 → 3 × 32 in OnDollyPerspectiveCamera);
 *  - F frames the bounds' sphere in the view (FocusViewportOnBox, minimum radius 10 cm) and glides there in 0.25 s, cubic out.
 * Mouse deltas are in raw counts with +Y = mouse moved up (Enhanced Input's Mouse2D). They are raw only while the project's
 * Mouse2D axis sensitivity is 1 (Config/DefaultInput.ini; the engine's default of 0.07 made every drag 14 times too slow);
 * StageCraft.Camera.InputConfig guards it.
 */
namespace StageCameraNavigation
{
	inline constexpr float DefaultCameraSpeed = 1.f;
	/** 20 cm/s flight: close-up rigging work. */
	inline constexpr float MinCameraSpeed = 0.01f;
	/** The editor's slider maximum. */
	inline constexpr float MaxCameraSpeed = 32.f;
	inline constexpr float SpeedStepFraction = 0.1f;
	inline constexpr float FlyAccelerationPerSpeed = 20000.f;
	inline constexpr float FlyDampingPerSecond = 10.f;
	inline constexpr float MaxFlyDampingPerStep = 0.75f;
	/** The editor never integrates a longer step (EditorMovementDeltaUpperBound), so a hitch cannot fling the camera. */
	inline constexpr float MaxFlightStepSeconds = 1.f;
	/** Where acceleration and damping balance. */
	inline constexpr float FlyCmPerSecondPerSpeed = FlyAccelerationPerSpeed / FlyDampingPerSecond;
	inline constexpr float DragCmPerCount = 1.f;
	inline constexpr float DegreesPerCount = 0.2f;
	inline constexpr float WheelDollyCm = 96.f;
	inline constexpr double MinimumFocusRadius = 10.0;
	inline constexpr float FocusTransitionSeconds = 0.25f;
	/** Mouse travel (counts) before a left press counts as a camera drag rather than a click. */
	inline constexpr float ClickDragThreshold = 3.f;
	/** Closest the orbit / dolly may bring the camera to its pivot. */
	inline constexpr double MinOrbitDistance = 10.0;
	/** Pivot distance when nothing is under the view centre (the editor's look-at default). */
	inline constexpr double DefaultPivotDistance = 1000.0;

	/** Which camera drag the buttons select. Alt with any button orbits; without Alt, LMB+RMB wins over either alone. */
	MODULARSCENEBUILDER_API EStageCameraDrag ClassifyDrag(const FStageCameraButtons& Buttons);

	/** A non-finite value becomes the default; others are clamped to Min..MaxCameraSpeed. */
	MODULARSCENEBUILDER_API float SanitizeCameraSpeed(float Speed);

	/** Speed after Notches wheel notches (+ faster): each notch adds or removes 10 % of the current speed. */
	MODULARSCENEBUILDER_API float StepCameraSpeed(float Speed, float Notches);

	/** Nominal flight speed in cm/s (acceleration / damping); the discrete step settles a little below it at low frame rates. */
	MODULARSCENEBUILDER_API float GetFlySpeed(float CameraSpeed);

	/**
	 * One flight step, exactly as FEditorCameraController::UpdatePosition: Velocity += Impulse · 20000 · S · dt, then
	 * Velocity −= Velocity · min(10 · dt, 0.75). Impulse is the held keys in world space (each axis −1..1, not normalised).
	 * DeltaSeconds is clamped to 0..MaxFlightStepSeconds; a non-finite input returns zero velocity.
	 */
	MODULARSCENEBUILDER_API FVector StepFlightVelocity(const FVector& Velocity, const FVector& WorldImpulse, float CameraSpeed, float DeltaSeconds);

	/** LMB drag: world translation (horizontal, along the view's yaw). */
	MODULARSCENEBUILDER_API FVector GetFlyAndTurnTranslation(const FVector2D& Delta, double YawDegrees, float CameraSpeed);

	/** LMB drag: yaw change in degrees (pitch is kept). */
	MODULARSCENEBUILDER_API double GetFlyAndTurnYaw(const FVector2D& Delta);

	/** MMB drag: world translation in the view plane (camera right and camera up). */
	MODULARSCENEBUILDER_API FVector GetPanTranslation(const FVector2D& Delta, const FRotator& View, float CameraSpeed);

	/** LMB + RMB drag: world translation along the view's horizontal right and world Z. */
	MODULARSCENEBUILDER_API FVector GetVerticalPanTranslation(const FVector2D& Delta, double YawDegrees, float CameraSpeed);

	/** Alt + LMB drag: the view rotation after orbiting (pitch is limited by the caller). */
	MODULARSCENEBUILDER_API FRotator GetOrbitRotation(const FRotator& View, const FVector2D& Delta);

	/** Where the camera sits for an orbit about Pivot at Distance, looking along View. */
	MODULARSCENEBUILDER_API FVector GetOrbitLocation(const FVector& Pivot, const FRotator& View, double Distance);

	/**
	 * Alt + RMB drag: the new distance to the pivot, never under MinOrbitDistance. Right or down moves closer, left or up
	 * further away (the editor's IsOrbitZoomMode: X − Y with bInvertRightMouseDollyYAxis off).
	 */
	MODULARSCENEBUILDER_API double GetDollyDistance(double Distance, const FVector2D& Delta, float CameraSpeed);

	/** Wheel without RMB: translation along the full view direction (+ notches move forward). */
	MODULARSCENEBUILDER_API FVector GetWheelDolly(const FRotator& View, float Notches);

	/**
	 * Where the camera goes to frame Bounds without turning (FocusViewportOnBox): back from the centre along the view by the
	 * distance at which the bounding sphere fills the field of view. Aspect is width / height. An invalid box returns Current.
	 */
	MODULARSCENEBUILDER_API FVector GetFocusLocation(const FBox& Bounds, const FRotator& View, float FOVDegrees, float Aspect, const FVector& Current);

	/** Cubic-out easing of the focus glide, Alpha 0..1. */
	MODULARSCENEBUILDER_API float EaseFocus(float Alpha);
}
