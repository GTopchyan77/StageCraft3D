// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** One server-sent event: its "event:" name (empty when none was given) and its "data:" lines joined with newlines. */
struct FStageSseEvent
{
	FString Event;
	FString Data;
};

/**
 * Splits a text/event-stream body into events as its bytes arrive (Docs/ADR/0030 §8, streaming). The Messages API streams one
 * JSON object per event; the HTTP module hands the body over in chunks of any size, cut anywhere, even inside a UTF-8 sequence.
 *
 * Follows the SSE rules the API uses: lines end in LF, CRLF or CR; a blank line ends an event; "event:" names it; "data:" lines
 * accumulate; a line starting with ":" is a comment (keep-alive); a single space after the colon is dropped; other fields are
 * ignored. An event with no data is not reported.
 *
 * Not thread-safe: one parser per response, fed from one thread at a time (the HTTP thread's stream delegate). Pure and covered by
 * the StageCraft.Assistant.Sse tests.
 */
class FStageSseParser
{
public:
	/** Longest line kept (bytes); a longer line is cut there and the stream is marked malformed. */
	static constexpr int32 MaxLineBytes = 1 << 20;

	/** Feeds Count bytes and appends every event they complete to OutEvents. */
	void Feed(const uint8* Bytes, int32 Count, TArray<FStageSseEvent>& OutEvents);

	/** Ends the stream: an event still open without its blank line is reported (some servers close without one). */
	void Finish(TArray<FStageSseEvent>& OutEvents);

	/** True once a line exceeded MaxLineBytes. */
	bool IsMalformed() const { return bMalformed; }

private:
	void HandleLine(TArray<FStageSseEvent>& OutEvents);
	void Dispatch(TArray<FStageSseEvent>& OutEvents);

	/** The current line's bytes, decoded as UTF-8 only once it is complete, so a sequence split across chunks stays whole. */
	TArray<uint8> Line;
	FString EventName;
	FString Data;
	bool bHasData = false;
	/** The previous byte was CR: an LF right after it ends nothing more. */
	bool bAfterCR = false;
	bool bMalformed = false;
};
