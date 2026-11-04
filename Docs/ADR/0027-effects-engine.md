# ADR 0027: Effects engine

- **Status:** accepted.
  - F4a (the engine, programmer effects, the Effects panel): implemented with STATE.md #63.
  - F4b (effects stored in cues and playbacks, with tracking): planned, STATE.md #64.
- **Related:**
  - ADR 0025 (programmer and output mix), ADR 0026 (cue timing, tracking, playbacks and masters), ADR 0028 (palettes), ADR 0003 (undo).
  - tasks/todo.md Phase 34 (gap G8).
  - Rules.md: one writer of fixture output, request bridge, ticks only while busy, §22 (measure), §54 (determinism).

## 1. Problem

Gap G8: StageCraft had no effects engine. Every console it is measured against (grandMA3, MagicQ, Titan, Daslight) runs procedural
effects: a circle or figure 8 on movers, a dimmer chase across a row, a rainbow, all spread in phase over the selection and run at a
rate in BPM. Without them every movement had to be built from cues by hand.

## 2. Decision: the effect and its maths

**`FStageEffect`** (`Public/Show/StageEffectTypes.h`) is one effect's settings:

| Setting | Meaning |
|---|---|
| `Target` | What it moves: **Intensity** (every intensity attribute: dimmer, cell dimmers), **Position** (pan, tilt), **Color**, **Beam** (one beam or focus attribute, `Attribute`). |
| `Waveform` | Sine, Cosine, Square (with `Duty`), Ramp Up, Ramp Down, Triangle, Random, Circle, Figure 8. Circle and Figure 8 move pan and tilt together; on any other target they act as a sine. |
| `Axis` | Position, one-axis waveforms: Pan, Tilt or Both. |
| `Low`, `High` | Intensity and Beam: the bottom and top of the wave as fractions 0..1 of **each fixture's own range**, so one effect suits every fixture type. |
| `Size` | Position: degrees either side of the static position. |
| `ColorMode`, `ColorA`, `ColorB` | Color: fade A..B with the wave, or Rainbow (the hue circle at full saturation). Each fixture matches the colour its own way (`MatchColor`: emitters, CMY, nearest wheel slot), as colour palettes do. |
| `RateHz` | Cycles per second; shown as BPM too (×60). 0 holds the pattern still. |
| `Phase` | Where the first fixture starts, in degrees. |
| `Spread` | Degrees between the first and the last step, across all fixtures: 0 = in step, 360 = one wave across them (a chase). |
| `Wings` | The fixtures split into equal parts, every other one mirrored: symmetrical patterns. |
| `Blocks` | Fixtures that move together as one step. |
| `Direction` | Forward (first to last), Reverse, Bounce (out and back without a jump: the clock folds 0→1→0). |
| `SizeFan` | The existing fan modes (ADR 0025 §4) scale the size per fixture. |

**`StageEffects`** (`Public/Show/StageEffects.h`) is pure and has no world:
- `GetPatternStep`: a fixture's step after wings, then blocks.
- `GetFixtureOffset`: `(Phase + Spread × step / steps) / 360` cycles.
- `GetWavePosition`: the clock with direction applied, minus the offset.
- `SampleWave`: the shape at a position, giving a unit level (0..1) and pan/tilt (−1..1).

**Determinism (Rules §54):** Random draws one level per whole cycle from a hash of (fixture step, cycle). The same effect gives the
same values on every run, fixtures in one block share a level, and mirrored wings stay symmetrical.

**The clock is counted in cycles**, advanced by each effect's own rate. Changing the rate therefore never jumps the pattern, and an
edit (same entry id) keeps its clock.

## 3. Decision: effects in the output mix

The effect layer sits **above the programmer and below the highlight**; the masters still apply last (ADR 0025 §2, ADR 0026 §6):

1. home look; 2. main cue list and playbacks (HTP/LTP); 3. programmer; **4. effects**; 5. highlight; 6. intensity × masters.

- **Position effects are relative:** their degrees are added to the static value beneath. An effect on a fixture with no static
  value does nothing.
- **Intensity, colour and beam effects are absolute:** they replace the static value.
- Each effect value has a **weight** (0..1): the static value shows through the rest. F4a always uses 1; F4b uses it to fade an
  effect in and out with its cue.
- Where two effects drive one attribute, the later entry wins.
- The highlight beats an effect (it means "show me this fixture"), and the Grand Master and Blackout scale an intensity effect.

**The runner** (`FStageEffectRunner`, `Public/Show/StageEffectRunner.h`), owned by `UShowControlSubsystem`, keeps every running
effect's clock and finds the fixtures it drives once (on change, on fixture register and unregister), never per frame. Per frame it
evaluates only the fixtures an effect drives, and only the attributes it drives. `UShowControlSubsystem` stays the only writer:
`ResolveFixture` gains a scope (`All`, `EffectAttributes`) and an `Effects` layer.

**Ticking:** the subsystem ticks while an effect runs under Internal control (`AreEffectsLive`). The tick advances the clocks first,
then the fades (which, with an effect live, go through the full mix: `IsMainOnlyMix` is false), then writes the effect attributes.
With no effect running the existing paths are untouched.

**Recording:** a Look store and a palette record take the static look, never the value an effect happened to show
(`GetRecordableValues` re-mixes without the effect layer).

**External DMX:** effects need a static value to move around and are written over the cue look, both of which the console owns.
Under External they hold still and are not written; the GameMode refuses effect requests.

**Renumbered fixtures** leave their effects (an effect names fixtures by id); a destroyed fixture simply drops out.

