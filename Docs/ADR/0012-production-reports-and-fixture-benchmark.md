# ADR 0012: Bill of materials as PDF, cable schedule and power distribution, fixture benchmark

- **Status:** accepted. See STATE.md #39.
- **Related:**
  - ADR 0005 §5 (bill of materials), ADR 0011 §3 (PDF writer, patch sheet export), ADR 0010 §7 (packaging, measured sky cost).
  - Rules.md: "Roles of the framework classes", Engineering Standard §21–22 (no polling, measure before claiming), §27 (configuration), §29 (async), §55 (validate inputs), §65 (dev-only code).

## 1. Problem

Gevor's next milestone (2026-10-21) asked for:

1. **More production paperwork from the same PDF writer.**
   - The bill of materials as PDF and CSV: equipment counts, total weight, total power.
   - A cable schedule with electrical loads: a wiring list and the load per phase and circuit.
2. **A fixture stress test.** 50, 200 and 500 active fixtures running cues and crossfades, with frame time and CPU/GPU time measured.
3. **Shipping validation.** The Sky Creator content and the PDF code must build, cook and package in Shipping without warnings.

What already exists:
- `StageBillOfMaterials` (pure model + CSV) and `UStageBillOfMaterialsSubsystem` (cached report, CSV export on a worker).
- `FStagePdfDocument` (PDF 1.4 writer, Helvetica, A4) and the patch sheet export (ADR 0011 §3).
- Items carry `FStageEquipmentSpecs::PowerDrawWatts` and `WeightKg`. Nothing in the project knows about circuits, phases or cables.

## 2. Decisions: reports

### 2.1 A shared flowing table layout for PDF reports

- The patch sheet draws a fixed grid of exactly 30 rows per page. The bill of materials and the cable schedule need headings, subtotal rows and several tables in one document, so a fixed grid does not fit.
- New private helper `FStageReportPdfComposer` (`Private/Report/StageReportPdfComposer.h`). It is a value type with no UObjects and is safe on any thread. It handles:
  - the title block (report name, scene, date, one summary line) and the footer ("scene · report", "Page n of m") on every page;
  - section headings and short note lines;
  - tables with relative column widths scaled to the printable width, a header row repeated after every page break, striped rows, and bold subtotal and total rows;
  - page breaks. A heading never ends a page alone: it needs room for itself, the table header and one row.
- "Page n of m" is only known at the end. `FStagePdfDocument` gains `SetCurrentPage(Index)`, so the composer draws the frames after the body.
- Same style as the patch sheet: black and greys only (it must photocopy), Helvetica 8.5 pt tables, and A4 landscape.
- **The patch sheet keeps its own renderer.** It is tested and was checked visually in #38. Moving it onto the composer is a follow-up with no user-visible change.

### 2.2 Bill of materials: PDF next to the CSV

- `StageBillOfMaterials::ToPdf(Report, SceneName, GeneratedAt)` draws:
  - one heading per discipline, then that discipline's lines and a bold subtotal row;
  - a grand total row at the end;
  - a summary line: lines, units, total weight in kg, total power in kW.
- Columns: Item, Manufacturer, Qty, Placements, Unit kg, Total kg, Unit W, Total W.
- Export writes `BOM_<scene>_<time>.pdf` and `.csv`, both or neither, like the patch sheet. The result's `FilePath` is the PDF. The panel button reads EXPORT PDF; the File menu reads "Export Bill of Materials (PDF + CSV)".
- The CSV is unchanged: same columns, so existing spreadsheets keep working.

### 2.3 Cable schedule and power distribution: a planned proposal, not stored data

A professional cable schedule records decisions a crew makes on site: which distro, which circuit, which cable. StageCraft stores none of these today. Two options were considered:

| Option | Cost | Result |
|---|---|---|
| A. Users assign circuits and cables per item, saved in the scene (format v4), with a patch-panel-like editor | New persistent state, migration, a new panel, undo | Exact but empty until someone fills it in |
| **B. Generate a balanced plan from the stage on every export** | Pure function, no new state | A useful first plan on day one, reproducible, can be redone after every change |

