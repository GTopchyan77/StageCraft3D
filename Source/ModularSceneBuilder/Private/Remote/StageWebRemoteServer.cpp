// Copyright Epic Games, Inc. All Rights Reserved.

#include "Remote/StageWebRemoteServer.h"

#include "Algo/Count.h"
#include "Common/TcpSocketBuilder.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Misc/FileHelper.h"
#include "ModularSceneBuilder.h"
#include "HAL/FileManager.h"
#include "Sockets.h"
#include "SocketSubsystem.h"

#include <atomic>

#define LOCTEXT_NAMESPACE "StageWebRemoteServer"

namespace StageWebRemoteServerImpl
{
	constexpr int32 ReadChunk = 16 * 1024;
	constexpr int32 MaxInboxBytes = 64 * 1024;
	constexpr int32 MaxChunksPerPass = 4;
	constexpr int32 MaxFramesPerPass = 64;
	constexpr int64 MaxPageBytes = 1024 * 1024;
	constexpr float BusySleepSeconds = 0.004f;
	const FTimespan IdleWait = FTimespan::FromMilliseconds(50.0);

	ISocketSubsystem* Sockets()
	{
		return ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	}
}

class FStageWebRemoteServer::FWorker : public FRunnable
{
public:
	FWorker(FSocket& InListener, FString InPageFile, TSharedRef<FShared, ESPMode::ThreadSafe> InShared)
		: Listener(InListener)
		, PageFile(MoveTemp(InPageFile))
		, Shared(MoveTemp(InShared))
	{
	}

	virtual uint32 Run() override
	{
		using namespace StageWebRemoteServerImpl;
		while (!bStopping.load())
		{
			bool bActive = AcceptConnections();
			bActive |= DistributeShared();
			for (int32 Index = Connections.Num() - 1; Index >= 0; --Index)
			{
				bActive |= Service(Connections[Index]);
				if (Connections[Index].bClosed)
				{
					CloseConnection(Index);
				}
			}
			if (!bActive)
			{
				if (Connections.IsEmpty())
				{
					Listener.Wait(ESocketWaitConditions::WaitForRead, IdleWait);
				}
				else
				{
					FPlatformProcess::SleepNoStats(BusySleepSeconds);
				}
			}
		}

		for (int32 Index = Connections.Num() - 1; Index >= 0; --Index)
		{
			FConnection& Connection = Connections[Index];
			if (Connection.Phase == EPhase::Open || Connection.Phase == EPhase::Hello)
			{
				Connection.SendBuffer.Append(StageWebRemote::EncodeClose(StageWebRemote::CloseCode::GoingAway, TEXT("StageCraft stopped the Web Remote")));
			}
			Flush(Connection);
			Connection.bClosed = true;
			CloseConnection(Index);
		}
		return 0;
	}

	virtual void Stop() override
	{
		bStopping.store(true);
	}

private:
	enum class EPhase : uint8
	{
		Http,
		Hello,
		Open,
		Closing,
	};

	struct FConnection
	{
		int32 Id = 0;
		FSocket* Socket = nullptr;
		EPhase Phase = EPhase::Http;
		double OpenedAt = 0.0;
		double LastHeard = 0.0;
		double LastPing = 0.0;
		StageWebRemote::FHttpRequestReader Reader;
		TArray<uint8> Inbox;
		TArray<uint8> SendBuffer;
		FString Token;
		uint64 StateSent = 0;
		uint64 CuesSent = 0;
		FString Client;
		double ClosingSince = 0.0;
		bool bCloseAfterFlush = false;
		bool bWriteShutdown = false;
		bool bClosed = false;
	};

