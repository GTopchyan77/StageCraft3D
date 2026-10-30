# ADR 0020: Professional fixture suite

- **Status:** accepted. See STATE.md #49.
- **Related:**
  - ADR 0007 (lighting, patch paths): §5 beam shaping and photometrics are extended here.
  - ADR 0011 (cues): cues record the attributes map; cells use it unchanged.
  - ADR 0012 (fixture benchmark): output-path costs, which the new outputs must not undo.
  - ADR 0016 / 0018 (assistant): the new `set_fixture` intent follows the same parse → validate → request path.
  - ADR 0019 (DMX patch): footprints of the new layouts go through the patch subsystem as before.
  - Rules.md: Architecture (one write path, push not poll), §8 (no boolean state), §21 (no tick), §27 (data-driven), §55 (validate data), §57 (assets).

## 1. Problem

Gevor's request (2026-10-30): a complete professional fixture suite.

1. **Types:** moving head spot, wash, beam and hybrid; LED pars; strobes; blinders; pixel bars; profiles with framing shutters.
2. **Optics:** zoom, static and rotating gobo wheels, prisms, frost, barndoors.
3. **Colour:** true RGBW mixing, CMY subtractive mixing, CTO correction, and Lee/Rosco gel libraries.
4. **Physical data:** dimensions, weight, photometrics (lumens, candela, throw).
5. **Control:** UI to fine-tune beams interactively or through the AI assistant.
6. **Standards:** `TObjectPtr`, no tick, tests, 0 warnings, STATE.md #49.

**What existed:**
- Spot, beam, wash, LED par, strobe and laser kinds.
- RGB, RGBW and CMY colour systems.
- CTC, one gobo wheel with an index angle, iris and framing shutters.
- Zoom and cone photometrics.
- An attributes map (`TMap<FGameplayTag, float>`) that cues, scenes, DMX and the inspector all carry.

**What was missing or wrong:**
- The bodies were 1 m engine spheres.
- The inner cone was 0.7 × the outer, with no beam versus field angle.
- RGBW white added pure (1, 1, 1).
- CMY ignored the lamp's colour.
- None of the rest of the request existed.

**Gevor's choices, asked before design:**

| Question | Choice |
|---|---|
| Pixel bars | Per-cell colour, recorded in cues and on DMX |
| Size | Catalog dimensions per model; per-placement editing is beam and accessories |
| Assistant | Fixture commands in this entry; the bigger upgrade (knowledge base, multi-step, memory) is #50 |

## 2. Decisions

### 2.1 Every capability is an attribute
- New attributes:
  - `ColorA` (amber), `CTO`, `ColorWheel`, `Gobo2` (static wheel), `GoboSpin` (rpm).
  - `Prism` and `PrismRotation` (rpm), `Frost`.
  - The cell attributes, below.
- `ULightingFixtureData` derives which attributes a model has from its capabilities. Cues, fades, scenes, DMX, the inspector and the assistant then work without fixture-specific code.
- Stepped attributes (gobo, static gobo, colour wheel, prism) are rounded, snap in fades and take an equal DMX range per slot (ADR 0007).
- **Mutually exclusive bools became an enum (Rules §8):** `EStageBeamShaper {None, FramingShutters, Barndoors}` replaces `bHasFramingShutters`.
  - `PostLoad` migrates old assets through `bHasFramingShutters_DEPRECATED`. UHT registers that property under its old name, so saved values load into it.
- **New kinds are appended** (Hybrid, Profile, Fresnel, Blinder, Pixel Bar), so saved enum names stay valid.

### 2.2 Cells are registered gameplay tags
- A pixel bar's cells are attributes like any other: `StageCraft.Attribute.Cell.NN.{Dimmer, ColorR, ColorG, ColorB, ColorW, ColorA, ColorRGB}`.
  - There are 32 cells × 7 tags, defined natively by a table macro in `StageFixtureCells.cpp`.
  - `ColorRGB` is a composite inspector row, never stored.
