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
- [~] Show file save/load: stage scenes done (STATE.md #26, JSON scene files with items, transforms, ids, SaveGame state); cue list not saved yet
- [ ] Rigging: snap fixtures to truss `RiggingPoints`, truss-to-truss `Connectors`, load per point against the safe working load
- [ ] Audio: coverage cone visualisation, line array hang builder (splay chaining)
- [~] Multi-select done (Ctrl+Click, group gizmo and numeric transforms, batch delete; STATE.md #26); group edit of non-transform inspector parameters still open

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
- [x] Undo/redo (STATE.md #25, ADR 0003)
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
- [ ] Follow-ups: gizmo scale snapping, local-space gizmo, audio page in the Preferences panel (ADR 0001 Phase 3)

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

## Phase 8: Undo/redo and object snapping (requested by Gevor 2026-10-06; design: `Docs/ADR/0003-undo-redo-and-object-snapping.md`, STATE.md #25)
- [x] Command history: `IStageEditCommand` + Place / Delete / Transform commands, `FStageCommandHistory` (depth 100), `UStageEditHistorySubsystem` (per world), `UStageEditHistoryComponent` (controller)
- [x] Stable item ids + SaveGame snapshots; undo of delete restores through `USpawnSystemComponent::RestoreItem` (placement rules apply)
- [x] Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z (Enhanced Input chords), Edit menu History section, status bar "Undo: ..." messages
- [x] Object snapping (`StageSnapMath`, `UStageSnappingComponent`): placement (X/Y) and gizmo Move (dragged axis), magenta guides, Snap cue, Edit > Snap to Items
- [x] Tests `StageCraft.History.Stack`, `StageCraft.History.ItemCommands`, `StageCraft.Snap.Math`; scripted Standalone runs (STATE.md #25)
- [ ] Gevor, by mouse/keyboard: shortcuts on the real keyboard (and not while typing in a field), mouse gizmo drag snapping + guide, Edit menu entries, status bar messages, Snap cue
- [ ] Follow-ups: undo for non-transform parameter edits, hold-to-bypass snap key, placement snapping for Blueprint-only visuals

## Phase 9 — Multi-selection, Clear Stage, scene files, renders (STATE.md #26, ADR 0004)
- [x] Selection set with Ctrl+Click toggle, Select All, group gizmo and numeric transforms, batch delete as one undo step
- [x] Clear Stage with confirmation submenu, undoable
- [x] Scene save / load / delete (async, validated JSON), File menu
- [x] Render subsystem + Render panel (presets, AA, post, watermark), Render menu, status bar
- [x] Editor / Game / Shipping builds clean; 20/20 automation; scripted Standalone runs R26_Run1-6
- [ ] Manual verification (Gevor): STATE.md #26 "Not verified" list
- [ ] Render banding in wide gradients: compare with viewport at the same framing; HDR capture + dithered quantisation
- [ ] Render cancel button; group edit of non-transform parameters; save cue list with scenes

## Phase 10 — Spans, Outliner, Bill of Materials (STATE.md #27, ADR 0005)
- [x] Span data (whole / stretch fit, segment + joint parts, caps), pure span math, instanced geometry component, span actor (Length parameter, quantity, no scale)
- [x] Two-click span tool with ghost, polar angle snap, chained runs, Esc steps back; status bar length / segment readout
- [x] Quantity-aware rules: validator + EvaluatePlacement quantity, spawn validates the prepared actor, lengthening checked, shrinking always allowed
- [x] Undoable span length edits (parameter command through the GameMode)
- [x] Visible / Locked item flags (SaveGame, inspector rows); GameMode lock rules; selection-wide transform checks; hidden items unclickable, unsnappable, skipped by Select All
- [x] Outliner panel (disciplines, eye / lock tri-state, filter, group selection, Ctrl add / remove)
- [x] Bill of Materials subsystem (derived cache), panel (tiles + table + subtotals), async atomic CSV export, File > Production menu
- [x] Content: DA_Panel_Outliner, DA_Panel_BillOfMaterials, DA_Span_TrussF34 / Railing / CableRun, span materials
- [x] Editor / Game / Shipping builds clean; 29/29 automation; scripted Standalone runs R27_Run1-4 (two bugs found and fixed)
- [ ] Manual verification (Gevor): STATE.md #27 "Not verified" list
- [ ] Auto-numbered labels for repeated items ("Truss Run 1, 2…"); hide / lock keyboard shortcuts
- [ ] Rigging loads per point and per-phase power distribution (amps) in the BOM
- [ ] Quantity check on span Type swaps; user-defined layers

## Phase 11 — Inspector polish, runtime model import (STATE.md #28, ADR 0006)
- [x] Themed inspector editors (numeric, checkbox, dropdown, text, 22 px fields, X/Y/Z tags, swatch); WBP units column, padding, amber headers
- [x] Runtime import subsystem (Interchange: glTF/GLB, OBJ, FBX where available), queue, validation, Imports.json restore, Imported category
- [x] OS drag and drop on every panel (Slate drop target with hint), IMPORT button + File > Library (IDesktopPlatform, dev-tool builds)
- [x] Library cards: format/size subtitle, clay thumbnails; GameMode EvaluateModelImport
- [x] Editor / Game / Shipping builds clean; 33/33 automation; scripted Standalone runs R28_*
- [ ] Manual verification (Gevor): STATE.md #28 "Not verified" list
- [ ] Remove imported models from the Library; per-import units/axis for OBJ; weight/power for imports
- [ ] Packaged build test of runtime import; repair engine InterchangeAssets material (Kmidden reference)
- [ ] Datasmith scene import (DatasmithRuntime) if needed; native dialog in Shipping

## Phase 12 — Professional lighting, patch sheet, span paths, inspector grid, import/render hardening (STATE.md #30, ADR 0007)
- [x] Unblock the build: 19 plugins from `b8448a5` + UnrealNGGMCP disabled in the `.uproject` (Gevor's choice for the 19; UnrealNGGMCP half-reverted)
- [x] Inspector column grid: label/units fixed columns enforced in C++, ellipsis + tooltips, read-only values aligned with field text, DMX Mode / Channels rows
- [x] Fixtures: CTC, stepped Gobo + rotation, Iris, four blades + rotation, capabilities, default DMX layout, validation, pure DMX mapping
- [x] Projection: `M_StageBeamLightFunction` + eight generated gobos; MID only for fixtures with beam shaping
- [x] Strobe clock in `UShowControlSubsystem` (ticks only while strobing)
- [x] `StagePhotometry` + Photometrics section; fader short names, Shapers group, sideways-scrolling bar
- [x] DMX Patch panel (table + channel map, conflicts, Patch Unpatched); `DA_Panel_Patch`; Default layout tab
- [x] Span paths: path math + tests, actor state + `SpanLength` migration, multi-point tool, close refusal, corner hub, cable sag (undoable, lock-aware)
- [x] Import: Win32 `IFileOpenDialog` in Shipping; Datasmith refusal
- [x] Render: self-registering editor helpers; banding A/B (HDR rejected, cause = fog slicing without temporal AA; no change)
- [x] Editor / Game / Shipping builds clean; 37/37 automation; scripted Standalone runs R30_Run1–8
- [ ] Manual verification (Gevor): STATE.md #30 "Not verified" list
- [ ] Re-enable plugins one by one once they compile with StageCraft (`PlacementGizmo` enum rename; UnrealNGGMCP pick one version)
- [ ] Haze render banding in Off/FXAA: force temporal AA or finer fog Z for haze renders (measure first)
- [ ] Undo for attribute and patch edits; cue list saved with scenes

## Milestone audit (STATE.md #31, 2026-10-19)
- [x] Audit every Tick / timer / polling path; gobo, iris and shutter MID updates only on attribute writes; patch sheet and faders push-only
- [x] Encoder: per-frame NativeTick replaced by a self-stopping Slate active timer (`DisableNativeTick`)
- [x] Span Info Weight / Power = spec x quantity, live on length / undo
- [x] Photometrics: "No surface" instead of 0.0 m / 0.0 lx when the beam hits nothing
- [x] Fader / encoder names: real ellipsis (left-justified text in a centred slot); audio "Polarity" short name
- [x] `bUsedWithInstancedStaticMeshes` on span and ghost materials (`fix_instanced_material_usage.py`)
- [x] Shipping unity fix: `WindowsHWrapper.h` before COM headers in `StageFileDialog.cpp`
- [x] Editor / Game / Shipping (full unity) builds clean; 37/37 automation; scripted runs R31_Run1-4 with screenshots
- [ ] Manual verification (Gevor): STATE.md #31 "Not verified" list
- [ ] BOM: current (A) at a configured supply voltage and per-phase distribution; rigging point loads

## Phase 13 — Viewport toolbar, ortho projections, view modes, global grid snap (STATE.md #32, ADR 0008)
- [x] Types + pure math: `EStageViewProjection`, `EStageViewMode`, `StageViewMath` (rotations, ortho fit/zoom, screen-scale distance, grid lines, snap presets), `StagePlacementMath::WithGlobalGrid`; tests
- [x] Settings: grid snap, angle snap, grid visibility (sanitised to presets)
- [x] `UStageSnappingComponent`: grid/angle snap + `OnSnapSettingsChanged`; controller pushes to gizmo, placement and span tools
- [x] Gizmo: absolute grid alignment on moves; ortho-aware screen scale (gizmo + snap guides)
- [x] Camera pawn fixed view (exact axis rotation, planar motion)
- [x] `UStageCraftGameViewportClient::ApplyStageViewMode` (Lit / Unlit / Lines)
- [x] `UStageLineViewCameraModifier` + `M_StageLineView` (content script)
- [x] `AStageGridActor` (line batch, no tick, render helper)
- [x] `UStageViewportComponent`: projection switch, ortho pan/zoom, view mode, grid; controller input routing (RMB pan, wheel zoom in ortho)
- [x] Render capture follows projection
- [x] `UStageViewportToolbar` + shell callback in the viewport tab
- [x] Dev console `StageCraft.View.*`, `StageCraft.Snap.*`
- [x] Builds (Editor / Game / Shipping full unity), automation, scripted runs with screenshots
- [x] STATE.md #32
- [x] Toolbar layout revision (STATE.md #33): grid and angle snap as one-click segments beside LIT / UNLIT / LINES / GRID, gizmo segment last
- [x] View menu (Unreal-style dropdown: Lit, Unlit, Wireframe / CAD Lines, Lighting Only, Detail Lighting, Reflections, Show > Grid) and toggling snap segments (STATE.md #34)
- [ ] Manual verification (Gevor): View menu open/pick/close by mouse, snap and angle toggles by mouse (STATE.md #34)
- [x] Toolbar sections with visible dividers (projection | view | grid snap | angle snap | gizmo) (STATE.md #35)
- [x] Outdoor environment: Sky Creator backend, Environment panel (sky on/off, weather presets, time, sun height/direction, clouds, rain, snow, fog) (STATE.md #35, ADR 0009)
- [ ] Manual verification (Gevor): Environment panel sliders and presets by mouse (STATE.md #35)
- [x] Packaged build: Sky Creator assets cooked, sky renders; measure sky GPU time / VRAM (ADR 0009 §7) — STATE.md #37
- [x] Save environment settings with scenes (scene format v2) — STATE.md #37
- [ ] Fix invisible 1 px header lines in code-built panels with `StageCraftWidgetStyle::MakeLine` (ADR 0009 E7)
- [ ] Manual verification (Gevor): STATE.md #32 "Not verified" list (toolbar clicks, right-drag pan, wheel zoom, WASD in ortho, packaged Unlit / Lines)
- [ ] Grid fade with distance in perspective (batched lines cannot fade; would need a line material or a mesh grid)

## Audit follow-ups (STATE.md #36, Docs/PROJECT_AUDIT.md)
- [x] PROJECT_AUDIT.md + CURRENT_SYSTEMS.md; 46/46 automation
- [ ] Review the audit and answer its open questions (Gevor; PROJECT_AUDIT.md §10)
- [ ] Untrack `Config/UserEngine.ini`, rotate the UnrealNGGMCP token; confirm the #29 Anthropic key was rotated (R1)
- [ ] Partial scene load stays dirty, does not silently drop items on re-save; load waits for pending imports (R2)
- [x] Undo/redo transforms checked by the GameMode (lock) (R3) — STATE.md #37
- [x] Coalesce inspector spin-box drags into one undo step (R4) — STATE.md #37
- [x] Quit prompt when dirty; timed autosave (R5) — STATE.md #37
- [ ] Sanitise restored SaveGame state: fixture attributes, patch, loudspeaker gain/delay (R6)
- [ ] Move a corrupt Imports.json aside instead of overwriting it (R8)
- [ ] World-based tests: GameMode rules, request bridge, selection, ApplyDocument, patch conflicts, cues, economy (R11)
- [ ] Docs: Rules.md subsystem table + foreign RunEditor/DamenTools8/UnrealClaude sections; ADR 0001 §3; stale todo boxes; Content/Python setup script (R12)

## Phase 14 — Edit rules on undo/redo, one-step drags, autosave + exit prompt, scene format v2 (STATE.md #37, ADR 0010)
- [x] `StageTransformRules::IsSaneTransform` (finite, |location| <= 10 km, non-degenerate rotation); scene store uses the same limit; tests
- [x] GameMode `EvaluateTransformChange` (every changed part through `EvaluateParameterChange`: lock, scale support, ownership)
- [x] History: `TransformValidator` on `UStageEditHistoryComponent`; undo/redo transforms refused for locked items; insane targets dropped; bound by the controller
- [x] Group edits: per-follower sanity check in `FollowLeader`; batch undo/redo checks every item (all-or-nothing rollback)
- [x] History transactions (`BeginTransaction` / `EndTransaction`): edits recorded in between merge into ONE step (first "before" per item / parameter)
- [x] Inspector rows: spin-box slider drag = one transaction (control widget interaction start/finish → view → controller); closed on destroy, re-inspect, undo/redo
- [x] Scene format v2: `environment` object; v1 files migrate (default environment); v3+ refused; environment sanitised; tests
- [x] Environment edits mark the scene dirty (session binds `OnEnvironmentChanged`); load applies the environment before MarkClean
- [x] Autosave: while dirty, every N s to `Saved/StageCraft/Scenes/Autosave/<Scene>.json`; deleted after a real save or Discard; File > Recover Autosave (stays dirty)
- [x] Exit prompt: closing the main window with unsaved changes shows Save / Don't Save / Cancel in the workspace; Save waits for the write, then quits
- [x] World tests: locked undo/redo (single + group), transaction coalescing; scene v1→v2 migration
- [x] Builds: Editor Development, Game Development, Game Shipping; automation
- [x] Package (Development + Shipping): Sky Creator assets cooked, no missing-asset errors; packaged run with sky on; GPU frame time and VRAM measured sky off / on
- [x] ADR 0010; STATE.md #37
- [x] Manual verification (Gevor, reported 2026-10-21): one undo step per inspector spin-box drag (Location / Rotation / Scale, span Length / Sag); exit prompt buttons and name field by mouse; File > Recover Autosave (STATE.md #37)
- [ ] Measure the sky on a machine without a second editor open (STATE.md #37 numbers were taken with the Kmidden editor sharing the GPU)

## Phase 15 — Cue list panel, cues in scenes (v3), patch sheet PDF + CSV (requested by Gevor 2026-10-21; STATE.md #38, ADR 0011)
- [x] `StageCueRules`: number range 0.001..9999.999 (thousandths, double precision), fade 0..600 s, label 256, list repair; tests
- [x] Show control: Store returns false for bad numbers, UpdateCue, Back, FindCue, ReplaceCues (undo keeps playback)
- [x] GameMode `EvaluateCueRequest`; controller RequestStoreCue / UpdateCue / DeleteCue / GoToCue / CueGo / CueBack / SetControlSource
- [x] Undo: `FStageCueListCommand` (whole-list snapshots) through `IStageItemEditor::SetCueList`; cue edits close an open drag first
- [x] Cue edits mark the stage dirty; scene format v3 `cues` (v1/v2 migrate with none; damaged entries dropped and counted); tests
- [x] Cue panel (`UStageCuePanel`, `DA_Panel_Cues`), default layout tab, File > Production > Cue List...
- [x] Patch sheet: `FStagePdfDocument`, `StagePatchSheet` (PDF + CSV), `UStagePatchSheetSubsystem`, File > Production entry, Patch panel EXPORT PDF, status bar; tests
- [x] Builds: Editor / Game Development / Game Shipping; 54/54 automation; scripted runs R38_Run1-4 (cue ops, refusals, undo, save/load v3, playback on fixtures, PDF checked)
- [ ] Manual verification (Gevor): Cue panel by mouse (GO/BACK, STORE/RE-RECORD, inline label/fade, GO TO, DELETE, source segment), File > Production entries, EXPORT PDF, print the PDF (STATE.md #38)
- [ ] GO keyboard shortcut; tracking cues, follow/wait times, timeline
- [x] Packaged build check with the new panel asset (#39: DA_Panel_Cues in the Shipping and Development containers)
- [ ] PDF: embedded Unicode font if non-Latin labels matter

## Phase 16 — Production reports (BOM PDF, cable schedule and power plan), fixture benchmark, Shipping validation (requested by Gevor 2026-10-21; STATE.md #39, ADR 0012)
- [x] `FStageReportPdfComposer`: flowing A4 report layout (title block and "Page n of m" on every page, headings kept with their table, header rows repeated after page breaks); `FStagePdfDocument::SetCurrentPage`; tests
- [x] Bill of materials as PDF + CSV (both or neither, `StageReportFiles::WritePdfAndCsv`); BOM panel EXPORT PDF, File > Production; tests
- [x] Cable schedule and power plan: `StageCableSchedule` (balanced phases and circuits, overloads, power home-runs, DMX chains with stock lengths, warnings), `UStagePowerDistributionSettings` (Project Settings), `UStageCableScheduleSubsystem`, File > Production, `StageCraft.Report.ExportCables`; tests
- [x] Fixture benchmark `StageCraft.Bench.Fixtures` (Development only) + `Scripts/Bench/RunFixtureBench.ps1`; `StageFrameStats` tested; the user's show is restored afterwards
- [x] Measured and fixed: light function only while the beam is shaped, batched light rebuild for moving heads, no navigation updates, one pivot update, only driven outputs, fades write only changing attributes; world tests Lighting.BeamShapingOnDemand / PanTiltTransform / FadeChangedOnly
- [x] Builds: Editor, Game Shipping (0 warnings); 63/63 automation; Shipping and Development packages (cook 0 warnings, Sky Creator 259 assets, dev commands absent from Shipping, Shipping starts and closes cleanly); exports run in the packaged Development build
- [ ] Manual verification (Gevor): File > Production > Export Bill of Materials / Export Cable Schedule & Power, BOM panel EXPORT PDF, print both PDFs; watch a crossfade of moving heads for flicker (lights are rebuilt while moving); gobo/iris/blade looks still project
- [ ] 500 fixtures crossfading at once is render-thread bound (27–33 ms): fewer light updates per frame (DMX-rate updates for distant fixtures) or engine changes
- [ ] Batch the per-attribute `OnParameterChanged` broadcasts during fades (about 2 ms at 200 fixtures)
- [ ] Startup model re-import blocks the game thread for tens of seconds (audit R2); VSM "One Pass Projection max lights overflow" from about 50 overlapping spot lights
- [ ] Cable schedule: saved circuit assignment (scene v4), power cable lengths once the distro has a position; patch sheet onto the composer

## Phase 17 — Preferences and key bindings, audit risks R2/R6/R8 (requested by Gevor 2026-10-21; STATE.md #40, ADR 0013)
- [x] Key map model `FStageKeyBindings` (24 commands, two slots, exact chords, conflict rules, fly keys may share a command key); saved as overrides in GameUserSettings.ini; tests
- [x] `UStageInputPreferencesSubsystem` (first ULocalPlayerSubsystem): key map, look sensitivity, invert look; single writer, OnInputPreferencesChanged
- [x] Controller: keyboard commands looked up on key press (viewport key state for modifiers), fly mapping rebuilt from the key map, look preferences applied; per-shortcut input actions and chord machinery removed
- [x] Menus and status bar hints name the user's keys; Edit > Preferences...; `UStagePreferencesPanel` + `DA_Panel_Preferences` (camera, viewport, audio, shortcuts with capture/clear/reset)
- [x] R2: unplaced items kept and saved back, partial load stays unsaved, damaged-entry backup (.bak), loads refused while imports run (ImportsPending)
- [x] R6: restored state sanitized (fixture attributes/patch/ID, audio gain/delay/splay, label, span path), repairs counted; no package loads from saved state
- [x] Found by the R6 test: the DMX patch was never saved (FStageDMXPatch fields not SaveGame) — fixed
- [x] R8: damaged Imports.json moved to .bak, unreadable list never overwritten; `StageAtomicFile` flushes before renaming (all saves, reports, renders)
- [x] Builds: Editor, Game Shipping (0 warnings); 69/69 automation; scripted runs R40_RunA–D
- [ ] Manual verification (Gevor): Preferences panel by mouse (click a shortcut, press keys, CLEAR, RESET, RESET ALL, refusal messages, look speed/invert while flying); Edit menu labels after a rebind; real keyboard Ctrl+Z/Ctrl+Shift+Z; File > Open of a scene with a missing imported model
- [ ] Unplaced items: show them somewhere (Outliner group) and allow discarding them; Clear Stage keeps them today
- [ ] Mouse buttons are not remappable (viewport capture depends on them)
