// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Remote/StageWebRemoteProtocol.h"
#include "Templates/UniquePtr.h"

/** What the worker hands the game thread. Only paired clients ever produce commands. */
struct FStageWebRemoteEvent
{
	enum class EKind : uint8
	{
		/** A paired page or HTTP client sent a command; Path and Value are as received (StageRemote::ParseOsc judges them). */
		Command,
		/** A device paired; DeviceName says which. */
		Paired,
		/** Too many wrong PINs: the PIN changed and pairing is locked for a while. */
		PairingLocked,
		/** A paired page's WebSocket opened or closed; Pages is how many are open now. */
		PagesChanged,
	};

	EKind Kind = EKind::Command;
	FString Path;
	TOptional<float> Value;
	FString DeviceName;
	int32 Pages = 0;
};

/**
 * The Web Remote's HTTP and WebSocket server (Docs/ADR/0036 §2–4). Not a UObject; UStageWebRemoteSubsystem owns it.
 *
 * Threads, as for the DCC bridge server (ADR 0022 §5):
 *  - Start, Stop and every other public call are game-thread calls.
 *  - One worker thread owns every connection: it accepts, reads, parses with StageWebRemote, answers HTTP requests, upgrades
 *    WebSockets, checks hellos and tokens, and writes. Unpaired traffic never reaches the game thread.
 *  - Pairing (StageWebRemote::FPairing), the latest state and cue list, and outgoing broadcasts live in a shared block under one lock.
 *    The worker sends a page the state and cues only when their version changed since it last sent them.
 *  - Events go into the locked queue; the first event after a drain calls ScheduleDrain once (on the worker thread).
 *  - The worker sleeps 4 ms between passes while connections are open (the latency of a fader move) and waits on the listening
 *    socket in 50 ms slices while none are.
 * Stop (and the destructor) joins the worker before the listening socket is closed.
 */
class FStageWebRemoteServer
{
public:
	FStageWebRemoteServer();
	~FStageWebRemoteServer();
	FStageWebRemoteServer(const FStageWebRemoteServer&) = delete;
	FStageWebRemoteServer& operator=(const FStageWebRemoteServer&) = delete;

	/**
	 * Listens on 127.0.0.1 (bLoopbackOnly) or every card, on Port. PageFile is the remote page, read on the worker at the first request.
	 * MakePin replaces the random PIN (tests). On failure nothing stays open and OutError says why (typically: the port is in use).
	 */
	bool Start(bool bLoopbackOnly, uint16 Port, const FString& PageFile, TFunction<void()> ScheduleDrain, FText& OutError,
		TFunction<FString()> MakePin = nullptr);

	/** Closes every page with "going away" and stops. Idempotent. */
	void Stop();

	bool IsRunning() const { return Thread.IsValid(); }

	/** The bound port; 0 while stopped. */
	uint16 GetPort() const { return BoundPort; }

	/** The PIN to show the user; empty while stopped. */
	FString GetPin() const;
	int32 GetPairedCount() const;
	/** FPlatformTime::Seconds until which pairing is locked; 0 when open. */
	double GetPairingLockedUntil() const;

	/** A new PIN; paired devices stay. */
	void RenewPin();

	/** Forgets every device (their pages are closed) and makes a new PIN. */
	void UnpairAll();

	/** The latest state and cue list messages (StageWebRemote::MakeStateMessage / MakeCuesMessage), sent to pages that lack them. */
	void SetState(const FString& Message);
	void SetCues(const FString& Message);

	/** Sends Message to every paired page now. */
	void Broadcast(const FString& Message);

	/** Visits the events since the last drain, in order. Returns how many events were dropped over StageWebRemote::MaxQueuedEvents since then. */
	int32 Drain(TFunctionRef<void(const FStageWebRemoteEvent&)> Visit);

private:
	class FWorker;

	struct FShared
	{
		explicit FShared(TFunction<FString()> MakePin)
			: Pairing(MoveTemp(MakePin))
		{
		}

		mutable FCriticalSection Lock;
		StageWebRemote::FPairing Pairing;
		/** Set when devices were forgotten: the worker closes pages whose token stopped working. */
		bool bPairingChanged = false;
		FString State;
		uint64 StateVersion = 0;
		FString Cues;
		uint64 CuesVersion = 0;
		TArray<FString> Outgoing;
		TArray<FStageWebRemoteEvent> Events;
		int32 DroppedEvents = 0;
		bool bDrainScheduled = false;
		TFunction<void()> ScheduleDrain;
	};

	TSharedPtr<FShared, ESPMode::ThreadSafe> Shared;
	TUniquePtr<FWorker> Worker;
	TUniquePtr<class FRunnableThread> Thread;
	class FSocket* Listener = nullptr;
	uint16 BoundPort = 0;
};
