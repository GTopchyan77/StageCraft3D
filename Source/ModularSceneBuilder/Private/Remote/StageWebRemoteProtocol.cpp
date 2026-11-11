// Copyright Epic Games, Inc. All Rights Reserved.

#include "Remote/StageWebRemoteProtocol.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Base64.h"
#include "Misc/Guid.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace StageWebRemoteImpl
{
	const TCHAR* WebSocketGuid = TEXT("258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
	constexpr int32 MaxTokenLength = 128;
	constexpr int32 MaxPinLength = 16;
	constexpr int32 MaxControlPayload = 125;
	constexpr int32 MaxCloseReasonBytes = 123;

	int32 FindHeadEnd(TConstArrayView<uint8> Bytes)
	{
		for (int32 Index = 0; Index + 3 < Bytes.Num(); ++Index)
		{
			if (Bytes[Index] == '\r' && Bytes[Index + 1] == '\n' && Bytes[Index + 2] == '\r' && Bytes[Index + 3] == '\n')
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	bool IsAllDigits(FStringView Text, int32 MaxDigits)
	{
		if (Text.IsEmpty() || Text.Len() > MaxDigits)
		{
			return false;
		}
		for (const TCHAR Char : Text)
		{
			if (!FChar::IsDigit(Char))
			{
				return false;
			}
		}
		return true;
	}

	bool HasToken(const FString& HeaderValue, const TCHAR* Token)
	{
		TArray<FString> Parts;
		HeaderValue.ParseIntoArray(Parts, TEXT(","), true);
		for (const FString& Part : Parts)
		{
			if (Part.TrimStartAndEnd().Equals(Token, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	bool IsValidPort(FStringView Port)
	{
		if (!IsAllDigits(Port, 5))
		{
			return false;
		}
		const int32 Value = FCString::Atoi(*FString(Port));
		return Value >= 1 && Value <= 65535;
	}

	bool IsIPv4(FStringView Host)
	{
		int32 Parts = 0;
		int32 Start = 0;
		for (int32 Index = 0; Index <= Host.Len(); ++Index)
		{
			if (Index == Host.Len() || Host[Index] == TCHAR('.'))
			{
				const FStringView Part = Host.Mid(Start, Index - Start);
				if (!IsAllDigits(Part, 3) || FCString::Atoi(*FString(Part)) > 255)
				{
					return false;
				}
				++Parts;
				Start = Index + 1;
			}
		}
		return Parts == 4;
	}

	bool IsIPv6Literal(FStringView Inner)
	{
		if (Inner.IsEmpty() || Inner.Len() > 45)
		{
			return false;
		}
		bool bHasColon = false;
		for (const TCHAR Char : Inner)
		{
			if (Char == TCHAR(':'))
			{
				bHasColon = true;
			}
			else if (!FChar::IsHexDigit(Char) && Char != TCHAR('.'))
			{
				return false;
			}
		}
		return bHasColon;
	}

	FString EscapeJson(const FString& Text)
	{
		FString Out;
		Out.Reserve(Text.Len() + 2);
		Out += TEXT('"');
		for (const TCHAR Char : Text)
		{
			switch (Char)
			{
			case TCHAR('"'): Out += TEXT("\\\""); break;
			case TCHAR('\\'): Out += TEXT("\\\\"); break;
			case TCHAR('\n'): Out += TEXT("\\n"); break;
			case TCHAR('\r'): Out += TEXT("\\r"); break;
			case TCHAR('\t'): Out += TEXT("\\t"); break;
			default:
				if (Char < 0x20 || Char == 0x7F)
				{
					Out += FString::Printf(TEXT("\\u%04x"), static_cast<uint32>(Char));
				}
				else
				{
					Out += Char;
				}
				break;
			}
		}
		Out += TEXT('"');
		return Out;
	}

	FString FormatNumber(double Value)
	{
		if (!FMath::IsFinite(Value))
		{
			return TEXT("0");
		}
		const double Rounded = FMath::RoundToDouble(Value * 1000.0) / 1000.0;
		return FString::SanitizeFloat(Rounded == 0.0 ? 0.0 : Rounded, 0);
	}

	const TCHAR* FormatBool(bool bValue)
	{
		return bValue ? TEXT("true") : TEXT("false");
	}

	FString CleanText(const FString& Text, int32 MaxLength)
	{
		FString Clean;
		Clean.Reserve(Text.Len());
		for (const TCHAR Char : Text)
		{
			if (Char >= 0x20 && Char != 0x7F)
			{
				Clean += Char;
			}
		}
		Clean.TrimStartAndEndInline();
		return Clean.Left(MaxLength);
	}

	TSharedPtr<FJsonObject> ParseObject(const FString& Json)
	{
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
		{
			return nullptr;
		}
		return Object;
	}

	TSharedPtr<FJsonObject> ParseBody(TConstArrayView<uint8> Body, FString& OutError)
	{
		if (Body.IsEmpty() || !StageWebRemote::IsValidUtf8(Body))
		{
			OutError = TEXT("The body must be JSON.");
			return nullptr;
		}
		TSharedPtr<FJsonObject> Object = ParseObject(StageWebRemote::Utf8ToString(Body));
		if (!Object.IsValid())
		{
			OutError = TEXT("The body must be a JSON object.");
		}
		return Object;
	}

	bool ReadString(const FJsonObject& Object, const TCHAR* Field, int32 MaxLength, FString& OutValue, bool bRequired)
	{
		const TSharedPtr<FJsonValue> Value = Object.TryGetField(Field);
		if (!Value.IsValid() || Value->Type == EJson::Null)
		{
			OutValue.Reset();
			return !bRequired;
		}
		if (Value->Type != EJson::String)
		{
			return false;
		}
		OutValue = Value->AsString();
		return OutValue.Len() <= MaxLength;
	}

	bool ReadValue(const FJsonObject& Object, TOptional<float>& OutValue)
	{
		OutValue.Reset();
		const TSharedPtr<FJsonValue> Value = Object.TryGetField(TEXT("value"));
		if (!Value.IsValid() || Value->Type == EJson::Null)
		{
			return true;
		}
		if (Value->Type == EJson::Boolean)
		{
			OutValue = Value->AsBool() ? 1.f : 0.f;
			return true;
		}
		if (Value->Type != EJson::Number)
		{
			return false;
		}
		const double Number = Value->AsNumber();
		if (!FMath::IsFinite(Number) || FMath::Abs(Number) > 1.0e6)
		{
			return false;
		}
		OutValue = static_cast<float>(Number);
		return true;
	}

	FString Hex(const FGuid& Guid)
	{
		return Guid.ToString(EGuidFormats::Digits).ToLower();
	}
}

FString StageWebRemote::FHttpRequest::GetHeader(const TCHAR* Name) const
{
	const FString* Value = Headers.Find(Name);
	return Value ? *Value : FString();
}

StageWebRemote::EReadResult StageWebRemote::FHttpRequestReader::Fail(int32 Status, const TCHAR* Message)
{
	ErrorStatus = Status;
	Error = Message;
	Buffer.Reset();
	return EReadResult::Error;
}

StageWebRemote::EReadResult StageWebRemote::FHttpRequestReader::Append(TConstArrayView<uint8> Bytes)
{
	if (ErrorStatus != 0)
	{
		return EReadResult::Error;
	}
	Buffer.Append(Bytes.GetData(), Bytes.Num());
	if (bComplete)
	{
		return EReadResult::Complete;
	}

	if (!bHeadDone)
	{
		const int32 HeadEnd = StageWebRemoteImpl::FindHeadEnd(Buffer);
		if (HeadEnd == INDEX_NONE)
		{
			return Buffer.Num() > MaxHeadBytes ? Fail(431, TEXT("The request head is too large.")) : EReadResult::NeedMore;
		}
		if (HeadEnd + 4 > MaxHeadBytes)
		{
			return Fail(431, TEXT("The request head is too large."));
		}
		const EReadResult HeadResult = ParseHead(HeadEnd);
		if (HeadResult == EReadResult::Error)
		{
			return HeadResult;
		}
		Buffer.RemoveAt(0, HeadEnd + 4, EAllowShrinking::No);
		bHeadDone = true;
	}

	if (Buffer.Num() < BodyLength)
	{
		return EReadResult::NeedMore;
	}
	Request.Body = TArray<uint8>(Buffer.GetData(), BodyLength);
	Buffer.RemoveAt(0, BodyLength, EAllowShrinking::No);
	bComplete = true;
	return EReadResult::Complete;
}

StageWebRemote::EReadResult StageWebRemote::FHttpRequestReader::ParseHead(int32 HeadLength)
{
	for (int32 Index = 0; Index < HeadLength; ++Index)
	{
		if (Buffer[Index] == 0)
		{
			return Fail(400, TEXT("The request head contains a NUL byte."));
		}
	}
	const FString Head = Utf8ToString(TConstArrayView<uint8>(Buffer.GetData(), HeadLength));
	if (Head.IsEmpty())
	{
		return Fail(400, TEXT("The request head is not valid text."));
	}

	TArray<FString> Lines;
	Head.ParseIntoArray(Lines, TEXT("\r\n"), false);
	if (Lines.IsEmpty())
	{
		return Fail(400, TEXT("The request line is missing."));
	}

	TArray<FString> RequestLine;
	Lines[0].ParseIntoArray(RequestLine, TEXT(" "), false);
	if (RequestLine.Num() != 3 || RequestLine[0].IsEmpty())
	{
		return Fail(400, TEXT("The request line is malformed."));
	}
	if (RequestLine[2] != TEXT("HTTP/1.1") && RequestLine[2] != TEXT("HTTP/1.0"))
	{
		return Fail(505, TEXT("Only HTTP/1.1 is supported."));
	}
	Request.Method = RequestLine[0] == TEXT("GET") ? EMethod::Get : RequestLine[0] == TEXT("POST") ? EMethod::Post : EMethod::Other;

	FString Target = RequestLine[1];
	if (!Target.StartsWith(TEXT("/")))
	{
		return Fail(400, TEXT("The request target must be a path."));
	}
	int32 Cut = INDEX_NONE;
	if (Target.FindChar(TCHAR('?'), Cut) || Target.FindChar(TCHAR('#'), Cut))
	{
		Target.LeftInline(Cut);
	}
	if (Target.Len() > MaxPathLength)
	{
		return Fail(414, TEXT("The request path is too long."));
	}
	Request.Path = MoveTemp(Target);

	for (int32 LineIndex = 1; LineIndex < Lines.Num(); ++LineIndex)
	{
		const FString& Line = Lines[LineIndex];
		if (Line.IsEmpty() || Line[0] == TCHAR(' ') || Line[0] == TCHAR('\t'))
		{
			return Fail(400, TEXT("A header line is malformed."));
		}
		int32 Colon = INDEX_NONE;
		if (!Line.FindChar(TCHAR(':'), Colon) || Colon == 0)
		{
			return Fail(400, TEXT("A header line is malformed."));
		}
		const FString Name = Line.Left(Colon).ToLower();
		if (Name.Contains(TEXT(" ")) || Name.Contains(TEXT("\t")))
		{
			return Fail(400, TEXT("A header name contains whitespace."));
		}
		const FString Value = Line.Mid(Colon + 1).TrimStartAndEnd();
		if (FString* Existing = Request.Headers.Find(Name))
		{
			*Existing += TEXT(", ") + Value;
		}
		else
		{
			if (Request.Headers.Num() >= MaxHeaders)
			{
				return Fail(431, TEXT("The request has too many headers."));
			}
			Request.Headers.Add(Name, Value);
		}
	}

	if (Request.Headers.Contains(TEXT("transfer-encoding")))
	{
		return Fail(501, TEXT("Transfer-Encoding is not supported; send Content-Length."));
	}
	const FString* ContentLength = Request.Headers.Find(TEXT("content-length"));
	if (ContentLength)
	{
		if (!StageWebRemoteImpl::IsAllDigits(*ContentLength, 9))
		{
			return Fail(400, TEXT("Content-Length is malformed."));
		}
		BodyLength = FCString::Atoi(**ContentLength);
		if (BodyLength > MaxBodyBytes)
		{
			return Fail(413, TEXT("The request body is too large."));
		}
	}
	else if (Request.Method == EMethod::Post)
	{
		return Fail(411, TEXT("Content-Length is required."));
	}
	return EReadResult::NeedMore;
}

TArray<uint8> StageWebRemote::FHttpRequestReader::TakeRemainder()
{
	if (!bComplete)
	{
		return {};
	}
	TArray<uint8> Remainder = MoveTemp(Buffer);
	Buffer.Reset();
	return Remainder;
}

StageWebRemote::ERoute StageWebRemote::Route(const FHttpRequest& Request)
{
	struct FEntry
	{
		const TCHAR* Path;
		EMethod Method;
		ERoute Route;
	};
	static const FEntry Table[] = {
		{ TEXT("/"), EMethod::Get, ERoute::Page },
		{ TEXT("/index.html"), EMethod::Get, ERoute::Page },
		{ TEXT("/api/pair"), EMethod::Post, ERoute::Pair },
		{ TEXT("/api/command"), EMethod::Post, ERoute::Command },
		{ TEXT("/api/state"), EMethod::Get, ERoute::State },
		{ TEXT("/ws"), EMethod::Get, ERoute::WebSocket },
	};
	for (const FEntry& Entry : Table)
	{
		if (Request.Path.Equals(Entry.Path, ESearchCase::CaseSensitive))
		{
			return Request.Method == Entry.Method ? Entry.Route : ERoute::MethodNotAllowed;
		}
	}
	return ERoute::NotFound;
}

bool StageWebRemote::IsAllowedHost(FStringView Host)
{
	using namespace StageWebRemoteImpl;
	if (Host.IsEmpty() || Host.Len() > 64)
	{
		return false;
	}
	if (Host[0] == TCHAR('['))
	{
		int32 Close = INDEX_NONE;
		if (!Host.FindChar(TCHAR(']'), Close) || !IsIPv6Literal(Host.Mid(1, Close - 1)))
		{
			return false;
		}
		const FStringView Rest = Host.Mid(Close + 1);
		return Rest.IsEmpty() || (Rest[0] == TCHAR(':') && IsValidPort(Rest.Mid(1)));
	}

	FStringView Name = Host;
	int32 Colon = INDEX_NONE;
	if (Host.FindChar(TCHAR(':'), Colon))
	{
		if (!IsValidPort(Host.Mid(Colon + 1)))
		{
			return false;
		}
		Name = Host.Left(Colon);
	}
	return Name.Equals(TEXT("localhost"), ESearchCase::IgnoreCase) || IsIPv4(Name);
}

bool StageWebRemote::IsSameOrigin(FStringView Origin, FStringView Host)
{
	if (Origin.IsEmpty())
	{
		return true;
	}
	if (Host.IsEmpty())
	{
		return false;
	}
	return FString(Origin).Equals(FString(TEXT("http://")) + FString(Host), ESearchCase::IgnoreCase);
}

FString StageWebRemote::ParseBearer(FStringView Authorization)
{
	const FString Value = FString(Authorization).TrimStartAndEnd();
	const TCHAR* Scheme = TEXT("Bearer ");
	if (!Value.StartsWith(Scheme, ESearchCase::IgnoreCase))
	{
		return FString();
	}
	return Value.Mid(FCString::Strlen(Scheme)).TrimStartAndEnd();
}

const TCHAR* StageWebRemote::StatusText(int32 Status)
{
	switch (Status)
	{
	case 101: return TEXT("Switching Protocols");
	case 200: return TEXT("OK");
	case 202: return TEXT("Accepted");
	case 400: return TEXT("Bad Request");
	case 401: return TEXT("Unauthorized");
	case 403: return TEXT("Forbidden");
	case 404: return TEXT("Not Found");
	case 405: return TEXT("Method Not Allowed");
	case 408: return TEXT("Request Timeout");
	case 411: return TEXT("Length Required");
	case 413: return TEXT("Content Too Large");
	case 414: return TEXT("URI Too Long");
	case 421: return TEXT("Misdirected Request");
	case 426: return TEXT("Upgrade Required");
	case 429: return TEXT("Too Many Requests");
	case 431: return TEXT("Request Header Fields Too Large");
	case 500: return TEXT("Internal Server Error");
	case 501: return TEXT("Not Implemented");
	case 503: return TEXT("Service Unavailable");
	case 505: return TEXT("HTTP Version Not Supported");
	default: return TEXT("Error");
	}
}

TArray<uint8> StageWebRemote::MakeResponse(int32 Status, const TCHAR* ContentType, TConstArrayView<uint8> Body, TConstArrayView<FString> ExtraHeaders)
{
	FString Head = FString::Printf(TEXT("HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n"),
		Status, StatusText(Status), ContentType, Body.Num());
	for (const FString& Header : ExtraHeaders)
	{
		Head += Header + TEXT("\r\n");
	}
	Head += TEXT("\r\n");
	TArray<uint8> Bytes = StringToUtf8(Head);
	Bytes.Append(Body.GetData(), Body.Num());
	return Bytes;
}

TArray<uint8> StageWebRemote::MakeJsonResponse(int32 Status, const FString& Json)
{
	return MakeResponse(Status, TEXT("application/json; charset=utf-8"), StringToUtf8(Json));
}

TArray<uint8> StageWebRemote::MakeErrorResponse(int32 Status, const FString& Message, TConstArrayView<FString> ExtraHeaders)
{
	return MakeResponse(Status, TEXT("application/json; charset=utf-8"), StringToUtf8(FString::Printf(TEXT("{\"error\":%s}"), *StageWebRemoteImpl::EscapeJson(Message))), ExtraHeaders);
}

TArray<uint8> StageWebRemote::MakePageResponse(TConstArrayView<uint8> Html)
{
	static const FString Headers[] = {
		TEXT("Content-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self' ws:; img-src data:; base-uri 'none'; form-action 'none'; frame-ancestors 'none'"),
		TEXT("X-Frame-Options: DENY"),
		TEXT("Referrer-Policy: no-referrer"),
	};
	return MakeResponse(200, TEXT("text/html; charset=utf-8"), Html, Headers);
}

StageWebRemote::EUpgradeResult StageWebRemote::CheckUpgrade(const FHttpRequest& Request, FString& OutAcceptKey)
{
	if (!StageWebRemoteImpl::HasToken(Request.GetHeader(TEXT("upgrade")), TEXT("websocket"))
		|| !StageWebRemoteImpl::HasToken(Request.GetHeader(TEXT("connection")), TEXT("upgrade")))
	{
		return EUpgradeResult::BadRequest;
	}
	const FString Version = Request.GetHeader(TEXT("sec-websocket-version"));
	if (Version.IsEmpty())
	{
		return EUpgradeResult::BadRequest;
	}
	if (Version != TEXT("13"))
	{
		return EUpgradeResult::WrongVersion;
	}
	const FString Key = Request.GetHeader(TEXT("sec-websocket-key"));
	TArray<uint8> Decoded;
	if (Key.Len() != 24 || !FBase64::Decode(Key, Decoded) || Decoded.Num() != 16)
	{
		return EUpgradeResult::BadRequest;
	}
	OutAcceptKey = MakeAcceptKey(Key);
	return EUpgradeResult::Ok;
}

FString StageWebRemote::MakeAcceptKey(const FString& ClientKey)
{
	const TArray<uint8> Input = StringToUtf8(ClientKey + StageWebRemoteImpl::WebSocketGuid);
	uint8 Hash[FSHA1::DigestSize];
	FSHA1::HashBuffer(Input.GetData(), Input.Num(), Hash);
	return FBase64::Encode(Hash, FSHA1::DigestSize);
}

TArray<uint8> StageWebRemote::MakeUpgradeResponse(const FString& AcceptKey)
{
	return StringToUtf8(FString::Printf(TEXT("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n"), *AcceptKey));
}

StageWebRemote::EFrameResult StageWebRemote::DecodeClientFrame(TConstArrayView<uint8> Buffer, FWebSocketFrame& OutFrame, int32& OutConsumed, uint16& OutCloseCode)
{
	OutConsumed = 0;
	if (Buffer.Num() < 2)
	{
		return EFrameResult::NeedMore;
	}
	const uint8 First = Buffer[0];
	const uint8 Second = Buffer[1];
	const bool bFinal = (First & 0x80) != 0;
	const uint8 Opcode = First & 0x0F;
	const bool bControl = (Opcode & 0x08) != 0;

	if ((First & 0x70) != 0)
	{
		OutCloseCode = CloseCode::ProtocolError;
		return EFrameResult::Error;
	}
	const bool bKnown = Opcode == 0x0 || Opcode == 0x1 || Opcode == 0x2 || Opcode == 0x8 || Opcode == 0x9 || Opcode == 0xA;
	if (!bKnown || (Second & 0x80) == 0)
	{
		OutCloseCode = CloseCode::ProtocolError;
		return EFrameResult::Error;
	}

	uint64 Length = Second & 0x7F;
	int32 HeaderLength = 2;
	if (Length == 126)
	{
		if (Buffer.Num() < 4)
		{
			return EFrameResult::NeedMore;
		}
		Length = (static_cast<uint64>(Buffer[2]) << 8) | Buffer[3];
		HeaderLength = 4;
	}
	else if (Length == 127)
	{
		if (Buffer.Num() < 10)
		{
			return EFrameResult::NeedMore;
		}
		Length = 0;
		for (int32 Index = 2; Index < 10; ++Index)
		{
			Length = (Length << 8) | Buffer[Index];
		}
		HeaderLength = 10;
	}

	if (bControl && (!bFinal || Length > StageWebRemoteImpl::MaxControlPayload))
	{
		OutCloseCode = CloseCode::ProtocolError;
		return EFrameResult::Error;
	}
	if (!bControl && (Opcode != 0x1 || !bFinal))
	{
		OutCloseCode = CloseCode::Unsupported;
		return EFrameResult::Error;
	}
	if (Length > static_cast<uint64>(MaxFramePayload))
	{
		OutCloseCode = CloseCode::TooBig;
		return EFrameResult::Error;
	}

	const int32 PayloadLength = static_cast<int32>(Length);
	const int32 Total = HeaderLength + 4 + PayloadLength;
	if (Buffer.Num() < Total)
	{
		return EFrameResult::NeedMore;
	}
	const uint8* Mask = Buffer.GetData() + HeaderLength;
	const uint8* Payload = Mask + 4;
	OutFrame.Opcode = static_cast<EOpcode>(Opcode);
	OutFrame.Payload.SetNumUninitialized(PayloadLength);
	for (int32 Index = 0; Index < PayloadLength; ++Index)
	{
		OutFrame.Payload[Index] = Payload[Index] ^ Mask[Index & 3];
	}
	OutConsumed = Total;
	return EFrameResult::Frame;
}

TArray<uint8> StageWebRemote::EncodeFrame(EOpcode Opcode, TConstArrayView<uint8> Payload)
{
	TArray<uint8> Frame;
	Frame.Reserve(Payload.Num() + 10);
	Frame.Add(static_cast<uint8>(0x80 | static_cast<uint8>(Opcode)));
	const uint64 Length = static_cast<uint64>(Payload.Num());
	if (Length < 126)
	{
		Frame.Add(static_cast<uint8>(Length));
	}
	else if (Length <= 0xFFFF)
	{
		Frame.Add(126);
		Frame.Add(static_cast<uint8>((Length >> 8) & 0xFF));
		Frame.Add(static_cast<uint8>(Length & 0xFF));
	}
	else
	{
		Frame.Add(127);
		for (int32 Shift = 56; Shift >= 0; Shift -= 8)
		{
			Frame.Add(static_cast<uint8>((Length >> Shift) & 0xFF));
		}
	}
	Frame.Append(Payload.GetData(), Payload.Num());
	return Frame;
}

TArray<uint8> StageWebRemote::EncodeText(const FString& Text)
{
	return EncodeFrame(EOpcode::Text, StringToUtf8(Text));
}

TArray<uint8> StageWebRemote::EncodeClose(uint16 Code, const FString& Reason)
{
	FString Cut = Reason;
	TArray<uint8> ReasonBytes = StringToUtf8(Cut);
	while (ReasonBytes.Num() > StageWebRemoteImpl::MaxCloseReasonBytes && !Cut.IsEmpty())
	{
		Cut.LeftChopInline(1);
		ReasonBytes = StringToUtf8(Cut);
	}
	TArray<uint8> Payload;
	Payload.Add(static_cast<uint8>(Code >> 8));
	Payload.Add(static_cast<uint8>(Code & 0xFF));
	Payload.Append(ReasonBytes);
	return EncodeFrame(EOpcode::Close, Payload);
}

bool StageWebRemote::IsValidUtf8(TConstArrayView<uint8> Bytes)
{
	int32 Index = 0;
	while (Index < Bytes.Num())
	{
		const uint8 Lead = Bytes[Index];
		if (Lead < 0x80)
		{
			++Index;
			continue;
		}
		int32 Extra = 0;
		uint32 CodePoint = 0;
		uint32 Minimum = 0;
		if ((Lead & 0xE0) == 0xC0)
		{
			Extra = 1;
			CodePoint = Lead & 0x1F;
			Minimum = 0x80;
		}
		else if ((Lead & 0xF0) == 0xE0)
		{
			Extra = 2;
			CodePoint = Lead & 0x0F;
			Minimum = 0x800;
		}
		else if ((Lead & 0xF8) == 0xF0)
		{
			Extra = 3;
			CodePoint = Lead & 0x07;
			Minimum = 0x10000;
		}
		else
		{
			return false;
		}
		if (Index + Extra >= Bytes.Num())
		{
			return false;
		}
		for (int32 Offset = 1; Offset <= Extra; ++Offset)
		{
			const uint8 Continuation = Bytes[Index + Offset];
			if ((Continuation & 0xC0) != 0x80)
			{
				return false;
			}
			CodePoint = (CodePoint << 6) | (Continuation & 0x3F);
		}
		if (CodePoint < Minimum || CodePoint > 0x10FFFF || (CodePoint >= 0xD800 && CodePoint <= 0xDFFF))
		{
			return false;
		}
		Index += Extra + 1;
	}
	return true;
}

FString StageWebRemote::Utf8ToString(TConstArrayView<uint8> Bytes)
{
	if (Bytes.IsEmpty() || !IsValidUtf8(Bytes))
	{
		return FString();
	}
	const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
	return FString::ConstructFromPtrSize(Converted.Get(), Converted.Length());
}

TArray<uint8> StageWebRemote::StringToUtf8(const FString& Text)
{
	const FTCHARToUTF8 Converted(*Text, Text.Len());
	return TArray<uint8>(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
}

bool StageWebRemote::ParseClientMessage(const FString& Json, FClientMessage& OutMessage, FString& OutError)
{
	using namespace StageWebRemoteImpl;
	const TSharedPtr<FJsonObject> Object = ParseObject(Json);
	if (!Object.IsValid())
	{
		OutError = TEXT("not a JSON object");
		return false;
	}
	FString Type;
	if (!ReadString(*Object, TEXT("type"), 32, Type, true))
	{
		OutError = TEXT("no message type");
		return false;
	}
	OutMessage = FClientMessage();
	if (Type == TEXT("hello"))
	{
		OutMessage.Type = FClientMessage::EType::Hello;
		if (!ReadString(*Object, TEXT("token"), MaxTokenLength, OutMessage.Token, true))
		{
			OutError = TEXT("hello without a token");
			return false;
		}
		return true;
	}
	if (Type == TEXT("command"))
	{
		OutMessage.Type = FClientMessage::EType::Command;
		if (!ReadString(*Object, TEXT("path"), MaxPathLength, OutMessage.Path, true))
		{
			OutError = TEXT("command without a path");
			return false;
		}
		if (!ReadValue(*Object, OutMessage.Value))
		{
			OutError = TEXT("the value must be a number");
			return false;
		}
		return true;
	}
	OutError = TEXT("unknown message type");
	return false;
}

bool StageWebRemote::ParsePairBody(TConstArrayView<uint8> Body, FString& OutPin, FString& OutName, FString& OutError)
{
	using namespace StageWebRemoteImpl;
	const TSharedPtr<FJsonObject> Object = ParseBody(Body, OutError);
	if (!Object.IsValid())
	{
		return false;
	}
	if (!ReadString(*Object, TEXT("pin"), MaxPinLength, OutPin, true))
	{
		OutError = TEXT("Send the PIN as text: {\"pin\":\"123456\"}.");
		return false;
	}
	FString Name;
	if (!ReadString(*Object, TEXT("name"), MaxDeviceNameLength * 4, Name, false))
	{
		OutError = TEXT("The device name must be text.");
		return false;
	}
	OutName = CleanText(Name, MaxDeviceNameLength);
	if (OutName.IsEmpty())
	{
		OutName = TEXT("Browser");
	}
	return true;
}

bool StageWebRemote::ParseCommandBody(TConstArrayView<uint8> Body, FString& OutPath, TOptional<float>& OutValue, FString& OutError)
{
	using namespace StageWebRemoteImpl;
	const TSharedPtr<FJsonObject> Object = ParseBody(Body, OutError);
	if (!Object.IsValid())
	{
		return false;
	}
	if (!ReadString(*Object, TEXT("path"), MaxPathLength, OutPath, true))
	{
		OutError = TEXT("Send the command path as text: {\"path\":\"/stagecraft/cue/go\"}.");
		return false;
	}
	if (!ReadValue(*Object, OutValue))
	{
		OutError = TEXT("The value must be a number or a boolean.");
		return false;
	}
	return true;
}

FString StageWebRemote::MakeStateMessage(const FShowState& State)
{
	using namespace StageWebRemoteImpl;
	FString Json = FString::Printf(TEXT("{\"type\":\"state\",\"cue\":{\"active\":%s,\"label\":%s},\"playbacks\":["),
		*FormatNumber(State.ActiveCue), *EscapeJson(CleanText(State.ActiveLabel, MaxLabelLength)));
	for (int32 Index = 0; Index < State.Playbacks.Num(); ++Index)
	{
		const FShowState::FPlayback& Playback = State.Playbacks[Index];
		Json += FString::Printf(TEXT("%s{\"name\":%s,\"level\":%s,\"flash\":%s,\"active\":%s,\"empty\":%s}"), Index > 0 ? TEXT(",") : TEXT(""),
			*EscapeJson(CleanText(Playback.Name, MaxLabelLength)), *FormatNumber(Playback.Level), FormatBool(Playback.bFlash),
			FormatBool(Playback.bActive), FormatBool(Playback.bEmpty));
	}
	Json += FString::Printf(TEXT("],\"grand\":%s,\"blackout\":%s,\"audio\":%s}"), *FormatNumber(State.GrandMaster), FormatBool(State.bBlackout),
		*FormatNumber(State.AudioMaster));
	return Json;
}

FString StageWebRemote::MakeCuesMessage(TConstArrayView<FCueEntry> Cues)
{
	using namespace StageWebRemoteImpl;
	FString Json = TEXT("{\"type\":\"cues\",\"cues\":[");
	const int32 Count = FMath::Min(Cues.Num(), MaxCues);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		Json += FString::Printf(TEXT("%s{\"number\":%s,\"label\":%s}"), Index > 0 ? TEXT(",") : TEXT(""), *FormatNumber(Cues[Index].Number),
			*EscapeJson(CleanText(Cues[Index].Label, MaxLabelLength)));
	}
	Json += TEXT("]}");
	return Json;
}

FString StageWebRemote::MakeWelcomeMessage()
{
	return TEXT("{\"type\":\"welcome\"}");
}

FString StageWebRemote::MakeByeMessage(const FString& Reason)
{
	return FString::Printf(TEXT("{\"type\":\"bye\",\"reason\":%s}"), *StageWebRemoteImpl::EscapeJson(Reason));
}

FString StageWebRemote::MakeRefusedMessage(const FString& Message)
{
	return FString::Printf(TEXT("{\"type\":\"refused\",\"message\":%s}"), *StageWebRemoteImpl::EscapeJson(Message));
}

FString StageWebRemote::MakeTokenJson(const FString& Token)
{
	return FString::Printf(TEXT("{\"token\":%s}"), *StageWebRemoteImpl::EscapeJson(Token));
}

FString StageWebRemote::MakeOkJson()
{
	return TEXT("{\"ok\":true}");
}

bool StageWebRemote::Sanitize(FStageWebRemoteSettings& Settings)
{
	const FStageWebRemoteSettings Original = Settings;
	if (static_cast<uint8>(Settings.Scope) > static_cast<uint8>(EStageWebRemoteScope::Network))
	{
		Settings.Scope = EStageWebRemoteScope::Off;
	}
	if (Settings.Port < MinPort || Settings.Port > MaxPort)
	{
		Settings.Port = DefaultPort;
	}
	return !(Settings == Original);
}

FString StageWebRemote::MakePin()
{
	const FGuid Guid = FGuid::NewGuid();
	const uint64 High = (static_cast<uint64>(Guid.A) << 32) | Guid.B;
	const uint64 Low = (static_cast<uint64>(Guid.C) << 32) | Guid.D;
	static_assert(PinDigits == 6, "The format below writes six digits.");
	constexpr uint64 Modulus = 1000000;
	return FString::Printf(TEXT("%06llu"), static_cast<unsigned long long>((High ^ Low) % Modulus));
}

FString StageWebRemote::MakeToken()
{
	return StageWebRemoteImpl::Hex(FGuid::NewGuid()) + StageWebRemoteImpl::Hex(FGuid::NewGuid());
}

bool StageWebRemote::IsSecretMatch(const FString& Expected, const FString& Presented)
{
	if (Expected.IsEmpty() || Expected.Len() != Presented.Len())
	{
		return false;
	}
	uint32 Difference = 0;
	for (int32 Index = 0; Index < Expected.Len(); ++Index)
	{
		Difference |= static_cast<uint32>(Expected[Index] ^ Presented[Index]);
	}
	return Difference == 0;
}

StageWebRemote::FPairing::FPairing(TFunction<FString()> InMakePin, TFunction<FString()> InMakeToken)
	: MakePinFunc(InMakePin ? MoveTemp(InMakePin) : TFunction<FString()>(&MakePin))
	, MakeTokenFunc(InMakeToken ? MoveTemp(InMakeToken) : TFunction<FString()>(&MakeToken))
{
	RenewPin();
}

void StageWebRemote::FPairing::RenewPin()
{
	WrongPinsSinceNewPin = 0;
	const FString Previous = Pin;
	Pin = MakePinFunc();
	for (int32 Attempt = 0; Attempt < 4 && Pin == Previous; ++Attempt)
	{
		Pin = MakePinFunc();
	}
}

StageWebRemote::FPairing::FClientAttempts& StageWebRemote::FPairing::FindClient(const FString& Client, double Now)
{
	if (!Clients.Contains(Client) && Clients.Num() >= MaxTrackedClients)
	{
		FString Oldest;
		double OldestSeen = TNumericLimits<double>::Max();
		for (const TPair<FString, FClientAttempts>& Entry : Clients)
		{
			if (Entry.Value.LockedUntil <= Now && Entry.Value.LastSeen < OldestSeen)
			{
				OldestSeen = Entry.Value.LastSeen;
				Oldest = Entry.Key;
			}
		}
		if (OldestSeen == TNumericLimits<double>::Max())
		{
			for (const TPair<FString, FClientAttempts>& Entry : Clients)
			{
				if (Entry.Value.LastSeen < OldestSeen)
				{
					OldestSeen = Entry.Value.LastSeen;
					Oldest = Entry.Key;
				}
			}
		}
		Clients.Remove(Oldest);
	}
	FClientAttempts& Attempts = Clients.FindOrAdd(Client);
	Attempts.LastSeen = Now;
	return Attempts;
}

StageWebRemote::FPairing::EResult StageWebRemote::FPairing::Pair(const FString& PresentedPin, const FString& DeviceName, const FString& Client, double Now,
	FString& OutToken, double& OutRetryAfterSeconds, bool& bOutEvicted)
{
	OutToken.Reset();
	OutRetryAfterSeconds = 0.0;
	bOutEvicted = false;
	FClientAttempts& Attempts = FindClient(Client, Now);
	if (Now < Attempts.LockedUntil)
	{
		OutRetryAfterSeconds = Attempts.LockedUntil - Now;
		return EResult::Locked;
	}
	if (!IsSecretMatch(Pin, PresentedPin))
	{
		++Attempts.WrongPins;
		if (++WrongPinsSinceNewPin >= WrongPinsBeforeNewPin)
		{
			WrongPinsSinceNewPin = 0;
			RenewPin();
		}
		if (Attempts.WrongPins >= MaxWrongPins)
		{
			Attempts.WrongPins = 0;
			Attempts.LockedUntil = Now + LockoutSeconds;
			LatestLockedUntil = FMath::Max(LatestLockedUntil, Attempts.LockedUntil);
			OutRetryAfterSeconds = LockoutSeconds;
			return EResult::Locked;
		}
		return EResult::WrongPin;
	}

	Attempts.WrongPins = 0;
	if (Devices.Num() >= MaxPairedDevices)
	{
		Devices.RemoveAt(0);
		bOutEvicted = true;
	}
	FDevice& Device = Devices.AddDefaulted_GetRef();
	Device.Token = MakeTokenFunc();
	Device.Name = DeviceName;
	OutToken = Device.Token;
	return EResult::Paired;
}

bool StageWebRemote::FPairing::IsPaired(const FString& Token) const
{
	bool bFound = false;
	for (const FDevice& Device : Devices)
	{
		bFound |= IsSecretMatch(Device.Token, Token);
	}
	return bFound;
}

void StageWebRemote::FPairing::UnpairAll()
{
	Devices.Reset();
	RenewPin();
}

TArray<FString> StageWebRemote::FPairing::GetDeviceNames() const
{
	TArray<FString> Names;
	Names.Reserve(Devices.Num());
	for (const FDevice& Device : Devices)
	{
		Names.Add(Device.Name);
	}
	return Names;
}
