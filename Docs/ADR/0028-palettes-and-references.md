# ADR 0028: Palettes and palette references

- **Status:** accepted. Implemented with STATE.md #60 (Phase F2).
- **Related:**
  - ADR 0025 (programmer and output mix), ADR 0011 (cue list), ADR 0020 (fixture suite: `MatchColor`), ADR 0003 (undo).
  - tasks/todo.md Phase 34 (gap G4).
  - Rules.md: Architecture (one writer, request bridge, push not poll), Engineering Standard §36 (one source of truth), §55 (trust boundaries).
- **Numbering:** ADR 0026 and 0027 stay reserved for F3 (cue timing, tracking, playbacks) and F4 (effects). Because F2 changes the scene format, F2 takes **v7**, and F3 takes **v8** instead of the planned v7.

## 1. Problem

Every console has palettes (grandMA3 presets, MagicQ palettes, Titan palettes, Daslight scenes and colours): named looks for one feature, such as colour, position or beam, that apply to any selection in one click. Cues refer to the palette instead of copying it, so changing a palette changes every cue that uses it.

StageCraft had none (gap G4). The same red had to be set by hand on every fixture, and changing it meant re-recording every cue that used it.

## 2. Decision: what a palette is

`FStagePalette {Id, Name, Kind, Color/bHasColor, GlobalValues, FixtureValues}` lives in `UStageShowLibrarySubsystem` next to the groups, and is saved in scenes.

- **Kind** is Intensity, Position, Color, Beam, Focus or All. Each attribute belongs to one kind (`StagePaletteRules::GetAttributeKind`):
  - Intensity: the dimmer and the cell dimmers.
  - Position: pan and tilt.
  - Color: emitters and filters, colour temperature, CTO, the colour wheel and the cell colours.
  - Focus: zoom and frost.
  - Beam: the rest (shutter, gobos, prisms, iris, blades).
  - A palette only holds attributes of its kind. All holds any attribute.
- **Per-fixture values** (`FixtureValues`, by fixture ID) are exact. They are recorded with RECORD PER FIXTURE, as on a console where Shift stores selective values.
- **Global values** apply to any fixture that has the attribute:
  - For Position, Beam, Focus and Intensity they are the primary fixture's values (`GlobalValues`).
  - For **Color** they are one colour (`Color`). Each fixture mixes that colour with its own system through `ALightingFixtureActor::MatchColor`:
    - RGB, RGBW, RGBA, CMY and cells mix it;
    - a wheel-only fixture takes its **nearest colour wheel slot**, compared at full brightness, as a console's colour picker does. This was added in #60 after R60a showed "Red" had nothing for the Concert stage's wheel movers. It also gives the highlight Open on wheel fixtures, and the assistant's colours reach them.
  - Colour attribute values are not shared between fixtures, because the same tag means different things on different fixtures. On a CMY fixture, ColorR is a filter's transmission, not an emitter level.
  - So "Red" works on every colour fixture, which is the point of a global palette.

**Resolution** for one fixture and one attribute (`StagePaletteRules::FindPaletteValue`):
1. The fixture's own value in the palette.
2. Otherwise the global value for that attribute.
3. Otherwise, for a colour palette, that attribute in the fixture's `MatchColor` of the palette colour.
4. Otherwise nothing; the caller falls back (§3).

## 3. Decision: references sit beside values

`FShowCueFixtureState` (one fixture in a cue or in the programmer) gains `PaletteRefs: TMap<attribute, palette id>`. Every referenced attribute also keeps its plain value in `Attributes`, written when the reference was made.

- The **palette is the one source of truth** for a referenced attribute. Readers resolve the reference first; the stored value is only the fallback when the palette is gone or no longer has the attribute.
- Everything that already reads `Attributes` keeps working unchanged: fades, the session, DMX, external input and older code paths. A reference is an addition, not a new kind of value.
- **Invariant:** a reference without a plain value for the same attribute is dropped (`StagePaletteRules::SanitizeReferences`). Every entry point applies it: the programmer, the cue list rules, scene load and undo.
- **The programmer** keeps the references with its values. A plain edit of an attribute (inspector, fader, assistant) replaces its reference. Applying a palette sets values and references together. Clear removes both.
- **The mix** (`UShowControlSubsystem::ResolveFixture`) resolves the programmer's references before mixing, so a held palette shows the palette's current value.
- **Cues:**
  - **GoToCue** and **RestoreActiveCue** resolve a cue's references into the playback layer. The fade therefore heads for the palette's current value, and the per-frame fade path is unchanged.
  - **A cue store** keeps references:
    - an attribute the programmer holds keeps the programmer's reference;
    - an attribute the programmer does not hold keeps the playing cue's reference, unless a fade is running, because the output is then between two values.
    - A highlighted fixture records its look under the highlight, as before.
- **A palette changes** (re-record, rename, undo, load): the show control hears `OnPalettesChanged` and does three things:
  1. snaps a running fade to its end;
  2. re-resolves the playing cue's references into the playback layer;
  3. re-mixes every fixture. Only changed values are written.

  Palette edits are rare user actions, so mixing everything once is cheaper than tracking which palette each fixture uses.

