// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "StageAudioReactiveTypes.generated.h"

/**
 * Where the music that drives audio-reactive lighting comes from (Docs/ADR/0029-audio-analysis-and-audio-reactive-effects.md §3).
 * Exactly one source is live at a time.
 */
UENUM(BlueprintType)
enum class EStageAudioSource : uint8
{
	Off,
	/** A music file decoded by StageCraft (WAV, FLAC, MP3, AAC/M4A, WMA) and played through the app; the audience hears it. */
	File,
	/** A capture device (line in, audio interface, microphone): analysed only, never played back. */
	Input,
	/** What this PC plays (a browser playing YouTube, a music player): WASAPI loopback, Windows only. Analysed only. */
	SystemAudio,
	/** A deterministic kick pattern at a set BPM: tests, the benchmark and a demo without music. Analysed only, silent. */
	TestSignal,
};

UENUM(BlueprintType)
enum class EStageAudioSourceState : uint8
{
	Off,
	/** Opening: a file being opened, a device being started (on its own thread; never long). */
	Starting,
	Running,
	/** The source could not start or stopped with an error; see FStageAudioStatus::Error. */
	Failed,
};

/** The values an audio-reactive effect can read (ADR 0029 §4 Band). */
UENUM(BlueprintType)
enum class EStageAudioBand : uint8
{
	SubBass,
	Bass,
	Mid,
	High,
	/** The whole spectrum. */
	Level,
};

/** File playback commands (ADR 0029 §3 File: play, pause, seek, loop). */
UENUM(BlueprintType)
enum class EStageAudioTransportCommand : uint8
{
	Play,
	Pause,
	Stop,
	/** To FStageAudioTransportRequest::Seconds. */
	Seek,
	/** On or off from FStageAudioTransportRequest::bLoop. */
	SetLoop,
};

/** What the GameMode is asked to allow (AStageCraftGameModeBase::EvaluateAudioRequest). */
UENUM(BlueprintType)
enum class EStageAudioRequest : uint8
{
	SetSource,
	OpenFile,
	Transport,
	ApplySettings,
	SetPlayInBackground,
};

USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageAudioTransportRequest
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "StageCraft|Audio")
	EStageAudioTransportCommand Command = EStageAudioTransportCommand::Play;

	UPROPERTY(BlueprintReadWrite, Category = "StageCraft|Audio")
	double Seconds = 0.0;

	UPROPERTY(BlueprintReadWrite, Category = "StageCraft|Audio")
	bool bLoop = true;
};

/**
 * One analysis frame of the live source, as the game thread sees it (ADR 0029 §3). Produced about 94 times a second by the
 * analyser thread and released when the audience hears the audio it describes, so a reader gets the music of "now".
 *
 * Counters (OnsetCount, BeatCount) only grow while a source runs: a reader that compares them with the last values it saw
 * cannot miss an onset or a beat between two reads, which a flag would allow.
 */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageAudioFeatures
{
	GENERATED_BODY()

	/** 0..1 each: energy in the band, normalised to the music's own recent loudness (a quiet and a loud song both use the range). */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Audio")
	float SubBass = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Audio")
	float Bass = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Audio")
	float Mid = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Audio")
	float High = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Audio")
	float Level = 0.f;

	/** Detected onsets (drum hits, note attacks) since the source started. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Audio")
	int64 OnsetCount = 0;

	/** Tempo; 0 until one has been found. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Audio")
	float BPM = 0.f;

	/** 0..1 through the current beat, 0 on the beat, at Timestamp. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Audio")
	float BeatPhase = 0.f;

	/** Beats since the tempo was first found. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Audio")
	int64 BeatCount = 0;

	/** 0..1: how sure the tempo and beat are. Falls on music without a steady pulse and while the tempo wanders. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Audio")
	float Confidence = 0.f;

	/** False after 2 s below the noise floor (the music stopped): audio effects then fall back to their audio-free selves. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Audio")
	bool bSignal = false;

	/** When the audience hears the audio this frame describes, on the FPlatformTime::Seconds clock. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Audio")
	double Timestamp = 0.0;

	float GetBand(EStageAudioBand Band) const;

	/** BeatPhase carried forward to NowSeconds at the current tempo (the reader's frame time is later than Timestamp). */
	float PhaseAt(double NowSeconds) const;
};

