// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Cloud/StageCloudTypes.h"
#include "Marketplace/StageMarketplaceTypes.h"

/**
 * The StageCraft Cloud wire format on the client (Docs/ADR/0032 §4.1): building request paths and bodies, and reading the server's
 * answers. The server is trusted for decisions it makes (an order is paid, a listing exists), never for the shape of its data:
 * every field is validated and capped here before the rest of the app sees it (Rules.md §55). Pure: no HTTP, no world; covered by the
 * StageCraft.Cloud.Json tests.
 */
namespace StageCloudJson
{
	/** /v1/listings?… for Query: every filter, the sort and the page; values URL-encoded. */
	MODULARSCENEBUILDER_API FString BuildListingsPath(const FStageMarketplaceQuery& Query);

	/** {"listingId":…,"idempotencyKey":…} for POST /v1/orders; the key is the economy's TransactionId. */
	MODULARSCENEBUILDER_API FString BuildOrderBody(const FGuid& ListingId, const FGuid& IdempotencyKey);

	/** {"deviceCode":…} for POST /v1/auth/device/token. */
	MODULARSCENEBUILDER_API FString BuildDeviceTokenBody(const FString& DeviceCode);

	/** The GUID as the server writes it (lowercase, hyphenated). */
	MODULARSCENEBUILDER_API FString GuidToWire(const FGuid& Guid);

	/** A listing as the server sends it, validated and capped like a package manifest. False (Out untouched) when unusable. */
	MODULARSCENEBUILDER_API bool ParseListing(const class FJsonObject& Object, FStageMarketplaceListing& Out);

	/** A page of listings; listings that fail ParseListing are left out and counted in OutDropped. False when the page is unreadable. */
	MODULARSCENEBUILDER_API bool ParseListingsPage(const FString& Body, FStageMarketplaceQueryResult& Out, int32& OutDropped);

	/** {error:{code,message}} from any failed answer; a transport failure is Status 0 with no code. Code and message are capped. */
	MODULARSCENEBUILDER_API FStageCloudError ParseError(const FStageCloudResponse& Response);

	/** What a failed cloud call means for the marketplace. Unknown codes are BackendUnavailable (deny by default). */
	MODULARSCENEBUILDER_API EStageMarketplaceResult ToMarketplaceResult(const FStageCloudError& Error);

	MODULARSCENEBUILDER_API bool ParseDeviceCode(const FString& Body, FStageCloudDeviceCode& Out);
	MODULARSCENEBUILDER_API FStageCloudDevicePollResult ParseDevicePoll(const FStageCloudResponse& Response);
	MODULARSCENEBUILDER_API bool ParseAccount(const FString& Body, FStageCloudAccount& Out);
	MODULARSCENEBUILDER_API bool ParseOrder(const FString& Body, FStageCloudOrder& Out);
	MODULARSCENEBUILDER_API EStageCloudOrderState ParseOrderState(const FString& Name);

	/** The listings the account owns (GET /v1/me/entitlements). False when unreadable. */
	MODULARSCENEBUILDER_API bool ParseEntitlements(const FString& Body, TArray<FGuid>& Out);

	/** The short-lived download address of a package (GET /v1/listings/:id/package?redirect=false). False unless it is safe to fetch. */
	MODULARSCENEBUILDER_API bool ParsePackageUrl(const FString& Body, FString& OutUrl);

	/** The ticket id the server gave a support report. */
	MODULARSCENEBUILDER_API bool ParseTicketId(const FString& Body, FString& OutTicketId);

	/**
	 * An address safe to open in the browser or to download from: https, or http only to this computer (a development server). No
	 * credentials in the address, no spaces or control characters, at most 2048 characters.
	 */
	MODULARSCENEBUILDER_API bool IsSafeBrowserUrl(const FString& Url);

	/** A server base address: IsSafeBrowserUrl, no query or fragment; the trailing slash is removed. False when unusable. */
	MODULARSCENEBUILDER_API bool NormalizeBaseUrl(const FString& Url, FString& OutBaseUrl);
}
