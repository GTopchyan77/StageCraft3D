// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "Data/StageParameterTypes.h"
#include "Economy/StageEconomyTypes.h"
#include "Game/StageSessionTypes.h"
#include "StageCraftGameModeBase.generated.h"

/**
 * Wires the stage editor framework classes together and is the rules authority of a stage
 * session. Set as GlobalDefaultGameMode in DefaultEngine.ini (levels use BP_StageCraftGameMode).
 * Derives from AGameModeBase rather than AGameMode: the editor has no match flow, so AGameMode's
 * match-state machine would be dead weight.
 *
 * Every guarded player request is decided here, never in UI or in the controller:
 *   EvaluatePlacement / EvaluateParameterChange = session rules (SessionRules, via
 *   UStageSessionSubsystem) + ownership (UStageEconomySubsystem, when bEnforceEntitlements).
 * Both are BlueprintNativeEvents, so a Blueprint GameMode can add rules (call the parent first).
 * The GameMode exists only on the authority, which is exactly where these decisions belong once
 * the editor goes multi-user.
 */
UCLASS()
class MODULARSCENEBUILDER_API AStageCraftGameModeBase : public AGameModeBase
{
	GENERATED_BODY()

public:
	AStageCraftGameModeBase();

	//~ Begin AGameModeBase Interface
	virtual void StartPlay() override;
	//~ End AGameModeBase Interface

	/**
	 * May Player place one new item of Item now? Quantity is how many catalog units it consists of (a span's segment count;
	 * 1 for a single piece), so power and weight limits see the whole run.
	 */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "StageCraft|Rules")
	FStageEconomyResultInfo EvaluatePlacement(const APlayerController* Player, const class UBaseItemData* Item, int32 Quantity = 1) const;

	/**
	 * May Player write Value to ParameterId on Target? Besides ownership this enforces the item's own state: a locked item
	 * refuses transform and length edits, a span refuses Scale, and lengthening a span must fit the session limits.
	 */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "StageCraft|Rules")
	FStageEconomyResultInfo EvaluateParameterChange(const APlayerController* Player, const UObject* Target, FGameplayTag ParameterId, const FStageParameterValue& Value) const;

	/**
	 * May Player put Item at NewTransform in one write (an undo or redo step, which restores a whole transform at once)?
	 * NewTransform must be sane (StageTransformRules::IsSaneTransform); then each part that differs from the item's current
	 * transform (Location, Rotation, Scale) is judged by EvaluateParameterChange, so a lock, a span's refusal of Scale and
	 * ownership apply exactly as for an inspector edit, including any Blueprint override. Unchanged parts are not judged.
	 * Not a BlueprintNativeEvent itself: it composes EvaluateParameterChange, which is the extension point.
	 */
	FStageEconomyResultInfo EvaluateTransformChange(const APlayerController* Player, const class AModularBaseActor& Item, const FTransform& NewTransform) const;

	/** May Player import model files into the Library? Off when SessionRules.bAllowModelImport is false. */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "StageCraft|Rules")
	FStageEconomyResultInfo EvaluateModelImport(const APlayerController* Player) const;

	UFUNCTION(BlueprintPure, Category = "StageCraft|Rules")
	const FStageSessionRules& GetSessionRules() const { return SessionRules; }

protected:
	/** Handed to UStageSessionSubsystem when play starts. Different modes are different defaults here. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "StageCraft|Rules", meta = (ShowOnlyInnerProperties))
	FStageSessionRules SessionRules;

private:
	/** The rules a placed item's own state imposes on an edit (lock, scale, quantity limits), independent of ownership. */
	FStageEconomyResultInfo EvaluateItemEdit(const class AModularBaseActor& Item, const FGameplayTag& ParameterId, const FStageParameterValue& Value) const;

	class UStageSessionSubsystem* GetSession() const;
	class UStageEconomySubsystem* GetEconomy() const;
};
