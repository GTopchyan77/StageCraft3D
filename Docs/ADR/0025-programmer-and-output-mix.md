# ADR 0025: Programmer, output mix and fixture groups

- **Status:** accepted. Implemented with STATE.md #59 (Phase F1).
- **Related:**
  - ADR 0011 (cue list), ADR 0019 (DMX patch and input), ADR 0020 (fixture suite), ADR 0003 (undo), ADR 0013 (key bindings).
  - The Phase F plan: a gap analysis against grandMA3, MagicQ, Avolites Titan and Daslight 5 (tasks/todo.md Phase 34).
  - Rules.md: Architecture (state by lifetime, request bridge, push not poll, deny by default), Engineering Standard §8, §36 (one source of truth), §37 (explicit state), §55 (trust boundaries), §60 (idempotency).

## 1. Problem

Gevor asked (2026-11-03) for the gap analysis above and for the missing console logic. The gaps, in the order the plan fixes them:

| # | Gap | Fixed in |
|---|---|---|
| G1 | No programmer: every edit wrote straight onto the fixture; the Inspector and Fader Bank edited only the last-selected fixture; no Clear, Highlight or Fan; attribute edits could not be undone. | F1 (this ADR) |
| G2 | No mixing between sources: a cue fade overwrote a manual edit on the next frame. | F1 (this ADR) |
| G3 | No fixture groups. | F1 (this ADR) |
| G4 | No palettes. | F2 |
| G5, G6 | One fade time per cue; no tracking. | F3 (ADR 0026) |
| G7 | One playback; no faders, flash, Grand Master or Blackout; no Go, Back or Blackout keys. | F3 (keys Go and Back here) |
| G8 | No effects engine. | F4 (ADR 0027) |
| G9 | No DMX output; no per-fixture pan/tilt invert or swap. | F5 (ADR 0019 amendment) |

Undockable windows, the patch with conflicts, Art-Net/sACN input and the fixture model already met the consoles' level (ADR 0001, 0019, 0020).

## 2. Decision: fixture output is a mix of layers with one writer

`ALightingFixtureActor::Attributes` stays the fixture's **output**, so every reader is unchanged: the inspector display, `WriteDMX`, photometrics and the shadow budget. The output is mixed from these layers, lowest priority first:

1. **Home look:** `HomeAttributes`, a new SaveGame map on the fixture. A fixture placed new has its defaults. A state saved before #59 has no home look, so `ApplyItemData` takes the restored output as the home look and an old stage looks exactly as it did. A duplicate's home look is what it shows when copied.
2. **Playback:** `UShowControlSubsystem::PlaybackValues`, per fixture ID: the values the cue list has set. While a fade runs, they are its current frame.
3. **Programmer:** `UStageProgrammerSubsystem`, per fixture ID (§3).
4. **Highlight:** a transient overlay of full dimmer, open shutter and the fixture's own full white (`MatchColor(White)`).

`StageShowMix::ResolveAttribute` is pure and covered by `StageCraft.Show.OutputMix`. Each attribute takes the highest layer that holds it, as a console's LTP channels do. (HTP intensity and masters are added with the playbacks in F3.)

**`UShowControlSubsystem` is the only writer of the mix.**
- `ResolveFixtures` runs on demand: on a programmer change, a highlight change, `RestoreActiveCue`, a switch back to Internal, and when a fixture registers that the show already has data for.
- A fade frame writes only the attributes it drives that no higher layer holds (`RemoveHeldAttributes`). A fade starts from the playback layer, not the output, so a held value is never where a cue's fade begins.
- Nothing new ticks. The subsystem still ticks only while a fade, the strobe clock or DMX output is active.
- `SetAttributes` still stores and applies only what changes (ADR 0012 §3).

**External DMX** keeps today's contract: input writes the output directly. A programmer edit under External writes only what the programmer and highlight hold, so the console's other values stay. Switching back to Internal re-mixes every fixture.

