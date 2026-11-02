// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Marketplace/StageMarketplaceTypes.h"

/**
 * The community folder behind UStageLocalMarketplaceBackend (Docs/ADR/0021-community-marketplace.md §3): one `<id>.stage` file per
 * listing plus `downloads.json` (popularity). The folder may be a shared network drive, which is a LAN showcase without a server.
 *
 * Every file in the folder is untrusted: browsing skips packages that do not read or whose manifest id differs from the file
 * name, and publishing re-reads the whole package before writing it. Pure logic plus file I/O, no UObjects. The directory is
 * immutable, so any thread may call it, but calls must not overlap (the backend runs them on one pipe).
 */
class FStageLocalMarketplaceStore
{
public:
	explicit FStageLocalMarketplaceStore(FString InDirectory);

	/** Saved/StageCraft/Marketplace/Community. */
	static FString GetDefaultDirectory();

	const FString& GetDirectory() const { return Directory; }

	/** Reads every package's manifest and answers Query. A missing folder is an empty marketplace, not an error. */
	EStageMarketplaceResult Query(const FStageMarketplaceQuery& Query, FStageMarketplaceQueryResult& OutResult) const;

	/** The package bytes, read but not verified (the caller verifies). bCountDownload adds one to the listing's popularity. */
	EStageMarketplaceResult Fetch(const FGuid& ListingId, bool bCountDownload, TArray<uint8>& OutBytes) const;

	/** Verifies the whole package and writes it as a new listing. AlreadyExists when its id is published. */
	EStageMarketplaceResult Publish(TConstArrayView<uint8> PackageBytes, FStageMarketplaceListing& OutListing) const;

	/**
	 * Unpublishes ListingId (ADR 0021 §15): deletes its package file and its popularity entry. NotFound when no readable package
	 * of that id is in the folder (including a file whose manifest names another id, which is not touched); WriteFailed when the
	 * file could not be deleted. Who may remove is decided by the caller: a folder has no authorization of its own.
	 */
	EStageMarketplaceResult Remove(const FGuid& ListingId) const;

	int64 GetDownloads(const FGuid& ListingId) const;

	FString GetPackagePath(const FGuid& ListingId) const;

private:
	TMap<FGuid, int64> LoadDownloads() const;
	bool SaveDownloads(const TMap<FGuid, int64>& Downloads) const;

	FString Directory;
};
