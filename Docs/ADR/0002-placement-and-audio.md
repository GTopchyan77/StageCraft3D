# ADR 0002 — Placement mode, transform editing and editor audio

- **Status:** accepted (plan approved by Gevor 2026-10-06), implemented. See STATE.md #23.
- **Related:** ADR 0001 (workspace; this ADR introduces its planned `UStageCraftUserSettings` early, with audio fields only).

## 1. Problem

Gevor asked for editor-grade object placement:
- **Placement mode with a ghost.** A dedicated mode in which a ghost of the armed item follows the cursor and a marker shows the exact landing spot. One click places exactly one object; holding the button must not keep placing.
- **Selecting never moves.** Clicking a placed object selects it and never makes it follow the mouse. Transforms are edited explicitly with Move / Rotate / Scale, a gizmo and numeric XYZ fields applied on Enter.
- **Central audio manager.** Feedback sounds for selection, placement, snapping and errors, with Master volume, Effects volume and Mute exposed to settings and UI.

What existed before:
- **Placement.** `USpawnSystemComponent` placed on press. With `EStageItemPlacementMode::Continuous` (the default for props) it kept stamping while the button was held. There was no mode, ghost or marker, and a click on empty ground placed the armed item.
- **Transform tools.** The gizmo had Move and Rotate but no Scale. The inspector had numeric Location and Rotation but no Scale.
- **Library and audio.** There was no library UI and no audio code.

## 2. Verified engine facts

| # | Fact | Source | Consequence |
|---|---|---|---|
| P1 | `FAudioDevice::SetTransientPrimaryVolume` is reset to 1 by the engine on world cleanup and camera fades. | `UnrealEngine.cpp:16299`, `PlayerCameraManager.cpp:503-526`, `PlayLevel.cpp:825` | Unusable for a persistent Master volume. |
| P2 | Sound mixes pushed with `PushSoundMixModifier` stay active until popped; class overrides apply to children with `bApplyToChildren`. Sounds without a sound class use `UAudioSettings::GetDefaultSoundClass()`. | `AudioDevice.h:1097-1122`, `AudioSettings.h:296` | Master and Mute are a transient `USoundMix` override on the default (Master) class. |
| P3 | `FSceneViewport::OnMouseMove` calls `FViewportClient::MouseMove` / `CapturedMouseMove`, which `UGameViewportClient` does not override. It only fires while the cursor is over the viewport. | `SceneViewport.cpp:895-920`, `ViewportClient.h:174-183` | Cursor movement is an event: the preview needs no tick. Works for the docked and floated viewport alike. |
| P4 | The fly camera turns the view with `AController::SetControlRotation` and moves with `AddActorWorldOffset` on the pawn root. | `StageCameraPawn.cpp:158,184` | View changes are observable with no polling: override `SetControlRotation`, plus the root's `TransformUpdated`. |
| P5 | For a native-only `UUserWidget`, `NativeOnInitialized` runs at `CreateWidget`, before `RebuildWidget`. | UMG `UserWidget.cpp` (`Initialize`) | A code-built default tree must be created in `NativeOnInitialized`, or `SetItem` right after `CreateWidget` finds no widgets. |
| P6 | Python exposes `EFoo` and `AFoo` as the same name `Foo`. | Python plugin warning at startup | The preview enum is `EStagePlacementPreviewState`, not `EStagePlacementPreview`. |

## 3. Decisions

### 3.1 Placement

**`StagePlacementMath::ComputePlacementTransform` (pure).** The preview and the spawn share it, so the ghost is exactly where the item lands. It covers grid snapping per axis, optional alignment to the surface normal, and the pivot offset. A degenerate normal falls back to world up.

**`USpawnSystemComponent` is a stateless executor.**
- It provides `SpawnItem(Item, Transform)`, guarded by `PlacementValidator`, and `TryDeleteActor`.
- The stroke API and `EStageItemPlacementMode` were removed, so continuous stamping cannot return as a data option. Old data assets simply drop the saved value.

**`UStagePlacementToolComponent`** lives on the controller (per-player tool state, Rules.md "Where state lives"):
- **State:**
  - `EStageEditMode { Select, Place }`;
  - the armed item, cached from `UStageItemSubsystem::OnSelectedItemChanged`;
  - `EStagePlacementPreviewState { Hidden, Valid, Refused }`;
  - the last landing point.
