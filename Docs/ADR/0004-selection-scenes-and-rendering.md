# ADR 0004 — Multi-selection and batch edits, scene files, still renders

- **Status:** accepted, implemented. See STATE.md #26.
- **Related:** ADR 0003 (command history, snapshots, instance ids), ADR 0001 (workspace, panels, preferences). Rules.md "Where state lives", "Safe data binding", "Economy security standards" (deny by default, catalog-only), Engineering Standard §16, §21, §26, §29, §55, §60.

## 1. Problem

Gevor asked for four features:
1. **Batch selection.** Ctrl+Click multi-select; Delete removes every selected item; transforms apply to the whole selection.
2. **Clear All.** Remove every placed item, with a safety check against accidental wipes.
3. **Save / load.** Write the stage (item, transform, instance id, settings) to disk and load it back at runtime, from the UI.
4. **Render.** A render panel with resolution presets (4K, 1080p, square), anti-aliasing and post-processing options, an optional watermark, and a non-blocking render to a project folder.

What existed: single selection (`USelectionComponent`), undo commands for one item (ADR 0003), `FStageItemSnapshot` (id, catalog item, transform, SaveGame bytes), the session's dirty flag, and no file or render pipeline.

## 2. Verified engine facts (UE 5.8 source)

| # | Fact | Source | Consequence |
|---|---|---|---|
| R1 | Scene captures turn Lumen GI and Lumen reflections off unless the component's post-process settings override them. | `SceneCaptureRendering.cpp` (~795-801) | The capture overrides both methods with the project's `r.DynamicGlobalIlluminationMethod` / `r.ReflectionMethod`. |
| R2 | `USceneCaptureComponent2D` defaults `ShowFlags.TemporalAA` and `MotionBlur` off. | `SceneCaptureComponent.cpp` (~684) | Temporal modes set the flag explicitly. |
| R3 | TSR/TAA fall back to FXAA without `bRealtimeUpdate`, and to none without a view state. Captures set `bRealtimeUpdate` from `bCaptureEveryFrame \|\| bAlwaysPersistRenderingState`. | `SceneView.cpp` `SetupAntiAliasingMethod`; `SceneCaptureRendering.cpp` (~913) | Captures render every frame with a persistent view state during a warm-up, so temporal AA, eye adaptation and Lumen converge. |
| R4 | `RTF_RGBA8` is linear `PF_B8G8R8A8` (not sRGB). | `TextureRenderTarget2D.h:52`, `.cpp:79` | Final-colour LDR bytes read back directly as `FColor`/BGRA8 sRGB. |
| R5 | `FRHIGPUTextureReadback::Lock` uses `FRHICommandListImmediate::Get()`; `IsReady` polls a GPU fence. | `RHIGPUReadback.cpp:211` | Copy, poll, map and release happen in render commands only. |
| R6 | `FImageUtils::CompressImage` loads `ImageWrapper` only on the game thread; elsewhere it only looks it up. | `ImageUtils.cpp:36-60` | The subsystem loads the module in `Initialize`, then compresses on a worker. |
| R7 | `UObject::SerializeScriptProperties` writes tagged (name-keyed) properties to a plain memory archive. | Visible in saved files (`GainDb`, `FloatProperty`...) | Per-item settings in scene files survive added/removed properties. |

## 3. Selection and batch edits

### 3.1 Selection set
- `USelectionComponent` holds an ordered set; the **last entry is the primary** (gizmo target, inspector subject). Every change goes through one private `ApplySelection`, which deselects what left, selects what joined, re-binds `OnDestroyed`, then notifies once.
- API: `SelectActor` (replace), `ToggleActorSelection` (Ctrl+Click), `DeselectActor`, `SelectActors` (Select All), `ClearSelection`, `GetSelectedActors`, `GetSelectionCount`, `IsActorSelected`.
- Delegates: existing `OnSelectionChanged(New, Previous)` now means "primary changed" (all consumers keep working); new `OnSelectionSetChanged(Count)`.
- Ctrl is read from Slate's modifier state (`FSlateApplication::GetModifierKeys`), independent of Enhanced Input mappings. Ctrl+Click on empty space keeps the selection (Unreal Editor behaviour).
- **Rejected:** a keyboard Select All (Ctrl+A). A/S/W/D/Q/E belong to the higher-priority camera context, which would block a Ctrl+A chord in the editor context; Select All is in the Edit menu.

