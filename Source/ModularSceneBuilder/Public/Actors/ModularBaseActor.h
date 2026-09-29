// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Interaction/InteractableInterface.h"
#include "ModularBaseActor.generated.h"

class AModularBaseActor;
class UBaseItemData;
class UMaterialInterface;
class UStaticMeshComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnModularActorSelectionChanged, AModularBaseActor*, Actor, bool, bSelected);

/**
 * Parent class for every item the user places on stage.
 *
 * Visuals come from the UBaseItemData the actor was spawned from, so a simple prop needs no
 * Blueprint: the data asset's ActorClass can stay AModularBaseActor and the Mesh field supplies
 * the look. Items with extra behaviour (lights, screens) subclass this and extend ApplyItemData().
 *
 * Preferred spawn pattern (so construction scripts already see the data):
 *   AModularBaseActor* Actor = World->SpawnActorDeferred<AModularBaseActor>(Class, Transform);
 *   Actor->InitializeFromItemData(Item);
 *   Actor->FinishSpawning(Transform);
 *
 * The actor never ticks. Hover and selection feedback update only when that state changes.
 */
UCLASS(Blueprintable)
class MODULARSCENEBUILDER_API AModularBaseActor : public AActor, public IInteractableInterface
{
	GENERATED_BODY()

public:
	AModularBaseActor();

	/**
	 * Binds the actor to its catalog entry and applies its visuals. Safe to call again with a
	 * different item (e.g. a "swap item" feature). Assets are expected to be streamed in already
	 * by UStageItemSubsystem::SelectItem; if not, they load synchronously as a fallback.
	 */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Item")
	virtual void InitializeFromItemData(UBaseItemData* InItemData);

	UFUNCTION(BlueprintPure, Category = "StageCraft|Item")
	UBaseItemData* GetItemData() const { return ItemData; }

	UFUNCTION(BlueprintPure, Category = "StageCraft|Item")
	UStaticMeshComponent* GetMeshComponent() const { return MeshComponent; }

	UFUNCTION(BlueprintPure, Category = "StageCraft|Interaction")
	bool IsHovered() const { return bIsHovered; }

	UFUNCTION(BlueprintPure, Category = "StageCraft|Interaction")
	bool IsSelected() const { return bIsSelected; }

	/**
	 * Fires on select/deselect, and with bSelected = false when a selected actor is destroyed.
	 * The gizmo (Phase 4) and details panel bind here instead of watching the actor.
	 */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Interaction")
	FOnModularActorSelectionChanged OnSelectionChanged;

	//~ Begin IInteractableInterface
	virtual void OnHoverBegin_Implementation() override;
	virtual void OnHoverEnd_Implementation() override;
	virtual void OnSelect_Implementation() override;
	virtual void OnDeselect_Implementation() override;
	virtual FStageItemInteractionDetails GetInteractionDetails_Implementation() const override;
	//~ End IInteractableInterface

protected:
	//~ Begin AActor Interface
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	//~ End AActor Interface

	/**
	 * Pushes data-driven visuals onto components. Subclasses call Super and then apply their own
	 * fields. Must be idempotent because it runs from both InitializeFromItemData and OnConstruction.
	 */
	virtual void ApplyItemData(const UBaseItemData& Data);

	/** Refreshes hover/selection feedback. Override to highlight extra components. */
	virtual void UpdateHighlight();

	/** Blueprint hook that runs after the C++ ApplyItemData, for per-item setup without C++. */
	UFUNCTION(BlueprintImplementableEvent, Category = "StageCraft|Item", meta = (DisplayName = "On Item Data Applied"))
	void BP_OnItemDataApplied(const UBaseItemData* AppliedData);

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UStaticMeshComponent> MeshComponent;

	/** Catalog entry this actor was spawned from. Also editable on level-placed instances for authoring preset stages. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "StageCraft|Item", meta = (ExposeOnSpawn = true))
	TObjectPtr<UBaseItemData> ItemData;

	/** Overlay drawn while hovered. Leave empty to disable hover feedback. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "StageCraft|Highlight")
	TObjectPtr<UMaterialInterface> HoverOverlayMaterial;

	/** Overlay drawn while selected. Takes priority over the hover overlay. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "StageCraft|Highlight")
	TObjectPtr<UMaterialInterface> SelectedOverlayMaterial;

private:
	UPROPERTY(Transient, VisibleInstanceOnly, Category = "StageCraft|Interaction")
	bool bIsHovered = false;

	UPROPERTY(Transient, VisibleInstanceOnly, Category = "StageCraft|Interaction")
	bool bIsSelected = false;
};
