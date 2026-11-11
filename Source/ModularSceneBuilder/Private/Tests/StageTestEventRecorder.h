// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Bridge/StageBridgeTypes.h"
#include "Economy/StageEconomyTypes.h"
#include "Fixtures/StageGdtfShareTypes.h"
#include "Marketplace/StageDispatchTypes.h"
#include "Marketplace/StageMarketplaceTypes.h"
#include "Services/StageAIToolTypes.h"
#include "UObject/Object.h"
#include "StageTestEventRecorder.generated.h"

/**
 * Automation tests only: records what the subsystems of a test game instance broadcast, so an integration test can check the
 * outcome a view would have been told (dynamic delegates take UFUNCTIONs, not lambdas). Bind once after the game instance
 * started; Unbind before it shuts down (the destructor of the test's scope is too late for a clean unbind).
 *
 * Ships as an unreachable class like UStageTestCommerceBackend (UHT cannot compile a UCLASS out). Game thread only.
 */
UCLASS(NotBlueprintable, Transient, HideDropdown)
class UStageTestEventRecorder : public UObject
{
	GENERATED_BODY()

public:
	void Bind(class UGameInstance& GameInstance);
	void Unbind();

	struct FListingPurchase { FGuid ListingId; FStageEconomyResultInfo Result; };
	struct FTopUp { int64 Granted = 0; FStageEconomyResultInfo Result; };
	struct FAcquire { FGuid ListingId; FStageMarketplaceResultInfo Result; FString SceneName; };

	TArray<FListingPurchase> ListingPurchases;
	TArray<FStageEconomyResultInfo> ProductPurchases;
	TArray<FTopUp> TopUps;
	TArray<FAcquire> Acquires;
	/** Marketplace publish, unpublish and library results (SceneName unused). */
	TArray<FAcquire> Publishes;
	TArray<FAcquire> Unpublishes;
	TArray<FAcquire> LibraryChanges;
	TArray<FStageOrderResultInfo> Orders;
	TArray<FStageBridgeResultInfo> Sends;
	TArray<FStageAIToolResultInfo> ToolJobs;
	TArray<FStageGdtfShareResultInfo> GdtfShareResults;

private:
	UFUNCTION()
	void HandleListingPurchase(FGuid ListingId, FStageEconomyResultInfo Result);
	UFUNCTION()
	void HandleProductPurchase(class UStageProductData* Product, FStageEconomyResultInfo Result);
	UFUNCTION()
	void HandleTopUp(FGameplayTag Currency, int64 GrantedAmount, FStageEconomyResultInfo Result);
	UFUNCTION()
	void HandleAcquire(FGuid ListingId, FStageMarketplaceResultInfo Result, const FString& SceneName);
	UFUNCTION()
	void HandlePublish(FStageMarketplaceResultInfo Result, const FStageMarketplaceListing& Listing);
	UFUNCTION()
	void HandleUnpublish(FGuid ListingId, FStageMarketplaceResultInfo Result);
	UFUNCTION()
	void HandleLibraryChanged(FGuid ListingId, FStageMarketplaceResultInfo Result);
	UFUNCTION()
	void HandleOrder(const FStageOrderResultInfo& Result);
	UFUNCTION()
	void HandleSend(const FStageBridgeResultInfo& Result);
	UFUNCTION()
	void HandleToolJob(const FStageAIToolResultInfo& Result);
	UFUNCTION()
	void HandleGdtfShare(const FStageGdtfShareResultInfo& Result);

	TWeakObjectPtr<class UGameInstance> Bound;
};
