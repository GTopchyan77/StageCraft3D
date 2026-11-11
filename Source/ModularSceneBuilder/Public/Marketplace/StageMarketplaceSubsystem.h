// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Economy/StageEconomyTypes.h"
#include "Engine/TimerHandle.h"
#include "Marketplace/StageMarketplaceBackend.h"
#include "Marketplace/StageMarketplaceTypes.h"
#include "Scene/StageSceneTypes.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tasks/Pipe.h"
#include "StageMarketplaceSubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStageMarketplaceListingsUpdated, FStageMarketplaceResultInfo, Result);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnStageMarketplaceAcquireCompleted, FGuid, ListingId, FStageMarketplaceResultInfo, Result, const FString&, SceneName);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnStageMarketplacePublishCompleted, FStageMarketplaceResultInfo, Result, const FStageMarketplaceListing&, Listing);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnStageMarketplaceUnpublishCompleted, FGuid, ListingId, FStageMarketplaceResultInfo, Result);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnStageMarketplaceLibraryChanged, FGuid, ListingId, FStageMarketplaceResultInfo, Result);

using FStageMarketplacePreviewScene = TSharedPtr<const FStageSceneDocument, ESPMode::ThreadSafe>;
/** Native (C++ views only): the verified scene of a previewed listing, or null with a failure. */
DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnStageMarketplacePreviewReady, const FGuid& /*ListingId*/, const FStageMarketplaceResultInfo& /*Result*/,
	const FStageMarketplacePreviewScene& /*Scene*/);

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

	/** The query whose answer is in GetListings() (sanitized), whoever sent it: views follow it (ADR 0021 §13.5). */
	const FStageMarketplaceQuery& GetAnsweredQuery() const { return AnsweredQuery; }

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

	// --- Read-only preview (Docs/ADR/0021-community-marketplace.md §12.3) ---

	/**
	 * Fetches a listing of the last query for the read-only preview, free or paid (viewing grants nothing), and verifies the whole
	 * package exactly as an import does. Nothing is written and it is not counted as a download. A newer preview supersedes one
	 * still loading. The verified scene arrives on OnPreviewReady.
	 */
	FStageMarketplaceResultInfo RequestPreview(FGuid ListingId);

	bool IsPreviewLoading() const { return bPreviewLoading; }

	FOnStageMarketplacePreviewReady OnPreviewReady;

	// --- Publishing ---

	/** Publishes a saved scene. One publish at a time. Completion arrives on OnPublishCompleted with the new listing. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Marketplace")
	FStageMarketplaceResultInfo RequestPublish(const FStageMarketplacePublishRequest& Request);

	UFUNCTION(BlueprintPure, Category = "StageCraft|Marketplace")
	bool IsPublishing() const { return bPublishing; }

	// --- Removing what was put in (Docs/ADR/0021-community-marketplace.md §15) ---

	/**
	 * Takes a listing this install published out of the marketplace. NotYours for any other listing (the local publish record
	 * decides: a folder has no accounts), Busy while another unpublish runs, InUse while it is being acquired here. People who
	 * already got it keep their copy. Completion arrives on OnUnpublishCompleted; the listings are queried again after a success.
	 * Prefer AModularPlayerController's request from UI.
	 */
	FStageMarketplaceResultInfo RequestUnpublish(FGuid ListingId);

	/** Whether this install published ListingId (its local publish record). Memory only. */
	bool IsPublishedByMe(const FGuid& ListingId) const { return PublishedByMe.Contains(ListingId); }

	/**
	 * Deletes the library copy of an acquired listing (Marketplace/Library/<id>.stage). Ownership and purchase history stay
	 * (they are the economy's; a paid listing can be got again for free while it is published). Scenes imported from it stay too,
	 * but cannot be exported until the listing is got again (ADR 0022 §11.1). NotFound without a copy, InUse while it is being
	 * acquired or ordered. Completion arrives on OnLibraryChanged.
	 */
	FStageMarketplaceResultInfo RequestRemoveFromLibrary(FGuid ListingId);

	/** Whether the library holds a copy of ListingId. Memory only (kept in step with imports and removals). */
	bool HasLibraryCopy(const FGuid& ListingId) const { return LibraryListings.Contains(ListingId); }

	/** Where this install records the listings it published (Saved/StageCraft/Marketplace/Published.json). */
	static FString GetPublishRecordPath();

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Marketplace")
	FOnStageMarketplaceUnpublishCompleted OnUnpublishCompleted;

	/** A library copy was added (acquire) or removed. */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Marketplace")
	FOnStageMarketplaceLibraryChanged OnLibraryChanged;

	/** User-facing name of the backend ("Community folder"). */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Marketplace")
	FText GetBackendName() const;

	/** Where copies of acquired packages are kept (Saved/StageCraft/Marketplace/Library). */
	static FString GetLibraryDirectory();

	/**
	 * May a stage acquired as OriginListingId leave the app (export, send to a 3D app)? Decided from the package kept in the
	 * library when it was acquired: the publisher's allowExport flag and, for a paid listing, ownership (ADR 0022 §11.1).
	 * Ok, or NotOriginal with the reason. Reads the library file synchronously; the export job does the two halves separately
	 * (ReadLibraryManifest on a worker, DecideExportRights on the game thread).
	 */
	FStageMarketplaceResultInfo CheckExportRights(const FGuid& OriginListingId) const;

	/** The manifest of the library copy of OriginListingId. False when missing or unreadable. File work only: any thread. */
	static bool ReadLibraryManifest(const FGuid& OriginListingId, struct FStagePackageManifest& OutManifest);

	/** CheckExportRights from a manifest already read (null: none could be read). Game thread (reads the profile). */
	FStageMarketplaceResultInfo DecideExportRights(const FGuid& OriginListingId, const struct FStagePackageManifest* LibraryManifest) const;

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

	/**
	 * Seconds the backend has to deliver a package being acquired before the acquire ends as TimedOut (ADR 0021 §14). A paid
	 * listing is owned by then, so getting it again downloads it for free. Values <= 0 fall back to the default.
	 */
	UPROPERTY(Config)
	float FetchTimeoutSeconds = 60.f;

