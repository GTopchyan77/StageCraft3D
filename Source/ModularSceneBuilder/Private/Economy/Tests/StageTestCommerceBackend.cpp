// Copyright Epic Games, Inc. All Rights Reserved.

#include "Economy/Tests/StageTestCommerceBackend.h"

#include "Engine/GameInstance.h"
#include "Subsystems/StageEconomySubsystem.h"
#include "UObject/UObjectHash.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageTestCommerceBackend)

void UStageTestCommerceBackend::ProcessPurchase(const FStagePurchaseRequest& Request, FOnStagePurchaseProcessed OnProcessed)
{
	PurchaseRequests.Add(Request);
	switch (Mode)
	{
	case EMode::ApproveNow:
		OnProcessed.ExecuteIfBound(true, FText::GetEmpty());
		return;
	case EMode::DeclineNow:
		OnProcessed.ExecuteIfBound(false, RefusalReason);
		return;
	case EMode::Hold:
	default:
		HeldPurchases.Add(Request.TransactionId, MoveTemp(OnProcessed));
		return;
	}
}

void UStageTestCommerceBackend::ProcessCreditTopUp(const FStageTopUpRequest& Request, FOnStageTopUpProcessed OnProcessed)
{
	TopUpRequests.Add(Request);
	switch (Mode)
	{
	case EMode::ApproveNow:
		OnProcessed.ExecuteIfBound(Request.Amount, FText::GetEmpty());
		return;
	case EMode::DeclineNow:
		OnProcessed.ExecuteIfBound(0, RefusalReason);
		return;
	case EMode::Hold:
	default:
		HeldTopUps.Add(Request.TransactionId, MoveTemp(OnProcessed));
		return;
	}
}

UStageTestCommerceBackend* UStageTestCommerceBackend::Find(const UGameInstance& GameInstance)
{
	const UStageEconomySubsystem* Economy = GameInstance.GetSubsystem<UStageEconomySubsystem>();
	UStageTestCommerceBackend* Found = nullptr;
	if (Economy)
	{
		ForEachObjectWithOuter(Economy, [&Found](UObject* Object)
		{
			if (!Found)
			{
				Found = Cast<UStageTestCommerceBackend>(Object);
			}
		}, EGetObjectsFlags::None);
	}
	return Found;
}

bool UStageTestCommerceBackend::AnswerHeldPurchase(const FGuid& TransactionId, bool bApprove)
{
	FOnStagePurchaseProcessed Callback;
	if (!HeldPurchases.RemoveAndCopyValue(TransactionId, Callback))
	{
		return false;
	}
	AnsweredPurchases.Add(TransactionId, Callback);
	Callback.ExecuteIfBound(bApprove, bApprove ? FText::GetEmpty() : RefusalReason);
	return true;
}

bool UStageTestCommerceBackend::AnswerHeldTopUp(const FGuid& TransactionId, int64 GrantedAmount)
{
	FOnStageTopUpProcessed Callback;
	if (!HeldTopUps.RemoveAndCopyValue(TransactionId, Callback))
	{
		return false;
	}
	Callback.ExecuteIfBound(GrantedAmount, GrantedAmount > 0 ? FText::GetEmpty() : RefusalReason);
	return true;
}

bool UStageTestCommerceBackend::AnswerAgain(const FGuid& TransactionId, bool bApprove)
{
	const FOnStagePurchaseProcessed* Callback = AnsweredPurchases.Find(TransactionId);
	if (!Callback)
	{
		return false;
	}
	Callback->ExecuteIfBound(bApprove, bApprove ? FText::GetEmpty() : RefusalReason);
	return true;
}
