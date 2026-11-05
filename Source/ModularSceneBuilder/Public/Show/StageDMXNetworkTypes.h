// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "StageDMXNetworkTypes.generated.h"

/**
 * How this PC listens for Art-Net and sACN (Docs/ADR/0019-dmx-patch-and-network-input.md §3.4). Saved per machine in
 * GameUserSettings.ini (UStageCraftUserSettings); UStageDMXNetworkSubsystem is the only writer. Always read through
 * StageDMXInput::Sanitize: the file can be edited by hand.
 */
USTRUCT()
struct MODULARSCENEBUILDER_API FStageDMXInputSettings
{
	GENERATED_BODY()

	/** Remembered on / off: input opens again with the next stage when it was on. Off by default. */
	UPROPERTY(Config)
	bool bEnabled = false;

	UPROPERTY(Config)
	bool bArtNet = true;

	UPROPERTY(Config)
	bool bSACN = true;

	/** Answer ArtPoll so consoles that send only to discovered nodes find StageCraft (a small reply on the LAN). */
	UPROPERTY(Config)
	bool bReplyToArtPoll = true;

	/** IPv4 of the network card to listen on; empty = every card. */
	UPROPERTY(Config)
	FString BindAddress;

	/** The Art-Net port-address that is StageCraft universe 1. 0 = Art-Net 0:0:0, as most consoles count. */
	UPROPERTY(Config)
	int32 ArtNetFirstPortAddress = 0;

	/** The sACN universe that is StageCraft universe 1. */
	UPROPERTY(Config)
	int32 SACNFirstUniverse = 1;

	bool operator==(const FStageDMXInputSettings& Other) const = default;
};

UENUM(BlueprintType)
enum class EStageDMXInputState : uint8
{
	Off,
	Listening,
	/** The ports could not be opened (another program holds them, a bad bind address); see FStageDMXInputStatus::Error. */
	Failed,
};

/** What the Patch panel shows about the input. A snapshot; counters only grow while listening. */
struct FStageDMXInputStatus
{
	EStageDMXInputState State = EStageDMXInputState::Off;
	FText Error;
	/** "Art-Net 6454, sACN 5568 on 192.168.1.20" */
	FString Endpoint;
	int64 ArtNetPackets = 0;
	int64 SACNPackets = 0;
	/** Datagrams that were neither valid Art-Net nor valid sACN. */
	int64 InvalidPackets = 0;
	/** Valid frames not shown: unmapped universe, a lower-priority or late source. */
	int64 IgnoredFrames = 0;
	/** StageCraft universes with a live source, ascending. */
	TArray<int32> LiveUniverses;
	int32 LiveSources = 0;
};

/** How Art-Net leaves this PC. */
UENUM()
enum class EStageArtNetOutputMode : uint8
{
	Off,
	/** To every node on the network (255.255.255.255, leaving through the chosen network card), as most consoles send by default. */
	Broadcast,
	/** To FStageDMXOutputSettings::UnicastAddress only. */
	Unicast,
};

/** How sACN leaves this PC. */
UENUM()
enum class EStageSACNOutputMode : uint8
{
	Off,
	/** To each universe's E1.31 multicast group (239.255.hi.lo), which every sACN receiver listens to. */
	Multicast,
	/** To FStageDMXOutputSettings::UnicastAddress only. */
	Unicast,
};

/**
 * How this PC sends Art-Net and sACN (Docs/ADR/0019-dmx-patch-and-network-input.md §9). Saved per machine in
 * GameUserSettings.ini (UStageCraftUserSettings); UStageDMXNetworkSubsystem is the only writer. Always read through
 * StageDMXOutput::Sanitize: the file can be edited by hand. The network card and the universe numbering are the input's
 * (FStageDMXInputSettings::BindAddress, ArtNetFirstPortAddress, SACNFirstUniverse), so they are set in one place for both.
 */
USTRUCT()
struct MODULARSCENEBUILDER_API FStageDMXOutputSettings
{
	GENERATED_BODY()

	/** Remembered on / off: output opens again with the next stage when it was on. Off by default. */
	UPROPERTY(Config)
	bool bEnabled = false;

