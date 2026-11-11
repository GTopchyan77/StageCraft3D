// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Remote/StageWebRemoteTypes.h"

/**
 * The pure half of the Web Remote (Docs/ADR/0036): HTTP request parsing and routing, the WebSocket handshake and frames, the JSON
 * messages, PIN pairing and settings sanitizing. Everything here reads untrusted bytes from the network, so every reader is capped
 * and fails closed. No sockets, no UObjects, no engine state; any thread (the server's worker uses it, the tests call it directly).
 */
namespace StageWebRemote
{
	inline constexpr int32 DefaultPort = 8080;
	inline constexpr int32 MinPort = 1024;
	inline constexpr int32 MaxPort = 65535;

	/** Docs/ADR/0036 §3 limits. */
	inline constexpr int32 MaxHeadBytes = 8 * 1024;
	inline constexpr int32 MaxHeaders = 64;
	inline constexpr int32 MaxPathLength = 256;
	inline constexpr int32 MaxBodyBytes = 4 * 1024;
	inline constexpr double RequestTimeoutSeconds = 10.0;
	inline constexpr int32 MaxConnections = 16;
	inline constexpr int32 MaxWebSockets = 8;
	inline constexpr int32 MaxPendingSendBytes = 256 * 1024;

	/** Docs/ADR/0036 §4 limits. */
	inline constexpr int32 MaxFramePayload = 4 * 1024;
	inline constexpr double HelloTimeoutSeconds = 5.0;
	inline constexpr double PingIntervalSeconds = 20.0;
	inline constexpr double IdleTimeoutSeconds = 60.0;

	/** Docs/ADR/0036 §5 limits. */
	inline constexpr int32 MaxCues = 200;
	inline constexpr int32 MaxLabelLength = 64;

	/** Docs/ADR/0036 §6 pairing. */
	inline constexpr int32 PinDigits = 6;
	inline constexpr int32 MaxPairedDevices = 8;
	inline constexpr int32 MaxWrongPins = 5;
	inline constexpr int32 WrongPinsBeforeNewPin = 20;
	inline constexpr int32 MaxTrackedClients = 256;
	/** Unauthenticated connections (HTTP requests and WebSockets before their hello) one address may hold at once. */
	inline constexpr int32 MaxPendingPerClient = 4;
	/** Events waiting for the game thread; more are dropped (a flooding page). */
	inline constexpr int32 MaxQueuedEvents = 4096;
	/** A connection that is closing is closed hard after this long (a peer that stops reading). */
	inline constexpr double ClosingTimeoutSeconds = 5.0;
	inline constexpr double LockoutSeconds = 30.0;
	inline constexpr int32 MaxDeviceNameLength = 48;

	/** WebSocket close codes the server sends (RFC 6455 §7.4.1). */
	namespace CloseCode
	{
		inline constexpr uint16 Normal = 1000;
		inline constexpr uint16 GoingAway = 1001;
		inline constexpr uint16 ProtocolError = 1002;
		inline constexpr uint16 Unsupported = 1003;
		inline constexpr uint16 InvalidData = 1007;
		inline constexpr uint16 PolicyViolation = 1008;
		inline constexpr uint16 TooBig = 1009;
	}

	enum class EMethod : uint8
	{
		Get,
		Post,
		/** Any other token: answered 405 on a known path, 404 elsewhere. */
		Other,
	};

	/** One parsed HTTP/1.1 request. Header names are lower case; a repeated header's values are joined with ", ". */
	struct FHttpRequest
	{
		EMethod Method = EMethod::Other;
		/** The request target without its query string, e.g. "/api/pair". */
		FString Path;
		TMap<FString, FString> Headers;
		TArray<uint8> Body;

		/** The header's value, or empty. Name must be lower case. */
		FString GetHeader(const TCHAR* Name) const;
	};

	enum class EReadResult : uint8
	{
		NeedMore,
		Complete,
		/** The request broke a rule; GetErrorStatus says which HTTP answer it gets. Nothing more is read. */
		Error,
	};

	/**
	 * Reads one request from a connection's bytes as they arrive: a CRLF head of at most MaxHeadBytes, then exactly Content-Length body
	 * bytes (at most MaxBodyBytes; Transfer-Encoding is refused). Bytes after the request are kept for TakeRemainder.
	 */
	class FHttpRequestReader
	{
	public:
		EReadResult Append(TConstArrayView<uint8> Bytes);

		const FHttpRequest& GetRequest() const { return Request; }

		/** 400, 405, 411, 413, 431, 501 or 505 after an Error. */
		int32 GetErrorStatus() const { return ErrorStatus; }
		const FString& GetError() const { return Error; }

		/** The bytes received after the request (a client's first WebSocket frame may arrive with the handshake). */
		TArray<uint8> TakeRemainder();

	private:
		EReadResult Fail(int32 Status, const TCHAR* Message);
		EReadResult ParseHead(int32 HeadLength);

