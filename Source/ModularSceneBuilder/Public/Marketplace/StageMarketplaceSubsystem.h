// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Economy/StageEconomyTypes.h"
#include "Marketplace/StageMarketplaceBackend.h"
#include "Marketplace/StageMarketplaceTypes.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tasks/Pipe.h"
#include "StageMarketplaceSubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStageMarketplaceListingsUpdated, FStageMarketplaceResultInfo, Result);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnStageMarketplaceAcquireCompleted, FGuid, ListingId, FStageMarketplaceResultInfo, Result, const FString&, SceneName);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnStageMarketplacePublishCompleted, FStageMarketplaceResultInfo, Result, const FStageMarketplaceListing&, Listing);

/**
 * The community marketplace on this client (Docs/ADR/0021-community-marketplace.md §4): browsing, acquiring (download and
 * import) and publishing `.stage` packages through a UStageMarketplaceBackend.
 *
 * On the GameInstance, because the visible listings and requests in flight outlive a level. It decides nothing about money: paid
 * listings are bought through UStageEconomySubsystem::RequestListingPurchase, the economy's only listing operation, and imported
 * when that completes. It never spawns actors: an acquired package becomes a saved scene, opened through the existing scene load
 * (catalog, entitlements, placement rules, undo and the unsaved-changes prompt all apply).
 *
 * Push, never poll: results arrive on OnListingsUpdated, OnAcquireCompleted and OnPublishCompleted. Requests return at once with
 * either a refusal or Success (accepted; the outcome follows on the delegate).
 */
UCLASS(Config = Game)
class MODULARSCENEBUILDER_API UStageMarketplaceSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem Interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	// --- Browsing ---

	/** Starts a query. A newer query supersedes one still running (its answer is dropped). Results arrive on OnListingsUpdated. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Marketplace")
	void RequestQuery(const FStageMarketplaceQuery& Query);

	/** The page of the last answered query. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Marketplace")
	const TArray<FStageMarketplaceListing>& GetListings() const { return Listings; }

	/** Matches of the last answered query before paging. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Marketplace")
	int32 GetTotalMatches() const { return TotalMatches; }

	UFUNCTION(BlueprintPure, Category = "StageCraft|Marketplace")
	bool IsQueryInFlight() const { return bQueryInFlight; }

	/** A listing of the last answered query, or null. The economy reads offers here, never from callers. */
	const FStageMarketplaceListing* FindListing(const FGuid& ListingId) const;

	/** Free, Owned, Purchasable or Locked (real money without a server, frozen profile). Locked also for an unknown listing. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Marketplace")
	EStageOwnershipState GetListingOwnership(FGuid ListingId) const;

	// --- Acquiring ---

	/**
	 * Gets a listing of the last query: free and owned listings are downloaded and imported; paid ones are bought with credits
	 * first. One acquisition per listing at a time. Completion arrives on OnAcquireCompleted with the imported scene's name.
	 * Prefer AModularPlayerController's marketplace request from UI.
	 */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Marketplace")
	FStageMarketplaceResultInfo RequestAcquire(FGuid ListingId);

	UFUNCTION(BlueprintPure, Category = "StageCraft|Marketplace")
	bool IsAcquiring(FGuid ListingId) const { return PendingAcquires.Contains(ListingId); }

	// --- Publishing ---

	/** Publishes a saved scene. One publish at a time. Completion arrives on OnPublishCompleted with the new listing. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Marketplace")
	FStageMarketplaceResultInfo RequestPublish(const FStageMarketplacePublishRequest& Request);

	UFUNCTION(BlueprintPure, Category = "StageCraft|Marketplace")
	bool IsPublishing() const { return bPublishing; }

	/** User-facing name of the backend ("Community folder"). */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Marketplace")
	FText GetBackendName() const;

	/** Where copies of acquired packages are kept (Saved/StageCraft/Marketplace/Library). */
	static FString GetLibraryDirectory();

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Marketplace")
	FOnStageMarketplaceListingsUpdated OnListingsUpdated;

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Marketplace")
	FOnStageMarketplaceAcquireCompleted OnAcquireCompleted;

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Marketplace")
	FOnStageMarketplacePublishCompleted OnPublishCompleted;

protected:
	/** Where listings come from. Empty uses UStageLocalMarketplaceBackend. */
	UPROPERTY(Config)
	TSoftClassPtr<class UStageMarketplaceBackend> MarketplaceBackendClass;

private:
	void HandleQueried(EStageMarketplaceResult Result, const FStageMarketplaceQueryResult& Page, uint32 Serial);

	UFUNCTION()
	void HandleListingPurchaseCompleted(FGuid ListingId, FStageEconomyResultInfo Result);

	void StartDownload(const FGuid& ListingId);
	void HandlePackageFetched(EStageMarketplaceResult Result, const FStageMarketplacePackageBytes& Bytes, FGuid ListingId);
	void FinishAcquire(const FGuid& ListingId, const FStageMarketplaceResultInfo& Result, const FString& SceneName);

	void HandlePackageBuilt(EStageMarketplaceResult Result, const FStageMarketplacePackageBytes& Bytes);
	void HandlePublished(EStageMarketplaceResult Result, const FStageMarketplaceListing& Listing);
	void FinishPublish(const FStageMarketplaceResultInfo& Result, const FStageMarketplaceListing& Listing);

	class UStageEconomySubsystem* GetEconomy() const;
	class UStageProfileSubsystem* GetProfile() const;

	UPROPERTY(Transient)
	TObjectPtr<class UStageMarketplaceBackend> Backend = nullptr;

	/** The page of the last answered query: the only offers that can be acquired or bought. */
	TArray<FStageMarketplaceListing> Listings;
	int32 TotalMatches = 0;

	/** Increments per query; an answer with an older serial is stale. */
	uint32 QuerySerial = 0;
	bool bQueryInFlight = false;

	/** Listings being bought, downloaded or imported. */
	TSet<FGuid> PendingAcquires;
	bool bPublishing = false;

	/** Package verification, scene writes and package builds run here one at a time, so two imports never pick the same scene name. */
	UE::Tasks::FPipe WorkPipe{ UE_SOURCE_LOCATION };
};
