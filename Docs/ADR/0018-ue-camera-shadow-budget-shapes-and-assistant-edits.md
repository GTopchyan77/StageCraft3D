# ADR 0018: Unreal Editor camera controls, a collapsible viewport toolbar, a fixture shadow budget, procedural stage shapes and the assistant's targeted edits

- **Status:** accepted. See STATE.md #45.
- **Related:**
  - ADR 0008 (viewport toolbar, projections and view modes): §3 adds a collapsed state to that toolbar.
  - ADR 0012 (fixture benchmark): the measurement §4 is checked against.
  - ADR 0013 (key bindings and preferences): two new commands, camera speed and the look link are stored there.
  - ADR 0015 (Sketch to Stage: decks, the build plan, one undo step) and ADR 0016 / 0017 (the assistant and its Armenian grammar): §5 and §6 extend them; the trust model does not change.
  - Rules.md: Architecture (state by lifetime, the request bridge, push not poll), Engineering Standard §21 (no tick polling), §22 (measure before claiming), §55 (validate at the trust boundary), §56 (undo).

## 1. Problem

Gevor's phase (2026-10-22):

1. **Viewport navigation and frame rate.** The viewport camera must use authentic Unreal Editor 5 mouse controls, speeds and smooth up/down movement. There must be no frame drops during crossfades, with many fixtures, or while editing the scene.
2. **Collapsible toolbar.** The viewport toolbar (Perspective, Lit, Snap, Angle, Move / Rotate / Scale, Sketch, Assist) can be collapsed and expanded.
3. **Contextual AI commands.** `UStageAICommandSubsystem` deletes, moves and replaces selected or named items (fixtures, decks, truss), in Armenian and English.
4. **Runtime procedural shapes.** The assistant builds circular risers, custom stairs and curved truss arcs from a description.
5. `TObjectPtr`, encapsulation, no per-frame Tick polling, all tests, zero warnings, STATE.md.

Gevor's choices, asked before design:

