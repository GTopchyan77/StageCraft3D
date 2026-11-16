// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "StageLicenceTypes.generated.h"

/** Where this install stands with StageCraft Pro (Docs/ADR/0034 §17). */
UENUM(BlueprintType)
enum class EStageLicenceStatus : uint8
{
	/** No licence on this computer. */
	Free,
	/** A verified, unexpired licence for this computer. */
	Pro,
	/** The licence ran out; refresh it online (Pro features are locked, scenes stay usable). */
	Expired,
	/** The stored licence cannot be used here (edited, another computer's, an untrusted key, the clock set back); see the message. */
	Invalid,
};

/** The Pro plans the server sells (Services/StageCraftCloud/src/subscriptions.ts PLANS). */
UENUM(BlueprintType)
enum class EStageProPlan : uint8
{
	Monthly,
	Yearly,
};

/** The account's subscription as the server last reported it (GET /v1/me/subscription). Display only: the token decides. */
struct FStageSubscriptionInfo
{
	/** False until the server has answered once while signed in. */
	bool bKnown = false;
	/** Stripe's status (active, past_due, canceled...) or "none". */
	FString Status = TEXT("none");
	bool bEntitled = false;
	FString Plan;
	/** Zero when unknown. */
	FDateTime PeriodEndUtc;
	bool bCancelAtPeriodEnd = false;
};