**Decision: B.** The report is titled a *plan*, and its footer states the assumptions. Option A remains possible later. The generated plan would become its starting point.

**Model:** `StageCableSchedule` (pure namespace, `Public/Report/StageCableSchedule.h`).
- **Inputs:**
  - one `FStagePowerLoad` per placed item that draws power: label, item type, fixture ID, watts = unit power × quantity;
  - one `FStageDmxNode` per lighting fixture: ID, label, universe, address, footprint, position;
  - the settings.
- **Settings** (`FStagePowerDistribution`), configurable in Project Settings › StageCraft › Power Distribution (`UStagePowerDistributionSettings`, `DefaultGame.ini`):

  | Setting | Default | Valid range |
  |---|---|---|
  | Phase voltage | 230 V | 100–400 |
  | Phases | 3 | 1 or 3 |
  | Circuit rating | 16 A | 1–125 |
  | Usable share of a circuit (continuous load) | 80 % | 10–100 % |
  | Power factor | 1.0 (the specs give watts; 1.0 treats W as VA) | 0.5–1.0 |

  The model clamps every value into its range (Rules §27, §55), so a damaged ini cannot produce a division by zero or a negative current.

- **Circuits.** These are deterministic, so the same stage always gives the same plan:
  1. Order loads by power, highest first. Break ties by label in natural order, then fixture ID.
  2. Put each load on the phase that carries the least power so far (ties: the lower phase), on the first circuit of that phase that still has room within the usable share. If none has room, open a new circuit on that phase.
  3. A single load larger than one circuit's usable share gets its own circuit, flagged **OVERLOAD: needs a larger feed**. It is never silently split.
  4. Current = watts / (voltage × power factor). Each circuit and phase reports W, A and the share of its rating used. Phase imbalance = (largest − smallest phase load) / largest phase load.
- **Superseded 2026-10-21 by ADR 0014 §2.3–2.4:** every run is now measured along its route (floor and truss) from the distros or the DMX node, with a configurable safety margin and stock list; long runs are joined cables instead of "split"; circuits are outputs of counted distros ("PD1 C03 / L2").
- **Power cables.** One home-run per load: "P-001" from "Distro C03 / L2" to the item. Lengths are not computed, because the distro position is not part of the stage; the column reads "site".
- **DMX cables.** One daisy chain per universe, ordered by start address:
  - "D1-01" runs from the console or node to the first fixture, then each fixture links to the next, and the chain ends with a terminator note. Ends are named "#ID label" ("#4 Spot 101"), because unlabelled fixtures of one type share a name.
  - Link lengths are the straight-line distance between the two fixtures plus 20 % for routing and drops. The result is rounded up to the next stock length (1, 2, 3, 5, 10, 15, 20, 25, 30, 50 m). A link above 50 m is flagged "split: add a node or opto-splitter".
  - The first link reads "site".
  - Unpatched fixtures are not chained. They are counted in the summary.
- **Warnings** are printed on the first page: overloaded circuits, phase imbalance above 15 %, unpatched fixtures, and lighting fixtures with no power data (0 W).

**Export:** `UStageCableScheduleSubsystem` follows the patch sheet exactly.
1. It gathers plain rows on the game thread.
2. It builds, renders and writes PDF + CSV (both or neither) on a worker.
3. The result returns to a weakly held subsystem.

It is requested through `AModularPlayerController::RequestExportCableSchedule`, from File › Production › "Export Cable Schedule & Power (PDF + CSV)" and from `StageCraft.Report.ExportCables`. There is no tick and no cached state.

The shared "write PDF + CSV, both or neither" step moves into `StageReportFiles::WritePdfAndCsv`, used by all three exports.

## 3. Decisions: fixture benchmark

