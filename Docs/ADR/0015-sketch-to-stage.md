# ADR 0015: Sketch to Stage (2D drawing window and 3D generation)

- **Status:** accepted. See STATE.md #42.
- **Related:**
  - ADR 0005 §2 and ADR 0007 §4 (spans and drawn paths: the truss lines become `AStageSpanActor` paths).
  - ADR 0003 and ADR 0004 §3.3 (undo history, `FStageBatchCommand`), ADR 0010 (rules decide undo).
  - ADR 0001 §3.1 (panels are data: `UStagePanelDefinition`).
  - Rules.md: Architecture (state by lifetime, request bridge, push not poll), Engineering Standard §21 (no tick), §37 (explicit state), §54 (determinism), §55 (validate at the trust boundary), §60 (one generation in flight).

## 1. Problem

Gevor's milestone (2026-10-21), "Step 2: Drawing Window & Interactive 2D Sketch-to-3D Reconstruction":

1. A dockable **drawing canvas** with tools: freehand line, polygon (stage shape), straight truss line, fixture point markers; stroke smoothing; erase and clear.
2. A **generator subsystem** that turns the strokes into 3D: closed polygons → stage decks / risers with a height; straight and curved lines → truss spans with corner joints; points → fixtures on grid-snapped positions.
3. The window opens from the main menu and a toolbar button. **Generation is undoable** through the existing history.
4. `TObjectPtr`, encapsulation, no per-frame tick, tests, zero warnings, STATE.md.

What exists:
- Truss runs are `AStageSpanActor` paths of whole sections with corner parts (ADR 0007 §4), placed by `USpawnSystemComponent::SpawnConfiguredItem` (the GameMode validates the full segment count).
- There is **no deck item**: no actor can take an arbitrary floor outline. `ProceduralMeshComponent` is already a module dependency (the gizmo uses it).
- Placements are recorded one step per item (`FStagePlaceItemCommand`); multi-item steps use `FStageBatchCommand`.

## 2. Decisions

### 2.1 Layers and ownership

| Piece | Kind | Owns |
|---|---|---|
| `FStageSketchStroke`, `FStageSketchSettings`, `FStageSketchBuildPlan` (`Sketch/StageSketchTypes.h`) | value types | — |
| `StageSketchMath` (`Sketch/StageSketchMath.h`) | pure functions | smoothing, simplification, grid snap, closing, distance to a stroke, stroke sanitizing |
| `StageDeckMath` (`Placement/StageDeckMath.h`) | pure functions | polygon area, orientation, simple-polygon test, ear-clipping triangulation, point in polygon, outline sanitizing, the extruded deck mesh, panel count |
| `StageSketchGenerator::BuildPlan` (`Sketch/StageSketchGenerator.h`) | pure function | strokes + rules → plan (decks, truss paths, fixtures, warnings) |
| `UStageSketchSubsystem` | `UWorldSubsystem` (Game, PIE) | **the sketch** (strokes, settings, chosen catalog items) of this stage session; planning; streaming the plan's items; one generation in flight |
| `StageSketchBuilder::Execute` (`Sketch/StageSketchBuilder.h`) | function | spawns a plan through a player's `USpawnSystemComponent`, **all or nothing** |
| `AModularPlayerController::RequestGenerateFromSketch` | request bridge | guards, asks for the plan and its assets, executes, records one undo step, reports refusals |
| `UStageDeckData`, `AStageDeckActor` | catalog item, placed item | a deck's outline and height (SaveGame) and its procedural mesh |
| `UStageSketchCanvasWidget` (panel) + `UStageSketchSurfaceWidget` (canvas) | views | the tool being used and the stroke being drawn (transient UI state only) |

- The sketch lives in a **world subsystem**, not in the widget: closing and reopening the window, floating it to another monitor or re-docking it keeps the drawing, and console commands and tests reach the same document. It is per stage session (dropped on level change). It is not saved in scene files (follow-up).
- Sketch edits (add, erase, clear, settings) are tool state, not stage state. The panel writes them through the subsystem's validated setters, as the Preferences panel writes the snapping component. Everything that changes the stage goes through the controller.
- The subsystem never iterates the world and never ticks. Views bind `OnSketchChanged` and `OnGenerationFinished`.

### 2.2 Coordinates

Sketch points are stored in **world plan coordinates in cm** (`FVector2D(X, Y)`). The canvas draws them like the viewport's Top projection (`StageViewMath`: pitch −90°, so screen up = +X, screen right = +Y). Strokes are bounded (±10 000 cm, 256 strokes, 512 points per stroke); out-of-range and non-finite points are dropped when the stroke is added (§55).

