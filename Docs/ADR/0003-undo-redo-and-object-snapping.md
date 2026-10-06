# ADR 0003 — Undo/redo command history and object-to-object snapping

- **Status:** accepted, implemented. See STATE.md #25.
- **Related:** ADR 0002 (placement tool, gizmo, transform rules). Rules.md "Where state lives", "Economy security standards" (deny by default), Engineering Standard §16, §21, §36, §60.

## 1. Problem

Gevor asked for two editor features:
- **Undo / redo.** A command history (an `ICommand`-style interface and history stacks) for Place, Transform (move, rotate, scale) and Delete, bound to Ctrl+Z, and to Ctrl+Y or Ctrl+Shift+Z.
- **Object snapping.** While placing or moving, an item should lock onto nearby placed items by their bounding boxes: side by side, stacked, edges aligned. Show alignment guides.

What existed before:
- Places went through `UStagePlacementToolComponent::TryPlace`, deletes through `USpawnSystemComponent::TryDeleteActor`, gizmo drags set the actor transform per frame, and inspector edits went through `AModularPlayerController::RequestParameterChange`.
- None of these was recorded. A deleted actor was simply gone, so no pointer to it could be used again.
- Snapping existed only as the item's placement grid (`FStageItemPlacementRules::GridSize`) and an unused gizmo step (`TranslationSnap`).

## 2. Verified engine facts

| # | Fact | Source | Consequence |
|---|---|---|---|
| U1 | `UInputTriggerChordAction` is an implicit trigger. The action fires only while `ChordAction` is triggering. | `InputTriggers.h:490-514` | Ctrl+Z is a Z mapping with a Ctrl chord; the modifiers are their own input actions. |
| U2 | For each chorded mapping, Enhanced Input adds a `UInputTriggerChordBlocker` to every **later** mapping of the same key. It blocks while the chorded action triggers. | `EnhancedInputSubsystemInterface.cpp:579-612` (`InjectChordBlockers`) | Mapping Ctrl+Shift+Z before Ctrl+Z means Ctrl+Shift+Z only redoes. |
| U3 | `UObject::SerializeScriptProperties` with `ArIsSaveGame` reads and writes only `SaveGame` properties. `AActor::Serialize` does extra editor and component work. | `Object.h:1131`, `Actor.cpp:926` | Snapshots use `SerializeScriptProperties`, not `Serialize`. |
| U4 | `AActor::GetComponentsBoundingBox(false)` returns the bounds of colliding components only. Attached actors (the gizmo) are not included. | `Actor.h` | Item bounds are what the cursor can hit, never the gizmo or a beam light. |

## 3. Decisions

### 3.1 Command model: record after doing

- Each tool performs its action through its existing validated path. Only after it succeeds is a command recorded, and the command knows only how to revert and re-apply the action.
  - There is no second `Execute` path, so the rules are never bypassed and every tool stays as it was.
- **`IStageEditCommand`** (`GetDescription`, `Undo`, `Redo`) has three implementations: `FStagePlaceItemCommand`, `FStageDeleteItemCommand` and `FStageTransformItemCommand`.
  - Commands are plain C++ held as `TSharedRef`, so they are non-UObject and hold no UObject pointers.
- **`IStageItemEditor`** is the only thing a command can act on: `CaptureItem`, `RestoreItem`, `RemoveItem` and `SetItemTransform`.
  - The live implementation is a short-lived adapter inside `UStageEditHistoryComponent`. Tests use an in-memory fake.
  - So every command and the stack are tested without a world.
- **`EStageCommandResult`** has four values:
  - `Succeeded` moves the command to the other stack.
  - `Refused` (the rules said no, and the validator reported it) leaves both stacks unchanged, so the user can retry.
  - `Invalid` (the item or its catalog asset is gone) drops the command.
  - `NothingToDo` means the stack is empty.

### 3.2 Item identity

- Commands name items by `FGuid` (`AModularBaseActor::GetInstanceId`), never by pointer.
  - Undoing a delete spawns a new actor with the old id (`AssignInstanceId`, valid only before BeginPlay). Older transform steps then still find the item.
- The id is `Transient`, assigned at BeginPlay. The history lasts one session, so the id is not saved.
- `UStageSessionSubsystem::FindItemById` searches the existing registry linearly, so there is no second map to keep in sync.
- `RegisterItem` ensures ids are unique.
- `FStageItemSnapshot` holds the id, a soft catalog reference, the transform, the `SaveGame` properties as bytes and the display name.
  - Restoring applies the bytes **before** `InitializeFromItemData`. `ApplyItemData` and BeginPlay then see the restored label, fixture ID, patch and attributes, and the fixture keeps its ID.
  - Place and Delete re-capture the snapshot just before each removal, so edits made after placing survive undo and redo.

### 3.3 Where state lives

- **`UStageEditHistorySubsystem` (`UWorldSubsystem`, Game and PIE only)** owns the stacks (`FStageCommandHistory`, depth 100) and `OnHistoryChanged`.
  - It is per world, because the history describes one stage session and must die with the level whose items it names.
