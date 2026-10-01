// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Interaction/InteractableInterface.h"
#include "Interaction/StageParameterInterface.h"
#include "ModularBaseActor.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnModularActorSelectionChanged, class AModularBaseActor*, Actor, bool, bSelected);

/** ParameterId is the row that changed. An invalid tag means "structure changed, rebuild everything". */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnStageParameterChanged, class AModularBaseActor*, Actor, FGameplayTag, ParameterId);

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
class MODULARSCENEBUILDER_API AModularBaseActor : public AActor, public IInteractableInterface, public IStageParameterInterface
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
	virtual void InitializeFromItemData(class UBaseItemData* InItemData);

	UFUNCTION(BlueprintPure, Category = "StageCraft|Item")
	class UBaseItemData* GetItemData() const { return ItemData; }

	/**
	 * Catalog items this instance can be switched to in place (the inspector's Type dropdown, whose
	 * option index is the index here): same data class and item type as the current ItemData, which
	 * is always included. Public so rule checks can resolve a Type option to the item it selects.
	 */
	void GetSwappableItems(TArray<class UBaseItemData*>& OutItems) const;

	UFUNCTION(BlueprintPure, Category = "StageCraft|Item")
	class UStaticMeshComponent* GetMeshComponent() const { return MeshComponent; }

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

	/**
	 * Fires whenever an inspector-visible value changes, from any source: the inspector itself,
	 * the gizmo (transform), cue playback or incoming DMX. The inspector binds here for live rows.
	 */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Parameters")
	FOnStageParameterChanged OnParameterChanged;

	/** User-facing instance name, e.g. "Spot 101" or "Main L 03". Falls back to the catalog name when empty. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Item")
	FText GetInstanceLabel() const;

	//~ Begin IInteractableInterface
	virtual void OnHoverBegin_Implementation() override;
	virtual void OnHoverEnd_Implementation() override;
	virtual void OnSelect_Implementation() override;
	virtual void OnDeselect_Implementation() override;
	virtual FStageItemInteractionDetails GetInteractionDetails_Implementation() const override;
	//~ End IInteractableInterface

	//~ Begin IStageParameterInterface
	virtual TArray<FStageParameterSection> GetParameterSections_Implementation() const override;
	virtual bool GetParameterValue_Implementation(FGameplayTag ParameterId, FStageParameterValue& OutValue) const override;
	virtual bool SetParameterValue_Implementation(FGameplayTag ParameterId, const FStageParameterValue& Value) override;
	//~ End IStageParameterInterface

protected:
	//~ Begin AActor Interface
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	//~ End AActor Interface

	/**
	 * Parameter model hooks. Subclasses call Super first so Info and Transform stay at the top,
	 * then append their own sections. Read/Write return true when they handled the id.
	 */
	virtual void GatherParameterSections(TArray<FStageParameterSection>& OutSections) const;
	virtual bool ReadParameter(const FGameplayTag& ParameterId, FStageParameterValue& OutValue) const;
	virtual bool WriteParameter(const FGameplayTag& ParameterId, const FStageParameterValue& Value);

	void NotifyParameterChanged(const FGameplayTag& ParameterId);

	/**
	 * Pushes data-driven visuals onto components. Subclasses call Super and then apply their own
	 * fields. Must be idempotent because it runs from both InitializeFromItemData and OnConstruction.
	 */
	virtual void ApplyItemData(const class UBaseItemData& Data);

	/** Refreshes hover/selection feedback. Override to highlight extra components. */
	virtual void UpdateHighlight();

	/** Blueprint hook that runs after the C++ ApplyItemData, for per-item setup without C++. */
	UFUNCTION(BlueprintImplementableEvent, Category = "StageCraft|Item", meta = (DisplayName = "On Item Data Applied"))
	void BP_OnItemDataApplied(const class UBaseItemData* AppliedData);

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<class UStaticMeshComponent> MeshComponent = nullptr;

	/** Catalog entry this actor was spawned from. Also editable on level-placed instances for authoring preset stages. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "StageCraft|Item", meta = (ExposeOnSpawn = true))
	TObjectPtr<class UBaseItemData> ItemData = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, SaveGame, Category = "StageCraft|Item")
	FText InstanceLabel;

	/** Overlay drawn while hovered. Leave empty to disable hover feedback. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "StageCraft|Highlight")
	TObjectPtr<class UMaterialInterface> HoverOverlayMaterial = nullptr;

	/** Overlay drawn while selected. Takes priority over the hover overlay. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "StageCraft|Highlight")
	TObjectPtr<class UMaterialInterface> SelectedOverlayMaterial = nullptr;

private:
	/** Turns gizmo drags (and any other move) into Transform parameter notifications. */
	void HandleRootTransformUpdated(class USceneComponent* UpdatedComponent, EUpdateTransformFlags UpdateTransformFlags, ETeleportType Teleport);

	FDelegateHandle RootTransformUpdatedHandle;

	UPROPERTY(Transient, VisibleInstanceOnly, Category = "StageCraft|Interaction")
	bool bIsHovered = false;

	UPROPERTY(Transient, VisibleInstanceOnly, Category = "StageCraft|Interaction")
	bool bIsSelected = false;
};