		TArray<uint8> Buffer;
		FHttpRequest Request;
		int32 BodyLength = 0;
		bool bHeadDone = false;
		bool bComplete = false;
		int32 ErrorStatus = 0;
		FString Error;
	};

	/** What a request asks for (Docs/ADR/0036 §3). */
	enum class ERoute : uint8
	{
		Page,
		Pair,
		Command,
		State,
		WebSocket,
		NotFound,
		MethodNotAllowed,
	};

	ERoute Route(const FHttpRequest& Request);

	/** An IPv4 literal, a bracketed IPv6 literal or "localhost", each with an optional port. Anything else is a name that could rebind. */
	bool IsAllowedHost(FStringView Host);

	/** True without an Origin, or when Origin is "http://" + Host (case-insensitive). */
	bool IsSameOrigin(FStringView Origin, FStringView Host);

	/** The token of an "Authorization: Bearer <token>" value, or empty. */
	FString ParseBearer(FStringView Authorization);

	/** "Not Found" for 404 and so on; "Error" for a status this server never sends. */
	const TCHAR* StatusText(int32 Status);

	/** A complete HTTP/1.1 response with Connection: close, no-store caching and nosniff. ExtraHeaders are "Name: value" lines. */
	TArray<uint8> MakeResponse(int32 Status, const TCHAR* ContentType, TConstArrayView<uint8> Body, TConstArrayView<FString> ExtraHeaders = {});
	TArray<uint8> MakeJsonResponse(int32 Status, const FString& Json);
	/** {"error":Message}. */
	TArray<uint8> MakeErrorResponse(int32 Status, const FString& Message, TConstArrayView<FString> ExtraHeaders = {});
	/** The remote page with its Content-Security-Policy and X-Frame-Options (Docs/ADR/0036 §3). */
	TArray<uint8> MakePageResponse(TConstArrayView<uint8> Html);

	enum class EUpgradeResult : uint8
	{
		Ok,
		/** Not a valid WebSocket handshake: 400. */
		BadRequest,
		/** Another protocol version: 426 with Sec-WebSocket-Version: 13. */
		WrongVersion,
	};

	/** Checks a GET /ws request's handshake headers (Docs/ADR/0036 §4); on Ok, OutAcceptKey is the Sec-WebSocket-Accept value. */
	EUpgradeResult CheckUpgrade(const FHttpRequest& Request, FString& OutAcceptKey);

	/** base64(SHA-1(ClientKey + the RFC 6455 GUID)). */
	FString MakeAcceptKey(const FString& ClientKey);

	TArray<uint8> MakeUpgradeResponse(const FString& AcceptKey);

	enum class EOpcode : uint8
	{
		Continuation = 0x0,
		Text = 0x1,
		Binary = 0x2,
		Close = 0x8,
		Ping = 0x9,
		Pong = 0xA,
	};

	struct FWebSocketFrame
	{
		EOpcode Opcode = EOpcode::Text;
		/** Unmasked. */
		TArray<uint8> Payload;
	};

	enum class EFrameResult : uint8
	{
		NeedMore,
		Frame,
		/** The frame broke the protocol or a limit; OutCloseCode says which close the server sends. */
		Error,
	};

	/**
	 * Decodes one client frame from the front of Buffer. Client frames must be masked and final, use no reserved bits, and carry at most
	 * MaxFramePayload bytes (control frames at most 125). Text, close, ping and pong are accepted; binary and continuation are Error
	 * with CloseCode::Unsupported. On Frame, OutConsumed is the frame's length in Buffer.
	 */
	EFrameResult DecodeClientFrame(TConstArrayView<uint8> Buffer, FWebSocketFrame& OutFrame, int32& OutConsumed, uint16& OutCloseCode);

	/** One final, unmasked server frame. */
	TArray<uint8> EncodeFrame(EOpcode Opcode, TConstArrayView<uint8> Payload);
	TArray<uint8> EncodeText(const FString& Text);
	/** A close frame with Code and Reason (cut so the payload stays within 125 bytes). */
	TArray<uint8> EncodeClose(uint16 Code, const FString& Reason);

	/** Well-formed UTF-8: no overlong forms, surrogates or code points above U+10FFFF. */
	bool IsValidUtf8(TConstArrayView<uint8> Bytes);

	/** Bytes as text; invalid UTF-8 gives an empty string. */
	FString Utf8ToString(TConstArrayView<uint8> Bytes);
	TArray<uint8> StringToUtf8(const FString& Text);

	/** A message a page sends on its WebSocket (Docs/ADR/0036 §4). */
	struct FClientMessage
	{
		enum class EType : uint8
		{
			Hello,
			Command,
		};
		EType Type = EType::Hello;
		FString Token;
		FString Path;
		TOptional<float> Value;
	};

	/** False with OutError for invalid JSON, an unknown type, a missing field or a value that is not a finite number or a boolean. */
	bool ParseClientMessage(const FString& Json, FClientMessage& OutMessage, FString& OutError);

