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
 * place if a catalog item is armed). Right-click: placed item (delete) > empty (deselect and disarm).
 *
 * Input assets are designer-assignable in a Blueprint subclass. If any slot is empty, an
 * equivalent mapping (LMB, RMB, Space) is built in code, so the project works with no content.
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

	/** Cursor trace against surfaces items can be placed on (floor, level geometry, other items). */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Trace")
	bool GetPlacementHitUnderCursor(FHitResult& OutHit) const;

	/** Cursor trace that only hits placed stage items. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Trace")
	bool GetStageItemHitUnderCursor(FHitResult& OutHit) const;

	/** Cursor trace that only hits transform gizmo handles. */
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

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input")
	TObjectPtr<class UInputMappingContext> EditorMappingContext = nullptr;

	/** Primary click: gizmo drag, select, or place (see class comment). Continuous items paint while held. */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input")
	TObjectPtr<class UInputAction> PlaceAction = nullptr;

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input")
	TObjectPtr<class UInputAction> DeleteAction = nullptr;

	/**
	 * Switches the gizmo between translate and rotate. Space is also the default pawn's "fly up"
	 * key, so an assigned asset should enable "Consumes Action And Axis Mappings".
	 */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input")
	TObjectPtr<class UInputAction> ToggleGizmoModeAction = nullptr;

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input")
	int32 MappingContextPriority = 0;

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Trace")
	TEnumAsByte<ECollisionChannel> PlacementTraceChannel;

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Trace")
	TEnumAsByte<ECollisionChannel> StageItemTraceChannel;

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Trace")
	TEnumAsByte<ECollisionChannel> GizmoTraceChannel;

private:
	void BuildDefaultInputMapping();

	UFUNCTION()
	void HandleSelectionChanged(AActor* NewSelection, AActor* PreviousSelection);

	void HandlePrimaryStarted();
	void HandlePrimaryTriggered();
	void HandlePrimaryCompleted();
	void HandleDeleteStarted();
	void HandleToggleGizmoMode();

	bool TryBeginGizmoDrag();
	bool GetCursorRay(FVector& OutOrigin, FVector& OutDirection) const;

	UPROPERTY(Transient)
	TObjectPtr<class AModularTransformGizmo> Gizmo = nullptr;
};
