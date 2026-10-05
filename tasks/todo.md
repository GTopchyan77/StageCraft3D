# StageCraft 3D — Task List

See `STATE.md` for detailed history of completed work.

## Phase 3 — Player Controller & Spawning/Deletion
- [x] Implemented, compiled, headless smoke-tested (see STATE.md #3)
- [ ] Manual PIE verification (Gevor): place props by holding LMB, lights by single click, RMB deletes

## Phase 4 — Selection & Transform Gizmo
- [x] `Gizmo` trace channel (ECC_GameTraceChannel2) + `StageCraftCollision::GizmoChannel`
- [x] `USelectionComponent`: interface-based selection, OnSelect/OnDeselect highlight swap, auto-release on OnDestroyed, `OnSelectionChanged` delegate
- [x] `AModularTransformGizmo`: world-space translate arrows / rotate rings, drag via cursor ray vs. drag plane, snapping props, Space toggle, constant screen size (ticks only while attached)
- [x] Controller click arbitration (gizmo > item select > empty deselect+place; RMB item delete > empty deselect+disarm), Space binding with legacy-key consumption
- [x] Game target compiles clean (editor target blocked by Live Coding while the editor was open)
- [x] Editor target rebuilt and up to date
- [x] Clean -Rebuild of Game + Editor targets, 0 errors / 0 warnings; config + input fallback audited (STATE.md #8)
- [ ] Manual PIE verification (Gevor): see STATE.md #8 checklist; re-run it on the Phase 5 build, since `AModularBaseActor` changed

## Phase 5 — Concert Production & Lighting Suite (architecture: STATE.md #9)

### 5.1 Core C++ (implemented; Editor target compiles clean; PIE smoke-tested by Claude, STATE.md #10)
- [x] Equipment data model: `ULightingFixtureData`, `UAudioEquipmentData`, `UStageTrussData`, `FStageEquipmentSpecs`, `EStageItemType::Truss/Audio`
- [x] Placed-instance state: `ALightingFixtureActor` (FixtureId, DMX patch, attributes, pan/tilt/beam), `AAudioEquipmentActor`, `AStageTrussActor`
- [x] Parameter model: `FStageParameterValue/Descriptor/Section`, `IStageParameterInterface`, Param/Attribute/FeatureGroup native tags
- [x] `AModularBaseActor`: Info + Transform sections, `OnParameterChanged` (including gizmo moves via root `TransformUpdated`), `InstanceLabel`
- [x] Inspector C++ bases: `UStageInspectorPanel`, `UStageParameterSectionWidget`, `UStageParameterRowWidget`, `UStageCraftUITheme`; `HUDWidgetClass` on the controller
- [x] `UShowControlSubsystem`: fixture registry, auto fixture ID and auto-patch, patch conflict query, cue store/go/fade, Internal/External control source
- [x] DMX boundary: `UStageDMXBridge`, fixture `WriteDMX`/`ReadDMX` (8/16-bit, CMY), `RenderDMXUniverse`
- [x] New catalog category tags (Stage.Deck/Riser, Truss.*, Rigging.*, Lighting.*, Audio.*)
- [x] Editor target build with the editor closed (STATE.md #10)
- [x] Row widget builds missing editors at runtime in `EditorSlot` (one generic row WBP for every type; interim Color R/G/B editor)
- [ ] Rebuild the Game target after the `EditorSlot` change (only the Editor target was rebuilt)

### 5.2 Content (Part 2, STATE.md #10)
- [x] `DA_StageCraftTheme` (UStageCraftUITheme, C++ default palette)
- [x] `WBP_StageParameterRow` (generic row: GroupColorStrip, LabelText, EditorSlot, ValueText, UnitsText); replaces the planned per-type row WBPs
- [x] `WBP_StageParameterSection` (HeaderBar, GroupColorStrip, HeaderText, RowContainer) and `WBP_StageInspectorPanel` (TitleBar, TitleText, SubtitleText, EmptyState, SectionContainer ScrollBox; defaults for Theme, SectionWidgetClass, FallbackRowWidgetClass)
- [x] `WBP_StageCraftHUD` (inspector docked on the right) + `BP_StageCraftPlayerController` (HUDWidgetClass) + `BP_StageCraftGameMode`, set as the `L_StageTest` GameMode override
- [x] Test equipment in `/Game/StageCraft/Data/`: `DA_MovingHead_Test`, `DA_LineArray_Test`, `DA_Truss_Test`; Data folder added to the Asset Manager scan
- [x] `L_StageTest`: volumetric fog, night lighting, 40 m floor fix, two moving heads + line array + truss pre-placed
- [ ] HUD layout: left catalog, bottom show panel, top bar; stretch-anchor the inspector top-to-bottom in the designer
- [ ] Style the runtime editors from the theme (SpinBox/EditableTextBox styles in `UStageCraftUITheme`); widen the units column
- [ ] More test content: LED par (RGBW, no pan/tilt), subwoofer, 2 m / 3 m truss, chain hoist; real meshes instead of 1 m engine shapes

### 5.3 Features on top of the core
- [ ] Light color picker WBP (HSV wheel + RGBW faders) calling `CommitValue`, replacing the old `bSupportsColorEditing` path
- [ ] UMG catalog panel: category tabs from `StageCraft.Category.*`, icons via the UI bundle, click to `SelectItem`
- [ ] Show panel WBP: cue list, Store / Go / Back / Delete, fade time, control source switch, patch conflict warning
- [ ] Strobe clock in `UShowControlSubsystem` (one tick for all strobing fixtures)
- [ ] Beam visual: cone mesh/material driven by Zoom and Dimmer; laser fixture actor (Niagara)
- [ ] `StageCraftDMX` module: bridge over the DMX Engine plugin (`DMXProtocol`, Art-Net + sACN ports), enabled via the uproject
- [ ] Show file save/load (`USaveGame`: placed items, instance `SaveGame` properties, cue list)
- [ ] Rigging: snap fixtures to truss `RiggingPoints`, truss-to-truss `Connectors`, load per point against the safe working load
- [ ] Audio: coverage cone visualisation, line array hang builder (splay chaining)
- [ ] Multi-select + group edit in the inspector (MA-style "selection" of many fixtures)

### 5.4 Verification (Gevor)
- [x] (Claude, automated PIE) Select Spot 102: the inspector shows Info/Transform/Patch/Dimmer/Position/Color/Beam with live values; Dimmer 25 dims the beam
- [ ] PIE: open `L_StageTest`, click each pre-placed item (Spot 101/102, Main L 01, Truss DS 01); check the Audio and Rigging sections
- [ ] PIE: edit Pan/Tilt/Zoom/Color R-G-B/Label; the light and rows update live
- [ ] PIE: drag with the gizmo; the Transform rows update live
- [ ] PIE: StoreCue 1 / change values / StoreCue 2 / GoToCue 1; the fade runs over the fade time
- [ ] Output Log: `Fixture N registered (...), patch U.AAA.` for each placed fixture, with no patch overlaps

## Backlog
- [x] RMB fly navigation (mouse look, WASD/QE, wheel speed) with click-vs-drag arbitration (STATE.md #11)
- [ ] PIE verification (Gevor): STATE.md #11 checklist (look/fly/speed, RMB click-delete vs. drag, buried gizmo visible and draggable, fog)
- [ ] Orbit (Alt+LMB) / pan (MMB) / focus-on-selection (F)
- [ ] Hover highlight via IInteractableInterface::OnHoverBegin/End (needs a cheap throttled cursor trace)
- [ ] Gizmo handle hover highlight
- [ ] Place onto an existing item with a modifier (plain LMB on an item now selects it)
- [x] Gizmo drawn on top of geometry: `M_GizmoHandle` (Disable Depth Test) + `TraceHandles` picking (STATE.md #11)
- [ ] Local-space gizmo option
- [ ] Undo/redo, hooking OnItemSpawned / OnItemDeleted / gizmo drag end
- [ ] Overlay materials for hover/selected (content)

## Editor UX fixes, round 2 (STATE.md #12)
- [x] Placement dead after RMB delete/miss: RMB on empty no longer disarms; Esc = Cancel; press-position click trace; threshold 12 counts
- [x] RMB capture moved to `UStageCraftGameViewportClient` (engine-native hide/raw-delta/restore); no mid-press SetInputMode or context churn
- [x] Editor build clean; PIE startup log clean, custom viewport client active
- [ ] PIE verification (Gevor): STATE.md #12 checklist (delete then place loop, RMB look/fly, gizmo drag + RMB, alt-tab)

## Fly camera rewrite (STATE.md #13)
- [x] `AStageCameraPawn` as plain APawn: damped look (pitch clamp 89, no roll), W/S look-vector, A/D strafe, E/Q pure world Z on its own axis, damped velocity, wheel speed
- [x] RMB navigation-only; Delete key deletes the selection (replaces RMB-click delete)
- [x] Editor build clean; in-PIE flight test passes (pure Z, smooth ease, 89 clamp)
- [ ] Feel check with a physical mouse (Gevor): STATE.md #13 checklist

## Wheel fly speed (STATE.md #14)
- [x] Wheel changes fly speed with or without RMB; uniform on all axes; eased; 10 cm/s to 200 m/s; `OnFlySpeedChanged` + interim on-screen readout
- [x] Editor build clean; in-PIE speed test passes (injected wheel, per-axis speeds, mid-flight easing, clamps)
- [ ] Bind `OnFlySpeedChanged` in `WBP_StageCraftHUD` and drop the debug-message readout
- [ ] Feel check (Gevor): STATE.md #14 checklist

## Look speed follows fly speed (STATE.md #15)
- [x] `GetLookSpeedScale()`: sqrt curve vs 1200 cm/s, clamp x0.5..x3, toggle `bScaleLookWithFlySpeed`; smoothing unchanged
- [x] Editor build clean; in-PIE look test matches formula exactly at 4 speeds incl. both clamps
- [ ] Feel check (Gevor): fast flight turns faster but controllable; tune exponent/clamps if needed

## Phase 5 Part 2 UI: inspector binding, dropdowns, fader bank (STATE.md #16)
- [x] `UStageParameterViewWidget` / `UStageParameterControlWidget` bases: selection follow, live refresh, commit + read-back, focus-safe refresh, deferred rebuild
- [x] Inspector + rows on the new bases; Enum type + dropdown rows; item Type selector (swap fixture/speaker model in place)
- [x] `UStageFaderWidget` (fader / toggle / color channel), `UStageEncoderWidget` (painted dial), `UStageFaderBankWidget` (quick-access bar)
- [x] `WBP_StageFaderBank` + docked in `WBP_StageCraftHUD` bottom-left; `DA_MovingHead_Wash_Test`
- [x] Editor + Game targets build clean; in-PIE two-way binding / clamp / focus / type-swap / audio test passes
- [ ] Visual + mouse check of faders and encoders (Gevor): STATE.md #16 checklist
- [ ] DMX mode list per fixture (data + patch), exposed through the Enum row
- [ ] Optional: one-line fix of `HandleAddWidgetToBlueprint` (UnrealNGGMCP) to honour `user_widget_class`

## Pan/Tilt encoder usability (STATE.md #17)
- [x] Vertical delta drag past the drag threshold, no value jump; high-precision mouse with the cursor restored on release
- [x] Shift fine mode (`FineScale` 0.2) for drag and wheel; clamped accumulator with no dead zone
- [x] Value popup ("Pan: 45.0°", "fine"), escapes the bar's clipping; hover/active highlight; `ResizeUpDown` cursor; eased needle
- [x] Double-click `ResetToDefault()` via the new descriptor `DefaultValue` (fixture defaults)
- [x] Editor + Game builds clean; in-PIE synthetic-input test passes on Pan and Tilt
- [ ] Feel and visual check with a physical mouse (Gevor): STATE.md #17 checklist
- [ ] Optional: share drag/popup/reset with `UStageFaderWidget`

## Core architecture & shop foundation (STATE.md #18)
- [x] `UStageProfileSubsystem` (persistent profile/wallet/entitlements, SaveGame slot, integrity hash, friend-only mutation)
- [x] `UStageEconomySubsystem` (product catalog, ownership, validate → backend → re-validate → atomic commit) + `UStageCommerceBackend` / local backend
- [x] `UStageProductData`, `UStageUnlockCondition` (+ Entitlements / ProfileLevel), economy structs and result codes; item `RequiredEntitlement` / `ParameterEntitlements`
- [x] `UStageSessionSubsystem` (placed items, power/weight totals, session limits, single parameter write path, dirty flag)
- [x] GameMode rules (`EvaluatePlacement` / `EvaluateParameterChange`, `SessionRules`); controller request bridge; views commit via the controller and show locks
- [x] Dev console commands (`StageCraft.Shop.*`, `StageCraft.Profile.*`), Shipping-safe
- [x] Editor / Game / Shipping builds clean; in-PIE economy test passes (locks, purchase, forged product, funds, parameter lock, power rule, travel, persistence, tamper)
- [x] Rules.md architecture + economy security section
- [ ] Manual check (Gevor): STATE.md #18 checklist
- [ ] Shop widget + wallet readout + rejection toast (replace on-screen debug messages)
- [ ] Lock badges / Unlock buttons in the catalog
- [ ] Session-limit check on Type swap; parameter display names in lock messages
- [ ] Server-authoritative commerce backend + RPC requests when multi-user/marketplace starts

## Phase 6 — Dockable workspace, detachable viewport, preferences (design: `Docs/ADR/0001-dockable-workspace.md`, STATE.md #20)
- [x] Architecture designed; engine facts verified in UE 5.8 source (ADR §2)
- [x] Gevor approved the ADR decisions (Standalone/packaged-only workspace, PIE keeps the current HUD; UMG panels in Slate dock tabs; existing view-binding pattern)
- [x] Phase 0 spike on `spike/dockable-workspace`: GO (ADR §8, STATE.md #21). Verified by Claude in Standalone Game: docked viewport + panels, float to monitor 3, cursor picking docked and floated, unknown tab on restore, reset, level travel, saved resolution untouched; +0.32 ms GPU at 1600x900
- [ ] Gevor, by mouse in Standalone Game (Play > Standalone Game): drag-split/re-dock/tear-off tabs, move the floating viewport between monitors, RMB fly + gizmo + Delete in a floated viewport, Alt+Enter/F11 with a floated viewport, OS-close a floating viewport window, resize the main window
- [ ] Phase 1 shell. Already in the spike: `UStageCraftGameEngine`, `UStageWorkspaceSubsystem`, `FStageWorkspaceShell`, `StageCraft.Panel.*` tags, Viewport/Inspector/Fader panels, default layout, controller register/unregister. Remaining: `UStagePanelDefinition` assets (replace the interim config classes, async load), Window menu, viewport-overlay HUD layer, hide the viewport tab's close button, floating window titles, fullscreen routing
- [ ] Phase 2 layouts: `FStageLayoutStore`, Save As/Load/Reset, restore on launch, monitor clamping, automation tests
- [ ] Phase 3 preferences: `UStageCraftUserSettings`, Preferences panel (General, Layout, Viewport, Graphics)
- [ ] Phase 4 key bindings: IMCs to assets with player-mappable keys, `UEnhancedInputUserSettings`, Keyboard page
- [ ] Phase 5 polish: Slate style from `UStageCraftUITheme`, overlay DPI rule, per-monitor fullscreen viewport
