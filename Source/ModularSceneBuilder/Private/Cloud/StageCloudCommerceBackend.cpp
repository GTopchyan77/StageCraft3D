// Copyright Epic Games, Inc. All Rights Reserved.

#include "Cloud/StageCloudCommerceBackend.h"

#include "Cloud/StageCloudJson.h"
#include "Cloud/StageCloudSubsystem.h"
#include "Engine/GameInstance.h"
#include "Marketplace/StageMarketplaceRules.h"
#include "Marketplace/StageMarketplaceSubsystem.h"
#include "ModularSceneBuilder.h"
#include "TimerManager.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageCloudCommerceBackend)

#define LOCTEXT_NAMESPACE "StageCloudCommerceBackend"

namespace StageCloudCommerce
{
	/** A dropped connection while waiting for payment is retried after this long, until the deadline. */
	constexpr float RetrySeconds = 5.f;

	FText DescribeRefusal(const FStageCloudError& Error)
	{
		if (Error.Status == 0)
		{
			return LOCTEXT("NoConnection", "StageCraft Cloud could not be reached. Nothing was charged.");
		}
		if (Error.Code == TEXT("Unauthenticated"))
		{
			return LOCTEXT("SignIn", "Sign in to StageCraft Cloud to buy this listing.");
		}
		if (Error.Code == TEXT("NotForSale") || Error.Code == TEXT("NotFound"))
		{
			return LOCTEXT("NotForSale", "This listing is no longer for sale.");
		}
		if (Error.Code == TEXT("SellerNotOnboarded"))
		{
			return LOCTEXT("SellerNotReady", "The seller cannot receive payments yet. Try again later.");
		}
		if (Error.Code == TEXT("RateLimited"))
		{
			return LOCTEXT("RateLimited", "Too many purchases in a short time. Wait a minute and try again.");
		}
		return FText::Format(LOCTEXT("Refused", "The store refused the purchase{0}"), Error.Message.IsEmpty() ? FText::FromString(TEXT("."))
			: FText::Format(LOCTEXT("RefusedReason", ": {0}"), FText::FromString(Error.Message)));
	}
}

UStageCloudSubsystem* UStageCloudCommerceBackend::GetCloud() const
{
	const UGameInstance* GameInstance = GetTypedOuter<UGameInstance>();
	return GameInstance ? GameInstance->GetSubsystem<UStageCloudSubsystem>() : nullptr;
}

bool UStageCloudCommerceBackend::CanSettleRealMoney() const
{
	const UStageCloudSubsystem* Cloud = GetCloud();
	return Cloud && Cloud->IsSignedIn();
}

TOptional<FGuid> UStageCloudCommerceBackend::FindRealMoneyListing(const FStagePurchaseRequest& Request) const
{
	if (Request.ProductId.PrimaryAssetType != FPrimaryAssetType(StageMarketplaceRules::ListingProductType))
	{
		return {};
	}
	FGuid ListingId;
	const UGameInstance* GameInstance = GetTypedOuter<UGameInstance>();
	const UStageMarketplaceSubsystem* Marketplace = GameInstance ? GameInstance->GetSubsystem<UStageMarketplaceSubsystem>() : nullptr;
	// The economy validated this offer from the marketplace's listings in the same call, so it is there now.
	const FStageMarketplaceListing* Listing = Marketplace && FGuid::Parse(Request.ProductId.PrimaryAssetName.ToString(), ListingId)
		? Marketplace->FindListing(ListingId) : nullptr;
	return Listing && Listing->Price.HasRealMoneyPrice() ? TOptional<FGuid>(ListingId) : TOptional<FGuid>();
}

float UStageCloudCommerceBackend::GetSettlementTimeoutSeconds(const FStagePurchaseRequest& Request, float DefaultSeconds) const
{
	// A minute over the checkout's own deadline, so this backend's answer (with its reason) always comes before the economy's timeout.
	return FindRealMoneyListing(Request).IsSet() ? CheckoutTimeoutSeconds + 60.f : DefaultSeconds;
}

