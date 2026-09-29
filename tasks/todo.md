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

### 5.1 Core C++ (implemented, Game target compiles clean; awaiting Editor build + PIE)
- [x] Equipment data model: `ULightingFixtureData`, `UAudioEquipmentData`, `UStageTrussData`, `FStageEquipmentSpecs`, `EStageItemType::Truss/Audio`
- [x] Placed-instance state: `ALightingFixtureActor` (FixtureId, DMX patch, attributes, pan/tilt/beam), `AAudioEquipmentActor`, `AStageTrussActor`
- [x] Parameter model: `FStageParameterValue/Descriptor/Section`, `IStageParameterInterface`, Param/Attribute/FeatureGroup native tags
- [x] `AModularBaseActor`: Info + Transform sections, `OnParameterChanged` (including gizmo moves via root `TransformUpdated`), `InstanceLabel`
- [x] Inspector C++ bases: `UStageInspectorPanel`, `UStageParameterSectionWidget`, `UStageParameterRowWidget`, `UStageCraftUITheme`; `HUDWidgetClass` on the controller
- [x] `UShowControlSubsystem`: fixture registry, auto fixture ID and auto-patch, patch conflict query, cue store/go/fade, Internal/External control source
- [x] DMX boundary: `UStageDMXBridge`, fixture `WriteDMX`/`ReadDMX` (8/16-bit, CMY), `RenderDMXUniverse`
- [x] New catalog category tags (Stage.Deck/Riser, Truss.*, Rigging.*, Lighting.*, Audio.*)
- [ ] Editor target build with the editor closed (Game target built by Claude, see STATE.md #9)

### 5.2 Content (next)
- [ ] `DA_StageCraftTheme` (UStageCraftUITheme)
- [ ] Row WBPs: `WBP_Row_Float` (ValueSpinBox), `WBP_Row_Vector` (SpinX/Y/Z; also used for Rotator), `WBP_Row_Bool`, `WBP_Row_Text`, `WBP_Row_Color` (ColorSwatch + picker), `WBP_Row_ReadOnly` (ValueText)
- [ ] `WBP_InspectorSection` (RowContainer, HeaderText, GroupColorStrip) and `WBP_Inspector` (SectionContainer in a ScrollBox, TitleText, SubtitleText, EmptyState)
- [ ] `WBP_StageCraftHUD` root layout (left: catalog, right: inspector, bottom: show panel, top bar), assigned via a `BP_ModularPlayerController` subclass or the game mode
- [ ] Test catalog: a moving head (`DA_Fix_MovingHeadSpot`), an LED par (RGBW, no pan/tilt), a line array element, a sub, an F34 2 m truss, a chain hoist
- [ ] Level: volumetric fog in `L_StageTest` so beams read

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
- [ ] PIE: place a fixture; the inspector shows Info/Transform/Patch/Dimmer/Position/Color/Beam; editing Dimmer/Pan/Tilt/Zoom updates the light live
- [ ] PIE: drag with the gizmo; the Transform rows update live
- [ ] PIE: StoreCue 1 / change values / StoreCue 2 / GoToCue 1; the fade runs over the fade time
- [ ] Output Log: `Fixture N registered (...), patch U.AAA.` for each placed fixture, with no patch overlaps

## Backlog
- [ ] Camera navigation (orbit/pan/zoom); mouse look is disabled so LMB/RMB don't fight the camera
- [ ] Hover highlight via IInteractableInterface::OnHoverBegin/End (needs a cheap throttled cursor trace)
- [ ] Gizmo handle hover highlight
- [ ] Place onto an existing item with a modifier (plain LMB on an item now selects it)
- [ ] Local-space gizmo option; gizmo drawn on top of geometry (needs a custom no-depth-test material)
- [ ] Undo/redo, hooking OnItemSpawned / OnItemDeleted / gizmo drag end
- [ ] Overlay materials for hover/selected (content)
