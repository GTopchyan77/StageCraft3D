// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Colour mixing of stage fixtures (Docs/ADR/0020-professional-fixture-suite.md §2), for the beam colour, the lens glow and
 * photometrics. Colours are linear Rec. 709 / sRGB primaries, like FLinearColor.
 *
 * Additive mixing (LED RGB, RGBW, RGBA, RGBWA, pixel cells) sums the emitters, each at its level; an emitter's output at full
 * is its colour times its relative flux, so a white emitter adds its own white, not pure (1, 1, 1). Subtractive mixing (CMY,
 * colour wheels, gels) multiplies the lamp's white by filter transmissions; CTO shifts the lamp's white by a mired amount.
 *
 * Planning accuracy, not spectral: real emitters and filters have spectra that RGB only approximates.
 *
 * Pure and deterministic: no world, no UObjects. Covered by StageCraft.Lighting.ColorScience.
 */
namespace StageColorScience
{
	/** Coolest and warmest temperatures accepted, in Kelvin. Inputs outside are clamped. */
	constexpr double MinKelvin = 1000.0;
	constexpr double MaxKelvin = 40000.0;

	struct FMix
	{
		/** Linear colour of the mix with its brightest channel at 1 (black when nothing is lit). */
		FLinearColor Color = FLinearColor::Black;

		/** Luminance of the mix relative to the fixture's full output, 0..1: how much of the rated flux leaves the fixture. */
		double LuminousFraction = 0.0;
	};

	/** Rec. 709 relative luminance. */
	MODULARSCENEBUILDER_API double GetLuminance(const FLinearColor& Color);

	/** White of a black-body source at Kelvin, linear, brightest channel 1. */
	MODULARSCENEBUILDER_API FLinearColor KelvinToLinear(double Kelvin);

	/** A source shifted by a filter of Mired micro reciprocal degrees: CTO is positive (warmer), CTB negative. */
	MODULARSCENEBUILDER_API double ShiftKelvinByMired(double Kelvin, double Mired);

	/**
	 * Additive mix: Outputs[i] is emitter i at full (colour x relative flux), Levels[i] its level 0..1. Levels beyond the
	 * emitters are ignored; missing ones count as 0; non-finite ones as 0.
	 */
	MODULARSCENEBUILDER_API FMix MixAdditive(TConstArrayView<FLinearColor> Outputs, TConstArrayView<float> Levels);

	/**
	 * Subtractive mix: a lamp of LampKelvin, warmed by Cto (0..1 of FullCtoMired), through a filter that passes Filter per
	 * channel (CMY: 1 - C, 1 - M, 1 - Y; a gel or a colour-wheel slot: its transmission).
	 */
	MODULARSCENEBUILDER_API FMix MixSubtractive(double LampKelvin, const FLinearColor& Filter, double Cto, double FullCtoMired);

	/** A mix seen through a filter (colour wheel slot, gel): its colour times the filter, and the light the filter passes. */
	MODULARSCENEBUILDER_API FMix ApplyFilter(const FMix& Mix, const FLinearColor& Filter);

	/** The light of several equal sources (the cells of a pixel bar) as one: luminance-weighted colour, mean fraction. */
	MODULARSCENEBUILDER_API FMix Combine(TConstArrayView<FMix> Sources);

	/**
	 * Emitter levels that reproduce Target's colour as brightly as the emitters allow. The first three outputs must span the
	 * colour space (red, green, blue); up to two further emitters (white, amber) share the mix so that the highest level is as
	 * low as possible before it is scaled to 1, which is how RGBW fixtures reach their rated output (white = every emitter at
	 * full). Further emitters stay off. Colours outside the emitters' gamut are clamped to its edge. Returns levels with the
	 * highest at 1, or all zero for a black or invalid target. Fewer than three outputs: all levels follow Target's brightest
	 * channel.
	 */
	MODULARSCENEBUILDER_API TArray<float> MatchAdditive(TConstArrayView<FLinearColor> Outputs, const FLinearColor& Target);
}