	bool AcceptConnections()
	{
		bool bAccepted = false;
		bool bPending = false;
		while (Listener.HasPendingConnection(bPending) && bPending)
		{
			FSocket* Socket = Listener.Accept(TEXT("StageCraft Web Remote client"));
			if (!Socket)
			{
				break;
			}
			bAccepted = true;
			Socket->SetNonBlocking(true);
			Socket->SetNoDelay(true);
			const TSharedRef<FInternetAddr> Peer = StageWebRemoteServerImpl::Sockets()->CreateInternetAddr();
			Socket->GetPeerAddress(*Peer);
			const FString Client = Peer->ToString(false);
			const int32 Pending = Algo::CountIf(Connections, [&Client](const FConnection& Other)
			{
				return Other.Client == Client && (Other.Phase == EPhase::Http || Other.Phase == EPhase::Hello);
			});
			if (Connections.Num() >= StageWebRemote::MaxConnections || Pending >= StageWebRemote::MaxPendingPerClient)
			{
				const TArray<uint8> Busy = StageWebRemote::MakeErrorResponse(503, TEXT("Too many connections."));
				int32 Sent = 0;
				Socket->Send(Busy.GetData(), Busy.Num(), Sent);
				Socket->Close();
				StageWebRemoteServerImpl::Sockets()->DestroySocket(Socket);
				UE_LOG(LogStageCraft, Warning, TEXT("Web Remote: refused a connection (%d open, %d pending from that address)."), Connections.Num(), Pending);
				continue;
			}
			FConnection& Connection = Connections.AddDefaulted_GetRef();
			Connection.Id = ++LastConnectionId;
			Connection.Socket = Socket;
			Connection.Client = Client;
			Connection.OpenedAt = FPlatformTime::Seconds();
			Connection.LastHeard = Connection.OpenedAt;
		}
		return bAccepted;
	}

	bool DistributeShared()
	{
		FString State;
		uint64 StateVersion = 0;
		FString Cues;
		uint64 CuesVersion = 0;
		TArray<FString> Outgoing;
		bool bPairingChanged = false;
		{
			FScopeLock ScopeLock(&Shared->Lock);
			StateVersion = Shared->StateVersion;
			CuesVersion = Shared->CuesVersion;
			const bool bAnyOpen = Connections.ContainsByPredicate([](const FConnection& Connection) { return Connection.Phase == EPhase::Open; });
			if (bAnyOpen)
			{
				State = Shared->State;
				Cues = Shared->Cues;
			}
			Outgoing = MoveTemp(Shared->Outgoing);
			Shared->Outgoing.Reset();
			bPairingChanged = Shared->bPairingChanged;
			Shared->bPairingChanged = false;
		}

		bool bActive = false;
		for (FConnection& Connection : Connections)
		{
			if (Connection.Phase != EPhase::Open)
			{
				continue;
			}
			if (bPairingChanged && !IsPaired(Connection.Token))
			{
				SayByeAndClose(Connection, TEXT("This device was unpaired in StageCraft. Pair it again with the new PIN."));
				bActive = true;
				continue;
			}
			if (Connection.CuesSent != CuesVersion && !Cues.IsEmpty())
			{
				Connection.SendBuffer.Append(StageWebRemote::EncodeText(Cues));
				Connection.CuesSent = CuesVersion;
				bActive = true;
			}
			if (Connection.StateSent != StateVersion && !State.IsEmpty())
			{
				Connection.SendBuffer.Append(StageWebRemote::EncodeText(State));
				Connection.StateSent = StateVersion;
				bActive = true;
			}
			for (const FString& Message : Outgoing)
			{
				Connection.SendBuffer.Append(StageWebRemote::EncodeText(Message));
				bActive = true;
			}
		}
		return bActive;
	}

