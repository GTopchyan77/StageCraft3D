// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/TimerHandle.h"
#include "Subsystems/WorldSubsystem.h"
#include "Show/ShowControlTypes.h"
#include "Show/StageEffectRunner.h"
#include "ShowControlSubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnShowFixturesChanged);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnShowCueListChanged);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnShowCueEvent, float, CueNumber);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnShowControlSourceChanged, EStageControlSource, NewSource);
/** A playback's recorded content or name changed (saved data: the stage has unsaved changes). */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnShowPlaybacksChanged);
/** Live desk state changed: a fader, a flash, a playback's position, the Grand Master or Blackout. Not saved, not undoable. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnShowDeskStateChanged);

/**
 * Show control for one world: fixture registry (IDs; addresses are UStageDMXPatchSubsystem's), cue list with crossfade playback,
 * and the DMX in/out boundary.
 *
 * A world subsystem because fixtures are world actors and a show belongs to a venue (level).
 * Fixtures register themselves on BeginPlay, so nothing iterates the world. It ticks only while
 * a fade, an armed Follow/Wait, a strobe, an effect or DMX output is running (IsTickable), never at idle.
 *
 * Signal flow (Internal):  cue list -> fixture attributes -> visuals, and -> DMX bridge -> wire
 * Signal flow (External):  wire -> DMX bridge -> ReceiveDMXUniverse -> fixture attributes -> visuals
 */
