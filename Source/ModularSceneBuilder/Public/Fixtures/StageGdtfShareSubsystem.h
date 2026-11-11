// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Fixtures/StageGdtfShareTypes.h"
#include "Interfaces/IHttpRequest.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "StageGdtfShareSubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnStageGdtfShareChanged);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStageGdtfShareRequestFinished, const FStageGdtfShareResultInfo&, Result);

/**
 * GDTF Share as an online fixture source, and the download manager for its files (Docs/ADR/0033 §18, roadmap Step 4).
 *
 * - **Account.** The API terms require every user of a client that connects directly to sign in with their own GDTF Share account.
 *   SignIn sends the user name and password once, over https, to the configured API root; the password is never stored, logged or
 *   kept after the request. Only the session cookie is kept, in memory, for this run. The session ends when the user signs out, the
 *   game instance ends, or GDTF Share answers 401 (two hours without a request); the user then signs in again.
 * - **List.** RefreshList fetches the whole list (getList.php) and parses it on a worker. It is kept in memory only and fetched again
 *   only when older than StageGdtfShare::ListMaxAgeSeconds (the terms ask for about 120 minutes) unless the user forces it.
 * - **Downloads.** Download fetches one revision (downloadFile.php) with a size cap and hands the bytes to the fixture library as a
 *   review (UStageFixtureLibrarySubsystem::RequestImportBytes): the file is validated like any GDTF file (ADR 0033 §8) and nothing
 *   reaches the library before the user adds it. Nothing is cached on disk, so the terms' 24-hour cache limit never applies; a file
 *   the user adds is their library copy, like a file they downloaded themselves. At most MaxConcurrentDownloads run at once; finished
 *   and failed downloads stay listed (at most MaxListedDownloads) until ClearFinishedDownloads.
 *
 * Who calls: AModularPlayerController's GDTF Share requests, after the GameMode allows online fixture sources; dev console commands.
 * Views read GetState, GetEntries (through StageGdtfShare::Search) and GetDownloads and follow OnChanged; request outcomes arrive on
 * OnRequestFinished. Requests return at once with a refusal, or Success when accepted.
 * Lifetime: the whole run (GameInstance). Game-thread API. Answers are matched by serial: one that belongs to an older session, list
 * request or download is dropped. Deinitialize cancels every request in flight.
 */
UCLASS(Config = Game)
class MODULARSCENEBUILDER_API UStageGdtfShareSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem Interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	static constexpr int32 MaxConcurrentDownloads = 4;
	static constexpr int32 MaxListedDownloads = 32;
	static constexpr int32 MaxUserNameLength = 128;
	static constexpr int32 MaxPasswordLength = 256;

	/**
	 * Signs in with the user's own GDTF Share account. Refused with InvalidRequest for an empty or over-long user name or password,
	 * Busy while signing in, NotAllowed when the configured API root is not https. Signing in while signed in starts a new session.
	 * The outcome arrives on OnRequestFinished; on success the list is fetched.
	 */
	FStageGdtfShareResultInfo SignIn(const FString& UserName, const FString& Password);

	/** Ends the session, cancels the list request and every download. The list stays visible until the next sign-in. Idempotent. */
	void SignOut();

	EStageGdtfShareState GetState() const { return State; }

	/** The name the session was opened with; empty when signed out. */
	const FString& GetUserName() const { return UserName; }

	/**
	 * Fetches the list. SignInRequired when signed out, Busy while a list request runs. When the list is younger than
	 * ListMaxAgeSeconds and bForce is false, nothing is sent and Success comes back with the list unchanged.
	 */
	FStageGdtfShareResultInfo RefreshList(bool bForce);

	bool IsListLoading() const { return bListLoading; }

	/** The last list fetched, in GDTF Share's order. Use StageGdtfShare::Search for display. */
	const TArray<FStageGdtfShareEntry>& GetEntries() const { return Entries; }

	/** When GetEntries was fetched; FDateTime() before the first list. */
	FDateTime GetListFetchedUtc() const { return ListFetchedUtc; }

	const FStageGdtfShareEntry* FindEntry(int32 RevisionId) const;

	/**
	 * Downloads a revision of the current list. SignInRequired when signed out, InvalidRequest for a revision not in the list, Busy
	 * when it is already downloading or MaxConcurrentDownloads run, TooLarge when the list states a size over the cap. Progress and
	 * the outcome show in GetDownloads (OnChanged); the outcome also arrives on OnRequestFinished.
	 */
	FStageGdtfShareResultInfo Download(int32 RevisionId);

	/** Stops a running download (it ends Cancelled). False when it is not running. */
	bool CancelDownload(int32 RevisionId);

	/** The download manager's rows, newest first. */
	const TArray<FStageGdtfShareDownload>& GetDownloads() const { return Downloads; }

	/** Removes delivered and failed rows. */
	void ClearFinishedDownloads();

	/** The attribution the API terms require wherever GDTF Share content is shown: "Powered by GDTF". */
	static FText GetAttribution();

	/** State, list or downloads changed. */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|GDTF Share")
	FOnStageGdtfShareChanged OnChanged;

	/** A sign-in, list request or download finished (success or failure), for the status bar. */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|GDTF Share")
	FOnStageGdtfShareRequestFinished OnRequestFinished;

