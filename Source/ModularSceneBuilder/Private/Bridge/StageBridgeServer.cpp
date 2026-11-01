// Copyright Epic Games, Inc. All Rights Reserved.

#include "Bridge/StageBridgeServer.h"

#include "Common/TcpSocketBuilder.h"
#include "HAL/PlatformProcess.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "ModularSceneBuilder.h"
#include "Sockets.h"
#include "SocketSubsystem.h"

#include <atomic>

#define LOCTEXT_NAMESPACE "StageBridgeServer"

namespace StageBridgeServerImpl
{
	constexpr int32 ReadChunk = 16 * 1024;
	const FTimespan IdleWait = FTimespan::FromMilliseconds(50.0);

	TArray<uint8> ToUtf8(const FString& Line)
	{
		const FTCHARToUTF8 Converted(*Line, Line.Len());
		return TArray<uint8>(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
	}
}

class FStageBridgeServer::FWorker : public FRunnable
{
public:
	FWorker(FSocket& InListener, FString InToken, TSharedRef<FShared, ESPMode::ThreadSafe> InShared)
		: Listener(InListener)
		, Token(MoveTemp(InToken))
		, Shared(MoveTemp(InShared))
	{
	}

	//~ Begin FRunnable Interface
	virtual uint32 Run() override
	{
		while (!bStopping.load())
		{
			bool bActive = AcceptClients();
			bActive |= TakeOutgoing();
			for (int32 Index = Clients.Num() - 1; Index >= 0; --Index)
			{
				bActive |= ServiceClient(Clients[Index]);
				if (Clients[Index].bClosed)
				{
					CloseClient(Index);
				}
			}
			if (!bActive)
			{
				Listener.Wait(ESocketWaitConditions::WaitForRead, StageBridgeServerImpl::IdleWait);
			}
		}

		for (int32 Index = Clients.Num() - 1; Index >= 0; --Index)
		{
			SayByeAndClose(Clients[Index], TEXT("StageCraft stopped the bridge"));
			CloseClient(Index);
		}
		return 0;
	}

	virtual void Stop() override
	{
		bStopping.store(true);
	}
	//~ End FRunnable Interface

private:
	struct FClient
	{
		int32 Id = 0;
		FSocket* Socket = nullptr;
		double ConnectedAt = 0.0;
		bool bAuthenticated = false;
		bool bClosed = false;
		FString CloseReason;
		StageBridgeProtocol::FLineReader Reader;
		TArray<uint8> SendBuffer;
	};

	bool AcceptClients()
	{
		bool bAccepted = false;
		bool bPending = false;
		while (Listener.HasPendingConnection(bPending) && bPending)
		{
			FSocket* Socket = Listener.Accept(TEXT("StageCraft Bridge client"));
			if (!Socket)
			{
				break;
			}
			bAccepted = true;
			Socket->SetNonBlocking(true);
			Socket->SetNoDelay(true);
			FClient Client;
			Client.Id = ++LastClientId;
			Client.Socket = Socket;
			Client.ConnectedAt = FPlatformTime::Seconds();
			if (Clients.Num() >= StageBridgeProtocol::MaxClients)
			{
				SayByeAndClose(Client, TEXT("too many connections"));
				ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
				UE_LOG(LogStageCraft, Warning, TEXT("Bridge: refused a connection (already %d)."), Clients.Num());
				continue;
			}
			Clients.Add(MoveTemp(Client));
		}
		return bAccepted;
	}

	/** Moves lines the game thread queued into the clients' send buffers. */
	bool TakeOutgoing()
	{
		TArray<TPair<int32, FString>> Outgoing;
		{
			FScopeLock ScopeLock(&Shared->Lock);
			Outgoing = MoveTemp(Shared->Outgoing);
			Shared->Outgoing.Reset();
		}
		for (const TPair<int32, FString>& Line : Outgoing)
		{
			if (FClient* Client = Clients.FindByPredicate([&Line](const FClient& Candidate) { return Candidate.Id == Line.Key && Candidate.bAuthenticated; }))
			{
				Client->SendBuffer.Append(StageBridgeServerImpl::ToUtf8(Line.Value));
			}
		}
		return !Outgoing.IsEmpty();
	}

