// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Bridge/StageBridgeTypes.h"

/**
 * The DCC bridge wire protocol (Docs/ADR/0022-dcc-bridge.md §5): newline-delimited JSON over a TCP connection to 127.0.0.1.
 * Tools/DCCBridge/common/stagecraft_bridge_client.py is the other end; the two must change together.
 *
 *   add-on -> StageCraft   {"type":"hello","protocol":1,"token":"<hex>","app":"blender","version":"4.3.0"}
 *   StageCraft -> add-on   {"type":"welcome","protocol":1,"client":3}
 *   StageCraft -> add-on   {"type":"asset","id":"<hex>","format":"glb","name":"My Show","path":"<abs>","sha1":"<hex>","bytes":123}
 *   add-on -> StageCraft   {"type":"ack","id":"<hex>","ok":true}   or   {"type":"ack","id":"<hex>","ok":false,"error":"..."}
 *   StageCraft -> add-on   {"type":"bye","reason":"..."}            (then the connection closes)
 *
 * Everything a client sends is untrusted: lines are capped, unknown or malformed messages are refused (the server then closes the
 * connection), free text is capped and stripped to printable characters. Files never travel over the socket; the add-on reads
 * the outbox file named by "path" after checking it lies inside the outbox and matches "sha1".
 *
 * Pure functions and value types: no sockets, any thread.
 */
namespace StageBridgeProtocol
{
	constexpr int32 Version = 1;
	/** Unassigned in the IANA registry. Overridable with -StageBridgePort=<n> (ADR 0022 §5). */
	constexpr uint16 DefaultPort = 47830;
	/** The longest line either side may send, newline included. A longer one closes the connection. */
	constexpr int32 MaxLineBytes = 64 * 1024;
	/** Connections at once, authenticated or not. One more is told "full" and closed. */
	constexpr int32 MaxClients = 4;
	/** A connection that has not said a valid hello by then is closed. */
	constexpr double HelloTimeoutSeconds = 5.0;
	constexpr int32 MaxAppVersionLength = 32;
	constexpr int32 MaxErrorLength = 256;
	constexpr int32 MaxIdLength = 64;

	enum class EClientMessage : uint8
	{
		Hello,
		Ack,
	};

	struct FHello
	{
		int32 Protocol = 0;
		FString Token;
		EStageBridgeApp App = EStageBridgeApp::Unknown;
		FString AppVersion;
	};

	struct FAck
	{
		FString Id;
		bool bOk = false;
		/** The add-on's reason when bOk is false; capped, printable characters only. */
		FString Error;
	};

	struct FClientMessage
	{
		EClientMessage Type = EClientMessage::Hello;
		FHello Hello;
		FAck Ack;
	};

	struct FAsset
	{
		FString Id;
		/** "glb" in protocol 1. */
		FString Format;
		FString Name;
		FString Path;
		FString Sha1;
		int64 Bytes = 0;
	};

	/**
	 * Splits a byte stream into UTF-8 lines. Append returns false (and keeps nothing) when a line, or the unterminated tail, would
	 * exceed MaxLineBytes: the sender broke the protocol and the connection should close.
	 */
	class FLineReader
	{
	public:
		bool Append(TConstArrayView<uint8> Bytes, TArray<FString>& OutLines);

	private:
		TArray<uint8> Pending;
	};

	/** One client line. False with OutError when it is not valid JSON, not an object, of an unknown type or missing fields. */
	bool ParseClientMessage(const FString& Line, FClientMessage& OutMessage, FString& OutError);

	/** Compares in time independent of where the strings differ, so the token cannot be guessed one character at a time. */
	bool IsTokenMatch(const FString& Expected, const FString& Presented);

	/** 64 hex characters from two GUIDs (on Windows CoCreateGuid, cryptographically random). */
	FString MakeToken();

	/** A fresh asset id: 32 hex characters. */
	FString MakeAssetId();

	FString MakeWelcome(int32 ClientId);
	FString MakeAsset(const FAsset& Asset);
	FString MakeBye(const FString& Reason);

	/** "blender", "maya", "max", "cinema4d"; empty for Unknown. */
	const TCHAR* GetAppKey(EStageBridgeApp App);
	FText GetAppDisplayName(EStageBridgeApp App);
	EStageBridgeApp ParseApp(const FString& Key);

	/** Whether the add-on for App imports glTF binary (protocol 1 sends nothing else). Maya waits for USD (ADR 0022 §1). */
	bool ImportsGlb(EStageBridgeApp App);

	/** Keeps printable characters only (no control characters, no line breaks) and at most MaxLength of them. */
	FString SanitizeText(const FString& Text, int32 MaxLength);
}