	bool Service(FConnection& Connection)
	{
		using namespace StageWebRemote;
		bool bActive = false;
		TArray<uint8> Buffer;
		Buffer.SetNumUninitialized(StageWebRemoteServerImpl::ReadChunk);
		for (int32 Chunk = 0; Chunk < StageWebRemoteServerImpl::MaxChunksPerPass && !Connection.bClosed; ++Chunk)
		{
			int32 Read = 0;
			if (!Connection.Socket->Recv(Buffer.GetData(), Buffer.Num(), Read))
			{
				Connection.bClosed = true;
				break;
			}
			if (Read <= 0)
			{
				break;
			}
			bActive = true;
			Connection.LastHeard = FPlatformTime::Seconds();
			const TConstArrayView<uint8> Bytes(Buffer.GetData(), Read);
			if (Connection.Phase == EPhase::Closing)
			{
				continue;
			}
			if (Connection.Phase == EPhase::Http)
			{
				ReadHttp(Connection, Bytes);
			}
			else if (Connection.Phase == EPhase::Hello || Connection.Phase == EPhase::Open)
			{
				Connection.Inbox.Append(Bytes.GetData(), Bytes.Num());
			}
		}
		if (!Connection.bClosed && (Connection.Phase == EPhase::Hello || Connection.Phase == EPhase::Open) && !Connection.Inbox.IsEmpty())
		{
			const int32 Before = Connection.Inbox.Num();
			ReadFrames(Connection);
			bActive |= Connection.Inbox.Num() != Before;
		}

		if (!Connection.bClosed && !Connection.bCloseAfterFlush)
		{
			CheckTimers(Connection);
		}
		if (!Connection.bClosed)
		{
			bActive |= Flush(Connection);
			if (Connection.SendBuffer.Num() > MaxPendingSendBytes)
			{
				UE_LOG(LogStageCraft, Warning, TEXT("Web Remote: connection %d stopped reading; closed."), Connection.Id);
				Connection.bClosed = true;
			}
			else if (Connection.bCloseAfterFlush && FPlatformTime::Seconds() - Connection.ClosingSince > ClosingTimeoutSeconds)
			{
				Connection.bClosed = true;
			}
			else if (Connection.bCloseAfterFlush && Connection.SendBuffer.IsEmpty() && !Connection.bWriteShutdown)
			{
				Connection.Socket->Shutdown(ESocketShutdownMode::Write);
				Connection.bWriteShutdown = true;
			}
		}
		return bActive;
	}

	void CheckTimers(FConnection& Connection)
	{
		using namespace StageWebRemote;
		const double Now = FPlatformTime::Seconds();
		switch (Connection.Phase)
		{
		case EPhase::Http:
			if (Now - Connection.OpenedAt > RequestTimeoutSeconds)
			{
				Respond(Connection, MakeErrorResponse(408, TEXT("The request took too long.")));
			}
			break;
		case EPhase::Hello:
			if (Now - Connection.OpenedAt > HelloTimeoutSeconds)
			{
				SayByeAndClose(Connection, TEXT("No hello: send {\"type\":\"hello\",\"token\":\"...\"} first."));
			}
			break;
		case EPhase::Open:
			if (Now - Connection.LastHeard > IdleTimeoutSeconds)
			{
				CloseWebSocket(Connection, CloseCode::GoingAway, TEXT("No answer for a minute."));
			}
			else if (Now - Connection.LastPing > PingIntervalSeconds)
			{
				Connection.SendBuffer.Append(EncodeFrame(EOpcode::Ping, {}));
				Connection.LastPing = Now;
			}
			break;
		default:
			break;
		}
	}

	void ReadHttp(FConnection& Connection, TConstArrayView<uint8> Bytes)
	{
		using namespace StageWebRemote;
		switch (Connection.Reader.Append(Bytes))
		{
		case EReadResult::NeedMore:
			break;
		case EReadResult::Error:
			Respond(Connection, MakeErrorResponse(Connection.Reader.GetErrorStatus(), Connection.Reader.GetError()));
			break;
		case EReadResult::Complete:
			HandleRequest(Connection, Connection.Reader.GetRequest());
			break;
		}
	}

