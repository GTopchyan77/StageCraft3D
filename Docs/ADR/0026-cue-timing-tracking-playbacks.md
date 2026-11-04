# ADR 0026: Cue timing, tracking, playbacks and masters

- **Status:** accepted.
  - F3a (timing and tracking): implemented with STATE.md #61.
  - F3b (cue lists, playbacks and masters): with STATE.md #62.
- **Related:**
  - ADR 0011 (cue list), ADR 0025 (programmer and output mix), ADR 0028 (palettes and references), ADR 0003 (undo).
  - tasks/todo.md Phase 34 (gaps G5, G6, G7).
  - Rules.md: one writer of fixture output, request bridge, ticks only while busy, §22 (measure), §37 (explicit state).

## 1. Problem

| Gap | What was missing |
|---|---|
| G5 | A cue had one fade time. There was no delay, no separate time for dimmers going down, no time per feature, no follow or wait, and no fade curve. |
| G6 | Every cue was a full snapshot of every fixture, so a change could not "track" through the cues that follow. grandMA3, MagicQ and Titan all track by default. |
| G7 | There was only the main stack: no playback faders, no flash, no Grand Master and no Blackout. |

F3a closes G5 and G6. F3b closes G7.

## 2. Decision: cue timing (F3a)

`FShowCue` keeps `FadeTime` as the cue's **In** time, so every existing reader keeps working. A new `FStageCueTiming Timing` member holds the rest:

| Field | Meaning |
|---|---|
| `Delay` | Seconds after Go before anything in the cue moves. |
| `bHasOutTime` / `OutTime` | The time for intensity going **down**. Without it, intensity going down uses the In time. |
| `FeatureTimes` | Optional times per feature (Intensity, Position, Color, Beam, Focus), using the palette kinds of ADR 0028. A feature time beats the In time; for intensity going down, the Out time beats both. |
| `Trigger` | `Go` (manual); `Follow` (starts when the cue before it finishes); `Wait` (starts `WaitTime` seconds after the cue before it was started). |
| `Curve` | `Linear` or `SCurve` (smoothstep), for the whole cue. |

`StageCueTiming::GetAttributeTiming` (pure) gives each attribute its delay and duration. The fade engine keeps one lerp per changing attribute, each with its own delay and duration. A cue is finished when its longest lerp ends, and only then does `OnCueFinished` fire.

**Follow and Wait:**
- When a cue starts from Go or Go To, the next cue's trigger arms an automatic Go: on finish for Follow, after `WaitTime` for Wait.
- **Back never arms**, because a step backwards should not run a chain.
- The automatic Go fires from the tick, never inside the call that started the cue. So a chain of zero-time Follow cues advances one cue per frame and cannot recurse.
- A manual Go, Back, Go To, a show load or a switch to External DMX cancels it.
- The subsystem ticks while an automatic Go is armed, and never at idle.

**Back** plays the cue before the active one, with that cue's own times, as before.

## 3. Decision: tracking (F3a)

A cue stores **only what it changes**. The look at cue *n* is worked out by `StageCueTracking::StateAt`:
- It starts at the nearest **blocked** cue at or before *n*.
- It walks forward, and each attribute (with its palette reference) takes the value of the latest cue that sets it. A plain value set later replaces an earlier reference.

**Blocked cues:**
- A blocked cue inherits nothing. A fixture or attribute it doesn't hold goes back to the home look.
- **Block** in the panel sets the flag **and** writes the inherited look into the cue, so nothing on stage changes. **Unblock** only clears the flag.

**Playing a cue** (`GoToCue`, `RestoreActiveCue`) uses the computed state:
- every fixture in the state, or in the playback layer now, fades;
- attributes the state holds fade to its value (palette references resolved, ADR 0028);
- attributes only the playback layer holds fade back to the home look.

When the fade completes, the playback layer **is** the state, so the home look shows through where the state is silent.

**Store modes** (`EStageCueStoreMode`, chosen in the Cues panel; Look stays the default of the C++ and Blueprint API so existing callers and the assistant keep their meaning):

