// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AI/StageAILanguage.h"
#include "Audio/StageAudioReactiveTypes.h"
#include "Show/ShowControlTypes.h"
#include "Show/StageDMXNetworkTypes.h"

/** How much a finding matters (Docs/ADR/0030 §5). Ordered: a higher value is worse. */
enum class EStageDiagnosticSeverity : uint8
{
	/** Worth knowing; nothing is wrong (effects hold while a console drives the show). */
	Info,
	/** Probably not what the user wants (Blackout on, a cue naming fixtures that are gone). */
	Warning,
	/** Something does not work (a patch conflict, DMX OUT failed, DMX IN followed but off). */
	Error,
};

/** What was found. Stable: tests and the assistant name findings by code. */
enum class EStageDiagnosticCode : uint8
{
	/** Fixtures whose DMX addresses overlap, or run past channel 512. */
	PatchConflict,
	/** Fixtures without an address while DMX goes in or out: a real fixture gets nothing, a console reaches nothing. */
	Unpatched,
	/** The show follows DMX IN (External) but the input is off. */
	DMXInOff,
	/** The DMX input could not open its ports. */
	DMXInFailed,
	/** The show follows DMX IN, but patched universes have no live source: their fixtures hold their last values. */
	DMXInNoSignal,
	/** The DMX output could not open or lost its socket. */
	DMXOutFailed,
	/** DMX OUT sends, but universes with patched fixtures are muted: those fixtures get nothing. */
	UniverseMuted,
	/** DMX OUT is on but sends nothing while a console drives the show (External). */
	OutputPausedUnderExternal,
	/** Effects are running but hold still while a console drives the show (External). */
	EffectsHeldUnderExternal,
	/** Cues follow palettes that no longer exist: they use the value stored with the cue instead. */
	PaletteReferenceMissing,
	/** Cues name fixture IDs that are not on stage. */
	CueNamesMissingFixture,
	BlackoutOn,
	/** The Grand Master is at or below GrandMasterLowLevel. */
	GrandMasterLow,
	/** The music source failed. */
	AudioSourceFailed,
	/** Effects follow the music but no source is chosen. */
	AudioNoSource,
	/** Effects follow the music, the source runs, but it hears nothing. */
	AudioNoSignal,
	/** Effects follow the music, but AUDIO KILL is on. */
	AudioKilled,
	/** The programmer holds values: those fixtures do not follow cues or playbacks until it is cleared. */
	ProgrammerHolds,
};

/** Where a finding is fixed, for the Problems list's GO TO and the assistant's SHOW ME. UI-free: the UI maps it to a panel. */
enum class EStageDiagnosticPlace : uint8
{
	None,
	Patch,
	DMXIn,
	DMXOut,
	/** The Cues panel's list. */
	Cues,
	/** The Cues panel's INTERNAL / EXTERNAL DMX switch. */
	ControlSource,
	Playback,
	GrandMaster,
	Blackout,
	Audio,
	AudioKill,
	Programmer,
	/** The Effects panel. */
	Effects,
};

/** One finding. Subjects are what GO TO selects and opens. */
struct MODULARSCENEBUILDER_API FStageDiagnostic
{
	EStageDiagnosticSeverity Severity = EStageDiagnosticSeverity::Info;
	EStageDiagnosticCode Code = EStageDiagnosticCode::BlackoutOn;
	EStageDiagnosticPlace Place = EStageDiagnosticPlace::None;
	/** Fixture IDs concerned, ascending, each once. */
	TArray<int32> FixtureIds;
	/** Cue numbers concerned, ascending (main list, or Playback's list when Playback is set). */
	TArray<float> Cues;
	/** 0-based playback index for a finding in a playback's cues, else INDEX_NONE. */
	int32 Playback = INDEX_NONE;
	/** Universes concerned, ascending. */
	TArray<int32> Universes;
	/** The failing system's own message (DMX, audio), when it gave one. */
	FText Error;
	/** GrandMasterLow: the level, 0..1. */
	float Level = 0.f;

	bool operator==(const FStageDiagnostic& Other) const;
};

/**
 * What diagnostics read: a copy of the show's state at one moment, gathered by UStageShowDiagnosticsSubsystem (or built by a
 * test). Plain data; the cue and playback views must outlive Run.
 */
struct MODULARSCENEBUILDER_API FStageShowSnapshot
{
	struct FFixture
	{
		int32 Id = 0;
		FStageDMXPatch Patch;
	};

	/** Every fixture on stage. */
	TArray<FFixture> Fixtures;
	/** UStageDMXPatchSubsystem::FindPatchConflicts. */
	TArray<int32> ConflictingFixtureIds;

	TConstArrayView<FShowCue> Cues;
	TConstArrayView<FStagePlayback> Playbacks;
	/** Every palette that exists. */
	TSet<FGuid> Palettes;
	/** Fixtures the programmer holds values for. */
	TArray<int32> ProgrammerFixtureIds;

	EStageControlSource ControlSource = EStageControlSource::Internal;
	EStageDMXInputState InputState = EStageDMXInputState::Off;
	FText InputError;
	TArray<int32> LiveUniverses;
	EStageDMXOutputState OutputState = EStageDMXOutputState::Off;
	FText OutputError;
	/** FStageDMXOutputSettings::MutedUniverses. */
	TArray<int32> MutedUniverses;

	float GrandMaster = 1.f;
	bool bBlackout = false;
	bool bEffectsRunning = false;
	/** A running effect follows the music (its audio mode is not Off). */
	bool bAudioEffectsRunning = false;
	bool bAudioKill = false;

	EStageAudioSource AudioSource = EStageAudioSource::Off;
	EStageAudioSourceState AudioState = EStageAudioSourceState::Off;
	FText AudioError;
	bool bAudioSignal = false;
};

/**
 * Show diagnostics (Docs/ADR/0030 §5): deterministic checks of a snapshot for the mistakes that make a show look or behave wrong.
 * Pure, any thread; covered by StageCraft.Show.Diagnostics. Works without the assistant: the Problems panel lists the same
 * findings the assistant reads.
 */
namespace StageShowDiagnostics
{
	/** GrandMasterLow fires at or below this level. */
	inline constexpr float GrandMasterLowLevel = 0.2f;

	/** Every finding, worst first (then in code order); empty for a clean show. */
	MODULARSCENEBUILDER_API TArray<FStageDiagnostic> Run(const FStageShowSnapshot& Snapshot);

	/** One sentence for Diagnostic, in Language: what is wrong, then what to do. */
	MODULARSCENEBUILDER_API FText Describe(const FStageDiagnostic& Diagnostic, EStageAILanguage Language);

	/** "1, 2, 3 and 4 more" for at most MaxShown numbers. */
	MODULARSCENEBUILDER_API FString JoinNumbers(TConstArrayView<int32> Numbers, EStageAILanguage Language, int32 MaxShown = 8);
	MODULARSCENEBUILDER_API FString JoinCues(TConstArrayView<float> Numbers, EStageAILanguage Language, int32 MaxShown = 8);
}
