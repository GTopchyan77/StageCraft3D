// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Cloud/StageCloudTypes.h"
#include "Engine/TimerHandle.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "StageCloudSubsystem.generated.h"

/**
 * The connection to StageCraft Cloud (Docs/ADR/0032 §5–§6): the server address, the signed-in account and its session, and the one path
 * every cloud request takes.
 *
 * Ownership and lifetime:
 *  - The source of truth for "who is signed in" on this install. The session token lives only here and in the credential vault
 *    (ADR 0023, DPAPI), bound to the server address it was issued by, so a token is never sent to another server. It never enters
 *    logs, config, saves or messages, and is wiped from memory on sign-out.
 *  - Sign-in is the RFC 8628 device flow: the app shows a code and opens the server's page in the browser; the user signs in there and
 *    the app's poll (at the server's interval, on a game-instance timer) receives the session. The app never sees a password.
 *  - Requests run on the engine HTTP module and answer exactly once on the game thread, unless this subsystem is deinitialized first
 *    (then they are cancelled and never answer; their owners are game-instance subsystems that end with it). An authenticated request
 *    answered 401 ends the session.
 *  - Offline (BaseUrl empty): every request answers at once with no connection, and the marketplace and store stay local.
 *
 * Writes come from the Account panel and the marketplace through the public calls here; views bind to OnStateChanged. Never ticks.
 */
UCLASS(Config = Game)
class MODULARSCENEBUILDER_API UStageCloudSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem Interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	EStageCloudState GetState() const { return State; }
	bool IsOnline() const { return State != EStageCloudState::Offline; }
	bool IsSignedIn() const { return State == EStageCloudState::SignedIn; }

	/** Display data of the signed-in account; empty when signed out. */
	const FStageCloudAccount& GetAccount() const { return Account; }

	/** The code to show while SigningIn. */
	const FStageCloudDeviceCode& GetDeviceCode() const { return DeviceCode; }

	/** What happened last, user-facing ("Signed in as Gevor.", "The code expired."). */
	const FText& GetStatusMessage() const { return StatusMessage; }

	/** The server address in use (validated); empty when offline. */
	const FString& GetBaseUrl() const { return ResolvedBaseUrl; }

	/**
	 * Starts a device sign-in: asks the server for a code, shows it (GetDeviceCode) and opens the verification page in the browser.
	 * False, with a reason, when offline, already signed in or signing in.
	 */
	bool StartSignIn(FText& OutError);

	/** Stops waiting for a sign-in; the code is forgotten. */
	void CancelSignIn();

	/** Ends the session here and on the server, and removes it from the vault. */
	void SignOut();

	/** Reads the account again (GET /v1/me), for example after seller onboarding in the browser. */
	void RefreshAccount();

	/** Opens Url in the system browser after StageCloudJson::IsSafeBrowserUrl. False when it is not safe or cannot open. */
	bool OpenInBrowser(const FString& Url);

	/**
	 * Sends a request to the server. Path starts with "/v1/". Authenticated requests carry the session and are answered at once with no
	 * connection when signed out. TimeoutSeconds 0 uses RequestTimeoutSeconds. OnDone runs exactly once on the game thread (see the
	 * class comment for deinitialization).
	 */
	void Send(const FString& Verb, const FString& Path, TArray<uint8> Body, const FString& ContentType, bool bAuthenticated, float TimeoutSeconds,
		FOnStageCloudResponse OnDone);

	/** Send with a UTF-8 JSON body (or none when Json is empty). */
	void SendJson(const FString& Verb, const FString& Path, const FString& Json, bool bAuthenticated, FOnStageCloudResponse OnDone, float TimeoutSeconds = 0.f);

	/** Fetches an absolute address the server handed out (a short-lived package download), without the session. Refused unless safe. */
	void Download(const FString& Url, float TimeoutSeconds, FOnStageCloudResponse OnDone);

	/** Strings that must never leave the machine in a support report (the session token), for StageSupportReport::Redact. */
	TArray<FString> GetSecretsToRedact() const;

	/** State, account, device code or message changed. */
	FSimpleMulticastDelegate OnStateChanged;

#if WITH_DEV_AUTOMATION_TESTS
	/** Tests: every request goes to Transport instead of the network, and the browser is never opened. */
	using FTestTransport = TFunction<void(const FStageCloudHttpRequest& /*Request*/, FOnStageCloudResponse /*Answer*/)>;
	void SetTransportForTesting(FTestTransport InTransport);

	/** Tests: uses Url as the server (the vault is the test's own folder) and starts over signed out. */
	void SetBaseUrlForTesting(const FString& Url);

	/** Tests: the addresses opened in the "browser". */
	const TArray<FString>& GetOpenedUrlsForTesting() const { return OpenedUrls; }
#endif

protected:
	/** The server, for example https://cloud.stagecraft3d.com. Empty: offline (no cloud features, nothing is ever sent). */
	UPROPERTY(Config)
	FString BaseUrl;

	/** Seconds an ordinary request may take. */
	UPROPERTY(Config)
	float RequestTimeoutSeconds = 30.f;

private:
	void ApplyBaseUrl(const FString& Url);
	void LoadSession();
	void StoreSession();
	void DropSession(const FText& Message);
	void SetState(EStageCloudState NewState, const FText& Message);
	void PollDeviceToken();
	void HandleDeviceStarted(const FStageCloudResponse& Response, uint32 Serial);
	void HandleDevicePolled(const FStageCloudResponse& Response, uint32 Serial);
	void HandleAccount(const FStageCloudResponse& Response);
	void SchedulePoll(int32 Seconds);
	void Dispatch(FStageCloudHttpRequest Request, float TimeoutSeconds, bool bAuthenticated, FOnStageCloudResponse OnDone);

	EStageCloudState State = EStageCloudState::Offline;
	FString ResolvedBaseUrl;
	FString SessionToken;
	FStageCloudAccount Account;
	FStageCloudDeviceCode DeviceCode;
	FDateTime DeviceExpiresUtc;
	FText StatusMessage;
	FTimerHandle PollTimer;

	/** Bumped by every sign-in start, cancel and sign-out, so a late device answer is recognised and dropped. */
	uint32 SignInSerial = 0;

	/** Requests on the network, cancelled in Deinitialize. */
	TArray<TSharedPtr<class IHttpRequest, ESPMode::ThreadSafe>> InFlight;

#if WITH_DEV_AUTOMATION_TESTS
	FTestTransport TestTransport;
	TArray<FString> OpenedUrls;
#endif
};
