# STATE.md — StageCraft 3D: Concert & Event Designer

Single source of truth for project status. Updated after every completed task.

Engine: UE 5.8 · Module: `ModularSceneBuilder` (Runtime) · Roadmap: 5 phases (Data core → Interfaces/Base actor → Controller & spawning → Gizmo → UI & color picker).

---

## #1 — Phase 1: Data-Driven Core (2026-09-29)

**What & why**
Built the data layer so props, stage elements and lights are pure content. You add or remove items by creating or deleting data assets, with no C++ changes.

- `UBaseItemData` (`UPrimaryDataAsset`) is the catalog entry. It holds DisplayName, Description, Icon, CategoryTag (GameplayTag under `StageCraft.Category`), ItemType (`Prop` / `StageElement` / `Light`), ActorClass (soft class to spawn), an optional Mesh override, and `FStageItemPlacementRules` (Single vs Continuous placement, grid size, offset, align-to-normal). Phase 3's spawn system reads these rules.
  - Every heavy reference is soft and tagged with an asset bundle. The catalog loads only `"UI"` (icons). Selecting an item streams its `"Game"` bundle (mesh and class).
  - `GetPrimaryAssetId()` returns type `StageItem`, so the Asset Manager discovers every asset under `/Game/StageCraft/Items` (config in `DefaultGame.ini`).
  - Editor data validation: a missing ActorClass is an error. An empty DisplayName, a missing CategoryTag, or a Light set to Continuous placement is a warning. Changing ItemType to Light sets placement to Single automatically.
- `EStageItemType` is an enum because systems branch on it (for example, lights get the color picker). Open-ended grouping uses GameplayTags, so designers add categories in Project Settings without code. Native tags live in `StageCraftTags::` (Stage, Truss, Audio, Screen, Lighting, Prop).
- `UStageItemSubsystem` (`UGameInstanceSubsystem`) owns the catalog and the current selection.
  - After the asset registry's initial scan, it async-loads all StageItem assets with the UI bundle, sorts them by name, and fires `OnCatalogLoaded`.
  - `SelectItem()` streams the Game bundle first and fires `OnSelectedItemChanged(New, Previous)` only when the item is ready to spawn. A newer request overrides one that is still loading.
  - It has no polling and no actor iteration. Consumers bind to the delegates.
  - It is a subsystem rather than a `UGameInstance` subclass, so it works with any GameInstance and keeps a single responsibility.
- Added a `LogStageCraft` log category and moved module sources into `Public/` and `Private/`.

**Files changed**
- `Source/ModularSceneBuilder/ModularSceneBuilder.Build.cs`: added the `GameplayTags` dependency
- `Source/ModularSceneBuilder/Public/ModularSceneBuilder.h` (moved from module root): LogStageCraft declaration
- `Source/ModularSceneBuilder/Private/ModularSceneBuilder.cpp` (moved from module root): LogStageCraft definition
- `Source/ModularSceneBuilder/Public/Data/StageItemTypes.h`, `Private/Data/StageItemTypes.cpp`: new
- `Source/ModularSceneBuilder/Public/Data/BaseItemData.h`, `Private/Data/BaseItemData.cpp`: new
- `Source/ModularSceneBuilder/Public/Subsystems/StageItemSubsystem.h`, `Private/Subsystems/StageItemSubsystem.cpp`: new
- `Config/DefaultGame.ini`: added the `StageItem` entry to `PrimaryAssetTypesToScan`
- `Content/StageCraft/Items/`: folder created, empty (git does not track empty folders)

**Verification**
- Compiled clean with `Build.bat ModularSceneBuilderEditor Win64 Development` on UE 5.8 (0 errors, 0 warnings). Done by Claude.
- Runtime behavior is implemented and awaiting manual verification. To test: create one or two `BaseItemData` assets in `/Game/StageCraft/Items`, run PIE, and check the Output Log for `Stage item catalog loaded: N items.`

**Commit:** uncommitted (working tree)

**Known issues / follow-ups**
- `ActorClass` is `TSoftClassPtr<AActor>`. Narrow it to `AModularBaseActor` in Phase 2.
- Game-bundle assets stay loaded after you switch selection. This is intentional: placed actors hard-reference them anyway. Revisit if the catalog grows large.
- No Obsidian session log was written: the vault path `S:\OBSIDIAN VAULTS\...` was not accessible from this machine.

---

## #2 — Phase 2: Interaction interface & base actor (2026-09-29)