UCLASS()
class MODULARSCENEBUILDER_API UShowControlSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem / UWorldSubsystem Interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem / UWorldSubsystem Interface

	//~ Begin FTickableGameObject Interface
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override;
	virtual TStatId GetStatId() const override;
	//~ End FTickableGameObject Interface

	// --- Fixtures & patch ---

	/** Called by ALightingFixtureActor::BeginPlay. Assigns a free fixture ID and, if enabled, a free DMX address. */
	void RegisterFixture(class ALightingFixtureActor* Fixture);
	void UnregisterFixture(class ALightingFixtureActor* Fixture);

	UFUNCTION(BlueprintCallable, Category = "StageCraft|Show")
	TArray<class ALightingFixtureActor*> GetFixtures() const;

	/**
	 * Something that decides which fixtures cast shadows changed: a fixture's output (ALightingFixtureActor) or the camera
	 * (the controller). Cheap: it only arms one re-ranking within UStageShowPerformanceSettings' interval (Docs/ADR/0018 §4).
	 */
	void MarkShadowBudgetDirty();

	/** Fixtures whose beams cast shadows now, for diagnostics and the benchmark. */
	int32 GetShadowedFixtureCount() const;

	UFUNCTION(BlueprintPure, Category = "StageCraft|Show")
	class ALightingFixtureActor* FindFixture(int32 FixtureId) const;

	UFUNCTION(BlueprintPure, Category = "StageCraft|Show")
	bool IsFixtureIdFree(int32 FixtureId, const class ALightingFixtureActor* Ignore = nullptr) const;

	UFUNCTION(BlueprintPure, Category = "StageCraft|Show")
	int32 GetNextFreeFixtureId() const;

	// DMX addresses (free ranges, conflicts, auto-patch, universes) are UStageDMXPatchSubsystem's (Docs/ADR/0019 §2).

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Show")
	FOnShowFixturesChanged OnFixturesChanged;

	// --- Strobe clock ---

	/**
	 * Joins (bStrobing) or leaves the strobe clock. Called by fixtures when their Shutter attribute crosses 0. While at least one
	 * fixture strobes the subsystem ticks and gates each fixture's output at its own rate; with none it does not tick.
	 * A fixture that leaves is left open. Idempotent.
	 */
	void SetFixtureStrobing(class ALightingFixtureActor* Fixture, bool bStrobing);

	int32 GetStrobingFixtureCount() const { return Strobes.Num(); }

	/** Fraction of each strobe period the output is open: a short, punchy flash like a discharge strobe. */
	static constexpr float StrobeDutyCycle = 0.3f;

	// --- Cues ---

	// Cue edits from the UI go through AModularPlayerController (RequestStoreCue etc.), which asks the GameMode and records undo
	// (Docs/ADR/0011-cue-list-and-patch-sheet-export.md §2.2). Every entry point applies StageCueRules.

	/**
	 * Records a cue (Docs/ADR/0026-cue-timing-tracking-playbacks.md §3):
	 *  - Look: every fixture's current look (never the highlight) as a blocked cue, replacing a cue of that number (its timing kept);
	 *  - Track: what the programmer holds, merged into the cue (a new cue, or an existing one; an empty Label keeps its label);
	 *  - CueOnly: Track, and the next cue keeps its old values for what changed.
	 * False (nothing changes) for an invalid number, or a tracking store with an empty programmer. Does not clear the programmer.
	 */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Show")
	bool StoreCue(float CueNumber, const FText& Label, float FadeTime = 2.f, EStageCueStoreMode Mode = EStageCueStoreMode::Look);

	/** Changes a cue's label and In time; its recorded values are kept. False if no cue has CueNumber. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Show")
	bool UpdateCue(float CueNumber, const FText& Label, float FadeTime);

	/** Changes a cue's delay, Out time, feature times, trigger and curve (sanitized). False if no cue has CueNumber. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Show")
	bool UpdateCueTiming(float CueNumber, const FStageCueTiming& Timing);

	/**
	 * Blocks a cue (it inherits nothing; the look it inherited is written into it, so nothing on stage changes) or unblocks it (it
	 * inherits again). False if no cue has CueNumber.
	 */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Show")
	bool SetCueBlocked(float CueNumber, bool bBlocked);

	/** The tracked look at CueNumber (StageCueTracking::StateAt); empty for an unknown cue. */
	void GetCueState(float CueNumber, TMap<int32, FShowCueFixtureState>& OutState) const;

	UFUNCTION(BlueprintCallable, Category = "StageCraft|Show")
	bool DeleteCue(float CueNumber);

	/**
	 * Fades to the cue's tracked look with its timing; the next cue's Follow or Wait trigger is armed. Fails in External control mode
	 * or for an unknown number.
	 */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Show")
	bool GoToCue(float CueNumber);

	/** Plays the cue after the active one (the first cue if none is active). */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Show")
	bool Go();

	/** Plays the cue before the active one, with that cue's timing, like a console's Back key; arms no Follow or Wait. False at the first cue. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Show")
	bool Back();

	/** True while a Follow or Wait cue is waiting to start by itself. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Show")
	bool IsAutoGoArmed() const { return PendingAutoGo.IsSet(); }

	/** The cue with CueNumber, or null. The pointer is valid until the cue list next changes. */
	const FShowCue* FindCue(float CueNumber) const;

	/** Snaps a running fade to its end state. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Show")
	void FinishFade();

	/** Sorted by cue number. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Show")
	const TArray<FShowCue>& GetCues() const { return Cues; }

	/**
	 * Replaces the cue list when a show is loaded: a running fade snaps to its end and no cue is active afterwards. The list is
	 * repaired first (StageCueRules::SanitizeCueList), because it comes from a file.
	 */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Show")
	void SetCues(const TArray<FShowCue>& InCues);

	/**
	 * Replaces the cue list for undo / redo. Unlike SetCues, playback carries on: the active cue number and a running fade
	 * (which holds its own target values) are kept. Repaired like SetCues.
	 */
	void ReplaceCues(const TArray<FShowCue>& InCues);

	/** Number of the last cue started, or a negative value if none. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Show")
	float GetActiveCueNumber() const { return ActiveCueNumber; }

	UFUNCTION(BlueprintPure, Category = "StageCraft|Show")
	bool IsFading() const { return !Fades.IsEmpty(); }

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Show")
	FOnShowCueListChanged OnCueListChanged;

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Show")
	FOnShowCueEvent OnCueStarted;

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Show")
	FOnShowCueEvent OnCueFinished;

	/**
	 * Makes CueNumber the active cue at once, without a fade or OnCueStarted: scene load resumes the show where it was saved, so the
	 * cue's values become the playback layer again. False for an unknown number or in External control mode.
	 */
	bool RestoreActiveCue(float CueNumber);

	// --- Output mix (Docs/ADR/0025-programmer-and-output-mix.md §2) ---

	/**
	 * Re-mixes these fixtures' output from their home look, the playing cue, the programmer and the highlight (StageShowMix), writing
	 * only the values that change. This subsystem is the only writer of the mix; it runs on demand (programmer edits, highlight,
	 * control source), never per frame except for the attributes a fade is driving. Unknown IDs are ignored.
	 */
	void ResolveFixtures(TConstArrayView<int32> FixtureIds);

	/** ResolveFixtures for every registered fixture. */
	void ResolveAllFixtures();

	/**
	 * State's values with its palette references resolved (Docs/ADR/0028-palettes-and-references.md §3): each referenced attribute
	 * takes the palette's current value for that fixture, or keeps its stored value when the palette is gone or has none. A colour
	 * palette needs the fixture on stage to mix its colour; without it the stored value stays.
	 */
	TMap<FGameplayTag, float> ResolveReferences(const FShowCueFixtureState& State) const;

	/** What Fixture shows without the highlight: what a cue store and a palette record take (never the highlight). */
	TMap<FGameplayTag, float> GetRecordableValues(const class ALightingFixtureActor& Fixture) const;

	// --- Playbacks and masters (Docs/ADR/0026 §6) ---
	// Edits (record, clear, rename) and live actions (level, flash, go, release, masters) from the UI go through
	// AModularPlayerController, which asks the GameMode (EvaluateCueRequest) and records undo for edits only.

	/** The StagePlaybackRules::Count playbacks' recorded content. */
	const TArray<FStagePlayback>& GetPlaybacks() const { return Playbacks; }

	/**
	 * Appends a cue to Index's list holding Entries and Effects (a tracking cue: what the programmer held, its effects and stops).
	 * False for a bad index, nothing to record or a full list.
	 */
	bool RecordPlayback(int32 Index, TConstArrayView<FShowCueFixtureState> Entries, const FStageProgrammerEffects& Effects = FStageProgrammerEffects());

	/** Releases Index and deletes its cues. False for a bad index or an empty playback. */
	bool ClearPlayback(int32 Index);

	/** False for a bad index. An empty name is allowed (the desk shows the number). */
	bool RenamePlayback(int32 Index, const FText& Name);

	/** Replaces every playback's content: a load (SetPlaybacks releases them all) or an undo step (ReplacePlaybacks keeps live state). Sanitized. */
	void SetPlaybacks(const TArray<FStagePlayback>& InPlaybacks);
	void ReplacePlaybacks(const TArray<FStagePlayback>& InPlaybacks);

	/** Fader level 0..1. Raising it from 0 activates the playback at its first cue and makes it the latest for LTP. False for a bad index. */
	bool SetPlaybackLevel(int32 Index, float Level);

	/** Flash: the playback at full while held (activating it like a fader). False for a bad index. */
	bool SetPlaybackFlash(int32 Index, bool bDown);

	/** Steps the playback to its next cue (its first when released) at once, and makes it the latest for LTP. False at the end or when empty. */
	bool PlaybackGo(int32 Index);

	/** Releases the playback: fader to 0, flash off, back before its first cue. False for a bad index. */
	bool ReleasePlayback(int32 Index);

	float GetPlaybackLevel(int32 Index) const;
	bool IsPlaybackFlashing(int32 Index) const;
	/** Index of the playback's current cue in its list; INDEX_NONE while released. */
	int32 GetPlaybackCueIndex(int32 Index) const;
	/** Whether the playback contributes to the stage now (fader up or flashed, at a cue). */
	bool IsPlaybackActive(int32 Index) const;

	/** Grand Master 0..1: scales every fixture's intensity last. Not saved: a stage always opens at full. */
	void SetGrandMaster(float Level);
	float GetGrandMaster() const { return GrandMaster; }

	void SetBlackout(bool bOn);
	bool IsBlackout() const { return bBlackout; }

	/**
	 * AUDIO master 0..1 (Docs/ADR/0029 §7): scales every effect that follows the music towards how it runs without music (Modulate
	 * fully open; at 0 Sync keeps its last tempo and Trigger holds). Not saved: a stage always opens at full.
	 */
	void SetAudioMaster(float Level);
	float GetAudioMaster() const { return AudioMaster; }

	/** AUDIO KILL: the AUDIO master at 0 while on, without moving the fader. Not saved. */
	void SetAudioKill(bool bOn);
	bool IsAudioKilled() const { return bAudioKill; }

	/** The flash limit the effects apply now: this PC's setting (StageAudioReactiveSettings::FlashLimitHz), pushed on every change. */
	float GetEffectFlashLimitHz() const { return EffectRunner.GetFlashLimitHz(); }

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Show")
	FOnShowPlaybacksChanged OnPlaybacksChanged;

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Show")
	FOnShowDeskStateChanged OnDeskStateChanged;

	// --- Effects (Docs/ADR/0027-effects-engine.md §3, §5) ---
	// Effects run from the main cue list's active cue, the active playbacks and the programmer (whose stops hold any of them off);
	// this subsystem keeps their clocks and fades and writes them into the mix. Under External DMX they hold still and are not
	// written: the console owns the fixtures.

	/** True while an effect runs (or fades out) and is written (Internal control). */
	bool AreEffectsLive() const { return EffectRunner.IsRunning() && ControlSource == EStageControlSource::Internal; }

	/** The clock of a running effect in cycles (0 when it is not running), for previews. */
	double GetEffectCycles(const FGuid& EffectId) const { return EffectRunner.GetCycles(EffectId); }

	/** The fade weight of a running effect now, 0..1 (0 when it is not running). */
	float GetEffectWeight(const FGuid& EffectId) const { return EffectRunner.GetWeight(EffectId); }

	/** Every running effect with its source, in mix order. */
	void GetRunningEffects(TArray<FStageRunningEffect>& OutEffects) const { EffectRunner.GetRunning(OutEffects); }

	/**
	 * The effect with EffectId that the main cue list's active cue or an active playback runs (the latest playback first), whether or
	 * not the programmer stops or overrides it. Null when none does. Valid until the next cue, playback or cue list change.
	 */
	const FStageEffectEntry* FindShowEffect(const FGuid& EffectId) const;

	// --- DMX ---

	/** Installs (or with nullptr removes) the transport. The previous bridge is stopped. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|DMX")
	void SetDMXBridge(class UStageDMXBridge* NewBridge);

	UFUNCTION(BlueprintPure, Category = "StageCraft|DMX")
	class UStageDMXBridge* GetDMXBridge() const { return DMXBridge; }

	UFUNCTION(BlueprintCallable, Category = "StageCraft|DMX")
	void SetControlSource(EStageControlSource NewSource);

	UFUNCTION(BlueprintPure, Category = "StageCraft|DMX")
	EStageControlSource GetControlSource() const { return ControlSource; }

	/**
	 * Inbound DMX frame (from the bridge). Applied only in External control mode, and only to the fixtures patched in Universe
	 * whose own channels differ from the last frame of that universe: consoles repeat unchanged frames ~44 times a second, and
	 * those cost nothing (Docs/ADR/0019 §4). Returns the number of fixtures updated.
	 */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|DMX")
	int32 ReceiveDMXUniverse(int32 Universe, const TArray<uint8>& Channels);

	/** Encodes every fixture patched in Universe into a 512-slot buffer. Also useful for debugging patches. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|DMX")
	TArray<uint8> RenderDMXUniverse(int32 Universe) const;

	/**
	 * Has every patched universe sent again on the next output frame (Docs/ADR/0019 §9.2): called when the output opens or the
	 * universes it sends change. Otherwise a universe is rendered only after a fixture in it changed, and the sender keeps the
	 * stream alive in between, so an idle show neither ticks nor renders.
	 */
	void RefreshDMXOutput();

	/** DMX refresh rate for output. 44 Hz is the practical maximum of a full 512-slot DMX512 line. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StageCraft|DMX", meta = (ClampMin = "1.0", ClampMax = "60.0", Units = "Hz"))
	float DMXOutputRateHz = 44.f;

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|DMX")
	FOnShowControlSourceChanged OnControlSourceChanged;

	// --- Diagnostics ---

	/** Time spent in Tick (fades, strobes, DMX output) since the last reset. */
	struct FTickCostStats
	{
		int32 Ticks = 0;
		double TotalSeconds = 0.0;
		double MaxSeconds = 0.0;
	};

	/**
	 * Measured in development builds only, for the fixture benchmark (Docs/ADR/0012-production-reports-and-fixture-benchmark.md
	 * §3); always zero in Shipping. Reading or resetting it does not change playback.
	 */
	const FTickCostStats& GetTickCostStats() const { return TickCostStats; }
	void ResetTickCostStats() { TickCostStats = FTickCostStats(); }

	/**
	 * Which layer of the output mix gives FixtureId's Attribute its value now, and whether an effect or the masters changed it
	 * (Docs/ADR/0030 §5). Built from the same layers the show writes, so it agrees with the fixture's output (a cue fade part-way
	 * through included). bFound false for an unknown fixture or an attribute no layer holds. Read-only; game thread.
	 */
	StageShowMix::FAttributeExplanation ExplainAttribute(int32 FixtureId, const FGameplayTag& Attribute) const;

	/** The number of the main cue list's cue that gives FixtureId's Attribute its value at the active cue (tracked), if one does. */
	TOptional<float> FindSettingCue(int32 FixtureId, const FGameplayTag& Attribute) const;

	/** Ticks that read the music for audio effects (Docs/ADR/0029 §4), for tests that prove no read happens without one. Development only. */
	int64 GetAudioReadsForTests() const { return AudioReadsForTests; }