- **Rejected: a separate cell array on the actor and in cues.** It would have needed:
  - a cue format version and a scene format version;
  - a fade path, a DMX path and an inspector path of its own.

  The tag form needs none of that. A 32-cell RGBWA bar with cell dimmers has 192 cell attributes, under the scene's limit of 256 per fixture.
- **Cell colour replaces master colour, as on real pixel personalities.**
  - The master dimmer scales every cell.
  - The DMX layout appends one block per cell after the fixture's own channels.

### 2.3 Colour science (`StageColorScience`, pure)
- **Additive:** each emitter at full is its colour × relative flux (`ULightingFixtureData::Emitters`).
  - The defaults are Rec. 709 primaries, a white emitter at `WhiteEmitterKelvin` and amber at about 590 nm.
  - The mix's colour is the sum. Its luminous fraction is its luminance ÷ that of every emitter at full.
  - So full RGB on an RGBW fixture is about half its rated output, as on the real fixture.
- **Subtractive:** the lamp's white (black body at `ColorTemperatureK`), warmed by CTO, is multiplied by the filters.
  - CTO is a mired shift: `FullCTOMiredShift` = +159 takes 6500 K to about 3200 K, like a full CTO gel.
  - CMY, the colour wheel and gel frames are filters.
- **Matching (gel picker, colour picker, assistant):**
  - `MatchAdditive` finds emitter levels that reproduce a target colour as brightly as possible.
  - The extra emitters' amounts (white, amber) minimise the highest level. That is a maximum of affine functions, so it is convex and solved by ternary search: jointly for two extras, one at a time for one.
  - White is therefore every emitter at full.
  - A greedy "replace RGB with W" was tried first and rejected: its white had R at 0 and about half the output (caught by the tests).
- **Light intensity:**
  - The spot light's intensity is rated flux × dimmer × mix fraction ÷ the light colour's luminance, with a floor of 0.05. The light colour multiplies the intensity, so dividing out its luminance makes the beam carry the mix's real flux.
  - **Visible change:** a scene saved with RGBW fixtures at W = 0 is about half as bright as before.
  - "Open white" now defaults to W = 100 %. Amber stays at 0, since it tints.
- UE's light temperature is used only for CTC on mixers. Fixed whites, CMY lamps and CTO are in the mix colour.

### 2.4 Beam and field angle, frost, prism, spin
- **Field angle:** `FieldAngleRatio` (field ÷ beam). The spot light's outer cone is the field and its inner cone is 2 × beam − field, so the falloff's midpoint sits at the beam edge.
- **Frost** multiplies both angles by up to `FrostFieldMultiplier` and softens every projected edge, including mip-biased gobos.
- **Prism:** N copies of the shaped beam, around the axis or in a row, inside a cone widened to hold them.
  - Photometrics report one copy: transmission ÷ facets.
- **The light function v2 (`M_StageBeamLightFunction`)** draws:
  - both gobo wheels;
  - iris, blades or barndoors (soft edge 0.25);
  - prism copies;
  - frost;
  - a gate at the beam edge.

  It maps cone space to beam space with `ConeToBeam`.
- **Spin without tick:** the material turns gobos and prisms with its `Time` node. When a speed changes, the actor writes the angle reached as a new phase and start time, so the projection does not jump.
- **The light function is still attached only while something is projected** (ADR 0012 §3); frost alone does not attach it.

### 2.5 Gels
- **Source:** `Scripts/Content/Data/StageGels.csv` has 118 gels (72 Lee, 46 Rosco), turned into `DT_StageGels` (`FStageGelRow`) by the content script.
  - The CSV is outside `Content/` so the editor never auto-imports it.
  - **The colours are sRGB approximations of the swatches, not measured spectra.**
- **Settings:** `UStageLightingSettings::GelTable` (Project Settings > Game > StageCraft Lighting) points at the table.
- **`FStageGelLibrary`:**
  - validates rows (digits-only number, no duplicates, no black swatch) and sorts Lee then Rosco by number;
  - finds gels by code, by brand and number ("lee 106", "R27", "roscolux 27"), by a bare number when only one brand has it, or by name or a unique part of a name;
  - refuses ambiguous queries.
