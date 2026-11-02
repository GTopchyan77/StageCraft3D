// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Sockets.h"
#include "SocketSubsystem.h"

/**
 * Tests only: the other end of the DCC bridge (Docs/ADR/0022-dcc-bridge.md §5) as a loopback socket, standing in for an app's
 * add-on (Tools/DCCBridge/common/stagecraft_bridge_client.py). It speaks raw lines, so a test can also send what a well-behaved
 * add-on never would. Blocking reads with short waits; the server runs on its own thread.
 */
struct FStageBridgeTestClient
{
	FSocket* Socket = nullptr;
	/** Everything received and not yet taken as a line. */
	FString Received;

	~FStageBridgeTestClient()
	{
		Close();
	}

	bool Connect(uint16 Port)
	{
		ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		Socket = Sockets->CreateSocket(NAME_Stream, TEXT("StageBridgeTestClient"), false);
		return Socket && Socket->Connect(*FIPv4Endpoint(FIPv4Address(127, 0, 0, 1), Port).ToInternetAddr());
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

	void Send(const FString& Line)
	{
		const FTCHARToUTF8 Utf8(*(Line + TEXT("\n")));
		int32 Sent = 0;
		Socket->Send(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length(), Sent);
	}

	/** The add-on's greeting: AppKey is "blender", "maya", "max" or "cinema4d". */
	void SendHello(const FString& Token, const FString& AppKey, const FString& Version)
	{
		Send(FString::Printf(TEXT("{\"type\":\"hello\",\"protocol\":1,\"token\":\"%s\",\"app\":\"%s\",\"version\":\"%s\"}"), *Token, *AppKey, *Version));
	}

	void SendAck(const FString& Id, bool bOk, const FString& Error = FString())
	{
		Send(bOk ? FString::Printf(TEXT("{\"type\":\"ack\",\"id\":\"%s\",\"ok\":true}"), *Id)
			: FString::Printf(TEXT("{\"type\":\"ack\",\"id\":\"%s\",\"ok\":false,\"error\":\"%s\"}"), *Id, *Error));
	}

	/** Reads what has arrived within WaitSeconds. False when the server closed the connection. */
	bool ReadSome(double WaitSeconds)
	{
		if (!Socket || !Socket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromSeconds(WaitSeconds)))
		{
			return true;
		}
		uint8 Buffer[4096];
		int32 Read = 0;
		if (!Socket->Recv(Buffer, sizeof(Buffer), Read) || Read == 0)
		{
			return false;
		}
		const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Buffer), Read);
		Received += FString::ConstructFromPtrSize(Text.Get(), Text.Length());
		return true;
	}

	/** Reads until Needle arrived, the server closed the connection, or the time ran out. True when Needle arrived. */
	bool WaitFor(const FString& Needle, bool* bOutClosed = nullptr, double Seconds = 3.0)
	{
		const double Deadline = FPlatformTime::Seconds() + Seconds;
		while (FPlatformTime::Seconds() < Deadline && !Received.Contains(Needle))
		{
			if (!ReadSome(0.05))
			{
				if (bOutClosed)
				{
					*bOutClosed = true;
				}
				break;
			}
		}
		return Received.Contains(Needle);
	}

	/** Waits for the server to close the connection. */
	bool WaitClosed(double Seconds = 3.0)
	{
		bool bClosed = false;
		WaitFor(TEXT("\u0001never\u0001"), &bClosed, Seconds);
		return bClosed;
	}

	/**
	 * Takes the first complete line whose "type" is Type and parses it, dropping lines before it. Reads for up to Seconds; Pump
	 * runs between reads (the game thread must run for the server's game-thread side to act, e.g. to send an asset).
	 */
	TSharedPtr<FJsonObject> TakeMessage(const FString& Type, TFunctionRef<void()> Pump, double Seconds = 5.0)
	{
		const double Deadline = FPlatformTime::Seconds() + Seconds;
		while (true)
		{
			int32 NewLine = INDEX_NONE;
			while (Received.FindChar(TEXT('\n'), NewLine))
			{
				const FString Line = Received.Left(NewLine);
				Received.RightChopInline(NewLine + 1);
				TSharedPtr<FJsonObject> Object;
				FString MessageType;
				if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Line), Object) && Object.IsValid()
					&& Object->TryGetStringField(TEXT("type"), MessageType) && MessageType == Type)
				{
					return Object;
				}
			}
			if (FPlatformTime::Seconds() >= Deadline || !ReadSome(0.02))
			{
				return nullptr;
			}
			Pump();
		}
	}
};

#endif // WITH_DEV_AUTOMATION_TESTS