- **Pushed in by the controller:** `UpdateTarget` / `ClearTarget` (cursor hits) and `TryPlace` (a click). The component never traces.
- **After a successful place:** the mode returns to Select and the ghost hides. The new actor is selectable but **not** auto-selected, so the Place cue is not followed by a Select cue.
- **Delegates:** `OnEditModeChanged`, `OnArmedItemChanged`, `OnItemPlaced`, `OnPlacementSnapped` (the landing point moved to another grid point, grid items only) and `OnPlacementFailed` (`NoItemArmed`, `NoSurface`). Rule refusals are reported by the validator's owner, the controller, through `OnRequestRejected`.
- **Preview colour is display, not permission.** It comes from `PreviewEvaluator` (the same GameMode rules, not reported). Every commit is validated again.

**`AStagePlacementPreview`** is owned by the tool and spawned lazily. It has no tick and no collision, so the placement trace can never hit it.
- **Ghost:** the item's `Mesh` with every slot set to `M_PlacementGhost` (translucent unlit, fresnel edge, `GhostColor`).
- **Marker:** a `UDecalComponent` using `M_PlacementMarker` (a decal ring with a faint fill, `MarkerColor`). It is projected along the surface normal at the snapped landing point and sized to the mesh footprint, at least 25 cm.
- **Missing materials** are logged once and degrade gracefully.

### 3.2 Input (controller)

**Handlers take a viewport position.** The Enhanced Input handlers read the cursor once and forward it to `HandlePrimaryPressedAt`, `HandlePrimaryHeldAt` or `HandlePrimaryReleased`. Traces use `GetHitResultAtScreenPosition` and `DeprojectScreenPositionToWorld`.

**What the primary button does:**
- **Press, Place mode:** `TryPlace`. A miss is passed as an empty hit, so the user gets the NoSurface error cue.
- **Press, Select mode:** gizmo handle > item (select) > empty (deselect). Select mode never places.
- **Held:** only continues a gizmo drag, so holding can never place.

**Preview refresh is event-driven.** Three sources (P3, P4) call `RequestPreviewRefresh`:
- the viewport client's `OnCursorMoved`;
- the camera root's `TransformUpdated`;
- `SetControlRotation`.

Edit-mode and armed-item changes call it too. Requests coalesce into one `SetTimerForNextTick` trace, and only in Place mode. So there is at most one trace per frame and none while idle.

**Keys:**
- **P** toggles Place mode.
- **Esc** leaves Place mode first (the item stays armed). A second Esc deselects and disarms.
- **Space** cycles Move → Rotate → Scale.
- **W/E/R are not used.** The always-applied camera context maps W/E for fly navigation and consumes them.
- Entering Place mode clears the selection, so the gizmo hides.

**`RequestPlaceItem(Item)`** is the Library's entry point. It runs the rules check (a refusal is reported and plays the error cue), then `SelectItem`, then `EnterPlaceMode`.

### 3.3 Transform editing

- **Gizmo Scale mode.** Each axis has a shaft plus a cube head, and a centre cube scales uniformly ("up the screen" grows). Drag one handle length outwards to double the scale.
- **One clamp.** `StageTransformRules` (pure) clamps scale to [0.01, 100] and repairs NaN to 1. The gizmo and the inspector both use it.
- **Inspector Scale row.** `StageCraft.Param.Transform.Scale` joins the base actor's Transform section next to Location and Rotation. Numeric XYZ applies through the existing validated `RequestParameterChange`. The spin boxes commit on Enter; like the Unreal Editor, they also commit when focus moves away, and the field reads the stored (clamped) value back.
- **Dirty flag.** A scale change marks the stage session dirty, like a move.

### 3.4 Library panel

`UStageItemLibraryPanel` and `UStageItemLibraryEntry` are C++ `UUserWidget`s:
- **Usable without a WBP.** They build a default tree in code when there is no designer tree (P5), so the panel definition `DA_Panel_Library` (`StageCraft.Panel.Library`) points straight at the native class. A WBP subclass can restyle them through `BindWidgetOptional`.
- **Content and events.** Items are grouped by category, with icons from the already-loaded UI bundle. The panel rebuilds on `OnCatalogLoaded` and highlights the armed item from `OnArmedItemChanged`.
- **Placement in the layout.** The Default layout docks it on the left (0.16). Existing sessions keep their layout, and the panel can be opened from Window.

