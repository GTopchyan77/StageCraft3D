// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "Data/StageParameterTypes.h"
#include "StageParameterInterface.generated.h"

UINTERFACE(MinimalAPI, BlueprintType)
class UStageParameterInterface : public UInterface
{
	GENERATED_BODY()
};

/**
 * Anything whose per-instance state the inspector can show and edit. Kept separate from
 * IInteractableInterface so non-selectable objects (e.g. a global show settings object) can be
 * inspected too, and so input code does not depend on the parameter model.
 *
 * From C++, call through the Execute_ wrappers so Blueprint implementers work as well.
 */
class MODULARSCENEBUILDER_API IStageParameterInterface
{
	GENERATED_BODY()

public:
	/** Full description of the editable state, grouped into sections. Called when the inspector (re)builds. */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "StageCraft|Parameters")
	TArray<FStageParameterSection> GetParameterSections() const;

	/** Cheap single-value read used for live row refreshes. Returns false for unknown ids. */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "StageCraft|Parameters")
	bool GetParameterValue(FGameplayTag ParameterId, FStageParameterValue& OutValue) const;

	/** Applies a value, clamped to the parameter's limits. Returns false for unknown or read-only ids or a mismatched type. */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "StageCraft|Parameters")
	bool SetParameterValue(FGameplayTag ParameterId, const FStageParameterValue& Value);
};
