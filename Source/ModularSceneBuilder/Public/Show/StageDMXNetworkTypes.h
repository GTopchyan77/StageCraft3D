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
