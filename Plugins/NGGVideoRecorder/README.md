# NGG Video Recorder

Records the running game viewport straight to an **MP4** (H.264 video + AAC audio), in PIE **and in packaged
builds**. No external tools, no FFmpeg, no redistributables — it uses Media Foundation, which ships with
Windows.

This is the "record my simulation while it runs" tool. It is complementary to Movie Render Queue, not a
replacement — see [When to use what](#when-to-use-what).

---

## Quick start

1. The plugin is already enabled in `RecordProject.uproject`.
2. Play the game (see [PIE caveat](#pie-caveat) below).
3. Use the **built-in control panel** in the bottom-right corner — **Record / Stop**, elapsed time, and
   an **Export** button that opens the finished MP4 in Explorer. Or just press **F9**.
4. The file lands in `<Project>/Saved/VideoCaptures/` — in a packaged build, next to the `.exe` under
   `<Game>/Saved/VideoCaptures/`.

That is the whole thing. Everything below is for when you want more control.

---

## Four ways to drive it

### Built-in control panel

A native Slate panel the plugin adds to the game viewport — no widgets to create, no input to bind,
works identically in PIE and packaged builds:

- **Record / Stop** button, a live **REC 00:12** elapsed readout, the last file's name, and an
  **Export** button that reveals the MP4 in Windows Explorer.
- Shown automatically when the game starts (**Project Settings → Plugins → NGG Video Recorder → UI →
  Show Recorder UI On Startup**), and toggled any time with **F10** (`Toggle UI Key`) or the
  `NGGRecorder.ToggleUI` console command. Blueprint/C++ can call `Show/Hide/ToggleRecorderUI` on the
  subsystem.
- If the game hides the mouse cursor, the panel turns it on while open and restores it when closed.
- The panel is part of the presented image, so it appears in the recording. Turn on
  **Hide Panel While Recording** to collapse it during capture (stop with F9; the panel returns when
  recording ends).

### Hotkey

Configured in **Project Settings → Plugins → NGG Video Recorder → Hotkey**. Default **F9**. It is handled at
the Slate level, so it works in a packaged build without the game binding any input.

### Console commands

```
NGGRecorder.Start [filename]   Start recording (optional file name, no extension)
NGGRecorder.Stop               Stop and finalise the MP4
NGGRecorder.Toggle             Start if idle, stop if recording
NGGRecorder.ToggleUI           Show or hide the control panel
NGGRecorder.Status             Print resolution, frame rate, elapsed time, frames written/dropped
```

### Blueprint / C++

`Get Game Instance Subsystem → NGG Video Recorder Subsystem`, then:

| Node | What it does |
|---|---|
| `Start Recording` | Begins a recording with explicit options. Returns the file path. |
| `Start Recording (Project Defaults)` | Same, using the settings from Project Settings. |
| `Stop Recording` | Finalises the MP4. Blocks briefly while the encoder drains. |
| `Toggle Recording` | Start/stop. |
| `Is Recording` / `Get Recording Duration` / `Get Stats` | Status, for a HUD. |
| `Get Last Recording Path` | Absolute path of the current or last recording. |
| `Reveal Last Recording In Explorer` | Opens the containing folder. |
| `On Recording Started` / `On Recording Finished` | Events. `Finished` reports success plus an error string. |

From C++:

```cpp
UNGGVideoRecorderSubsystem* Recorder = GetGameInstance()->GetSubsystem<UNGGVideoRecorderSubsystem>();

FNGGVideoRecorderOptions Options;
Options.Resolution = ENGGRecorderResolution::HD1080;
Options.FrameRate  = 60;
Options.Quality    = ENGGRecorderQuality::High;

FString Path, Error;
if (!Recorder->StartRecording(Options, Path, Error))
{
    UE_LOG(LogTemp, Error, TEXT("%s"), *Error);
}
```

---

## Options

All of these exist both on the `Start Recording` node and as project-wide defaults in
**Project Settings → Plugins → NGG Video Recorder**.

| Option | Notes |
|---|---|
| **Frame Rate** | Output frame rate. 1–240. |
| **Resolution** | Match Viewport, 720p, 1080p, 1440p, 4K, or Custom. Rounded down to even numbers, which H.264 requires. |
| **Quality** | Low / Medium / High / Very High, or a custom Mbps. Presets are bits-per-pixel-per-frame, so "High" means the same thing at 720p30 and 4K60. |
| **Preserve Aspect Ratio** | Letterbox with black bars rather than stretch when the window aspect does not match the output aspect. |
| **Capture Audio** | Records the main submix as an AAC track. |
| **Lock To Fixed Frame Rate** | See below — this is the important one for deliverables. |
| **Output Directory** | Blank means `<Project>/Saved/VideoCaptures`. Relative paths resolve against the project directory. |
| **File Name** | Supports `{project}`, `{map}`, `{date}`, `{time}`. Blank means `{project}_{map}_{date}_{time}`. A numeric suffix is added if the file exists. |

### Lock To Fixed Frame Rate

This is what makes a heavy simulation record cleanly.

- **Off** (default): the game runs in real time and frames are captured at the target rate. If the sim drops
  to 40 fps while you record at 60, the video honestly shows that stutter.
- **On**: the engine advances exactly `1/FrameRate` of game time per rendered frame. The game no longer runs
  in real time — it runs as fast as it can render — but **every output frame is rendered in full**, so a
  1000-frame recording is exactly 1000 frames of perfectly smooth motion no matter how heavy the scene is.
  This is the same trick Movie Render Queue uses.

Audio is disabled when this is on, because the audio engine still runs against the wall clock and would drift
away from the video within seconds.

---

## PIE caveat

The recorder captures the back buffer of the window the game viewport lives in. That means:

| How you run | What gets recorded |
|---|---|
| **Packaged build** | The game. Always correct. |
| **Standalone Game** | The game. Correct. |
| **PIE → New Editor Window** | The PIE window. Correct. |
| **PIE → Selected Viewport** | The **entire editor window**, including the toolbar and panels. |

So for a clean capture in-editor, play in a **separate window**. In a packaged build there is nothing to
think about.

Because it is a back buffer capture, whatever is on screen is in the video — UMG, debug draws, `stat fps`,
and the plugin's own "REC" indicator (which is why that indicator defaults to **off**).

---

## When to use what

| | NGG Video Recorder | Movie Render Queue |
|---|---|---|
| Runs in a packaged build | Yes | Editor only in practice |
| Needs a Level Sequence | No | Yes |
| Captures live gameplay/physics as it happens | Yes | Only via Take Recorder first |
| Includes UMG / HUD | Yes | Only with extra setup |
| Anti-aliasing, motion blur, accumulation samples | Whatever the game renders | Far better — that is its whole point |
| Best for | Client review clips, bug repros, demos, anything from a shipped build | Final cinematic deliverables |

If the goal is a hero shot for a client, MRQ still wins on image quality. If the goal is "let the customer
press a button in the built simulator and get an MP4", this plugin is the answer.

---

## How it works

Per presented frame, on the render thread:

1. **Pace** — skip the frame unless the target frame interval has elapsed (skipped entirely when locked to a
   fixed rate, where one rendered frame is always one output frame).
2. **Resolve** — draw the back buffer into a fixed-size `PF_B8G8R8A8` render target. This one pass handles
   rescaling, letterboxing and format conversion, and means a mid-recording window resize does not break the
   stream (the encoder cannot change frame size once it has started).
3. **Read back** — enqueue an asynchronous GPU copy into a staging texture. There are three of these in a
   ring, so nothing ever blocks on the GPU.
4. **Hand over** — any readback that has landed is copied into a pooled buffer and pushed to the encoder
   thread, strictly in order.

The encoder thread owns a Media Foundation sink writer and interleaves the video and audio queues in
timestamp order. Video frames are held back by one so each frame's duration can be derived from the next
frame's timestamp — a hitch in the game lengthens a single frame instead of shifting the whole timeline.

Audio taps the main submix, downmixes to stereo (centre channel folded in at −3 dB), converts to 16-bit PCM
and timestamps from a running sample count, so the audio track is exactly the right length regardless of how
the video frames were paced.

Frame buffers are pooled and pre-allocated, so a recording does no per-frame heap allocation.

### Files

```
Source/NGGVideoRecorder/
  Public/
    NGGVideoRecorderTypes.h        Options and stats structs, enums
    NGGVideoRecorderSettings.h     Project Settings page
    NGGVideoRecorderSubsystem.h    The Blueprint-facing API
  Private/
    NGGVideoRecorderSubsystem.cpp  Lifetime, hotkeys, console commands, output paths, panel lifecycle
    NGGVideoRecorderPanel.*         The Slate control panel (record/stop, status, export)
    NGGVideoRecorderFrameCapture.*  Back buffer hook, resolve pass, readback ring
    NGGVideoRecorderAudioCapture.*  Submix listener and downmix
    NGGVideoRecorderWriter.*        Encoder thread, frame pool, A/V interleave
    NGGVideoRecorderEncoder.h       Encoder interface
    NGGVideoRecorderMFEncoder.cpp   Media Foundation H.264/AAC/MP4 implementation
```

---

## Performance

At 1080p60 the cost is one full-screen draw plus an 8 MB GPU→CPU copy per frame, and the H.264 encode runs on
the GPU's dedicated encoder (NVENC / Quick Sync / AMF) when one is present. Expect a small single-digit
percentage frame time cost on a modern GPU.

If `NGGRecorder.Status` reports dropped frames, the encoder is not keeping up. Raise
**Max Queued Frames** in the advanced settings (each slot costs `Width × Height × 4` bytes), lower the
resolution or bitrate, or turn on **Lock To Fixed Frame Rate**, which never drops frames.

---

## Limitations

- **Windows only.** Media Foundation is the encoder. The module is marked `Win64` in the `.uplugin`.
- **HDR output** is not tone-mapped. A 10-bit *integer* back buffer — which Windows commonly hands out for an
  ordinary SDR swap chain — converts to 8-bit cleanly and needs no attention. But if HDR display output is
  genuinely on, the back buffer is floating point, highlights above 1.0 are clipped, and a warning is logged
  once. Turn HDR display output off while recording.
- **Audio needs 44.1 or 48 kHz.** Media Foundation's AAC encoder accepts nothing else. At any other device
  rate the video still records and a warning is logged. 48 kHz is the engine default.
- Surround mixes are folded to stereo; rear channels are dropped.
- Output resolution is fixed when recording starts. Resizing the window mid-recording rescales into the
  original frame size rather than changing it.
