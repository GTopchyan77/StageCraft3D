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

	// --- Audio (Docs/ADR/0029-audio-analysis-and-audio-reactive-effects.md §4) ---

	/** Below this tempo confidence Sync keeps the last good tempo instead of following the analyser. */
	inline constexpr float SyncMinConfidence = 0.5f;
	/** Sync moves the clock towards the beat by at most this much of a cycle per frame, so a tempo change never jumps the pattern. */
	inline constexpr double SyncMaxPullCycles = 0.05;
	/** Trigger takes at most this many steps per frame, so a stream that resumes cannot race the pattern. */
	inline constexpr int32 TriggerMaxStepsPerFrame = 4;

	/** What the music gives the effects at one frame: one snapshot of the analyser, taken by the show once per tick. */
	struct FAudioInput
	{
		/** By EStageAudioBand. */
		float Bands[5] = { 0.f, 0.f, 0.f, 0.f, 0.f };
		/** Beats since the tempo was found, with the fraction through the current one, at the frame time. */
		double Beats = 0.0;
		float BPM = 0.f;
		float Confidence = 0.f;
		int64 OnsetCount = 0;
		bool bSignal = false;

		float GetBand(EStageAudioBand Band) const;
		/** A tempo Sync and Trigger may follow: a signal, a tempo and enough confidence. */
		bool HasTempo() const { return bSignal && BPM > 0.f && Confidence >= SyncMinConfidence; }

		/** From an analyser frame, the beat carried forward to NowSeconds (the frame is stamped when the audience hears it). */
		static FAudioInput FromFeatures(const FStageAudioFeatures& Features, double NowSeconds);
	};

	/**
	 * The band Modulate reads when there is no music (no signal, AUDIO KILL, master 0): 1 for most effects, so a show built on audio
	 * stays lit when the music stops; 0 for one that drives the Shutter, so a music strobe rests open instead of strobing at its
	 * fastest in silence (STATE.md #86, amends Docs/ADR/0029 §4).
	 */
	MODULARSCENEBUILDER_API float GetSilentBand(const FStageEffect& Effect);

	/** The band Modulate reads: the analyser's, or SilentBand without a signal (see GetSilentBand). */
	MODULARSCENEBUILDER_API float GetModulationBand(const FStageEffectAudio& Audio, const FAudioInput& Input, float SilentBand = 1.f);

	/** Band smoothed from Previous towards Target over DeltaSeconds: rises at once, falls with the response's time constant. */
	MODULARSCENEBUILDER_API float SmoothBand(float Previous, float Target, float DeltaSeconds, EStageEffectAudioResponse Response);

	/** The share 1 − Amount + Amount × Band that Modulate keeps of the level range or size. */
	MODULARSCENEBUILDER_API float GetModulationScale(const FStageEffectAudio& Audio, float Band);

	/**
	 * Effect with Modulate applied for Band: Level moves the top of the wave towards Low (High' = Low + (High − Low) × scale), Size
	 * scales the size; Rate is applied to the clock by the runner. Other modes leave the settings as they are.
	 */
	MODULARSCENEBUILDER_API void ApplyModulation(FStageEffect& InOutEffect, float Band);

	/** The clock rate Modulate Rate gives: RateHz × (1 + Amount × Band). */
	MODULARSCENEBUILDER_API float GetModulatedRate(const FStageEffect& Effect, float RateHz, float Band);

	/** Sync: where the clock should be (cycles) at Beats. Only the fraction is used. */
	MODULARSCENEBUILDER_API double GetSyncTarget(const FStageEffectAudio& Audio, double Beats);

	/** Trigger: the event count at Input, which a step follows each time it rises (beats every BeatsPerCycle, or onsets with Level). */
	MODULARSCENEBUILDER_API int64 GetTriggerEvents(const FStageEffectAudio& Audio, const FAudioInput& Input);

	/** Trigger: how far one step moves the clock: one pattern step of Count fixtures, or a whole cycle for Random (a new level). */
	MODULARSCENEBUILDER_API double GetTriggerStep(const FStageEffect& Effect, int32 Count);

	/** Shortest signed distance from Cycles to Target on the cycle, -0.5 .. 0.5. */
	MODULARSCENEBUILDER_API double GetCycleError(double Cycles, double Target);

	/** The AUDIO master's share of Band for Modulate: SilentBand (as without music) at Master 0, Band itself at 1. */
	MODULARSCENEBUILDER_API float ApplyAudioMaster(float Band, float Master, float SilentBand = 1.f);

	// --- Flash safety (Docs/ADR/0029-audio-analysis-and-audio-reactive-effects.md §7) ---

	/** A flash is a rise from below FlashLowFraction to above FlashHighFraction of an attribute's range. */
	inline constexpr float FlashLowFraction = 0.2f;
	inline constexpr float FlashHighFraction = 0.8f;

	/**
	 * Limits one music-driven intensity to at most LimitHz flashes a second. A rise that would flash sooner is refused: the output
	 * holds its last value (at most FlashHighFraction) until the flash is due, so a too-fast pattern shows its flashes at the
	 * limit instead of a reshaped flicker. Values between the thresholds, and a light that stays high, pass unchanged.
	 *
	 * Called with the same NowSeconds again (the show mixes a fixture more than once in a frame), it answers from the state the
	 * frame started with, so it is idempotent within a frame. Value object, game thread, no allocation.
	 */
	struct FFlashGate
	{
		/** Fraction 0..1 of the attribute's range at NowSeconds (seconds on the caller's own clock) -> the fraction to show. */
		MODULARSCENEBUILDER_API float Apply(float Fraction, double NowSeconds, float LimitHz);

	private:
		struct FState
		{
			double LastFlashSeconds = -1.0e9;
			/** Below FlashLowFraction since the last flash: the next rise above FlashHighFraction is a flash. */
			bool bArmed = true;
			float Output = 0.f;
		};
		/** As of the end of the previous frame, and after this frame's call. */
		FState Committed;
		FState Current;
		double FrameSeconds = -1.0e9;
	};

	/**
	 * The highest Shutter value (0 = open, 0..1 = strobe rate across StrobeRateHz, ALightingFixtureActor::GetStrobeRateHz) whose
	 * strobe is no faster than LimitHz. 0 (open) when even the slowest strobe is faster.
	 */
	MODULARSCENEBUILDER_API float GetStrobeShutterCap(const FVector2D& StrobeRateHz, float LimitHz);
}
