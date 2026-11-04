// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "Show/ShowControlTypes.h"

/**
 * How a fixture's output is made from its layers (Docs/ADR/0025-programmer-and-output-mix.md §2, Docs/ADR/0026 §6), lowest first:
 *   home look  <  main cue list and playback faders  <  programmer  <  effects  <  highlight,  then the masters on intensity.
 * Between the cue list and the playbacks, intensity (the Intensity kind: dimmer and cell dimmers) is HTP: the highest of the cue
 * list's value and each playback's value times its fader. Every other attribute is LTP: the playback activated last that holds it,
 * else the cue list. Above that each attribute takes the highest layer that holds it; a running effect (Docs/ADR/0027 §3) then
 * adds to that value (position) or replaces it (intensity, colour, beam), and intensity is finally scaled by IntensityScale
 * (Grand Master x Blackout). UShowControlSubsystem is the only caller that writes the result onto fixtures. Pure,
 * any thread; covered by StageCraft.Show.OutputMix and StageCraft.Show.PlaybackMix.
 */
namespace StageShowMix
{
	using FAttributeMap = TMap<FGameplayTag, float>;

	/** One active playback fader's contribution to a fixture. */
	struct FPlaybackLayer
	{
		const FAttributeMap* Values = nullptr;
		/** The fader level, or 1 while flashed. Scales intensity only. */
		float Level = 1.f;
		/** Activation order: the higher one wins an LTP attribute. */
		uint32 Order = 0;
	};

	/** An effect's output for one attribute (Docs/ADR/0027 §3). */
	struct FEffectValue
	{
		/** Relative: degrees added to the static value. Absolute: the value itself. */
		float Value = 0.f;
		bool bRelative = false;
		/** 0..1: how much of the effect shows (its fade in or out); the static value shows through the rest. */
		float Weight = 1.f;
	};

	using FEffectMap = TMap<FGameplayTag, FEffectValue>;

	/**
	 * Puts Over on top of Into, where two effects drive one attribute (Docs/ADR/0027 §5): the result applied to a static value equals
	 * applying Into, then Over, each by its weight. So Over at weight 1 replaces Into (the later effect wins), and one fading in over
	 * another crossfades. A relative and an absolute value do not mix: Over replaces Into.
	 */
	MODULARSCENEBUILDER_API void ComposeEffect(FEffectValue& Into, const FEffectValue& Over);

	/** One fixture's layers. Null means the layer holds nothing for this fixture. */
	struct FFixtureLayers
	{
		const FAttributeMap* Home = nullptr;
		/** The main cue list's playback layer. */
		const FAttributeMap* Playback = nullptr;
		/** Active playback faders that hold values for this fixture. */
		TArray<FPlaybackLayer, TInlineAllocator<4>> Playbacks;
		const FAttributeMap* Programmer = nullptr;
		/** Running effects' output for this fixture, above the programmer and below the highlight. */
		const FEffectMap* Effects = nullptr;
		const FAttributeMap* Highlight = nullptr;
		/** Grand Master x (Blackout ? 0 : 1), applied to intensity last. */
		float IntensityScale = 1.f;
	};

	/** The value Attribute takes. False when no layer holds it (the output keeps its value). */
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
