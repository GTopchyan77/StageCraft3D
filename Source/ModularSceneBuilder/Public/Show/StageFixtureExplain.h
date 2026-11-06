// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AI/StageAILanguage.h"
#include "Show/ShowControlTypes.h"
#include "Show/StageDMXNetworkTypes.h"
#include "Show/StageShowDiagnostics.h"
#include "Show/StageShowMix.h"

/** Why a fixture gives no light, the first cause along its signal chain (Docs/ADR/0030 §5). */
enum class EStageDarkCause : uint8
{
	/** It gives light. */
	None,
	/** No fixture has that ID. */
	NotFound,
	/** The show follows DMX IN, but the input is off or failed. */
	NoInput,
	/** The show follows DMX IN, but no console sends the fixture's universe (or it has no address to be reached at). */
	NoInputSignal,
	Blackout,
	GrandMaster,
	/** Its dimmer is at zero: FStageFixtureFacts::Dimmer says which layer set it. */
	Dimmer,
	/** An effect takes the dimmer down (at this moment of its cycle, or for good). */
	Effect,
	/** The colour mix lets no light out (every emitter at 0, or full CMY). */
	Color,
	IrisClosed,
};

/** Why a real fixture on the network would not follow, independent of how it looks on screen. */
enum class EStageOutputCause : uint8
{
	/** DMX OUT sends its universe. */
	None,
	/** DMX OUT is off: real fixtures get nothing (normal for a previs-only stage). */
	OutputOff,
	OutputFailed,
	/** DMX OUT is on but paused while DMX IN follows a console. */
	OutputPaused,
	Unpatched,
	UniverseMuted,
	PatchConflict,
};

/** Everything the explanation reads about one fixture, gathered by UStageShowDiagnosticsSubsystem (or built by a test). */
struct MODULARSCENEBUILDER_API FStageFixtureFacts
{
	int32 FixtureId = 0;
	bool bExists = false;
	/** The catalog name ("Spot 575"). */
	FString Name;

	EStageControlSource ControlSource = EStageControlSource::Internal;
	EStageDMXInputState InputState = EStageDMXInputState::Off;
	/** Its universe has a live source on DMX IN. */
	bool bUniverseLive = false;

	FStageDMXPatch Patch;
	bool bPatchConflict = false;
	EStageDMXOutputState OutputState = EStageDMXOutputState::Off;
	bool bUniverseMuted = false;

	/** It has a dimmer attribute. Without one, its colour is its intensity. */
	bool bHasDimmer = true;
	/** How the mix made its dimmer (ExplainAttribute on the dimmer). */
	StageShowMix::FAttributeExplanation Dimmer;
	/** The fixture's dimmer output now, 0..1 (a fade part-way included). */
	float OutputDimmer = 0.f;
	/** For Dimmer.Layer CueList: the active cue, and the cue that set the dimmer (tracked from). */
	TOptional<float> ActiveCue;
	TOptional<float> DimmerCue;
	/** For Dimmer.Layer Playback: its name and fader level. */
	FText PlaybackName;
	float PlaybackLevel = 0.f;

	/** The share of the fixture's rated output the colour mix lets out, 0..1 (StageColorScience::FMix::LuminousFraction). */
	float ColorFraction = 1.f;
	bool bHasIris = false;
	/** 0 open .. 1 closed. */
	float Iris = 0.f;

	float GrandMaster = 1.f;
	bool bBlackout = false;

	/** Running effects on this fixture's intensity (names), and whether one of them follows the music. */
	TArray<FText> IntensityEffects;
	bool bAudioEffect = false;
	bool bAudioKill = false;
	/** The flash limit audio effects pass through (Hz). */
	float FlashLimitHz = 3.f;
};

/** The answer: the first cause on screen and on the wire, and the whole chain as text. */
struct MODULARSCENEBUILDER_API FStageFixtureVerdict
{
	EStageDarkCause DarkCause = EStageDarkCause::None;
	EStageOutputCause OutputCause = EStageOutputCause::None;
	/** Where DarkCause (else OutputCause) is fixed, for SHOW ME. */
	EStageDiagnosticPlace Place = EStageDiagnosticPlace::None;
	/** First line: the verdict. Then one line per step of the chain (control, intensity, effect, masters, colour, DMX). */
	FString Text;
};

/**
 * "Why is fixture 12 dark?" answered exactly (Docs/ADR/0030 §5): walks the signal chain of one fixture (DMX in, the layer that set
 * its dimmer, effects, the masters, colour and iris, then the patch and DMX out) and names the first thing that keeps it dark and
 * the first that keeps a real fixture from following. Pure, any thread; covered by StageCraft.Show.Explain.
 */
namespace StageFixtureExplain
{
	/** Below this (0..1) the fixture counts as dark. */
	inline constexpr float DarkLevel = 0.005f;

	MODULARSCENEBUILDER_API FStageFixtureVerdict Explain(const FStageFixtureFacts& Facts, EStageAILanguage Language);
}