### 3.5 Audio

**`UStageAudioSubsystem` (GameInstance) is the single API.** It has `PlayCue(Tag)` and Master, Effects and Mute getters and setters, and broadcasts `OnAudioSettingsChanged`.
- **Master and Mute:** a transient mix override on the default sound class (P1, P2). It is pushed once per audio device, re-applied after each map load (PIE and travel can change the device), and popped in `Deinitialize` so PIE never leaves the editor muted.
- **Effects:** a per-play gain on cues. Every UI and interaction sound must go through `PlayCue`.

**`UStageCraftUserSettings : UGameUserSettings` is the single source of truth for preferences** (ADR 0001 §Preferences):
- It holds `MasterVolume`, `EffectsVolume`, `bAudioMuted` and `AudioSettingsVersion`.
- Setters clamp, and `LoadSettings` sanitises what is read from disk.
- The audio subsystem is its only caller. Saving is debounced (one core-ticker handle, 1 s) and flushed on shutdown.

**`UStageAudioDeveloperSettings`** (Project Settings > Game > StageCraft Audio) maps `StageCraft.Sound.{Select, Place, Snap, Error}` to a sound, a level and a minimum retrigger interval. Sounds load asynchronously at startup, and a cue that isn't loaded is skipped with one warning.

**`UStageEditorAudioFeedbackComponent`** sits on the controller and maps events to cues: selection → Select, placed → Place, snapped → Snap, failed or refused → Error. The audio subsystem knows nothing about gameplay.

**UI:** an Audio menu in the workspace menu bar (Mute All, plus Master and Effects sliders that apply live and read back), and an Edit menu (mode and transform tool).

### 3.6 Content pipeline

The editor's MCP bridge was attached to another project, so content is generated by scripts in the repo. That also makes it reproducible:
- `Scripts/Content/make_feedback_wavs.ps1` synthesises the four WAVs into `Saved/ContentSource/Feedback`.
- `Scripts/Content/create_placement_audio_content.py` runs headless with `UnrealEditor-Cmd -run=pythonscript`. It builds both materials, imports the sounds and creates `DA_Panel_Library`.
- `PythonScriptPlugin` is enabled for the Editor target only.

## 4. Alternatives rejected

- **Keep `Continuous` as an opt-in mode.** It contradicts "one click = exactly one object". Repeat placement is P again or the Library.
- **Tick the controller to move the ghost.** That polls every frame even when idle. Events plus a next-tick coalesce give the same smoothness at zero idle cost.
- **Spawn a real item actor as the ghost.** Items register with the session and show control in `BeginPlay`, so a preview actor would show up as a placed fixture.
- **Transient primary volume for Master** (P1).
- **Sound class assets plus a Sound Mix asset.** These add content without adding capability. The transient mix on the engine Master class needs no assets.
- **Store volumes in the audio subsystem's own config.** That would be a second preferences store. ADR 0001 already decided `UStageCraftUserSettings`.
- **Auto-select the placed object.** It would play two cues and contradicts the spec's "selectable".

## 5. Consequences

- **Data assets lose `PlacementRules.PlacementMode`.** The tagged property is skipped on load and no fix-up is needed.
- **`GameUserSettingsClassName` is now `StageCraftUserSettings`.** Values previously saved under `[/Script/Engine.GameUserSettings]` are read as defaults once.
- **Feedback sounds must be cooked.** They are referenced only from config, so `/Game/StageCraft/Audio` (and `/Game/StageCraft/Placement`) are in `DirectoriesToAlwaysCook`.
- **Dev verification hooks.** `StageCraft.Edit.*` drives the real click handlers at a simulated cursor. It is compiled out of Shipping.
- **PIE has no Library panel yet.** The fixed HUD WBP has no Library; it needs a designer step. The console `StageCraft.Edit.Arm` works everywhere.

## 6. Rollback

- **Placement.** Revert the controller, tool, preview and spawn changes. Old data assets keep working because the field is gone, not renamed.
- **Audio.** Remove `GameUserSettingsClassName` and the subsystem. Engine settings fall back to defaults.
