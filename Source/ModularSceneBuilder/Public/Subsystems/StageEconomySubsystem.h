// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/TimerHandle.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Data/StageParameterTypes.h"
#include "Economy/StageEconomyTypes.h"
#include "Marketplace/StageMarketplaceTypes.h"
#include "Startup/StageStartupTypes.h"
#include "StageEconomySubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnStageProductCatalogLoaded);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnStagePurchaseCompleted, class UStageProductData*, Product, FStageEconomyResultInfo, Result);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnStageListingPurchaseCompleted, FGuid, ListingId, FStageEconomyResultInfo, Result);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnStageCreditTopUpCompleted, FGameplayTag, Currency, int64, GrantedAmount, FStageEconomyResultInfo, Result);

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

	/** Load progress of the product catalog in [0, 1]: 1 once loaded. For the startup loader (Docs/ADR/0024 §2). */
	float GetProductCatalogLoadProgress() const;

	/** Pushed while the product catalog streams in. Not fired after OnProductCatalogLoaded. */
	FOnStageLoadProgress OnProductCatalogLoadProgress;

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

	// --- Community marketplace listings (Docs/ADR/0021-community-marketplace.md §5) ---

	/**
	 * Everything RequestListingPurchase checks, without buying. The offer is the one the marketplace backend returned
	 * (UStageMarketplaceSubsystem::FindListing), never a price from the caller.
	 */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Economy")
	FStageEconomyResultInfo CanPurchaseListing(FGuid ListingId) const;

	/**
	 * Starts buying a paid listing with credits. Success means accepted and sent to the commerce backend; completion arrives on
	 * OnListingPurchaseCompleted. Real-money listings are refused (ServerRequired) unless the commerce backend can settle them.
	 * Prefer UStageMarketplaceSubsystem::RequestAcquire, which also downloads and imports the listing.
	 */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Economy")
	FStageEconomyResultInfo RequestListingPurchase(FGuid ListingId);

	UFUNCTION(BlueprintPure, Category = "StageCraft|Economy")
	bool IsListingPurchasePending(FGuid ListingId) const;

	/**
	 * Stops waiting for the pending purchase of ListingId (the buyer left the hosted checkout, ADR 0032 §6). The backend answers it as
	 * declined, so it ends through the normal path (OnListingPurchaseCompleted). False when nothing is pending or the backend cannot
	 * cancel (purchases settled at once never wait).
	 */
	bool RequestCancelListingPurchase(FGuid ListingId);

	/** True when the commerce backend is an authoritative server that can settle real money. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Economy")
	bool CanSettleRealMoney() const;

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Economy")
	FOnStageListingPurchaseCompleted OnListingPurchaseCompleted;

	// --- Currency top-ups (Docs/ADR/0022-dcc-bridge.md §11.4) ---

	/** The most one top-up may add. */
	static constexpr int64 MaxTopUpAmount = 1000000;

	/** True when the commerce backend is a store that sells currency. False offline: credits cannot be bought then. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Economy")
	bool CanSellCredits() const;

	/**
	 * Starts buying Amount of Currency from the commerce backend. Success means accepted; completion arrives on
	 * OnCreditTopUpCompleted. Refused as ServerRequired when the backend does not sell currency (always offline), PurchasePending
	 * while one is in flight, IntegrityViolation for a frozen profile, InvalidRequest for an amount outside 1..MaxTopUpAmount or
	 * one that would overflow the balance. The amount granted is re-checked and applied here, the profile's only writer.
	 */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Economy")
	FStageEconomyResultInfo RequestCreditTopUp(FGameplayTag Currency, int64 Amount);

	UFUNCTION(BlueprintPure, Category = "StageCraft|Economy")
	bool IsCreditTopUpPending() const { return PendingTopUp.TransactionId.IsValid(); }

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Economy")
	FOnStageCreditTopUpCompleted OnCreditTopUpCompleted;

	// --- Development only (compiled out of Shipping) ---

	/** Adds currency (negative removes), bypassing the shop. For testing and for future reward systems' prototypes. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Economy|Dev", meta = (DevelopmentOnly))
	bool DevGrantCurrency(FGameplayTag Currency, int64 Amount);

	/** The time the commerce backend has to answer a purchase or top-up (BackendTimeoutSeconds, validated). */
	float GetBackendTimeoutSeconds() const;

	/** Transactions that timed out and are still waiting for a late answer (diagnostics and tests). */
	int32 GetUnresolvedTransactionCount() const { return UnresolvedProducts.Num() + UnresolvedListingOffers.Num() + UnresolvedTopUps.Num(); }

