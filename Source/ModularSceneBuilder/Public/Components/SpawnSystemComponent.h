// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "SpawnSystemComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStageItemSpawned, class AModularBaseActor*, SpawnedActor);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStageItemDeleted, AActor*, DeletedActor);

/**
 * Turns cursor hits into placed stage items and removes them again.
 *
 * Knows nothing about input or tracing: the owning controller supplies FHitResults, which keeps
 * this component reusable (e.g. from a UI drag-drop or automated tests). The active item is
 * cached from UStageItemSubsystem::OnSelectedItemChanged, so nothing is polled.
 *
 * Placement happens in "strokes": BeginPlacement on press, UpdatePlacement while held,
 * EndPlacement on release. Single-placement items (lights) spawn once and never open a stroke;
 * continuous items stamp at most one instance per grid cell per stroke.
 */
UCLASS(ClassGroup = (StageCraft), meta = (BlueprintSpawnableComponent))
class MODULARSCENEBUILDER_API USpawnSystemComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	USpawnSystemComponent();

	/** Press: spawns the active item at the hit and, for continuous items, opens a stroke. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Spawning")
	void BeginPlacement(const struct FHitResult& Hit);

	/** Held: spawns into the hit's grid cell if this stroke has not filled it yet. No-op outside a stroke. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Spawning")
	void UpdatePlacement(const struct FHitResult& Hit);

	/** Release: closes the current stroke. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Spawning")
	void EndPlacement();

	/** True while a continuous stroke is open. Callers use this to skip per-frame traces otherwise. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Spawning")
	bool IsPlacementStrokeActive() const { return bStrokeActive; }

	/**
	 * Destroys the target if it is a stage item (implements IInteractableInterface).
	 * Returns false for anything else, so level geometry can never be deleted by mistake.
	 */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Spawning")
	bool TryDeleteActor(AActor* Target);

	UFUNCTION(BlueprintPure, Category = "StageCraft|Spawning")
	class UBaseItemData* GetActiveItem() const { return ActiveItem; }

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Spawning")
	FOnStageItemSpawned OnItemSpawned;

	/** Fires just before the actor is destroyed, while it is still valid. */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Spawning")
	FOnStageItemDeleted OnItemDeleted;

protected:
	//~ Begin UActorComponent Interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	//~ End UActorComponent Interface

	/**
	 * Stroke de-duplication cell size for axes whose item GridSize is zero (snapping disabled).
	 * Without it, a continuous item with no grid would spawn on every centimetre of mouse movement.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Spawning", meta = (ClampMin = "1.0", Units = "cm"))
	double FallbackStrokeCellSize = 100.0;

private:
	UFUNCTION()
	void HandleSelectedItemChanged(class UBaseItemData* NewItem, class UBaseItemData* PreviousItem);

	class AModularBaseActor* SpawnItemAt(class UBaseItemData& Item, const FTransform& Transform);
	FTransform ComputePlacementTransform(const class UBaseItemData& Item, const struct FHitResult& Hit) const;
	FIntPoint ToStrokeCell(const class UBaseItemData& Item, const FVector& Location) const;

	UPROPERTY(Transient)
	TObjectPtr<class UStageItemSubsystem> ItemSubsystem = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UBaseItemData> ActiveItem = nullptr;

	/** Grid cells (XY) filled during the current stroke. */
	TSet<FIntPoint> StrokeCells;

	bool bStrokeActive = false;
};
