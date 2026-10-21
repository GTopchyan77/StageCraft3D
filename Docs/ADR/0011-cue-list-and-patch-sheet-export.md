# ADR 0011: Cue list panel, cues in scene files (format v3), patch sheet export (PDF + CSV)

- **Status:** accepted. See STATE.md #38.
- **Related:**
  - ADR 0003 (undo/redo), ADR 0005 §5 (bill of materials export), ADR 0007 (patch panel), ADR 0010 (scene format v2, edit rules).
  - `Docs/PROJECT_AUDIT.md`: the cue engine has no UI and is not saved; patch conflict detection is untested (R11).
  - Rules.md: "Roles of the framework classes", "Safe data binding"; Engineering Standard §7, §21, §26, §29, §55.

## 1. Problem

Gevor's next milestone (2026-10-21) asked for:

1. **A Cue panel.** `UShowControlSubsystem` has had a cue store with crossfade playback since Phase 5, but nothing calls it: there is no panel, no request path through the rules, no undo, and `Cues` is `Transient`, so a show is lost when the scene closes.
2. **Professional paperwork.** A lighting crew needs a printed patch sheet (fixture, mode, universe/address, position). Today the only export is the bill of materials as CSV, and nothing in the project writes PDF.

Scope chosen by Gevor: the full cue list (Store/Update, Go/Back, Delete, label and fade time, Internal/External), saved with scenes, undoable; and a patch sheet as PDF plus CSV.

## 2. Decisions: cues

### 2.1 One set of cue rules, applied everywhere

`StageCueRules` (pure, in `Show/ShowControlTypes.h`) defines what a valid cue is:

| Field | Rule |
|---|---|
| Number | Finite, 0.001 to 9999.999, rounded to 3 decimals (the lookup tolerance is 0.001, so two different numbers can never look equal) |
| Fade time | Finite, clamped to 0..600 s |
| Label | At most 256 characters (trimmed) |

- The subsystem applies the rules on every entry: `StoreCue`, `UpdateCue`, `SetCues` (loading) and `ReplaceCues` (undo). A list with bad numbers or duplicates is repaired (first one wins) and the repair is logged.
- The GameMode refuses a request with an invalid number before anything runs, so the user gets a message instead of a silent repair.

### 2.2 Requests go through the rules authority

- Widgets and console commands call the controller: `RequestStoreCue`, `RequestUpdateCue`, `RequestDeleteCue`, `RequestGoToCue`, `RequestCueGo`, `RequestCueBack`, `RequestSetControlSource`.
- The controller asks `AStageCraftGameModeBase::EvaluateCueRequest(Player, Request, CueNumber)` (BlueprintNativeEvent), then calls the subsystem, then records undo.
- Rules:
  - No show control in the world → refused.
  - Store / Update / Delete with an invalid number → refused. Update and Delete of a cue that does not exist → refused.
  - Go / Back / Go To while the control source is External → refused ("an external console is driving the fixtures"). Storing is allowed in External mode: it records the look the external console sent, which is how a visualiser user captures a show.
- Playback state (active cue, fades) and the control source stay in the subsystem; the panel reads them and binds to `OnCueListChanged`, `OnCueStarted`, `OnCueFinished` and `OnControlSourceChanged`. No polling.

### 2.3 New subsystem operations

- `UpdateCue(Number, Label, FadeTime)` changes the label and fade time without re-recording the fixtures.
- `Back()` plays the cue before the active one with that cue's fade time, like a console's Back key.
- `ReplaceCues(Cues)` replaces the list for undo/redo and keeps the playback state (active cue, running fade). `SetCues` stays the "load a show" entry and still resets playback.
- `StoreCue` returns false for an invalid number instead of clamping it to 0.

### 2.4 Undo: whole-list snapshots

- A cue edit is recorded as `FStageCueListCommand(Description, Before, After)`: the whole cue list before and after the edit. Undo writes Before, Redo writes After, through a new `IStageItemEditor::SetCueList`, which the live editor implements with `ReplaceCues`. It has a default implementation (Invalid), so test fakes that do not care about cues are unchanged.
- **Why snapshots, not per-operation commands:** Store can add or overwrite, Update changes two fields, and Delete removes a whole recorded look; inverting each correctly needs the old cue anyway. A full list is small (a 100-cue show with 40 fixtures and 20 attributes is about 80,000 values, under 1 MB), and the history keeps at most 100 steps.
- Playback (Go, Back, Go To) and the control source are not undoable, like on a lighting console. Clear Stage does not clear cues (cues are keyed by fixture ID and survive a re-patch).
- A cue edit closes any open inspector transaction first, so the two never merge.

