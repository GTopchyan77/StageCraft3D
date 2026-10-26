// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Player/StageCameraNavigation.h"
#include "StageCameraNavigationComponent.generated.h"

/** A mouse button as the camera sees it. */
enum class EStageCameraButton : uint8
{
	Left,
	Middle,
	Right,
};

/**
 * The Unreal Editor's viewport navigation for AStageCameraPawn (Docs/ADR/0018 §2), on the local AModularPlayerController.
 *
 * The controller decides who a press belongs to (placing, the gizmo, selection) and hands camera presses here; this component
 * owns what the held buttons mean (StageCameraNavigation::ClassifyDrag, re-read with Alt on every mouse move), and turns mouse
 * deltas, the wheel and the fly keys into pawn motion:
 *  - perspective: LMB drag = forward/back + yaw, RMB = look (+ fly keys), MMB = pan, LMB+RMB = pan sideways and up/down,
 *    Alt+LMB = orbit, Alt+RMB = dolly, Alt+MMB = pan with the pivot, wheel = dolly, RMB+wheel = camera speed;
 *  - orthographic: RMB or MMB drag = pan, wheel = zoom at the cursor, fly keys pan (UStageViewportComponent).
 *
 * A plain left press in perspective is a click until the mouse travels ClickDragThreshold counts: then it becomes a camera drag
 * and the click is dropped (EndButton reports it), so selection happens on release only when nothing moved, as in the editor.
 *
 * The orbit pivot is the last focus point while the camera has not moved since; otherwise whatever is under the view centre
 * (a trace), or a point DefaultPivotDistance ahead. Camera speed is the pawn's; it is stored in the user's preferences when a
 * drag or speed change ends (UStageInputPreferencesSubsystem::StoreCameraSpeed), not on every notch.
 *
 * Game thread only. Event-driven, no tick: every call comes from an input event.
 */
UCLASS(ClassGroup = (StageCraft), meta = (BlueprintSpawnableComponent))
class MODULARSCENEBUILDER_API UStageCameraNavigationComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UStageCameraNavigationComponent();

	/**
	 * A camera button went down. For Left without Alt, bCanStartDrag says whether it may become a fly-and-turn drag (the
	 * controller passes false in Place mode, over a gizmo handle, with Ctrl or Shift, or in an orthographic view); Right and
	 * Middle always navigate.
	 */
	void BeginButton(EStageCameraButton Button, bool bCanStartDrag = true);

	/**
	 * The button came up. Returns true when it was a plain left press that never became a drag: the controller then treats it
	 * as a click (select or deselect).
	 */
	bool EndButton(EStageCameraButton Button);

	/** Every button released (focus lost, the controller tearing down): ends any drag without a click. Idempotent. */
	void CancelAll();

	/** Raw mouse delta (counts, +Y up) for this event. */
	void HandleMouseDelta(const FVector2D& Delta);

	/** Wheel notches (+ away from the user). CursorPosition (viewport pixels) is where an orthographic zoom stays put. */
	void HandleWheel(float Notches, const FVector2D& CursorPosition);

	/** Fly-key axes for this frame (X forward, Y right, Z up); used only while the right button is held. */
	void HandleFlyInput(const FVector& Input);

	/**
	 * F: frames the selection (perspective: FocusViewportOnBox's distance and glide; orthographic: centre and fit). Returns
	 * false, changing nothing, when nothing is selected or nothing selected has bounds.
	 */
	bool FocusSelection();

	/** Frames an explicit box, as FocusSelection does. */
	bool FocusBounds(const FBox& Bounds);

	/** A drag is moving the camera (or the right button is held). Keyboard commands wait while this is true. */
	bool IsNavigating() const { return ActiveDrag != EStageCameraDrag::None; }

	/** The right button is held without Alt: the fly keys move the camera. */
	bool IsFlying() const;

	EStageCameraDrag GetActiveDrag() const { return ActiveDrag; }

	/**
	 * Asked by UStageCraftGameViewportClient at the moment a mouse button goes down, before any gameplay input runs: true when
	 * this press will move the camera, so Slate hides the cursor and switches to raw mouse deltas. bLeftMayDrag is the
	 * controller's verdict for a plain left press (as for BeginButton).
	 */
	bool ShouldHideCursorForPress(const FStageCameraButtons& Buttons, bool bLeftMayDrag) const;

protected:
	//~ Begin UActorComponent Interface
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	//~ End UActorComponent Interface

	/** How far ahead the orbit pivot trace looks for something under the view centre. */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Camera", meta = (ClampMin = "100.0", Units = "cm"))
	double PivotTraceDistance = 100000.0;

private:
	class AStageCameraPawn* GetCameraPawn() const;
	class UStageViewportComponent* GetViewport() const;
	bool IsAltDown() const;
	bool IsOrthographic() const;

	/** Re-reads what the held buttons (and Alt, now) mean; starts or ends an orbit as the mode changes. */
	void UpdateDrag();

	FVector FindOrbitPivot(const class AStageCameraPawn& Pawn) const;
	void StoreCameraSpeed();

	FStageCameraButtons Held;

	/** A plain left press waiting to become a drag (or a click on release). */
	bool bLeftPending = false;
	FVector2D LeftPendingTravel = FVector2D::ZeroVector;

	EStageCameraDrag ActiveDrag = EStageCameraDrag::None;

	/** The last focus point, reused as the orbit pivot while the camera is still where that focus (or orbit) left it. */
	TOptional<FVector> RememberedPivot;
	FVector RememberedPivotCameraLocation = FVector::ZeroVector;

	/** The speed last stored in the preferences, so an unchanged speed is not written again. */
	float StoredCameraSpeed = -1.f;
};
