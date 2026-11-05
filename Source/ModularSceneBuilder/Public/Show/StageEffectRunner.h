// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Show/ShowControlTypes.h"
#include "Show/StageEffectTypes.h"
#include "Show/StageEffects.h"
#include "Show/StageShowMix.h"

/** Where a running effect comes from (Docs/ADR/0027-effects-engine.md §5). */
enum class EStageEffectSource : uint8
{
	/** The main cue list's active cue (tracked). */
	Cue,
	/** A playback fader's active cue. */
	Playback,
	Programmer,
};

/** An effect as the show runs it, for panels (UShowControlSubsystem::GetRunningEffects). */
struct FStageRunningEffect
{
	FStageEffectEntry Entry;
	EStageEffectSource Source = EStageEffectSource::Programmer;
	/** The playback index (0-based) for Source Playback, else INDEX_NONE. */
	int32 Playback = INDEX_NONE;
	/** Fading out after a cue stopped it; gone when the fade ends. */
	bool bStopping = false;
};

/**
 * The running effects of one show (Docs/ADR/0027-effects-engine.md §3, §5): each effect's clock and fade, and what it gives each of
 * its fixtures now as an effect layer of the output mix (StageShowMix::FEffectMap). Owned by UShowControlSubsystem, which hands it the
 * effects of every source (main cue list, playbacks, programmer), advances it from its tick and writes the mix; the runner never
 * touches a fixture's output. Game thread.
 *
 * The clock is counted in cycles and advanced by each effect's own rate, so changing the rate keeps the pattern where it is; an
 * effect that stays (same Id, from any source) keeps its clock through any edit or cue change.
 */
class MODULARSCENEBUILDER_API FStageEffectRunner
{
public:
	/** Looks a fixture up by ID on the stage; null when none has it. */
	using FFindFixture = TFunctionRef<class ALightingFixtureActor*(int32 /*FixtureId*/)>;

	/** One effect to run, with its source. Ids are unique across the list (the show merges sources first). */
	struct FSourcedEntry
	{
		FStageEffectEntry Entry;
		EStageEffectSource Source = EStageEffectSource::Programmer;
		int32 Playback = INDEX_NONE;
		/** Scales the effect's intensity values: the playback fader's level; 1 otherwise. */
		float IntensityScale = 1.f;
	};

	/**
	 * Replaces the running effects with Entries, in order (a later one wins an attribute). Effects already running keep their clock.
	 * With TimedBy (a cue being played) every effect that starts, changes or stops fades with that cue's timing (a change morphs from
	 * the old settings, a stop fades out and is then dropped); without it they change at once. An effect left as it was keeps any fade
	 * it is in, and one already fading out finishes doing so. Returns true when anything changed but a playback's scale (an effect started,
	 * stopped, changed its settings, fixtures or source): then the fixtures it drove need a full re-mix.
	 */
	bool SetEffects(TConstArrayView<FSourcedEntry> Entries, FFindFixture FindFixture, const FShowCue* TimedBy);

	/** Finds every effect's fixtures again: one arrived, left, or changed its ID. */
	void RefreshFixtures(FFindFixture FindFixture);

	bool IsRunning() const { return !Running.IsEmpty(); }

	/**
	 * Moves every clock on by its rate and every fade on by DeltaSeconds. Effects whose fade-out ended are dropped; their fixtures'
	 * IDs are added to OutReleased (each once), as those fixtures need a full re-mix.
	 *
	 * Audio is this frame's music (Docs/ADR/0029 §4), read by effects whose audio mode is not Off: Sync runs the clock at the tempo
	 * and pulls it to the beat, Trigger steps it on beats or onsets, Modulate scales the level range, size or rate by a band. Null
	 * (or no signal) makes them fall back: Sync keeps its last good tempo or its own rate, Trigger holds, Modulate reads the band as 1.
	 */
	void Advance(float DeltaSeconds, TArray<int32>& OutReleased, const StageEffects::FAudioInput* Audio = nullptr);

	/** An effect running now follows the music: the show then hands Advance a snapshot of it. */
	bool NeedsAudio() const { return bNeedsAudio; }

	bool DrivesFixture(int32 FixtureId) const { return ByFixture.Contains(FixtureId); }

	/** A fixture an effect drives, and the ID it was found under (a different ID now means it was renumbered). */
	struct FDrivenFixture
	{
		TWeakObjectPtr<class ALightingFixtureActor> Fixture;
		int32 FixtureId = 0;
	};

	/** The fixtures on stage that effects drive, each once. Changes on SetEffects, RefreshFixtures and a fade-out ending only. */
	const TArray<FDrivenFixture>& GetDrivenFixtures() const { return DrivenFixtures; }

