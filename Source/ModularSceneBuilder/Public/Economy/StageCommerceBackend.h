// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Economy/StageEconomyTypes.h"
#include "StageCommerceBackend.generated.h"

/** Backend answer for one purchase. bApproved = false carries a user-facing reason. */
DECLARE_DELEGATE_TwoParams(FOnStagePurchaseProcessed, bool /*bApproved*/, const FText& /*Reason*/);

/** Backend answer for one currency top-up: the amount granted (0 = declined) and, when declined, a user-facing reason. */
DECLARE_DELEGATE_TwoParams(FOnStageTopUpProcessed, int64 /*GrantedAmount*/, const FText& /*Reason*/);

/**
 * Where a purchase is settled: the seam between the game's economy rules and whoever is
 * authoritative over money.
 *
 * UStageEconomySubsystem validates a purchase locally (catalog, ownership, conditions, funds),
 * then hands it here and applies it only when the backend approves. Today that is
 * UStageLocalCommerceBackend (offline, approves at once). A marketplace replaces it with a backend
 * that calls a server or a platform store and answers asynchronously; the economy, the UI and the
 * profile do not change. A server backend must re-validate everything itself and treat
 * TransactionId as an idempotency key, because the client is never trusted.
 *
 * Selected by UStageEconomySubsystem's CommerceBackendClass (DefaultGame.ini).
 */
UCLASS(Abstract)
class MODULARSCENEBUILDER_API UStageCommerceBackend : public UObject
{
	GENERATED_BODY()

public:
	/** Must call OnProcessed exactly once, now or later (on the game thread). */
	virtual void ProcessPurchase(const FStagePurchaseRequest& Request, FOnStagePurchaseProcessed OnProcessed)
		PURE_VIRTUAL(UStageCommerceBackend::ProcessPurchase, );

	/**
	 * True only for a backend whose answers come from an authoritative server that re-validates the purchase and moves real money
	 * (Economy security 7). Real-money marketplace listings are refused as ServerRequired unless this is true (ADR 0021 §5).
	 */
	virtual bool CanSettleRealMoney() const { return false; }

	/**
	 * Seconds this backend needs to answer Request before the economy ends it as BackendTimeout (ADR 0021 §14). The default is the
	 * economy's own deadline (DefaultSeconds); a backend that sends the buyer to a hosted checkout page asks for that page's lifetime
	 * (ADR 0032 §6). The economy clamps the answer to 1 s..1 h.
	 */
	virtual float GetSettlementTimeoutSeconds(const FStagePurchaseRequest& Request, float DefaultSeconds) const { return DefaultSeconds; }

	/**
	 * Abandons the purchase TransactionId while it still waits (the buyer closed the checkout page). A backend that can, answers that
	 * purchase now as declined, through its OnProcessed (still exactly once) and returns true. The default cannot.
	 */
	virtual bool CancelPurchase(const FGuid& TransactionId) { return false; }

	/**
	 * True only for a server backend that sells in-app currency for real money (Economy security 7). The economy refuses top-ups
	 * as ServerRequired otherwise, and never calls ProcessCreditTopUp (ADR 0022 §11.4).
	 */
	virtual bool CanSellCredits() const { return false; }

	/**
	 * Settles a currency top-up: the store charges the user and answers the amount actually granted (0 = declined, with a
	 * reason). Must call OnProcessed exactly once, on the game thread. The default declines.
	 */
	virtual void ProcessCreditTopUp(const FStageTopUpRequest& Request, FOnStageTopUpProcessed OnProcessed);
};

/** Offline backend: the local profile is the authority, so every validated purchase is approved. */
UCLASS()
class MODULARSCENEBUILDER_API UStageLocalCommerceBackend : public UStageCommerceBackend
{
	GENERATED_BODY()

public:
	virtual void ProcessPurchase(const FStagePurchaseRequest& Request, FOnStagePurchaseProcessed OnProcessed) override;
};
