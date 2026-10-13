// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Economy/StageEconomyTypes.h"
#include "SpawnSystemComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStageItemSpawned, class AModularBaseActor*, SpawnedActor);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStageItemDeleted, AActor*, DeletedActor);

/**
 * Decides whether one new item of Item, made of Quantity catalog units (a span's segment count, otherwise 1), may be placed
 * now. Bound by the owning controller, which asks the GameMode.
 */
DECLARE_DELEGATE_RetVal_TwoParams(FStageEconomyResultInfo, FStagePlacementValidator, const class UBaseItemData& /*Item*/, int32 /*Quantity*/);

/**
 * Executes spawns and deletions of stage items. Stateless: it knows nothing about input, cursors,
 * modes or which item is armed. The placement tool (UStagePlacementToolComponent) decides what and
 * where; this component only validates through PlacementValidator and spawns, so UI drag-drop, tools
 * and tests can reuse it.
 */
UCLASS(ClassGroup = (StageCraft), meta = (BlueprintSpawnableComponent))
class MODULARSCENEBUILDER_API USpawnSystemComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	USpawnSystemComponent();

	/**
	 * Spawns exactly one instance of Item at Transform after PlacementValidator approves it.
	 * Returns null (and spawns nothing) on refusal, a missing world or a missing ActorClass.
	 * Game thread only.
	 */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Spawning")
	class AModularBaseActor* SpawnItem(class UBaseItemData* Item, const FTransform& Transform);

	/**
	 * SpawnItem with per-instance setup: Configure runs on the deferred actor before its item data is applied and before it is
	 * validated, so the rules see the configured item (e.g. a span's length and therefore its segment count). Configure must not
	 * keep the actor; it is destroyed again if the rules refuse it.
	 */
	class AModularBaseActor* SpawnConfiguredItem(class UBaseItemData* Item, const FTransform& Transform, TFunctionRef<void(class AModularBaseActor&)> Configure);

	/**
	 * Undo support: recreates a removed item from its snapshot, with the same instance id and SaveGame
	 * state, at the snapshot's transform. Validated by PlacementValidator exactly like SpawnItem, so a
	 * restore can be refused (e.g. a session item limit). Returns null if refused or the catalog item is gone.
	 */
	class AModularBaseActor* RestoreItem(const struct FStageItemSnapshot& Snapshot);

	/**
	 * Destroys the target if it is a stage item (implements IInteractableInterface).
	 * Returns false for anything else, so level geometry can never be deleted by mistake.
	 */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Spawning")
	bool TryDeleteActor(AActor* Target);

	/**
	 * Asked before every spawn. Unbound means no rules apply (tests, tools); in play the controller
	 * always binds it to the GameMode's EvaluatePlacement.
	 */
	FStagePlacementValidator PlacementValidator;

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Spawning")
	FOnStageItemSpawned OnItemSpawned;

	/** Fires just before the actor is destroyed, while it is still valid. */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Spawning")
	FOnStageItemDeleted OnItemDeleted;

private:
	/**
	 * Spawns deferred, runs Prepare (restored id and state, or a caller's configuration) and applies the item data, then asks
	 * PlacementValidator with the prepared item's quantity: refused items are destroyed before they ever begin play, approved ones
	 * are finished, so BeginPlay sees the prepared state. LogVerb names the operation in the log ("Placed", "Restored").
	 */
	class AModularBaseActor* SpawnValidated(class UBaseItemData& Item, const FTransform& Transform, TFunctionRef<void(class AModularBaseActor&)> Prepare, const TCHAR* LogVerb);
};
