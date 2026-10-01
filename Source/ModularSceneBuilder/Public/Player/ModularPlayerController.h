// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "ModularPlayerController.generated.h"

/**
 * Stage editing controller. Its only jobs are Enhanced Input, cursor tracing and deciding which
 * system a click belongs to; the actual work lives in focused components/actors:
 *   USpawnSystemComponent (place/delete), USelectionComponent (selection), AModularTransformGizmo (transform).
 *
 * Left-click priority: gizmo handle (drag) > placed item (select) > empty surface (deselect, then
 * place if a catalog item is armed).
 *
 * The right mouse button works like the Unreal Editor viewport. Holding it hides and captures the cursor
 * and enables fly navigation (mouse = look, WASD = move, Q/E = down/up, wheel = fly speed). Releasing
 * it puts the cursor back where it was. A short press with no mouse or fly movement counts as a click:
 * placed item (delete) > empty (deselect and disarm).
 *
 * Input assets are designer-assignable in a Blueprint subclass. If any slot is empty, an equivalent
 * mapping (LMB, RMB, Space, and the fly keys) is built in code, so the project works with no content.
 */
UCLASS()
class MODULARSCENEBUILDER_API AModularPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	AModularPlayerController();

	UFUNCTION(BlueprintPure, Category = "StageCraft")
	class USpawnSystemComponent* GetSpawnSystem() const { return SpawnSystem; }

	UFUNCTION(BlueprintPure, Category = "StageCraft")
	class USelectionComponent* GetSelection() const { return Selection; }

	UFUNCTION(BlueprintPure, Category = "StageCraft")
	class AModularTransformGizmo* GetGizmo() const { return Gizmo; }

	UFUNCTION(BlueprintPure, Category = "StageCraft|UI")
	class UUserWidget* GetHUDWidget() const { return HUDWidget; }

	/** True while the secondary button is held and the camera is in fly navigation. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Camera")
	bool IsNavigatingCamera() const { return bIsNavigatingCamera; }

	/** Cursor trace against surfaces items can be placed on (floor, level geometry, other items). */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Trace")
	bool GetPlacementHitUnderCursor(FHitResult& OutHit) const;

	/** Cursor trace that only hits placed stage items. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Trace")
	bool GetStageItemHitUnderCursor(FHitResult& OutHit) const;

	/**
	 * Cursor ray against the gizmo handles only. Each handle component is tested directly instead of
	 * through the world, so a handle buried inside another mesh is still hit, matching its always-on-top rendering.
	 */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Trace")
	bool GetGizmoHitUnderCursor(FHitResult& OutHit) const;

