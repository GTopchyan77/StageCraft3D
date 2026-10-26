// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/StaticArray.h"
#include "Misc/Guid.h"

enum class EStageDMXNetProtocol : uint8
{
	ArtNet,
	SACN,
};

/** What a datagram turned out to be. */
enum class EStageDMXNetPacket : uint8
{
	/** Not a valid Art-Net or E1.31 packet (wrong header, size, version or field value). Counted, never applied. */
	Invalid,
	/** Valid, but nothing StageCraft acts on (ArtSync, sACN discovery, alternate START codes, ...). */
	Ignored,
	ArtDmx,
	ArtPoll,
	SACNData,
};

/** One DMX frame read off the wire. */
struct MODULARSCENEBUILDER_API FStageDMXNetFrame
{
	FStageDMXNetFrame();

	EStageDMXNetProtocol Protocol = EStageDMXNetProtocol::ArtNet;

	/** The number on the wire: Art-Net 15-bit port-address (0..32767) or sACN universe (1..63999). The settings map it. */
	int32 WireUniverse = 0;

	/** sACN priority 0..200. Art-Net has none and counts as the sACN default, 100. */
	uint8 Priority = 100;

	/** Wraps at 255. Art-Net sends 0 when it does not sequence. */
	uint8 Sequence = 0;

	/** sACN Stream_Terminated: the source is leaving; the frame's slots are not to be shown. */
	bool bTerminated = false;

	/** Who sent it: the sACN CID, or (filled by the receiver) the Art-Net sender's address. */
	FGuid Source;

	/** Slots carried (1..512); the rest of Slots is zero. */
	int32 SlotCount = 0;

	/** Index 0 is DMX address 1. */
	TStaticArray<uint8, 512> Slots;
};

/** What StageCraft says about itself in an ArtPollReply. */
struct FStageArtPollReplyInfo
{
	/** IPv4 of this node, host byte order (a.b.c.d = a << 24 | ...). */
	uint32 IPv4 = 0;
	FString ShortName;
	FString LongName;
	/** Art-Net port-addresses StageCraft listens to (its patched universes). */
	TArray<int32> PortAddresses;
	/** Whether data is arriving, reported as "output active" on the ports. */
	bool bReceiving = false;
};

/**
 * Art-Net 4 (Artistic Licence) and ANSI E1.31-2018 (sACN) packets, pure and allocation-light so the receiver threads can call
 * them (Docs/ADR/0019-dmx-patch-and-network-input.md §3). Every field of an incoming packet is validated: it is network input.
 * The builders exist for ArtPollReply and for tests and the loopback console command.
 */
namespace StageDMXNet
{
	inline constexpr uint16 ArtNetPort = 6454;
	inline constexpr uint16 SACNPort = 5568;
	inline constexpr int32 MaxArtNetPortAddress = 32767;
	inline constexpr int32 MaxSACNUniverse = 63999;
	inline constexpr int32 ArtPollReplySize = 239;

	/** Reads Packet. OutFrame is filled for ArtDmx and SACNData only. */
	MODULARSCENEBUILDER_API EStageDMXNetPacket Parse(TConstArrayView<uint8> Packet, FStageDMXNetFrame& OutFrame);

	MODULARSCENEBUILDER_API TArray<uint8> BuildArtDmx(int32 PortAddress, uint8 Sequence, TConstArrayView<uint8> Slots);
	MODULARSCENEBUILDER_API TArray<uint8> BuildArtPoll();

	/**
	 * One reply per bind index: Art-Net puts at most four ports in a reply, all sharing one Net and Sub-Net, so the
	 * port-addresses are grouped by their upper 11 bits. A node without ports still sends one reply.
	 */
	MODULARSCENEBUILDER_API TArray<TArray<uint8>> BuildArtPollReplies(const FStageArtPollReplyInfo& Info);

	MODULARSCENEBUILDER_API TArray<uint8> BuildSACNData(int32 Universe, const FGuid& Cid, uint8 Priority, uint8 Sequence,
		TConstArrayView<uint8> Slots, bool bTerminated, FStringView SourceName);

	/** E1.31 multicast group of Universe, 239.255.hi.lo in host byte order. */
	MODULARSCENEBUILDER_API uint32 GetSACNMulticastAddress(int32 Universe);
}

/**
 * Picks which source drives each universe when several send to it (E1.31 §6.2.3 rules; Art-Net counts as priority 100):
 *  - the highest priority wins; an equal or lower one is ignored while the current source is alive;
 *  - a source is gone after SourceTimeoutSeconds of silence or when it sends Stream_Terminated, and the next sender takes over;
 *  - sACN frames older than the last one shown (sequence within 20 behind) are dropped, as E1.31 §6.7.2 asks.
 * No merging (HTP / LTP): one source per universe, as most visualisers do. Pure; the time is passed in.
 * Not thread-safe; the receiver calls it under its lock.
 */
class MODULARSCENEBUILDER_API FStageDMXSourceArbiter
{
public:
	static constexpr double SourceTimeoutSeconds = 2.5;

	/** Whether Frame, already mapped onto StageCraft Universe, is to be shown. Updates the universe's current source. */
	bool Accept(int32 Universe, const FStageDMXNetFrame& Frame, double NowSeconds);

	/** Universes whose current source is alive, ascending. */
	TArray<int32> GetLiveUniverses(double NowSeconds) const;

	/** Distinct sources currently driving at least one universe. */
	int32 GetLiveSourceCount(double NowSeconds) const;

	void Reset() { Current.Reset(); }

private:
	struct FCurrentSource
	{
		FGuid Source;
		uint8 Priority = 0;
		uint8 Sequence = 0;
		EStageDMXNetProtocol Protocol = EStageDMXNetProtocol::ArtNet;
		double LastSeen = 0.0;
	};

	static bool IsAlive(const FCurrentSource& Source, double NowSeconds);

	TMap<int32, FCurrentSource> Current;
};
