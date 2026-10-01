// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "Economy/StageEconomyTypes.h"
#include "StageProductData.generated.h"

/**
 * A shop product: what it costs, what it grants, and what must hold before it can be bought.
 *
 * Products grant entitlements (GameplayTags), never items or parameters directly. Content then
 * declares what each entitlement unlocks (UBaseItemData::RequiredEntitlement and
 * ParameterEntitlements), so one product can unlock a whole pack, several products can sell the
 * same thing (a single item and a bundle), and the shop never needs to know about actors.
 *
 * Adding a product is content-only: create a data asset of this class under /Game/StageCraft/Shop
 * and the Asset Manager registers it (StageProduct entry in DefaultGame.ini). The economy only sells
 * products from that scanned catalog.
 */
UCLASS(BlueprintType, Const)
class MODULARSCENEBUILDER_API UStageProductData : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	static const FPrimaryAssetType ProductAssetType;

	//~ Begin UObject Interface
	virtual FPrimaryAssetId GetPrimaryAssetId() const override;
#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(class FDataValidationContext& Context) const override;
#endif
	//~ End UObject Interface

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Display")
	FText DisplayName;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Display", meta = (MultiLine = true))
	FText Description;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Display", meta = (AssetBundles = "UI"))
	TSoftObjectPtr<class UTexture2D> Icon;

	/** Every amount is charged; an empty price makes the product free (still needs a "purchase" to own). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Economy")
	TArray<FStageCurrencyAmount> Price;

	/** What owning this product means. At least one is required. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Economy", meta = (Categories = "StageCraft.Entitlement"))
	FGameplayTagContainer GrantedEntitlements;

	/** All must be met to buy. */
	UPROPERTY(EditDefaultsOnly, Instanced, BlueprintReadOnly, Category = "Economy")
	TArray<TObjectPtr<class UStageUnlockCondition>> Requirements;
};
