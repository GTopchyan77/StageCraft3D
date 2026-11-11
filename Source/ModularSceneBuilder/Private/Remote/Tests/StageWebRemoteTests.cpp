// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Game/StageCraftGameModeBase.h"
#include "Game/StageSessionTypes.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Misc/AutomationTest.h"
#include "Remote/StageRemoteMapping.h"
#include "Remote/StageWebRemoteProtocol.h"
#include "Remote/StageWebRemoteSubsystem.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Settings/StageCraftUserSettings.h"
#include "Show/ShowControlTypes.h"
#include "Sockets.h"
#include "SocketSubsystem.h"

namespace StageWebRemoteTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	struct FScopedWorld
	{
		UWorld* World = nullptr;

		FScopedWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("StageWebRemoteTestWorld"));
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			World->BeginPlay();
			World->GetWorldSettings()->NotifyBeginPlay();
		}

		~FScopedWorld()
		{
			World->BeginTearingDown();
			World->EndPlay(EEndPlayReason::Quit);
			World->DestroyWorld(false);
			GEngine->DestroyWorldContext(World);
		}

		UStageWebRemoteSubsystem* GetWeb() const { return World->GetSubsystem<UStageWebRemoteSubsystem>(); }
	};

	struct FScopedWebSettings
	{
		FStageWebRemoteSettings Original;
		UStageCraftUserSettings* UserSettings = nullptr;

		explicit FScopedWebSettings(const FStageWebRemoteSettings& Settings)
		{
			UserSettings = UStageCraftUserSettings::Get();
			if (UserSettings)
			{
				Original = UserSettings->GetWebRemoteSettings();
				UserSettings->SetWebRemoteSettings(Settings);
			}
		}

		~FScopedWebSettings()
		{
			if (UserSettings)
			{
				UserSettings->SetWebRemoteSettings(Original);
			}
		}
	};

	TArray<uint8> Utf8(const FString& Text)
	{
		return StageWebRemote::StringToUtf8(Text);
	}

	StageWebRemote::EReadResult ReadText(StageWebRemote::FHttpRequestReader& Reader, const FString& Text)
	{
		return Reader.Append(Utf8(Text));
	}

	int32 ReadError(const FString& Text)
	{
		StageWebRemote::FHttpRequestReader Reader;
		return ReadText(Reader, Text) == StageWebRemote::EReadResult::Error ? Reader.GetErrorStatus() : 0;
	}

	TArray<uint8> ClientFrame(uint8 FirstByte, TConstArrayView<uint8> Payload, bool bMasked = true)
	{
		TArray<uint8> Frame;
		Frame.Add(FirstByte);
		const uint8 MaskBit = bMasked ? 0x80 : 0x00;
		if (Payload.Num() < 126)
		{
			Frame.Add(MaskBit | static_cast<uint8>(Payload.Num()));
		}
		else if (Payload.Num() <= 0xFFFF)
		{
			Frame.Add(MaskBit | 126);
			Frame.Add(static_cast<uint8>(Payload.Num() >> 8));
			Frame.Add(static_cast<uint8>(Payload.Num() & 0xFF));
		}
		else
		{
			Frame.Add(MaskBit | 127);
			for (int32 Shift = 56; Shift >= 0; Shift -= 8)
			{
				Frame.Add(static_cast<uint8>((static_cast<uint64>(Payload.Num()) >> Shift) & 0xFF));
			}
		}
		const uint8 Mask[4] = { 0x37, 0xFA, 0x21, 0x3D };
		if (bMasked)
		{
			Frame.Append(Mask, 4);
		}
		for (int32 Index = 0; Index < Payload.Num(); ++Index)
		{
			Frame.Add(bMasked ? Payload[Index] ^ Mask[Index & 3] : Payload[Index]);
		}
		return Frame;
	}

	TArray<uint8> TextFrame(const FString& Text)
	{
		return ClientFrame(0x81, Utf8(Text));
	}

	bool TakeServerFrame(TArray<uint8>& Buffer, uint8& OutOpcode, TArray<uint8>& OutPayload)
	{
		if (Buffer.Num() < 2)
		{
			return false;
		}
		uint64 Length = Buffer[1] & 0x7F;
		int32 Header = 2;
		if (Length == 126)
		{
			if (Buffer.Num() < 4)
			{
				return false;
			}
			Length = (static_cast<uint64>(Buffer[2]) << 8) | Buffer[3];
			Header = 4;
		}
		else if (Length == 127)
		{
			if (Buffer.Num() < 10)
			{
				return false;
			}
			Length = 0;
			for (int32 Index = 2; Index < 10; ++Index)
			{
				Length = (Length << 8) | Buffer[Index];
			}
			Header = 10;
		}
		if (Buffer.Num() < Header + static_cast<int32>(Length))
		{
			return false;
		}
		OutOpcode = Buffer[0] & 0x0F;
		OutPayload = TArray<uint8>(Buffer.GetData() + Header, static_cast<int32>(Length));
		Buffer.RemoveAt(0, Header + static_cast<int32>(Length));
		return true;
	}

	TSharedPtr<FJsonObject> ParseJson(const FString& Json)
	{
		TSharedPtr<FJsonObject> Object;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object);
		return Object;
	}

	struct FTestSocket
	{
		FSocket* Socket = nullptr;
		TArray<uint8> Received;
		bool bClosed = false;

		~FTestSocket()
		{
			Close();
		}

		bool Connect(uint16 Port, const FIPv4Address& Address = FIPv4Address(127, 0, 0, 1))
		{
			ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
			Socket = Sockets->CreateSocket(NAME_Stream, TEXT("StageWebRemoteTestClient"), false);
			return Socket && Socket->Connect(*FIPv4Endpoint(Address, Port).ToInternetAddr());
		}

		void Close()
		{
			if (Socket)
			{
				Socket->Close();
				ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
				Socket = nullptr;
			}
		}

		void Send(TConstArrayView<uint8> Bytes)
		{
			int32 Sent = 0;
			Socket->Send(Bytes.GetData(), Bytes.Num(), Sent);
		}

		void Send(const FString& Text)
		{
			Send(Utf8(Text));
		}

		void ReadFor(double Seconds)
		{
			const double Deadline = FPlatformTime::Seconds() + Seconds;
			while (!bClosed && FPlatformTime::Seconds() < Deadline)
			{
				if (!Socket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromMilliseconds(20.0)))
				{
					continue;
				}
				uint8 Buffer[8192];
				int32 Read = 0;
				if (!Socket->Recv(Buffer, sizeof(Buffer), Read) || Read == 0)
				{
					bClosed = true;
					return;
				}
				Received.Append(Buffer, Read);
				return;
			}
		}

		void ReadUntilClosed(double Seconds)
		{
			const double Deadline = FPlatformTime::Seconds() + Seconds;
			while (!bClosed && FPlatformTime::Seconds() < Deadline)
			{
				ReadFor(0.05);
			}
		}

		bool ReadUntilHeadEnd(double Seconds)
		{
			const double Deadline = FPlatformTime::Seconds() + Seconds;
			while (FPlatformTime::Seconds() < Deadline)
			{
				const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Received.GetData()), Received.Num());
				const FString Head = FString::ConstructFromPtrSize(Text.Get(), Text.Length());
				int32 End = Head.Find(TEXT("\r\n\r\n"));
				if (End != INDEX_NONE)
				{
					const int32 HeadBytes = Utf8(Head.Left(End + 4)).Num();
					LastHead = Head.Left(End + 4);
					Received.RemoveAt(0, HeadBytes);
					return true;
				}
				if (bClosed)
				{
					return false;
				}
				ReadFor(0.05);
			}
			return false;
		}

		bool WaitForMessage(const FString& Type, FString& OutText, double Seconds, TFunctionRef<void()> Pump)
		{
			const double Deadline = FPlatformTime::Seconds() + Seconds;
			while (FPlatformTime::Seconds() < Deadline)
			{
				uint8 Opcode = 0;
				TArray<uint8> Payload;
				while (TakeServerFrame(Received, Opcode, Payload))
				{
					if (Opcode == 0x8)
					{
						LastCloseCode = Payload.Num() >= 2 ? static_cast<uint16>((Payload[0] << 8) | Payload[1]) : 0;
						continue;
					}
					bSawPong |= Opcode == 0xA;
					if (Opcode != 0x1)
					{
						continue;
					}
					const FString Text = StageWebRemote::Utf8ToString(Payload);
					Messages.Add(Text);
					const TSharedPtr<FJsonObject> Object = ParseJson(Text);
					if (Object.IsValid() && Object->GetStringField(TEXT("type")) == Type)
					{
						OutText = Text;
						return true;
					}
				}
				if (bClosed)
				{
					return false;
				}
				Pump();
				ReadFor(0.02);
			}
			return false;
		}

		FString LastHead;
		TArray<FString> Messages;
		uint16 LastCloseCode = 0;
		bool bSawPong = false;
	};

	FString MakeRequest(const TCHAR* Method, const TCHAR* Path, uint16 Port, const FString& Body = FString(), const FString& ExtraHeaders = FString())
	{
		const int32 BodyBytes = Utf8(Body).Num();
		FString Request = FString::Printf(TEXT("%s %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n"), Method, Path, Port);
		if (FCString::Strcmp(Method, TEXT("POST")) == 0)
		{
			Request += FString::Printf(TEXT("Content-Type: application/json\r\nContent-Length: %d\r\n"), BodyBytes);
		}
		return Request + ExtraHeaders + TEXT("\r\n") + Body;
	}

	struct FHttpAnswer
	{
		int32 Status = 0;
		FString Head;
		FString Body;
	};

	FHttpAnswer Exchange(uint16 Port, const FString& Raw)
	{
		FHttpAnswer Answer;
		FTestSocket Client;
		if (!Client.Connect(Port))
		{
			return Answer;
		}
		Client.Send(Raw);
		Client.ReadUntilClosed(3.0);
		const FString Text = StageWebRemote::Utf8ToString(Client.Received);
		int32 End = Text.Find(TEXT("\r\n\r\n"));
		if (End == INDEX_NONE)
		{
			return Answer;
		}
		Answer.Head = Text.Left(End);
		Answer.Body = Text.Mid(End + 4);
		if (Answer.Head.StartsWith(TEXT("HTTP/1.1 ")))
		{
			Answer.Status = FCString::Atoi(*Answer.Head.Mid(9, 3));
		}
		return Answer;
	}

	FStageRemoteCommand MakeWebCommand(EStageRemoteAction Action)
	{
		FStageRemoteCommand Command;
		Command.Action = Action;
		Command.Source = EStageRemoteSource::Web;
		return Command;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageWebRemoteHttpTest, "StageCraft.WebRemote.Http", StageWebRemoteTests::Flags)
