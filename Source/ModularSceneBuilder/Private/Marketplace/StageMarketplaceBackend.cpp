// Copyright Epic Games, Inc. All Rights Reserved.

#include "Marketplace/StageMarketplaceBackend.h"

#include "Async/Async.h"
#include "Marketplace/StageLocalMarketplaceStore.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageMarketplaceBackend)

#define LOCTEXT_NAMESPACE "StageCraftMarketplace"

void UStageLocalMarketplaceBackend::BeginDestroy()
{
	Pipe.WaitUntilEmpty();
	Super::BeginDestroy();
}

FText UStageLocalMarketplaceBackend::GetDisplayName() const
{
	return LOCTEXT("LocalBackendName", "Community folder");
}

FString UStageLocalMarketplaceBackend::GetDirectory() const
{
	return CommunityDirectory.TrimStartAndEnd().IsEmpty() ? FStageLocalMarketplaceStore::GetDefaultDirectory() : CommunityDirectory.TrimStartAndEnd();
}

TSharedRef<const FStageLocalMarketplaceStore, ESPMode::ThreadSafe> UStageLocalMarketplaceBackend::GetStore()
{
	// Created on first use rather than in the constructor, so the class default object never touches the configured path.
	if (!Store.IsValid())
	{
		Store = MakeShared<FStageLocalMarketplaceStore, ESPMode::ThreadSafe>(GetDirectory());
	}
	return Store.ToSharedRef();
}

void UStageLocalMarketplaceBackend::Query(const FStageMarketplaceQuery& Query, FOnStageMarketplaceQueried OnQueried)
{
	Pipe.Launch(UE_SOURCE_LOCATION, [WeakThis = TWeakObjectPtr<ThisClass>(this), FolderStore = GetStore(), Query, OnQueried = MoveTemp(OnQueried)]() mutable
	{
		FStageMarketplaceQueryResult Page;
		const EStageMarketplaceResult Result = FolderStore->Query(Query, Page);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Result, Page = MoveTemp(Page), OnQueried = MoveTemp(OnQueried)]()
		{
			if (WeakThis.IsValid())
			{
				OnQueried.ExecuteIfBound(Result, Page);
			}
		});
	});
}

void UStageLocalMarketplaceBackend::FetchPackage(const FGuid& ListingId, EStageMarketplaceFetchPurpose Purpose, FOnStageMarketplacePackageFetched OnFetched)
{
	Pipe.Launch(UE_SOURCE_LOCATION, [WeakThis = TWeakObjectPtr<ThisClass>(this), FolderStore = GetStore(), ListingId, Purpose, OnFetched = MoveTemp(OnFetched)]() mutable
	{
		const TSharedRef<TArray<uint8>, ESPMode::ThreadSafe> Bytes = MakeShared<TArray<uint8>, ESPMode::ThreadSafe>();
		const EStageMarketplaceResult Result = FolderStore->Fetch(ListingId, Purpose == EStageMarketplaceFetchPurpose::Acquire, *Bytes);
		FStageMarketplacePackageBytes Answer = Result == EStageMarketplaceResult::Success ? FStageMarketplacePackageBytes(Bytes) : FStageMarketplacePackageBytes();
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Result, Answer = MoveTemp(Answer), OnFetched = MoveTemp(OnFetched)]()
		{
			if (WeakThis.IsValid())
			{
				OnFetched.ExecuteIfBound(Result, Answer);
			}
		});
	});
}

void UStageLocalMarketplaceBackend::Publish(const FStageMarketplacePackageBytes& PackageBytes, FOnStageMarketplacePublished OnPublished)
{
	Pipe.Launch(UE_SOURCE_LOCATION, [WeakThis = TWeakObjectPtr<ThisClass>(this), FolderStore = GetStore(), PackageBytes, OnPublished = MoveTemp(OnPublished)]() mutable
	{
		FStageMarketplaceListing Listing;
		const EStageMarketplaceResult Result = PackageBytes.IsValid() ? FolderStore->Publish(*PackageBytes, Listing) : EStageMarketplaceResult::InvalidRequest;
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Result, Listing = MoveTemp(Listing), OnPublished = MoveTemp(OnPublished)]()
		{
			if (WeakThis.IsValid())
			{
				OnPublished.ExecuteIfBound(Result, Listing);
			}
		});
	});
}

#undef LOCTEXT_NAMESPACE
