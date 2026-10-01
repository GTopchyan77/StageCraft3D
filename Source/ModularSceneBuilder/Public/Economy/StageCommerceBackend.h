// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Economy/StageEconomyTypes.h"
#include "StageCommerceBackend.generated.h"

/** Backend answer for one purchase. bApproved = false carries a user-facing reason. */
DECLARE_DELEGATE_TwoParams(FOnStagePurchaseProcessed, bool /*bApproved*/, const FText& /*Reason*/);

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
};

/** Offline backend: the local profile is the authority, so every validated purchase is approved. */
UCLASS()
class MODULARSCENEBUILDER_API UStageLocalCommerceBackend : public UStageCommerceBackend
{
	GENERATED_BODY()

public:
	virtual void ProcessPurchase(const FStagePurchaseRequest& Request, FOnStagePurchaseProcessed OnProcessed) override;
};
