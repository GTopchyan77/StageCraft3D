// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Data/LightingFixtureData.h"

/**
 * DMX encoding through a channel's functions (Docs/ADR/0033 §4.3): the ranges an imported fixture's channel is divided into,
 * each with its own meaning. Values are in the channel's own attribute (FStageFixtureDMXChannel::Attribute) and DMX values in
 * the channel's resolution. A channel without functions is not handled here (ULightingFixtureData keeps the linear encoding).
 *
 * Pure, allocation free and safe on any thread: a short scan over the channel's ranges, which are fixed after import.
 */
namespace StageDMXFunctions
{
	/** Most ranges one channel may hold; an import with more keeps the channel at its default. */
	constexpr int32 MaxFunctions = 256;

	/**
	 * The DMX value for Value. The first range of the channel's attribute that contains it wins: a slot range by its slot (sent
	 * at the range's centre), a fixed range at its centre, a continuous range interpolated from DMXFrom to DMXTo. A value no range
	 * holds goes to the nearest one, except an open Shutter (0) on a channel with no open range, which sends DefaultDMX. A channel
	 * with no range of its attribute sends DefaultDMX. Always within 0..GetMaxValue().
	 */
	MODULARSCENEBUILDER_API int32 Encode(const FStageFixtureDMXChannel& Channel, float Value);

	/**
	 * The value a received DMX value selects: the slot of a slot range, or the interpolated value of a continuous one. Unset when
	 * the value is in a range StageCraft does not drive or in no range, so the attribute keeps its value; on a Shutter channel
	 * such a value reads as open (0), so a console leaving strobe never leaves the fixture strobing.
	 */
	MODULARSCENEBUILDER_API TOptional<float> Decode(const FStageFixtureDMXChannel& Channel, int32 DMXValue);

	/** True when the ranges are usable: at most MaxFunctions, each inside the resolution with DMXFrom <= DMXTo, rising and apart. */
	MODULARSCENEBUILDER_API bool AreValid(const FStageFixtureDMXChannel& Channel);
}