	/** Reads, handles and writes for one client. True when anything moved. */
	bool ServiceClient(FClient& Client)
	{
		bool bActive = false;
		TArray<uint8> Buffer;
		Buffer.SetNumUninitialized(StageBridgeServerImpl::ReadChunk);
		while (!Client.bClosed)
		{
			int32 Read = 0;
			if (!Client.Socket->Recv(Buffer.GetData(), Buffer.Num(), Read))
			{
				// For a stream socket, false is an orderly close or an error; "would block" comes back as true with 0 bytes.
				MarkClosed(Client, TEXT("the app closed the connection"));
				break;
			}
			if (Read <= 0)
			{
				break;
			}
			bActive = true;
			TArray<FString> Lines;
			if (!Client.Reader.Append(TConstArrayView<uint8>(Buffer.GetData(), Read), Lines))
			{
				SayByeAndClose(Client, TEXT("message too long"));
				break;
			}
			for (const FString& Line : Lines)
			{
				HandleLine(Client, Line);
				if (Client.bClosed)
				{
					break;
				}
			}
		}

		if (!Client.bClosed && !Client.bAuthenticated && FPlatformTime::Seconds() - Client.ConnectedAt > StageBridgeProtocol::HelloTimeoutSeconds)
		{
			SayByeAndClose(Client, TEXT("no hello"));
		}
		if (!Client.bClosed)
		{
			bActive |= Flush(Client);
		}
		return bActive;
	}

	void HandleLine(FClient& Client, const FString& Line)
	{
		using namespace StageBridgeProtocol;
		FClientMessage Message;
		FString Error;
		if (!ParseClientMessage(Line, Message, Error))
		{
			SayByeAndClose(Client, Error);
			return;
		}

		if (!Client.bAuthenticated)
		{
			if (Message.Type != EClientMessage::Hello)
			{
				SayByeAndClose(Client, TEXT("say hello first"));
				return;
			}
			if (Message.Hello.Protocol != StageBridgeProtocol::Version)
			{
				SayByeAndClose(Client, FString::Printf(TEXT("protocol %d is not supported; StageCraft speaks %d"), Message.Hello.Protocol, StageBridgeProtocol::Version));
				return;
			}
			if (!IsTokenMatch(Token, Message.Hello.Token))
			{
				// Logged without the presented value: it may be a stale token of the same user, or a probe.
				UE_LOG(LogStageCraft, Warning, TEXT("Bridge: client %d presented a wrong token; closed."), Client.Id);
				SayByeAndClose(Client, TEXT("wrong token: restart the add-on's connection"));
				return;
			}
			Client.bAuthenticated = true;
			Client.SendBuffer.Append(StageBridgeServerImpl::ToUtf8(MakeWelcome(Client.Id)));

			FStageBridgeEvent Event;
			Event.Kind = FStageBridgeEvent::EKind::Connected;
			Event.ClientId = Client.Id;
			Event.Client.ClientId = Client.Id;
			Event.Client.App = Message.Hello.App;
			Event.Client.AppVersion = Message.Hello.AppVersion;
			Event.Client.bImportsGlb = ImportsGlb(Message.Hello.App);
			Post(MoveTemp(Event));
			return;
		}

		if (Message.Type != EClientMessage::Ack)
		{
			SayByeAndClose(Client, TEXT("already said hello"));
			return;
		}
		FStageBridgeEvent Event;
		Event.Kind = FStageBridgeEvent::EKind::Ack;
		Event.ClientId = Client.Id;
		Event.Ack = MoveTemp(Message.Ack);
		Post(MoveTemp(Event));
	}

	/** Writes what the socket takes now; the rest stays for the next pass. */
	bool Flush(FClient& Client)
	{
		if (Client.SendBuffer.IsEmpty())
		{
			return false;
		}
		int32 Sent = 0;
		if (!Client.Socket->Send(Client.SendBuffer.GetData(), Client.SendBuffer.Num(), Sent))
		{
			if (ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->GetLastErrorCode() != SE_EWOULDBLOCK)
			{
				MarkClosed(Client, TEXT("the connection broke"));
			}
			return false;
		}
		Client.SendBuffer.RemoveAt(0, Sent, EAllowShrinking::No);
		return Sent > 0;
	}

	/** Best effort: a client that broke the protocol is told why before it is closed. */
	void SayByeAndClose(FClient& Client, const FString& Reason)
	{
		Client.SendBuffer.Append(StageBridgeServerImpl::ToUtf8(StageBridgeProtocol::MakeBye(Reason)));
		int32 Sent = 0;
		Client.Socket->Send(Client.SendBuffer.GetData(), Client.SendBuffer.Num(), Sent);
		Client.SendBuffer.Reset();
		MarkClosed(Client, Reason);
	}

