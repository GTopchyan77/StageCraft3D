// Copyright Epic Games, Inc. All Rights Reserved.

#include "Marketplace/Tests/StageTestMarketplaceBackend.h"

#include "Engine/GameInstance.h"
#include "Marketplace/StageMarketplaceSubsystem.h"
#include "UObject/UObjectHash.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageTestMarketplaceBackend)

void UStageTestMarketplaceBackend::FetchPackage(const FGuid& ListingId, EStageMarketplaceFetchPurpose Purpose, FOnStageMarketplacePackageFetched OnFetched)
{
	FetchRequests.Add(ListingId);
	if (!bHoldFetches)
	{
		Super::FetchPackage(ListingId, Purpose, MoveTemp(OnFetched));
		return;
	}
	FHeldFetch& Held = HeldFetches.Add(ListingId);
	Held.Purpose = Purpose;
	Held.OnFetched = MoveTemp(OnFetched);
}

UStageTestMarketplaceBackend* UStageTestMarketplaceBackend::Find(const UGameInstance& GameInstance)
{
	const UStageMarketplaceSubsystem* Marketplace = GameInstance.GetSubsystem<UStageMarketplaceSubsystem>();
	UStageTestMarketplaceBackend* Found = nullptr;
	if (Marketplace)
	{
		ForEachObjectWithOuter(Marketplace, [&Found](UObject* Object)
		{
			if (!Found)
			{
				Found = Cast<UStageTestMarketplaceBackend>(Object);
			}
		}, EGetObjectsFlags::None);
	}
	return Found;
}

bool UStageTestMarketplaceBackend::DeliverHeldFetch(const FGuid& ListingId)
{
	FHeldFetch Held;
	if (!HeldFetches.RemoveAndCopyValue(ListingId, Held))
	{
		return false;
	}
	// The real folder read, answered on the game thread as usual; only its start was late.
	Super::FetchPackage(ListingId, Held.Purpose, MoveTemp(Held.OnFetched));
	return true;
}