### 3.2 Group transforms
- `StageGroupTransform::ApplyLeaderChange` (pure): followers move by the leader's offset, orbit its pivot by its world rotation, and scale **in place** by its per-axis ratio (clamped by `StageTransformRules`). Results depend only on start transforms, so drags never accumulate error.
- `UStageGroupTransformComponent` (controller) holds start transforms between `BeginGroupEdit` and `EndGroupEdit` only (weak pointers). The controller calls it from the gizmo drag (start / each update after `UpdateDrag` / finish) and around a validated numeric Location / Rotation / Scale edit, so both tools move the group the same way.
- Followers are written directly, as the gizmo writes its target (ADR 0002 precedent): no per-follower GameMode check.
- Snapping ignores the followers as neighbours (`UStageSnappingComponent::BeginMove(Leader, MovingWith)`), because they travel with the leader.

### 3.3 One action, one undo step
- `FStageBatchCommand` (pure) wraps several commands as one step. **All or nothing:** undo runs children newest first, redo oldest first; when a child fails, the children already applied are reverted, and the child's result (`Refused` / `Invalid`) is returned. A failed rollback is logged as an error and the step reported `Invalid`.
- `UStageEditHistoryComponent::DeleteItems` / `ClearStage` / `RecordTransformChanges` record a single command when one item is involved and a batch otherwise ("Delete 3 items", "Clear Stage (12 items)", "Transform 2 items").

### 3.4 Clear Stage safety
- Three layers: the Edit menu's **Clear Stage...** opens a confirmation submenu stating the item count; the step is **undoable** (Ctrl+Z restores everything, through the placement rules); the console form requires the word `confirm`.
- **Scope decision:** Clear removes every registered stage item (`AModularBaseActor`), including those the level was authored with. Level geometry is never an item and is never touched. This keeps Clear, Save and Load consistent (a scene is "every item on the stage").

## 4. Scene files

- **Where state lives.** The stage stays with `UStageSessionSubsystem`, which now also holds the **scene name** and an **edit serial** (incremented on every dirtying edit). `UStageSceneComponent` (controller) runs file operations; `FStageSceneStore` (pure + file I/O, any thread) owns format and validation. The controller's `RequestSaveScene` / `RequestLoadScene` / `RequestDeleteScene` add guards and report refusals on `OnRequestRejected`.
- **Format.** `Saved/StageCraft/Scenes/<Name>.json`, version 1: `format`, `version`, `name`, `level`, `savedAt`, `items[]` with `id` (GUID), `item` (catalog soft path), `label`, `location`, `rotation` (quaternion, exact round trip), `scale`, `state` (base64 of the SaveGame properties, the same bytes undo snapshots hold). JSON was chosen over binary so scenes are readable, diffable and repairable.
- **Trust boundary.** A scene file is untrusted input:
  - refused whole: not JSON, wrong `format`, unknown `version`, more than 10,000 items;
  - dropped per item: invalid/zero GUID, path outside `/Game`, wrong-length or non-finite vectors, |coordinate| > 10 km, zero quaternion, state over 64 KB;
  - repaired: scale clamped, quaternion normalized, duplicate ids replaced by fresh ones;
  - when applied: only items **in the loaded catalog** are spawned, each through `USpawnSystemComponent::RestoreItem`, so the GameMode's placement rules and entitlements decide (deny by default). Refusals are counted silently (`PlacementEvaluator`) and summarised once.
  - A damaged file is reported and left untouched.
- **Async.** Saves capture the stage on the game thread, then write atomically (temp file + move) on a `UE::Tasks` worker; loads read and parse on a worker. Results return through `AsyncTask(GameThread)` to a weakly held component. One operation at a time (`Busy`).
- **Dirty state.** A save marks the session clean only if the edit serial is unchanged since capture, so edits made while the file was written stay unsaved.
- **Loading replaces the stage** (selection cleared, Place mode left), keeps saved instance ids, and **clears the undo history**, whose steps name the previous stage. The File menu asks before discarding unsaved changes; Save As warns before replacing another scene.
- **Rejected:** a separate scene `UWorldSubsystem`. The session subsystem already owned the dirty flag that a save clears; adding the name there avoids a second source of truth.