	void HandleRequest(FConnection& Connection, const StageWebRemote::FHttpRequest& Request)
	{
		using namespace StageWebRemote;
		const FString Host = Request.GetHeader(TEXT("host"));
		if (!IsAllowedHost(Host))
		{
			Respond(Connection, MakeErrorResponse(421, TEXT("Open the Web Remote by this PC's IP address, as StageCraft's Preferences show it.")));
			return;
		}
		if (!IsSameOrigin(Request.GetHeader(TEXT("origin")), Host))
		{
			Respond(Connection, MakeErrorResponse(403, TEXT("Requests from other sites are refused.")));
			return;
		}

		switch (Route(Request))
		{
		case ERoute::Page:
			ServePage(Connection);
			return;
		case ERoute::Pair:
			HandlePair(Connection, Request);
			return;
		case ERoute::Command:
			HandleCommand(Connection, Request);
			return;
		case ERoute::State:
			HandleState(Connection, Request);
			return;
		case ERoute::WebSocket:
			HandleUpgrade(Connection, Request);
			return;
		case ERoute::MethodNotAllowed:
			Respond(Connection, MakeErrorResponse(405, TEXT("That method is not allowed here.")));
			return;
		case ERoute::NotFound:
		default:
			Respond(Connection, MakeErrorResponse(404, TEXT("Not found.")));
			return;
		}
	}

	void ServePage(FConnection& Connection)
	{
		using namespace StageWebRemote;
		if (!bPageLoaded)
		{
			bPageLoaded = true;
			const int64 Size = IFileManager::Get().FileSize(*PageFile);
			if (Size > 0 && Size <= StageWebRemoteServerImpl::MaxPageBytes && FFileHelper::LoadFileToArray(Page, *PageFile, FILEREAD_Silent))
			{
				UE_LOG(LogStageCraft, Log, TEXT("Web Remote: page loaded (%d bytes)."), Page.Num());
			}
			else
			{
				Page.Reset();
				UE_LOG(LogStageCraft, Error, TEXT("Web Remote: the page %s is missing."), *PageFile);
			}
		}
		if (Page.IsEmpty())
		{
			Respond(Connection, MakeErrorResponse(500, TEXT("The remote page is missing from this installation.")));
			return;
		}
		Respond(Connection, MakePageResponse(Page));
	}

	void HandlePair(FConnection& Connection, const StageWebRemote::FHttpRequest& Request)
	{
		using namespace StageWebRemote;
		FString Pin;
		FString Name;
		FString Error;
		if (!ParsePairBody(Request.Body, Pin, Name, Error))
		{
			Respond(Connection, MakeErrorResponse(400, Error));
			return;
		}

		FString Token;
		double RetryAfter = 0.0;
		FPairing::EResult Result = FPairing::EResult::WrongPin;
		double LockedBefore = 0.0;
		double LockedAfter = 0.0;
		{
			FScopeLock ScopeLock(&Shared->Lock);
			LockedBefore = Shared->Pairing.GetLockedUntil();
			bool bEvicted = false;
			Result = Shared->Pairing.Pair(Pin, Name, Connection.Client, FPlatformTime::Seconds(), Token, RetryAfter, bEvicted);
			LockedAfter = Shared->Pairing.GetLockedUntil();
			Shared->bPairingChanged |= bEvicted;
		}

		switch (Result)
		{
		case FPairing::EResult::Paired:
		{
			Respond(Connection, MakeJsonResponse(200, MakeTokenJson(Token)));
			FStageWebRemoteEvent Event;
			Event.Kind = FStageWebRemoteEvent::EKind::Paired;
			Event.DeviceName = Name;
			Post(MoveTemp(Event));
			UE_LOG(LogStageCraft, Log, TEXT("Web Remote: \"%s\" paired."), *Name);
			return;
		}
		case FPairing::EResult::WrongPin:
			Respond(Connection, MakeErrorResponse(403, TEXT("Wrong PIN. It is shown in StageCraft under Preferences > Web Remote.")));
			UE_LOG(LogStageCraft, Warning, TEXT("Web Remote: a wrong PIN was entered (connection %d)."), Connection.Id);
			return;
		case FPairing::EResult::Locked:
		default:
		{
			const int32 Seconds = FMath::Max(1, FMath::CeilToInt(RetryAfter));
			const FString RetryHeader = FString::Printf(TEXT("Retry-After: %d"), Seconds);
			Respond(Connection, MakeErrorResponse(429,
				FString::Printf(TEXT("Too many wrong PINs from this device. Check the PIN in StageCraft and try again in %d s."), Seconds), MakeArrayView(&RetryHeader, 1)));
			if (LockedAfter != LockedBefore)
			{
				FStageWebRemoteEvent Event;
				Event.Kind = FStageWebRemoteEvent::EKind::PairingLocked;
				Post(MoveTemp(Event));
				UE_LOG(LogStageCraft, Warning, TEXT("Web Remote: too many wrong PINs from %s; it waits %d s."), *Connection.Client, Seconds);
			}
			return;
		}
		}
	}

