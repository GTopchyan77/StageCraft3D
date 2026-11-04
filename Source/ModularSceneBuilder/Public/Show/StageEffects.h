// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Show/StageEffectTypes.h"

/**
 * The effect maths (Docs/ADR/0027-effects-engine.md §2): where each fixture sits in the pattern and what the wave gives it at a point
 * of the effect's clock. Pure, no world, any thread; deterministic, so Random gives the same values on every run. Covered by
 * StageCraft.Show.EffectWaves and StageCraft.Show.EffectSpread.
 *
 * The clock is counted in cycles (the effect's rate integrated over time by its owner), so changing the rate never jumps the pattern.
 */
namespace StageEffects
{
	/** One fixture's sample of the wave. */
	struct FSample
	{
		/** 0 (bottom) .. 1 (top): intensity, beam, colour. */
		float Unit = 0.f;
		/** -1 .. 1: position. A single-axis waveform gives both the same value. */
		float Pan = 0.f;
		float Tilt = 0.f;
	};

	/**
	 * Waveform at Position (in cycles; only the fraction matters, except for Random, which draws a new level each whole cycle).
	 * Duty is Square's on fraction. Seed separates Random's fixtures (fixtures with the same seed move together).
	 */
	MODULARSCENEBUILDER_API FSample SampleWave(EStageEffectWaveform Waveform, double Position, float Duty, uint32 Seed);

	/**
	 * Fixture Index of Count's step in the pattern after wings (mirrored parts) and blocks (fixtures moving together): OutStep of
	 * OutSteps. Out-of-range indices count as the ends.
	 */
	MODULARSCENEBUILDER_API void GetPatternStep(const FStageEffect& Effect, int32 Index, int32 Count, int32& OutStep, int32& OutSteps);

	/** How far behind the first fixture Index is, in cycles: (Phase + Spread * step / steps) / 360. */
	MODULARSCENEBUILDER_API double GetFixtureOffset(const FStageEffect& Effect, int32 Index, int32 Count);

	/** Where in the wave (cycles) fixture Index of Count is at the effect clock Cycles: direction applied, then its offset. */
	MODULARSCENEBUILDER_API double GetWavePosition(const FStageEffect& Effect, int32 Index, int32 Count, double Cycles);

	/** Fixture Index of Count at the effect clock Cycles: the wave at GetWavePosition. */
	MODULARSCENEBUILDER_API FSample Evaluate(const FStageEffect& Effect, int32 Index, int32 Count, double Cycles);

	/** Size scale for fixture Index of Count (StageShowMix::FanFactor of SizeFan); 1 with the fan off. */
	MODULARSCENEBUILDER_API float GetSizeFactor(const FStageEffect& Effect, int32 Index, int32 Count);

	/** The colour a Color effect shows at Unit: ColorA..ColorB, or the hue circle (Rainbow) at full saturation. */
	MODULARSCENEBUILDER_API FLinearColor GetColor(const FStageEffect& Effect, float Unit);

	/** Intensity and Beam: the fraction 0..1 of a fixture's range at Unit, after the size fan (which narrows the swing from Low). */
	MODULARSCENEBUILDER_API float GetLevel(const FStageEffect& Effect, float Unit, float SizeFactor);
}
