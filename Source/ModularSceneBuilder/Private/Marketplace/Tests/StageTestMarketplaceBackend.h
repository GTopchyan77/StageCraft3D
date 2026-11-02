// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Marketplace/StageMarketplaceBackend.h"
#include "StageTestMarketplaceBackend.generated.h"

/**
 * Automation tests only: the local (folder) marketplace backend, except that the test can hold package fetches and deliver them
 * later or never, to drive UStageMarketplaceSubsystem's fetch deadline (Docs/ADR/0021-community-marketplace.md §14).
 *
 * The test harness (FStageTestGameInstance) selects it through the marketplace's class default object and restores it afterwards.
 * Unless bHoldFetches is set it behaves exactly as UStageLocalMarketplaceBackend. It ships as an unreachable class like
 * UStageTestCommerceBackend (UHT cannot compile a UCLASS out). Game thread only.
 */
UCLASS(NotBlueprintable, Transient, HideDropdown)
class UStageTestMarketplaceBackend : public UStageLocalMarketplaceBackend
{
	GENERATED_BODY()

public:
	//~ Begin UStageMarketplaceBackend Interface
	virtual void FetchPackage(const FGuid& ListingId, EStageMarketplaceFetchPurpose Purpose, FOnStageMarketplacePackageFetched OnFetched) override;
	//~ End UStageMarketplaceBackend Interface

	/** The backend the marketplace of this game instance created (it is the marketplace's sub-object), or null. */
	static UStageTestMarketplaceBackend* Find(const class UGameInstance& GameInstance);

	/** Delivers the held fetch of ListingId now, from the folder as the local backend would. False when none is held. */
	bool DeliverHeldFetch(const FGuid& ListingId);

	int32 GetHeldFetchCount() const { return HeldFetches.Num(); }

	/** When true, FetchPackage keeps the request until DeliverHeldFetch (or forever). */
	bool bHoldFetches = false;

	/** Every fetch seen, in order. */
	TArray<FGuid> FetchRequests;

private:
	struct FHeldFetch
	{
		EStageMarketplaceFetchPurpose Purpose = EStageMarketplaceFetchPurpose::Acquire;
		FOnStageMarketplacePackageFetched OnFetched;
	};
	TMap<FGuid, FHeldFetch> HeldFetches;
};