	bool IsAuthorized(const StageWebRemote::FHttpRequest& Request) const
	{
		return IsPaired(StageWebRemote::ParseBearer(Request.GetHeader(TEXT("authorization"))));
	}

	void RespondUnauthorized(FConnection& Connection)
	{
		using namespace StageWebRemote;
		static const FString Challenge = TEXT("WWW-Authenticate: Bearer");
		Respond(Connection, MakeErrorResponse(401, TEXT("Pair this device first (POST /api/pair), then send its token."), MakeArrayView(&Challenge, 1)));
	}

	void HandleCommand(FConnection& Connection, const StageWebRemote::FHttpRequest& Request)
	{
		using namespace StageWebRemote;
		if (!IsAuthorized(Request))
		{
			RespondUnauthorized(Connection);
			return;
		}
		FString Path;
		TOptional<float> Value;
		FString Error;
		if (!ParseCommandBody(Request.Body, Path, Value, Error))
		{
			Respond(Connection, MakeErrorResponse(400, Error));
			return;
		}
		PostCommand(Path, Value);
		Respond(Connection, MakeJsonResponse(202, MakeOkJson()));
	}

	void HandleState(FConnection& Connection, const StageWebRemote::FHttpRequest& Request)
	{
		using namespace StageWebRemote;
		if (!IsAuthorized(Request))
		{
			RespondUnauthorized(Connection);
			return;
		}
		FString State;
		{
			FScopeLock ScopeLock(&Shared->Lock);
			State = Shared->State;
		}
		if (State.IsEmpty())
		{
			Respond(Connection, MakeErrorResponse(503, TEXT("The stage is not ready yet.")));
			return;
		}
		Respond(Connection, MakeJsonResponse(200, State));
	}

	void HandleUpgrade(FConnection& Connection, const StageWebRemote::FHttpRequest& Request)
	{
		using namespace StageWebRemote;
		const int32 Sockets = Algo::CountIf(Connections, [](const FConnection& Other) { return Other.Phase == EPhase::Open; });
		if (Sockets >= MaxWebSockets)
		{
			Respond(Connection, MakeErrorResponse(503, TEXT("Too many remote pages are open.")));
			return;
		}
		FString AcceptKey;
		switch (CheckUpgrade(Request, AcceptKey))
		{
		case EUpgradeResult::WrongVersion:
		{
			static const FString VersionHeader = TEXT("Sec-WebSocket-Version: 13");
			Respond(Connection, MakeErrorResponse(426, TEXT("Only WebSocket version 13 is supported."), MakeArrayView(&VersionHeader, 1)));
			return;
		}
		case EUpgradeResult::BadRequest:
			Respond(Connection, MakeErrorResponse(400, TEXT("Not a WebSocket handshake.")));
			return;
		case EUpgradeResult::Ok:
		default:
			break;
		}
		Connection.SendBuffer.Append(MakeUpgradeResponse(AcceptKey));
		Connection.Phase = EPhase::Hello;
		Connection.OpenedAt = FPlatformTime::Seconds();
		Connection.Inbox = Connection.Reader.TakeRemainder();
		if (Connection.Inbox.Num() > 0)
		{
			ReadFrames(Connection);
		}
	}

