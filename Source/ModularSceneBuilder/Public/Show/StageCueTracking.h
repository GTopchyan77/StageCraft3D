// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "Show/ShowControlTypes.h"

/**
 * Tracking cue lists (Docs/ADR/0026-cue-timing-tracking-playbacks.md §3): a cue holds only what it changes, and the look at a cue is
 * worked out from the nearest blocked cue forward. Pure, any thread; covered by StageCraft.Show.CueTracking. UShowControlSubsystem
 * uses it on Go, Back, Go To, restore and store, never per frame.
 */
namespace StageCueTracking
{
	/** Per fixture ID: the values (and palette references) a cue list gives that fixture. */
	using FFixtureStates = TMap<int32, FShowCueFixtureState>;

	/**
	 * The look at Cues[Index]: from the nearest blocked cue at or before Index, each attribute takes the value (and reference) of the
	 * latest cue that sets it; a plain value replaces an earlier reference. Empty for an index out of range. Cues must be sorted.
	 */
	MODULARSCENEBUILDER_API void StateAt(TConstArrayView<FShowCue> Cues, int32 Index, FFixtureStates& OutState);

	/** Puts Entry's values and references onto Into (same fixture); a value Entry sets without a reference drops Into's reference. */
	MODULARSCENEBUILDER_API void ApplyEntry(FShowCueFixtureState& Into, const FShowCueFixtureState& Entry);

	/** Merges Entries (one per fixture) into Cue's fixtures with ApplyEntry; fixtures new to the cue are appended. */
	MODULARSCENEBUILDER_API void MergeInto(FShowCue& Cue, TConstArrayView<FShowCueFixtureState> Entries);

	/** States as cue fixtures, sorted by fixture ID (Block writes the tracked look into a cue with this). */
	MODULARSCENEBUILDER_API TArray<FShowCueFixtureState> ToEntries(const FFixtureStates& States);

	/**
	 * Cue Only: the cue after Index keeps the look it had before Entries were stored at Index. For each attribute in Entries that the
	 * next cue does not set itself, the next cue gets Before's value and reference, or the fixture's home value (HomeValue) when
	 * Before had none. Nothing when there is no next cue or it is blocked (it inherits nothing anyway). Returns how many values it wrote.
	 */
	MODULARSCENEBUILDER_API int32 KeepNextCue(TArray<FShowCue>& Cues, int32 Index, const FFixtureStates& Before, TConstArrayView<FShowCueFixtureState> Entries,
		TFunctionRef<bool(int32 /*FixtureId*/, const FGameplayTag& /*Attribute*/, float& /*OutHome*/)> HomeValue);

	// --- Effects (Docs/ADR/0027-effects-engine.md §5) ---

	/**
	 * The effects running at Cues[Index]: from the nearest blocked cue at or before Index, each cue's effects start (a later entry
	 * with the same id replaces the earlier one and moves to the end, so it wins an attribute) and its stops end them. Empty for an
	 * index out of range. Cues must be sorted.
	 */
	MODULARSCENEBUILDER_API void EffectsAt(TConstArrayView<FShowCue> Cues, int32 Index, TArray<FStageEffectEntry>& OutEffects);

	/**
	 * Track store of effects: each of Effects replaces the cue's entry with its id (or is appended) and drops a stop of that id; each
	 * of Stops is added unless already there and drops the cue's effect of that id.
	 */
	MODULARSCENEBUILDER_API void MergeEffectsInto(FShowCue& Cue, TConstArrayView<FStageEffectEntry> Effects, TConstArrayView<FGuid> Stops);

	/**
	 * Cue Only for effects: the cue after Index keeps the effects it had before Effects and Stops were stored at Index. For each id
	 * stored that the next cue does not name itself (as an effect or a stop), the next cue gets Before's entry of that id (the
	 * effect as it ran), or a stop when Before had none. Nothing when there is no next cue or it is blocked. Returns how many
	 * entries it wrote.
	 */
	MODULARSCENEBUILDER_API int32 KeepNextCueEffects(TArray<FShowCue>& Cues, int32 Index, TConstArrayView<FStageEffectEntry> Before,
		TConstArrayView<FStageEffectEntry> Effects, TConstArrayView<FGuid> Stops);
}

/** Cue timing per attribute (Docs/ADR/0026 §2). Pure, any thread; covered by StageCraft.Show.CueTracking and StageCraft.Show.CueTiming. */
namespace StageCueTiming
{
	/**
	 * When Attribute moves in Cue: OutDelay after the cue starts, over OutDuration. The duration is the Out time for intensity going
	 * down (bGoingDown) when the cue has one, else the attribute's feature time, else the In time (FadeTime).
	 */
	MODULARSCENEBUILDER_API void GetAttributeTiming(const FShowCue& Cue, const FGameplayTag& Attribute, bool bGoingDown, float& OutDelay, float& OutDuration);

	/** GetAttributeTiming for a whole feature (Kind Intensity..Focus): how an effect on that feature fades in a cue (Docs/ADR/0027 §5). */
	MODULARSCENEBUILDER_API void GetFeatureTiming(const FShowCue& Cue, EStagePaletteKind Kind, bool bGoingDown, float& OutDelay, float& OutDuration);

	/** How far a move with Delay and Duration is at Elapsed seconds, 0..1 (a zero duration jumps at its delay). */
	MODULARSCENEBUILDER_API float GetProgress(float Elapsed, float Delay, float Duration);

	/** Progress shaped by the cue's curve: unchanged for Linear, smoothstep for S-curve. */
	MODULARSCENEBUILDER_API float ApplyCurve(EStageCueCurve Curve, float Progress);
}
