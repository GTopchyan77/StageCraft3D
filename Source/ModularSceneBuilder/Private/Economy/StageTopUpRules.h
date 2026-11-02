// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Economy/StageEconomyTypes.h"

/** What the economy knows when it decides on a currency top-up (Docs/ADR/0022-dcc-bridge.md §11.4). */
struct FStageTopUpContext
{
	/** Null when there is no profile. */
	const FStagePlayerProfile* Profile = nullptr;
	bool bIntegrityValid = false;
	bool bPending = false;
	/** True only for a commerce backend that is a store selling currency (UStageCommerceBackend::CanSellCredits). */
	bool bCanSellCredits = false;
	FGameplayTag Currency;
	int64 Amount = 0;
	int64 MaxAmount = 0;
};

/** Pure top-up checks; no world, no UObjects; tested directly. */
namespace StageTopUpRules
{
	/**
	 * In order: profile, integrity, a concrete currency, 1..MaxAmount without overflowing the balance, nothing pending, a store
	 * that sells currency. Deny by default.
	 */
	FStageEconomyResultInfo Validate(const FStageTopUpContext& Context);
}