	void ReadFrames(FConnection& Connection)
	{
		using namespace StageWebRemote;
		for (int32 Handled = 0; Handled < StageWebRemoteServerImpl::MaxFramesPerPass && !Connection.bCloseAfterFlush && !Connection.bClosed; ++Handled)
		{
			FWebSocketFrame Frame;
			int32 Consumed = 0;
			uint16 CloseCodeValue = CloseCode::ProtocolError;
			const EFrameResult Result = DecodeClientFrame(Connection.Inbox, Frame, Consumed, CloseCodeValue);
			if (Result == EFrameResult::NeedMore)
			{
				if (Connection.Inbox.Num() > StageWebRemoteServerImpl::MaxInboxBytes)
				{
					CloseWebSocket(Connection, CloseCode::TooBig, TEXT("Too much data."));
				}
				return;
			}
			if (Result == EFrameResult::Error)
			{
				CloseWebSocket(Connection, CloseCodeValue, CloseCodeValue == CloseCode::TooBig ? TEXT("Message too large.")
					: CloseCodeValue == CloseCode::Unsupported ? TEXT("Only unfragmented text messages are supported.") : TEXT("Protocol error."));
				return;
			}
			Connection.Inbox.RemoveAt(0, Consumed, EAllowShrinking::No);
			HandleFrame(Connection, Frame);
		}
		if (!Connection.bCloseAfterFlush && !Connection.bClosed && Connection.Inbox.Num() > StageWebRemoteServerImpl::MaxInboxBytes)
		{
			CloseWebSocket(Connection, CloseCode::TooBig, TEXT("Too much data."));
		}
	}

	void HandleFrame(FConnection& Connection, const StageWebRemote::FWebSocketFrame& Frame)
	{
		using namespace StageWebRemote;
		switch (Frame.Opcode)
		{
		case EOpcode::Ping:
			Connection.SendBuffer.Append(EncodeFrame(EOpcode::Pong, Frame.Payload));
			return;
		case EOpcode::Pong:
			return;
		case EOpcode::Close:
			CloseWebSocket(Connection, CloseCode::Normal, FString());
			return;
		case EOpcode::Text:
			break;
		default:
			CloseWebSocket(Connection, CloseCode::Unsupported, TEXT("Only text messages are supported."));
			return;
		}

		if (!IsValidUtf8(Frame.Payload))
		{
			CloseWebSocket(Connection, CloseCode::InvalidData, TEXT("Messages must be UTF-8."));
			return;
		}
		FClientMessage Message;
		FString Error;
		if (!ParseClientMessage(Utf8ToString(Frame.Payload), Message, Error))
		{
			CloseWebSocket(Connection, CloseCode::InvalidData, Error);
			return;
		}

		if (Connection.Phase == EPhase::Hello)
		{
			if (Message.Type != FClientMessage::EType::Hello)
			{
				SayByeAndClose(Connection, TEXT("Say hello first."));
				return;
			}
			if (!IsPaired(Message.Token))
			{
				UE_LOG(LogStageCraft, Warning, TEXT("Web Remote: connection %d presented a token that is not paired; closed."), Connection.Id);
				SayByeAndClose(Connection, TEXT("This device is not paired with StageCraft (it restarted or unpaired it). Pair it again."));
				return;
			}
			Connection.Phase = EPhase::Open;
			Connection.Token = Message.Token;
			Connection.LastPing = FPlatformTime::Seconds();
			Connection.StateSent = 0;
			Connection.CuesSent = 0;
			Connection.SendBuffer.Append(EncodeText(MakeWelcomeMessage()));
			PostPagesChanged();
			return;
		}

		if (Message.Type != FClientMessage::EType::Command)
		{
			SayByeAndClose(Connection, TEXT("Already said hello."));
			return;
		}
		PostCommand(Message.Path, Message.Value);
	}

	void Respond(FConnection& Connection, TArray<uint8>&& Response)
	{
		Connection.SendBuffer.Append(MoveTemp(Response));
		Connection.ClosingSince = FPlatformTime::Seconds();
		Connection.bCloseAfterFlush = true;
		Connection.Phase = EPhase::Closing;
	}

	void SayByeAndClose(FConnection& Connection, const FString& Reason)
	{
		Connection.SendBuffer.Append(StageWebRemote::EncodeText(StageWebRemote::MakeByeMessage(Reason)));
		CloseWebSocket(Connection, StageWebRemote::CloseCode::PolicyViolation, Reason);
	}