| Question | Choice |
|---|---|
| Frame rate | **Shadow budget**: only the N most important fixtures cast beam shadows (default 64, Project Settings), plus the CPU fixes |
| Curved truss | **Procedural curved truss**: an item with chords and bracing along the true curve, undoable, saved, counted in curved sections |
| Stairs | **Procedural stairs item**: width, rise and step count, optional handrails |
| Mouse wheel | **As in UE**: the wheel alone dollies 96 cm per notch; RMB + wheel changes the camera speed by 10 %; the speed is saved (replaces STATE.md #14) |
| Look speed | **As in UE**: 0.2° per mouse count × the look sensitivity; the STATE.md #15 link to the camera speed stays as a Preferences option, off |

Facts found first:

- **The editor's numbers** come from the UE 5.8 source (`EditorViewportClient.cpp`, `CameraController.h`, `EditorViewportSettings.h`, `BaseEditorPerProjectUserSettings.ini`).
  - The camera speed is one continuous value; RMB + wheel steps it by ±10 %.
  - Flight at speed S settles at 2000·S cm/s (acceleration 20000·S, damping 10).
  - Drags move 1 cm per count × S and turn 0.2° per count.
  - The wheel dollies 96 cm per notch.
  - F frames the bounds' sphere (minimum radius 10 cm, radius × aspect on wide screens, distance r / tan(FOV/2)) with a 0.25 s cubic-out glide.
- **What a press does is decided per press.** The viewport calls the client's `InputKey` first, then captures the mouse and asks `HideCursorDuringCapture` on every press, so whether the cursor hides can follow the buttons held.
- **Moving shadowed spot lights are the render-thread cost** with many fixtures (STATE.md #39 benchmark): each moved shadow-casting light re-renders its shadow depths every frame.
- **Item traces are simple-collision queries** (`GetHitResultAtScreenPosition(..., bTraceComplex = false)`), so a procedural mesh is selectable only with simple collision.

## 2. Camera: the Unreal Editor's controls

### 2.1 Rules as pure functions

`StageCameraNavigation` holds the editor's rules as functions of plain values; the tests cover them without a viewport.

- `ClassifyDrag(FStageCameraButtons)` maps the buttons held to an `EStageCameraDrag`:

  | Input | Drag |
  |---|---|
  | LMB | forward along the yaw, and yaw |
  | RMB | look (WASDQE fly while held) |
  | MMB | pan in the view plane |
  | LMB + RMB | pan sideways and straight up / down |
  | Alt + LMB | orbit |
  | Alt + RMB | dolly to the pivot |
  | Alt + MMB | pan with the pivot |

- Also: speed steps and limits (0.01..32), the fly speed, each drag's translation or rotation, the wheel dolly, the focus location and the easing.

### 2.2 One component, one source of truth per piece of state

- **`UStageCameraNavigationComponent`** (on the controller) owns the input state.
  - It holds the buttons held, the pending left click and the active drag.
  - It turns mouse deltas and the wheel into pawn calls.
  - It finds the orbit pivot: where F last framed, else what is under the view centre (a visibility trace of 1 km), else 10 m ahead.
  - It tells the viewport client whether a press hides the cursor (`FStageCursorCapturePolicy`).
- **`AStageCameraPawn`** owns the camera's motion: speed, translation, orbit, flight, the F glide and the fixed views. Its tick runs only during a flight or the F glide, then turns itself off, so a still camera costs nothing per frame (§2.6).
- **`UStageCraftGameViewportClient`** tracks L / M / R and Alt as the viewport saw them, and broadcasts `OnMouseButtonsLost` when focus goes, so no drag sticks.

### 2.3 A left press is a click until it moves

A left press over the stage could be a selection click or the start of an LMB camera drag. It stays pending until it travels 3 px; then it becomes a drag and applies the full travel. Released before that, it selects.

It is a click immediately in these cases:

- Place mode, Ctrl / Shift, a gizmo handle, the orthographic views, or Alt (orbit).
- An RMB or MMB press cancels a pending click.

### 2.4 Saved per user

The camera speed (saved when the buttons are released, not per notch) and the look link are fields of `UStageCraftUserSettings`, read and written through `UStageInputPreferencesSubsystem`. Two key-map commands are new:

- **Focus Selection** (F; in the orthographic views it centres and fits).
- **Show / Hide Viewport Toolbar** (Ctrl+Shift+T).

### 2.5 No gizmo tick

`AModularTransformGizmo` no longer ticks. Its on-screen size follows the view through `SetView(FStageGizmoView)` on camera, view and selection changes, and it follows its target through the target's transform-updated event.

### 2.6 Amendment (STATE.md #47): raw mouse counts, no easing, the editor's flight

Measured with `StageCraft.View.MouseDrag` (a real drag through Slate's raw-mouse path), the camera after #45 differed from the editor in three ways. This amendment replaces the parts of §1 and §2.2 that say otherwise.

- **Every drag ran at 7 %.**
  - The project's `Config/DefaultInput.ini` kept the engine's mouse axis sensitivity of 0.07 for `MouseX`, `MouseY` and `Mouse2D`. Enhanced Input applies it to `Mouse2D`, so a 100-count drag turned the view 1.4° instead of 20°, and the orthographic pan lagged the cursor.
  - The sensitivity is now 1. `StageCraft.Camera.InputConfig` fails if it changes again.
- **Alt + RMB dolly was reversed vertically.** The editor's `IsOrbitZoomMode` moves closer by X − Y (right or down closer, up away); #45 used X + Y.
- **The pawn eased what the editor applies at once.**
  - Look, drags, the wheel dolly and orbit now change the view the moment the input arrives. The editor's rotation physics are off by default (`bUsePhysicsBasedRotation`).
  - The view rotation's only source is the controller's control rotation.
  - Flight runs the editor's own step: `v += impulse · 20000 · S · dt`, then `v −= v · min(10 · dt, 0.75)`, with dt at most 1 s. Impulses are not normalised. Like the editor's, it depends on the frame rate: at 60 fps it settles at 1667 · S cm/s and coasts 139 · S cm after release, and at 120 fps 1833 · S cm/s and 168 · S cm, which is what the scripted run measured.
  - The pawn ticks only during a flight or the F glide.
- **The mouse direction matches the editor's defaults and is unchanged.** Mouse up looks up, LMB up flies forward, MMB and LMB+RMB up move up, and Alt+LMB up raises the pitch (`bInvertMouseLookYAxis`, `bInvertOrbitYAxis`, `bInvertMiddleMousePan` and `bInvertRightMouseDollyYAxis` are all off). This is measured.

## 3. Collapsible viewport toolbar

A « segment at the left of `UStageViewportToolbar` collapses it to a 14 px strip, "» VIEWPORT TOOLS", and a click (or Ctrl+Shift+T) brings it back.

- The state is `UStageViewportComponent::IsToolbarCollapsed` / `SetToolbarCollapsed`, saved in the user settings and broadcast on `OnViewChanged`. The toolbar re-reads it, so the menu, the key and the button never disagree.

## 4. Fixture shadow budget

`StageShadowBudget::SelectCasters` picks which fixtures cast beam shadows.

- **Importance** is the flux at the dimmer over the squared distance to the viewer, in metres, with anything closer than 1 m counted as 1 m.
- The **Budget** most important cast. Dark fixtures never do.
- **Hysteresis:** a current caster counts 25 % brighter, so beams do not flicker in and out as a fade crosses another.
- Ties keep the registration order.

`UShowControlSubsystem` re-ranks event-driven, never per frame.

- These **mark the ranking dirty:** a fixture registering or unregistering, an intensity output change, and a camera move (through the controller's camera-transform event).
- A one-shot timer (0.25 s, coalescing) re-ranks, and only fixtures whose choice changed get `SetCastShadows`.
- The settings are in **Project Settings › Game › StageCraft Performance** (`UStageShowPerformanceSettings`): on / off, the budget (0..2000, default 64), the interval and the hysteresis. Off is the previous look and cost.

## 5. Procedural stage shapes

`StageProceduralShapes` is pure, deterministic geometry with no world and no UObjects; the StageCraft.Shapes tests cover it.

### 5.1 Round and curved risers are decks

`MakeCircle` and `MakeArcRing` produce outlines for `AStageDeckActor` / `DA_Deck_Riser`, so these risers need no new item, save format or BOM rule. They are counted in deck panels.

- A circle has 16..96 corners, enough that every chord stays within 1 cm of the curve.
- An arc ring is limited to 359°, because a full ring is not a simple polygon.

### 5.2 Stairs: `AStageStairsActor` / `UStageStairsData` (`DA_Stairs_Stage`)

- **Shape:** equal steps no taller than the item's limit (20 cm) on 30 cm treads, climbing toward the actor's +X, centred, standing on the floor. Optional handrails on posts at the first and last tread, 90 cm above the nosing line.
- **State:** three SaveGame values (rise, width, handrails). They are sanitized against the item when it is applied; damaged values are clamped or defaulted and counted with `ReportStateRepair`.
- **Inspector:** rise, width and handrails are undoable shape parameters. Steps and step rise are read-only.
- **Counting:** one item. The stairs do not scale.
- **Collision:** complex as simple, so traces hit the solid steps.

### 5.3 Curved truss: `AStageArcTrussActor` / `UStageArcTrussData` (`DA_Truss_Arc_F34`)

- **Geometry:**
  - The apex of the arc on the centre line is the actor origin, its tangent is +X, and the arc bends toward -Y around (0, -R).
  - The four chord tubes (8 sides) are swept along the true curve, sampled at least every 3°. The chord centre lines sit one chord radius inside the outside profile width (F34: 29 cm).
  - Every bay (about one profile width) has rungs around the profile and a zigzag diagonal per face.
  - Open arcs get end caps.
- **State:** two SaveGame values (radius, angle), sanitized like the stairs.
- **Quantity:** ⌈arc length ÷ section length⌉ rental sections (2 m).
  - `PredictQuantity` answers for radius and angle edits, so the session's limits judge an edit before it applies. Both are quantity parameters.
  - Specs and the bill of materials count per section.
- **Collision:** one 12-point convex hull per rental section around the profile (`SetCollisionConvexMeshes`); the tubes are too thin to aim at.

### 5.4 The build plan carries them

`FStageSketchBuildPlan` gains `Stairs` and `ArcTrusses` (with their items). `StageSketchBuilder::Execute` checks and spawns them like decks and truss: all or nothing, one undo step through `RequestBuildPlan`. A missing item refuses the build before anything spawns.

## 6. The assistant's targeted edits and shapes

### 6.1 Four new intents

The commands stay a closed set; each new intent is validated by `StageAICommandSchema::Validate` and runs only through controller requests.

| Intent | Fields | Runs through |
|---|---|---|
| `delete_items` | target, item, count | `RequestDeleteItems`: one step, like the Delete key |
| `move_items` | target, item, count, direction, distance_cm (1 cm..50 m, default 1 m) | `RequestMoveItems` |
| `replace_items` | target, item, count, replacement | `RequestReplaceItems` |
| `build_shape` | shape (round_riser, arc_riser, stairs, arc_truss), radius_cm, width_cm, height_cm, angle_degrees, handrails | `RequestBuildPlan` |

- **`RequestMoveItems`:** every new transform is checked by `EvaluateTransformChange` first (a locked item refuses the whole move), then written as a Location edit and recorded as one step.
- **`RequestReplaceItems`:** the old items are removed first, so the limits count the stage without them. Each new item is checked by the placement rules and spawned. On any refusal the new items go and the old ones return with their ids. One step (`RecordReplacement`) undoes it: delete commands, then place commands, in one batch.
- **Shape sizes:** `build_shape` sizes are clamped to the item's ranges. The shape goes in front of (towards -X) everything placed, and the curved truss hangs at 6 m unless told otherwise.

### 6.2 Targets

- **`selection`** is the asking player's selected items. The controller passes their names into `SubmitPrompt`, so the online model's scene summary has a "Selected:" line. A query narrows it: "the selected spots".
- **`named`** is the placed items whose catalog name contains every word of `item`, singular and case-insensitive. Generic words ("lights", "fixtures") name every fixture. Optional `count`: the latest placed first. At most 200 items per command.
- **Clearing the whole stage stays explained, not run:** "delete everything", "clear the stage" and deleting cues remain `unsupported` and point to Edit › Clear Stage or the Cues panel.

### 6.3 Directions

Left, right, forward and back are **as the camera sees the stage**, snapped to the nearest stage axis, so items stay on their grid. Up and down are world Z.

### 6.4 Grammar (English, and Armenian through the same rules)

- **Sizes:** "3 m", "60cm", "2.5 metres", "90°" / "90 degrees", feet.
  - A size before "wide", "high" or "radius", or after "width", "height" or "radius of", is that size.
  - An unlabelled size is the round riser's or truss circle's **diameter**, as risers are sold.
  - A bare number up to 20 is metres; above it, centimetres.
- **Guards against misreading:**
  - "remove / clear the fog" still clears the weather.
  - "clear the selection" never deletes.
  - Fixtures "in a circle" are fixtures, not a shape.
  - A real question ("how many …?") is not an edit; a polite one ("can you delete …?") is.
- **Armenian lexicon additions:**
  - Verbs: տեղափոխ / տեղաշարժ (move), բարձրացր / իջեցր (raise / lower), փոխարին (replace), կառուց (build).
  - Selection: ընտրված (selected).
  - Directions: ձախ, աջ, առաջ, վեր, ներքև.
  - Shapes: կլոր, շրջանաձև, աղեղ, կիսաշրջան, աստիճան / սանդուղք, բազրիք.
  - Sizes: շառավիղ, տրամագիծ, լայնություն, բարձրություն.
  - Units: մ, սմ, մմ, մետր.
- **Two Armenian rules:**
  - The replacement is the noun in the instrumental case (-ով, "with"): "փոխարինիր փարերը վոշերով" reads "replace par with wash".
  - աստիճան (step / degree) after a number in a curved request is degrees.

## 7. Alternatives considered

- **A frame-time governor (dynamic resolution, lower shadow quality).** Rejected by Gevor in favour of a direct, explainable rule; dynamic resolution also blurs the plot.
- **Shadows off for every fixture.** Cheapest, but beams through truss and set pieces matter in a stage visualiser; the budget keeps them where they are seen.
- **Curved truss as segmented straight pieces.** Rejected: rental curved truss is bent, the plot and the bill of materials must say so.
- **Stairs as decks.** A stepped outline cannot carry treads and rails; a separate item keeps decks simple.
- **Moving through the selection (select the targets, then a group Location edit).** Rejected: it changes what the user has selected as a side effect; `RequestMoveItems` takes the items directly.
- **Camera-independent directions (world ±X / ±Y).** Rejected: "move it left" means left on the screen.

## 8. Consequences

- **New items:** `DA_Stairs_Stage` and `DA_Truss_Arc_F34`, plus the materials `M_Handrail` and `M_TrussArc` (`Scripts/Content/create_procedural_shapes_content.py`). New gameplay tags: the Stairs / ArcTruss feature groups, their parameters, and `StageCraft.Category.Truss.Curved`.
- **Save data:** new actors with their own SaveGame fields; no existing format changed. Old scenes load unchanged.
- **New per-user settings:** camera speed, look link, toolbar collapsed. New project settings: StageCraft Performance.
- **The wheel** no longer changes the fly speed on its own (STATE.md #14 is superseded); RMB + wheel does.
- **Fixtures beyond the budget** light the stage without beam shadows.
- **Known limits:**
  - Replacing a fixture does not carry its DMX patch over.
  - The curved riser's outline is limited to 359°.
  - A shape is always placed in front of the stage at yaw 0; the user moves or rotates it after.

## 9. Rollback

Each part is separable:

- The camera component and the pawn (the old FlySpeed API is gone, so reverting means restoring both).
- The toolbar segment and its setting.
- The shadow budget: set "Limit Fixture Shadows" off; no code change is needed.
- The shape actors and items: removing them leaves decks and truss untouched; scenes that hold them would report missing items.
- The four intents: remove them from the enum, the schema and the runner.