## 4. Decision: where effects live, requests, undo, files

- **F4a: the programmer** holds effect entries (`FStageEffectEntry {Id, Effect, FixtureIds}`) beside its values
  (`UStageProgrammerSubsystem::AddEffect`, `UpdateEffect`, `RemoveEffect`, `SetEffects`). Changes broadcast `OnEffectsChanged` (the
  show restarts its runner) and `OnValuesChanged` (the stage is dirty, panels refresh). Clear empties values **and** effects as one
  undo step.
- **Requests** (`AModularPlayerController`, judged by `EvaluateCueRequest` with the new `EStageCueRequest::Effects`, refused under External):

  | Request | Undo |
  |---|---|
  | `RequestStartEffect(Effect)`: on the selected fixtures, in selection order | one step |
  | `RequestPreviewEffect(Id, Effect)`: while a control is dragged | none; the first preview remembers the effects before |
  | `RequestUpdateEffect(Id, Effect)`: a release or a button | one step from before the first preview |
  | `RequestStopEffect(Id)`, or every effect with an invalid id | one step |

- **Undo:** `FStageProgrammerEffectsCommand` holds every programmer effect before and after; `IStageItemEditor::SetProgrammerEffects`.
- **Validation** (`StageEffectRules`): finite values, rate 0..25 Hz, size 0..270°, phase and spread ±3600°, wings 1..16, blocks 1..512,
  Low/High/Duty and colours 0..1, a Beam attribute that is really a beam or focus attribute (else Zoom), at most 64 effects of at most
  4096 unique positive fixture ids each. Applied on every entry: request, undo step and file.
- **Scene format v9:** `"programmerEffects": [{id, name, fixtures, target, waveform, attribute, axis, colorMode, colorA, colorB, low,
  high, size, rate, duty, phase, spread, wings, blocks, direction, sizeFan}]`. Enums are written by name. An entry without a valid id is
  dropped; an unknown name or a bad number falls back to the default for that setting; each is counted in `InvalidShowEntries`.
  Files v1–v8 load with no effects. v9 files are refused by builds before #63.

## 5. F4b (planned, STATE.md #64)

Cues and playback cues hold effect entries too, tracking like values: a later cue with the same id replaces the effect, a stop entry
ends it, Block starts from none; Track store carries the programmer's effects into the cue, Cue Only adds a stop in the next cue, Look
captures every running effect. An effect fades its weight in and out over its cue's In time. A playback's effects scale their
intensity by its fader.

## 6. UI

- **The Effects panel** (`StageCraft.Panel.Effects`, Window > Effects..., floating 900×460, also a tab of the Lighting Desk layout;
  `DA_Panel_Effects` made by `Scripts/Content/create_effects_content.py`):
  - **Quick effects:** Circle, Figure 8, Pan Sweep, Tilt Wave, Dimmer Sine, Dimmer Chase, Strobe, Random Sparkle, Rainbow, Colour
    Fade, Iris Pulse, Zoom Breathe (`StageEffectRules::MakeQuickEffects`); one click starts one on the selection.
  - **Editor** of the chosen running effect: WAVE, DIR, FAN, AXIS, COLOUR, A, B and BEAM choices (only those that apply show), and
    RATE (Hz and BPM, quadratic up to 10 Hz, with **TAP**: one cycle per tap, the mean of the last five), SIZE, LOW, HIGH, PHASE,
    SPREAD, DUTY, WINGS and BLOCKS sliders; a drag previews live and its release is one undo step.
  - **Live preview** (`UStageEffectPreview` hosting `SStageEffectPreview`): one cycle of the wave and a dot per fixture where the
    pattern has it now (Circle and Figure 8 draw their pan/tilt path); repaints on an active timer.
  - **Running list** with STOP per effect and STOP ALL.
- **Dev command** (compiled out of Shipping): `StageCraft.Show.Effect Start <quick effect> [key=value...] | Set <n> key=value... |
  Stop <n>|All | List`, keys `rate bpm size low high phase spread duty wings blocks wave dir fan axis`.

## 7. Measured cost

`StageCraft.Bench.Fixtures ... Effects=1|2` (and `RunFixtureBench.ps1 -Effects`) runs a circle and a dimmer sine (2: also a rainbow)
on every bench fixture through the idle and crossfade phases. Numbers are in STATE.md #63.

## 8. Alternatives rejected

- **Effects as a separate world subsystem writing fixtures.** A second writer of fixture output would fight the mix (Rules: one
  writer). The runner is a plain object the show subsystem owns.
- **Evaluate effects by re-mixing every attribute of each driven fixture each frame.** Measurably more work for no gain: only the
  attributes an effect drives can change.
- **Store each effect's start time and compute `time × rate`.** Changing the rate would jump every fixture to another point in the
  pattern; an integrated clock does not.
- **An effect pool (saved templates) in F4.** The quick effects plus effects stored in cues (F4b) cover the brief without a fourth
  pool editor; a pool can come later on the same `FStageEffect`.
- **A global speed master.** Each effect has its own rate and TAP; a master can scale the runner's advance later without a format change.

## 9. Consequences

- The show ticks while an effect runs. With an effect live every fading fixture is mixed in full (as with a playback up).
- Effects in the programmer survive save and reload (v9) and Clear removes them.
- Under External DMX effects hold still; switching back to Internal resumes them from where their clocks stopped.
- Fixtures without a dimmer channel still ignore the Grand Master (ADR 0026 §8); an intensity effect drives their cell dimmers if
  they have them.
