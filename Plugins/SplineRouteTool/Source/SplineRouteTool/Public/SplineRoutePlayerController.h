// Copyright DamenTools internal. Spline Route Tool plugin.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "SplineRoutePlayerController.generated.h"

class UInputMappingContext;
class UInputAction;
struct FInputActionValue;
class ASplineFollowerVessel;

/**
 * PlayerController for the runtime spline-route authoring demo.
 * Wires Enhanced Input actions to USplineRouteAuthoringSubsystem.
 */
UCLASS()
class SPLINEROUTETOOL_API ASplineRoutePlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	ASplineRoutePlayerController();

	/** IMC pushed onto the Enhanced Input subsystem on BeginPlay. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Spline Route|Input")
	TObjectPtr<UInputMappingContext> InputMappingContext;

	/** Triggered on the Place key (F): appends a new control point at the cursor. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Spline Route|Input")
	TObjectPtr<UInputAction> PlaceAction;

	/** Triggered on LMB press: selects the waypoint nearest the cursor (selection only, never moves it). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Spline Route|Input")
	TObjectPtr<UInputAction> SelectAction;

	/** Triggered on LMB release: ends the drag. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Spline Route|Input")
	TObjectPtr<UInputAction> ReleaseAction;

	/** Held while LMB is down: drags the selected point — but ONLY while the Spacebar move-modifier is held. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Spline Route|Input")
	TObjectPtr<UInputAction> DragAction;

	/** Triggered on Delete key or RMB press: removes the currently selected point. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Spline Route|Input")
	TObjectPtr<UInputAction> DeleteAction;

	/** Triggered on mouse wheel: moves the pawn forward/backward along its facing (camera zoom). */
	UPROPERTY(EditDefaultsOnly, Category = "Spline Route|Input")
	TObjectPtr<UInputAction> ZoomAction;

	/** Triggered on the Cycle Vessel Camera key (C): advances through the last-spawned vessel's 5 camera slots, then back to free cam. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Spline Route|Input")
	TObjectPtr<UInputAction> CycleVesselCameraAction;

	/** Units/sec of pawn movement per unit of scroll wheel input. */
	UPROPERTY(EditDefaultsOnly, Category = "Spline Route|Camera")
	float ZoomSpeed = 100.0f;

	/** Held for forward(+1)/back(-1) pan (W/S, Up/Down arrows). Applied as a direct position delta each tick — see PanPawn. */
	UPROPERTY(EditDefaultsOnly, Category = "Spline Route|Input")
	TObjectPtr<UInputAction> PanForwardAction;

	/** Held for right(+1)/left(-1) pan (D/A). Applied as a direct position delta each tick — see PanPawn. */
	UPROPERTY(EditDefaultsOnly, Category = "Spline Route|Input")
	TObjectPtr<UInputAction> PanRightAction;

	/** Held for up(+1)/down(-1) pan (E/Q). Applied as a direct position delta each tick — see PanPawn. */
	UPROPERTY(EditDefaultsOnly, Category = "Spline Route|Input")
	TObjectPtr<UInputAction> PanUpAction;

	/**
	 * Units/sec the camera pans at while a pan key is held. Applied directly via AddActorWorldOffset
	 * (PanPawn) instead of AddMovementInput, so panning has no acceleration ramp-up and stops the
	 * instant the key is released — no reliance on UFloatingPawnMovement's Deceleration at all.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Camera", meta = (ClampMin = "1.0"))
	float PanSpeed = 8000.0f;

protected:
	virtual void BeginPlay() override;
	virtual void SetupInputComponent() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	void OnPlacePressed(const FInputActionValue& Value);
	void OnSelectPressed(const FInputActionValue& Value);
	void OnReleased(const FInputActionValue& Value);
	void OnDragTriggered(const FInputActionValue& Value);
	void OnDeletePressed(const FInputActionValue& Value);
	void OnZoomTriggered(const FInputActionValue& Value);
	void OnPanForwardTriggered(const FInputActionValue& Value);
	void OnPanRightTriggered(const FInputActionValue& Value);
	void OnPanUpTriggered(const FInputActionValue& Value);
	void OnCycleVesselCameraPressed(const FInputActionValue& Value);

	/** Shared direct-position-delta pan helper. Direction need not be normalized. No-op if AxisValue is ~0 or there's no possessed pawn. */
	void PanPawn(const FVector& Direction, float AxisValue);

	/** -1 = free/authoring camera, not cycling. 0..4 = index into the cycling vessel's Cameras array. */
	int32 CurrentCameraSlotIndex = -1;

	/** The vessel CurrentCameraSlotIndex applies to. Tracked separately from the subsystem's LastSpawnedVessel
	 *  so a newer vessel spawned mid-cycle is detected and cleanly restarts the cycle at slot 0 instead of
	 *  resuming a stale index on the wrong vessel. */
	TWeakObjectPtr<ASplineFollowerVessel> CyclingVessel;

	/** GetPawn() cached at the moment cycling starts (index -1 -> 0), so wrapping back to free cam restores exactly this pawn. */
	TWeakObjectPtr<APawn> CachedFreeCameraPawn;
};