/**
 * Audio-reactive settings of this PC (ADR 0029 §5), saved in GameUserSettings.ini (UStageCraftUserSettings);
 * UStageAudioReactiveSubsystem is the only writer. Always read through StageAudioReactive::Sanitize: the file can be edited by hand.
 */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageAudioReactiveSettings
{
	GENERATED_BODY()

	/** Opened again with the next stage. Never File: a song is chosen per session (ADR 0029 §5). */
	UPROPERTY(Config, BlueprintReadWrite, Category = "StageCraft|Audio")
	EStageAudioSource LastSource = EStageAudioSource::Off;

	/** The capture device for Input (AudioCaptureCore's device id); empty = the Windows default recording device. */
	UPROPERTY(Config, BlueprintReadWrite, Category = "StageCraft|Audio")
	FString InputDeviceId;

	/** Gain on the Input before analysis, -24..+24 dB. */
	UPROPERTY(Config, BlueprintReadWrite, Category = "StageCraft|Audio")
	float InputGainDb = 0.f;

	/**
	 * How long after the mixer renders a file's samples the audience hears them (the engine's output buffering, measured at
	 * 78 ms in Phase G0, ADR 0029 §2.1). File analysis is held back by this much so the lights land with the sound. 0..500 ms.
	 */
	UPROPERTY(Config, BlueprintReadWrite, Category = "StageCraft|Audio")
	float FileOutputDelayMs = 78.f;

	/** Per source, added to the timing: positive = the lights later, negative = earlier. -500..+500 ms. */
	UPROPERTY(Config, BlueprintReadWrite, Category = "StageCraft|Audio")
	float FileLatencyOffsetMs = 0.f;

	UPROPERTY(Config, BlueprintReadWrite, Category = "StageCraft|Audio")
	float InputLatencyOffsetMs = 0.f;

	UPROPERTY(Config, BlueprintReadWrite, Category = "StageCraft|Audio")
	float SystemAudioLatencyOffsetMs = 0.f;

	/** Band edges in Hz: sub-bass below SubBassTopHz, bass to BassTopHz, mid to MidTopHz, high to HighTopHz (ADR 0029 §3). */
	UPROPERTY(Config, BlueprintReadWrite, Category = "StageCraft|Audio")
	float SubBassTopHz = 60.f;

	UPROPERTY(Config, BlueprintReadWrite, Category = "StageCraft|Audio")
	float BassTopHz = 250.f;

	UPROPERTY(Config, BlueprintReadWrite, Category = "StageCraft|Audio")
	float MidTopHz = 4000.f;

	UPROPERTY(Config, BlueprintReadWrite, Category = "StageCraft|Audio")
	float HighTopHz = 16000.f;

	/** A file starts again at its end. */
	UPROPERTY(Config, BlueprintReadWrite, Category = "StageCraft|Audio")
	bool bLoopFile = true;

	/** TestSignal tempo, 60..200 BPM. Not remembered across sessions as a choice of music, only as the last value used. */
	UPROPERTY(Config, BlueprintReadWrite, Category = "StageCraft|Audio")
	float TestSignalBPM = 128.f;

	bool operator==(const FStageAudioReactiveSettings& Other) const = default;
};

/** What the dev console (and the G2 Audio panel) shows. A snapshot. */
struct FStageAudioStatus
{
	EStageAudioSource Source = EStageAudioSource::Off;
	EStageAudioSourceState State = EStageAudioSourceState::Off;
	FText Error;
	/** "Speakers (Realtek), 48000 Hz, 2 ch" / the file name. */
	FString Description;
	int32 SampleRate = 0;
	int32 Channels = 0;

	/** File only: position and length in seconds, whether it plays, pauses or loops. */
	double PositionSeconds = 0.0;
	double DurationSeconds = 0.0;
	bool bPlaying = false;
	bool bPaused = false;
	bool bLoop = false;
	/** Render callbacks that found the decoder behind (silence was played). */
	int64 Underruns = 0;

	/** Samples the analyser lost because it fell behind its source (the ring buffer was overrun). */
	int64 DroppedSamples = 0;
	/** Analyser thread time per second of audio analysed, in % of one core (ADR 0029 §8: under 2). */
	float AnalyzerCpuPercent = 0.f;
	double AnalysedSeconds = 0.0;

	FStageAudioFeatures Features;
};

namespace StageAudioReactive
{
	constexpr float MinLatencyOffsetMs = -500.f;
	constexpr float MaxLatencyOffsetMs = 500.f;
	constexpr float MaxFileOutputDelayMs = 500.f;
	constexpr float MinInputGainDb = -24.f;
	constexpr float MaxInputGainDb = 24.f;
	constexpr float MinTestSignalBPM = 60.f;
	constexpr float MaxTestSignalBPM = 200.f;
	/** The analysis runs at 48 kHz; band edges above Nyquist margin are clamped here. */
	constexpr float MinBandHz = 20.f;
	constexpr float MaxBandHz = 20000.f;

	/** Clamps every value; non-finite numbers fall back to the defaults; band edges are forced ascending; File is never a LastSource. */
	MODULARSCENEBUILDER_API FStageAudioReactiveSettings Sanitize(const FStageAudioReactiveSettings& Settings);

	/** Seconds from the moment a source's samples reach the analysis tap to the moment the audience hears them, with the user's offset. */
	MODULARSCENEBUILDER_API double GetHeardDelaySeconds(const FStageAudioReactiveSettings& Settings, EStageAudioSource Source);

	/** A capture source the status bar must show as "Listening" (ADR 0029 §6). */
	inline bool IsCaptureSource(EStageAudioSource Source) { return Source == EStageAudioSource::Input || Source == EStageAudioSource::SystemAudio; }
}
