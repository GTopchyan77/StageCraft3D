# ADR 0007: Professional lighting, DMX patch sheet, multi-point span paths, inspector column grid, import/render hardening

- **Status:** accepted, implemented. See STATE.md #30.
- **Related:** ADR 0006 (inspector theming, runtime import), ADR 0005 (spans, outliner, BOM), ADR 0004 (render subsystem), STATE.md #9 / #16 (show control, parameter model, fader bank). Rules.md "Where state lives", "Safe data binding", Engineering Standard §8, §16, §21, §36, §37, §55, §62.

## 1. Problem

Gevor asked for a "professional upgrade" in five parts. Several parts already exist (#26 render, #27 spans, #28 import and inspector theming, `085cc16` label column), so this ADR covers only the gaps:

1. **Inspector.** Labels and fields must sit on one strict two-column grid: every label the same width, every field starting at the same X, in every row type.
2. **Spans as drawn paths.** Today a span is a straight two-click run. Gevor chose a **multi-point path** (decision 2026-10-13): any number of clicked points, one actor per path, corner parts at the bends, whole segments per leg, closed loops, and catenary sag for cables.
3. **Professional fixtures.** Gobo wheel, colour temperature (Kelvin), iris, framing shutters (four blades + rotation), a working strobe, and live photometrics (intensity, throw, illuminance, beam diameter). Gobos, iris and blades are **projected** (decision 2026-10-13).
4. **A DMX patch sheet** (grandMA3 style), mapped to the inspector and faders by events only.
5. **Import / render hardening.** A native file dialog in Shipping; Datasmith stays unsupported with a clear message (decision 2026-10-13); render helpers are hidden automatically; the render banding follow-up from #26.

## 2. Verified engine facts (UE 5.8 source)

| # | Fact | Source | Consequence |
|---|---|---|---|
| F1 | For a spot light, a light-function material's TexCoord0 runs 0→1 across the outer cone at any distance (`LightVector.xy / (LightVector.z * tan) * .5 + .5`). | `Shaders/Private/LightFunctionCommon.ush` 37-58 | One light-function material draws the gobo, iris and blades in cone-relative coordinates. Zooming scales the projection with the cone, as a real fixture does. Parameters change only on edits (a dynamic material instance), so there is no per-frame cost. |
| F2 | `SCS_FinalToneCurveHDR` selects the **HDR** output device (`EDisplayOutputFormat::HDR_LinearWithToneCurve`), not the SDR curve the viewport uses. | `PostProcessTonemap.cpp` `GetTonemapperOutputDeviceParameters` | An HDR capture quantised to 8-bit sRGB on the CPU came out far brighter than the viewport (A/B, STATE.md #30). Rejected: the capture stays `SCS_FinalColorLDR`. |
| F3 | Project plugins are enabled unless `EnabledByDefault` is false or the `.uproject` disables them. | UBT plugin rules | The 19 plugins added in `b8448a5` were compiled into the project, and `PlacementGizmo` broke UHT (duplicate `EGizmoMode`). Gevor chose to disable all 19 in the `.uproject` (2026-10-13). UnrealNGGMCP was disabled too: `b8448a5` left it half-reverted (duplicate symbols at link). |
| F4 | `UPanelWidget::InsertChildAt` only reorders UMG's slot list (no live Slate update); `ReplaceChildAt` is `WITH_EDITOR` only. A template slot of the same class copies its settings. | `PanelWidget.h` 66-95, `PanelWidget.cpp` 132-198 | Blueprint layouts are restructured (size box around the label, scroll box around the fader strip) in `NativeOnInitialized`, before the widget builds its Slate tree (`StageWidgetTree::WrapInPlace`). |
| F5 | MegaLights is off for this project (`r.MegaLights.EnableForProject` default 0), so spot lights render deferred with full light-function support. | `MegaLights.cpp` 26-31 | The beam-shaping light function needs no light-function atlas. |

## 3. Inspector column grid

- **Root causes** (screenshots, STATE.md #30): a label longer than the column ("Mode: Standard 12ch") pushed its value right; read-only values started at the field outline while field text starts 7 px inside it; long read-only values ran into the units column.
- **Contract.** The grid metrics sit next to `FieldHeight` in `StageCraftWidgetStyle`: `InspectorLabelWidth` (112, Gevor's value from `085cc16`), `InspectorUnitsWidth` (22), `FieldTextInset` (6) and `FieldOutlineWidth` (1). `UStageParameterRowWidget::ApplyColumnGrid` (in `NativeOnInitialized`) puts the label and units in fixed-width size boxes (the Blueprint's own, or one wrapped around them, F4), so the grid does not depend on a Blueprint author keeping widths in sync (Rules.md "C++ correctness must never depend on an undocumented Blueprint setup step").
- **Overflow.** Long labels and read-only values are cut with an ellipsis and carry the full text as a tooltip.
- **Read-only values** are inset by `FieldTextInset + FieldOutlineWidth`, so their text starts where field text starts.
- **Patch rows:** the "Mode: …" label hack became two rows, `DMX Mode` (text) and `Channels`.

## 4. Multi-point span paths

### 4.1 State (one source of truth)
`AStageSpanActor` stores the path as SaveGame values: `PathPoints` (actor-local, `[0]` is the origin), `bClosedPath` and `SagDepth`. Undo snapshots and scene files carry them through tagged property serialisation, so the scene format does not change. The legacy `SpanLength` is read only to migrate scene files written before this change into a one-leg path.

### 4.2 Pure math (`StageSpanMath`, unit-tested)
- `NormalisePath(Rules, Points, bClosed)`: leg by leg, whole-segment items shorten or lengthen each leg to a whole number of segments and move every later point by the same delta, so the shape is kept. Stretch items keep the drawn points.
  - Limits: at most `MaxPathPoints` (64) points, and the total segment count is capped by the item's `MaxSegments`.
  - Untrusted input: non-finite points are rejected; a closed path needs three or more points, and its closing leg must itself be whole (or the item stretches).
- `ComputePathLayout`: per-leg `FStageSpanLayout`, total length and segment count.
- Instances: segment parts per leg (leg yaw), joint parts at every segment joint, and corner parts (or joint parts when the item has none) at interior vertices, turned to the bisector. A closed path's start vertex is not doubled.
- **Sag** (Stretch items with `MaxSagPercent > 0`): each leg follows a parabola, depth `SagDepth` at mid-span. Each segment is a straight chord between consecutive points on the curve (rotated and stretched to the chord), so neighbouring segments meet exactly at their joints.

### 4.3 Tool (`UStageSpanToolComponent`)
- `PickStart` → `PickEnd` ("next point"). Each click adds a normalised point; the ghost shows the committed legs plus the leg under the cursor.
- **Finish:** Enter, a click on the last point (double-click), or a click on the first point, which closes the loop when three or more points exist.
- **Edit while drawing:** Backspace removes the last point. Esc drops the whole path, then leaves Place mode.
- **A loop that cannot close** (its closing side is not a whole number of segments): a click on the start point does nothing, the ghost turns red and the status bar says why. It never adds a stray point next to the start.
- The whole path is spawned through `USpawnSystemComponent::SpawnConfiguredItem` with its total quantity, so the GameMode validates it once, at full size.

### 4.4 Parameters
- `Length`: editable for a one-leg path (as before); read-only total for multi-leg paths.
- `Legs` (read-only).
- `Sag`: editable when the item supports it; an undoable parameter change.
- Quantity is the total segment count, so session totals, the outliner and the bill of materials need no change.

### 4.5 Rejected
- **Bezier splines with tangent handles:** needs point/handle gizmo editing. Not chosen.
- **One actor per leg:** breaks "one path = one item" for selection, lock, BOM placements and undo.

## 5. Professional fixtures

### 5.1 Attributes (floats in physical units: recordable in cues, addressable over DMX)
| Attribute | Range | Notes |
|---|---|---|
| `ColorTemp` | fixture `ColorTempMinK..MaxK` | CTC for colour-mixing fixtures; applied as the light's temperature (6500 K = neutral). |
| `Gobo` | 0..N (0 = open) | **Stepped**: stored values are rounded. DMX maps equal slot ranges; cue fades snap per slot, as on a console. |
| `GoboRotation` | -180..180 ° | Static index angle. |
| `Iris` | 0..1 (0 = open) | Closes to `IrisMinFraction` of the beam. |
| `BladeA..D` | 0..1 insertion | Framing shutters, top/right/bottom/left in the fixture's frame. |
| `BladeRotation` | -45..45 ° | Rotates the shutter assembly. |
| `Shutter` | 0..1 | 0 = open; above 0 = strobe at `StrobeMinHz..StrobeMaxHz`. Now animated (§5.3). |

The data asset declares capabilities (`GoboWheel`, `bHasIris`, `bHasFramingShutters`, CTC range). `GetSupportedAttributes`, ranges, defaults and the default DMX layout derive from them.

### 5.2 Projection
- `M_StageBeamLightFunction` (light-function domain) draws the gobo texture × iris disc × four blade half-planes in rotated cone coordinates (F1).
- Each fixture creates one dynamic material instance on first need, and only when the fixture has a gobo wheel, iris or blades. Plain pars and washes keep no light function (no cost).
- Starter gobo wheel: eight generated greyscale textures in `/Game/StageCraft/Lighting/Gobos/`, made headless by a content script.

### 5.3 Strobe clock
`UShowControlSubsystem` owns one strobe clock. A fixture with `Shutter > 0` registers. The subsystem ticks only while one or more fixtures strobe (`IsTickable`) and gates each fixture's output with a per-fixture phase. No fixture ticks.

### 5.4 Photometrics (`StagePhotometry`, pure, unit-tested)
- Effective beam angle θ = zoom × iris factor.
- Peak intensity I [cd] ≈ Φ·T / Ω, with Ω = 2π(1 − cos θ/2): a uniform-cone approximation, stated as approximate in the UI tooltip.
- T is the transmission: dimmer × colour-filter luminance (Rec. 709 luminance of the mixed colour) × gobo slot transmission × open blade area.
- Throw d [m]: a single line trace along the beam axis, up to `BeamRange`, run when pan/tilt/transform changes and read lazily. Never per frame while idle.
- Illuminance E = I / d² [lx]; beam diameter = 2·d·tan(θ/2).
- Read-only inspector section "Photometrics". Its rows refresh from the same `OnParameterChanged` events as the attributes that drive them.

## 6. DMX patch sheet

- `UStagePatchPanel` (code-built dock panel, `StageCraft.Panel.Patch`).
  - **Fixture table:** FID, name, type, mode, patch `U.AAA`, footprint, and a conflict marker.
  - **Channel map** for the primary selection: channel, attribute, 8/16-bit, live DMX value.
- **Push only:**
  - `UShowControlSubsystem::OnFixturesChanged` triggers a rebuild on the next tick.
  - Each listed fixture's `OnParameterChanged` restyles its row, or updates the channel values. Bindings are owned by the panel and removed on rebuild and in `NativeDestruct`.
- **Writes** (FID, universe, address) go through `AModularPlayerController::RequestParameterChange`, the existing single write path. Each value is then read back.
- **Faders:** descriptors may carry a short name for narrow strips (`Abbreviated`, e.g. "CTC", "Blade T"); the full name is the tooltip. The bar scrolls sideways whatever container its Blueprint gives (F4). New ranged attributes appear automatically (the fader bank is built from parameter sections). Blades get their own feature group `Shapers`, which is added to the bar's groups. Gobo is an Enum row in the inspector, stored as a stepped attribute.

## 7. Import and render hardening

- **Native dialog in Shipping (Windows):** `StageFileDialog` falls back to `IFileOpenDialog` (COM) when DesktopPlatform is not linked, so the IMPORT button exists in Shipping.
- **Datasmith:** `.udatasmith` / `.datasmith` drops get a specific refusal ("export as glTF/GLB or FBX") instead of the generic list.
- **Render helpers:** editor-only actors (gizmo, placement and span ghosts, beam helpers) register with `UStageRenderSubsystem` on BeginPlay and are hidden from every capture. This replaces the controller's hand-kept list, so a new helper cannot be forgotten.
- **Banding (investigated, not changed):**
  - The A/B rejected the HDR capture (F2).
  - The banding is volumetric-fog depth slicing: it shows in Off and FXAA renders and is gone in every Temporal and Supersampled render.
  - 16 warm-up frames without temporal AA did not remove it, so that change was reverted.
  - The default anti-aliasing is Temporal, so default renders are clean. A future option is to force temporal AA (or a higher fog Z resolution) for renders taken while haze is on.

## 8. Consequences

- The span tool UX changes: two clicks no longer place a run. Clicks add points; Enter or a double-click finishes. A single straight run is click, click, Enter (or double-click the end).
- Scene files from #26–#29 load unchanged (`SpanLength` migration). Files written now are not readable by older builds' spans (they would see one segment).
- Each fixture with optics carries one material instance and a light function; the cost is measured in STATE.md #30.

## 9. Rollback

Each part is separable. Paths: restore the `SpanLength`-only actor and the two-click tool (scene files with `PathPoints` would then load as one segment). Fixtures: data assets without gobo wheel / iris / blades behave exactly as before. Patch panel: remove the panel definition. Dialog fallback: compile out the COM path.
