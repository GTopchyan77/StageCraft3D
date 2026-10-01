// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "Data/StageParameterTypes.h"
#include "Economy/StageEconomyTypes.h"
#include "ModularPlayerController.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStageRequestRejected, const FStageEconomyResultInfo&, Result);

/**
 * Stage editing controller. Its only jobs are Enhanced Input, cursor tracing and deciding which
 * system an input belongs to; the actual work lives in focused components/actors:
 *   USpawnSystemComponent (place/delete), USelectionComponent (selection), AModularTransformGizmo
 *   (transform), AStageCameraPawn (fly camera motion and smoothing).
 *
 * Mouse buttons have exclusive jobs, as in the Unreal Editor viewport:
 *  - Left: gizmo handle (drag) > placed item (select) > empty surface (deselect, then place if a
 *    catalog item is armed). Ignored entirely while the right button is held.
 *  - Right: navigation only. While held: mouse = look, W/S = forward/back along the view, A/D =
 *    strafe, E/Q = straight up/down world Z. It never selects, places or deletes, and is ignored
 *    while a left-button drag is in progress. Cursor hide/capture/restore is done by
 *    UStageCraftGameViewportClient, so this class never changes the input mode mid-press.
 *  - Wheel: fly speed, with or without the right button held. It scales every fly axis alike.
 *
 * Keys: Delete removes the selected item, Esc deselects and disarms the catalog item, Space toggles
 * the gizmo between translate and rotate.
 *
 * Request bridge: widgets never write to the game directly. Parameter edits, purchases and
 * placements go through RequestParameterChange / RequestPurchase / the spawn component's
 * PlacementValidator, which ask the GameMode (rules + ownership) and only then let the
 * session or economy subsystem apply them. Refusals are broadcast on OnRequestRejected.
 *
 * Input assets are designer-assignable in a Blueprint subclass. If any slot is empty, an equivalent
 * mapping is built in code, so the project works with no content.
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

	/** True while the right mouse button is held and the camera is in fly navigation. */
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

	// --- Request bridge (UI -> GameMode rules -> subsystems) ---

	/** Validates with the GameMode, then applies through the session. The single write path for UI edits. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Requests")
	FStageEconomyResultInfo RequestParameterChange(UObject* Target, FGameplayTag ParameterId, const FStageParameterValue& Value);

	/** Starts a purchase. The result is the validation outcome; completion is reported on OnRequestRejected (failure) or the economy's OnPurchaseCompleted. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Requests")
	FStageEconomyResultInfo RequestPurchase(class UStageProductData* Product);

	/** Whether Item may be placed now (for graying out catalog entries). */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Requests")
	FStageEconomyResultInfo CanPlaceItem(const class UBaseItemData* Item) const;

	/**
	 * Marks what this player may not edit before a panel builds controls: locked parameters become
	 * read-only with bLocked set, and locked Type options are labelled. Display only; every write
	 * is still validated by RequestParameterChange.
	 */
	void DecorateParameterSections(const UObject* Target, TArray<FStageParameterSection>& Sections) const;

	/** Every refused request (UI toasts, shop prompts). */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Requests")
	FOnStageRequestRejected OnRequestRejected;

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

	/** Hold to navigate (right mouse button). Navigation only: it never selects, places or deletes. */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input")
	TObjectPtr<class UInputAction> NavigateAction = nullptr;

	/** Deletes the selected item (Delete key). */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input")
	TObjectPtr<class UInputAction> DeleteAction = nullptr;

	/** Backs out of everything: clears the actor selection and disarms the catalog item (Esc). */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input")
	TObjectPtr<class UInputAction> CancelAction = nullptr;

	/** Switches the gizmo between translate and rotate (Space). */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input")
	TObjectPtr<class UInputAction> ToggleGizmoModeAction = nullptr;

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input")
	int32 MappingContextPriority = 0;

	/**
	 * Applied for the whole session; the handlers act only while NavigateAction is held. Never
	 * added/removed per press, because rebuilding mappings while keys are held makes press/release
	 * pairing unreliable. Must map CameraLookAction, CameraMoveAction and CameraSpeedAction.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input|Camera")
	TObjectPtr<class UInputMappingContext> CameraNavigationMappingContext = nullptr;

	/** Axis2D look delta in raw mouse counts (X = yaw, Y = pitch). */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input|Camera")
	TObjectPtr<class UInputAction> CameraLookAction = nullptr;

	/** Axis3D fly input (X = view forward, Y = view right, Z = world up). */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input|Camera")
	TObjectPtr<class UInputAction> CameraMoveAction = nullptr;

	/** Axis1D fly speed steps (mouse wheel), like the Unreal Editor's camera speed. */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input|Camera")
	TObjectPtr<class UInputAction> CameraSpeedAction = nullptr;

	/** Above the editor context, so the fly keys win when both map the same key. */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input|Camera")
	int32 CameraNavigationContextPriority = 1;

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Trace")
	TEnumAsByte<ECollisionChannel> PlacementTraceChannel;

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Trace")
	TEnumAsByte<ECollisionChannel> StageItemTraceChannel;

private:
	void BuildDefaultInputMapping();
	void BuildDefaultCameraMapping();

	UFUNCTION()
	void HandleSelectionChanged(AActor* NewSelection, AActor* PreviousSelection);

	UFUNCTION()
	void HandlePurchaseCompleted(class UStageProductData* Product, FStageEconomyResultInfo Result);

	FStageEconomyResultInfo ValidatePlacement(const class UBaseItemData& Item);
	void ReportRejection(const FStageEconomyResultInfo& Result);
	class AStageCraftGameModeBase* GetStageGameMode() const;
	class UStageEconomySubsystem* GetEconomy() const;

	void HandlePrimaryStarted();
	void HandlePrimaryTriggered();
	void HandlePrimaryCompleted();
	void HandleNavigateStarted();
	void HandleNavigateCompleted();
	void HandleDelete();
	void HandleCancel();
	void HandleToggleGizmoMode();

	void HandleCameraLook(const struct FInputActionValue& Value);
	void HandleCameraMove(const struct FInputActionValue& Value);
	void HandleCameraSpeed(const struct FInputActionValue& Value);

	/** The fly camera, or null when a different pawn is possessed (navigation is then a no-op). */
	class AStageCameraPawn* GetCameraPawn() const;

	bool IsPrimaryInteractionActive() const;
	bool TryBeginGizmoDrag();
	bool GetCursorRay(FVector& OutOrigin, FVector& OutDirection) const;

	UPROPERTY(Transient)
	TObjectPtr<class AModularTransformGizmo> Gizmo = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UUserWidget> HUDWidget = nullptr;

	bool bIsNavigatingCamera = false;
};
