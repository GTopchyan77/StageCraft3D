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
- [ ] Shop widget + wallet readout (refusals and purchases now show in the workspace status bar, STATE.md #24)
- [ ] Lock badges / Unlock buttons in the catalog
- [ ] Session-limit check on Type swap; parameter display names in lock messages
- [ ] Server-authoritative commerce backend + RPC requests when multi-user/marketplace starts

## Phase 6 — Dockable workspace, detachable viewport, preferences (design: `Docs/ADR/0001-dockable-workspace.md`, STATE.md #20)
- [x] Architecture designed; engine facts verified in UE 5.8 source (ADR §2)
- [x] Gevor approved the ADR decisions (Standalone/packaged-only workspace, PIE keeps the current HUD; UMG panels in Slate dock tabs; existing view-binding pattern)
- [x] Phase 0 spike on `spike/dockable-workspace`: GO (ADR §8, STATE.md #21). Verified by Claude in Standalone Game: docked viewport + panels, float to monitor 3, cursor picking docked and floated, unknown tab on restore, reset, level travel, saved resolution untouched; +0.32 ms GPU at 1600x900
- [ ] Gevor, by mouse in Standalone Game (Play > Standalone Game): drag-split/re-dock/tear-off tabs, move the floating viewport between monitors, RMB fly + gizmo + Delete in a floated viewport, Alt+Enter/F11 with a floated viewport, OS-close a floating viewport window, resize the main window
- [ ] **Phase 1 as requested by Gevor 2026-10-06 ("Core Window Manager & Workspace Layout System") = ADR Phase 1 shell + ADR Phase 2 layouts.** The spike code is the base. Plan (ADR §9):
  - [x] `UStagePanelDefinition` (`StagePanel` primary asset, ID name = panel tag, widget class in the `UI` bundle, async load); replaces the interim `InspectorPanelClass`/`FaderBankPanelClass` config
  - [x] Shell: spawners from the subsystem's panel list (idempotent), tab labels from definitions, menu bar slot, `CaptureLayout`, main-window placement capture/apply, persist hook, main-window close snapshot (`RequestDestroyWindowOverride`)
  - [x] `FStageLayoutStore` (pure): versioned JSON file, name validation, list/save/load/delete, corrupt/version mismatch -> `.bak`, clamp main + floating windows to monitor work areas
  - [x] Subsystem API: `ClosePanel`, `GetAvailablePanels`, `GetPanelDisplayName`, `ApplyLayout`, `SaveCurrentLayoutAs`, `DeleteLayout`, `GetBuiltInLayouts`/`GetUserLayouts`, `GetActiveLayoutName`; `OnLayoutApplied`, `OnLayoutsChanged`; session autosave (deferred, async write) + restore on launch
  - [x] `SStageWorkspaceMenuBar` view: Window (panel toggles, Reset Layout) and Layout (built-ins, user layouts, Save As, Delete) menus; reads state when opened, commits through the subsystem
  - [x] Console commands: `OpenPanel`, `ClosePanel`, `SaveLayout`, `LoadLayout`, `DeleteLayout`, `ListLayouts`
  - [x] Automation tests `StageCraft.Workspace.LayoutStore.*`
  - [x] Data assets `DA_Panel_Inspector`, `DA_Panel_FaderBank`; AssetManager `StagePanel` scan entry
  - [x] Build Editor + Game (Development, Shipping), run tests, scripted Standalone verification (save, restart, restored; corrupt file; off-screen clamp; level travel). STATE.md #22
  - [ ] Gevor, by mouse in Standalone Game: menu bar visuals, Window/Layout menus (toggle, Reset, Save As + inline error, Delete), drag tabs/windows then restart restores, maximized main window round-trip (STATE.md #22)
  - Left for later phases: viewport-overlay HUD layer, fullscreen routing, viewport tab close button, floating window titles (ADR Phase 5)
- [ ] Phase 3 preferences: `UStageCraftUserSettings`, Preferences panel (General, Layout, Viewport, Graphics)
- [ ] Phase 4 key bindings: IMCs to assets with player-mappable keys, `UEnhancedInputUserSettings`, Keyboard page
- [ ] Phase 5 polish: Slate style from `UStageCraftUITheme`, overlay DPI rule, per-monitor fullscreen viewport

## Phase 7 — Placement mode, transform editing, audio (requested by Gevor 2026-10-06; design: `Docs/ADR/0002-placement-and-audio.md`, STATE.md #23)
- [x] `StagePlacementMath` (pure transform rules) + `USpawnSystemComponent` reduced to a stateless spawn/delete executor; continuous placement (`EStageItemPlacementMode`) removed
- [x] `UStagePlacementToolComponent` (Select/Place mode, armed item, preview state, single-click place, snap/fail events) + `AStagePlacementPreview` (ghost mesh + landing decal)
- [x] Controller: screen-position input handlers, Place-mode click routing, hold never spawns, event-driven preview refresh (viewport `OnCursorMoved` + camera moves, coalesced to next tick), P / Esc / Space keys, `RequestPlaceItem`
- [x] Gizmo Scale mode (axis + uniform) and Space cycling; inspector Scale row with clamping
- [x] `UStageItemLibraryPanel` + `Panel.Library` + `DA_Panel_Library`, docked left in the Default layout
- [x] Audio: `UStageAudioSubsystem`, `UStageCraftUserSettings` (master/effects/mute, persisted), `UStageAudioDeveloperSettings` (cue map), `UStageEditorAudioFeedbackComponent`
- [x] Menu bar: Edit (modes) and Audio (mute, sliders) menus; console commands `StageCraft.Edit.*`, `StageCraft.Audio.*`
- [x] Content via Python commandlet: `M_PlacementGhost`, `M_PlacementMarker`, `SFX_*` feedback sounds, `DA_Panel_Library`
- [x] Automation tests `StageCraft.Placement.*`, `StageCraft.Audio.*`, `StageCraft.Gizmo.*`
- [x] Builds (Editor, Game Dev, Game Shipping) + scripted Standalone verification (STATE.md #23)
- [ ] Gevor, by mouse: ghost/marker look, click vs hold, Library panel, menus/sliders, hearing the cues, scale gizmo drag
- [ ] Designer: add `UStageItemLibraryPanel` to `WBP_StageCraftHUD` (left side) so PIE has a Library too
- [ ] Follow-ups: gizmo scale snapping, local-space gizmo, undo/redo, audio page in the Preferences panel (ADR 0001 Phase 3)

## Phase 7b: Library redesign, stamping, status bar (requested by Gevor 2026-10-06; ADR 0002 §7, STATE.md #24)
- [x] Stamping: Place mode + ghost stay after each placement; exit only by Esc / P / Library PLACE toggle / Edit menu; verdict re-evaluated after each stamp
- [x] Library panel restyled from `UStageCraftUITheme`: toolbar (title, count, PLACE toggle), search, collapsible categories, themed rows with armed outline
- [x] `UStageToolButton` (never takes keyboard focus) for Library buttons
- [x] Workspace status bar (`UStageStatusBarWidget`): mode chip + hint, timed messages; viewport `AddOnScreenDebugMessage` feedback removed
- [x] Keyboard focus restored to the viewport after every layout change / viewport move (fixes keys going nowhere after Reset / float / dock)
- [x] Tests `StageCraft.UI.LibrarySearch`, `StageCraft.UI.StatusHint`; dev `StageCraft.Edit.Key`, `StageCraft.Workspace.Screenshot`
- [x] Builds + scripted Standalone verification with screenshots (STATE.md #24)
- [ ] Gevor, by mouse: Library look/hover/collapse/search, PLACE toggle, stamping by real clicks, Esc/P on the real keyboard, status bar messages
- [ ] Designer: add `UStageStatusBarWidget` (and the Library) to `WBP_StageCraftHUD` so PIE gets the status bar too
- [ ] Follow-ups: item thumbnails (icons are empty in the test catalog, rows show initials), lock badge on locked Library items
