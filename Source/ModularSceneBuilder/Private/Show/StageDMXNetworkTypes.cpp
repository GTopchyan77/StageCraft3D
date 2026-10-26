// Copyright Epic Games, Inc. All Rights Reserved.

#include "Show/StageDMXNetworkTypes.h"

#include "Interfaces/IPv4/IPv4Address.h"
#include "Show/StageDMXNetProtocol.h"
#include "Show/StageDMXPatching.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageDMXNetworkTypes)

namespace
{
	int32 MapToStage(int32 Wire, int32 First)
	{
		const int64 Universe = static_cast<int64>(Wire) - First + 1;
		return Universe >= 1 && Universe <= StageDMXPatching::MaxUniverse ? static_cast<int32>(Universe) : 0;
	}

	int32 MapToWire(int32 Universe, int32 First, int32 MinWire, int32 MaxWire)
	{
		if (Universe < 1)
		{
			return INDEX_NONE;
		}
		const int64 Wire = static_cast<int64>(Universe) + First - 1;
		return Wire >= MinWire && Wire <= MaxWire ? static_cast<int32>(Wire) : INDEX_NONE;
	}
}

FStageDMXInputSettings StageDMXInput::Sanitize(const FStageDMXInputSettings& Settings)
{
	FStageDMXInputSettings Result = Settings;
	Result.ArtNetFirstPortAddress = FMath::Clamp(Settings.ArtNetFirstPortAddress, 0, StageDMXNet::MaxArtNetPortAddress);
	Result.SACNFirstUniverse = FMath::Clamp(Settings.SACNFirstUniverse, 1, StageDMXNet::MaxSACNUniverse);
	Result.BindAddress.TrimStartAndEndInline();

	FIPv4Address Parsed;
	if (!Result.BindAddress.IsEmpty() && !FIPv4Address::Parse(Result.BindAddress, Parsed))
	{
		Result.BindAddress.Reset(); // Not an IPv4 address: listen on every card rather than on nothing.
	}
	return Result;
}

int32 StageDMXInput::FromArtNet(int32 PortAddress, const FStageDMXInputSettings& Settings)
{
	return PortAddress >= 0 && PortAddress <= StageDMXNet::MaxArtNetPortAddress ? MapToStage(PortAddress, Settings.ArtNetFirstPortAddress) : 0;
}

int32 StageDMXInput::ToArtNet(int32 Universe, const FStageDMXInputSettings& Settings)
{
	return MapToWire(Universe, Settings.ArtNetFirstPortAddress, 0, StageDMXNet::MaxArtNetPortAddress);
}

int32 StageDMXInput::FromSACN(int32 SACNUniverse, const FStageDMXInputSettings& Settings)
{
	return SACNUniverse >= 1 && SACNUniverse <= StageDMXNet::MaxSACNUniverse ? MapToStage(SACNUniverse, Settings.SACNFirstUniverse) : 0;
}

int32 StageDMXInput::ToSACN(int32 Universe, const FStageDMXInputSettings& Settings)
{
	return MapToWire(Universe, Settings.SACNFirstUniverse, 1, StageDMXNet::MaxSACNUniverse);
}