void UStageCloudCommerceBackend::ProcessPurchase(const FStagePurchaseRequest& Request, FOnStagePurchaseProcessed OnProcessed)
{
	const TOptional<FGuid> ListingId = FindRealMoneyListing(Request);
	if (!ListingId.IsSet())
	{
		if (!Local)
		{
			Local = NewObject<UStageLocalCommerceBackend>(this);
		}
		Local->ProcessPurchase(Request, MoveTemp(OnProcessed));
		return;
	}
	UStageCloudSubsystem* Cloud = GetCloud();
	if (!Cloud || !Cloud->IsSignedIn() || Checkouts.Contains(Request.TransactionId))
	{
		OnProcessed.ExecuteIfBound(false, LOCTEXT("SignInFirst", "Sign in to StageCraft Cloud to buy this listing."));
		return;
	}

	FCheckout& Checkout = Checkouts.Add(Request.TransactionId);
	Checkout.ListingId = *ListingId;
	Checkout.DeadlineUtc = FDateTime::UtcNow() + FTimespan::FromSeconds(CheckoutTimeoutSeconds);
	Checkout.OnProcessed = MoveTemp(OnProcessed);
	UE_LOG(LogStageCraft, Log, TEXT("Cloud store: ordering listing %s (transaction %s)."), *ListingId->ToString(), *Request.TransactionId.ToString());
	Cloud->SendJson(TEXT("POST"), TEXT("/v1/orders"), StageCloudJson::BuildOrderBody(*ListingId, Request.TransactionId), /*bAuthenticated*/ true,
		FOnStageCloudResponse::CreateWeakLambda(this, [this, TransactionId = Request.TransactionId](const FStageCloudResponse& Response)
		{
			HandleOrderCreated(Response, TransactionId);
		}));
}

void UStageCloudCommerceBackend::HandleOrderCreated(const FStageCloudResponse& Response, FGuid TransactionId)
{
	FCheckout* Checkout = Checkouts.Find(TransactionId);
	if (!Checkout)
	{
		return;
	}
	if (!Response.IsSuccess())
	{
		const FStageCloudError Error = StageCloudJson::ParseError(Response);
		if (Error.Code == TEXT("AlreadyOwned"))
		{
			// Bought before, on this account (another computer): the server already holds the entitlement; nothing is charged.
			Finish(TransactionId, true, LOCTEXT("AlreadyOwned", "You already own this listing on your StageCraft Cloud account."));
			return;
		}
		Finish(TransactionId, false, StageCloudCommerce::DescribeRefusal(Error));
		return;
	}
	FStageCloudOrder Order;
	if (!StageCloudJson::ParseOrder(Response.GetBodyAsString(), Order) || Order.ListingId != Checkout->ListingId)
	{
		Finish(TransactionId, false, LOCTEXT("BadOrder", "The store's answer could not be read. Nothing was charged here."));
		return;
	}
	Checkout->OrderId = Order.OrderId;
	if (Order.IsPaid())
	{
		Finish(TransactionId, true, FText::GetEmpty());
		return;
	}
	if (Order.State != EStageCloudOrderState::CheckoutOpen || Order.CheckoutUrl.IsEmpty())
	{
		Finish(TransactionId, false, LOCTEXT("NoCheckout", "The store could not open a checkout. Nothing was charged."));
		return;
	}
	UStageCloudSubsystem* Cloud = GetCloud();
	if (!Cloud || !Cloud->OpenInBrowser(Order.CheckoutUrl))
	{
		Finish(TransactionId, false, LOCTEXT("NoBrowser", "The payment page could not be opened in the browser. Nothing was charged."));
		return;
	}
	WaitForOrder(TransactionId);
}

void UStageCloudCommerceBackend::WaitForOrder(FGuid TransactionId)
{
	FCheckout* Checkout = Checkouts.Find(TransactionId);
	UStageCloudSubsystem* Cloud = GetCloud();
	if (!Checkout)
	{
		return;
	}
	if (!Cloud || !Cloud->IsSignedIn())
	{
		Finish(TransactionId, false, LOCTEXT("SignedOutWhileWaiting", "You signed out before the payment was confirmed. If you paid, buy the listing again: you will not be charged twice."));
		return;
	}
	if (FDateTime::UtcNow() >= Checkout->DeadlineUtc)
	{
		Finish(TransactionId, false, LOCTEXT("TimedOut", "No payment was confirmed in time. If you paid, buy the listing again: you will not be charged twice."));
		return;
	}
	const int32 Wait = FMath::Clamp(OrderWaitSeconds, 0, 25);
	Cloud->SendJson(TEXT("GET"), FString::Printf(TEXT("/v1/orders/%s?waitSeconds=%d"), *Checkout->OrderId, Wait), FString(), /*bAuthenticated*/ true,
		FOnStageCloudResponse::CreateWeakLambda(this, [this, TransactionId](const FStageCloudResponse& Response)
		{
			HandleOrderState(Response, TransactionId);
		}),
		float(Wait) + 15.f);
}