	void MarkClosed(FClient& Client, const FString& Reason)
	{
		Client.bClosed = true;
		Client.CloseReason = Reason;
	}

	void CloseClient(int32 Index)
	{
		FClient& Client = Clients[Index];
		if (Client.bAuthenticated)
		{
			FStageBridgeEvent Event;
			Event.Kind = FStageBridgeEvent::EKind::Disconnected;
			Event.ClientId = Client.Id;
			Event.Reason = Client.CloseReason;
			Post(MoveTemp(Event));
		}
		else
		{
			UE_LOG(LogStageCraft, Log, TEXT("Bridge: unauthenticated connection %d closed: %s."), Client.Id, *Client.CloseReason);
		}
		Client.Socket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Client.Socket);
		Clients.RemoveAt(Index);
	}

	void Post(FStageBridgeEvent&& Event)
	{
		bool bSchedule = false;
		{
			FScopeLock ScopeLock(&Shared->Lock);
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
	const FString Token;
	const TSharedRef<FShared, ESPMode::ThreadSafe> Shared;
	std::atomic<bool> bStopping{ false };
	TArray<FClient> Clients;
	int32 LastClientId = 0;
};

FStageBridgeServer::FStageBridgeServer() = default;

FStageBridgeServer::~FStageBridgeServer()
{
	Stop();
}

bool FStageBridgeServer::Start(uint16 Port, const FString& Token, TFunction<void()> ScheduleDrain, FText& OutError)
{
	Stop();

	// Loopback only: no other machine can reach the bridge, whatever the firewall says.
	Listener = FTcpSocketBuilder(TEXT("StageCraft Bridge"))
		.AsNonBlocking()
		.AsReusable(false)
		.BoundToEndpoint(FIPv4Endpoint(FIPv4Address(127, 0, 0, 1), Port))
		.Listening(StageBridgeProtocol::MaxClients)
		.Build();
	if (!Listener)
	{
		OutError = FText::Format(LOCTEXT("PortInUse", "Could not listen on port {0}; another program may be using it."), FText::AsNumber(Port, &FNumberFormattingOptions::DefaultNoGrouping()));
		return false;
	}
	BoundPort = static_cast<uint16>(Listener->GetPortNo());

	Shared = MakeShared<FShared, ESPMode::ThreadSafe>();
	Shared->ScheduleDrain = MoveTemp(ScheduleDrain);
	Worker = MakeUnique<FWorker>(*Listener, Token, Shared.ToSharedRef());
	Thread.Reset(FRunnableThread::Create(Worker.Get(), TEXT("StageCraftBridge"), 0, TPri_BelowNormal));
	if (!Thread.IsValid())
	{
		OutError = LOCTEXT("NoThread", "Could not start the bridge thread.");
		Stop();
		return false;
	}
	return true;
}

void FStageBridgeServer::Stop()
{
	if (Thread.IsValid())
	{
		// Kill(true) calls FWorker::Stop and waits for Run to return; the worker closes its clients on the way out.
		Thread->Kill(/*bShouldWait*/ true);
		Thread.Reset();
	}
	Worker.Reset();
	if (Listener)
	{
		Listener->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Listener);
		Listener = nullptr;
	}
	// Events the worker posted while stopping are dropped with it: the owner resets its client list on stop.
	Shared.Reset();
	BoundPort = 0;
}

void FStageBridgeServer::Send(int32 ClientId, const FString& Line)
{
	if (Shared.IsValid())
	{
		FScopeLock ScopeLock(&Shared->Lock);
		Shared->Outgoing.Emplace(ClientId, Line);
	}
}

void FStageBridgeServer::Drain(TFunctionRef<void(const FStageBridgeEvent&)> Visit)
{
	if (!Shared.IsValid())
	{
		return;
	}
	TArray<FStageBridgeEvent> Events;
	{
		FScopeLock ScopeLock(&Shared->Lock);
		Events = MoveTemp(Shared->Events);
		Shared->Events.Reset();
		Shared->bDrainScheduled = false;
	}
	for (const FStageBridgeEvent& Event : Events)
	{
		Visit(Event);
	}
}

#undef LOCTEXT_NAMESPACE
