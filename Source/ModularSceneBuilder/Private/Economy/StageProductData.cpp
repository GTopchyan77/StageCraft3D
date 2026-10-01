// Copyright Epic Games, Inc. All Rights Reserved.

#include "Economy/StageProductData.h"

#include "Economy/StageUnlockCondition.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageProductData)

#define LOCTEXT_NAMESPACE "StageCraftProductData"

const FPrimaryAssetType UStageProductData::ProductAssetType(TEXT("StageProduct"));

FPrimaryAssetId UStageProductData::GetPrimaryAssetId() const
{
	return FPrimaryAssetId(ProductAssetType, GetFName());
}

#if WITH_EDITOR
EDataValidationResult UStageProductData::IsDataValid(FDataValidationContext& Context) const
{
	EDataValidationResult Result = Super::IsDataValid(Context);

	if (GrantedEntitlements.IsEmpty())
	{
		Context.AddError(LOCTEXT("NoGrants", "A product must grant at least one entitlement."));
		Result = EDataValidationResult::Invalid;
	}

	TSet<FGameplayTag> SeenCurrencies;
	for (const FStageCurrencyAmount& Cost : Price)
	{
		bool bDuplicate = false;
		SeenCurrencies.Add(Cost.Currency, &bDuplicate);
		if (!Cost.Currency.MatchesTag(StageCraftTags::Currency) || Cost.Amount < 0 || bDuplicate)
		{
			Context.AddError(LOCTEXT("BadPrice", "Each price entry needs a StageCraft.Currency tag, listed once, with an amount of 0 or more."));
			Result = EDataValidationResult::Invalid;
			break;
		}
	}

	for (const UStageUnlockCondition* Condition : Requirements)
	{
		if (!Condition)
		{
			Context.AddError(LOCTEXT("EmptyRequirement", "Requirements contains an empty entry; pick a condition class or remove it."));
			Result = EDataValidationResult::Invalid;
			break;
		}
	}

	return Result;
}
#endif

#undef LOCTEXT_NAMESPACE