- **Development-only harness:** `StageCraft.Bench.Fixtures <count> [<count>...] [Seconds=10] [Fade=2] [Settle=3] [Zoom=1] [Item=<asset>]`, compiled out of Shipping (§65). It runs each count in turn, inside the real game world:
  1. Spawn the fixtures through the controller's `USpawnSystemComponent`, which is the same registration path as a user placement, including auto-patch. They form a grid at 6 m with 1 m spacing. The first lighting fixture in the catalog is used.
  2. Settle, and wait until no model import is pending. The startup re-import of the user's models runs Interchange on the game thread and would otherwise be measured as fixture cost.
  3. Record two cues: a dim, warm look and a bright, cool, panned look. `Zoom=0` keeps the zoom equal in both (see §3.1).
  4. Measure an **idle** phase (no fades) for N seconds.
  5. Measure a **crossfade** phase for N seconds, with continuous Go between the two cues at the given fade time.
  6. Clean up: delete the fixtures, then give back the user's show. That means the cue list, the active cue (so GO continues from it) and every existing fixture's look; the bench cues record every fixture, so cue playback changes the user's fixtures too. It also restores a clean session if it was clean.
- **Per frame it records:**
  - frame time, game thread time and GPU time (the `stat unit` sources);
  - the time spent inside `UShowControlSubsystem::Tick`. It is measured inside the subsystem with `FPlatformTime`, in non-Shipping builds only (`GetTickCostStats` / `ResetTickCostStats`, always zero in Shipping). Reading it does not change behaviour.
  - The render thread counter (`GRenderThreadTime`) is not sampled. It is computed per Slate window present and reads 0 in this multi-window workspace. When the frame is longer than both the game thread and the GPU, the render thread is the limit.
- **Summary:** average, median, p95, p99 and maximum, plus **frame drops**. A frame drop is a crossfade frame longer than 1.5 × the idle median and longer than the 60 Hz budget (16.7 ms). The summary goes to the log and to `Saved/StageCraft/Benchmarks/FixtureBench_<time>.csv`.
- Each phase is a trace region ("Bench 500 crossfade"), so an Unreal Insights capture (`-trace=cpu,frame -statnamedevents`) can be cut to one phase.
- The statistics are a pure function (`StageFrameStats::Summarize`, `CountDroppedFrames`) with an automation test.
- The harness is a plain C++ object driven by `FTSTicker`, alive only while a run is in progress. Nothing ticks when idle.
- **`Scripts/Bench/RunFixtureBench.ps1`** launches a Standalone game window with the command, waits for the result, closes the game and prints the summary, so anyone can repeat the measurement.
- Measurements are taken with no other editor open. Any optimisation follows Rules §22: measure the baseline, change one thing, measure again.

### 3.1 What the measurements found, and the fixes

The baseline, measured before any change on the "R38 Cues" stage (Development, D3D12, 1600×900):

| Fixtures | Phase | Frame ms (mean / p95) | GPU ms |
|---|---|---|---|
| 50 | crossfade | 15.6 / 16.6 | 12.8 |
| 200 | crossfade | 26.0 / 29.2 | 24.0 |
| 500 | crossfade | 56.2 / 63.3 | 53.5 |

Unreal Insights traces and show-flag experiments located the costs. Each was fixed at its cause:

| Cost | Evidence | Fix |
|---|---|---|
| **Beam-shaping light function on every fixture.** Every fixture carried it, including an open beam, which projects nothing. A light function is an extra lighting pass per light. | At 500 crossfading: GPU 42.8 ms with it, 14.2 ms with `ShowFlag.LightFunctions 0` | `ApplyBeamShaping` attaches the material only while a gobo, a closed iris or a blade shapes the beam, and detaches it when the beam is open again |
| **Moved lights take the engine's per-light transform path.** `FScene::UpdateLightTransform_RenderThread` takes about 87 µs per moving light per frame, while a rebuilt light goes through the batched remove/add path at about 11 µs. | Render-thread trace: 3.97 s in 45,501 calls at 500 panning fixtures. A zoom fade, which rebuilds every light, was twice as fast as a pan-only fade. | After moving the pivots, the beam light's render state is marked dirty, so the move is carried by the batched rebuild. Engine-cost workaround, documented at the call. |
| **Navigation updates.** The colliding head and yoke meshes updated navigation data on every pan/tilt step, and StageCraft has no navigation. | `Component UpdateNavData`: 110,400 calls, about a fifth of a moving head's game-thread cost | `SetCanEverAffectNavigation(false)` on the fixture's moving meshes, as the span geometry already does |
| **Double subtree update.** Pan and tilt each propagated through the head, lens and beam. | `MoveComponent` 2 per fixture per frame | Both pivots' rotations are set, then one `UpdateComponentToWorld` from the pan pivot |
| **Full re-apply.** Every attribute change re-applied every output (8 material parameters, cone, colour, temperature, pivots), and a fade lerped every recorded attribute. | Show tick 13.5 ms at 500 | `EFixtureOutputs` drives only the outputs the changed attributes affect, and fades carry only the attributes whose start and target differ |

