// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Economy/StageCommerceBackend.h"
#include "Engine/TimerHandle.h"
#include "StageCloudCommerceBackend.generated.h"

/**
 * Real-money purchases through StageCraft Cloud (Docs/ADR/0032 §3, §6). The only commerce backend that can settle real money, and only
 * while signed in.
 *
 * A real-money marketplace listing:
 *  1. the server creates the order from its own listing price (the client sends only the listing and the economy's TransactionId,
 *     the idempotency key: asking again returns the same order and never charges twice);
 *  2. Stripe's hosted checkout opens in the system browser (card data never reaches StageCraft);
 *  3. the order is followed by long polls (the server answers when it changes, at most every 25 s) until the server, told by Stripe's
 *     signed webhook, reports it paid, or it expires, fails or the deadline passes;
 *  4. the economy is answered exactly once: approved only when the server says paid.
 * A listing the account already owns on the server is approved without a new charge (a purchase made on another computer).
 *
 * Everything else (credit purchases, store products) is settled locally as before (UStageLocalCommerceBackend): the local profile stays
 * the authority over credits, with the honest limit of Economy security 7. Owned by UStageEconomySubsystem. Game thread only.
 */
UCLASS(Config = Game)
class MODULARSCENEBUILDER_API UStageCloudCommerceBackend : public UStageCommerceBackend
{
	GENERATED_BODY()

public:
	//~ Begin UStageCommerceBackend Interface
	virtual void ProcessPurchase(const FStagePurchaseRequest& Request, FOnStagePurchaseProcessed OnProcessed) override;
	virtual bool CanSettleRealMoney() const override;
	virtual float GetSettlementTimeoutSeconds(const FStagePurchaseRequest& Request, float DefaultSeconds) const override;
	virtual bool CancelPurchase(const FGuid& TransactionId) override;
	//~ End UStageCommerceBackend Interface

	//~ Begin UObject Interface
	virtual void BeginDestroy() override;
	//~ End UObject Interface

	/** True while a checkout of ListingId waits for payment. */
	bool IsWaitingForPayment(const FGuid& ListingId) const;

private:
	struct FCheckout
	{
		FGuid ListingId;
		FString OrderId;
		FDateTime DeadlineUtc;
		FOnStagePurchaseProcessed OnProcessed;
		FTimerHandle RetryTimer;
	};

	class UStageCloudSubsystem* GetCloud() const;

	/** The real-money listing Request buys, or unset for anything settled locally. */
	TOptional<FGuid> FindRealMoneyListing(const FStagePurchaseRequest& Request) const;

	void HandleOrderCreated(const struct FStageCloudResponse& Response, FGuid TransactionId);
	void WaitForOrder(FGuid TransactionId);
	void HandleOrderState(const struct FStageCloudResponse& Response, FGuid TransactionId);
	void RetryLater(const FGuid& TransactionId);
	void Finish(const FGuid& TransactionId, bool bApproved, const FText& Reason);

	UPROPERTY(Transient)
	TObjectPtr<class UStageLocalCommerceBackend> Local = nullptr;

	/** How long a checkout may wait for payment: Stripe Checkout sessions last at least 30 minutes. */
	UPROPERTY(Config)
	float CheckoutTimeoutSeconds = 1860.f;

	/** Seconds the server may hold a long poll (the server allows 0..25). */
	UPROPERTY(Config)
	int32 OrderWaitSeconds = 25;

	TMap<FGuid, FCheckout> Checkouts;
};
