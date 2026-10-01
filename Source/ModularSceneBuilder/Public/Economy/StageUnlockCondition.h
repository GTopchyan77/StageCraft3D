// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "UObject/Object.h"
#include "StageUnlockCondition.generated.h"

/**
 * One requirement a product has before it can be bought, edited inline on the product asset.
 *
 * Extensible without touching the economy: subclass in C++ (override IsMet) or in Blueprint (the
 * class is Blueprintable; implement Is Met). Conditions only read the profile, never change it, so
 * evaluating them is side-effect free and safe to call from UI every frame.
 */
UCLASS(Abstract, Blueprintable, EditInlineNew, DefaultToInstanced, CollapseCategories)
class MODULARSCENEBUILDER_API UStageUnlockCondition : public UObject
{
	GENERATED_BODY()

public:
	/** True when the requirement holds. On false, OutReason says what is missing, in player terms. */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "StageCraft|Economy")
	bool IsMet(const class UStageProfileSubsystem* Profile, FText& OutReason) const;
};

/** Requires owning every listed entitlement first (e.g. the base pack before an expansion). */
UCLASS(meta = (DisplayName = "Requires Entitlements"))
class MODULARSCENEBUILDER_API UStageUnlockCondition_Entitlements : public UStageUnlockCondition
{
	GENERATED_BODY()

public:
	virtual bool IsMet_Implementation(const class UStageProfileSubsystem* Profile, FText& OutReason) const override;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Condition", meta = (Categories = "StageCraft.Entitlement"))
	FGameplayTagContainer RequiredEntitlements;
};

/** Requires a minimum profile level. */
UCLASS(meta = (DisplayName = "Requires Profile Level"))
class MODULARSCENEBUILDER_API UStageUnlockCondition_ProfileLevel : public UStageUnlockCondition
{
	GENERATED_BODY()

public:
	virtual bool IsMet_Implementation(const class UStageProfileSubsystem* Profile, FText& OutReason) const override;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Condition", meta = (ClampMin = "1"))
	int32 MinLevel = 1;
};
