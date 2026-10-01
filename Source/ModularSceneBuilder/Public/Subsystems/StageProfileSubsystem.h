// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Economy/StageEconomyTypes.h"
#include "StageProfileSubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnStageBalanceChanged, FGameplayTag, Currency, int64, NewBalance);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnStageEntitlementsChanged);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnStageProfileLoaded);

/**
 * The player's persistent state: profile, wallet, owned entitlements, purchase history.
 *
 * On the GameInstance, so it survives level travel, and saved to a SaveGame slot so it survives
 * restarts. This class stores and persists; it does not decide anything. Everything that changes
 * money or ownership is private and reachable only from UStageEconomySubsystem (friend), which
 * validates first. Blueprints and widgets get read access and change events only.
 *
 * Integrity: the save carries a salted hash of the economy-relevant fields. A save edited outside
 * the game loads, but IsIntegrityValid() turns false and the economy refuses to spend from it.
 * That stops casual file editing, not a determined cheat with a debugger: anything sold for real
 * money must be validated by a server (see UStageCommerceBackend).
 */
UCLASS(Config = Game)
class MODULARSCENEBUILDER_API UStageProfileSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

	friend class UStageEconomySubsystem;

public:
	//~ Begin USubsystem Interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	UFUNCTION(BlueprintPure, Category = "StageCraft|Profile")
	const FStagePlayerProfile& GetProfile() const { return Profile; }

	UFUNCTION(BlueprintPure, Category = "StageCraft|Profile")
	int64 GetBalance(FGameplayTag Currency) const;

	/** Exact match: owning StageCraft.Entitlement.Item does not imply owning every item. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Profile")
	bool HasEntitlement(FGameplayTag Entitlement) const;

	UFUNCTION(BlueprintPure, Category = "StageCraft|Profile")
	const FGameplayTagContainer& GetEntitlements() const { return Profile.Entitlements; }

	UFUNCTION(BlueprintPure, Category = "StageCraft|Profile")
	int32 GetPlayerLevel() const { return Profile.Level; }

	/** False when the loaded save failed its integrity check. Spending is frozen until the profile is reset. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Profile")
	bool IsIntegrityValid() const { return bIntegrityValid; }

	/** Cosmetic only, so it is not economy-guarded. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Profile")
	void SetDisplayName(const FString& NewName);

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Profile")
	FOnStageBalanceChanged OnBalanceChanged;

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Profile")
	FOnStageEntitlementsChanged OnEntitlementsChanged;

	/** After the initial load and after a reset or reload. */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Profile")
	FOnStageProfileLoaded OnProfileLoaded;

	// --- Development only (compiled out of Shipping) ---

	/** Back to a fresh profile with the configured starting balances, and saved. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Profile|Dev", meta = (DevelopmentOnly))
	void DevResetProfile();

	/** Re-reads the save slot, e.g. to test persistence or the integrity check. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Profile|Dev", meta = (DevelopmentOnly))
	void DevReloadProfile();

	UFUNCTION(BlueprintPure, Category = "StageCraft|Profile|Dev")
	const FString& GetSaveSlotName() const { return SaveSlotName; }

protected:
	UPROPERTY(Config)
	FString SaveSlotName = TEXT("StageCraftProfile");

	/** Balances of a brand-new profile. */
	UPROPERTY(Config)
	TArray<FStageCurrencyAmount> StartingBalances;

	/** Off: the profile lives in memory only (useful for automated tests and kiosks). */
	UPROPERTY(Config)
	bool bPersistProfile = true;

private:
	// --- Economy-only mutation (friend UStageEconomySubsystem). Callers validate before calling. ---

	/** Debits the record's AmountPaid, grants its entitlements, appends it to the history, then saves. All or nothing. */
	bool CommitPurchase(const FStagePurchaseRecord& Record);

	/** Adds (or with a negative amount removes) currency. Fails if the balance would go below zero. */
	bool AdjustBalance(const FGameplayTag& Currency, int64 Delta);

	void LoadOrCreateProfile();
	void ResetToNewProfile();
	void RequestSave();
	void HandleSaveFinished(const FString& SlotName, int32 UserIndex, bool bSuccess);
	FString ComputeIntegrityHash() const;

	FStagePlayerProfile Profile;
	bool bIntegrityValid = true;
	bool bSaveInFlight = false;
	bool bSaveQueued = false;
};
