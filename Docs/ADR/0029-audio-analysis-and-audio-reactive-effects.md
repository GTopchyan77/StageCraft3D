# ADR 0029: Audio analysis and audio-reactive effects

- **Status:** approved by Gevor 2026-11-05. Drafted in STATE.md #67.
  - Spike G0 done (STATE.md #68, §2.1): it changed the file source (§3) and the module dependencies (§11).
  - G1 implemented (STATE.md #69, §13): sources, analyser, subsystem, settings, requests.
  - G2 implemented (STATE.md #70, §14): audio in effects, the Audio panel, scene v11, `Bench.Fixtures Audio=1`.
  - G3 implemented (STATE.md #71, §15): the flash limiter, the AUDIO master and KILL, LISTENING, latency offsets in the Audio panel.
- **Related:**
  - ADR 0027 (effects engine; this ADR amends §2 and §4 with an audio setting on `FStageEffect`).
  - ADR 0025 / 0026 (the output mix and its masters; the show control stays the only writer).
  - ADR 0019 §9 (DMX output: audio-driven looks reach real fixtures, hence §7 below).
  - ADR 0017 (voice prompts already use `AudioCaptureCore` through `StageVoiceRecorder`).
  - Rules.md: Architecture (state by lifetime, no idle tick), §21 (no polling), §29 (threads), §54 (determinism), §55 (external data), §65 (Shipping).
- **Roadmap:** `Docs/Roadmap/2026-11-05-realism-audio-assistant.md`, Phase G.

## 1. Problem

Gevor's brief (2026-11-05) asks for:
- music from a file (WAV, MP3), from line or media input, and from streamed audio such as YouTube;
- a real-time analyser with four frequency bands (sub-bass, bass, mid, high) and beat and BPM detection;
- these values driving the F4 effects engine: intensity pulses, strobe chases, position swings and colour shifts that follow the music.

What exists:
- **Effects** have their own clock counted in cycles, advanced by `RateHz` (ADR 0027 §2). TAP sets a rate by hand. There is no outside clock and no modulation input.
- **Audio input:** `StageVoiceRecorder` records the microphone through `AudioCaptureCore` (the AudioCapture plugin is enabled).
- **Nothing analyses audio.** Nothing plays music files: `UStageAudioSubsystem` only plays UI feedback cues.

## 2. Verified engine facts (UE 5.8, checked 2026-11-05)

| # | Fact | Where | Consequence |
|---|---|---|---|
| A1 | `ISubmixBufferListener::OnNewSubmixBuffer(Submix, float* AudioData, NumSamples, NumChannels, SampleRate, AudioClock)` gives interleaved float PCM on the audio render thread. | `Engine/Public/ISubmixBufferListener.h:26` | Anything the engine plays through a submix can be tapped without a second decoder. |
| A2 | `Audio::FSpectrumAnalyzer` / `ISpectrumBandExtractor` and `Audio::IFFTAlgorithm` (`FFTAlgorithm.h`, `AudioFFT.h`) are in the `SignalProcessing` runtime module. | `Runtime/SignalProcessing/Public/DSP/` | An FFT of our own costs no plugin. |
| A3 | `UMediaSoundComponent` offers `SetEnableEnvelopeFollowing`, `GetSpectralData` and `GetNormalizedSpectralData`. | `Runtime/MediaAssets/Public/MediaSoundComponent.h:109-117` | Media playback has analysis of its own. It covers only media, so it is not the shared path (§3). |
| A4 | The AudioCapture plugin and `AudioCaptureCore` have **no loopback** (system output) capture. The source tree has no match for "loopback". | `Plugins/Runtime/AudioCapture`, `Runtime/AudioCaptureImplementations` | Capturing what the PC plays (a browser, Spotify) needs WASAPI loopback code of our own, Windows only. |
| A5 | WmfMedia, ElectraPlayer, AudioSynesthesia, MassCrowd, AnimToTexture and MetaHuman plugins ship with 5.8. | `Engine/Plugins/...` | Recorded for later phases; none is a dependency yet. |
| A6 | **Measured in G0, partly false.** WmfMedia refuses `.mp3` and `.flac` by extension: its factory's list has wav, aac, m4a, wma, mp4 and video containers ("none of the enabled media player plug-ins support it"). ElectraPlayer (off by default) opens `.mp3`, but not bare `.flac`. | `WmfMediaFactoryModule.cpp:153-170`, `ElectraPlayerFactoryModule.cpp:185`; G0 runs | Files are decoded by Media Foundation directly (§2.1, §3); the media framework is not used. |
| A7 | **Measured in G0.** The engine mutes an unfocused app: `BaseEngine.ini [Audio] UnfocusedVolumeMultiplier=0.0`. This also silences every submix tap: the analysis goes dead when the operator clicks into a browser. | `BaseEngine.ini:1771`, `FApp::GetUnfocusedVolumeMultiplier`; G0 runs | StageCraft sets `FApp::SetUnfocusedVolumeMultiplier` from a user setting (default 1: keep playing). |

### 2.1 G0 measurements (STATE.md #68)

**Setup:**
- **Signal:** a 20 s synthetic 120 BPM track: a kick every 0.5 s from exactly 1.000 s, an off-beat hi-hat, a chord pad and a noise floor. It was saved as WAV, FLAC, MP3 (320k, 44.1 kHz) and M4A (AAC 256k).
- **Device:** 48 kHz stereo float.
- **Builds:** each run used one process of the spike worktree, `spike/g0-audio`. Every number below was taken in Development `-game`, and the format, tap, loopback and detector numbers were **repeated in a packaged Shipping build** with the same results.
- **Accuracy:** truth is sample-accurate to ±1 ms (the threshold-crossing bias was measured on the files with ffmpeg and subtracted).

| Question | Result |
|---|---|
| **Formats: our own Media Foundation decoder** (`IMFSourceReader` → float; `mfplat`, `mfreadwrite`, `mfuuid`) | WAV, FLAC, MP3 and M4A all decode, in Development and Shipping. A 20 s file takes 54–133 ms (150–370× real time). Encoder priming is **not** trimmed: MP3 +39 ms, AAC +21 ms against the file's timeline. |
| **Formats: media framework** | WMF plays WAV only (of the four). Electra plays MP3. Both work in Shipping. |
| **Start latency** (play → first sample of the kick rendered by the mixer) | Procedural wave fed with the decoded PCM: 3 ms (Development), 16 ms (Shipping) for WAV. The MP3 adds its 39 ms priming. WMF: 20–36 ms after `IsPlaying`. Electra: 71–86 ms. |
| **The player's clock** (`UMediaPlayer::GetTime`) | WMF steps in 100 ms increments. Electra runs about 250 ms ahead of what the mixer renders. **Neither can place a beat**: song position must come from samples we count. |
| **Submix tap** (`ISubmixBufferListener` on the main submix) | 1024 frames per callback, every 21.3 ms on average (19–31 ms). |
| **Engine output delay** | The kick reaches Windows' loopback stream 78 ms (73–83, sd 3) after the mixer rendered it. So the engine's own output buffering is about 78 ms. |
| **WASAPI loopback** (shared mode, default render device) | Works in Development and Shipping: 48 kHz stereo float, 480-frame packets every 10 ms, QPC stamps steady, one discontinuity flag at start, silent packets flagged. The detector finds 37–38 of 38 kicks on it. |
| **Microphone** (`AudioCaptureCore`, USB device) | 480-frame callbacks every 10 ms at 48 kHz. The acoustic path (speaker to mic) could not be measured: the mic heard almost nothing (peak 0.0007). Left for a manual check. |
| **Onset detectors** on the tapped stream: 38 kicks + 38 hats, decision latency = the last sample a detector needed minus the true onset | **Ours, flux 2048/512 Hann, log magnitude:** 38/38 kicks, 36–38 hats, 0–2 false positives, **27 ms** mean (20–41), 1.2 ms CPU per second of audio. Ours 1024/256: 13–16 ms but misses kicks and 4–6 false positives. AudioSynesthesia `FOnsetStrengthAnalyzer` defaults (4096/1024, lag 4): 38/38, 1–4 false positives, **64 ms** (52–80), 0.4 ms CPU per second. Synesthesia at 2048/512, lag 1: 25 ms with 5–6 false positives. |

**What it decides:**
- **File source.** Decode with Media Foundation on a worker, play the PCM through our own procedural sound, and analyse that same PCM **before** it is played. The analysis can then be lined up with what the audience hears: features are held back by the measured output delay, about 78 ms minus the detection latency, rather than arriving late.
  - Encoder priming then costs nothing: the lights follow the samples we actually play.
  - The song position is exact.
  - No media plugin and no extension list are needed.
- **Detector.** Our flux at 2048/512 is the onset stage. AudioSynesthesia adds a plugin for no gain in accuracy, and its default settings are more than twice as slow.
- **Unfocused mute** (A7) becomes a setting.

## 3. Decision: sources, and one analysis path

**`EStageAudioSource { Off, File, Input, SystemAudio, TestSignal }`** is one explicit state. Exactly one source is live at a time.

| Source | How | Notes |
|---|---|---|
| File | `FStageAudioFileDecoder` (Windows Media Foundation source reader: WAV, FLAC, MP3, AAC/M4A, WMA) on a worker, streaming PCM in chunks to our procedural sound and, ahead of the output, to the tap (§2.1) | Transport: play, pause, seek, loop, with an exact sample position. The listener hears it. Changed by G0 from `UMediaPlayer`. |
| Input | `AudioCaptureCore` device (line in, interface, mic) | Analysed only. Not played back, so no feedback loop. |
| SystemAudio | `FStageLoopbackCapture` (Windows WASAPI loopback, `#if PLATFORM_WINDOWS`) | **This is how YouTube is used:** the user plays it in a browser and StageCraft listens to the PC's output. |
| TestSignal | A deterministic generator: a kick at a set BPM plus band tones | Tests, the benchmark and a demo without music. |

**No YouTube download, URL ingestion or stream extraction.** YouTube's terms forbid it, it breaks whenever the site changes, and the music is the user's to play anyway. System audio covers YouTube, Spotify and any player without touching them.

**Every source feeds the same `FStageAudioTap`:**
- It is called on the audio or capture thread.
- It down-mixes to mono and writes into a fixed single-producer, single-consumer ring buffer.
- It takes no lock, makes no allocation and does no UObject access.

**`FStageAudioAnalyzer` runs on its own `FRunnable` thread**, the pattern of `FStageDMXNetworkSender`.
- It resamples to 48 kHz if needed, then frames at FFT 2048 with hop 512 (about 94 frames a second) under a Hann window, through `Audio::IFFTAlgorithm`.
- It publishes **`FStageAudioFeatures`** through a lock-free triple buffer: `Bands[4]`, `Level`, `bOnset`, `BPM`, `BeatPhase` (0..1), `BeatCount`, `Confidence`, `bSignal` and the timestamp.

The maths is pure and has no engine state (`StageAudioAnalysis::`):
- **Bands:** sub-bass 20–60 Hz, bass 60–250, mid 250–4 k, high 4 k–16 k (edges in settings). Each band is energy in dB, normalised by an adaptive ceiling (fast attack, slow 8 s decay) and a noise floor, with attack and release smoothing. One quiet song and one loud song both use the full 0..1 range.
- **Onsets:** half-wave rectified spectral flux against a moving-median threshold, with a 100 ms refractory gap.
- **Tempo:** autocorrelation of the onset envelope over 6 s, 60–200 BPM, with an octave prior near 120 BPM so 64 is not reported for 128. Beat phase comes from a phase-locked loop on the onsets. `Confidence` falls while the tempo wanders.
- **No signal** (level under the floor for 2 s) clears `bSignal`. Effects then behave as their audio-free selves (§4).

**Rejected for the shared path:**
- **`UMediaSoundComponent` spectral data (A3):** media only, and no tempo.
- **AudioSynesthesia:** UObject analysers made for Blueprint, and a plugin dependency. G0 ran its streaming `FOnsetStrengthAnalyzer` against ours (§2.1). It was no more accurate, was 64 ms late at its defaults, and was noisier when made fast. Tempo and beat phase are ours either way.
- **`UMediaPlayer` for files (WmfMedia, Electra):** each plays only part of the formats (A6), and neither clock can place a beat (§2.1).

## 4. Decision: audio in the effects engine (amends ADR 0027 §2 and §4)

`FStageEffect` gains **`FStageEffectAudio Audio`**:

| Setting | Values | Effect |
|---|---|---|
| `Mode` | Off, Sync, Modulate, Trigger | Off is today's effect, byte for byte. |
| `Band` | SubBass, Bass, Mid, High, Level | What Modulate reads. |
| `Amount` | 0..1 | Depth of the modulation. |
| `Modulates` | Level, Size, Rate | Intensity/beam level range, position size, or speed. |
| `BeatsPerCycle` | 1/4, 1/2, 1, 2, 4, 8 | Sync and Trigger: one wave cycle per this many beats. |
| `Response` | Fast, Medium, Slow | Extra smoothing on top of the analyser's (Fast ≈ 30 ms release, Slow ≈ 400 ms). |

**How each mode works:**
- **Sync:** the effect's clock (still integrated, ADR 0027 §2) advances at `BPM / 60 / BeatsPerCycle`. It is pulled towards `BeatPhase` by at most 5 % of a cycle per frame, so a tempo change never makes the pattern jump. With low confidence or no signal it keeps the last good tempo, or the effect's own `RateHz` if there never was one.
- **Modulate:**
  - Level: the top of the wave moves between `Low` and `High` by `Amount × band`.
  - Size: `Size × (1 − Amount + Amount × band)`.
  - Rate: `RateHz × (1 + Amount × band)`.

  With no signal the band reads 1, so a show built on audio is fully lit, not dark, when the music stops.
- **Trigger:** each beat (or each onset with `Band = Level`) advances the clock by one pattern step (`1 / steps` of a cycle). A chase steps on the beat, and Random draws a new level per beat.

**Where it sits.** The runner reads one `FStageAudioFeatures` snapshot per `Advance` (one atomic read), and only while an effect with `Mode != Off` is running. `UShowControlSubsystem` stays the only writer. `IsTickable` is unchanged: the show ticks while effects are live and not at rest.

**Deterministic where it can be (§54).** Given a sequence of feature frames, every value is reproducible. Tests feed recorded frame sequences and the TestSignal source, never live audio.

**Unchanged rules:**
- Look stores and palettes record the static look, never an audio-moved value (`GetRecordableValues`).
- Under External DMX, effects hold.
- `StageEffectRules` clamp the new fields.

**Scene format v11:**
- Each effect gains `"audio": {mode, band, amount, modulates, beatsPerCycle, response}`, with enums written by name.
- v1–v10 files load with `Mode = Off`.
- v11 files are refused by builds before G2, as with every bump.

## 5. Decision: ownership, settings, requests

**`UStageAudioReactiveSubsystem`** (`UWorldSubsystem`, Game and PIE, like `UStageDMXNetworkSubsystem`):
- **Owns:** the source state, the tap, the analyser thread and the file player.
- **Exposes:** `GetFeatures()` (a snapshot copy), `GetStatus()` and `OnAudioStatusChanged`.
- **Status timer:** at most 2 Hz, only while a source is live, for the panel. No tick.
- **Teardown:** `Deinitialize` stops the source, then joins the thread, then frees the buffers. Stop is idempotent.

**Settings and controls:**
- **Per PC** (`UStageCraftUserSettings::AudioReactive`, sanitised on read): device id, input gain, latency offset in ms, band edges, and the last source except File.
  - The **latency offset** shifts beat phase so the lights land with what the audience hears. Loopback and interfaces add 20–80 ms.
- **The file and transport** live in the stage session. They are not saved in scenes in G (a follow-up could store a song path with a cue list).
- **Requests on `AModularPlayerController`:** `RequestSetAudioSource`, `RequestAudioTransport`, `RequestSetAudioSettings`, judged by the GameMode. These are live device actions and not undoable, like DMX IN and OUT. Effect audio settings go through the existing effect requests and are undoable.
- **UI:**
  - an **Audio** panel: source picker, device, meters for the four bands and the level, BPM with confidence and a beat lamp, latency, a file transport bar;
  - an **AUDIO** row in the Effects panel's editor;
  - six audio quick effects: bass pulse, beat chase, high strobe sparkle, hue per bar, swing on the beat, mid-driven zoom.
- **Dev console** (`#if !UE_BUILD_SHIPPING`), built in G1 as `StageCraft.Music.*` because `StageCraft.Audio.*` already holds the UI-sound commands, including its own `Status`:
  - `StageCraft.Music.Source Off|Input|SystemAudio|TestSignal [BPM=n]|File <path>`;
  - `StageCraft.Music.Status`;
  - `StageCraft.Music.Transport`;
  - `StageCraft.Music.Settings`;
  - `StageCraft.Music.Devices`.

## 6. Decision: privacy

- **Memory only.** Input and system audio are analysed in memory and never recorded, saved, logged or sent anywhere.
- **Visible state.** The status bar shows a "Listening" indicator whenever a capture source is live.
- **Clean stop.** Changing source or closing the stage stops capture.

## 7. Decision: flash safety on real lights

Audio-triggered intensity and strobe can flash very fast, and with DMX OUT on they reach real fixtures and a real audience.

**Flash limiter.** A flash limiter in the effect stage of the mix caps how often an **audio-driven** intensity can rise from below 20 % to above 80 %:
- **Default:** 3 per second. 3 Hz is the general flash threshold in photosensitive seizure guidance (e.g. WCAG 2.3.1).
- **Setting:** per PC, 1–10. Above 3 it shows a warning.
- **Strobe:** audio-driven strobe attributes are capped to the same rate.
- **Scope:** the cap applies only to audio modes. A deliberately programmed effect is the operator's responsibility, as on any console.

**Audio master.** An **AUDIO** master (0..1, plus a KILL button) in the Effects and Playback panels scales every audio modulation towards its no-signal value. It is not saved; it resets to 1 per session.

## 8. Performance

| Where | Expected | Gate |
|---|---|---|
| Audio thread (tap) | a copy and a down-mix per buffer | no allocation; checked by test with an allocation counter in Development |
| Analyser thread | about 94 FFTs of 2048 a second | under 2 % of one core, measured with Insights in G1 |
| Game thread | one snapshot read per show tick and a few multiplies per driven attribute | `Bench.Fixtures Effects=1 Audio=1` (TestSignal source) within 5 % of `Effects=1` at 200 and 500 fixtures, A/B against the commit before G1 |
| Idle | nothing ticks; the status timer runs only while a source is live | `Bench.Fixtures` idle phase unchanged |

## 9. Tests (planned)

| Test | Kind | Proves |
|---|---|---|
| `StageCraft.Audio.Bands` | pure | Tones at 40 Hz / 120 Hz / 1 kHz / 8 kHz land in the right band; normalisation spans 0..1 for a quiet and a loud signal; release smoothing; silence clears `bSignal` |
| `StageCraft.Audio.Tempo` | pure | Kick patterns at 90, 120, 128, 140 and 174 BPM are detected within ±1 BPM in 6 s, with no octave error; phase locks within 4 beats; a tempo change re-locks; noise gives low confidence |
| `StageCraft.Audio.RingBuffer` | pure | SPSC wrap, overrun drops the oldest, no allocation after Init |
| `StageCraft.Audio.Analyzer` | thread | TestSignal → features on the game thread; stop joins; double stop is safe |
| `StageCraft.Show.AudioEffects` | world | Sync, Modulate and Trigger from a recorded frame sequence give exact values; no-signal fallbacks; recording ignores audio; External holds; the flash limiter caps at 3 Hz; AUDIO master and KILL |
| `StageCraft.Scene.EffectAudioData` | file | v11 round trip; v10 loads with audio off; hostile values clamped |

## 10. Alternatives considered

- **YouTube URL playback inside StageCraft.** Rejected (§3): terms of service, fragility and DRM. System audio covers it.
- **A third-party runtime audio importer plugin** for MP3. Not taken unless G0 shows WmfMedia cannot play MP3 in Shipping. It would add a dependency and a licence.
- **Analysing on the game thread.** Rejected: it ties analysis to the frame rate (an 8 ms frame and a 43 ms window do not mix), and it costs the game thread for nothing.
- **A separate "audio effects" layer in the mix.** Rejected: audio is a clock and a modulation input to the existing effects, so cues, tracking, fades and undo work unchanged. A second writer would break Rules (one writer).
- **Driving cue GOs from beats** (a beat-triggered cue list). It is a follow-up: it needs a rule for timing that a beat interrupts.

## 11. Consequences

- New Private `Audio/` files and one engine module dependency (`SignalProcessing`). On Windows: `ole32`, `mfplat`, `mfreadwrite` and `mfuuid` system libraries. No media plugin and no AudioSynesthesia (G0, §2.1).
- Scene format v11.
- **Follow-ups:**
  - songs saved with a show, and beat-triggered cues;
  - section detection (verse and chorus) for an "auto show";
  - an audio-energy input to the crowd (Phase I);
  - MIDI clock and Ableton Link as tempo sources.

## 12. Rollback

Set `Mode = Off` on every effect (v11 files keep loading), then remove `Audio/` and the panel. The effects engine needs no change to fall back: Off is today's behaviour.

## 13. G1 implementation notes (STATE.md #69)

How G1 realised §3 and §5, and where it differs in detail.

### Files
All under `Source/ModularSceneBuilder`:

| File | Role |
|---|---|
| `Public/Audio/StageAudioReactiveTypes.h` | Enums, `FStageAudioFeatures`, settings, status |
| `Public/Audio/StageAudioAnalysis.h` | The pure maths |
| `Private/Audio/StageAudioTap.h` | The SPSC ring and the triple buffer |
| `Private/Audio/StageAudioAnalyzer.h` | The thread |
| `Private/Audio/StageAudioFileDecoder.h` | Media Foundation |
| `Private/Audio/StageAudioFileSound.h` | `USoundWaveProcedural` |
| `Private/Audio/StageAudioCaptureSources.h` | WASAPI loopback and AudioCaptureCore input |
| `Public/Subsystems/StageAudioReactiveSubsystem.h` | The world subsystem |
| `Private/Audio/StageAudioReactiveConsoleCommands.cpp` | Dev commands |

**Module dependencies:** `SignalProcessing` and `AudioExtensions`. `AudioExtensions` is needed by any `USoundWaveProcedural` subclass.

### Timing: the heard delay
Every frame carries `Timestamp` = the moment the audience hears the audio it describes. The analyser holds each frame until then, so a reader always gets the music of "now".

The delay from tap to audience (`StageAudioReactive::GetHeardDelaySeconds`):
- **File:** `FileOutputDelayMs` (default 78 ms, the G0 mixer-to-loopback measurement) plus the user's offset. The file is tapped in the procedural sound's render callback, before the engine's output buffering.
- **Input and SystemAudio:** the user's offset only (default 0). That audio has already been heard, so its features are late by the detection time; `FStageAudioFeatures::PhaseAt(now)` carries the beat phase forward at the tempo.

### Analysis
- **Tempo envelope** = low-band (below 250 Hz) flux in plain magnitude + 0.35 × the full log flux.
  - The log compression G0 used for onsets made an off-beat hat's faint low end rise as much as a kick, so 90 BPM with hats read as 180. With plain magnitude in the low band it reads 90.
- **Absolute onset floor** (flux 0.1): a steady tone's rounding noise was producing onsets.
- **Beat phase:**
  - a comb fit places the beat;
  - a least-squares fit of the onsets on that grid refines the period and the beat time;
  - the phase is pulled 20 % of its error per frame;
  - the beat counter never runs backwards.
- **Measured on generated kicks** (tests):
  - 90–174 BPM within 0.07 BPM, with or without off-beat hats;
  - beats 5–8 ms from the kicks;
  - onset decision latency 26 ms;
  - analyser 0.3 % of one core;
  - noise confidence 0.14.

### Sources and state
- **State:** `EStageAudioSourceState` gained `Starting`, for a file or system audio opening on its own thread. Results reach the game thread through the task graph and are matched to the current source by a generation number, so a late result for a replaced source is ignored.
- **Each source start gets a fresh tap.** A render or capture callback still running for the old source writes into a tap nobody reads.
- **Seek** restarts the decoder at the position, and with it the analysis. The tempo window refills, about 3 s before a BPM is reported.
- **Background audio** is `UStageCraftUserSettings::bPlayAudioInBackground`, on by default. `UStageAudioSubsystem` applies it through `FApp::SetUnfocusedVolumeMultiplier` and restores the previous value on `Deinitialize`, because PIE shares it with the editor.
- **GameMode gate:** `AStageCraftGameModeBase::EvaluateAudioRequest(Player, EStageAudioRequest)`, a BlueprintNativeEvent. By default it allows any player and refuses no player.


## 14. G2 implementation notes (STATE.md #70)

How G2 realised §4, §5 and §8, and what it settled that §4 left open.

### Effects
- **`FStageEffectAudio`** on `FStageEffect` (`Public/Show/StageEffectTypes.h`): `Mode`, `Band` (G1's `EStageAudioBand`), `Amount`, `Modulates` (`EStageEffectAudioTarget`), `BeatsPerCycle` (`EStageEffectBeatDivision`), `Response`. `Equals`, `SanitizeEffect`, undo and every effect request carry it unchanged.
- **The pure maths** is in `StageEffects` (`Public/Show/StageEffects.h`): `FAudioInput` (one analyser snapshot, the beat carried forward to the frame time), `ApplyModulation`, `GetModulatedRate`, `GetSyncTarget`, `GetTriggerEvents`, `GetTriggerStep`, `SmoothBand`, `GetCycleError`.
- **The runner** (`FStageEffectRunner::Advance(Delta, Released, Audio)`) keeps per effect a smoothed band, the last good tempo and the trigger count. `NeedsAudio()` is rebuilt only when the running set changes.
- **The show** reads the music once per tick, and only while an audio effect runs: one lock-free copy, no allocation. `IsTickable` is unchanged.
- **Off** takes the same code path as before G2: the same clock, value for value (test `Show.AudioEffects`).

### Clarifications of §4
- **Sync** follows the tempo only at confidence ≥ 0.5 with a signal. Below that it keeps the last tempo it followed, and the pull to the beat stops. The pull is at most 0.05 cycle a frame, along the shorter way round the cycle.
- **Trigger** steps once per event:
  - Events are `floor(beats / BeatsPerCycle)`: ¼ gives four steps a beat, 8 one step every two bars.
  - With `Band = Level` they are onsets instead, and need a signal but no tempo.
  - A step is one pattern step: `1 / steps` of a cycle, where steps come after wings and blocks. For Random it is a whole cycle, so each step draws a new level.
  - At most 4 steps a frame.
  - Without a usable signal the pattern holds. When the signal returns, the count primes again without catching up.
- **Modulate:**
  - Level moves `High` towards `Low`, so it applies to Intensity and Beam. Size applies to Position. Rate scales the clock.
  - The band rises at once and falls with the response's time constant: Fast 30 ms, Medium 120 ms, Slow 400 ms.
- **Scene v11:**
  - `"audio"` is written only when the mode is not Off, so a show without audio effects writes what v10 did, apart from the version number.
  - Enums are written by name, with `BeatsPerCycle` as `"1/4"`, `"1/2"`, `"1"`, `"2"`, `"4"` or `"8"`.

### UI
- **Effects panel:**
  - AUDIO rows: MODE, then BAND, MOD, BEATS, RESP and AMOUNT, shown by mode. RATE is greyed while Sync or Trigger drives the clock.
  - An AUDIO group of six quick effects: Bass Pulse, Beat Chase, High Sparkle, Hue per Bar, Beat Swing and Mid Zoom.
  - Running effects that follow the music show ♪ with their mode.
- **Cues and Playback panels:** ♪ after the FX count when one of a cue's effects follows the music.
- **Audio panel** (`StageCraft.Panel.Audio`, `UStageAudioPanel`, `DA_Panel_Audio`; Window > Audio..., a tab of the Lighting Desk):
  - source buttons; a FILE... dialog;
  - file transport and a seek bar;
  - the input device and its gain;
  - SUB, BASS, MID, HIGH and LEVEL meters, BPM with confidence and a beat lamp;
  - "keep playing in the background";
  - the analyser's cost.
  - Every write goes through the controller's audio requests.
  - The meters run from a 30 Hz timer that exists only while a source runs.
- **Latency offsets, the AUDIO master, KILL, the flash limiter and the Listening indicator stay in G3.**

### Measurements
- **The file's heard delay:**
  - `StageCraft.Music.MeasureDelay` (dev) compares the playing file's beat phase with a second analyser on the WASAPI loopback.
  - On this PC the file's beat trailed Windows' output by 7.4 ms (median of 1,596 frames, 10–90 % from −10.4 to −5.8 ms).
  - That is inside the 10 ms tolerance, so the default stays at 78 ms.
- **Cost:** `Bench.Fixtures Effects=1 Audio=1` against `Effects=1`; see STATE.md #70.


## 15. G3 implementation notes (STATE.md #71)

How G3 realised §6 and §7, and what it settled that they left open.

### Flash limiter
- **Where:** `FStageEffectRunner::Evaluate`, after an attribute's effect values are composed. It applies to every intensity attribute (palette kind Intensity) and the Shutter attribute that an effect following the music drives. If a programmed effect drives the same attribute, the composed value is limited too.
- **The gate** (`StageEffects::FFlashGate`, one per fixture and attribute):
  - A flash is a rise from below 20 % to above 80 % of the attribute's range, at most `FlashLimitHz` a second.
  - **A refused rise holds the last value shown**, at most 80 %. A clamp at 80 % would have let a 10 Hz square flicker between 0 and 80 % at full rate. Holding shows the pattern's flashes at the limit instead, so a too-fast pattern reads as a slower one.
  - Values between the thresholds and a light that stays up pass unchanged.
  - The gate runs on the runner's own clock (seconds advanced), so it is deterministic.
  - It is idempotent within a frame: a second call with the same time answers from the state the frame started with. The show mixes a fixture more than once in some frames.
  - Gates are kept while a fixture stays driven, so a cue change cannot let a flash through early. They are dropped when no effect drives the fixture.
- **Strobe:** a music-driven Shutter is capped at the value whose strobe rate (`StrobeRateHz` across 0..1) equals the limit. A fixture whose slowest strobe is already faster than the limit stays open (0).
- **Setting:** `FStageAudioReactiveSettings::FlashLimitHz`, default 3, sanitised to 1..10.
  - It is set by the Audio panel's FLASH slider, which warns above 3, or by `StageCraft.Music.Settings FlashLimit=<Hz>`.
  - The show takes it from `OnAudioStatusChanged`, so it is pushed, never polled.
- **Not limited:**
  - effects that do not follow the music (§7: the operator's responsibility);
  - colour effects. An RGB fixture can still flash through a music-driven colour effect, which is an open follow-up.
- **Cost:** a map lookup and a few compares per limited attribute and fixture per frame. Measured in STATE.md #71.

### AUDIO master and KILL
- **Owner:** `UShowControlSubsystem` (`SetAudioMaster`, `SetAudioKill`). Session only: a stage always opens at full with KILL off.
- **Requests:** `AModularPlayerController::RequestSetAudioMaster` and `RequestSetAudioKill`, judged as `EStageCueRequest::Masters` like the Grand Master and BLACKOUT.
- **Effect on the runner** (`FStageEffectRunner::SetAudioMaster`, with KILL meaning 0):
  - Modulate reads the band as `1 + (band − 1) × master`: fully open at 0, as without music.
  - At 0 every audio effect gets the silent input: Sync keeps its last tempo without the pull, and Trigger holds, then primes again without catching up when the master returns.
  - Between 0 and 1, Sync and Trigger follow the music fully. Only Modulate has a degree.
- **KILL is not a blackout.** The brief asked for an "immediate mute". §7 defines KILL as the no-signal value, so a pulse goes fully open and a chase holds; the lights do not go dark. BLACKOUT is still the way to dark.
- **UI:**
  - an AUDIO fader and KILL next to GM and BLACKOUT in the Playback panel;
  - an AUDIO slider and KILL above the audio quick effects in the Effects panel;
  - `StageCraft.Music.Master` and `StageCraft.Music.Kill` (dev).

### Listening (§6)
- "● LISTENING" shows in the status bar and in the Audio panel's header while Input or SystemAudio is Starting or Running.
- Both are pushed by `OnAudioStatusChanged`.

### Latency offsets
- **The Audio panel's LATENCY slider** (−500..+500 ms, 5 ms steps):
  - It edits the offset of the source playing (File, Input or SystemAudio) and is hidden for TestSignal and Off.
  - Positive makes the lights later.
  - The value is applied on release, as one settings request.
- **No restart:** `ApplySettings` now restarts the analysis only for band edges and the test signal's tempo.
  - Timing changes go to the running analyser (`FStageAudioAnalyzer::SetHeardDelaySeconds`, an atomic read per block), so a latency change does not lose the tempo. Before G3 every settings change restarted it, which took about 3 s to find the beat again.
  - Frames already held keep the delay they were stamped with.

### Tests
- `Show.FlashLimiter` (the gate, the strobe cap, the master's band, the setting bounds);
- `Show.AudioMaster` (Modulate Rate at 1, 0.5 and 0; Trigger holds and primes again; Sync keeps its tempo);
- `Show.FlashLimitOnFixtures` (an 8 Hz synced square limited to 3 a second on a real fixture, untouched at 10 and without music; the shutter capped; Bass Pulse under the master);
- `Show.AudioOnFixtures`, extended with KILL on the show, the limit pushed from the settings, and a latency change that keeps the tempo.

## 16. Amendment (STATE.md #86, 2026-11-10): a music strobe rests open, and FOLLOW MUSIC

- **Problem.** §4 makes Modulate read the band as 1 without music, so a show built on audio stays lit. For an effect on the **Shutter** that means the strobe's fastest setting in silence and under KILL, which is the opposite of what §7 wants.
- **Decision.** `StageEffects::GetSilentBand(Effect)` is 0 for a Beam effect on `Attribute_Shutter` and 1 for every other effect. `GetModulationBand` and `ApplyAudioMaster` take it, and the runner starts a new effect's smoothed band at it, so a music strobe starts open. Intensity, colour, position and other beam effects are unchanged byte for byte.
- **FOLLOW MUSIC.** Users read "the lights do not react to the music" (Gevor, 2026-11-10) when a source played but no effect followed it: listening alone moves nothing, by design (§4). The Audio panel gains a LIGHTS row: FOLLOW MUSIC starts `StageEffectRules::MakeMusicLook()` on the selected fixtures (bass → Bass Pulse on intensity; mids → Mid Colour, a rainbow whose rate the mids double; highs → High Strobe on the shutter, open in silence and capped by the flash limiter) through `AModularPlayerController::RequestStartEffects`, all or nothing and one undo step, with a hint saying why listening alone moves nothing.
- **Tests:** `StageCraft.ImageLook.MusicLook`.