public:
	/** FetchTimeoutSeconds, validated. */
	float GetFetchTimeoutSeconds() const;

private:
	void HandleFetchTimeout(FGuid ListingId, uint32 Serial);
	void ClearFetchTimer(const FGuid& ListingId);
	void HandleQueried(EStageMarketplaceResult Result, const FStageMarketplaceQueryResult& Page, uint32 Serial);

	UFUNCTION()
	void HandleListingPurchaseCompleted(FGuid ListingId, FStageEconomyResultInfo Result);

	void StartDownload(const FGuid& ListingId);
	void HandlePackageFetched(EStageMarketplaceResult Result, const FStageMarketplacePackageBytes& Bytes, FGuid ListingId, uint32 Serial);
	/** Note, when set, follows the success message (what happened to the GDTF fixture types the package carried). */
	void FinishAcquire(const FGuid& ListingId, const FStageMarketplaceResultInfo& Result, const FString& SceneName, const FText& Note = FText::GetEmpty());

	void HandlePreviewFetched(EStageMarketplaceResult Result, const FStageMarketplacePackageBytes& Bytes, FGuid ListingId, uint32 Serial);
	void FinishPreview(const FGuid& ListingId, const FStageMarketplaceResultInfo& Result, const FStageMarketplacePreviewScene& Scene, uint32 Serial);

	void HandlePackageBuilt(EStageMarketplaceResult Result, const FStageMarketplacePackageBytes& Bytes);
	void HandlePublished(EStageMarketplaceResult Result, const FStageMarketplaceListing& Listing);
	void FinishPublish(const FStageMarketplaceResultInfo& Result, const FStageMarketplaceListing& Listing);

	void HandleUnpublished(EStageMarketplaceResult Result, FGuid ListingId);
	void FinishRemoveFromLibrary(const FGuid& ListingId, EStageMarketplaceResult Result);

	/** Reads the publish record and lists the library at start: one small file and one folder listing. */
	void LoadLocalRecords();
	/** Writes the publish record on the work pipe (after the writes before it). */
	void SavePublishRecord();

	/** Listings this install published (ADR 0021 §15). Loaded at Initialize, saved after each publish and unpublish. */
	TSet<FGuid> PublishedByMe;
	/** Listings with a copy in the library. Filled at Initialize, kept in step with imports and removals. */
	TSet<FGuid> LibraryListings;
	bool bUnpublishing = false;

	class UStageEconomySubsystem* GetEconomy() const;
	class UStageProfileSubsystem* GetProfile() const;

	UPROPERTY(Transient)
	TObjectPtr<class UStageMarketplaceBackend> Backend = nullptr;

	/** The page of the last answered query: the only offers that can be acquired or bought. */
	TArray<FStageMarketplaceListing> Listings;
	int32 TotalMatches = 0;

	/** The newest query sent, and the one the current Listings answer. */
	FStageMarketplaceQuery RequestedQuery;
	FStageMarketplaceQuery AnsweredQuery;

	/** Increments per query; an answer with an older serial is stale. */
	uint32 QuerySerial = 0;
	bool bQueryInFlight = false;

	/** Increments per preview request; an answer with an older serial is stale and dropped. */
	uint32 PreviewSerial = 0;
	bool bPreviewLoading = false;

	/** Listings being bought, downloaded or imported. */
	TSet<FGuid> PendingAcquires;

	/** Per listing being downloaded: the fetch awaited (an answer with another serial is stale) and its deadline. */
	struct FPendingFetch
	{
		uint32 Serial = 0;
		FTimerHandle Timer;
	};
	TMap<FGuid, FPendingFetch> PendingFetches;
	uint32 FetchSerial = 0;
	bool bPublishing = false;

	/** Package verification, scene writes and package builds run here one at a time, so two imports never pick the same scene name. */
	UE::Tasks::FPipe WorkPipe{ UE_SOURCE_LOCATION };
};