protected:
	/** Settles purchases. Empty uses UStageLocalCommerceBackend. */
	UPROPERTY(Config)
	TSoftClassPtr<class UStageCommerceBackend> CommerceBackendClass;

	/**
	 * Seconds the backend has to answer before the request ends as BackendTimeout (ADR 0021 §14). Without it a backend that
	 * never answers leaves the purchase pending for the rest of the run, and every later request for it is PurchasePending.
	 * Values <= 0 fall back to the default.
	 */
	UPROPERTY(Config)
	float BackendTimeoutSeconds = 30.f;

private:
	/**
	 * Starts the answer deadline of TransactionId: Seconds, or BackendTimeoutSeconds when 0. Called before the backend is asked: it may
	 * answer inside the call.
	 */
	void StartBackendTimer(const FGuid& TransactionId, float Seconds = 0.f);

	/** The deadline Backend asks for to settle Request (UStageCommerceBackend::GetSettlementTimeoutSeconds), clamped to 1 s..1 h. */
	float GetSettlementTimeoutSeconds(const class UStageCommerceBackend& CommerceBackend, const struct FStagePurchaseRequest& Request) const;
	/** Clears the deadline of TransactionId, if it still has one. Idempotent. */
	void StopBackendTimer(const FGuid& TransactionId);
	void HandleBackendTimeout(FGuid TransactionId);

	/** Applies an approved, re-validated product purchase; shared by the on-time and the late answer. */
	FStageEconomyResultInfo CommitProductPurchase(const FGuid& TransactionId, class UStageProductData& Product);
	FStageEconomyResultInfo CommitListingPurchase(const FGuid& TransactionId, const FStageMarketplaceListing& Offer);
	FStageEconomyResultInfo ApplyTopUp(const FStageTopUpRequest& Request, int64 GrantedAmount, const FText& Reason);
	void LoadProductCatalog();
	void HandleProductCatalogLoaded();
	void HandlePurchaseProcessed(bool bApproved, const FText& Reason, FGuid TransactionId, TWeakObjectPtr<class UStageProductData> WeakProduct);
	void HandleListingPurchaseProcessed(bool bApproved, const FText& Reason, FGuid TransactionId);
	void HandleCreditTopUpProcessed(int64 GrantedAmount, const FText& Reason, FGuid TransactionId);

	/** Listing checks shared by CanPurchaseListing and the re-check on settlement (against the offer snapshot). */
	FStageEconomyResultInfo ValidateListingPurchase(const FStageMarketplaceListing* Offer, bool bIgnorePending) const;

	/** Checks shared by CanPurchase and the re-check on settlement. bIgnorePending skips the in-flight check. */
	FStageEconomyResultInfo ValidatePurchase(const class UStageProductData* Product, bool bIgnorePending) const;

	class UStageProfileSubsystem* GetProfile() const;

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageProductData>> Products;

	UPROPERTY(Transient)
	TObjectPtr<class UStageCommerceBackend> Backend = nullptr;

	/** Product -> transaction in flight. Listings use StageListing:<id> ids, so one map guards both kinds. */
	TMap<FPrimaryAssetId, FGuid> PendingPurchases;

	/**
	 * Transaction -> the listing as offered when the purchase was accepted. Settlement re-validates and charges this snapshot, so a
	 * newer marketplace query that replaces the visible listings can neither lose the purchase nor change its price.
	 */
	TMap<FGuid, FStageMarketplaceListing> PendingListingOffers;

	/** The top-up in flight; an invalid TransactionId when none. At most one at a time. */
	FStageTopUpRequest PendingTopUp;

	/** Answer deadline per transaction in flight (purchases, listing offers, the top-up). */
	TMap<FGuid, FTimerHandle> BackendTimers;

	// Timed out, waiting for a late answer: an approval is applied (re-validated), a refusal dropped. Not saved: a restart
	// leaves reconciliation to the server, which keys on the TransactionId.
	TMap<FGuid, TWeakObjectPtr<class UStageProductData>> UnresolvedProducts;
	TMap<FGuid, FStageMarketplaceListing> UnresolvedListingOffers;
	TMap<FGuid, FStageTopUpRequest> UnresolvedTopUps;

	/** The product catalog load in flight; reset once it completes. */
	TSharedPtr<struct FStreamableHandle> ProductCatalogHandle;

	bool bCatalogLoaded = false;
};
