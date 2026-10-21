// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Data/StageParameterTypes.h"
#include "Economy/StageEconomyTypes.h"
#include "Environment/StageEnvironmentTypes.h"
#include "Game/StageSessionTypes.h"
#include "StageSessionSubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStageSessionStatsChanged, const FStageSessionStats&, Stats);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStageSessionDirtyChanged, bool, bIsDirty);

/** An item was added or removed, or changed how it is listed (label, type, visibility, lock, quantity). Views rebuild on the next tick. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnStageItemListChanged);

/**
 * State of the stage being built in this world: which items are placed, their running totals
 * (power, weight), the session's rules, and whether there are unsaved edits.
 *
 * A world subsystem because all of it belongs to one level and must reset when the level changes,
 * while the player's profile and wallet (GameInstance subsystems) carry over. It is also the one
 * place parameter edits are applied (ApplyParameterChange), so undo/redo, dirty tracking and
 * future replication have a single hook.
 *
 * It holds rules but does not decide on them: AStageCraftGameModeBase evaluates requests (using
 * EvaluateSessionLimits and the economy) and only then asks this subsystem to apply them.
 * Items register themselves on BeginPlay, so nothing iterates the world.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageSessionSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem Interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	/** Called by the GameMode when play starts. */
	void BeginSession(const FStageSessionRules& InRules);

	UFUNCTION(BlueprintPure, Category = "StageCraft|Session")
	const FStageSessionRules& GetRules() const { return Rules; }

	UFUNCTION(BlueprintPure, Category = "StageCraft|Session")
	bool HasSessionStarted() const { return bSessionStarted; }

	// --- Placed items ---

	/** Called by AModularBaseActor::BeginPlay / EndPlay. */
	void RegisterItem(class AModularBaseActor* Item);
	void UnregisterItem(class AModularBaseActor* Item);

	UFUNCTION(BlueprintCallable, Category = "StageCraft|Session")
	TArray<class AModularBaseActor*> GetPlacedItems() const;

	/** The live placed item with this instance id (AModularBaseActor::GetInstanceId), or null. */
	class AModularBaseActor* FindItemById(const FGuid& InstanceId) const;

	UFUNCTION(BlueprintPure, Category = "StageCraft|Session")
	const FStageSessionStats& GetStats() const { return Stats; }

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Session")
	FOnStageSessionStatsChanged OnStatsChanged;

	/** The outliner and the bill of materials listen here instead of binding every item. */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Session")
	FOnStageItemListChanged OnItemListChanged;

	// --- Rules support ---

	/**
	 * Would adding one new item of Quantity units of Item (a span of Quantity segments; 1 for a single piece) break
	 * MaxPlacedItems, MaxTotalPowerWatts or MaxTotalWeightKg? A Quantity below 1 is an invalid request.
	 */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Session")
	FStageEconomyResultInfo EvaluateSessionLimits(const class UBaseItemData* Item, int32 Quantity = 1) const;

	/**
	 * Would AddedQuantity more units of Item on an item already placed (a longer span) break the power or weight limit?
	 * Zero or negative (a shorter span) is always allowed.
	 */
	FStageEconomyResultInfo EvaluateQuantityIncrease(const class UBaseItemData* Item, int32 AddedQuantity) const;

	// --- Applying edits (after the GameMode allowed them) ---

	/** Writes the value through IStageParameterInterface and marks the session dirty. Returns whether the target accepted it. */
	bool ApplyParameterChange(UObject* Target, const FGameplayTag& ParameterId, const FStageParameterValue& Value);

	UFUNCTION(BlueprintPure, Category = "StageCraft|Session")
	bool IsDirty() const { return bDirty; }

	/** Called by whatever saves the stage (show file / level save) once the state is written. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Session")
	void MarkClean();

	/**
	 * The stage differs from its scene file without an edit having happened: a load that skipped or repaired items
	 * (Docs/ADR/0013-preferences-key-bindings-and-audit-fixes.md §3.1). Marks it dirty, so autosave and the exit prompt cover it.
	 */
	void MarkUnsaved();

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Session")
	FOnStageSessionDirtyChanged OnDirtyChanged;

	/** The scene file this stage was last saved as or loaded from; empty while the stage has never been saved. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Session")
	const FString& GetSceneName() const { return SceneName; }

	/** Called by UStageSceneComponent after a successful save or load, together with MarkClean. */
	void SetSceneName(const FString& InSceneName);

	/**
	 * Increases with every edit that marks the session dirty. A save captures it when it starts and marks the session clean
	 * only if it is unchanged when the file has been written, so edits made during a background save are never lost track of.
	 */
	uint64 GetEditSerial() const { return EditSerial; }

protected:
	//~ Begin UWorldSubsystem Interface
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	//~ End UWorldSubsystem Interface

private:
	UFUNCTION()
	void HandleItemParameterChanged(class AModularBaseActor* Item, FGameplayTag ParameterId);

	/** The outdoor environment is part of the scene file (format version 2), so changing it is an unsaved edit. */
	UFUNCTION()
	void HandleEnvironmentChanged(const FStageEnvironmentSettings& Settings);

	TWeakObjectPtr<class UStageEnvironmentSubsystem> BoundEnvironment;

	/** Cues are part of the scene file (format version 3), so storing, editing or deleting one is an unsaved edit. */
	UFUNCTION()
	void HandleCueListChanged();

	TWeakObjectPtr<class UShowControlSubsystem> BoundShowControl;

	void RecomputeStats();
	void SetDirty(bool bNewDirty);

	/** The power and weight limits for Quantity more units of Item; shared by new items and longer spans. */
	FStageEconomyResultInfo EvaluateResourceLimits(const class UBaseItemData& Item, int32 Quantity) const;

	TArray<TWeakObjectPtr<class AModularBaseActor>> Items;
	FStageSessionRules Rules;
	FStageSessionStats Stats;
	FString SceneName;
	uint64 EditSerial = 0;
	bool bSessionStarted = false;
	bool bDirty = false;
};