**What & why**
Added the interaction contract and the parent class for every placed stage item. Input code (Phase 3) and the gizmo (Phase 4) will talk only to the interface and delegates, never to concrete actor types.

- `IInteractableInterface` (`UInteractableInterface`): `OnHoverBegin`, `OnHoverEnd`, `OnSelect`, `OnDeselect`, `GetInteractionDetails`.
  - All are BlueprintNativeEvents, so Blueprint-only actors can implement them. C++ callers use `IInteractableInterface::Execute_*`.
  - Implementations must be idempotent.
  - `GetInteractionDetails` returns `FStageItemInteractionDetails` (ItemData, DisplayName, ItemType, `bSupportsColorEditing`, Color). The Phase 5 color picker reads it; light subclasses opt in.
- `AModularBaseActor` (AActor + IInteractableInterface):
  - **Component:** a `UStaticMeshComponent` root. It is Movable, uses BlockAllDynamic as QueryOnly (traceable, no physics), and has overlap events off.
  - **Item data:** a `TObjectPtr<UBaseItemData> ItemData` link, exposed on spawn and editable on level-placed instances.
  - **Initialization:** `virtual InitializeFromItemData(UBaseItemData*)` stores the data, calls `virtual ApplyItemData()` (mesh plus per-slot material overrides, only when the data supplies a Mesh, so Blueprint-authored visuals stay untouched), then fires the Blueprint event `BP_OnItemDataApplied`. `OnConstruction` re-applies the data for editor-placed instances.
  - **Highlight:** hover and selection feedback use `SetOverlayMaterial` with `HoverOverlayMaterial` / `SelectedOverlayMaterial` (set in class defaults). It updates only when state changes; the actor has no tick.
  - **Selection event:** the `OnSelectionChanged(Actor, bSelected)` delegate fires on select and deselect. It also fires with `false` from `EndPlay` when a selected actor is destroyed, so the gizmo and details panel release it without polling.