	/** POST /api/pair's body: {"pin":"…","name":"…"}. The name is optional; it is trimmed, cut and stripped of control characters. */
	bool ParsePairBody(TConstArrayView<uint8> Body, FString& OutPin, FString& OutName, FString& OutError);

	/** POST /api/command's body: {"path":"…","value":…}; value optional. */
	bool ParseCommandBody(TConstArrayView<uint8> Body, FString& OutPath, TOptional<float>& OutValue, FString& OutError);

	/** The show as a page sees it (Docs/ADR/0036 §5). Built on the game thread, sent by the worker. */
	struct FShowState
	{
		struct FPlayback
		{
			FString Name;
			float Level = 0.f;
			bool bFlash = false;
			bool bActive = false;
			bool bEmpty = true;
		};

		/** Negative when no cue has run. */
		float ActiveCue = -1.f;
		FString ActiveLabel;
		TArray<FPlayback> Playbacks;
		float GrandMaster = 1.f;
		bool bBlackout = false;
		float AudioMaster = 1.f;
	};

	struct FCueEntry
	{
		float Number = 0.f;
		FString Label;
	};

	FString MakeStateMessage(const FShowState& State);
	/** At most MaxCues entries, labels cut to MaxLabelLength. */
	FString MakeCuesMessage(TConstArrayView<FCueEntry> Cues);
	FString MakeWelcomeMessage();
	FString MakeByeMessage(const FString& Reason);
	FString MakeRefusedMessage(const FString& Message);
	/** {"token":Token}: POST /api/pair's answer. */
	FString MakeTokenJson(const FString& Token);
	/** {"ok":true}: POST /api/command's answer. */
	FString MakeOkJson();

	/**
	 * Repairs settings from an untrusted source (the ini): an unknown scope is Off, a port outside MinPort–MaxPort the default. Returns
	 * true if anything changed.
	 */
	bool Sanitize(FStageWebRemoteSettings& Settings);

	/** PinDigits decimal digits from the platform's GUID generator (cryptographically random on Windows). */
	FString MakePin();

	/** 64 hex characters from two GUIDs. */
	FString MakeToken();

	/** Equal length and content, compared in constant time for that length. */
	bool IsSecretMatch(const FString& Expected, const FString& Presented);

	/**
	 * PIN pairing (Docs/ADR/0036 §6). Not thread-safe: the server keeps it under its lock. Time comes from the caller, so tests control it.
	 * The PIN and tokens never leave this class except through GetPin (shown to the user) and Pair's OutToken (sent to the device).
	 */
	class FPairing
	{
	public:
		enum class EResult : uint8
		{
			Paired,
			WrongPin,
			/** Too many wrong PINs; OutRetryAfterSeconds says how long pairing waits. */
			Locked,
		};

		/** MakePinFunc and MakeTokenFunc default to MakePin and MakeToken; tests pass predictable ones. Starts with a fresh PIN. */
		explicit FPairing(TFunction<FString()> MakePinFunc = nullptr, TFunction<FString()> MakeTokenFunc = nullptr);

		const FString& GetPin() const { return Pin; }

		/** A new PIN; the old one stops working. Paired devices stay paired. */
		void RenewPin();

		/**
		 * Pairs a device that knows the PIN. Client is the caller's address: its MaxWrongPins-th wrong PIN in a row locks that client
		 * (and only that one) for LockoutSeconds, so one host on the LAN cannot keep the real phone from pairing; attempts while locked
		 * are refused without counting. Every WrongPinsBeforeNewPin wrong PINs from all clients together replace the PIN, which bounds
		 * guessing from many addresses. A successful pairing resets the client's count. The (MaxPairedDevices + 1)-th device replaces
		 * the oldest; bOutEvicted says so (its open pages must be closed).
		 */
		EResult Pair(const FString& PresentedPin, const FString& DeviceName, const FString& Client, double Now, FString& OutToken,
			double& OutRetryAfterSeconds, bool& bOutEvicted);

		bool IsPaired(const FString& Token) const;

		/** Forgets every device and replaces the PIN. */
		void UnpairAll();

		int32 GetPairedCount() const { return Devices.Num(); }
		TArray<FString> GetDeviceNames() const;

		/** The latest time until which some client is locked out (0 before any lockout): the panel says a device was locked. */
		double GetLockedUntil() const { return LatestLockedUntil; }

	private:
		struct FDevice
		{
			FString Token;
			FString Name;
		};

		struct FClientAttempts
		{
			int32 WrongPins = 0;
			double LockedUntil = 0.0;
			double LastSeen = 0.0;
		};

		FClientAttempts& FindClient(const FString& Client, double Now);

		TFunction<FString()> MakePinFunc;
		TFunction<FString()> MakeTokenFunc;
		FString Pin;
		TArray<FDevice> Devices;
		TMap<FString, FClientAttempts> Clients;
		int32 WrongPinsSinceNewPin = 0;
		double LatestLockedUntil = 0.0;
	};
}
