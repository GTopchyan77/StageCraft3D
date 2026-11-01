// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Marketplace/StageMarketplaceTypes.h"
#include "Tasks/Pipe.h"
#include "UObject/Object.h"
#include "StageMarketplaceBackend.generated.h"

using FStageMarketplacePackageBytes = TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>;

DECLARE_DELEGATE_TwoParams(FOnStageMarketplaceQueried, EStageMarketplaceResult /*Result*/, const FStageMarketplaceQueryResult& /*Page*/);
/** Bytes are null unless Result is Success. They are unverified: the caller reads them through StagePackage::Read. */
DECLARE_DELEGATE_TwoParams(FOnStageMarketplacePackageFetched, EStageMarketplaceResult /*Result*/, const FStageMarketplacePackageBytes& /*Bytes*/);
DECLARE_DELEGATE_TwoParams(FOnStageMarketplacePublished, EStageMarketplaceResult /*Result*/, const FStageMarketplaceListing& /*Listing*/);

/** Why a package is fetched; only acquisitions count towards popularity. */
UENUM()
enum class EStageMarketplaceFetchPurpose : uint8
{
	Acquire,
	Preview,
};

/**
 * Where marketplace listings come from (Docs/ADR/0021-community-marketplace.md §3): the seam between the app and whoever hosts
 * community content. Today UStageLocalMarketplaceBackend (a folder); a server backend replaces it without changing the
 * subsystem, the economy or the UI.
 *
 * Contract: every call answers exactly once, on the game thread, now or later. A server backend must re-validate the package and
 * the price itself (the client is never trusted) and authenticate the publisher.
 *
 * Selected by UStageMarketplaceSubsystem's MarketplaceBackendClass (DefaultGame.ini).
 */
UCLASS(Abstract)
class MODULARSCENEBUILDER_API UStageMarketplaceBackend : public UObject
{
	GENERATED_BODY()

public:
	virtual void Query(const FStageMarketplaceQuery& Query, FOnStageMarketplaceQueried OnQueried)
		PURE_VIRTUAL(UStageMarketplaceBackend::Query, );

	virtual void FetchPackage(const FGuid& ListingId, EStageMarketplaceFetchPurpose Purpose, FOnStageMarketplacePackageFetched OnFetched)
		PURE_VIRTUAL(UStageMarketplaceBackend::FetchPackage, );

	/** PackageBytes is a complete .stage package (StagePackage::Write). */
	virtual void Publish(const FStageMarketplacePackageBytes& PackageBytes, FOnStageMarketplacePublished OnPublished)
		PURE_VIRTUAL(UStageMarketplaceBackend::Publish, );

	/** User-facing name, for the status line ("Community folder"). */
	virtual FText GetDisplayName() const PURE_VIRTUAL(UStageMarketplaceBackend::GetDisplayName, return FText::GetEmpty(););
};

/**
 * Offline backend: a community folder of .stage files (FStageLocalMarketplaceStore). Folder operations run one at a time on a
 * background pipe, so browsing never blocks the game thread and a publish never races a query. Answers return to the game thread
 * and are dropped if this backend was destroyed meanwhile (its owner, the subsystem, is then gone too).
 */
UCLASS(Config = Game)
class MODULARSCENEBUILDER_API UStageLocalMarketplaceBackend : public UStageMarketplaceBackend
{
	GENERATED_BODY()

public:
	virtual void Query(const FStageMarketplaceQuery& Query, FOnStageMarketplaceQueried OnQueried) override;
	virtual void FetchPackage(const FGuid& ListingId, EStageMarketplaceFetchPurpose Purpose, FOnStageMarketplacePackageFetched OnFetched) override;
	virtual void Publish(const FStageMarketplacePackageBytes& PackageBytes, FOnStageMarketplacePublished OnPublished) override;
	virtual FText GetDisplayName() const override;

	// UObject: waits for folder operations still on the pipe (bounded file reads and writes), because the pipe must be empty when destroyed.
	virtual void BeginDestroy() override;

	/** The folder in use: CommunityDirectory, or Saved/StageCraft/Marketplace/Community when that is empty. */
	FString GetDirectory() const;

protected:
	/** Absolute path of the community folder; a shared network folder makes a LAN showcase. Empty: the default folder. */
	UPROPERTY(Config)
	FString CommunityDirectory;

private:
	TSharedRef<const class FStageLocalMarketplaceStore, ESPMode::ThreadSafe> GetStore();

	TSharedPtr<const class FStageLocalMarketplaceStore, ESPMode::ThreadSafe> Store;

	UE::Tasks::FPipe Pipe{ UE_SOURCE_LOCATION };
};