	void CloseWebSocket(FConnection& Connection, uint16 Code, const FString& Reason)
	{
		const bool bWasOpen = Connection.Phase == EPhase::Open;
		Connection.SendBuffer.Append(StageWebRemote::EncodeClose(Code, Reason));
		Connection.ClosingSince = FPlatformTime::Seconds();
		Connection.bCloseAfterFlush = true;
		Connection.Phase = EPhase::Closing;
		if (bWasOpen)
		{
			PostPagesChanged();
		}
	}

	bool Flush(FConnection& Connection)
	{
		if (Connection.SendBuffer.IsEmpty())
		{
			return false;
		}
		int32 Sent = 0;
		if (!Connection.Socket->Send(Connection.SendBuffer.GetData(), Connection.SendBuffer.Num(), Sent))
		{
			if (StageWebRemoteServerImpl::Sockets()->GetLastErrorCode() != SE_EWOULDBLOCK)
			{
				Connection.bClosed = true;
			}
			return false;
		}
		Connection.SendBuffer.RemoveAt(0, Sent, EAllowShrinking::No);
		return Sent > 0;
	}

	void CloseConnection(int32 Index)
	{
		FConnection& Connection = Connections[Index];
		const bool bWasOpen = Connection.Phase == EPhase::Open;
		Connection.Socket->Close();
		StageWebRemoteServerImpl::Sockets()->DestroySocket(Connection.Socket);
		Connections.RemoveAt(Index);
		if (bWasOpen)
		{
			PostPagesChanged();
		}
	}

	bool IsPaired(const FString& Token) const
	{
		FScopeLock ScopeLock(&Shared->Lock);
		return Shared->Pairing.IsPaired(Token);
	}

	void PostCommand(const FString& Path, TOptional<float> Value)
	{
		FStageWebRemoteEvent Event;
		Event.Kind = FStageWebRemoteEvent::EKind::Command;
		Event.Path = Path;
		Event.Value = Value;
		Post(MoveTemp(Event));
	}

	void PostPagesChanged()
	{
		FStageWebRemoteEvent Event;
		Event.Kind = FStageWebRemoteEvent::EKind::PagesChanged;
		Event.Pages = Algo::CountIf(Connections, [](const FConnection& Connection) { return Connection.Phase == EPhase::Open; });
		Post(MoveTemp(Event));
	}

	void Post(FStageWebRemoteEvent&& Event)
	{
		bool bSchedule = false;
		{
			FScopeLock ScopeLock(&Shared->Lock);
			if (Shared->Events.Num() >= StageWebRemote::MaxQueuedEvents)
			{
				++Shared->DroppedEvents;
				return;
			}
			Shared->Events.Add(MoveTemp(Event));
			bSchedule = !Shared->bDrainScheduled;
			Shared->bDrainScheduled = true;
		}
		if (bSchedule && Shared->ScheduleDrain)
		{
			Shared->ScheduleDrain();
		}
	}

	FSocket& Listener;
	const FString PageFile;
	const TSharedRef<FShared, ESPMode::ThreadSafe> Shared;
	std::atomic<bool> bStopping{ false };
	TArray<FConnection> Connections;
	int32 LastConnectionId = 0;
	TArray<uint8> Page;
	bool bPageLoaded = false;
};

FStageWebRemoteServer::FStageWebRemoteServer() = default;

FStageWebRemoteServer::~FStageWebRemoteServer()
{
	Stop();
}