void UStageCloudCommerceBackend::HandleOrderState(const FStageCloudResponse& Response, FGuid TransactionId)
{
	FCheckout* Checkout = Checkouts.Find(TransactionId);
	if (!Checkout)
	{
		return;
	}
	if (!Response.bConnected || Response.Status >= 500 || Response.Status == 429)
	{
		RetryLater(TransactionId);
		return;
	}
	FStageCloudOrder Order;
	if (!Response.IsSuccess() || !StageCloudJson::ParseOrder(Response.GetBodyAsString(), Order) || Order.OrderId != Checkout->OrderId)
	{
		Finish(TransactionId, false, Response.Status == 401 ? LOCTEXT("SessionEnded", "Your StageCraft Cloud session ended while waiting. If you paid, sign in and buy again: you will not be charged twice.")
			: StageCloudCommerce::DescribeRefusal(StageCloudJson::ParseError(Response)));
		return;
	}
	switch (Order.State)
	{
	case EStageCloudOrderState::Paid:
	case EStageCloudOrderState::PartiallyRefunded:
	case EStageCloudOrderState::Disputed:
		Finish(TransactionId, true, FText::GetEmpty());
		return;
	case EStageCloudOrderState::Expired:
		Finish(TransactionId, false, LOCTEXT("Expired", "The checkout expired before payment. Nothing was charged."));
		return;
	case EStageCloudOrderState::Failed:
		Finish(TransactionId, false, LOCTEXT("Failed", "The payment did not go through. Nothing was charged."));
		return;
	case EStageCloudOrderState::Refunded:
		Finish(TransactionId, false, LOCTEXT("Refunded", "This payment was refunded."));
		return;
	case EStageCloudOrderState::OnHold:
		Finish(TransactionId, false, LOCTEXT("OnHold", "The payment is being checked by StageCraft support. You will get the listing as soon as it is confirmed."));
		return;
	default:
		// Created, CheckoutOpen or a state this build does not know: keep waiting until the deadline.
		WaitForOrder(TransactionId);
		return;
	}
}

void UStageCloudCommerceBackend::RetryLater(const FGuid& TransactionId)
{
	FCheckout* Checkout = Checkouts.Find(TransactionId);
	UGameInstance* GameInstance = GetTypedOuter<UGameInstance>();
	if (!Checkout || !GameInstance)
	{
		return;
	}
	GameInstance->GetTimerManager().SetTimer(Checkout->RetryTimer, FTimerDelegate::CreateUObject(this, &ThisClass::WaitForOrder, TransactionId),
		StageCloudCommerce::RetrySeconds, /*bLoop*/ false);
}

void UStageCloudCommerceBackend::Finish(const FGuid& TransactionId, bool bApproved, const FText& Reason)
{
	FCheckout Checkout;
	if (!Checkouts.RemoveAndCopyValue(TransactionId, Checkout))
	{
		return;
	}
	if (UGameInstance* GameInstance = GetTypedOuter<UGameInstance>())
	{
		GameInstance->GetTimerManager().ClearTimer(Checkout.RetryTimer);
	}
	UE_LOG(LogStageCraft, Log, TEXT("Cloud store: transaction %s %s."), *TransactionId.ToString(), bApproved ? TEXT("paid") : TEXT("not completed"));
	Checkout.OnProcessed.ExecuteIfBound(bApproved, Reason);
}

bool UStageCloudCommerceBackend::CancelPurchase(const FGuid& TransactionId)
{
	if (!Checkouts.Contains(TransactionId))
	{
		return false;
	}
	Finish(TransactionId, false, LOCTEXT("Cancelled", "Stopped waiting for the payment. If you completed it, buy the listing again: you will not be charged twice."));
	return true;
}

bool UStageCloudCommerceBackend::IsWaitingForPayment(const FGuid& ListingId) const
{
	for (const TPair<FGuid, FCheckout>& Entry : Checkouts)
	{
		if (Entry.Value.ListingId == ListingId)
		{
			return true;
		}
	}
	return false;
}

void UStageCloudCommerceBackend::BeginDestroy()
{
	// Nothing is answered after this: the economy that owns this backend is going away with the game instance.
	Checkouts.Reset();
	Super::BeginDestroy();
}

#undef LOCTEXT_NAMESPACE