protected:
	//~ Begin APlayerController Interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void SetupInputComponent() override;
	//~ End APlayerController Interface

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<class USpawnSystemComponent> SpawnSystem = nullptr;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<class USelectionComponent> Selection = nullptr;

	/** Spawned once for the local player and re-targeted on every selection change. */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Gizmo")
	TSubclassOf<class AModularTransformGizmo> GizmoClass;

	/**
	 * Root editor UI (catalog, inspector, show control panels), created once for the local player.
	 * Clicks on its visible panels are consumed by UMG and never reach placement/selection input.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|UI")
	TSubclassOf<class UUserWidget> HUDWidgetClass;

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input")
	TObjectPtr<class UInputMappingContext> EditorMappingContext = nullptr;

	/** Primary click: gizmo drag, select, or place (see class comment). Continuous items paint while held. */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input")
	TObjectPtr<class UInputAction> PlaceAction = nullptr;

	/** Secondary button: hold for fly navigation, click without movement to delete or back out. */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input")
	TObjectPtr<class UInputAction> SecondaryAction = nullptr;

	/**
	 * Switches the gizmo between translate and rotate. Space is also the default pawn's "fly up"
	 * key, so an assigned asset should enable "Consumes Action And Axis Mappings".
	 */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input")
	TObjectPtr<class UInputAction> ToggleGizmoModeAction = nullptr;

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input")
	int32 MappingContextPriority = 0;

	/**
	 * Added only while the secondary button is held, so the fly keys stay free the rest of the time.
	 * Must map CameraLookAction, CameraMoveAction and CameraSpeedAction.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input|Camera")
	TObjectPtr<class UInputMappingContext> CameraNavigationMappingContext = nullptr;

	/** Axis2D look delta (X = yaw, Y = pitch). */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input|Camera")
	TObjectPtr<class UInputAction> CameraLookAction = nullptr;

	/** Axis3D fly input (X = view forward, Y = view right, Z = world up). */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input|Camera")
	TObjectPtr<class UInputAction> CameraMoveAction = nullptr;

	/** Axis1D fly speed steps (mouse wheel), like the Unreal Editor's camera speed. */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input|Camera")
	TObjectPtr<class UInputAction> CameraSpeedAction = nullptr;

	/** Above the editor context, so the fly keys win while navigating. */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input|Camera")
	int32 CameraNavigationContextPriority = 1;

	/** Degrees of rotation per raw mouse count. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StageCraft|Camera", meta = (ClampMin = "0.001"))
	float LookSensitivity = 0.2f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StageCraft|Camera", meta = (ClampMin = "1.0", Units = "cm/s"))
	float FlySpeed = 1200.f;

	UPROPERTY(EditAnywhere, Category = "StageCraft|Camera", meta = (ClampMin = "1.0", Units = "cm/s"))
	float MinFlySpeed = 50.f;

	UPROPERTY(EditAnywhere, Category = "StageCraft|Camera", meta = (ClampMin = "1.0", Units = "cm/s"))
	float MaxFlySpeed = 20000.f;

	/** Speed multiplier per mouse-wheel notch while navigating. */
	UPROPERTY(EditAnywhere, Category = "StageCraft|Camera", meta = (ClampMin = "1.01"))
	float FlySpeedStepFactor = 1.25f;

	/**
	 * A secondary press counts as a click (delete/back out) only if the mouse moved less than this many
	 * raw counts, no fly key was used, and the button was released within MaxSecondaryClickDuration.
	 */
	UPROPERTY(EditAnywhere, Category = "StageCraft|Camera", meta = (ClampMin = "0.0"))
	float SecondaryClickDragThreshold = 4.f;

	/** Holding the button longer than this never deletes, even without movement: deletion must be deliberate. */
	UPROPERTY(EditAnywhere, Category = "StageCraft|Camera", meta = (ClampMin = "0.0", Units = "s"))
	float MaxSecondaryClickDuration = 0.35f;

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Trace")
	TEnumAsByte<ECollisionChannel> PlacementTraceChannel;

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Trace")
	TEnumAsByte<ECollisionChannel> StageItemTraceChannel;

private:
	void BuildDefaultInputMapping();
	void BuildDefaultCameraMapping();
	void ApplyEditorInputMode();

	UFUNCTION()
	void HandleSelectionChanged(AActor* NewSelection, AActor* PreviousSelection);

	void HandlePrimaryStarted();
	void HandlePrimaryTriggered();
	void HandlePrimaryCompleted();
	void HandleSecondaryStarted();
	void HandleSecondaryCompleted();
	void HandleSecondaryClick();
	void HandleToggleGizmoMode();

	void HandleCameraLook(const struct FInputActionValue& Value);
	void HandleCameraMove(const struct FInputActionValue& Value);
	void HandleCameraSpeed(const struct FInputActionValue& Value);

	void BeginCameraNavigation();
	void EndCameraNavigation();
	void ApplyFlySpeed() const;

	bool TryBeginGizmoDrag();
	bool GetCursorRay(FVector& OutOrigin, FVector& OutDirection) const;

	UPROPERTY(Transient)
	TObjectPtr<class AModularTransformGizmo> Gizmo = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UUserWidget> HUDWidget = nullptr;

	// Fly navigation state, captured when the secondary button goes down.
	bool bIsNavigatingCamera = false;
	bool bNavigationUsedFlyKeys = false;
	bool bHasNavigationCursorPosition = false;
	float NavigationMouseTravel = 0.f;
	double NavigationStartTime = 0.0;
	FVector2D NavigationCursorPosition = FVector2D::ZeroVector;
};
