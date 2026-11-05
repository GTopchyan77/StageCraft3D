# ADR 0029: Audio analysis and audio-reactive effects

- **Status:** proposed (draft 2026-11-05, STATE.md #67). Not approved, not implemented. Spike G0 must confirm the facts marked **(G0)** before G1 starts.
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
| A6 | **(G0)** WmfMedia plays a local `.mp3`, `.wav` and `.flac` through `UMediaPlayer` in a **Shipping** build, and `UMediaSoundComponent` output can be sent to a chosen submix. | to be measured | If not, files are decoded by a small WAV reader of our own plus the Media path for MP3 only, or MP3 is dropped. |

## 3. Decision: sources, and one analysis path

**`EStageAudioSource { Off, File, Input, SystemAudio, TestSignal }`** is one explicit state. Exactly one source is live at a time.

| Source | How | Notes |
|---|---|---|
| File | `UMediaPlayer` + `UMediaSoundComponent` sent to `SM_StageMusic`, tapped by A1 | Transport: play, pause, seek, loop. The listener hears it. |
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
- **AudioSynesthesia:** UObject analysers made for Blueprint, and a plugin dependency. Whether its real-time analysers could replace our onset stage is checked in G0, but tempo and beat phase are ours either way.

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
- **Dev console** (`#if !UE_BUILD_SHIPPING`): `StageCraft.Audio.Source`, `StageCraft.Audio.Status`, `StageCraft.Audio.TestSignal bpm=128`.

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

- New Private `Audio/` files, two engine module dependencies (`SignalProcessing`, `MediaAssets`), WmfMedia enabled if G0 confirms A6, and on Windows `ole32` (already linked).
- Scene format v11.
- **Follow-ups:**
  - songs saved with a show, and beat-triggered cues;
  - section detection (verse and chorus) for an "auto show";
  - an audio-energy input to the crowd (Phase I);
  - MIDI clock and Ableton Link as tempo sources.

## 12. Rollback

Set `Mode = Off` on every effect (v11 files keep loading), then remove `Audio/` and the panel. The effects engine needs no change to fall back: Off is today's behaviour.
