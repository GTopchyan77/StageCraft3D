// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Fixtures/StageGdtfShareTypes.h"

/**
 * The GDTF Share public API v1 (https://gdtf-share.com/apis/public/, Docs/ADR/0033 §18): request bodies, answer parsing, the session
 * cookie, search and file names.
 *
 * Every answer is untrusted remote input: sizes are capped before parsing, unknown or malformed entries are skipped and counted,
 * texts are cleaned and bounded, and nothing is clamped silently into a different meaning.
 *
 * Pure logic: no world, no UObjects, no network. Safe on any thread; covered by StageCraft.Fixtures.GdtfShare.* tests.
 */
namespace StageGdtfShare
{
	/** The public API root. A sync server (recommended by the API terms) can replace it in config. */
	extern const TCHAR* const DefaultBaseUrl;
	extern const TCHAR* const LoginEndpoint;
	extern const TCHAR* const ListEndpoint;
	extern const TCHAR* const DownloadEndpoint;

	/** The attribution the API terms require next to GDTF Share content. */
	extern const TCHAR* const AttributionText;

	/** Answer caps: the login answer, the whole list, and one GDTF file (the fixture library's own limit). */
	constexpr int64 MaxLoginResponseBytes = 64 * 1024;
	constexpr int64 MaxListResponseBytes = 64ll * 1024 * 1024;
	constexpr int64 MaxDownloadBytes = 50ll * 1024 * 1024;

	/** List caps: entries kept, characters per text, modes per entry. Entries past the cap are skipped and counted. */
	constexpr int32 MaxEntries = 200000;
	constexpr int32 MaxTextLength = 256;
	constexpr int32 MaxModes = 64;

	/** The API terms ask clients to refresh the list about every 120 minutes, not more often. */
	constexpr double ListMaxAgeSeconds = 120.0 * 60.0;

	/** True for an https:// address ending in "/": the only kind of base address the subsystem sends a password to. */
	bool IsSafeBaseUrl(const FString& Url);

	/** {"user":User,"password":Password} as UTF-8 JSON, with every character escaped by the JSON writer. */
	TArray<uint8> MakeLoginBody(const FString& User, const FString& Password);

	/**
	 * Reads {"result":bool,...} answers (login, errors). False when Body is not such an object. OutError is the answer's "error" or
	 * "notice" text, cleaned and bounded, or empty.
	 */
	bool ParseResultFlag(TConstArrayView<uint8> Body, bool& bOutResult, FString& OutError);

	/**
	 * Reads a getList.php answer: {"result":true,"list":[...]}. False when the answer is not that shape. Entries without a positive
	 * integer rid, a manufacturer or a fixture name are skipped and counted in OutSkipped, as are repeated rids and entries past
	 * MaxEntries. Dates may be numbers or numeric strings (Unix seconds), as the API documentation shows both.
	 */
	bool ParseList(TConstArrayView<uint8> Body, TArray<FStageGdtfShareEntry>& OutEntries, int32& OutSkipped);

	/**
	 * The Cookie header for the next requests from login Set-Cookie values: each cookie's leading name=value, joined with "; ". A
	 * value may hold several cookies joined with "," (HTTP layers merge repeated headers; a cookie value never contains a comma, and
	 * the parts that are only an Expires date are skipped). Cookies without a name, with characters a cookie may not hold, or set
	 * to "deleted" are dropped; a repeated name keeps the last value. Empty when nothing usable came back.
	 */
	FString JoinCookies(TConstArrayView<FString> SetCookieValues);

	/**
	 * Indices of the entries matching Text, at most MaxResults: every whitespace-separated word must appear, ignoring case, in the
	 * manufacturer, fixture name, revision or creator. Uploader, when set, keeps only that uploader. Sorted by manufacturer, then
	 * fixture name (ignoring case), then the newest revision first.
	 */
	TArray<int32> Search(TConstArrayView<FStageGdtfShareEntry> Entries, const FString& Text, TOptional<EStageGdtfShareUploader> Uploader, int32 MaxResults);

	/**
	 * The library file name of a download, after GDTF Share's own naming: "Manufacturer@Fixture@Revision.gdtf" (the revision part only
	 * when there is one). Characters other than letters, digits, space, '-', '_', '.', '(' and ')' become '_'; each part is trimmed and
	 * at most 60 characters. "GDTF_Share_<rid>.gdtf" when nothing is left.
	 */
	FString MakeFileName(const FStageGdtfShareEntry& Entry);

	/** "Manufacturer Fixture (Revision)" for lists and messages. */
	FString MakeLabel(const FStageGdtfShareEntry& Entry);

	/** True when Bytes start with a ZIP local file header, as every .gdtf does. */
	bool IsZipFile(TConstArrayView<uint8> Bytes);
}
