// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Marketplace/StageMarketplaceBackend.h"
#include "StageCloudMarketplaceBackend.generated.h"

/**
 * The marketplace on StageCraft Cloud (Docs/ADR/0032 §6): listings, downloads, publishing and unpublishing over /v1/listings, with
 * the community folder (UStageLocalMarketplaceBackend) whenever the cloud is offline, so a build without a server behaves exactly as
 * before.
 *
 * The server authorizes every call (who may download a paid package, who may unpublish) and re-validates every package; this class
 * only carries requests and validates answers (StageCloudJson). Downloads are fetched from the short-lived address the server hands
 * out, without the session, and are read by the marketplace through StagePackage::Read like any package. Every call answers exactly
 * once on the game thread. Owned by UStageMarketplaceSubsystem.
 */
UCLASS(Config = Game)
class MODULARSCENEBUILDER_API UStageCloudMarketplaceBackend : public UStageMarketplaceBackend
{
	GENERATED_BODY()

public:
	//~ Begin UStageMarketplaceBackend Interface
	virtual void Query(const FStageMarketplaceQuery& Query, FOnStageMarketplaceQueried OnQueried) override;
	virtual void FetchPackage(const FGuid& ListingId, EStageMarketplaceFetchPurpose Purpose, FOnStageMarketplacePackageFetched OnFetched) override;
	virtual void Publish(const FStageMarketplacePackageBytes& PackageBytes, FOnStageMarketplacePublished OnPublished) override;
	virtual void Unpublish(const FGuid& ListingId, FOnStageMarketplaceUnpublished OnUnpublished) override;
	virtual FText GetDisplayName() const override;
	//~ End UStageMarketplaceBackend Interface

	/** True while the cloud is configured: listings come from the server, not the community folder. */
	bool UsesCloud() const;

private:
	class UStageCloudSubsystem* GetCloud() const;
	UStageMarketplaceBackend& GetLocal();

	UPROPERTY(Transient)
	TObjectPtr<UStageMarketplaceBackend> Local = nullptr;

	/** Seconds a package download may take (up to 96 MiB). */
	UPROPERTY(Config)
	float DownloadTimeoutSeconds = 300.f;
};