**Deleting a palette that is in use** first turns every reference to it into its current value, in every cue and in the programmer, then removes the palette. Nothing visible changes. The panel's delete tooltip names the cues affected. Palette, cues and programmer change as **one undo step** (a batch of `FStagePalettesCommand`, `FStageCueListCommand` and `FStageProgrammerCommand`).

## 4. Requests

All palette edits go through `AModularPlayerController`:

| Request | What it does | Undo |
|---|---|---|
| `RequestApplyPalette(id)` | For each selected fixture, the GameMode judges each attribute (`EvaluateParameterChange`, as any programmer edit). The resolved values and references go into the programmer. A fixture the palette has nothing for is skipped. | Programmer step (merged inside a transaction) |
| `RequestRecordPalette(kind, name, scope)` | `EStagePaletteScope::Global` or `PerFixture`. Records what the selected fixtures **show**, without the highlight (`GetRecordableValues`), in that kind's attributes. Global colour records the primary fixture's displayed colour. | Palette step |
| `RequestUpdatePalette(id, scope)` | Re-records from the selection, keeping name and kind. Every cue that refers to it changes with it. | Palette step |
| `RequestRenamePalette(id, name)` | — | Palette step |
| `RequestDeletePalette(id)` | Turns references into values (§3), then removes the palette. | One batch step |

Palettes, like groups, hold no money, entitlement or placement, so the GameMode is asked only about the fixture writes, as for any programmer edit.

## 5. Starter palettes

`StagePaletteRules::MakeStarterPalettes` holds ten palettes:
- **Colour** (global colour): White, Red, Green, Blue, Amber, Magenta, Cyan, UV.
- **Position:** Home (pan 0, tilt 0).
- **Beam:** Open (shutter, both gobo wheels, prism and iris at 0). Frost belongs to Focus, so it is not in Open.

Their ids are fixed, so they are the same palettes in every stage and test. They are scene data, not assets:
- a fresh stage starts with them (the library's `Initialize`);
- a file older than v7, including every starter stage, loads with them;
- a v7 file loads exactly the palettes it holds, so a user who deleted them keeps them deleted.

## 6. Scene format v7

`FStageSceneStore::FormatVersion` is 7.

- `"palettes"`: `[{id, name, kind, color?: [r, g, b], values: {tag: value}, fixtures: [{id, attributes}]}]`. `kind` is the enum name.
- A fixture entry in `"cues"` and `"programmer"` may carry `"palettes": {tag: id}`.
- **Hostile data** is dropped and counted in `InvalidShowEntries`. Marketplace packages already treat that count as Corrupt. This covers:
  - palettes without a valid GUID, duplicated, or past 999;
  - an unknown kind;
  - attributes of another kind;
  - non-finite values or colours;
  - references with a bad GUID or tag, or without a plain value.
- **Load order:** the palettes are set **before** the active cue is restored and the programmer is filled, because both resolve references.

## 7. UI

**The Palettes panel** (`StageCraft.Panel.Palettes`, `UStagePalettesPanel`, Window > Palettes...; `DA_Panel_Palettes` made by `Scripts/Content/create_palettes_content.py`):
- **Kind tabs:** ALL, INTENSITY, POSITION, COLOR, BEAM, FOCUS.
- **Pool:** colour swatches on colour palettes. A click applies the palette to the selection.
- **Record bar:** RECORD (global) and RECORD PER FIXTURE, for the selected tab's kind. ALL records any kind of attribute.
- **Current-palette row:** rename, UPDATE, DELETE, and "Used by cues 1, 3, 5".

It rebuilds on the next tick after `OnPalettesChanged` or `OnCueListChanged`.

**Development command** `StageCraft.Show.Palette Record <kind> [PerFixture] | Apply <n> | Update <n> | Delete <n> | List`, compiled out of Shipping.

## 8. Alternatives rejected

- **Copy on apply** (no references). This is simpler, but it is what Daslight does and what the brief asks to go beyond: re-recording a palette would change nothing already stored.
- **References only, no stored value.** A missing palette (a damaged file, a package that lost it) would leave the attribute without any value, so the cue would no longer play what it did.
- **Global colour as attribute values.** It breaks across colour systems (§2).
- **Tracking which fixtures use which palette** for the change re-mix. That is a second source of truth to keep in sync, for a rare event. A full re-mix runs only on a palette edit, never per frame. It is the same work as switching the control source back to Internal, and writes only the values that change.

## 9. Consequences and open items

- **v7 files are refused by builds before #60** (VersionMismatch).
- **A palette change during a fade** snaps the fade to its end.
- **A stored value goes stale after a palette update.** This is harmless while the palette exists, because the palette wins. It matters only if the palette is lost without the delete path (a damaged file).
- **A global colour palette holds a colour only.**
  - Colour temperature, CTO and an exact wheel slot are not stored globally; record per fixture to keep those exactly.
  - Recording a global colour while the primary fixture is highlighted takes the highlight's white, because it reads the colour row the inspector shows. The rest of a recording leaves the highlight out.
- **The panel's current-palette row** appears after a tile click or RECORD in the panel, not after a console-made palette (the same as the Groups panel).
- **Effects (F4) may refer to palettes later**; the reference map leaves room for it.
- **The assistant** gets no palette intents yet (follow-up).
