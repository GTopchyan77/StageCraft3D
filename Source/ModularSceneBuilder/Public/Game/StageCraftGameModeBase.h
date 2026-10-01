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

	/** May Player place Item now? */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "StageCraft|Rules")
	FStageEconomyResultInfo EvaluatePlacement(const APlayerController* Player, const class UBaseItemData* Item) const;

	/** May Player write Value to ParameterId on Target? */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "StageCraft|Rules")
	FStageEconomyResultInfo EvaluateParameterChange(const APlayerController* Player, const UObject* Target, FGameplayTag ParameterId, const FStageParameterValue& Value) const;

	UFUNCTION(BlueprintPure, Category = "StageCraft|Rules")
	const FStageSessionRules& GetSessionRules() const { return SessionRules; }

protected:
	/** Handed to UStageSessionSubsystem when play starts. Different modes are different defaults here. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "StageCraft|Rules", meta = (ShowOnlyInnerProperties))
	FStageSessionRules SessionRules;

private:
	class UStageSessionSubsystem* GetSession() const;
	class UStageEconomySubsystem* GetEconomy() const;
};
