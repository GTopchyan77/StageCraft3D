// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Bridge/StageBridgeProtocol.h"
#include "Bridge/StageBridgeTypes.h"
#include "Templates/UniquePtr.h"

/** What the socket thread hands the game thread. Only authenticated clients ever produce events. */
struct FStageBridgeEvent
{
	enum class EKind : uint8
	{
		/** A client presented the token; Client describes it. */
		Connected,
		/** A connected client answered an asset message. */
		Ack,
		/** A connected client went away or was closed; Reason says why. */
		Disconnected,
	};

	EKind Kind = EKind::Connected;
	int32 ClientId = 0;
	FStageBridgeClientInfo Client;
	StageBridgeProtocol::FAck Ack;
	FString Reason;
};

/**
 * The bridge's localhost TCP server (Docs/ADR/0022-dcc-bridge.md §5-7). Not a UObject; UStageBridgeSubsystem owns it.
 *
 * Threads, as for the DMX receiver (ADR 0019 §3.2):
 *  - Start, Stop, Send, Disconnect and Drain are game-thread calls.
 *  - One worker thread owns every client socket: it accepts, reads, checks the handshake (a valid hello with the token within
 *    HelloTimeoutSeconds, or the connection closes), parses messages and writes queued lines. A connection that breaks the
 *    protocol is told why ("bye") and closed. Nothing about an unauthenticated connection reaches the game thread.
 *  - Events go into a locked queue; the first event after a drain calls ScheduleDrain once (on the worker thread), so the game
 *    thread wakes once per batch and never while the bridge is idle.
 *  - The worker waits on the listening socket in 50 ms slices when nothing happened: that is the latency of a message, not a
 *    game-thread cost.
 * Stop (and the destructor) joins the worker before the listening socket is closed, so the worker never sees a dead socket.
 */
class FStageBridgeServer
{
public:
	FStageBridgeServer();
	~FStageBridgeServer();
	FStageBridgeServer(const FStageBridgeServer&) = delete;
	FStageBridgeServer& operator=(const FStageBridgeServer&) = delete;

	/**
	 * Listens on 127.0.0.1:Port (0: any free port, for tests). Token is what clients must present. On failure nothing stays open
	 * and OutError says why (typically: the port is in use).
	 */
	bool Start(uint16 Port, const FString& Token, TFunction<void()> ScheduleDrain, FText& OutError);

	/** Says "bye" to every client, closes everything. Idempotent. */
	void Stop();

	bool IsRunning() const { return Thread.IsValid(); }

	/** The bound port; 0 while stopped. */
	uint16 GetPort() const { return BoundPort; }

	/** Queues Line (a protocol line, newline included) for ClientId. Unknown or departed clients drop it. */
	void Send(int32 ClientId, const FString& Line);

	/** Visits the events since the last drain, in order. */
	void Drain(TFunctionRef<void(const FStageBridgeEvent&)> Visit);

private:
	class FWorker;

	/** State the worker and the game thread share, under Lock. */
	struct FShared
	{
		FCriticalSection Lock;
		TArray<FStageBridgeEvent> Events;
		TArray<TPair<int32, FString>> Outgoing;
		bool bDrainScheduled = false;
		TFunction<void()> ScheduleDrain;
	};

	TSharedPtr<FShared, ESPMode::ThreadSafe> Shared;
	TUniquePtr<FWorker> Worker;
	TUniquePtr<class FRunnableThread> Thread;
	class FSocket* Listener = nullptr;
	uint16 BoundPort = 0;
};
