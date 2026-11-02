// Copyright Epic Games, Inc. All Rights Reserved.

#include "Economy/StageTopUpRules.h"

#define LOCTEXT_NAMESPACE "StageCraftEconomy"

FStageEconomyResultInfo StageTopUpRules::Validate(const FStageTopUpContext& Context)
{
	if (!Context.Profile)
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("TopUpNoProfile", "There is no profile to add credits to."));
	}
	if (!Context.bIntegrityValid)
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::IntegrityViolation, LOCTEXT("TopUpFrozen", "Spending is frozen because the profile failed its integrity check."));
	}

	const int64* Balance = Context.Profile->Balances.Find(Context.Currency);
	const int64 Current = Balance ? *Balance : 0;
	if (!Context.Currency.MatchesTag(StageCraftTags::Currency) || Context.Currency == StageCraftTags::Currency
		|| Context.Amount < 1 || Context.Amount > Context.MaxAmount || Current > MAX_int64 - Context.Amount)
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest,
			FText::Format(LOCTEXT("TopUpAmount", "Choose an amount of 1-{0} credits."), FText::AsNumber(Context.MaxAmount)));
	}
	if (Context.bPending)
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::PurchasePending, LOCTEXT("TopUpPending", "A credit purchase is already in progress."));
	}
	// Currency is sold for real money, and only a server may do that (Economy security 7).
	if (!Context.bCanSellCredits)
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::ServerRequired,
			LOCTEXT("TopUpServerRequired", "Buying credits needs the StageCraft online store, which is not connected in this version."));
	}
	return FStageEconomyResultInfo::Ok();
}

#undef LOCTEXT_NAMESPACE