protected:
	//~ Begin UWorldSubsystem Interface
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	//~ End UWorldSubsystem Interface

private:
	struct FAttributeLerp
	{
		FGameplayTag Attribute;
		float From = 0.f;
		float To = 0.f;
		/** Seconds after the cue started before this attribute moves, and how long it takes (StageCueTiming::GetAttributeTiming). */
		float Delay = 0.f;
		float Duration = 0.f;
	};

	struct FAttributeFade
	{
		TWeakObjectPtr<class ALightingFixtureActor> Fixture;
		/** Attributes whose start and target differ: the only ones a fade frame writes, so unchanged ones cost nothing. */
		TArray<FAttributeLerp> Changing;
		/** Every target, applied once when the fade completes: the cue's look, and the home look where the cue is silent. */
		TMap<FGameplayTag, float> To;
		/** What the playback layer holds when the fade completes: the cue's look only, so the home look shows through elsewhere. */
		TMap<FGameplayTag, float> Final;
	};

	/** How a cue was started: forward (Go, Go To, a Follow or Wait) arms the next cue's trigger; Back never does. */
	enum class ECueStart : uint8
	{
		Forward,
		Back,
	};

	/** One playback's live state (not saved). */
	struct FPlaybackRuntime
	{
		float Level = 0.f;
		bool bFlash = false;
		int32 CueIndex = INDEX_NONE;
		/** Activation order for LTP; the latest activated or stepped playback wins. */
		uint32 Order = 0;
		/** The current cue's tracked look, palette references resolved, per fixture ID. */
		TMap<int32, TMap<FGameplayTag, float>> Output;
		/** The current cue's tracked effects (StageCueTracking::EffectsAt). */
		TArray<FStageEffectEntry> Effects;

		bool IsActive() const { return (Level > UE_KINDA_SMALL_NUMBER || bFlash) && CueIndex != INDEX_NONE; }
		float GetEffectiveLevel() const { return bFlash ? 1.f : Level; }
	};

	/** The next cue waiting to start by itself (Docs/ADR/0026 §2). */
	struct FPendingAutoGo
	{
		float CueNumber = 0.f;
		/** Follow: waiting for the playing cue to finish; then Remaining is 0 and it fires on the next tick. */
		bool bAfterFinish = false;
		/** Seconds left before it starts. */
		float Remaining = 0.f;
	};

	struct FStrobeState
	{
		TWeakObjectPtr<class ALightingFixtureActor> Fixture;

		/** Position in the current period, 0..1. */
		double Phase = 0.0;
		bool bOpen = true;
	};

	bool IsDMXOutputActive() const;
	void TickStrobes(float DeltaTime);
	void TickFades(float DeltaTime);
	void TickAutoGo(float DeltaTime);
	/** Writes every effect-driven fixture's effect attributes for this frame (the clocks were advanced at the start of Tick). */
	void TickEffects();
	void TickDMXOutput(float DeltaTime);

	/** Writes Values to Fixture (only what differs is applied) and, if anything changed, marks its universe for output. */
	void WriteFixtureOutput(class ALightingFixtureActor& Fixture, const TMap<FGameplayTag, float>& Values);

	/** Encodes every fixture patched in Universe into Data (512 slots, zeroed first). */
	void RenderDMXUniverseInto(int32 Universe, TArrayView<uint8> Data) const;

	/** Whether output has universes waiting to be rendered (or the patch changed since the last frame). */
	bool HasPendingDMXOutput() const;
	void CompleteFade();

	/** Starts Cues[Index]: fades every fixture of its tracked look (and of the playback layer) with the cue's timing. */
	bool PlayCue(int32 Index, ECueStart Start);

	/** Every fixture's look for a Look store, with the palette references that produced it. */
	TArray<FShowCueFixtureState> CaptureLook() const;

	/** Grand Master x Blackout: what intensity is scaled by last. */
	float GetIntensityScale() const { return bBlackout ? 0.f : GrandMaster; }

	/**
	 * True when no playback is up and the masters are at full: a fade frame may then be written straight onto fixtures. Otherwise a
	 * fading fixture is re-mixed, so HTP, LTP and the masters apply to every frame.
	 */
	bool IsMainOnlyMix() const;

	/** Recomputes Index's Output from its current cue and adds every fixture it touched (before or after) to OutChanged. */
	void RefreshPlaybackOutput(int32 Index, TArray<int32>& OutChanged);

	/** Re-mixes the fixtures playback Index contributes to; broadcasts OnDeskStateChanged. */
	void ApplyPlaybackChange(int32 Index, TArray<int32> Changed);

	/** The masters changed: re-mix everything (Internal), or have the next DMX frames re-read in full and scaled (External). */
	void ApplyMastersChange();

	/** Scales Fixture's intensity attributes by the masters (External DMX input, which bypasses the mix). */
	void ApplyMastersToInput(class ALightingFixtureActor& Fixture) const;
	/** Ranks the fixtures (StageShadowBudget::SelectCasters) and switches only those whose shadows change. */
	void UpdateShadowBudget();
	void AssignSanitizedCues(const TArray<FShowCue>& InCues, const TCHAR* Source);
	int32 FindCueIndex(float CueNumber) const;

	void HandleProgrammerFixturesChanged(TConstArrayView<int32> FixtureIds);

	/** The programmer's effects or stops changed: the effects re-gather at once (RefreshEffectSources). */
	void HandleProgrammerEffectsChanged();

	/** The audio settings may have changed: the effects take this PC's flash limit (Docs/ADR/0029 §7). */
	void HandleAudioStatusChanged();

	/**
	 * Hands the runner every source's effects (Docs/ADR/0027 §5): the main list's, the active playbacks' in activation order, the
	 * programmer's, the same id once as its latest, the programmer's stops left out. TimedBy (the cue being played) fades what it
	 * changes; null changes it at once. When anything but a fader's scale changed, every fixture the effects drove or drive re-mixes.
	 */
	void RefreshEffectSources(const FShowCue* TimedBy);

	/** Every effect running on stage now and not fading out, in mix order (a Look store records them). */
	TArray<FStageEffectEntry> CaptureEffects() const;

	/** Finds the effects' fixtures again after one arrived, left or was renumbered. */
	void RefreshEffectFixtures();

	/** Which attributes ResolveFixture writes. */
	enum class EResolveScope : uint8
	{
		/** Every attribute of the fixture. */
		All,
		/** Only those an effect drives on it: the per-frame effect write. */
		EffectAttributes,
	};

	/** Re-mixes one fixture (ResolveFixtures). In External mode only the programmer and highlight are written, so the console's look stays. */
	void ResolveFixture(class ALightingFixtureActor& Fixture, EResolveScope Scope = EResolveScope::All);

	/** Storage the layers of one fixture point into while it is mixed or explained. */
	struct FLayerScratch
	{
		TMap<FGameplayTag, float> Highlight;
		TMap<FGameplayTag, float> Programmer;
		StageShowMix::FEffectMap Effects;
	};

	/** The layers ResolveFixture mixes for Fixture now (under External: only what the user holds by hand), pointing into Scratch. */
	void BuildLayers(const class ALightingFixtureActor& Fixture, FLayerScratch& Scratch, StageShowMix::FFixtureLayers& OutLayers) const;

	/**
	 * The values the highlight shows on Fixture: full dimmer, open shutter and the fixture's own full white. Stored in OutValues so
	 * the mix can point at them.
	 */
	static void MakeHighlightValues(const class ALightingFixtureActor& Fixture, TMap<FGameplayTag, float>& OutValues);

	/** Removes from Frame the attributes the programmer or the highlight holds on Fixture, so a cue never drives them. */
	void RemoveHeldAttributes(const class ALightingFixtureActor& Fixture, TMap<FGameplayTag, float>& Frame) const;

	/** Where a fade of Attribute starts and Clear returns to: the playback layer, else the home look, else the output. */
	float GetPlaybackValue(const class ALightingFixtureActor& Fixture, const FGameplayTag& Attribute) const;

	/** Replaces InOutValues' referenced attributes with their palettes' values for FixtureId (Fixture, if on stage, mixes colours). */
	void ApplyReferences(const class ALightingFixtureActor* Fixture, int32 FixtureId, const TMap<FGameplayTag, FGuid>& References, TMap<FGameplayTag, float>& InOutValues) const;

	/** The programmer layer of Fixture with its references resolved (into Scratch when it has any), or null when it holds nothing. */
	const TMap<FGameplayTag, float>* GetProgrammerLayer(const class ALightingFixtureActor& Fixture, TMap<FGameplayTag, float>& Scratch) const;

	/**
	 * Re-resolves the playing cue's palette references into the playback layer (all fixtures, or only OnlyFixtureId), so a changed
	 * palette or a fixture that arrived after the cue started shows the palette's current value.
	 */
	void RefreshPlaybackReferences(int32 OnlyFixtureId = INDEX_NONE);

	/** A palette changed (Docs/ADR/0028 §3): a running fade snaps to its end, the playing cue's references re-resolve, every fixture re-mixes. */
	UFUNCTION()
	void HandlePalettesChanged();

	TArray<TWeakObjectPtr<class ALightingFixtureActor>> Fixtures;

	/** The addressing this show reads and writes DMX by. Resolved in Initialize; same world, so it outlives every use. */
	UPROPERTY(Transient)
	TObjectPtr<class UStageDMXPatchSubsystem> Patch = nullptr;

	/** The programmer layer of the mix (Docs/ADR/0025 §3). Resolved in Initialize; same world. */
	UPROPERTY(Transient)
	TObjectPtr<class UStageProgrammerSubsystem> Programmer = nullptr;

	/** The music audio effects follow (Docs/ADR/0029 §4), read once per tick while one runs. Resolved in Initialize; same world. */
	UPROPERTY(Transient)
	TObjectPtr<class UStageAudioReactiveSubsystem> AudioReactive = nullptr;

	FDelegateHandle ProgrammerChangedHandle;
	FDelegateHandle ProgrammerEffectsHandle;
	FDelegateHandle AudioStatusHandle;

	/** The running effects' clocks, fades and per-fixture output (Docs/ADR/0027 §3, §5). */
	FStageEffectRunner EffectRunner;

	/** The main cue list's effects at the active cue (StageCueTracking::EffectsAt): changed by Go, Back, Go To, restore and load only. */
	TArray<FStageEffectEntry> MainEffects;

	/** The palettes references resolve against (Docs/ADR/0028). Resolved in Initialize; same world. */
	UPROPERTY(Transient)
	TObjectPtr<class UStageShowLibrarySubsystem> Library = nullptr;

	/**
	 * The playback layer: per fixture ID, the values the cue list has set (the active cue's, part-way through a fade while one runs).
	 * Kept by ID like cues, so a re-spawned fixture finds its values. Cleared when a show is loaded (SetCues).
	 */
	TMap<int32, TMap<FGameplayTag, float>> PlaybackValues;

	/**
	 * The last frame received per universe, to find which fixtures' channels changed. Forgotten when the patch changes
	 * (PatchRevision) or the control source does, so the next frame is applied in full.
	 */
	TMap<int32, TArray<uint8>> LastInputFrames;
	uint32 LastInputPatchRevision = 0;

	UPROPERTY(Transient)
	TArray<FShowCue> Cues;

	UPROPERTY(Transient)
	TObjectPtr<class UStageDMXBridge> DMXBridge = nullptr;

	TArray<FAttributeFade> Fades;
	TArray<FStrobeState> Strobes;
	/** The longest delay + duration of the running fade's attributes. */
	float FadeDuration = 0.f;
	float FadeElapsed = 0.f;
	EStageCueCurve FadeCurve = EStageCueCurve::Linear;
	TOptional<FPendingAutoGo> PendingAutoGo;

	/** Saved: what each playback holds (StagePlaybackRules::Count of them). */
	TArray<FStagePlayback> Playbacks;
	/** Live: one per playback. */
	TArray<FPlaybackRuntime> PlaybackRuntime;
	uint32 ActivationCounter = 0;
	float GrandMaster = 1.f;
	bool bBlackout = false;
	float AudioMaster = 1.f;
	bool bAudioKill = false;
	float ActiveCueNumber = -1.f;
	float DMXOutputAccumulator = 0.f;
	/** Universes whose fixtures changed since they were last sent (Internal output only). */
	TSet<int32> DirtyOutputUniverses;
	/** The patch revision the output last rendered against: a re-patch sends every universe again. */
	uint32 LastOutputPatchRevision = 0;
	/** One universe being rendered; reused, so output does not allocate per frame. */
	TArray<uint8> OutputScratch;
	EStageControlSource ControlSource = EStageControlSource::Internal;
	FTickCostStats TickCostStats;
	int64 AudioReadsForTests = 0;

	/** One pending re-ranking at most; armed by MarkShadowBudgetDirty, cleared in Deinitialize. */
	FTimerHandle ShadowBudgetTimer;

	/** World time (s) at which the budget last switched each fixture's shadows, for the minimum hold. Pruned on unregister. */
	TMap<TWeakObjectPtr<class ALightingFixtureActor>, double> ShadowSwitchTimes;

	/** Arms the re-ranking timer DelaySeconds from now unless one is already armed. */
	void ArmShadowBudgetTimer(float DelaySeconds);
};
