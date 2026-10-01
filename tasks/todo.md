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
