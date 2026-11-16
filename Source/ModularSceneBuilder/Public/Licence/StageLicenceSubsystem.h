// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/TimerHandle.h"
#include "GameplayTagContainer.h"
#include "Licence/StageLicenceTypes.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "StageLicenceSubsystem.generated.h"

DECLARE_MULTICAST_DELEGATE(FOnStageLicenceChanged);

/**
 * The StageCraft Pro licence on this install (Docs/ADR/0034 §17.3): the only owner of the signed token, and the source of truth for
 * which Pro entitlements this computer has right now.
 *
 * Ownership and lifetime:
 *  - Game instance lifetime. The token is kept in <user data>/StageCraft/Licence.json together with the newest time this install has
 *    seen (the clock-rollback check), written atomically. The file is not trusted: every load verifies the signature
 *    (StageLicenceToken::Verify against the compiled-in keys) and the machine, so editing or copying it gives nothing.
 *  - The token comes only from the server (POST /v1/licences) while signed in to StageCraft Cloud (UStageCloudSubsystem). It is
 *    refreshed at sign-in, at start and every RefreshIntervalHours on a timer; an authoritative "no subscription" answer removes it;
 *    an unreachable server keeps it until it expires (offline use, 14 days by default). A one-shot timer re-evaluates it at expiry.
 *  - Never ticks, never polls the world. Requests answer on the game thread; Deinitialize cancels them through the cloud subsystem.
 *
 * Readers: AStageCraftGameModeBase's evaluators (through HasPro, deny by default) and the Preferences panel (status, OnLicenceChanged).
 * Writers: AModularPlayerController's Pro requests only (refresh, checkout, manage, release). Pro is never written to the profile: it
 * expires and is server-signed, which a profile entitlement is not (Economy security 1, 7, 8).
 *
 * Development builds: the StageCraft.Licence.DevAssumePro console variable (absent from Shipping) makes every Pro entitlement present.
 */
UCLASS(Config = Game)
class MODULARSCENEBUILDER_API UStageLicenceSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem Interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	/**
	 * Whether GameInstance's licence grants Tag (an exact StageCraft.Entitlement.Pro.* tag). False without a game instance or this
	 * subsystem: deny by default. The GameMode asks here.
	 */
	static bool HasPro(const class UGameInstance* GameInstance, const FGameplayTag& Tag);

	/** True outside Shipping while StageCraft.Licence.DevAssumePro is non-zero. Always false in Shipping. */
	static bool IsDevAssumeProActive();

	/** Exact match against the verified, unexpired licence (or the development override). */
	bool HasProEntitlement(const FGameplayTag& Tag) const;

	EStageLicenceStatus GetStatus() const { return Status; }

	/** What happened last, user-facing ("StageCraft Pro is active until 30 Nov 2026.", "No subscription on this account."). */
	const FText& GetStatusMessage() const { return StatusMessage; }

	/** The verified licence's expiry; zero without one. */
	FDateTime GetExpiresUtc() const { return ExpiresUtc; }

	/** The verified licence's plan ("monthly", "yearly"); empty without one. */
	const FString& GetPlan() const { return Plan; }

	const FStageSubscriptionInfo& GetSubscription() const { return Subscription; }

	/** A licence request is on its way to the server. */
	bool IsRequestInFlight() const { return RequestsInFlight > 0; }

	/** A checkout page is open in the browser and the subscription is being watched. */
	bool IsWaitingForCheckout() const { return bWaitingForCheckout; }

	// --- Requests (AModularPlayerController only; each refuses with a user-facing reason) ---

	/** Asks the server for a fresh licence for this computer. Needs a signed-in account. */
	bool RequestRefresh(FText& OutError);

	/** Opens the Stripe checkout for Plan in the browser, then watches the subscription and refreshes the licence when it is active. */
	bool StartCheckout(EStageProPlan Plan, FText& OutError);

	/** Stops watching a checkout (the page may still be paid; REFRESH picks it up). */
	void CancelCheckoutWait();

	/** Opens the Stripe Billing Portal (cancel, card, invoices) in the browser. */
	bool OpenManageSubscription(FText& OutError);

	/** Frees this computer's place on the account and removes the licence here. */
	bool ReleaseThisMachine(FText& OutError);

	/** Status, licence, subscription, message or the development override changed. */
	FOnStageLicenceChanged OnLicenceChanged;