**Direct `SetAttribute` writers** (the benchmark, tests, an inspector with no stage controller) still work. Their values last until the next mix of that fixture.

## 3. Decision: the programmer

`UStageProgrammerSubsystem` (world, Game and PIE) holds `TMap<FixtureId, TMap<Attribute, value>>` and the highlight set. It is state only:
- Every entry point drops non-finite values and tags outside `StageCraft.Attribute`.
- Bounds match a cue's: 4096 fixtures and 256 attributes per fixture.
- A native delegate carries the changed fixture IDs to the show control. A dynamic `OnValuesChanged` serves the session's unsaved mark and the panels.

**How an edit lands in the programmer.**
- `AModularPlayerController::RequestParameterChange` sends a fixture's look row to `RequestProgrammerChange`. A look row is a `StageCraft.Attribute.*` row, including the composite colour rows, plus the gel and all-cells rows (`StageCraftTags::IsFixtureLookParameter`).
- For each fixture, the GameMode judges the write (`EvaluateParameterChange`). The fixture's own `WriteParameter` then translates it into attribute values: a colour into its emitters, a gel into its filters.
- The values that write changed, plus the attribute itself, go into the programmer. The mix that follows gives the same output, so there is no visible double write. The gel label keeps working.
- A refused fixture is left out, and the first refusal is reported. The edit fails only when no fixture took it.

**Undo.** `FStageProgrammerCommand` holds the touched fixtures' values before and after, where a fixture without an entry had none.
- Inside an edit transaction (a fader drag, an assistant build), programmer edits merge per fixture, keeping the first "before", and become part of the one step.
- This closes PROJECT_AUDIT's "fixture attribute edits are not undoable".
- `RequestClearProgrammer` is one step.

**Cue store takes the programmer in**, as on a console.
- `RequestStoreCue` stores, makes the new cue the playing one (`RestoreActiveCue`, no fade, so the stage keeps what it shows) and clears the programmer.
- The store and the clear are one undo step (`FStageCueListCommand` and `FStageProgrammerCommand` in one batch, through the new public `UStageEditHistoryComponent::RecordAsOneStep`).
- A store records the look under the highlight, never the highlight (`GetRecordableValues`).
- Without this, a Go after storing would have shown the programmer's values over the new cue, because the programmer outranks playback.

## 4. Decision: the selection and the fan

A look edit on a selected fixture applies to **every selected lighting fixture** in selection order. The Inspector and Fader Bank titles show "(+N)".

`EStageFanMode {Off, Linear, V, InvertedV}` is a tool setting of the player, held on the controller and not saved. On a float row, the change made on the primary fixture is spread with `StageShowMix::FanFactor`:
- Linear: −1 … +1 along the selection;
- V: 1 … 0 … 1;
- Inverted V: 0 … 1 … 0.

The primary fixture is the one the edit was made on.

Patch, shape and transform rows keep their own paths and stay single-fixture (transforms keep moving the group as before).

## 5. Decision: fixture groups

`FStageFixtureGroup {Id, Name, ordered FixtureIds}` lives in the new `UStageShowLibrarySubsystem` (world). Palettes (F2) and effects (F4) will join it there.
- `StageFixtureGroupRules` repairs every entry: unique valid ids, positive fixture IDs once each, names trimmed to 64 characters, at most 999 groups.
- Requests: record (from the selection), update, rename and delete. Each is one undo step (`FStageFixtureGroupsCommand`, the whole list before and after).
- Select is not an undo step, like any selection.
- Groups have no rules beyond validation, so the GameMode is not asked. They hold no money, entitlement or placement.

## 6. Keys

Appended to `EStageInputCommand` so saved key maps keep their meaning, under a new SHOW category in Preferences:

| Command | Default key |
|---|---|
| Go | Ctrl+Enter |
| Back | Ctrl+Backspace |
| Clear Programmer | Ctrl+Shift+C |
| Highlight (toggle) | Ctrl+Shift+H |

