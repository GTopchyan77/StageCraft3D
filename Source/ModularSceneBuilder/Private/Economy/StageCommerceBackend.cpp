// Copyright Epic Games, Inc. All Rights Reserved.

#include "Economy/StageCommerceBackend.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageCommerceBackend)

void UStageLocalCommerceBackend::ProcessPurchase(const FStagePurchaseRequest& Request, FOnStagePurchaseProcessed OnProcessed)
{
	OnProcessed.ExecuteIfBound(/*bApproved*/ true, FText::GetEmpty());
}
