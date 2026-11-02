// Copyright Epic Games, Inc. All Rights Reserved.

#include "Tests/StageTestEventRecorder.h"

#include "Bridge/StageBridgeSubsystem.h"
#include "Engine/GameInstance.h"
#include "Marketplace/StageAcquireDispatchSubsystem.h"
#include "Marketplace/StageMarketplaceSubsystem.h"
#include "Services/StageAIToolSubsystem.h"
#include "Subsystems/StageEconomySubsystem.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageTestEventRecorder)

void UStageTestEventRecorder::Bind(UGameInstance& GameInstance)
{
	Unbind();
	Bound = &GameInstance;
	if (UStageEconomySubsystem* Economy = GameInstance.GetSubsystem<UStageEconomySubsystem>())
	{
		Economy->OnListingPurchaseCompleted.AddUniqueDynamic(this, &ThisClass::HandleListingPurchase);
		Economy->OnPurchaseCompleted.AddUniqueDynamic(this, &ThisClass::HandleProductPurchase);
		Economy->OnCreditTopUpCompleted.AddUniqueDynamic(this, &ThisClass::HandleTopUp);
	}
	if (UStageMarketplaceSubsystem* Marketplace = GameInstance.GetSubsystem<UStageMarketplaceSubsystem>())
	{
		Marketplace->OnAcquireCompleted.AddUniqueDynamic(this, &ThisClass::HandleAcquire);
		Marketplace->OnPublishCompleted.AddUniqueDynamic(this, &ThisClass::HandlePublish);
		Marketplace->OnUnpublishCompleted.AddUniqueDynamic(this, &ThisClass::HandleUnpublish);
		Marketplace->OnLibraryChanged.AddUniqueDynamic(this, &ThisClass::HandleLibraryChanged);
	}
	if (UStageAcquireDispatchSubsystem* Dispatch = GameInstance.GetSubsystem<UStageAcquireDispatchSubsystem>())
	{
		Dispatch->OnOrderCompleted.AddUniqueDynamic(this, &ThisClass::HandleOrder);
	}
	if (UStageBridgeSubsystem* Bridge = GameInstance.GetSubsystem<UStageBridgeSubsystem>())
	{
		Bridge->OnSendCompleted.AddUniqueDynamic(this, &ThisClass::HandleSend);
	}
	if (UStageAIToolSubsystem* Tools = GameInstance.GetSubsystem<UStageAIToolSubsystem>())
	{
		Tools->OnJobCompleted.AddUniqueDynamic(this, &ThisClass::HandleToolJob);
	}
}

void UStageTestEventRecorder::Unbind()
{
	UGameInstance* GameInstance = Bound.Get();
	Bound.Reset();
	if (!GameInstance)
	{
		return;
	}
	if (UStageEconomySubsystem* Economy = GameInstance->GetSubsystem<UStageEconomySubsystem>())
	{
		Economy->OnListingPurchaseCompleted.RemoveDynamic(this, &ThisClass::HandleListingPurchase);
		Economy->OnPurchaseCompleted.RemoveDynamic(this, &ThisClass::HandleProductPurchase);
		Economy->OnCreditTopUpCompleted.RemoveDynamic(this, &ThisClass::HandleTopUp);
	}
	if (UStageMarketplaceSubsystem* Marketplace = GameInstance->GetSubsystem<UStageMarketplaceSubsystem>())
	{
		Marketplace->OnAcquireCompleted.RemoveDynamic(this, &ThisClass::HandleAcquire);
		Marketplace->OnPublishCompleted.RemoveDynamic(this, &ThisClass::HandlePublish);
		Marketplace->OnUnpublishCompleted.RemoveDynamic(this, &ThisClass::HandleUnpublish);
		Marketplace->OnLibraryChanged.RemoveDynamic(this, &ThisClass::HandleLibraryChanged);
	}
	if (UStageAcquireDispatchSubsystem* Dispatch = GameInstance->GetSubsystem<UStageAcquireDispatchSubsystem>())
	{
		Dispatch->OnOrderCompleted.RemoveDynamic(this, &ThisClass::HandleOrder);
	}
	if (UStageBridgeSubsystem* Bridge = GameInstance->GetSubsystem<UStageBridgeSubsystem>())
	{
		Bridge->OnSendCompleted.RemoveDynamic(this, &ThisClass::HandleSend);
	}
	if (UStageAIToolSubsystem* Tools = GameInstance->GetSubsystem<UStageAIToolSubsystem>())
	{
		Tools->OnJobCompleted.RemoveDynamic(this, &ThisClass::HandleToolJob);
	}
}

void UStageTestEventRecorder::HandleListingPurchase(FGuid ListingId, FStageEconomyResultInfo Result)
{
	ListingPurchases.Add({ ListingId, Result });
}

void UStageTestEventRecorder::HandleProductPurchase(UStageProductData* Product, FStageEconomyResultInfo Result)
{
	ProductPurchases.Add(Result);
}

void UStageTestEventRecorder::HandleTopUp(FGameplayTag Currency, int64 GrantedAmount, FStageEconomyResultInfo Result)
{
	TopUps.Add({ GrantedAmount, Result });
}

void UStageTestEventRecorder::HandleAcquire(FGuid ListingId, FStageMarketplaceResultInfo Result, const FString& SceneName)
{
	Acquires.Add({ ListingId, Result, SceneName });
}

void UStageTestEventRecorder::HandlePublish(FStageMarketplaceResultInfo Result, const FStageMarketplaceListing& Listing)
{
	Publishes.Add({ Listing.ListingId, Result, FString() });
}

void UStageTestEventRecorder::HandleUnpublish(FGuid ListingId, FStageMarketplaceResultInfo Result)
{
	Unpublishes.Add({ ListingId, Result, FString() });
}

void UStageTestEventRecorder::HandleLibraryChanged(FGuid ListingId, FStageMarketplaceResultInfo Result)
{
	LibraryChanges.Add({ ListingId, Result, FString() });
}

void UStageTestEventRecorder::HandleOrder(const FStageOrderResultInfo& Result)
{
	Orders.Add(Result);
}

void UStageTestEventRecorder::HandleSend(const FStageBridgeResultInfo& Result)
{
	Sends.Add(Result);
}

void UStageTestEventRecorder::HandleToolJob(const FStageAIToolResultInfo& Result)
{
	ToolJobs.Add(Result);
}