- **Applying a gel:**
  - A mixer matches the gel over its emitters.
  - CMY and gel frames take the gel's transmission.
  - The label (`AppliedGel`, SaveGame) is cleared by any later colour edit.
- **Gel frames:** white-only fixtures with `bAcceptsGel` (conventional profiles, fresnels) keep the gel in R/G/B attributes without DMX channels.
  - Conventional library fixtures are dimmer-only on DMX.

### 2.6 Bodies and catalog dimensions
- **Data:** `FStageEquipmentSpecs::DimensionsCm` for every item, plus `FStageFixtureBody` (base, yoke, head, lens, cell strip).
- **The base is the actor's root**, so scaling it would scale the whole fixture.
  - Library base meshes are built at size by the content script: engine cube, build scale, box collision.
  - The yoke, head, lens and cell strip are scaled at runtime from their mesh bounds.
- **Lens and cell glow:**
  - Unlit `M_StageFixtureEmitter`, one MID per fixture.
  - Single-source fixtures show the beam mix as a colour.
  - Multi-cell fixtures use a `CellCount × 1` texture, uploaded only when a cell colour changes.
  - The dimmer and strobe gate are scalars, so fades and flashes never re-upload.
  - A multi-cell fixture casts one light (the luminance-weighted mix of its cells), which keeps 500 fixtures affordable.

### 2.7 Inspector
- **Rows:** colour (solved over all emitters, read back as the mix), white, amber, CTC, CTO, colour wheel, gel picker.
- **Cells:** All Cells and per-cell rows.
- **Beam:** zoom, frost, iris, gobos, index, spin, prism and its spin.
- **Shapers:** blades or barndoors, labelled accordingly.
- **Fixture (read-only):** type, size, rated output.
- **Photometrics:** gains the field angle.
- Every write still goes through `RequestParameterChange` and the GameMode. Attribute edits are show state (cues record them) and are not undo steps, as before.

### 2.8 Assistant `set_fixture`
- **Controls:** dimmer, colour by name, gel, zoom (degrees or narrow/wide), frost, iris, CTO, prism, prism spin, gobo (slot or name), gobo spin, strobe (Hz), pan and tilt. Several are allowed in one prompt.
- **Targets:** as for edits (the selection, "them", or named fixtures with an optional count).
- **Language:** offline grammar in English and in Armenian through the lexicon. The Claude tool schema gains `fixture_settings`, and colours are resolved by the stage, not the model.
- **Grammar boundaries:**
  - "Remove" and "change ... to <item>" still delete and replace.
  - A colour word among unrelated words is not a request ("purple elephant").
  - Other edit verbs (add, move, store) keep their prompts.
- **Execution:** the runner sends one `RequestParameterChange` per fixture and attribute inside one edit transaction, and reports what was set and what the targets lack.

## 3. Consequences

**Good:**
- One attribute model covers every new control and cells, with no format change.
- The colour model is physical enough that RGBW, CMY + CTO and gels look and photometer like the real fixtures.
- Everything is data-driven and validated (`IsDataValid`, asset tests).
- There is still no tick, and nothing is uploaded per frame unless a colour fades.

**Costs and limits:**
- Bodies are simplified: a box base, a box yoke crossbar, and a cylinder or box head.
- Gel colours are approximations.
- A multi-cell fixture casts one light.
- A prism copy's intensity is an even share.
- Negative pan or tilt values cannot be spoken to the assistant ("-" is dropped).
- The content script makes a transient `StaticMeshEditorSubsystem` when headless; UE deprecates this, but it is the only headless path.

**Measured:** see STATE.md #49 (tests, builds, the fixture bench and a visual run).

## 4. Rollback
Revert the commit, then:
- Assets saved after #49 keep `BeamShaper`. Older code ignores it (framing shutters off).
- The `DA_Fixture_*` assets, `DT_StageGels`, `M_StageFixtureEmitter` and the base meshes can be deleted.
- `M_StageBeamLightFunction` v1 is rebuilt by restoring the HLSL in `create_lighting_content.py` from git.