#if WITH_DEV_AUTOMATION_TESTS
	/** Tests: the machine id tokens are judged against (instead of this computer's). Re-evaluates the stored licence. */
	void SetMachineIdForTesting(const FString& MachineId);

	/** Tests: the machine id in use. */
	const FString& GetMachineIdForTesting() const { return MachineId; }

	/** Tests: where the licence file is. */
	FString GetLicencePathForTesting() const { return GetLicencePath(); }
#endif

protected:
	/** Hours between licence refreshes while signed in. Far below the offline window, so a renewed subscription never lapses. */
	UPROPERTY(Config)
	float RefreshIntervalHours = 12.f;

	/** Minutes a checkout is watched (Stripe Checkout sessions last at least 30 minutes). */
	UPROPERTY(Config)
	float CheckoutWaitMinutes = 31.f;

	/** Seconds the server may hold one subscription long poll (the server allows 0..25). */
	UPROPERTY(Config)
	int32 SubscriptionWaitSeconds = 25;

private:
	struct FStoredLicence
	{
		FString Token;
		int64 HighWaterUnix = 0;
	};

	FString GetLicencePath() const;
	FStoredLicence LoadStored() const;
	bool SaveStored(const FStoredLicence& Stored) const;
	/** Removes the file and forgets the current token. */
	void DeleteStored();

	/** Verifies and evaluates Token and makes it the current state (or the reason it is not). Broadcasts when anything changed. */
	void ApplyToken(const FString& Token, const FText& SuccessMessage);
	/** Drops the licence's entitlements; KeptExpiresUtc is shown for an expired one. */
	void ClearLicence(EStageLicenceStatus NewStatus, const FText& Message, FDateTime KeptExpiresUtc = FDateTime());
	void SetStatusMessage(const FText& Message);
	void ScheduleExpiryCheck();
	void HandleExpiryCheck();

	void HandleCloudStateChanged();
	void UpdateRefreshTimer();
	void HandleLicenceAnswer(const struct FStageCloudResponse& Response);
	/** Serial 0 reads the subscription for display; CheckoutSerial continues watching that checkout. */
	void FetchSubscription(int32 WaitSeconds, uint32 Serial = 0);
	void HandleSubscriptionAnswer(const struct FStageCloudResponse& Response, uint32 Serial);
	void HandleCheckoutAnswer(const struct FStageCloudResponse& Response);
	void HandlePortalAnswer(const struct FStageCloudResponse& Response);
	void HandleReleaseAnswer(const struct FStageCloudResponse& Response);
	void StopCheckoutWait();
	void HandleDevOverrideChanged();

	class UStageCloudSubsystem* GetCloud() const;

	EStageLicenceStatus Status = EStageLicenceStatus::Free;
	FText StatusMessage;
	FGameplayTagContainer Entitlements;
	FDateTime ExpiresUtc;
	FString Plan;
	FStageSubscriptionInfo Subscription;
	FString MachineId;

	/** The newest Unix time seen by this install (stored with the token); a clock far behind it is a rollback. */
	int64 HighWaterUnix = 0;

	/** The token the state was last evaluated from, re-evaluated at expiry (not re-read: the file may be gone or stale). */
	FString CurrentToken;

	int32 RequestsInFlight = 0;
	bool bWaitingForCheckout = false;

	/** Bumped by every checkout start and stop, so a long poll of an abandoned checkout is recognised and ends there. */
	uint32 CheckoutSerial = 0;
	FDateTime CheckoutDeadlineUtc;

	FTimerHandle ExpiryTimer;
	FTimerHandle RefreshTimer;
	FTimerHandle CheckoutRetryTimer;
	FDelegateHandle CloudStateHandle;
	FDelegateHandle DevOverrideHandle;
	bool bWasSignedIn = false;
};