bool FStageWebRemoteServer::Start(bool bLoopbackOnly, uint16 Port, const FString& PageFile, TFunction<void()> ScheduleDrain, FText& OutError,
	TFunction<FString()> MakePin)
{
	Stop();

	const FIPv4Address Address = bLoopbackOnly ? FIPv4Address(127, 0, 0, 1) : FIPv4Address::Any;
	Listener = FTcpSocketBuilder(TEXT("StageCraft Web Remote"))
		.AsNonBlocking()
		.AsReusable(false)
		.BoundToEndpoint(FIPv4Endpoint(Address, Port))
		.Listening(StageWebRemote::MaxConnections)
		.Build();
	if (!Listener)
	{
		OutError = FText::Format(LOCTEXT("PortInUse", "The Web Remote could not listen on port {0}; another program may be using it."),
			FText::AsNumber(Port, &FNumberFormattingOptions::DefaultNoGrouping()));
		return false;
	}
	BoundPort = static_cast<uint16>(Listener->GetPortNo());

	Shared = MakeShared<FShared, ESPMode::ThreadSafe>(MoveTemp(MakePin));
	Shared->ScheduleDrain = MoveTemp(ScheduleDrain);
	Worker = MakeUnique<FWorker>(*Listener, PageFile, Shared.ToSharedRef());
	Thread.Reset(FRunnableThread::Create(Worker.Get(), TEXT("StageCraftWebRemote"), 0, TPri_BelowNormal));
	if (!Thread.IsValid())
	{
		OutError = LOCTEXT("NoThread", "Could not start the Web Remote thread.");
		Stop();
		return false;
	}
	return true;
}

void FStageWebRemoteServer::Stop()
{
	if (Thread.IsValid())
	{
		Thread->Kill(true);
		Thread.Reset();
	}
	Worker.Reset();
	if (Listener)
	{
		Listener->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Listener);
		Listener = nullptr;
	}
	Shared.Reset();
	BoundPort = 0;
}

FString FStageWebRemoteServer::GetPin() const
{
	if (!Shared.IsValid())
	{
		return FString();
	}
	FScopeLock ScopeLock(&Shared->Lock);
	return Shared->Pairing.GetPin();
}

int32 FStageWebRemoteServer::GetPairedCount() const
{
	if (!Shared.IsValid())
	{
		return 0;
	}
	FScopeLock ScopeLock(&Shared->Lock);
	return Shared->Pairing.GetPairedCount();
}

double FStageWebRemoteServer::GetPairingLockedUntil() const
{
	if (!Shared.IsValid())
	{
		return 0.0;
	}
	FScopeLock ScopeLock(&Shared->Lock);
	return Shared->Pairing.GetLockedUntil();
}

void FStageWebRemoteServer::RenewPin()
{
	if (Shared.IsValid())
	{
		FScopeLock ScopeLock(&Shared->Lock);
		Shared->Pairing.RenewPin();
	}
}

void FStageWebRemoteServer::UnpairAll()
{
	if (Shared.IsValid())
	{
		FScopeLock ScopeLock(&Shared->Lock);
		Shared->Pairing.UnpairAll();
		Shared->bPairingChanged = true;
	}
}

void FStageWebRemoteServer::SetState(const FString& Message)
{
	if (Shared.IsValid())
	{
		FScopeLock ScopeLock(&Shared->Lock);
		if (Shared->State != Message)
		{
			Shared->State = Message;
			++Shared->StateVersion;
		}
	}
}

void FStageWebRemoteServer::SetCues(const FString& Message)
{
	if (Shared.IsValid())
	{
		FScopeLock ScopeLock(&Shared->Lock);
		if (Shared->Cues != Message)
		{
			Shared->Cues = Message;
			++Shared->CuesVersion;
		}
	}
}

void FStageWebRemoteServer::Broadcast(const FString& Message)
{
	if (Shared.IsValid())
	{
		FScopeLock ScopeLock(&Shared->Lock);
		Shared->Outgoing.Add(Message);
	}
}

int32 FStageWebRemoteServer::Drain(TFunctionRef<void(const FStageWebRemoteEvent&)> Visit)
{
	if (!Shared.IsValid())
	{
		return 0;
	}
	TArray<FStageWebRemoteEvent> Events;
	int32 Dropped = 0;
	{
		FScopeLock ScopeLock(&Shared->Lock);
		Dropped = Shared->DroppedEvents;
		Shared->DroppedEvents = 0;
		Events = MoveTemp(Shared->Events);
		Shared->Events.Reset();
		Shared->bDrainScheduled = false;
	}
	for (const FStageWebRemoteEvent& Event : Events)
	{
		Visit(Event);
	}
	return Dropped;
}

#undef LOCTEXT_NAMESPACE
