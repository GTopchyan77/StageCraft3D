// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Economy/StageEconomyTypes.h"
#include "Marketplace/StageMarketplaceTypes.h"
#include "Marketplace/StagePackage.h"

/** What the economy knows when it decides on a listing purchase (Docs/ADR/0021-community-marketplace.md §5). */
struct FStageListingPurchaseContext
{
	/** The backend's listing; null when the listing is unknown to the marketplace. */
	const FStageMarketplaceListing* Listing = nullptr;
	/** Null when there is no profile. */
	const FStagePlayerProfile* Profile = nullptr;
	bool bIntegrityValid = false;
	bool bPending = false;
	/** True only for a commerce backend that is an authoritative server (UStageCommerceBackend::CanSettleRealMoney). */
	bool bCanSettleRealMoney = false;
};

/** Whether a stage acquired from the marketplace may leave the app (Docs/ADR/0022-dcc-bridge.md §11.1). */
enum class EStageExportRights : uint8
{
	Allowed,
	/** The acquired package is not in the user's library, or cannot be read. Getting the listing again restores it. */
	NoLibraryCopy,
	/** The library copy belongs to another listing. */
	Mismatch,
	/** The publisher did not allow export. */
	NotAllowedByPublisher,
	/** A paid listing the profile does not own. */
	NotOwned,
};

/**
 * The marketplace's pure rules: browsing (filter, sort, page), listing ownership, listing-purchase validation and the names of
 * imported scenes. No world, no UObjects; safe on any thread and tested directly.
 */
namespace StageMarketplaceRules
{
	/** The primary asset type of listing purchases in purchase records and commerce requests ("StageListing:<guid>"). */
	inline constexpr TCHAR ListingProductType[] = TEXT("StageListing");

	FPrimaryAssetId MakeListingProductId(const FGuid& ListingId);

	/** Clamps the page and trims the search text to its limit. */
	FStageMarketplaceQuery SanitizeQuery(const FStageMarketplaceQuery& Query);

	/** Every search word appears in the title, description, author or a keyword; kind, category and price filters hold. */
	bool Matches(const FStageMarketplaceListing& Listing, const FStageMarketplaceQuery& Query);

	/**
	 * Filters, sorts and pages Listings by Query. Ties are broken by title and then id, so the same folder always pages the same
	 * way. Real-money prices sort after every credits price of the same amount, because the two cannot be compared.
	 */
	FStageMarketplaceQueryResult Apply(TArray<FStageMarketplaceListing> Listings, const FStageMarketplaceQuery& Query);

	bool OwnsListing(const FStagePlayerProfile& Profile, const FGuid& ListingId);

	/** Free, Owned, Purchasable (credits, spending allowed) or Locked (real money without a server, or a frozen profile). */
	EStageOwnershipState GetOwnership(const FStageMarketplaceListing& Listing, const FStagePlayerProfile& Profile, bool bIntegrityValid, bool bCanSettleRealMoney);

	/**
	 * The listing-purchase checks, in order: profile, integrity, known listing, not free, not owned, not pending, real money only
	 * with a server, enough credits. Deny by default: anything missing is a refusal.
	 */
	FStageEconomyResultInfo ValidateListingPurchase(const FStageListingPurchaseContext& Context);

	/**
	 * A scene name for an imported package: Title reduced to the characters scene names allow, or "Marketplace Stage", made unique
	 * with " (2)", " (3)", ... against Exists. Empty when 999 names are taken.
	 */
	FString MakeImportSceneName(const FString& Title, TFunctionRef<bool(const FString&)> Exists);

	/**
	 * Export rights of a stage whose origin is ListingId. Manifest is the package kept in the library when the stage was acquired,
	 * or null when there is none (deny by default). Allowed only for that listing's own manifest, with the publisher's flag set,
	 * and a free price or an owned listing. Like every client-side check this deters casual copying; it is not a security boundary.
	 */
	EStageExportRights ResolveExportRights(const FGuid& ListingId, const FStagePackageManifest* Manifest, const FStagePlayerProfile& Profile);
}