All four work from any panel, except in a text field (`StageShortcutRouting`).

A plain key was avoided on purpose. A new default takes its chord from any user override that already uses it, and the loader drops that override. The first draft used plain H, and `StageCraft.Input.SavedBindings`, whose user map binds H, caught it.

Blackout waits for F3, when Blackout exists.

## 7. Scene format v6

`FStageSceneStore::FormatVersion` is 6. It adds:
- `"programmer"`: `[{id, attributes{tag: value}}]`, the same shape as a cue's fixtures;
- `"groups"`: `[{id, name, fixtures: [ids]}]`;
- `"activeCue"`: the cue that was playing.

The fixture's home look rides in its existing SaveGame state.

**Load order** (`UStageSceneComponent::ApplyDocument`):
1. The old show's cues, playback and programmer are cleared before the new fixtures register, because they name fixture IDs that new fixtures may reuse.
2. The new fixtures are restored.
3. The active cue is restored without a fade.
4. The programmer and the groups are restored.

v1–v5 files load with an empty programmer, no groups and no cue playing.

**Hostile data.** Every bad entry is dropped and counted in `FStageSceneParseReport::InvalidShowEntries`:
- programmer entries without a valid id, duplicated or not an object;
- groups without a valid GUID;
- fixture IDs that are not positive integers;
- an invalid active cue number.

The rest loads. Marketplace packages treat any such drop as Corrupt, as they already do for cue entries.

## 8. UI

**The Groups panel** (`StageCraft.Panel.Groups`, `UStageGroupsPanel`, Window > Groups & Programmer...; `DA_Panel_Groups` by `Scripts/Content/create_groups_content.py`):
- **Programmer bar:** RECORD, "Programmer: N fixtures", CLEAR, HIGHLIGHT and the FAN mode.
- **Pool:** one tile per group; a click selects its fixtures in group order.
- **Current group row:** rename, UPDATE and DELETE.

It rebuilds on the next tick after `OnGroupsChanged` or `OnValuesChanged`. Its own clicks also rebuild it, because highlight and fan are tool state with no delegate.

**Development commands:**
- `StageCraft.Show.Select <ids>`
- `StageCraft.Show.Programmer Clear | Highlight On|Off | Fan <mode> | List`
- `StageCraft.Show.Group Record | Select | Update | Delete | List`
- `StageCraft.Workspace.ScreenshotWindow All`, which also saves the floating panel windows

All are compiled out of Shipping.

## 9. Alternatives rejected

- **Keep writing onto the fixture and protect manual edits with per-attribute timestamps.** It hides a layer instead of modelling it. Clear, Highlight and a later Grand Master would each need their own exception.
- **The programmer inside `UShowControlSubsystem`.** That subsystem is already 700 lines and owns playback. The programmer is state with a different lifetime contract (edited by the user, saved, undone), so it is a separate subsystem that show control reads.
- **The home look as the fixture's defaults only** (no saved map). Old scenes would have lost their look on the first Clear, because they saved their look as output.
- **Fan as a separate request.** It is a mode of the same edit, so the request reads the player's fan setting. The panels need no second write path.

## 10. Consequences and open items

- **Behaviour change.** A manual edit now survives a cue fade (programmer above playback) until Clear or a store. On a console that is expected; in StageCraft it is new, and the Groups panel shows the programmer count and CLEAR.
- **Behaviour change.** Switching from External DMX back to Internal now restores the show's look instead of keeping the console's last frame.
- **Not done in F1:**
  - storing only the programmer (tracking) and Cue Only;
  - HTP intensity, masters and Blackout (F3);
  - palette references in the programmer (F2).
- **v6 files are refused by builds before #59** (VersionMismatch), as with every format bump.
- **Fan applies to float rows only.** Colour and wheel rows set the same value on every fixture.
