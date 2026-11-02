// Copyright Epic Games, Inc. All Rights Reserved.

#include "Economy/StageCommerceBackend.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageCommerceBackend)

#define LOCTEXT_NAMESPACE "StageCraftEconomy"

void UStageCommerceBackend::ProcessCreditTopUp(const FStageTopUpRequest& Request, FOnStageTopUpProcessed OnProcessed)
{
	OnProcessed.ExecuteIfBound(/*GrantedAmount*/ 0, LOCTEXT("TopUpUnsupported", "This store does not sell credits."));
}

void UStageLocalCommerceBackend::ProcessPurchase(const FStagePurchaseRequest& Request, FOnStagePurchaseProcessed OnProcessed)
{
	OnProcessed.ExecuteIfBound(/*bApproved*/ true, FText::GetEmpty());
}

#undef LOCTEXT_NAMESPACE