	UPROPERTY(Config)
	EStageArtNetOutputMode ArtNet = EStageArtNetOutputMode::Broadcast;

	UPROPERTY(Config)
	EStageSACNOutputMode SACN = EStageSACNOutputMode::Multicast;

	/** IPv4 of the receiver for the protocols set to Unicast (a node, a visualiser PC). */
	UPROPERTY(Config)
	FString UnicastAddress;

	/** sACN priority, 0..200; receivers show the highest. */
	UPROPERTY(Config)
	int32 SACNPriority = 100;

	/** Frames per second while the look changes, 1..44 (44 is the most a DMX512 line carries). */
	UPROPERTY(Config)
	int32 RateHz = 44;

	/** StageCraft universes that are never sent, ascending and unique. */
	UPROPERTY(Config)
	TArray<int32> MutedUniverses;

	/** This PC's sACN component ID, made once (UStageCraftUserSettings) so receivers see the same source across runs. */
	UPROPERTY(Config)
	FGuid Cid;

	bool operator==(const FStageDMXOutputSettings& Other) const = default;
};

UENUM(BlueprintType)
enum class EStageDMXOutputState : uint8
{
	Off,
	Sending,
	/** The socket could not be opened or the target is invalid; see FStageDMXOutputStatus::Error. */
	Failed,
};

/** What the Patch panel shows about the output. A snapshot; counters only grow while sending. */
struct FStageDMXOutputStatus
{
	EStageDMXOutputState State = EStageDMXOutputState::Off;
	FText Error;
	/** "Art-Net broadcast 192.168.1.255, sACN multicast" */
	FString Target;
	int64 ArtNetPackets = 0;
	int64 SACNPackets = 0;
	/** Datagrams the network refused (an unreachable unicast address, a card that went away). */
	int64 SendErrors = 0;
	/** StageCraft universes being sent, ascending. */
	TArray<int32> Universes;
};

/** Universe numbering between the wire and StageCraft, and validation of the saved settings. Pure. */
namespace StageDMXInput
{
	MODULARSCENEBUILDER_API FStageDMXInputSettings Sanitize(const FStageDMXInputSettings& Settings);

	/** StageCraft universe of an Art-Net port-address, or 0 when it maps below universe 1 or above 63999. */
	MODULARSCENEBUILDER_API int32 FromArtNet(int32 PortAddress, const FStageDMXInputSettings& Settings);

	/** Art-Net port-address of a StageCraft universe, or INDEX_NONE when it has none (outside 0..32767). */
	MODULARSCENEBUILDER_API int32 ToArtNet(int32 Universe, const FStageDMXInputSettings& Settings);

	/** StageCraft universe of an sACN universe, or 0 when unmapped. */
	MODULARSCENEBUILDER_API int32 FromSACN(int32 SACNUniverse, const FStageDMXInputSettings& Settings);

	/** sACN universe of a StageCraft universe, or INDEX_NONE when it has none (outside 1..63999). */
	MODULARSCENEBUILDER_API int32 ToSACN(int32 Universe, const FStageDMXInputSettings& Settings);
}

/** Validation of the saved output settings. Pure. */
namespace StageDMXOutput
{
	inline constexpr int32 MinRateHz = 1;
	inline constexpr int32 MaxRateHz = 44;
	inline constexpr int32 MaxSACNPriority = 200;

	/**
	 * Clamps the rate and priority, trims the unicast address and empties it when it is not IPv4, sorts and dedupes the muted
	 * universes (dropping ones outside 1..63999). A protocol set to Unicast without a valid address stays so: StartOutput
	 * reports it, rather than quietly sending somewhere else. Leaves Cid as it is (UStageCraftUserSettings makes one).
	 */
	MODULARSCENEBUILDER_API FStageDMXOutputSettings Sanitize(const FStageDMXOutputSettings& Settings);

	/** Whether Universe is in Settings.MutedUniverses (sanitized, so sorted). */
	MODULARSCENEBUILDER_API bool IsMuted(const FStageDMXOutputSettings& Settings, int32 Universe);
}
