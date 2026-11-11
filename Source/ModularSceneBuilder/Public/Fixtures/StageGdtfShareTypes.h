// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "StageGdtfShareTypes.generated.h"

/** Outcome of a GDTF Share request (Docs/ADR/0033 §18). One closed enum, so views branch without parsing text. */
UENUM(BlueprintType)
enum class EStageGdtfShareResult : uint8
{
	Success,
	/** The GameMode does not allow online fixture sources, or the subsystem is not available. */
	NotAllowed,
	/** Sign in to GDTF Share first. */
	SignInRequired,
	/** The same request is already running, or too many downloads are running. */
	Busy,
	/** Empty user name or password, or an unknown revision. */
	InvalidRequest,
	/** GDTF Share refused the user name or password. */
	BadCredentials,
	/** GDTF Share ended the session (two hours without a request); sign in again. */
	SessionExpired,
	/** Nothing came back: no network, a timeout, or the request was cancelled by the network layer. */
	NetworkError,
	/** GDTF Share answered with an error status. */
	ServerError,
	/** The answer was not what the API documents (not JSON, wrong fields, not a GDTF file). */
	BadResponse,
	/** GDTF Share no longer has that file. */
	NotFound,
	/** The list or file is larger than StageCraft accepts. */
	TooLarge,
	/** The downloaded file could not be handed to the fixture library (the library refused it; the message says why). */
	ImportRefused,
	/** The user cancelled the download, or signed out while it ran. */
	Cancelled,
};

/** Result of a GDTF Share request. Message is user-facing; Code is for logic. */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageGdtfShareResultInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|GDTF Share")
	EStageGdtfShareResult Code = EStageGdtfShareResult::Success;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|GDTF Share")
	FText Message;

	bool IsSuccess() const { return Code == EStageGdtfShareResult::Success; }

	static FStageGdtfShareResultInfo Ok(const FText& InMessage = FText::GetEmpty())
	{
		FStageGdtfShareResultInfo Result;
		Result.Message = InMessage;
		return Result;
	}

	static FStageGdtfShareResultInfo Fail(EStageGdtfShareResult InCode, const FText& InMessage)
	{
		FStageGdtfShareResultInfo Result;
		Result.Code = InCode;
		Result.Message = InMessage;
		return Result;
	}
};

/** Where this client stands with GDTF Share. */
UENUM(BlueprintType)
enum class EStageGdtfShareState : uint8
{
	SignedOut,
	SigningIn,
	SignedIn,
};

/** Who uploaded a GDTF Share file. The API terms require every listing to show it, so users can tell manufacturer files from user files. */
UENUM(BlueprintType)
enum class EStageGdtfShareUploader : uint8
{
	Manufacturer,
	User,
	/** The list named another uploader kind; shown as such, never guessed. */
	Unknown,
};

/** One DMX mode of a GDTF Share file, as the list describes it (the file itself decides on import). */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageGdtfShareMode
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|GDTF Share")
	FString Name;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|GDTF Share")
	int32 Footprint = 0;
};

/**
 * One file revision in the GDTF Share list (getList.php). Untrusted remote data: every text is cleaned of control characters and
 * at most StageGdtfShare::MaxTextLength long, numbers are range-checked, and the file is validated again by the fixture library
 * when it is downloaded (Docs/ADR/0033 §8).
 */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageGdtfShareEntry
{
	GENERATED_BODY()

	/** The revision id (rid) the download asks for. Positive. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|GDTF Share")
	int32 RevisionId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|GDTF Share")
	FString Manufacturer;

	/** The fixture type name. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|GDTF Share")
	FString Fixture;

	/** The revision's own label, e.g. "Firmware 2.1". May be empty. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|GDTF Share")
	FString Revision;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|GDTF Share")
	EStageGdtfShareUploader Uploader = EStageGdtfShareUploader::Unknown;

	/** The GDTF Share user name of the original uploader. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|GDTF Share")
	FString Creator;

	/** The file's GDTF version, e.g. "1.2". */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|GDTF Share")
	FString GdtfVersion;

	/** 0 to 5. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|GDTF Share")
	float Rating = 0.f;

	/** The GDTF FixtureTypeID, the same across revisions: the library type a download adds or replaces. Invalid when the list gave none. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|GDTF Share")
	FGuid FixtureTypeId;

	/** The size the list states; 0 when unknown. The download is capped whatever this says. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|GDTF Share")
	int64 FileSizeBytes = 0;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|GDTF Share")
	FDateTime LastModifiedUtc;

	/** At most StageGdtfShare::MaxModes, in list order. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|GDTF Share")
	TArray<FStageGdtfShareMode> Modes;
};

/** Where one download stands in the download manager. */
UENUM(BlueprintType)
enum class EStageGdtfShareDownloadState : uint8
{
	Downloading,
	/** The file arrived and is with the fixture library, waiting for the user's review (ADD, REPLACE or CANCEL). */
	Delivered,
	Failed,
};

/** One download of the download manager (UStageGdtfShareSubsystem::GetDownloads). */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageGdtfShareDownload
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|GDTF Share")
	int32 RevisionId = 0;

	/** "Manufacturer Fixture (Revision)". */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|GDTF Share")
	FString Label;

	/** The name the file is reviewed and kept under (StageGdtfShare::MakeFileName). */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|GDTF Share")
	FString FileName;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|GDTF Share")
	EStageGdtfShareDownloadState State = EStageGdtfShareDownloadState::Downloading;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|GDTF Share")
	int64 ReceivedBytes = 0;

	/** 0 while the size is unknown. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|GDTF Share")
	int64 TotalBytes = 0;

	/** Why it failed, or what happens next. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|GDTF Share")
	FText Message;
};

/** One HTTP request the GDTF Share subsystem sends (also what a test transport receives). Body is the exact bytes sent. */
struct FStageGdtfShareHttpRequest
{
	FString Verb;
	FString Url;
	FString ContentType;
	TArray<uint8> Body;
	/** The "Cookie" header: the session, or empty before signing in. A secret: never logged. */
	FString Cookie;
	/** Larger answers are cut off and reported with bTooLarge. */
	int64 MaxResponseBytes = 0;
};

/** One HTTP answer. bConnected is false when nothing came back (no network, timeout, cancelled). */
struct FStageGdtfShareHttpResponse
{
	bool bConnected = false;
	/** The answer grew past the request's MaxResponseBytes and was dropped. */
	bool bTooLarge = false;
	int32 Status = 0;
	TArray<uint8> Body;
	/** Every Set-Cookie header value, in order. */
	TArray<FString> SetCookies;
};

DECLARE_DELEGATE_OneParam(FOnStageGdtfShareResponse, const FStageGdtfShareHttpResponse& /*Response*/);
DECLARE_DELEGATE_TwoParams(FOnStageGdtfShareProgress, int64 /*ReceivedBytes*/, int64 /*TotalBytes*/);
