// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/StaticArray.h"
#include "HAL/Runnable.h"
#include "Show/StageDMXNetProtocol.h"
#include "Show/StageDMXNetworkTypes.h"
#include "Templates/UniquePtr.h"

#include <atomic>

/** Everything a sender needs to start. Ports are the protocol ports, except in tests. */
struct FStageDMXSenderConfig
{
	FStageDMXOutputSettings Settings;
	/** The network card (BindAddress) and the universe numbering, shared with the input (Docs/ADR/0019 §9.4). */
	FStageDMXInputSettings Numbering;
	uint16 ArtNetPort = StageDMXNet::ArtNetPort;
	uint16 SACNPort = StageDMXNet::SACNPort;
	/** An unchanged universe is sent again after this long, so receivers keep seeing a live source (E1.31 §6.6.1). */
	double KeepAliveSeconds = 1.0;
	/** The sACN source name. */
	FString SourceName = TEXT("StageCraft");
};

/**
 * Art-Net and sACN output (Docs/ADR/0019-dmx-patch-and-network-input.md §9): one UDP socket and one thread of its own, so the
 * game thread never waits on the network (Rules.md §29).
 *
 * The game thread posts a universe's frame when its look changed (Post); the frames wait in a mailbox that keeps only the
 * newest one per universe, and the thread sends them as ArtDmx and/or E1.31 data with their own sequence numbers. A universe that
 * stops changing is sent again every KeepAliveSeconds, so the game thread renders only on change while receivers still see a
 * steady stream. The universes to send are set with SetUniverses: a universe left out (unpatched, muted) gets the E1.31 stream
 * termination and is no longer kept alive. Close terminates every sACN universe, joins the thread, then closes the socket.
 *
 * Game-thread API apart from the thread itself; not a UObject (UStageNetworkDMXBridge owns it).
 */
class FStageDMXNetworkSender : private FRunnable
{
public:
	FStageDMXNetworkSender();
	virtual ~FStageDMXNetworkSender() override;
	FStageDMXNetworkSender(const FStageDMXNetworkSender&) = delete;
	FStageDMXNetworkSender& operator=(const FStageDMXNetworkSender&) = delete;

	/** Opens the socket and starts the thread. On failure nothing stays open and OutError says why. */
	bool Open(const FStageDMXSenderConfig& Config, FText& OutError);

	/** Idempotent: terminates the sACN streams, joins the thread, closes the socket. */
	void Close();

	bool IsOpen() const { return Thread.IsValid(); }

	/** The newest frame of Universe (512 slots; fewer are zero-padded). Ignored for a universe not in SetUniverses. Never blocks on I/O. */
	void Post(int32 Universe, TConstArrayView<uint8> Slots);

	/** The StageCraft universes to send and keep alive, already without the muted ones. */
	void SetUniverses(TConstArrayView<int32> Universes);

	FStageDMXOutputStatus GetStatus() const;

private:
	//~ Begin FRunnable Interface
	virtual uint32 Run() override;
	virtual void Stop() override;
	//~ End FRunnable Interface

	/** The thread's own record of a universe: what it last sent and when. Only the thread touches these. */
	struct FUniverseStream
	{
		TStaticArray<uint8, 512> Slots;
		double LastSentSeconds = 0.0;
		uint8 ArtNetSequence = 0;
		uint8 SACNSequence = 0;
	};

	void SendFrame(int32 Universe, FUniverseStream& Stream, double NowSeconds);
	void SendSACNTermination(int32 Universe, FUniverseStream& Stream);
	bool SendTo(const TArray<uint8>& Packet, const class FInternetAddr& Destination);
	void CloseSocket();

	FStageDMXSenderConfig Config;
	class FSocket* Socket = nullptr;
	TSharedPtr<class FInternetAddr> ArtNetDestination;
	/** For sACN unicast; multicast addresses are made per universe. */
	TSharedPtr<class FInternetAddr> SACNUnicastDestination;
	TSharedPtr<class FInternetAddr> Scratch;
	FString Target;

	TUniquePtr<class FRunnableThread> Thread;
	class FEvent* WakeEvent = nullptr;
	std::atomic<bool> bStopping{ false };

	/** Shared between Post / SetUniverses and the thread. */
	mutable FCriticalSection Lock;
	TMap<int32, TStaticArray<uint8, 512>> Pending;
	TSet<int32> Universes;
	bool bUniversesChanged = false;

	/** The thread's. */
	TMap<int32, FUniverseStream> Streams;

	std::atomic<int64> ArtNetPackets{ 0 };
	std::atomic<int64> SACNPackets{ 0 };
	std::atomic<int64> SendErrors{ 0 };
};
