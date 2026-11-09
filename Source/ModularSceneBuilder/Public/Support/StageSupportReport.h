// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** What a support request is about (wire names in StageSupportReport::GetCategoryName). */
enum class EStageSupportCategory : uint8
{
	Bug,
	Crash,
	Performance,
	Marketplace,
	Purchase,
	Account,
	Feature,
	Other,
	Count
};

/** One support request or bug report, as the user filled it in plus what the app adds. Plain data; game thread. */
struct FStageSupportReport
{
	/** Generated once per report; the server's idempotency key, so a resent report is one ticket. */
	FGuid ReportId;
	FDateTime CreatedUtc;

	EStageSupportCategory Category = EStageSupportCategory::Bug;
	FString Title;
	FString Description;
	FString Steps;
	FString Expected;
	FString Actual;
	/** Optional: where the user wants an answer when they are not signed in. */
	FString ContactEmail;

	// --- Added by the app (shown to the user before sending) ---
	FString AppVersion;
	FString Platform;
	FString Gpu;
	FString SceneSummary;

	/** The last StageSupportReport::MaxLogBytes of the log, redacted; sent only when bIncludeLog. */
	bool bIncludeLog = false;
	FString LogTail;

	/** A PNG of the app window; sent only when bIncludeScreenshot. */
	bool bIncludeScreenshot = false;
	TArray<uint8> ScreenshotPng;
};

/**
 * Support requests and bug reports (Docs/ADR/0032 §7): validation, redaction of secrets and personal data before anything leaves the
 * machine, and the formats a report is sent or saved in. Pure: no HTTP, no files, no world; covered by StageCraft.Support.* tests.
 */
namespace StageSupportReport
{
	constexpr int32 MinTitleLength = 3;
	constexpr int32 MaxTitleLength = 120;
	constexpr int32 MaxTextLength = 8000;
	constexpr int32 MaxEmailLength = 254;
	constexpr int32 MaxLogBytes = 256 * 1024;
	constexpr int32 MaxScreenshotBytes = 4 * 1024 * 1024;

	MODULARSCENEBUILDER_API const TCHAR* GetCategoryName(EStageSupportCategory Category);
	MODULARSCENEBUILDER_API FText GetCategoryDisplayName(EStageSupportCategory Category);

	/** A plausible e-mail address: one @, a dotted domain, no spaces, at most MaxEmailLength. */
	MODULARSCENEBUILDER_API bool IsPlausibleEmail(const FString& Email);

	/** True when Bytes start with the PNG signature. */
	MODULARSCENEBUILDER_API bool IsPng(TConstArrayView<uint8> Bytes);

	/** Why the report cannot be sent yet, user-facing; empty when it can. */
	MODULARSCENEBUILDER_API FText Validate(const FStageSupportReport& Report);

	/**
	 * Text with secrets and personal data replaced, safe to send:
	 *  - API keys and webhook secrets (sk-ant-…, sk_live_…, sk_test_…, rk_live_…, rk_test_…, whsec_…) and Bearer / x-api-key values;
	 *  - each of KnownSecrets (for example the cloud session token) wherever it appears;
	 *  - e-mail addresses;
	 *  - the user's name in user-profile paths (C:\Users\<name>\, /Users/<name>/, /home/<name>/).
	 */
	MODULARSCENEBUILDER_API FString Redact(const FString& Text, const FString& UserName, TConstArrayView<FString> KnownSecrets = {});

	/** Redacts every text field of Report (the contact e-mail is the user's own choice and is kept). */
	MODULARSCENEBUILDER_API void RedactReport(FStageSupportReport& Report, const FString& UserName, TConstArrayView<FString> KnownSecrets = {});

	/** The end of a log: at most MaxBytes of UTF-8, starting at a line boundary. */
	MODULARSCENEBUILDER_API FString TailLog(const FString& LogText, int32 MaxBytes = MaxLogBytes);

	/** The JSON body of POST /v1/support/tickets. The screenshot is base64; the log and screenshot only when included. */
	MODULARSCENEBUILDER_API FString ToJson(const FStageSupportReport& Report);

	/** The report as plain text (report.txt of a report saved on disk). */
	MODULARSCENEBUILDER_API FString ToText(const FStageSupportReport& Report);
}