### 2.3 Strokes and tools

| Tool | Gesture | Stroke | Becomes |
|---|---|---|---|
| Freehand | press, drag, release | points sampled every 4 px, stabilised while drawing (exponential "lazy brush", strength = Smoothing), then simplified (Ramer–Douglas–Peucker, 15 cm) | ends within 75 cm of its start (and ≥ 3 points) → **closed shape → deck**; otherwise a **curved truss line** |
| Stage shape | click corners; click the first corner, double-click or Enter closes | closed polygon | **deck** |
| Truss line | click points; double-click or Enter finishes; a click on the first point closes a loop | polyline | **truss path** (straight legs, corner parts) |
| Fixture | click | one point | **fixture** |
| Erase | click near a stroke (8 px) | — | removes that stroke |

Backspace removes the last clicked point, Esc drops the stroke being drawn. UNDO STROKE removes the last stroke and CLEAR empties the sketch (both buttons, not history steps: the sketch is a scratch pad, the stage is what the history protects). New shapes take the panel's current deck height.

### 2.4 Plan rules (`StageSketchGenerator::BuildPlan`)

Deterministic: the same strokes and rules always give the same plan (§54).

- **Grid.** With snapping on, deck corners and floor fixtures snap to the grid step (25, 50 or 100 cm). Truss points of a whole-section item snap to its **section length** (2 m for F34), so a drawn rectangle has whole sides and closes; stretched items use the grid step.
- **Decks.** Corners are snapped, duplicates and collinear corners removed, and the outline must be a simple polygon (no self-crossing) with at least 0.25 m², else the shape is skipped with a warning. The deck stands on the floor (`FloorHeight`, 0), its origin at the outline's centroid; the height is the stroke's, clamped to the deck item's range.
- **Truss.** Laid out with `StageSpanMath::ComputePathLayout` exactly as the span actor will normalise it, at the truss height (default 6 m). A loop that cannot close, or a path cut by the section cap, is built as laid out and warned about.
- **Fixtures.** A marker within 75 cm of a planned truss is projected onto it (its distance along the leg snapped to the grid) and **hung**: at the truss height, rolled 180°. Otherwise a marker inside a planned deck stands on the **deck top** (the highest deck), otherwise on the floor.
- **Items.** One catalog item per kind, chosen in the panel: a `UStageDeckData`, a `UStageSpanData` whose item type is Truss, and a `ULightingFixtureData`. A missing kind skips its strokes with a warning ("No deck item in the catalog: 2 stage shapes skipped").

### 2.5 Generation request: validate, load, spawn all or nothing, one undo step

