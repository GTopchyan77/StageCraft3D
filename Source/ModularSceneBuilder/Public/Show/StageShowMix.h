// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "Show/ShowControlTypes.h"

/**
 * How a fixture's output is made from its layers (Docs/ADR/0025-programmer-and-output-mix.md §2), lowest priority first:
 *   home look  <  playing cue  <  programmer  <  highlight.
 * Each attribute takes the value of the highest layer that holds it (latest takes precedence, as a console's LTP channels).
 * UShowControlSubsystem is the only caller that writes the result onto fixtures. Pure, any thread; covered by StageCraft.Show.Mix.
 */
namespace StageShowMix
{
	using FAttributeMap = TMap<FGameplayTag, float>;

	/** One fixture's layers. Null means the layer holds nothing for this fixture. */
	struct FFixtureLayers
	{
		const FAttributeMap* Home = nullptr;
		const FAttributeMap* Playback = nullptr;
		const FAttributeMap* Programmer = nullptr;
		const FAttributeMap* Highlight = nullptr;
	};

	/** The value Attribute takes: the highest layer's that holds it. False when no layer does (the output keeps its value). */
	MODULARSCENEBUILDER_API bool ResolveAttribute(const FGameplayTag& Attribute, const FFixtureLayers& Layers, float& OutValue);

	/** True when a layer above the playback (programmer or highlight) holds Attribute, so a cue fade must leave it alone. */
	MODULARSCENEBUILDER_API bool IsHeldAbovePlayback(const FGameplayTag& Attribute, const FFixtureLayers& Layers);

	/** Resolves each of Attributes (the fixture's own) into OutValues (reset first); attributes no layer holds are left out. */
	MODULARSCENEBUILDER_API void ResolveFixture(TConstArrayView<FGameplayTag> Attributes, const FFixtureLayers& Layers, FAttributeMap& OutValues);

	/**
	 * The share of the primary fixture's change that fixture Index of Count (selection order) takes under Mode: 1 with the fan off
	 * or a single fixture; -1 .. 1 for Linear, 1 .. 0 .. 1 for V, 0 .. 1 .. 0 for Inverted V. Out-of-range indices count as the ends.
	 */
	MODULARSCENEBUILDER_API float FanFactor(EStageFanMode Mode, int32 Index, int32 Count);
}