- `UBaseItemData` changes (follow-up from #1):
  - `ActorClass` is now `TSoftClassPtr<AModularBaseActor>` and defaults to `AModularBaseActor`, so simple props need no Blueprint.
  - Added `MaterialOverrides` (soft, in the Game bundle).
  - New validation error: the plain base actor with no Mesh would spawn invisible.

**Files changed**
- `Source/ModularSceneBuilder/Public/Interaction/InteractableInterface.h`: new (header-only)
- `Source/ModularSceneBuilder/Public/Actors/ModularBaseActor.h`, `Private/Actors/ModularBaseActor.cpp`: new
- `Source/ModularSceneBuilder/Public/Data/BaseItemData.h`, `Private/Data/BaseItemData.cpp`: ActorClass type and default, MaterialOverrides, validation

**Verification**
- Compiled clean with `Build.bat ModularSceneBuilderEditor Win64 Development` on UE 5.8 (0 errors, 0 warnings). Done by Claude.
- Runtime behavior is implemented and awaiting manual verification. To test: create a `BaseItemData` with a Mesh, drop an `AModularBaseActor` into a level, and set its ItemData. The mesh should appear. Assign overlay materials in a Blueprint subclass and call `OnHoverBegin` / `OnSelect` from a test Blueprint to check the highlight.

**Commit:** uncommitted (working tree, on top of the uncommitted #1)

**Known issues / follow-ups**
- No overlay materials ship yet. The highlight shows nothing until `HoverOverlayMaterial` / `SelectedOverlayMaterial` are set (for example in a `BP_ModularBaseActor` defaults class). This is a content task.
- Phase 3 should add a dedicated trace channel (for example `StageItem`) so cursor traces don't depend on BlockAllDynamic/Visibility.

---

## #3 — Phase 3: Player controller, spawning & deletion (2026-09-29)

**What & why**
Users can now place and delete stage items with the mouse. Responsibilities are split. The controller owns input and cursor traces only. `USpawnSystemComponent` owns spawn and delete logic, receives `FHitResult`s, and knows nothing about input, so UI drag-drop or tests can reuse it. There is no `GetAllActorsOfClass` and no tick anywhere.

- **`USpawnSystemComponent`** (ActorComponent on the controller, no tick):
  - **Active item:** caches the active item from `UStageItemSubsystem::OnSelectedItemChanged`. It binds in BeginPlay, unbinds in EndPlay, and reads the current selection once at BeginPlay.
  - **Placement strokes:** `BeginPlacement(Hit)` spawns once. For `Continuous` items it also opens a stroke. `UpdatePlacement(Hit)` spawns into a grid cell (XY) only if this stroke hasn't filled it. That rule also stops self-stacking while the cursor rests on the item just placed. `EndPlacement()` closes the stroke. `Single` items (lights by default) never open a stroke.
  - **Grid cell size:** snapping and cell size follow `FStageItemPlacementRules`. An axis with GridSize 0 falls back to `FallbackStrokeCellSize` (100 cm) for stroke de-duplication.
  - **Spawning:** `SpawnActorDeferred` → `InitializeFromItemData` → `FinishSpawning`, with AlwaysSpawn collision handling.
  - **Deletion:** `TryDeleteActor(Actor)` only destroys actors implementing `IInteractableInterface`, so level geometry can't be deleted.
  - **Delegates:** `OnItemSpawned` and `OnItemDeleted` (the latter fires before Destroy). Undo/redo and UI can hook these later.
  - Switching the selected item mid-drag ends the stroke.
- **`AModularPlayerController`:**
  - **Input:** Enhanced Input with designer-assignable `EditorMappingContext`, `PlaceAction` and `DeleteAction`. If any slot is empty, `BuildDefaultInputMapping()` creates an equivalent runtime mapping (LMB = Place, RMB = Delete), so it works with no content.
  - **Place bindings:** Started → BeginPlacement, Triggered → UpdatePlacement (traces only while a continuous stroke is open), Completed/Canceled → EndPlacement.
  - **Delete binding:** Started → trace on the StageItem channel → TryDeleteActor.
  - **Traces:** placement uses Visibility (the floor, level geometry, and other items, so props can sit on a stage deck). Delete uses the new `StageItem` channel. Both channels are editable defaults.
  - **Cursor and camera:** the cursor is visible in GameAndUI mode. Look input is ignored so mouse clicks don't swing the default pawn's camera; camera navigation comes later.
- **`AStageCraftGameModeBase`** (AGameModeBase; the editor has no match flow, so AGameMode's state machine isn't needed) uses `AModularPlayerController` and `ADefaultPawn` (WASD/QE fly). It is set as `GlobalDefaultGameMode`.
- **`StageItem` trace channel:** `ECC_GameTraceChannel1`, default response Ignore. The C++ constant is `StageCraftCollision::StageItemChannel`. `AModularBaseActor` now blocks it.

**Files changed**
- `Source/ModularSceneBuilder/Public/Components/SpawnSystemComponent.h`, `Private/Components/SpawnSystemComponent.cpp`: new
- `Source/ModularSceneBuilder/Public/Player/ModularPlayerController.h`, `Private/Player/ModularPlayerController.cpp`: new
- `Source/ModularSceneBuilder/Public/Game/StageCraftGameModeBase.h`, `Private/Game/StageCraftGameModeBase.cpp`: new
- `Source/ModularSceneBuilder/Public/Interaction/StageCraftCollision.h`: new
- `Source/ModularSceneBuilder/Private/Actors/ModularBaseActor.cpp`: blocks the StageItem channel
- `Config/DefaultEngine.ini`: `GlobalDefaultGameMode` and the StageItem trace channel
- `tasks/todo.md`: new (plan and backlog, per Rules.md)

**Verification**
- Compiled clean on UE 5.8 (0 errors, 0 warnings). Done by Claude.
- Headless `-game -nullrhi` smoke test by Claude. The log confirmed `Game class is 'StageCraftGameModeBase'`, the input fallback message, and `Stage item catalog loaded: 0 items`. No crashes or ensures. This checks startup only, since there are no item assets yet.
- Placing and deleting is implemented, awaiting manual verification in PIE. To test:
  1. Create a `BaseItemData` in `/Game/StageCraft/Items` with a Mesh.
  2. Select it. There is no UI until Phase 5, so call `UStageItemSubsystem::SelectItem` from a level Blueprint BeginPlay.
  3. Hold LMB and drag: instances should appear one per 1 m cell.
  4. Make a Light item and single-click: exactly one instance should appear.
  5. RMB on an item deletes it; RMB on the floor does nothing.

**Commit:** uncommitted (working tree)

**Known issues / follow-ups**
- No camera look or orbit yet (WASD/QE only). Tracked in `tasks/todo.md`.
- No way to select an item in-game until the Phase 5 UI exists.
- It is not verified that ADefaultPawn's legacy WASD bindings work alongside the Enhanced Input component in 5.8. They should, but confirm in PIE.
- The plan was written to `tasks/todo.md` rather than going through a formal plan-mode approval, because the phase spec was provided in full. No Obsidian spec or session log was written: the `S:\` vault is still unreachable from this machine.