1. `UStageSketchCanvasWidget` (GENERATE 3D) → `AModularPlayerController::RequestGenerateFromSketch()`.
2. The controller refuses while a gizmo drag or navigation is in progress, while another generation is loading, or when the plan is empty (the reason, including the plan's warnings, goes to `OnRequestRejected` and the status bar).
3. `UStageSketchSubsystem::RequestPlanAssets` streams the chosen items' Game bundles (`UAssetManager::LoadPrimaryAssets`), keeps the plan pending, and calls back (possibly at once). One generation at a time (§60); the handle is cancelled in `Deinitialize`.
4. `StageSketchBuilder::Execute` spawns each element through `SpawnConfiguredItem`, so `EvaluatePlacement` sees every item with its real quantity (deck panels, truss sections) against the totals of the ones already spawned. **If any element is refused, every element spawned by this request is removed again** and the refusal is reported: the stage is unchanged.
5. On success the controller records **one** history step, `UStageEditHistoryComponent::RecordPlacements` → `FStageBatchCommand` of `FStagePlaceItemCommand`s ("Generate from Sketch (12 items)"). Undo removes all of them; redo restores them through `RestoreItem`, validated by the rules again (a batch is all or nothing, ADR 0004 §3.3).
6. `UStageSketchSubsystem::ReportGeneration` broadcasts the result for the panel's message line; the status bar already shows the recorded step.

The sketch is kept after generating, so a shape can be corrected and generated again (Ctrl+Z first removes the previous attempt).

### 2.6 The deck item

- `UStageDeckData` (catalog): default footprint, default / minimum / maximum height, panel size (quantity unit), surface and skirt materials (Game bundle). Specs (weight, power) are **per panel**; `GetQuantity` = ⌈area ÷ panel area⌉, so session limits and the bill of materials count rental deck panels.
- `AStageDeckActor`: SaveGame `Outline` (actor-local, cm) and `DeckHeight` (cm), so undo snapshots and scene files carry decks with **no scene format change**. Restored values are sanitized in `ApplyItemData` (non-finite or out-of-range points dropped, a self-crossing or too-small outline replaced by the default footprint, height clamped), counted as state repairs (ADR 0013 §3.2).
- A `UProceduralMeshComponent` builds the extruded prism: section 0 the top (triangulated by ear clipping), section 1 the skirt walls (flat-shaded quads, outward normals). No bottom face. Query-only collision on the stage item channel, so decks are selectable and placement surfaces (items can be placed on them). Rebuilt only when the outline, height or item changes.
- Inspector "Deck" section: Height (editable, an undoable shape parameter, refused while locked: `Param_Deck_Height` is in `IsShapeParameter`), Area, Panels and Corners (read-only). Decks do not scale (`SupportsScaling` false). Placing a deck from the Library gives the default footprint.
- Triangle winding follows the engine's own face normal, `(P1 − P2) × (P0 − P2)` (`UKismetProceduralMeshLibrary::CalculateTangentsForMesh`); every triangle is built and then checked against its intended normal, and the tests assert it.

### 2.7 Window and entry points

- Panel tag `StageCraft.Panel.Sketch`, definition `DA_Panel_Sketch` (headless script `Scripts/Content/create_sketch_content.py`), not in the default layout: it opens as **its own pop-up window, like a floating Unreal Editor tool window** (Gevor, 2026-10-21: docking it squeezed the viewport and its toolbar). The shell registers a default window size of 1100 × 820 for it (`FTabManager::RegisterDefaultTabWindowSize`; Slate's fallback is 1000 × 600). Like any panel it can still be docked by dragging its tab, and the layout remembers where it was.
- Entry points: Edit › Sketch to Stage..., the Window menu, and a SKETCH button at the end of the viewport toolbar.
- The canvas is a `UUserWidget` that paints in `NativePaint` (grid, filled deck shapes, truss lines, fixture markers, the stroke being drawn, the snapped cursor) and repaints only on input or `OnSketchChanged`. Wheel zooms about the cursor, right or middle drag pans, Frame resets the view to the strokes.

### 2.8 Alternatives considered

- **Sketch state in the widget.** Lost when the tab closes or the window is re-docked; not reachable by tests or console commands. Rejected.
- **Generator that spawns from the subsystem with its own validation.** A second spawn path would bypass the GameMode or duplicate it. The subsystem plans; the player's spawn system spawns, so the only validation is the existing one.
- **Partial generation** (keep what fits, report the rest). One click would then leave a half-built stage and an undo step that does not match what was asked. All or nothing is simpler to reason about and to undo.
- **Decks from instanced 2 × 1 m panel meshes.** Exact for rectangles only; drawn shapes are arbitrary polygons. The procedural prism fits any simple polygon; the panel count still bills rental decks.
- **Dynamic Mesh / Geometry Script.** More capable, but a new plugin dependency for an extruded polygon. ProceduralMeshComponent is already linked.
- **Interactive 3D preview while sketching.** Useful, but doubles the work of every tool; the plan's counts and warnings in the panel cover the decision before generating. Follow-up.

## 3. Verification

- Pure tests: `StageCraft.Sketch.Math` (smoothing, RDP, snapping, closing, distance, sanitizing), `StageCraft.Sketch.Deck` (area, orientation, self-crossing, triangulation of convex and concave outlines, mesh normals by the engine formula, panel count, outline repair), `StageCraft.Sketch.Plan` (decks, truss snapping to sections and closing, fixtures hung on truss / standing on a deck / on the floor, invalid shapes and missing items as warnings, determinism).
- World test `StageCraft.Sketch.Generate`: a plan spawns decks, a truss path and fixtures through a spawn system; one undo step removes everything; redo restores it; a refusal of the last element leaves the stage empty; a deck's height edit and a restored damaged outline.
- Scripted Standalone run: draw through the console commands, generate, screenshot, undo, redo, save and reload a scene with a deck.

## 4. Consequences

- New placed item type (`AStageDeckActor`), saved through SaveGame properties: scene files stay at format v3.
- The truss height is one value per generation; truss legs are level. Towers, motors and ground supports are not generated.
- The sketch is not saved with the scene, and there is no 3D preview of the plan.
- A fixture on a deck counts as rigged in the cable schedule when the deck top is 2 m or higher (existing rule, ADR 0014).