- **`UStageEditHistoryComponent` (on the controller)** connects one player's tools to the history:
  - It records `OnItemPlaced`.
  - It provides `DeleteItem` (the Delete key's path; it releases the selection first) and `RecordTransformChange`.
  - It runs Undo and Redo through the live adapter.
- **The controller is the request bridge.**
  - `RequestUndo` / `RequestRedo` (keys, Edit menu, console) refuse while a gizmo drag or camera navigation is active.
  - `Invalid` is reported on `OnRequestRejected`.
  - The controller also requests a ghost refresh.
- **Restores go through `USpawnSystemComponent::RestoreItem`**, which shares one spawn routine with `SpawnItem`, including `PlacementValidator`. Undoing a delete past the session item limit is therefore refused and reported, as a placement would be (deny by default).
- **Recording points.** Each user action is exactly one step:
  - a placement click;
  - a Delete;
  - a whole gizmo drag, recorded once from the new `AModularTransformGizmo::OnDragFinished`, which fires once per drag including drags cut short;
  - one accepted Location, Rotation or Scale edit in `RequestParameterChange`, with the clamped value read back.
- Undo and redo never pass through those points, so they cannot record themselves. `FStageCommandHistory` also ignores, with an ensure, any `Record` made while a step is running.

### 3.4 Shortcuts

- The default editor mapping context gains modifier actions for Ctrl (left and right) and Shift. The mappings are:
  - Z with chords Ctrl and Shift → Redo;
  - Z with chord Ctrl → Undo;
  - Y with chord Ctrl → Redo.
- The order follows U2.
- A focused text field (inspector, Library search) consumes these keys first, so typing keeps its own text undo.
- `UndoAction` and `RedoAction` are assignable like the other input slots. If one is empty, the built-in mapping is used.

### 3.5 Object snapping

- **`StageSnapMath` (pure).** For each axis allowed by the mask, the moving AABB snaps to the closest neighbour candidate within `SnapDistance` (20 cm).
  - The candidates are flush after, flush before, align min, align max and align centre, in that priority order.
  - A neighbour counts on an axis only if the boxes overlap, or are within the threshold, on both other axes.
  - Ties keep the first neighbour and the first anchor, so the result is deterministic for the session's registry order.
  - The module also produces guide segments.
- **`UStageSnappingComponent` (on the controller)** supplies the boxes, the user preference and the guide thickness (from camera distance). It also raises `OnSnapEngaged`, which plays the existing Snap cue.
  - Consumers stay unaware of snapping and receive it as delegates, as with the existing `PlacementValidator`:
    - **Placement** (`UStagePlacementToolComponent::PlacementSnapper`) measures the X and Y snap at the free (no-grid) landing transform. Snapped axes win; the others keep the grid. Z stays with the surface trace.
      - The preview and `TryPlace` share one `ComputeLandingTransform`, so the ghost is still exactly where the item lands.
      - Items with no `Mesh` in their data cannot be measured before they exist, so they snap to the grid only.
    - **Gizmo Move** (`AModularTransformGizmo::TranslationSnapper`) snaps along the dragged axis only. Z works too, so an item can be stacked by dragging it up.
      - The neighbour boxes are cached once in `BeginMove` (from `OnDragStarted`) and dropped in `EndMove`. The scene is static during a drag, so per-frame drag updates do no searching.
- **Guides.** `UStageSnapGuidesComponent` draws up to three world-space line meshes with the always-on-top gizmo material in magenta. One instance is on the preview and one on the gizmo, so both look the same. It has no tick and no collision.
- **Preference.** "Snap to Items" (Edit menu, `StageCraft.Edit.Snap`) is stored in `UStageCraftUserSettings` and saved when toggled. The snapping component is its only writer. It is on by default.

## 4. Alternatives rejected

- **Commands with `Execute()` that perform the action.** That duplicates every tool's validated path inside the commands, or routes tools through the history. Recording after doing keeps one write path per action.
- **Actor pointers or `TWeakObjectPtr` in commands.** These break on delete and undo-delete, because the restored actor is a new object.
- **Hiding deleted actors instead of destroying them, so undo can unhide them.** Hidden items would still count in the session stats and registries, fixture IDs and DMX patches, and they would need a second "exists" state everywhere (§36).
- **A history inside `UStageSessionSubsystem`.** That puts two responsibilities in one class. The session keeps registry, stats and rules; history is its own subsystem.
- **Automatic recording by listening to `OnItemSpawned` / `OnItemDeleted`.** Undo and redo spawn and delete too, so a "suppress while applying" flag would be needed. Explicit recording points have no feedback loop.
- **Rebuilding the snapping neighbour list from the world** (`TActorIterator`). Rules forbid world searches in tools; the session registry is the source of truth.
- **Debug-draw guides** (`DrawDebugLine`). They are development-only and do not ship.

## 5. Consequences

- **History scope.** Non-transform parameter edits (label, patch, attributes, Type swap) are not undoable steps. They do survive undo and redo of Place and Delete through the snapshot. Adding them later is one more command type at `RequestParameterChange`.
- **Fixture IDs.** A restored fixture keeps its fixture ID and patch, which assumes nothing took them while it was deleted. `UShowControlSubsystem` handles conflicts as it does for any registration.
- **Placement snapping and meshes.** Placement snapping needs the item's `Mesh`. Fixtures whose look comes only from a Blueprint snap to the grid while placing, but do snap when moved, because a placed actor has bounds.
- **No snap bypass key.** There is no hold-to-disable key yet; the Edit menu toggle is the only switch.
- **History length.** The history is per level and capped at 100 steps. Level travel clears it.

## 6. Rollback

- **Snapping:** unbind the two delegates in `AModularPlayerController::BeginPlay`. Placement falls back to the grid and the gizmo to its own step.
- **Undo:** remove the `UndoAction` / `RedoAction` bindings and the Edit menu entries. Recording is harmless without them.
