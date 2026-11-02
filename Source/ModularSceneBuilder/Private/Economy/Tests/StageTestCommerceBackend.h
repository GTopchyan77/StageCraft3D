// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Economy/StageCommerceBackend.h"
#include "StageTestCommerceBackend.generated.h"

/**
 * Automation tests only: a commerce backend whose answers the test controls, to drive UStageEconomySubsystem through approvals,
 * refusals, late answers and answers that never come (Docs/ADR/0021-community-marketplace.md §14).
 *
 * Never configured as CommerceBackendClass outside tests; the test harness (FStageTestGameInstance) selects it through the
 * economy's class default object and restores it afterwards. It ships as an unreachable class (UHT cannot compile a UCLASS out),
 * like UStageMockAIToolProvider. Game thread only.
 */
UCLASS(NotBlueprintable, Transient, HideDropdown)
class UStageTestCommerceBackend : public UStageCommerceBackend
{
	GENERATED_BODY()

public:
	enum class EMode : uint8
	{
		/** Approves inside the call, like the local backend. */
		ApproveNow,
		/** Refuses inside the call with RefusalReason. */
		DeclineNow,
		/** Keeps the request; the test answers it later with AnswerHeld (or never). */
		Hold,
	};

	//~ Begin UStageCommerceBackend Interface
	virtual void ProcessPurchase(const FStagePurchaseRequest& Request, FOnStagePurchaseProcessed OnProcessed) override;
	virtual bool CanSellCredits() const override { return bSellsCredits; }
	virtual void ProcessCreditTopUp(const FStageTopUpRequest& Request, FOnStageTopUpProcessed OnProcessed) override;
	//~ End UStageCommerceBackend Interface

	/** The backend the economy of this game instance created (it is the economy's sub-object), or null. */
	static UStageTestCommerceBackend* Find(const class UGameInstance& GameInstance);

	/** Answers the held purchase TransactionId (approve or refuse). False when no such request is held. Calls the economy back. */
	bool AnswerHeldPurchase(const FGuid& TransactionId, bool bApprove);

	/** Answers the held top-up with GrantedAmount (0 refuses). False when no such request is held. */
	bool AnswerHeldTopUp(const FGuid& TransactionId, int64 GrantedAmount);

	/**
	 * Calls the economy back for TransactionId once more, although it was already answered: a backend breaking the exactly-once
	 * contract. Only purchases answered by AnswerHeldPurchase are remembered for this.
	 */
	bool AnswerAgain(const FGuid& TransactionId, bool bApprove);

	EMode Mode = EMode::ApproveNow;
	bool bSellsCredits = false;
	FText RefusalReason;

	/** Every request seen, in order (purchases and top-ups have separate lists). */
	TArray<FStagePurchaseRequest> PurchaseRequests;
	TArray<FStageTopUpRequest> TopUpRequests;

private:
	TMap<FGuid, FOnStagePurchaseProcessed> HeldPurchases;
	TMap<FGuid, FOnStageTopUpProcessed> HeldTopUps;
	TMap<FGuid, FOnStagePurchaseProcessed> AnsweredPurchases;
};
