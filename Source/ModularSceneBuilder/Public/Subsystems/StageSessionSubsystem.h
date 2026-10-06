// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Data/StageParameterTypes.h"
#include "Economy/StageEconomyTypes.h"
#include "Game/StageSessionTypes.h"
#include "StageSessionSubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStageSessionStatsChanged, const FStageSessionStats&, Stats);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStageSessionDirtyChanged, bool, bIsDirty);

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

	// --- Rules support ---

	/** Would adding Item break MaxPlacedItems, MaxTotalPowerWatts or MaxTotalWeightKg? */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Session")
	FStageEconomyResultInfo EvaluateSessionLimits(const class UBaseItemData* Item) const;

	// --- Applying edits (after the GameMode allowed them) ---

	/** Writes the value through IStageParameterInterface and marks the session dirty. Returns whether the target accepted it. */
	bool ApplyParameterChange(UObject* Target, const FGameplayTag& ParameterId, const FStageParameterValue& Value);

	UFUNCTION(BlueprintPure, Category = "StageCraft|Session")
	bool IsDirty() const { return bDirty; }

	/** Called by whatever saves the stage (show file / level save) once the state is written. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Session")
	void MarkClean();

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Session")
	FOnStageSessionDirtyChanged OnDirtyChanged;

protected:
	//~ Begin UWorldSubsystem Interface
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	//~ End UWorldSubsystem Interface

private:
	UFUNCTION()
	void HandleItemParameterChanged(class AModularBaseActor* Item, FGameplayTag ParameterId);

	void RecomputeStats();
	void SetDirty(bool bNewDirty);

	TArray<TWeakObjectPtr<class AModularBaseActor>> Items;
	FStageSessionRules Rules;
	FStageSessionStats Stats;
	bool bSessionStarted = false;
	bool bDirty = false;
};