| Mode | What it stores |
|---|---|
| **Track** (the panel's default) | What the programmer holds, values and references, **merged** into the cue: a new cue, or an existing one re-recorded. Refused when the programmer is empty. |
| **Cue Only** | Track, and also the cue after it gets each changed attribute's old tracked value (or the fixture's home value), unless it sets that attribute itself. The change then stops at this cue. |
| **Look** | Every fixture's whole look (never the highlight) as a **blocked** cue, replacing the cue. This is what every store did before #61. |

After any store, the programmer is cleared and the stored cue becomes the playing one (ADR 0025 §3), as one undo step.

**Old files:** v3–v7 cues were full snapshots, so they load as **blocked** cues and play as they did. The one difference: a fixture added after a cue was recorded now goes to its home look in that cue, instead of keeping whatever it showed.

**Cost:** `StateAt` starts from the nearest block, so a list of old snapshot cues costs one cue. It runs on Go, Back, Go To, restore and store, never per frame.

## 4. Requests (F3a)

Through `AModularPlayerController`, judged by `EvaluateCueRequest` (`Store` or `Update`), one undo step each:
- `RequestStoreCue(number, label, fade, mode)`.
- `RequestUpdateCueTiming(number, timing)`: delay, out, feature times, trigger, wait, curve.
- `RequestSetCueBlocked(number, blocked)`.

Times are sanitized by `StageCueRules::SanitizeTiming`: finite, 0 to 600 s, and only the five feature kinds (not All).

## 5. Scene format v8 (F3a)

Each cue may carry:
- `"delay"`;
- `"out"`, only when set;
- `"times"`, as `{"Intensity": s, ...}`;
- `"trigger"` (`"Go"`, `"Follow"` or `"Wait"`) and `"wait"`;
- `"curve"` (`"Linear"` or `"SCurve"`);
- `"block"`.

Unknown names, non-finite or out-of-range values are dropped and counted in `InvalidCueEntries`, and the cue still loads. Files from v3 to v7 load with every cue blocked.

## 6. Decision: playbacks and masters (F3b, STATE.md #62)

**Ten playbacks** (`StagePlaybackRules::Count`). Each one **owns its own tracking cue list** (`FStagePlayback {Name, Cues}`), instead of being assigned one of several shared lists.
- **REC** stores what the programmer holds as the playback's next cue, as a tracking cue, then clears the programmer (one undo step).
- **GO** steps through the list. It stops at the last cue.
- A single-cue playback is a submaster.
- *Deviation from the plan:* there is no separate "cue list pool" or list picker. A console's playback with its own stack covers the brief, and it avoids a second editor for lists the Cues panel would also have to show.
- *Limitation:* a playback steps at once, because its fader is the fade. Its cues' timing is not played.

**Live state** (`FPlaybackRuntime`, not saved, not undoable):
- fader level, flash, current cue, and an activation order;
- the current cue's tracked look, with references resolved.

| Action | Effect |
|---|---|
| A fader leaving 0, a flash going down, a GO | Activates the playback, at its first cue if it was released, and makes it the latest for LTP. |
| REL | Fader to 0, flash off, back before the first cue. |
| A show load | Releases every playback. |

**The mix** (`StageShowMix::ResolveAttribute`, pure), from the bottom up:
1. the home look;
2. the main cue list and the active playbacks:
   - **Intensity** (the Intensity kind: the dimmer and the cell dimmers) is **HTP**: the highest of the main list's value and each playback's value × its effective level (1 while flashed).
   - **Every other attribute** is **LTP**: the active playback with the highest activation order that holds it, otherwise the main list;
3. the programmer;
4. the highlight;
5. then intensity × **Grand Master × (Blackout ? 0 : 1)**. This includes the programmer and the highlight, because Blackout means dark.

**The fade engine:**
- While no playback is active and the masters are at full (`IsMainOnlyMix`), a fade frame is written straight onto the fixtures, exactly as before. The benchmark's path is unchanged.
- Otherwise each fading fixture is re-mixed every frame. Measured at **+12%** show tick (§8).