bool FStageWebRemoteHttpTest::RunTest(const FString& Parameters)
{
	using namespace StageWebRemote;
	using namespace StageWebRemoteTests;

	{
		FHttpRequestReader Reader;
		TestEqual(TEXT("Half a head needs more"), ReadText(Reader, TEXT("GET /index.html?x=1 HTTP/1.1\r\nHost: 127.0.0.1:8080\r\n")), EReadResult::NeedMore);
		TestEqual(TEXT("The blank line completes a GET"), ReadText(Reader, TEXT("Accept: a\r\naccept: b\r\n\r\n")), EReadResult::Complete);
		TestEqual(TEXT("Method"), Reader.GetRequest().Method, EMethod::Get);
		TestEqual(TEXT("Query stripped"), Reader.GetRequest().Path, FString(TEXT("/index.html")));
		TestEqual(TEXT("Header names are lower case"), Reader.GetRequest().GetHeader(TEXT("host")), FString(TEXT("127.0.0.1:8080")));
		TestEqual(TEXT("Repeated headers are joined"), Reader.GetRequest().GetHeader(TEXT("accept")), FString(TEXT("a, b")));
		TestEqual(TEXT("Routed to the page"), Route(Reader.GetRequest()), ERoute::Page);
	}
	{
		FHttpRequestReader Reader;
		TestEqual(TEXT("POST head waits for its body"), ReadText(Reader, TEXT("POST /api/pair HTTP/1.1\r\nHost: localhost\r\nContent-Length: 12\r\n\r\n{\"pin\":")), EReadResult::NeedMore);
		TestEqual(TEXT("Body completes, extra bytes stay"), ReadText(Reader, TEXT("\"12\"}\x81\x80")), EReadResult::Complete);
		TestEqual(TEXT("Body"), Utf8ToString(Reader.GetRequest().Body), FString(TEXT("{\"pin\":\"12\"}")));
		TestEqual(TEXT("Remainder kept"), Reader.TakeRemainder().Num(), 4);
		TestEqual(TEXT("Routed to pairing"), Route(Reader.GetRequest()), ERoute::Pair);
	}

	TestEqual(TEXT("POST without Content-Length"), ReadError(TEXT("POST /api/pair HTTP/1.1\r\nHost: localhost\r\n\r\n")), 411);
	TestEqual(TEXT("Body too large"), ReadError(FString::Printf(TEXT("POST /api/pair HTTP/1.1\r\nContent-Length: %d\r\n\r\n"), MaxBodyBytes + 1)), 413);
	TestEqual(TEXT("Malformed Content-Length"), ReadError(TEXT("POST /api/pair HTTP/1.1\r\nContent-Length: -1\r\n\r\n")), 400);
	TestEqual(TEXT("Two Content-Lengths"), ReadError(TEXT("POST /api/pair HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n")), 400);
	TestEqual(TEXT("Chunked bodies are refused"), ReadError(TEXT("POST /api/pair HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n")), 501);
	TestEqual(TEXT("Other protocol version"), ReadError(TEXT("GET / HTTP/2.0\r\n\r\n")), 505);
	TestEqual(TEXT("Absolute-form target"), ReadError(TEXT("GET http://evil/ HTTP/1.1\r\n\r\n")), 400);
	TestEqual(TEXT("Malformed request line"), ReadError(TEXT("GET  / HTTP/1.1\r\n\r\n")), 400);
	TestEqual(TEXT("Folded header"), ReadError(TEXT("GET / HTTP/1.1\r\nHost: a\r\n b\r\n\r\n")), 400);
	TestEqual(TEXT("Space before the colon"), ReadError(TEXT("GET / HTTP/1.1\r\nHost : a\r\n\r\n")), 400);
	TestEqual(TEXT("Path too long"), ReadError(FString::Printf(TEXT("GET /%s HTTP/1.1\r\n\r\n"), *FString::ChrN(MaxPathLength + 1, TEXT('a')))), 414);
	TestEqual(TEXT("Head without an end"), ReadError(TEXT("GET / HTTP/1.1\r\nX: ") + FString::ChrN(MaxHeadBytes, TEXT('a'))), 431);
	{
		FString ManyHeaders = TEXT("GET / HTTP/1.1\r\n");
		for (int32 Index = 0; Index <= MaxHeaders; ++Index)
		{
			ManyHeaders += FString::Printf(TEXT("X-%d: 1\r\n"), Index);
		}
		TestEqual(TEXT("Too many headers"), ReadError(ManyHeaders + TEXT("\r\n")), 431);
	}
	{
		FHttpRequestReader Reader;
		TArray<uint8> Bytes = Utf8(TEXT("GET / HTTP/1.1\r\nX: a"));
		Bytes.Add(0);
		Bytes.Append(Utf8(TEXT("\r\n\r\n")));
		TestEqual(TEXT("NUL in the head"), Reader.Append(Bytes), EReadResult::Error);
		TestEqual(TEXT("Nothing more after an error"), ReadText(Reader, TEXT("GET / HTTP/1.1\r\n\r\n")), EReadResult::Error);
	}

	const auto RouteOf = [this](const TCHAR* Text)
	{
		FHttpRequestReader Reader;
		ReadText(Reader, Text);
		return Route(Reader.GetRequest());
	};
	TestEqual(TEXT("Command"), RouteOf(TEXT("POST /api/command HTTP/1.1\r\nContent-Length: 0\r\n\r\n")), ERoute::Command);
	TestEqual(TEXT("State"), RouteOf(TEXT("GET /api/state HTTP/1.1\r\n\r\n")), ERoute::State);
	TestEqual(TEXT("WebSocket"), RouteOf(TEXT("GET /ws HTTP/1.1\r\n\r\n")), ERoute::WebSocket);
	TestEqual(TEXT("GET on a POST route"), RouteOf(TEXT("GET /api/pair HTTP/1.1\r\n\r\n")), ERoute::MethodNotAllowed);
	TestEqual(TEXT("Other method on the page"), RouteOf(TEXT("DELETE / HTTP/1.1\r\n\r\n")), ERoute::MethodNotAllowed);
	TestEqual(TEXT("Unknown path"), RouteOf(TEXT("GET /etc/passwd HTTP/1.1\r\n\r\n")), ERoute::NotFound);
	TestEqual(TEXT("Paths are case-sensitive"), RouteOf(TEXT("GET /WS HTTP/1.1\r\n\r\n")), ERoute::NotFound);

	TestTrue(TEXT("IPv4 with port"), IsAllowedHost(TEXT("192.168.1.20:8080")));
	TestTrue(TEXT("IPv4"), IsAllowedHost(TEXT("10.0.0.1")));
	TestTrue(TEXT("localhost"), IsAllowedHost(TEXT("LocalHost:8080")));
	TestTrue(TEXT("IPv6 literal"), IsAllowedHost(TEXT("[::1]:8080")));
	TestFalse(TEXT("Empty Host"), IsAllowedHost(TEXT("")));
	TestFalse(TEXT("A name (DNS rebinding)"), IsAllowedHost(TEXT("evil.example:8080")));
	TestFalse(TEXT("localhost subdomain"), IsAllowedHost(TEXT("localhost.evil.example")));
	TestFalse(TEXT("Octet over 255"), IsAllowedHost(TEXT("192.168.1.256")));
	TestFalse(TEXT("Three octets"), IsAllowedHost(TEXT("192.168.1")));
	TestFalse(TEXT("Bad port"), IsAllowedHost(TEXT("127.0.0.1:99999")));
	TestFalse(TEXT("Two colons"), IsAllowedHost(TEXT("127.0.0.1:80:80")));
	TestFalse(TEXT("Unclosed bracket"), IsAllowedHost(TEXT("[::1")));

	TestTrue(TEXT("No Origin"), IsSameOrigin(TEXT(""), TEXT("127.0.0.1:8080")));
	TestTrue(TEXT("Same origin"), IsSameOrigin(TEXT("http://127.0.0.1:8080"), TEXT("127.0.0.1:8080")));
	TestFalse(TEXT("Other site"), IsSameOrigin(TEXT("http://evil.example"), TEXT("127.0.0.1:8080")));
	TestFalse(TEXT("Other port"), IsSameOrigin(TEXT("http://127.0.0.1:9000"), TEXT("127.0.0.1:8080")));
	TestFalse(TEXT("Null origin"), IsSameOrigin(TEXT("null"), TEXT("127.0.0.1:8080")));

	TestEqual(TEXT("Bearer"), ParseBearer(TEXT("Bearer abc123")), FString(TEXT("abc123")));
	TestEqual(TEXT("Bearer is case-insensitive"), ParseBearer(TEXT("  bearer   abc  ")), FString(TEXT("abc")));
	TestTrue(TEXT("Other scheme"), ParseBearer(TEXT("Basic abc")).IsEmpty());

	{
		const FString Response = Utf8ToString(MakeErrorResponse(404, TEXT("Not \"here\".")));
		TestTrue(TEXT("Status line"), Response.StartsWith(TEXT("HTTP/1.1 404 Not Found\r\n")));
		TestTrue(TEXT("Connection: close"), Response.Contains(TEXT("\r\nConnection: close\r\n")));
		TestTrue(TEXT("no-store"), Response.Contains(TEXT("\r\nCache-Control: no-store\r\n")));
		TestTrue(TEXT("Escaped JSON body"), Response.EndsWith(TEXT("\r\n\r\n{\"error\":\"Not \\\"here\\\".\"}")));
		TestTrue(TEXT("Content-Length counts bytes"), Response.Contains(TEXT("Content-Length: 25\r\n")));
	}
	{
		const FString Page = Utf8ToString(MakePageResponse(Utf8(TEXT("<html>é</html>"))));
		TestTrue(TEXT("Page: CSP"), Page.Contains(TEXT("Content-Security-Policy: default-src 'none';")));
		TestTrue(TEXT("Page: no framing"), Page.Contains(TEXT("X-Frame-Options: DENY")));
		TestTrue(TEXT("Page: UTF-8 length"), Page.Contains(TEXT("Content-Length: 15\r\n")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageWebRemoteWebSocketTest, "StageCraft.WebRemote.WebSocket", StageWebRemoteTests::Flags)
bool FStageWebRemoteWebSocketTest::RunTest(const FString& Parameters)
{
	using namespace StageWebRemote;
	using namespace StageWebRemoteTests;

	TestEqual(TEXT("RFC 6455 accept key"), MakeAcceptKey(TEXT("dGhlIHNhbXBsZSBub25jZQ==")), FString(TEXT("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")));

	const auto Upgrade = [](const FString& Headers, FString& OutKey)
	{
		FHttpRequestReader Reader;
		ReadText(Reader, TEXT("GET /ws HTTP/1.1\r\nHost: 127.0.0.1\r\n") + Headers + TEXT("\r\n"));
		return CheckUpgrade(Reader.GetRequest(), OutKey);
	};
	FString Key;
	TestEqual(TEXT("A browser's handshake"), Upgrade(TEXT("Upgrade: websocket\r\nConnection: keep-alive, Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"), Key), EUpgradeResult::Ok);
	TestEqual(TEXT("Accept key"), Key, FString(TEXT("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")));
	TestEqual(TEXT("No Upgrade header"), Upgrade(TEXT("Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"), Key), EUpgradeResult::BadRequest);
	TestEqual(TEXT("No Connection: Upgrade"), Upgrade(TEXT("Upgrade: websocket\r\nConnection: keep-alive\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"), Key), EUpgradeResult::BadRequest);
	TestEqual(TEXT("Old version"), Upgrade(TEXT("Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 8\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"), Key), EUpgradeResult::WrongVersion);
	TestEqual(TEXT("Short key"), Upgrade(TEXT("Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: abcd\r\n"), Key), EUpgradeResult::BadRequest);
	TestEqual(TEXT("Key not base64 of 16 bytes"), Upgrade(TEXT("Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: ************************\r\n"), Key), EUpgradeResult::BadRequest);

	const auto Decode = [](const TArray<uint8>& Bytes, FWebSocketFrame& OutFrame, int32& OutConsumed, uint16& OutCode)
	{
		return DecodeClientFrame(Bytes, OutFrame, OutConsumed, OutCode);
	};
	FWebSocketFrame Frame;
	int32 Consumed = 0;
	uint16 Code = 0;

	TArray<uint8> Hello = TextFrame(TEXT("{\"type\":\"hello\"}"));
	Hello.Append(TextFrame(TEXT("x")));
	TestEqual(TEXT("A masked text frame"), Decode(Hello, Frame, Consumed, Code), EFrameResult::Frame);
	TestEqual(TEXT("Unmasked"), Utf8ToString(Frame.Payload), FString(TEXT("{\"type\":\"hello\"}")));
	TestEqual(TEXT("Only the first frame is consumed"), Consumed, 2 + 4 + 16);
	for (int32 Cut = 0; Cut < 2 + 4 + 16; ++Cut)
	{
		if (Decode(TArray<uint8>(Hello.GetData(), Cut), Frame, Consumed, Code) != EFrameResult::NeedMore)
		{
			AddError(FString::Printf(TEXT("A frame cut at %d bytes should need more."), Cut));
			break;
		}
	}

	const TArray<uint8> Medium = TextFrame(FString::ChrN(300, TEXT('a')));
	TestEqual(TEXT("16-bit length"), Decode(Medium, Frame, Consumed, Code), EFrameResult::Frame);
	TestEqual(TEXT("16-bit payload"), Frame.Payload.Num(), 300);

	TestEqual(TEXT("Unmasked client frame"), Decode(ClientFrame(0x81, Utf8(TEXT("x")), false), Frame, Consumed, Code), EFrameResult::Error);
	TestEqual(TEXT("... is a protocol error"), Code, CloseCode::ProtocolError);
	TestEqual(TEXT("Binary"), Decode(ClientFrame(0x82, Utf8(TEXT("x"))), Frame, Consumed, Code), EFrameResult::Error);
	TestEqual(TEXT("... is unsupported"), Code, CloseCode::Unsupported);
	TestEqual(TEXT("Fragment"), Decode(ClientFrame(0x01, Utf8(TEXT("x"))), Frame, Consumed, Code), EFrameResult::Error);
	TestEqual(TEXT("... is unsupported"), Code, CloseCode::Unsupported);
	TestEqual(TEXT("Continuation"), Decode(ClientFrame(0x80, Utf8(TEXT("x"))), Frame, Consumed, Code), EFrameResult::Error);
	TestEqual(TEXT("Reserved bit"), Decode(ClientFrame(0xC1, Utf8(TEXT("x"))), Frame, Consumed, Code), EFrameResult::Error);
	TestEqual(TEXT("... is a protocol error"), Code, CloseCode::ProtocolError);
	TestEqual(TEXT("Unknown opcode"), Decode(ClientFrame(0x83, Utf8(TEXT("x"))), Frame, Consumed, Code), EFrameResult::Error);
	TestEqual(TEXT("Fragmented ping"), Decode(ClientFrame(0x09, Utf8(TEXT("x"))), Frame, Consumed, Code), EFrameResult::Error);
	TestEqual(TEXT("Ping over 125 bytes"), Decode(ClientFrame(0x89, Utf8(FString::ChrN(126, TEXT('a')))), Frame, Consumed, Code), EFrameResult::Error);
	{
		TArray<uint8> Big = ClientFrame(0x81, Utf8(FString::ChrN(MaxFramePayload + 1, TEXT('a'))));
		Big.SetNum(10);
		TestEqual(TEXT("Too large, refused from the header alone"), Decode(Big, Frame, Consumed, Code), EFrameResult::Error);
		TestEqual(TEXT("... with 1009"), Code, CloseCode::TooBig);
	}
	{
		TArray<uint8> Huge = { 0x81, 0xFF, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
		TestEqual(TEXT("64-bit length with the top bit"), Decode(Huge, Frame, Consumed, Code), EFrameResult::Error);
		TestEqual(TEXT("... is too big"), Code, CloseCode::TooBig);
	}
	TestEqual(TEXT("Ping"), Decode(ClientFrame(0x89, Utf8(TEXT("hi"))), Frame, Consumed, Code), EFrameResult::Frame);
	TestEqual(TEXT("Ping opcode"), Frame.Opcode, EOpcode::Ping);
	TestEqual(TEXT("Close"), Decode(ClientFrame(0x88, {}), Frame, Consumed, Code), EFrameResult::Frame);
	TestEqual(TEXT("Close opcode"), Frame.Opcode, EOpcode::Close);

	{
		TArray<uint8> Encoded = EncodeText(TEXT("abc"));
		TestEqual(TEXT("Short server frame"), Encoded.Num(), 5);
		TestEqual(TEXT("FIN + text"), static_cast<int32>(Encoded[0]), 0x81);
		TestEqual(TEXT("Unmasked length"), static_cast<int32>(Encoded[1]), 3);
		TestEqual(TEXT("126 bytes use the 16-bit form"), EncodeFrame(EOpcode::Text, TArray<uint8>(Utf8(FString::ChrN(126, TEXT('a'))))).Num(), 4 + 126);
		TestEqual(TEXT("125 bytes fit the short form"), EncodeFrame(EOpcode::Text, TArray<uint8>(Utf8(FString::ChrN(125, TEXT('a'))))).Num(), 2 + 125);
		TArray<uint8> Large;
		Large.SetNumZeroed(70000);
		TestEqual(TEXT("70000 bytes use the 64-bit form"), EncodeFrame(EOpcode::Text, Large).Num(), 10 + 70000);
		TArray<uint8> Round = EncodeFrame(EOpcode::Text, Large);
		uint8 Opcode = 0;
		TArray<uint8> Payload;
		TestTrue(TEXT("Round trip"), TakeServerFrame(Round, Opcode, Payload) && Payload.Num() == 70000);
	}
	{
		const TArray<uint8> Close = EncodeClose(CloseCode::PolicyViolation, FString::ChrN(200, TEXT('x')));
		TestEqual(TEXT("Close code"), (Close[2] << 8) | Close[3], static_cast<int32>(CloseCode::PolicyViolation));
		TestEqual(TEXT("Close payload capped at 125"), static_cast<int32>(Close[1]), 125);
	}

	TestTrue(TEXT("ASCII"), IsValidUtf8(Utf8(TEXT("hello"))));
	TestTrue(TEXT("Armenian"), IsValidUtf8(Utf8(TEXT("Բարև"))));
	TestTrue(TEXT("Four-byte"), IsValidUtf8(TArray<uint8>({ 0xF0, 0x9F, 0x8E, 0xAD })));
	TestFalse(TEXT("Overlong"), IsValidUtf8(TArray<uint8>({ 0xC0, 0xAF })));
	TestFalse(TEXT("Surrogate"), IsValidUtf8(TArray<uint8>({ 0xED, 0xA0, 0x80 })));
	TestFalse(TEXT("Above U+10FFFF"), IsValidUtf8(TArray<uint8>({ 0xF4, 0x90, 0x80, 0x80 })));
	TestFalse(TEXT("Truncated"), IsValidUtf8(TArray<uint8>({ 0xE2, 0x82 })));
	TestFalse(TEXT("Lone continuation"), IsValidUtf8(TArray<uint8>({ 0x80 })));
	TestTrue(TEXT("Invalid UTF-8 gives no text"), Utf8ToString(TArray<uint8>({ 0x41, 0xFF })).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageWebRemoteMessagesTest, "StageCraft.WebRemote.Messages", StageWebRemoteTests::Flags)
bool FStageWebRemoteMessagesTest::RunTest(const FString& Parameters)
{
	using namespace StageWebRemote;
	using namespace StageWebRemoteTests;

	FClientMessage Message;
	FString Error;
	TestTrue(TEXT("Hello"), ParseClientMessage(TEXT("{\"type\":\"hello\",\"token\":\"abc\"}"), Message, Error));
	TestEqual(TEXT("Hello type"), Message.Type, FClientMessage::EType::Hello);
	TestEqual(TEXT("Token"), Message.Token, FString(TEXT("abc")));
	TestFalse(TEXT("Hello without a token"), ParseClientMessage(TEXT("{\"type\":\"hello\"}"), Message, Error));
	TestFalse(TEXT("Token as a number"), ParseClientMessage(TEXT("{\"type\":\"hello\",\"token\":5}"), Message, Error));
	TestTrue(TEXT("Command with a value"), ParseClientMessage(TEXT("{\"type\":\"command\",\"path\":\"/stagecraft/playback/2/level\",\"value\":0.5}"), Message, Error));
	TestEqual(TEXT("Command type"), Message.Type, FClientMessage::EType::Command);
	TestEqual(TEXT("Path"), Message.Path, FString(TEXT("/stagecraft/playback/2/level")));
	TestTrue(TEXT("Value"), Message.Value.IsSet() && Message.Value.GetValue() == 0.5f);
	TestTrue(TEXT("Command without a value"), ParseClientMessage(TEXT("{\"type\":\"command\",\"path\":\"/stagecraft/cue/go\"}"), Message, Error) && !Message.Value.IsSet());
	TestTrue(TEXT("Null value"), ParseClientMessage(TEXT("{\"type\":\"command\",\"path\":\"/x\",\"value\":null}"), Message, Error) && !Message.Value.IsSet());
	TestTrue(TEXT("Boolean value"), ParseClientMessage(TEXT("{\"type\":\"command\",\"path\":\"/x\",\"value\":true}"), Message, Error) && Message.Value.GetValue() == 1.f);
	TestFalse(TEXT("String value"), ParseClientMessage(TEXT("{\"type\":\"command\",\"path\":\"/x\",\"value\":\"1\"}"), Message, Error));
	TestFalse(TEXT("Enormous value"), ParseClientMessage(TEXT("{\"type\":\"command\",\"path\":\"/x\",\"value\":1e300}"), Message, Error));
	TestFalse(TEXT("Command without a path"), ParseClientMessage(TEXT("{\"type\":\"command\"}"), Message, Error));
	TestFalse(TEXT("Path too long"), ParseClientMessage(FString::Printf(TEXT("{\"type\":\"command\",\"path\":\"%s\"}"), *FString::ChrN(MaxPathLength + 1, TEXT('a'))), Message, Error));
	TestFalse(TEXT("Unknown type"), ParseClientMessage(TEXT("{\"type\":\"admin\"}"), Message, Error));
	TestFalse(TEXT("Not an object"), ParseClientMessage(TEXT("[1,2]"), Message, Error));
	TestFalse(TEXT("Not JSON"), ParseClientMessage(TEXT("go"), Message, Error));

	FString Pin;
	FString Name;
	TestTrue(TEXT("Pair body"), ParsePairBody(Utf8(TEXT("{\"pin\":\"012345\",\"name\":\"  Gevor's\\tphone  \"}")), Pin, Name, Error));
	TestEqual(TEXT("PIN keeps its leading zero"), Pin, FString(TEXT("012345")));
	TestEqual(TEXT("Name cleaned"), Name, FString(TEXT("Gevor'sphone")));
	TestTrue(TEXT("Pair body without a name"), ParsePairBody(Utf8(TEXT("{\"pin\":\"1\"}")), Pin, Name, Error));
	TestEqual(TEXT("Default name"), Name, FString(TEXT("Browser")));
	TestTrue(TEXT("Long name cut"), ParsePairBody(Utf8(FString::Printf(TEXT("{\"pin\":\"1\",\"name\":\"%s\"}"), *FString::ChrN(100, TEXT('n')))), Pin, Name, Error) && Name.Len() == MaxDeviceNameLength);
	TestFalse(TEXT("PIN as a number"), ParsePairBody(Utf8(TEXT("{\"pin\":123456}")), Pin, Name, Error));
	TestFalse(TEXT("Empty body"), ParsePairBody({}, Pin, Name, Error));
	TestFalse(TEXT("Invalid UTF-8 body"), ParsePairBody(TArray<uint8>({ 0x7B, 0xFF, 0x7D }), Pin, Name, Error));

	FString Path;
	TOptional<float> Value;
	TestTrue(TEXT("Command body"), ParseCommandBody(Utf8(TEXT("{\"path\":\"/stagecraft/master/grand\",\"value\":0.25}")), Path, Value, Error));
	TestTrue(TEXT("Command body value"), Value.IsSet() && Value.GetValue() == 0.25f);
	TestFalse(TEXT("Command body without a path"), ParseCommandBody(Utf8(TEXT("{\"value\":1}")), Path, Value, Error));

	FShowState State;
	State.ActiveCue = 2.5f;
	State.ActiveLabel = TEXT("Chorus \"loud\"\n");
	State.Playbacks.SetNum(StagePlaybackRules::Count);
	State.Playbacks[0].Name = TEXT("Strobe");
	State.Playbacks[0].Level = 0.3f;
	State.Playbacks[0].bActive = true;
	State.Playbacks[0].bEmpty = false;
	State.GrandMaster = 0.75f;
	State.bBlackout = true;
	const FString StateJson = MakeStateMessage(State);
	const TSharedPtr<FJsonObject> Parsed = ParseJson(StateJson);
	if (TestTrue(TEXT("State is JSON"), Parsed.IsValid()))
	{
		TestEqual(TEXT("State type"), Parsed->GetStringField(TEXT("type")), FString(TEXT("state")));
		const TSharedPtr<FJsonObject> Cue = Parsed->GetObjectField(TEXT("cue"));
		TestEqual(TEXT("Active cue"), Cue->GetNumberField(TEXT("active")), 2.5);
		TestEqual(TEXT("Label escaped and cleaned"), Cue->GetStringField(TEXT("label")), FString(TEXT("Chorus \"loud\"")));
		const TArray<TSharedPtr<FJsonValue>>& Playbacks = Parsed->GetArrayField(TEXT("playbacks"));
		TestEqual(TEXT("Ten playbacks"), Playbacks.Num(), StagePlaybackRules::Count);
		TestEqual(TEXT("Level rounded"), Playbacks[0]->AsObject()->GetNumberField(TEXT("level")), 0.3);
		TestTrue(TEXT("Active"), Playbacks[0]->AsObject()->GetBoolField(TEXT("active")));
		TestTrue(TEXT("Empty playback"), Playbacks[1]->AsObject()->GetBoolField(TEXT("empty")));
		TestEqual(TEXT("Grand Master"), Parsed->GetNumberField(TEXT("grand")), 0.75);
		TestTrue(TEXT("Blackout"), Parsed->GetBoolField(TEXT("blackout")));
	}
	TestTrue(TEXT("No cue: negative"), MakeStateMessage(FShowState()).Contains(TEXT("\"active\":-1")));
	TestTrue(TEXT("Rounded numbers are short"), StateJson.Contains(TEXT("\"level\":0.3,")));

	TArray<FCueEntry> Cues;
	for (int32 Index = 0; Index < MaxCues + 20; ++Index)
	{
		Cues.Add({ static_cast<float>(Index + 1), FString::ChrN(100, TEXT('L')) });
	}
	const TSharedPtr<FJsonObject> CueList = ParseJson(MakeCuesMessage(Cues));
	if (TestTrue(TEXT("Cue list is JSON"), CueList.IsValid()))
	{
		const TArray<TSharedPtr<FJsonValue>>& Entries = CueList->GetArrayField(TEXT("cues"));
		TestEqual(TEXT("Cue list capped"), Entries.Num(), MaxCues);
		TestEqual(TEXT("Labels capped"), Entries[0]->AsObject()->GetStringField(TEXT("label")).Len(), MaxLabelLength);
	}
	TestTrue(TEXT("Bye is JSON"), ParseJson(MakeByeMessage(TEXT("a\\b\"c"))).IsValid());
	TestTrue(TEXT("Refused is JSON"), ParseJson(MakeRefusedMessage(TEXT("\x01 control"))).IsValid());

	FStageWebRemoteSettings Settings;
	TestFalse(TEXT("Defaults are clean"), Sanitize(Settings));
	TestEqual(TEXT("Off by default"), Settings.Scope, EStageWebRemoteScope::Off);
	Settings.Port = 80;
	TestTrue(TEXT("Privileged port repaired"), Sanitize(Settings));
	TestEqual(TEXT("Default port"), Settings.Port, DefaultPort);
	Settings.Scope = static_cast<EStageWebRemoteScope>(9);
	TestTrue(TEXT("Unknown scope repaired"), Sanitize(Settings));
	TestEqual(TEXT("Unknown scope is Off"), Settings.Scope, EStageWebRemoteScope::Off);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageWebRemotePairingTest, "StageCraft.WebRemote.Pairing", StageWebRemoteTests::Flags)
bool FStageWebRemotePairingTest::RunTest(const FString& Parameters)
{
	using namespace StageWebRemote;

	const FString Random = MakePin();
	TestEqual(TEXT("PIN length"), Random.Len(), PinDigits);
	TestTrue(TEXT("PIN digits"), Random.IsNumeric());
	TestEqual(TEXT("Token length"), MakeToken().Len(), 64);
	TestNotEqual(TEXT("Tokens differ"), MakeToken(), MakeToken());
	TestTrue(TEXT("Secret match"), IsSecretMatch(TEXT("abc"), TEXT("abc")));
	TestFalse(TEXT("Secret mismatch"), IsSecretMatch(TEXT("abc"), TEXT("abd")));
	TestFalse(TEXT("Secret prefix"), IsSecretMatch(TEXT("abc"), TEXT("ab")));
	TestFalse(TEXT("Empty secret never matches"), IsSecretMatch(TEXT(""), TEXT("")));

	int32 PinCount = 0;
	int32 TokenCount = 0;
	FPairing Pairing([&PinCount]() { return FString::Printf(TEXT("%06d"), 100000 + ++PinCount); },
		[&TokenCount]() { return FString::Printf(TEXT("token-%d"), ++TokenCount); });
	TestEqual(TEXT("First PIN"), Pairing.GetPin(), FString(TEXT("100001")));

	FString Token;
	double RetryAfter = 0.0;
	bool bEvicted = false;
	const auto Pair = [&Pairing, &Token, &RetryAfter, &bEvicted](const TCHAR* Pin, const TCHAR* Client, double Now)
	{
		return Pairing.Pair(Pin, TEXT("Device"), Client, Now, Token, RetryAfter, bEvicted);
	};
	const TCHAR* Phone = TEXT("192.168.1.30");
	const TCHAR* Attacker = TEXT("192.168.1.66");

	TestEqual(TEXT("Wrong PIN"), Pair(TEXT("999999"), Phone, 0.0), FPairing::EResult::WrongPin);
	TestTrue(TEXT("No token for a wrong PIN"), Token.IsEmpty());
	TestEqual(TEXT("Right PIN"), Pairing.Pair(TEXT("100001"), TEXT("Phone"), Phone, 1.0, Token, RetryAfter, bEvicted), FPairing::EResult::Paired);
	TestEqual(TEXT("Token"), Token, FString(TEXT("token-1")));
	TestFalse(TEXT("Nobody evicted"), bEvicted);
	TestTrue(TEXT("Paired"), Pairing.IsPaired(TEXT("token-1")));
	TestFalse(TEXT("Unknown token"), Pairing.IsPaired(TEXT("token-2")));
	TestFalse(TEXT("Empty token"), Pairing.IsPaired(TEXT("")));
	TestEqual(TEXT("Device name"), Pairing.GetDeviceNames()[0], FString(TEXT("Phone")));

	for (int32 Attempt = 1; Attempt < MaxWrongPins; ++Attempt)
	{
		TestEqual(TEXT("Wrong PINs below the limit"), Pair(TEXT("000000"), Attacker, 2.0), FPairing::EResult::WrongPin);
	}
	TestEqual(TEXT("The fifth wrong PIN locks that address"), Pair(TEXT("000000"), Attacker, 2.0), FPairing::EResult::Locked);
	TestEqual(TEXT("Retry after the lockout"), RetryAfter, LockoutSeconds);
	TestEqual(TEXT("A lock does not change the PIN"), Pairing.GetPin(), FString(TEXT("100001")));
	TestEqual(TEXT("Locked until"), Pairing.GetLockedUntil(), 2.0 + LockoutSeconds);
	TestEqual(TEXT("Even the right PIN waits at that address"), Pair(TEXT("100001"), Attacker, 10.0), FPairing::EResult::Locked);
	TestEqual(TEXT("Remaining wait"), RetryAfter, 2.0 + LockoutSeconds - 10.0);
	TestEqual(TEXT("Another device still pairs"), Pair(TEXT("100001"), TEXT("192.168.1.31"), 10.0), FPairing::EResult::Paired);

	for (int32 Index = 0; Index < WrongPinsBeforeNewPin - 2 - MaxWrongPins; ++Index)
	{
		TestEqual(TEXT("Wrong PINs from many addresses"), Pair(TEXT("000000"), *FString::Printf(TEXT("10.0.0.%d"), Index), 20.0), FPairing::EResult::WrongPin);
	}
	TestEqual(TEXT("Below the total: the PIN stays"), Pairing.GetPin(), FString(TEXT("100001")));
	Pair(TEXT("000000"), TEXT("10.0.1.1"), 20.0);
	TestEqual(TEXT("The total replaces the PIN"), Pairing.GetPin(), FString(TEXT("100002")));
	TestEqual(TEXT("The old PIN is dead"), Pair(TEXT("100001"), Phone, 21.0), FPairing::EResult::WrongPin);
	TestEqual(TEXT("After the wait the address may try again"), Pair(TEXT("100002"), Attacker, 40.0), FPairing::EResult::Paired);
	TestTrue(TEXT("Earlier devices stay paired"), Pairing.IsPaired(TEXT("token-1")));

	for (int32 Index = 0; Index < MaxTrackedClients + 10; ++Index)
	{
		Pair(TEXT("000000"), *FString::Printf(TEXT("10.1.%d.%d"), Index / 200, Index % 200), 45.0);
	}
	TestEqual(TEXT("Many addresses: the PIN keeps working for a known device"), Pairing.Pair(Pairing.GetPin(), TEXT("Tablet"), Phone, 46.0, Token, RetryAfter, bEvicted), FPairing::EResult::Paired);

	for (int32 Device = Pairing.GetPairedCount(); Device < MaxPairedDevices; ++Device)
	{
		Pair(*Pairing.GetPin(), Phone, 50.0);
		TestFalse(TEXT("Below the cap nobody is evicted"), bEvicted);
	}
	TestEqual(TEXT("Full"), Pairing.GetPairedCount(), MaxPairedDevices);
	TestEqual(TEXT("One more"), Pair(*Pairing.GetPin(), Phone, 51.0), FPairing::EResult::Paired);
	TestTrue(TEXT("The oldest was evicted"), bEvicted);
	TestEqual(TEXT("Still at the cap"), Pairing.GetPairedCount(), MaxPairedDevices);
	TestFalse(TEXT("The oldest token is dead"), Pairing.IsPaired(TEXT("token-1")));
	TestTrue(TEXT("The newest is paired"), Pairing.IsPaired(Token));

	const FString BeforeRenew = Pairing.GetPin();
	Pairing.RenewPin();
	TestNotEqual(TEXT("New PIN"), Pairing.GetPin(), BeforeRenew);
	TestTrue(TEXT("Devices stay after a new PIN"), Pairing.IsPaired(Token));
	const FString BeforeUnpair = Pairing.GetPin();
	Pairing.UnpairAll();
	TestEqual(TEXT("Unpair all"), Pairing.GetPairedCount(), 0);
	TestFalse(TEXT("Tokens dead"), Pairing.IsPaired(Token));
	TestNotEqual(TEXT("Unpair all makes a new PIN"), Pairing.GetPin(), BeforeUnpair);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageWebRemoteRulesTest, "StageCraft.WebRemote.Rules", StageWebRemoteTests::Flags)
bool FStageWebRemoteRulesTest::RunTest(const FString& Parameters)
{
	using namespace StageWebRemoteTests;
	FScopedWebSettings Off{ FStageWebRemoteSettings() };
	FScopedWorld Stage;
	AStageCraftGameModeBase* GameMode = Stage.World->SpawnActor<AStageCraftGameModeBase>();
	const APlayerController* Player = Stage.World->SpawnActor<APlayerController>();
	if (!TestNotNull(TEXT("Game mode"), GameMode) || !TestNotNull(TEXT("Player"), Player))
	{
		return false;
	}
	TestFalse(TEXT("No player: refused"), GameMode->EvaluateWebRemote(nullptr).IsSuccess());
	TestTrue(TEXT("Allowed by default"), GameMode->EvaluateWebRemote(Player).IsSuccess());

	const FStructProperty* RulesProperty = FindFProperty<FStructProperty>(AStageCraftGameModeBase::StaticClass(), TEXT("SessionRules"));
	if (!TestNotNull(TEXT("SessionRules property"), RulesProperty))
	{
		return false;
	}
	RulesProperty->ContainerPtrToValuePtr<FStageSessionRules>(GameMode)->bAllowWebRemote = false;
	const FStageEconomyResultInfo Verdict = GameMode->EvaluateWebRemote(Player);
	TestEqual(TEXT("A curated session refuses"), Verdict.Code, EStageEconomyResult::SessionRuleViolation);
	TestFalse(TEXT("And says why"), Verdict.Message.IsEmpty());
	TestTrue(TEXT("MIDI and OSC are a separate rule"), GameMode->EvaluateRemoteControl(Player).IsSuccess());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageWebRemoteSubsystemTest, "StageCraft.WebRemote.Subsystem", StageWebRemoteTests::Flags)
bool FStageWebRemoteSubsystemTest::RunTest(const FString& Parameters)
{
	using namespace StageWebRemoteTests;
	FScopedWebSettings Off{ FStageWebRemoteSettings() };
	FScopedWorld Stage;
	UStageWebRemoteSubsystem* Web = Stage.GetWeb();
	if (!TestNotNull(TEXT("Web Remote subsystem"), Web))
	{
		return false;
	}
	TestFalse(TEXT("Off: not listening"), Web->GetStatus().bListening);
	TestEqual(TEXT("Off: no port"), Web->GetListeningPort(), 0);
	TestFalse(TEXT("Off: no PIN to renew"), Web->RenewPin());
	TestFalse(TEXT("Off: nothing to unpair"), Web->UnpairAll());
	TestTrue(TEXT("The page ships with the module"), FPaths::FileExists(UStageWebRemoteSubsystem::GetPageFile()));

	Web->ReceiveCommand(TEXT("/stagecraft/cue/go"), {});
	Web->FlushCommands();
	TestEqual(TEXT("Without a handler: refused"), Web->GetStatus().Refused, int64(1));

	TArray<FStageRemoteCommand> Carried;
	bool bRefuse = false;
	Web->SetCommandHandler(UStageWebRemoteSubsystem::FCommandHandler::CreateLambda([&Carried, &bRefuse](const FStageRemoteCommand& Command)
	{
		Carried.Add(Command);
		return bRefuse ? FStageEconomyResultInfo::Fail(EStageEconomyResult::SessionRuleViolation, FText::FromString(TEXT("Not now.")))
			: FStageEconomyResultInfo::Ok();
	}));

	Web->ReceiveCommand(TEXT("/stagecraft/playback/3/level"), 0.2f);
	Web->ReceiveCommand(TEXT("/stagecraft/playback/3/level"), 0.6f);
	Web->ReceiveCommand(TEXT("/stagecraft/cue/go"), 0.f);
	Web->ReceiveCommand(TEXT("/stagecraft/admin/delete"), 1.f);
	Web->ReceiveCommand(TEXT("/stagecraft/master/blackout"), 1.f);
	Web->FlushCommands();
	if (TestEqual(TEXT("Coalesced level, blackout"), Carried.Num(), 2))
	{
		TestEqual(TEXT("Marked as web"), Carried[0].Source, EStageRemoteSource::Web);
		TestEqual(TEXT("Playback index"), Carried[0].Playback, 2);
		TestEqual(TEXT("Latest value wins"), Carried[0].Value, 0.6f);
		TestEqual(TEXT("Blackout"), Carried[1].Action, EStageRemoteAction::Blackout);
	}
	const FStageWebRemoteStatus& Status = Web->GetStatus();
	TestEqual(TEXT("Received"), Status.Received, int64(6));
	TestEqual(TEXT("Applied"), Status.Applied, int64(2));
	TestEqual(TEXT("Ignored: a release and an unknown path"), Status.Ignored, int64(2));

	bRefuse = true;
	Web->ReceiveCommand(TEXT("/stagecraft/cue/back"), {});
	Web->FlushCommands();
	TestEqual(TEXT("Refused counted"), Web->GetStatus().Refused, int64(2));
	TestEqual(TEXT("Refusal kept for the panel"), Web->GetStatus().LastRefusal.ToString(), FString(TEXT("Not now.")));

	const AActor* Stranger = Stage.World->SpawnActor<AActor>();
	Web->ClearCommandHandler(Stranger);
	bRefuse = false;
	Web->ReceiveCommand(TEXT("/stagecraft/cue/go"), {});
	Web->FlushCommands();
	TestEqual(TEXT("Handler kept"), Carried.Last().Action, EStageRemoteAction::CueGo);
	return true;
}

namespace StageWebRemoteTests
{
	struct FLoopbackState
	{
		static constexpr int32 Port = 48781;
		static inline const TCHAR* Pin = TEXT("246810");

		TUniquePtr<FScopedWebSettings> Settings;
		TUniquePtr<FScopedWorld> Stage;
		TUniquePtr<FTestSocket> Page;
		TArray<FStageRemoteCommand> Carried;
		bool bRefuse = false;
		FString Token;
		double Deadline = 0.0;
		int32 Step = 0;

		UStageWebRemoteSubsystem* GetWeb() const { return Stage ? Stage->GetWeb() : nullptr; }

		void TickStage() const
		{
			Stage->World->Tick(LEVELTICK_All, 0.01f);
		}

		void Close()
		{
			Page.Reset();
			Stage.Reset();
			Settings.Reset();
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageWebRemoteLoopbackTest, "StageCraft.WebRemote.Loopback", StageWebRemoteTests::Flags)
bool FStageWebRemoteLoopbackTest::RunTest(const FString& Parameters)
{
	using namespace StageWebRemoteTests;
	TSharedRef<FLoopbackState> State = MakeShared<FLoopbackState>();

	FStageWebRemoteSettings Local;
	Local.Scope = EStageWebRemoteScope::ThisComputer;
	Local.Port = FLoopbackState::Port;
	State->Settings = MakeUnique<FScopedWebSettings>(Local);
	State->Stage = MakeUnique<FScopedWorld>();

	UStageWebRemoteSubsystem* Web = State->GetWeb();
	if (!TestNotNull(TEXT("Web Remote subsystem"), Web) || !TestTrue(TEXT("Listening from the saved settings"), Web->GetStatus().bListening))
	{
		State->Close();
		return false;
	}
	Web->SetPinForTesting(FLoopbackState::Pin);
	FText Error;
	TestTrue(TEXT("Restart with the test PIN"), Web->ApplySettings(Local, Error));
	TestEqual(TEXT("Status shows the PIN"), Web->GetStatus().Pin, FString(FLoopbackState::Pin));
	TestEqual(TEXT("This PC: the loopback address"), Web->GetStatus().Addresses, TArray<FString>({ FString::Printf(TEXT("http://127.0.0.1:%d"), FLoopbackState::Port) }));
	Web->SetCommandHandler(UStageWebRemoteSubsystem::FCommandHandler::CreateLambda([WeakState = TWeakPtr<FLoopbackState>(State)](const FStageRemoteCommand& Command)
	{
		const TSharedPtr<FLoopbackState> Pinned = WeakState.Pin();
		if (!Pinned)
		{
			return FStageEconomyResultInfo::Ok();
		}
		Pinned->Carried.Add(Command);
		return Pinned->bRefuse ? FStageEconomyResultInfo::Fail(EStageEconomyResult::SessionRuleViolation, FText::FromString(TEXT("Switched off for this session.")))
			: FStageEconomyResultInfo::Ok();
	}));

	const uint16 Port = FLoopbackState::Port;
	const FHttpAnswer PageAnswer = Exchange(Port, MakeRequest(TEXT("GET"), TEXT("/"), Port));
	TestEqual(TEXT("The page"), PageAnswer.Status, 200);
	TestTrue(TEXT("The page is HTML"), PageAnswer.Body.Contains(TEXT("<html")) && PageAnswer.Body.Contains(TEXT("StageCraft")));
	TestTrue(TEXT("The page carries its CSP"), PageAnswer.Head.Contains(TEXT("Content-Security-Policy")));
	TestEqual(TEXT("A name in Host is refused"), Exchange(Port, TEXT("GET / HTTP/1.1\r\nHost: evil.example\r\n\r\n")).Status, 421);
	TestEqual(TEXT("Another site's Origin is refused"), Exchange(Port, MakeRequest(TEXT("GET"), TEXT("/"), Port, FString(), TEXT("Origin: http://evil.example\r\n"))).Status, 403);
	TestEqual(TEXT("Unknown path"), Exchange(Port, MakeRequest(TEXT("GET"), TEXT("/secret"), Port)).Status, 404);
	TestEqual(TEXT("Wrong method"), Exchange(Port, MakeRequest(TEXT("GET"), TEXT("/api/pair"), Port)).Status, 405);
	TestEqual(TEXT("Malformed request"), Exchange(Port, TEXT("NONSENSE\r\n\r\n")).Status, 400);
	TestEqual(TEXT("Wrong PIN"), Exchange(Port, MakeRequest(TEXT("POST"), TEXT("/api/pair"), Port, TEXT("{\"pin\":\"111111\"}"))).Status, 403);

	const FHttpAnswer Paired = Exchange(Port, MakeRequest(TEXT("POST"), TEXT("/api/pair"), Port, TEXT("{\"pin\":\"246810\",\"name\":\"Test phone\"}")));
	TestEqual(TEXT("Right PIN"), Paired.Status, 200);
	const TSharedPtr<FJsonObject> PairedJson = ParseJson(Paired.Body);
	State->Token = PairedJson.IsValid() ? PairedJson->GetStringField(TEXT("token")) : FString();
	if (!TestEqual(TEXT("A 64-character token"), State->Token.Len(), 64))
	{
		State->Close();
		return false;
	}
	const FString Bearer = FString::Printf(TEXT("Authorization: Bearer %s\r\n"), *State->Token);
	TestEqual(TEXT("Command without a token"), Exchange(Port, MakeRequest(TEXT("POST"), TEXT("/api/command"), Port, TEXT("{\"path\":\"/stagecraft/cue/go\"}"))).Status, 401);
	TestEqual(TEXT("Command with a wrong token"), Exchange(Port, MakeRequest(TEXT("POST"), TEXT("/api/command"), Port, TEXT("{\"path\":\"/stagecraft/cue/go\"}"), TEXT("Authorization: Bearer nope\r\n"))).Status, 401);
	TestEqual(TEXT("Malformed command body"), Exchange(Port, MakeRequest(TEXT("POST"), TEXT("/api/command"), Port, TEXT("{\"value\":1}"), Bearer)).Status, 400);
	TestEqual(TEXT("HTTP command accepted"), Exchange(Port, MakeRequest(TEXT("POST"), TEXT("/api/command"), Port, TEXT("{\"path\":\"/stagecraft/cue/go\"}"), Bearer)).Status, 202);
	const FHttpAnswer StateAnswer = Exchange(Port, MakeRequest(TEXT("GET"), TEXT("/api/state"), Port, FString(), Bearer));
	TestEqual(TEXT("State"), StateAnswer.Status, 200);
	TestTrue(TEXT("State JSON"), StateAnswer.Body.Contains(TEXT("\"type\":\"state\"")));

	{
		bool bCanBind = false;
		uint32 LanIp = 0;
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->GetLocalHostAddr(*GLog, bCanBind)->GetIp(LanIp);
		if (LanIp != 0 && !FIPv4Address(LanIp).IsLoopbackAddress())
		{
			FTestSocket FromLan;
			TestFalse(TEXT("This PC: the network address does not answer"), FromLan.Connect(Port, FIPv4Address(LanIp)));
		}
		else
		{
			AddInfo(TEXT("No network address on this PC: the loopback-only check was skipped."));
		}
	}

	State->Page = MakeUnique<FTestSocket>();
	FTestSocket& Page = *State->Page;
	if (!TestTrue(TEXT("Page socket"), Page.Connect(Port)))
	{
		State->Close();
		return false;
	}
	Page.Send(FString::Printf(TEXT("GET /ws HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nOrigin: http://127.0.0.1:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n"), Port, Port));
	TestTrue(TEXT("Handshake answered"), Page.ReadUntilHeadEnd(3.0));
	TestTrue(TEXT("101"), Page.LastHead.StartsWith(TEXT("HTTP/1.1 101")));
	TestTrue(TEXT("Accept key"), Page.LastHead.Contains(TEXT("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")));
	Page.Send(TextFrame(FString::Printf(TEXT("{\"type\":\"hello\",\"token\":\"%s\"}"), *State->Token)));
	FString Text;
	TestTrue(TEXT("Welcome"), Page.WaitForMessage(TEXT("welcome"), Text, 3.0, []() {}));
	TestTrue(TEXT("The cue list follows"), Page.WaitForMessage(TEXT("cues"), Text, 3.0, []() {}));
	TestTrue(TEXT("The state follows"), Page.WaitForMessage(TEXT("state"), Text, 3.0, []() {}));
	Page.Send(ClientFrame(0x89, Utf8(TEXT("ping"))));
	Page.Send(TextFrame(TEXT("{\"type\":\"command\",\"path\":\"/stagecraft/playback/2/level\",\"value\":0.75}")));
	State->Deadline = FPlatformTime::Seconds() + 5.0;

	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State]() -> bool
	{
		State->TickStage();
		const bool bTimedOut = FPlatformTime::Seconds() > State->Deadline;
		UStageWebRemoteSubsystem* Remote = State->GetWeb();
		FTestSocket& Page = *State->Page;
		FString Text;
		switch (State->Step)
		{
		case 0:
			if (State->Carried.Num() < 2 && !bTimedOut)
			{
				return false;
			}
			if (TestEqual(TEXT("The HTTP and the WebSocket command arrived"), State->Carried.Num(), 2))
			{
				TestEqual(TEXT("HTTP: GO"), State->Carried[0].Action, EStageRemoteAction::CueGo);
				TestEqual(TEXT("WebSocket: playback 2 level"), State->Carried[1].Action, EStageRemoteAction::PlaybackLevel);
				TestEqual(TEXT("Playback index"), State->Carried[1].Playback, 1);
				TestEqual(TEXT("Level"), State->Carried[1].Value, 0.75f);
				TestEqual(TEXT("Marked as web"), State->Carried[1].Source, EStageRemoteSource::Web);
			}
			TestEqual(TEXT("One page open"), Remote ? Remote->GetStatus().ConnectedPages : -1, 1);
			TestEqual(TEXT("One device paired"), Remote ? Remote->GetStatus().PairedDevices : -1, 1);
			State->bRefuse = true;
			Page.Send(TextFrame(TEXT("{\"type\":\"command\",\"path\":\"/stagecraft/cue/back\"}")));
			State->Step = 1;
			State->Deadline = FPlatformTime::Seconds() + 5.0;
			return false;
		case 1:
			if (Page.WaitForMessage(TEXT("refused"), Text, 0.02, []() {}))
			{
				const TSharedPtr<FJsonObject> Refused = ParseJson(Text);
				TestEqual(TEXT("The page hears the GameMode's reason"), Refused.IsValid() ? Refused->GetStringField(TEXT("message")) : FString(), FString(TEXT("Switched off for this session.")));
			}
			else if (!bTimedOut)
			{
				return false;
			}
			else
			{
				AddError(TEXT("No refused message reached the page."));
			}
			TestTrue(TEXT("Pong for the ping"), Page.bSawPong);
			if (Remote)
			{
				Remote->UnpairAll();
			}
			State->Step = 2;
			State->Deadline = FPlatformTime::Seconds() + 5.0;
			return false;
		case 2:
			if (Page.WaitForMessage(TEXT("bye"), Text, 0.02, []() {}))
			{
				Page.ReadUntilClosed(2.0);
				Page.WaitForMessage(TEXT("none"), Text, 0.05, []() {});
				TestTrue(TEXT("Unpaired: the page is closed"), Page.bClosed);
				TestEqual(TEXT("With a policy close"), static_cast<int32>(Page.LastCloseCode), static_cast<int32>(StageWebRemote::CloseCode::PolicyViolation));
			}
			else if (!bTimedOut)
			{
				return false;
			}
			else
			{
				AddError(TEXT("Unpairing did not close the page."));
			}
			{
				const uint16 Port = FLoopbackState::Port;
				const FString Bearer = FString::Printf(TEXT("Authorization: Bearer %s\r\n"), *State->Token);
				TestEqual(TEXT("The old token is dead"), Exchange(Port, MakeRequest(TEXT("POST"), TEXT("/api/command"), Port, TEXT("{\"path\":\"/stagecraft/cue/go\"}"), Bearer)).Status, 401);
				int32 LastStatus = 0;
				for (int32 Attempt = 0; Attempt < StageWebRemote::MaxWrongPins; ++Attempt)
				{
					LastStatus = Exchange(Port, MakeRequest(TEXT("POST"), TEXT("/api/pair"), Port, TEXT("{\"pin\":\"000000\"}"))).Status;
				}
				TestEqual(TEXT("The fifth wrong PIN locks pairing"), LastStatus, 429);
				const FHttpAnswer Locked = Exchange(Port, MakeRequest(TEXT("POST"), TEXT("/api/pair"), Port, TEXT("{\"pin\":\"246810\"}")));
				TestEqual(TEXT("Even the right PIN waits"), Locked.Status, 429);
				TestTrue(TEXT("Retry-After"), Locked.Head.Contains(TEXT("Retry-After: ")));
			}
			State->Step = 3;
			State->Deadline = FPlatformTime::Seconds() + 3.0;
			return false;
		default:
			if (Remote && Remote->GetStatus().PairingLockedUntil <= 0.0 && !bTimedOut)
			{
				return false;
			}
			TestTrue(TEXT("The panel hears about the lock"), Remote && Remote->GetStatus().PairingLockedUntil > 0.0);
			TestEqual(TEXT("No devices left"), Remote ? Remote->GetStatus().PairedDevices : -1, 0);
			TestEqual(TEXT("No pages left"), Remote ? Remote->GetStatus().ConnectedPages : -1, 0);
			if (Remote)
			{
				Remote->SetPinForTesting(FString());
			}
			State->Close();
			return true;
		}
	}));
	return true;
}

#endif