#if WITH_DEV_AUTOMATION_TESTS
	/** Tests: every request goes to Transport instead of the network. Progress may be left unbound. */
	using FTestTransport = TFunction<void(const FStageGdtfShareHttpRequest& /*Request*/, FOnStageGdtfShareResponse /*Answer*/, FOnStageGdtfShareProgress /*Progress*/)>;
	void SetTransportForTesting(FTestTransport InTransport);

	/** Tests: the session cookie the next request sends. */
	const FString& GetSessionCookieForTesting() const { return SessionCookie; }

	/** Tests: makes the list look ListMaxAgeSeconds older, so RefreshList(false) fetches it again. */
	void AgeListForTesting();
#endif

protected:
	/** The API root. Empty uses StageGdtfShare::DefaultBaseUrl; a sync server (as the API terms recommend) can be set here. Must be https. */
	UPROPERTY(Config)
	FString BaseUrl;

	/** Seconds a sign-in or list request may take. Values <= 0 fall back to the default. */
	UPROPERTY(Config)
	float RequestTimeoutSeconds = 60.f;

	/** Seconds one file download may take. Values <= 0 fall back to the default. */
	UPROPERTY(Config)
	float DownloadTimeoutSeconds = 120.f;

private:
	FString GetBaseUrl() const;

	/** Sends Request; Answer runs on the game thread exactly once (also when the request cannot start). */
	void Send(FStageGdtfShareHttpRequest Request, float TimeoutSeconds, FOnStageGdtfShareResponse Answer, FOnStageGdtfShareProgress Progress, int32 DownloadRevisionId);

	void HandleSignIn(uint32 Serial, const FStageGdtfShareHttpResponse& Response);
	void HandleList(uint32 Serial, const FStageGdtfShareHttpResponse& Response);
	void FinishList(uint32 Serial, TArray<FStageGdtfShareEntry> Parsed, int32 Skipped, bool bParsed);
	void HandleDownloadProgress(int32 RevisionId, uint32 Serial, int64 Received, int64 Total);
	void HandleDownload(int32 RevisionId, uint32 Serial, const FStageGdtfShareHttpResponse& Response);

	/** Turns a failed answer into a result; a 401 ends the session (SessionExpired). */
	FStageGdtfShareResultInfo DescribeFailure(const FStageGdtfShareHttpResponse& Response, const FText& What);

	/** Ends the session, cancels everything in flight and reports Reason (unless empty). */
	void DropSession(const FText& Reason);

	void FailDownload(int32 RevisionId, const FStageGdtfShareResultInfo& Result);
	FStageGdtfShareDownload* FindDownload(int32 RevisionId);
	void CancelRequests(bool bDownloadsOnly);

	EStageGdtfShareState State = EStageGdtfShareState::SignedOut;
	FString UserName;
	/** The session; a secret, never logged. */
	FString SessionCookie;
	/** Increments with every sign-in and sign-out: answers of an older session are dropped. */
	uint32 SessionSerial = 0;

	TArray<FStageGdtfShareEntry> Entries;
	FDateTime ListFetchedUtc;
	bool bListLoading = false;
	uint32 ListSerial = 0;

	TArray<FStageGdtfShareDownload> Downloads;
	/** Running downloads: revision id to the serial of the request awaited. */
	TMap<int32, uint32> RunningDownloads;
	uint32 DownloadSerial = 0;

	/** Requests on the network, to cancel; with the revision they download (0 for sign-in and list). */
	struct FInFlight
	{
		TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Request;
		int32 RevisionId = 0;
	};
	TArray<FInFlight> InFlight;

#if WITH_DEV_AUTOMATION_TESTS
	FTestTransport TestTransport;
#endif
};