**External DMX:**
- Playbacks are refused (GameMode), and the mix uses only the programmer and highlight.
- The masters scale incoming intensity after `ReadDMX`. When they change, every universe is read again in full on its next frame, because the console's raw values are not kept separately. So under External the masters take effect on the next frame, which a console sends at 44 Hz.

**Recording:** a Look store and a palette record take the look without the highlight **and without the masters** (`GetRecordableValues` re-mixes when either is in play). So a store with the Grand Master at 50% never records halved dimmers.

**Requests** (`EvaluateCueRequest`; the playback's index travels as the number, 0–9, checked):

| Kind | Requests | Rules |
|---|---|---|
| `PlaybackEdit` (undoable) | REC, CLR, rename | `FStagePlaybacksCommand` holds every playback before and after. |
| `PlaybackLive` (live) | level, flash, go, release | Refused under External. Letting go of a flash is never refused, so nothing sticks at full. |
| `Masters` (live) | Grand Master, Blackout | Always allowed. |

**Keys:** Blackout toggles on Ctrl+Shift+B. It is in the SHOW category, works from any panel, and was appended to `EStageInputCommand`.

**Scene format v8** (the same version as F3a, since nothing in between was committed):
- `"playbacks": [{"index", "name", "cues"}]`, holding only the used playbacks.
- Cues use the main list's format.
- A bad index, a duplicate or a bad cue is dropped and counted in `InvalidCueEntries`.
- Not saved: fader levels, flash, Blackout and the Grand Master. A stage always opens at full and released.

**UI:**
- **The Playback panel** (`StageCraft.Panel.Playback`, Window > Playbacks & Masters...; `DA_Panel_Playback`, made by `Scripts/Content/create_playback_content.py`):
  - ten strips, each with: name, cue position, vertical fader, level, GO, FLASH (held), REC, REL and CLR;
  - GM and BLACKOUT;
  - the strips are built once and refreshed in place on the next tick, so a fader being dragged is never rebuilt.
- **The built-in "Lighting Desk" layout:**
  - viewport over the Playback, Cues and Fader Bank tabs;
  - Groups and Palettes on the right;
  - Inspector and Outliner below them.

**Development commands** (compiled out of Shipping):
- `StageCraft.Show.Playback <1..10> Record|Clear|Rename|Level|Flash|Go|Release|List`
- `StageCraft.Show.Master GM <0..1>|Blackout On|Off`
- `StageCraft.Bench.Fixtures ... GM=<0..1>`, which measures the fully mixed fade path.

## 7. Alternatives rejected

- **Keep snapshots and add "track" as a copy-forward at store time.** Then every later cue holds a copy, and re-recording one cue no longer changes the cues it should flow into. That is the opposite of tracking.
- **Store tracked state as a cached full snapshot in each cue.** That is two sources of truth that would drift on every edit. `StateAt` computed from the nearest block is cheap enough at Go time.
- **Per-attribute curves.** No console exposes them; a curve per cue covers the need.
- **A countdown timer per Wait cue in a timer manager.** The show already ticks while busy. A single armed automatic Go in the tick is one state to test and cancel, with no timer lifetimes to manage.

## 8. Consequences

- **v8 files are refused by builds before #61** (VersionMismatch).
- **Store from the panel now tracks by default.** The tooltip and the store-mode segment say so; LOOK gives the old behaviour.
- **A palette edit or a new fixture during a fade** still snaps the fade to its end (ADR 0028 §3).
- **Measured cost** (`RunFixtureBench.ps1 -Counts 200,500`, show tick while crossfading):

  | Path | 200 fixtures (mean / max) | 500 fixtures (mean / max) |
  |---|---|---|
  | Main only (GM=1) | 3.56 / 6.90 ms | 7.99 / 15.6 ms |
  | Fully mixed (GM=0.9) | 4.00 / 8.92 ms | 8.92 / 18.2 ms |

  The main-only path matches F2 (3.64 / 8.39 ms means), so it has not regressed.
- **Fixtures without a dimmer channel are not mastered.** Their intensity lives in their colour levels, so the Grand Master and Blackout leave them as they are. A virtual dimmer for them is a follow-up.
- **A playback's cue timing is not played**: the playback steps at once.
