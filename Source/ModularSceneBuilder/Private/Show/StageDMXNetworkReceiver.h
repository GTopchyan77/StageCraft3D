// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Show/StageDMXNetProtocol.h"
#include "Show/StageDMXNetworkTypes.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"

#include <atomic>

/** Everything a receiver needs to start. Ports are the protocol ports, except in tests. */
struct FStageDMXReceiverConfig
{
	FStageDMXInputSettings Settings;
	uint16 ArtNetPort = StageDMXNet::ArtNetPort;
	uint16 SACNPort = StageDMXNet::SACNPort;
	/** Name StageCraft gives itself in ArtPollReply. */
	FString NodeName = TEXT("StageCraft");
};

/**
 * The state the receiver threads and the game thread share (Docs/ADR/0019 §3.2). Datagrams arrive on the socket threads; they
 * are parsed and arbitrated there, and the newest frame of each universe is kept here under one lock. The first frame after a
 * drain asks the owner (ScheduleDrain, called on the socket thread) to drain once on the game thread, so however many packets
 * arrive in between, the game thread does one hand-off per frame batch and nothing at all while the wire is quiet.
 */
class FStageDMXInbox : public TSharedFromThis<FStageDMXInbox, ESPMode::ThreadSafe>
{
public:
	FStageDMXInbox(const FStageDMXInputSettings& InSettings, TFunction<void()> InScheduleDrain);

	/** Socket thread. Called by FUdpSocketReceiver for every datagram on the Art-Net port; may answer ArtPoll on ArtNetSocket. */
	void HandleArtNetDatagram(const TSharedPtr<class FArrayReader, ESPMode::ThreadSafe>& Data, const struct FIPv4Endpoint& Sender);

	/** Socket thread. Every datagram on the sACN port. */
	void HandleSACNDatagram(const TSharedPtr<class FArrayReader, ESPMode::ThreadSafe>& Data, const struct FIPv4Endpoint& Sender);

	/** Any thread. Frame, received at NowSeconds, after parsing: maps, arbitrates, keeps the newest per universe. */
	void HandleFrame(const FStageDMXNetFrame& Frame, double NowSeconds);

	/** Game thread. Visits the newest frame of every universe that received one since the last drain (512 slots each). */
	void Drain(TFunctionRef<void(int32 Universe, TConstArrayView<uint8> Slots)> Visit);

	/** Game thread. What ArtPollReply announces; also called when the patch changes. Socket is not owned. */
	void SetArtPollReply(class FSocket* InArtNetSocket, const FStageArtPollReplyInfo& Info);

	FStageDMXInputStatus GetStatus(double NowSeconds) const;

private:
	void ReplyToArtPoll(const struct FIPv4Endpoint& Sender, double NowSeconds);

	const FStageDMXInputSettings Settings;
	const TFunction<void()> ScheduleDrain;

	mutable FCriticalSection Lock;
	FStageDMXSourceArbiter Arbiter;
	/** Newest frame per StageCraft universe since the last drain. */
	TMap<int32, TStaticArray<uint8, 512>> Pending;
	bool bDrainScheduled = false;
	class FSocket* ArtNetSocket = nullptr;
	TArray<TArray<uint8>> ArtPollReplies;
	double LastArtPollReplySeconds = -1.0;

	std::atomic<int64> ArtNetPackets{ 0 };
	std::atomic<int64> SACNPackets{ 0 };
	std::atomic<int64> InvalidPackets{ 0 };
	std::atomic<int64> IgnoredFrames{ 0 };
};

/**
 * Art-Net and sACN listener (Docs/ADR/0019 §3): two UDP sockets, each read by an engine FUdpSocketReceiver thread, feeding one
 * FStageDMXInbox. Owns the sockets and threads; Stop (or the destructor) joins the threads before the sockets go, so a
 * callback never sees a dead socket. Game-thread API; not a UObject (UStageNetworkDMXBridge owns it).
 */
class FStageDMXNetworkReceiver
{
public:
	// Out of line: the socket receiver type is only complete in the .cpp.
	FStageDMXNetworkReceiver();
	~FStageDMXNetworkReceiver();
	FStageDMXNetworkReceiver(const FStageDMXNetworkReceiver&) = delete;
	FStageDMXNetworkReceiver& operator=(const FStageDMXNetworkReceiver&) = delete;

	/** Opens the enabled protocols' ports. On failure nothing stays open and OutError says why. ScheduleDrain: see FStageDMXInbox. */
	bool Start(const FStageDMXReceiverConfig& Config, TFunction<void()> ScheduleDrain, FText& OutError);

	/** Idempotent. */
	void Stop();

	bool IsRunning() const { return Inbox.IsValid(); }

	void Drain(TFunctionRef<void(int32 Universe, TConstArrayView<uint8> Slots)> Visit);

	/**
	 * The StageCraft universes to receive: joins (and leaves) their sACN multicast groups and announces their Art-Net
	 * port-addresses in ArtPollReply. Unicast and broadcast frames of other universes still arrive and are mapped.
	 */
	void SetUniverses(TConstArrayView<int32> Universes);

	FStageDMXInputStatus GetStatus() const;

private:
	class FSocket* OpenSocket(const TCHAR* Name, uint16 Port, bool bBindToCard, FText& OutError) const;
	void CloseSocket(class FSocket*& Socket) const;
	FStageArtPollReplyInfo MakeArtPollReply(TConstArrayView<int32> Universes) const;

	FStageDMXReceiverConfig Config;
	TSharedPtr<FStageDMXInbox, ESPMode::ThreadSafe> Inbox;
	class FSocket* ArtNetSocket = nullptr;
	class FSocket* SACNSocket = nullptr;
	TUniquePtr<class FUdpSocketReceiver> ArtNetReceiver;
	TUniquePtr<class FUdpSocketReceiver> SACNReceiver;
	/** sACN universes (wire numbers) whose multicast group the socket has joined. */
	TSet<int32> JoinedSACNUniverses;
	uint32 LocalIPv4 = 0;
	FString Endpoint;
};