	/**
	 * What the effects give Fixture now (OutValues reset first): Position adds degrees to pan and tilt; Intensity and Beam set each
	 * attribute between Low and High of the fixture's range; Color sets the fixture's own match of the colour. Each value carries its
	 * effect's fade weight; where two effects drive one attribute they compose in order (StageShowMix::ComposeEffect). False when no
	 * effect drives Fixture.
	 */
	bool Evaluate(const class ALightingFixtureActor& Fixture, StageShowMix::FEffectMap& OutValues) const;

	/** The clock of the effect with Id in cycles, or 0 when it is not running (for the panel's preview). */
	double GetCycles(const FGuid& Id) const;

	/** The fade weight of the effect with Id now (0..1), or 0 when it is not running. */
	float GetWeight(const FGuid& Id) const;

	/** Every running effect with its source, in mix order (those fading out first). */
	void GetRunning(TArray<FStageRunningEffect>& OutEffects) const;

private:
	/** A cue's fade of one effect: its weight from From to To, and (for a change) its settings from MorphFrom to the entry's. */
	struct FTransition
	{
		float From = 0.f;
		float To = 1.f;
		float Delay = 0.f;
		float Duration = 0.f;
		float Elapsed = 0.f;
		EStageCueCurve Curve = EStageCueCurve::Linear;
		TOptional<FStageEffect> MorphFrom;

		/** 0..1 through the fade, shaped by the curve. */
		float GetAlpha() const;
		bool IsDone() const { return Elapsed >= Delay + Duration; }
	};

	struct FRunning
	{
		FSourcedEntry Source;
		double Cycles = 0.0;
		bool bStopping = false;
		TOptional<FTransition> Transition;

		// Audio state (Docs/ADR/0029 §4), used only while the effect's audio mode is not Off.
		/** Modulate: the band after the effect's own response smoothing; 1 (no signal) until the music says otherwise. */
		float SmoothedBand = 1.f;
		/** Sync: the last tempo followed, in cycles per second; 0 until there was one. */
		double LastGoodHz = 0.0;
		/** Trigger: the event count at the last step, valid once primed (a source that starts or restarts primes without a step). */
		int64 LastTriggerEvents = 0;
		bool bTriggerPrimed = false;
		/** Modulate Level or Size: the settings with the band applied, refreshed every Advance; unset otherwise. */
		TOptional<FStageEffect> Modulated;

		float GetWeight() const;
		/** The settings evaluated now: modulated when Modulate applies, else the entry's own. */
		const FStageEffect& GetEvaluated() const { return Modulated.IsSet() ? Modulated.GetValue() : Source.Entry.Effect; }
	};

	/** Moves Item's clock on by DeltaSeconds at Rate (cycles per second), following Audio where its effect uses audio. */
	static void AdvanceClock(FRunning& Item, double Rate, float DeltaSeconds, const StageEffects::FAudioInput& Audio);

	/** Refreshes Item.Modulated from its smoothed band (Modulate Level or Size), or clears it. */
	static void UpdateModulated(FRunning& Item);

	/** One effect's hold on a fixture: which effect, and the fixture's place in its order. */
	struct FDrive
	{
		int32 Effect = INDEX_NONE;
		int32 Index = INDEX_NONE;
	};

	/**
	 * Starts a fade of Item's weight from From to To with TimedBy's timing for Item's feature (and its settings from MorphFrom). False,
	 * with no fade armed, when the cue's time for it is zero.
	 */
	static bool StartTransition(FRunning& Item, const FShowCue& TimedBy, float From, float To, TOptional<FStageEffect> MorphFrom);

	/** Rebuilds ByFixture from Running; DrivenFixtures from FindFixture, or (without one) filtered from the current list. */
	void RebuildDrives(const FFindFixture* FindFixture);

	/** Calls Emit(Attribute, Value, bRelative) for every value Effect gives Fixture at Index of Count on clock Cycles. */
	static void EvaluateEffect(const class ALightingFixtureActor& Fixture, const FStageEffect& Effect, int32 Index, int32 Count, double Cycles,
		TFunctionRef<void(const FGameplayTag& /*Attribute*/, float /*Value*/, bool /*bRelative*/)> Emit);

	void EvaluateDrive(const class ALightingFixtureActor& Fixture, const FDrive& Drive, StageShowMix::FEffectMap& OutValues) const;

	TArray<FRunning> Running;
	/** Per fixture ID, the effects driving it in mix order. */
	TMap<int32, TArray<FDrive, TInlineAllocator<2>>> ByFixture;
	TArray<FDrivenFixture> DrivenFixtures;
	/** Any running effect uses audio; refreshed whenever Running changes. */
	bool bNeedsAudio = false;

	void RefreshNeedsAudio();
};