## 5. Still renders

### 5.1 Pipeline (`UStageRenderSubsystem`, world subsystem, Game/PIE)
1. **Capturing.** A transient `ASceneCapture2D` renders the player camera's `FMinimalViewInfo` (location, rotation, FOV, camera post-process) into a transient `RTF_RGBA8` target at the internal size, every frame for the warm-up (4 frames; 16 with temporal AA; doubled for Cinematic). The capture overrides Lumen GI/reflections (R1), sets TemporalAA (R2), keeps a view state (R3), speeds up auto exposure, and hides the capture actor, the gizmo and the placement preview. Selection/hover overlays are suppressed on items for the job (`AModularBaseActor::SetHighlightSuppressed`).
2. **Watermark.** Optional footer drawn with `UCanvas` into the target (sized from the image height).
3. **Reading.** A render command copies the target to an `FRHIGPUTextureReadback`; job-scoped next-tick timers queue at most one poll command at a time, which maps the buffer only once the GPU fence has passed (R5). Neither thread waits on the GPU. 600-poll timeout.
4. **Encoding.** A `UE::Tasks` worker forces alpha opaque, downsamples supersampled images (`FImageCore::ResizeImageAllocDest`, linear light), compresses PNG (R6), and writes atomically to `Saved/Renders/StageCraft_<Scene>_<yyyyMMdd-HHmmss>_<W>x<H>.png` (unique suffix on collision).
- One job at a time; a second request is refused. A job is cancelled with its world; late results carry an old job serial and are dropped. Nothing ticks or polls while idle.

### 5.2 Settings
- Resolution: 4K UHD 3840x2160, 1080p 1920x1080, Square 2160x2160.
- Anti-aliasing: Off, FXAA, Temporal (TSR/TAA, converged), Supersampled (temporal plus a larger render filtered down).
- Post-processing: Clean (no vignette, grain, fringe, lens flares, bloom), Standard (as the viewport), Cinematic (Lumen final gather / scene lighting / reflection quality 2, AO and SSR quality 100, longer warm-up).
- Watermark on/off.
- Persisted in `UStageCraftUserSettings` (sanitised on load); `UStageRenderSubsystem::SetSettings` is the only writer.

### 5.3 Supersampling budget (measured)
- A supersampled internal image never exceeds **one 4K frame (8.3 MP)**: 2x for 1080p, 1.33x for Square, 1x for 4K (the panel says so).
- Measured on an 8 GB RTX 3060 Ti shared with another open editor: 8.3 MP Cinematic captures ran at ~28 ms per frame; a 20 MP Cinematic capture (the first budget) exhausted video memory and dropped the app to ~1 fps for the whole job. See STATE.md #26.

### 5.4 UI
- `UStageRenderPanel` (code-built, themed, `StageCraft.Panel.Render`, `DA_Panel_Render`, a second tab next to the Inspector in the Default layout): segmented choices (`UStageChoiceButton`), a summary line, RENDER IMAGE, status and Open Folder. It writes only through `SetSettings`, reads back from `OnSettingsChanged`, and follows the job through state/finished delegates.
- Render menu: Render Image, Render Settings..., Open Renders Folder. Status bar: progress and result.

## 6. Consequences and limitations
- Group moves of followers are not checked by the GameMode per follower (same as gizmo moves). Non-transform inspector edits still apply to the primary only.
- Square renders keep the camera's horizontal field of view, so they show more vertically than the viewport.
- Renders show visible banding in wide smooth gradients (beam cones, back wall) in the Square framing, independent of Clean/Standard and supersampling (STATE.md #26). The root cause is not established; candidates are the 8-bit LDR capture path and the capture view's volumetric-fog resolution. Next step: compare with a viewport screenshot at the same framing, then capture HDR (`RTF_RGBA16f`) and quantise with dithering on the worker.
- The capture frames cost GPU time, so the viewport frame rate dips during a render (measured averages 9-28 ms per frame for the presets here); the game thread is never blocked by readback or encoding.
- Scene state bytes use the running build's tagged property format; files are versioned at the wrapper level only.

## 7. Rollback
- Each feature is separable: the render subsystem, panel, tag and `DA_Panel_Render`; the scene component, store and session name/serial; the batch command and group component. Removing the Render panel tab from `MakeDefaultLayout` restores the previous default layout.
