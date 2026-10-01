// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Data/StageParameterTypes.h"
#include "Economy/StageEconomyTypes.h"
#include "StageEconomySubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnStageProductCatalogLoaded);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnStagePurchaseCompleted, class UStageProductData*, Product, FStageEconomyResultInfo, Result);

/**
 * The economy's rules and its only writer: product catalog, ownership queries, purchase
 * validation and settlement, and the entitlement checks behind every guarded action.
 *
 * Security model (offline-first, marketplace-ready):
 *  - Single writer. UStageProfileSubsystem exposes no public mutation; this class is its friend.
 *  - Validate, then settle, then apply. A purchase is checked here (integrity, catalog membership,
 *    ownership, unlock conditions, funds), settled by the UStageCommerceBackend, re-checked when
 *    the backend answers (state can change meanwhile), and applied to the profile atomically.
 *  - Catalog-only. Only products found by the Asset Manager scan are sold, so a product object
 *    built at runtime (by a mod, a script, a malformed request) is rejected as UnknownProduct.
 *  - One in flight per product. A second request while one is pending is PurchasePending, so a
 *    double click cannot double-charge.
 *  - Deny by default. A failed or unanswerable check is a refusal, never a pass.
 *
 * What it guards beyond purchases: items need UBaseItemData::RequiredEntitlement to be placed or
 * switched to, and parameters listed in UBaseItemData::ParameterEntitlements need theirs to be
 * edited. Content without those fields is Free, so the economy is opt-in per asset.
 */
UCLASS(Config = Game)
class MODULARSCENEBUILDER_API UStageEconomySubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem Interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	// --- Product catalog ---

	UFUNCTION(BlueprintPure, Category = "StageCraft|Economy")
	bool IsProductCatalogLoaded() const { return bCatalogLoaded; }

	/** Sorted by display name. Empty until OnProductCatalogLoaded. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Economy")
	const TArray<class UStageProductData*>& GetProducts() const { return ObjectPtrDecay(Products); }

	UFUNCTION(BlueprintPure, Category = "StageCraft|Economy")
	class UStageProductData* FindProduct(FPrimaryAssetId ProductId) const;

	/** Products that grant the entitlement, e.g. for an "Unlock" button next to a locked item. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Economy")
	TArray<class UStageProductData*> GetProductsGranting(FGameplayTag Entitlement) const;

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Economy")
	FOnStageProductCatalogLoaded OnProductCatalogLoaded;

	// --- Ownership ---

	/** An invalid (empty) tag is Free. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Economy")
	EStageOwnershipState GetEntitlementState(FGameplayTag Entitlement) const;

	UFUNCTION(BlueprintPure, Category = "StageCraft|Economy")
	EStageOwnershipState GetItemState(const class UBaseItemData* Item) const;

	/** State of editing ParameterId on instances of Item. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Economy")
	EStageOwnershipState GetParameterState(const class UBaseItemData* Item, FGameplayTag ParameterId) const;

	static bool IsUsable(EStageOwnershipState State) { return State == EStageOwnershipState::Free || State == EStageOwnershipState::Owned; }

	// --- Guarded actions (called by the GameMode's rule checks) ---

	/** Placing Item, or switching an instance to it. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Economy")
	FStageEconomyResultInfo ValidateItemUse(const class UBaseItemData* Item) const;

	/** Writing Value to ParameterId on Target. Also guards the Type dropdown (switching to a locked item). */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Economy")
	FStageEconomyResultInfo ValidateParameterChange(const UObject* Target, FGameplayTag ParameterId, const FStageParameterValue& Value) const;

	// --- Purchases ---

	/** Everything RequestPurchase checks, without buying. Use it to enable or disable Buy buttons. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Economy")
	FStageEconomyResultInfo CanPurchase(const class UStageProductData* Product) const;

	/**
	 * Starts a purchase. The returned result is the validation outcome: Success means accepted and
	 * sent to the backend, not completed. Completion (success or failure) arrives on OnPurchaseCompleted.
	 * Prefer AModularPlayerController::RequestPurchase from UI.
	 */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Economy")
	FStageEconomyResultInfo RequestPurchase(class UStageProductData* Product);

	UFUNCTION(BlueprintPure, Category = "StageCraft|Economy")
	bool IsPurchasePending(const class UStageProductData* Product) const;

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Economy")
	FOnStagePurchaseCompleted OnPurchaseCompleted;

	// --- Development only (compiled out of Shipping) ---

	/** Adds currency (negative removes), bypassing the shop. For testing and for future reward systems' prototypes. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Economy|Dev", meta = (DevelopmentOnly))
	bool DevGrantCurrency(FGameplayTag Currency, int64 Amount);

protected:
	/** Settles purchases. Empty uses UStageLocalCommerceBackend. */
	UPROPERTY(Config)
	TSoftClassPtr<class UStageCommerceBackend> CommerceBackendClass;

private:
	void LoadProductCatalog();
	void HandleProductCatalogLoaded();
	void HandlePurchaseProcessed(bool bApproved, const FText& Reason, FGuid TransactionId, TWeakObjectPtr<class UStageProductData> WeakProduct);

	/** Checks shared by CanPurchase and the re-check on settlement. bIgnorePending skips the in-flight check. */
	FStageEconomyResultInfo ValidatePurchase(const class UStageProductData* Product, bool bIgnorePending) const;

	class UStageProfileSubsystem* GetProfile() const;

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageProductData>> Products;

	UPROPERTY(Transient)
	TObjectPtr<class UStageCommerceBackend> Backend = nullptr;

	/** Product -> transaction in flight. */
	TMap<FPrimaryAssetId, FGuid> PendingPurchases;

	bool bCatalogLoaded = false;
};