**Result on the same stage and settings as the baseline** ("R38 Cues", Development editor `-game`, D3D12, RTX 3060 Ti, 1600×900, zoom fading, no other editor open):

| Fixtures | Phase | Frame ms, baseline → now (mean / p95) | GPU ms | Show tick ms | Dropped |
|---|---|---|---|---|---|
| 50 | crossfade | 15.6 / 16.6 → **11.1 / 12.5** | 12.8 → 9.7 | 1.56 → 0.77 | 13 → 1 of 901 |
| 200 | crossfade | 26.0 / 29.2 → **14.2 / 15.8** | 24.0 → 10.7 | 6.2 → 3.5 | 370 → 19 of 704 |
| 500 | idle | 33.7 / 55.0 → **12.7 / 14.6** | 31.3 → 10.4 | — | 3 → 1 |
| 500 | crossfade | 56.2 / 63.3 → **29.0 / 31.2** | 53.5 → 14.2 | 13.5 → 7.7 | all frames, before and after |

**Packaged Development build** (no editor overhead):

| Fixtures | Phase | Frame ms (mean / p95) | Dropped |
|---|---|---|---|
| 50 | idle / crossfade | 5.9 / 6.8, then 6.7 / 8.0 | 0 |
| 200 | idle / crossfade | 6.9 / 8.0, then 14.8 / 16.8 | 35 of 674 |
| 500 | idle / crossfade | 9.9 / 11.0, then 32.7 / 35.4 | render-thread bound |

That run also had two single long frames: a 651 ms frame in the 200 crossfade, which is probably garbage collection after hundreds of actors were destroyed and spawned, and a 57 ms frame while the three exports ran.

Zoom fixed (`Zoom=0`) gives the same 500-fixture time (27 ms on the default stage), because moves now take the batched rebuild path too.

**Remaining limits**, recorded as follow-ups and not hidden:

- **500 fixtures crossfading at once is render-thread bound, at 27–33 ms a frame (30–37 fps).** Every light is rebuilt every frame: the zoom always forces a rebuild, and moves now take the cheaper batched rebuild. Further gains need fewer moving lights per frame, for example DMX-rate updates for distant fixtures, or engine changes.
- **Parameter-change broadcasts.** Each fade frame broadcasts about 13 `OnParameterChanged` events per fixture to dynamic delegates, about 2 ms at 200 fixtures. Batching them changes the notification contract shared by the inspector, the Patch panel and the session, so it is a separate change.
- **The startup model re-import** runs Interchange on the game thread for tens of seconds (audit R2). The benchmark waits for it, but a user would feel it.
- **Virtual shadow maps** warn "One Pass Projection max lights overflow" from about 50 overlapping spot lights. This is a possible shadow artifact, not a cost.

## 4. Shipping

- Full `BuildCookRun` Shipping package: cook, stage and pak. Pass criteria:
  - zero warnings in the cook;
  - the Sky Creator folders are present in the pak;
  - the packaged game starts, and the report exports work in it, because the PDF code is in the runtime module.
- The benchmark and report console commands are absent from Shipping; this is checked with `strings` on the Shipping executable.

## 5. Consequences

- Three PDF reports (patch sheet, bill of materials, cable schedule) share one writer. Two of them share the composer.
- The power plan is a proposal. Crews may re-balance on site, and the report says so. Persistent circuit assignment (option A) is a separate milestone with a scene format bump.
- Power cable lengths are not estimated until the distro position is part of the stage.
- `UShowControlSubsystem` carries development-only tick timing (zero in Shipping).
- A fixture with an open beam has no light function. Opening or closing a gobo, iris or blade attaches or detaches it, which rebuilds that one light.