### 2.5 Cues make the scene dirty and are saved (scene format v3)

- `UStageSessionSubsystem` binds `UShowControlSubsystem::OnCueListChanged` (after play has begun) and marks the stage dirty, exactly like environment edits (ADR 0010 §5). Autosave and the exit prompt therefore cover cue edits.
- **Format v3** adds a `cues` array to the scene root:

```json
"cues": [
  { "number": 1.5, "label": "Walk-in", "fade": 3.0,
    "fixtures": [ { "id": 101, "attributes": { "StageCraft.Attribute.Dimmer": 0.75 } } ] }
]
```

- **Migration:** v1 and v2 files have no cues; they load with an empty cue list. Files are written as v3. Files newer than v3 are refused (unchanged rule).
- **Reading is tolerant and bounded:** at most 2000 cues, 4096 fixtures per cue and 256 attributes per fixture; a cue with an invalid number, a fixture with a non-positive id, an attribute whose tag is unknown to this build or whose value is not finite is skipped (logged with a count). Numbers and fades go through `StageCueRules`.
- **Loading** applies the cues after the items are restored (so the fixture IDs exist) and before the stage is marked clean.

### 2.6 Cue panel

- `UStageCuePanel`, a code-built UMG dock panel like the Patch and BOM panels (no WBP): `DA_Panel_Cues` (tag `StageCraft.Panel.Cues`), created by `Scripts/Content/create_cue_content.py`. It opens in the bottom stack with the Fader Bank and Patch tabs.
- Layout: a transport row (Go, Back, active cue, Internal/External switch), an entry row (number, label, fade, Store / Update), and the cue list (number, label, fade, fixture count, Go To, Delete). The row of the active cue is highlighted. Label and fade are edited in place and committed through `RequestUpdateCue`.
- Changes rebuild on the next tick (`RequestRebuild`), never inside the callback that caused them.

## 3. Decisions: patch sheet export

### 3.1 Our own small PDF writer

- UE ships no PDF library, and the module must stay free of third-party code. `FStagePdfDocument` writes PDF 1.4 directly: the standard Helvetica and Helvetica-Bold fonts (not embedded, WinAnsi encoding), text, lines and filled rectangles, uncompressed content streams, a correct cross-reference table. About 400 lines, pure, any thread.
- Text is measured with the real Adobe font metrics, so cells are truncated with "..." rather than overflowing.
- **Limitation:** the standard fonts only cover Western European text. Characters outside WinAnsi print as "?" in the PDF; the CSV beside it keeps the full Unicode text. Embedding a font with full Unicode coverage is a follow-up if non-Latin labels matter.

### 3.2 Pure model, game-thread gathering, background writing

- `StagePatchSheet` (pure) sorts and counts the rows (patched, unpatched, conflicts, universes) and renders CSV and PDF. It is covered by `StageCraft.Report.PatchSheet` and `StageCraft.Report.Pdf`.
- `UStagePatchSheetSubsystem` (world) gathers the rows on the game thread from `UShowControlSubsystem` (fixtures, `FindPatchConflicts`), renders both documents on a worker task and writes them atomically (temp file, then move) to `Saved/StageCraft/Reports` as `PatchSheet_<Scene>_<time>.pdf` and `.csv`. One export at a time; the result is broadcast on `OnExportFinished` (status bar), like the BOM export.
- Report folder helpers (`GetOutputDirectory`, unique file names, atomic write of bytes) move to `Report/StageReportFiles`, shared by both exports.
- Entry points: File › Production › Export Patch Sheet (PDF + CSV), an Export button on the Patch panel, `AModularPlayerController::RequestExportPatchSheet`, and the dev console command `StageCraft.Report.ExportPatch`.

## 4. Alternatives considered

- **Per-operation cue commands** (StoreCueCommand, DeleteCueCommand...): more code and more ways to get an inverse wrong; rejected for snapshots (§2.4).
- **Saving cues in a separate show file:** a show belongs to the stage it lights, and one file keeps autosave, Save/Discard and recovery consistent; rejected.
- **HTML for printing** and **XLSX**: offered to Gevor; PDF + CSV chosen.
- **A third-party PDF library** (libharu, PDFium): new third-party dependency and licensing for a few tables; rejected.

## 5. Consequences

- Scene files are now version 3; an older build refuses them (VersionMismatch), as with v2.
- Show control has its first tests (`StageCraft.Show.Cues`).
- Follow-ups: tracking cues, cue timing per attribute, follow/wait times, a timeline; a light plot drawing; font embedding for non-Latin text; undo for fixture attribute and patch edits (still open from ADR 0010).
