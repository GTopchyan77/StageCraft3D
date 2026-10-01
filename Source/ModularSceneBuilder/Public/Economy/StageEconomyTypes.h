// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "NativeGameplayTags.h"
#include "UObject/PrimaryAssetId.h"
#include "StageEconomyTypes.generated.h"

/**
 * Outcome of every economy-guarded request (purchase, placement, parameter edit). One closed enum
 * so UI can branch on it (open the shop on Locked, show "not enough credits" on InsufficientFunds)
 * without parsing text.
 */
UENUM(BlueprintType)
enum class EStageEconomyResult : uint8
{
	Success,
	/** Malformed request: null product or target, negative amount, unknown currency. */
	InvalidRequest,
	/** The product is not in the loaded product catalog (also rejects objects forged at runtime). */
	UnknownProduct,
	/** Everything the product grants is already owned. */
	AlreadyOwned,
	/** An unlock condition of the product is not met (see the result message for which). */
	RequirementsNotMet,
	InsufficientFunds,
	/** A purchase of the same product is still being processed. */
	PurchasePending,
	/** The commerce backend declined (store, payment provider or server). */
	BackendRejected,
	/** The saved profile failed its integrity check; spending is frozen until it is reset. */
	IntegrityViolation,
	/** The item or parameter needs an entitlement the player does not own. */
	Locked,
	/** A session rule (GameMode) forbids the action, e.g. the stage's item limit is reached. */
	SessionRuleViolation,
};

/** How an item, parameter or entitlement relates to the current player. */
UENUM(BlueprintType)
enum class EStageOwnershipState : uint8
{
	/** No entitlement required. */
	Free,
	Owned,
	/** Not owned, and a product that grants it is for sale to this player (conditions met; funds are checked at purchase). */
	Purchasable,
	/** Not owned, and no product can be bought now (none sells it, or its conditions are unmet). */
	Locked,
};

/** Result of a guarded request. Message is user-facing; Code is for logic. */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageEconomyResultInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Economy")
	EStageEconomyResult Code = EStageEconomyResult::Success;

	UPROPERTY(BlueprintReadOnly, Category = "Economy")
	FText Message;

	bool IsSuccess() const { return Code == EStageEconomyResult::Success; }

	static FStageEconomyResultInfo Ok() { return FStageEconomyResultInfo(); }
	static FStageEconomyResultInfo Fail(EStageEconomyResult InCode, const FText& InMessage)
	{
		FStageEconomyResultInfo Result;
		Result.Code = InCode;
		Result.Message = InMessage;
		return Result;
	}
};

/** An amount of one currency. Amounts are whole units (no floating point money). */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageCurrencyAmount
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, SaveGame, Category = "Economy", meta = (Categories = "StageCraft.Currency"))
	FGameplayTag Currency;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, SaveGame, Category = "Economy", meta = (ClampMin = "0"))
	int64 Amount = 0;
};

/** One completed purchase, kept in the profile for receipts, refunds and support. */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStagePurchaseRecord
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, SaveGame, Category = "Economy")
	FGuid TransactionId;

	UPROPERTY(BlueprintReadOnly, SaveGame, Category = "Economy")
	FPrimaryAssetId ProductId;

	/** UTC. */
	UPROPERTY(BlueprintReadOnly, SaveGame, Category = "Economy")
	FDateTime Timestamp;

	UPROPERTY(BlueprintReadOnly, SaveGame, Category = "Economy")
	TArray<FStageCurrencyAmount> AmountPaid;

	UPROPERTY(BlueprintReadOnly, SaveGame, Category = "Economy")
	FGameplayTagContainer EntitlementsGranted;
};

/**
 * Everything persistent about the player. Owned by UStageProfileSubsystem; only
 * UStageEconomySubsystem changes balances, entitlements and history.
 */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStagePlayerProfile
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, SaveGame, Category = "Profile")
	FString DisplayName;

	/** Feeds level-based unlock conditions. There is no XP system yet; it is set by future progression code. */
	UPROPERTY(BlueprintReadOnly, SaveGame, Category = "Profile")
	int32 Level = 1;

	UPROPERTY(BlueprintReadOnly, SaveGame, Category = "Profile")
	TMap<FGameplayTag, int64> Balances;

	UPROPERTY(BlueprintReadOnly, SaveGame, Category = "Profile")
	FGameplayTagContainer Entitlements;

	UPROPERTY(BlueprintReadOnly, SaveGame, Category = "Profile")
	TArray<FStagePurchaseRecord> PurchaseHistory;
};

/** A purchase handed to the commerce backend. TransactionId is unique per attempt (idempotency key for a server). */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStagePurchaseRequest
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Economy")
	FGuid TransactionId;

	UPROPERTY(BlueprintReadOnly, Category = "Economy")
	FPrimaryAssetId ProductId;

	UPROPERTY(BlueprintReadOnly, Category = "Economy")
	TArray<FStageCurrencyAmount> Price;
};

namespace StageCraftTags
{
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Currency);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Currency_Credits);

	/** Roots for what can be owned. Content defines leaves (e.g. StageCraft.Entitlement.Item.MovingHeadWash) in the tag table. */
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Entitlement);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Entitlement_Item);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Entitlement_Feature);
}
