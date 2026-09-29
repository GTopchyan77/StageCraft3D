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

**Commit:** `0098752` (committed together with #2)

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

**Commit:** `0098752`

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

**Commit:** `2f6da9e`

**Known issues / follow-ups**
- No camera look or orbit yet (WASD/QE only). Tracked in `tasks/todo.md`.
- No way to select an item in-game until the Phase 5 UI exists.
- It is not verified that ADefaultPawn's legacy WASD bindings work alongside the Enhanced Input component in 5.8. They should, but confirm in PIE.
- The plan was written to `tasks/todo.md` rather than going through a formal plan-mode approval, because the phase spec was provided in full. No Obsidian spec or session log was written: the `S:\` vault is still unreachable from this machine.

---

## #4 — Phase 4: Selection & transform gizmo (2026-09-29)

**What & why**
Users can select placed items, see them highlighted, and move or rotate them with an Unreal-style gizmo. Space toggles between translate and rotate. Everything is event-driven and interface-based: neither the selection component nor the gizmo knows any concrete item class.

- **`USelectionComponent`** (on the controller, no tick):
  - **Selecting:** `SelectActor(AActor*)` accepts only `IInteractableInterface` implementers. It calls `Execute_OnDeselect` on the previous actor, which reverts its highlight, and `Execute_OnSelect` on the new one, which applies `SelectedOverlayMaterial` via the Phase 2 actor code.
  - **Delegate:** fires `OnSelectionChanged(New, Previous)`.
  - **Destroyed actors:** it binds the selected actor's `OnDestroyed`, so deleting or destroying a selected item releases the selection and hides the gizmo automatically.
- **`AModularTransformGizmo`** (actor):
  - **Handles:** three world-aligned translate arrows (engine `/Engine/BasicShapes` Cylinder and Cone) and three rotate rings (a torus generated with `UProceduralMeshComponent`, since the engine ships no runtime ring mesh; both triangle windings are emitted so a one-sided material still renders). Each axis has a dynamic material instance of `/Engine/EngineMaterials/GizmoMaterial` (param `GizmoColor`): red, green and blue, turning yellow while dragged.
  - **Attachment:** attaches to the target with SnapToTarget location. The root uses absolute rotation and scale, so it follows the target's location while the handles stay world-aligned.
  - **Screen size:** ticks only while attached, scaling with camera distance for a roughly constant screen size.
  - **Traces:** handles block only the new `Gizmo` trace channel (ECC_GameTraceChannel2, default Ignore), so placement, selection and deletion traces pass through them. Hidden-mode handles also turn off collision.
  - **Translate drag math:** on drag start it captures a plane through the pivot that contains the drag axis and faces the camera. The movement is the cursor-ray/plane intersection delta projected onto the axis. A drag can't start when looking straight down the axis.
  - **Rotate drag math:** the plane is perpendicular to the axis. The rotation is the signed angle between the grab vector and the current vector around the axis, applied as `FQuat(axis, angle) * startRotation`. Near-parallel rays are rejected to avoid jumps.
  - **Snapping and settings:** optional `TranslationSnap` (cm) and `RotationSnapDegrees` (both 0 = free). Meshes, material, colors and screen factor are designer-overridable.
  - **Input-free:** the gizmo receives cursor rays through `TryBeginDrag`, `UpdateDrag` and `EndDrag`. It only touches `AActor` location and rotation.
- **`AModularPlayerController`** changes:
  - Owns `USelectionComponent` and spawns one gizmo (`GizmoClass`, owner = the controller) for the local player. It re-targets the gizmo from `OnSelectionChanged`.
  - **LMB priority:** gizmo handle (start drag) > placed item (select) > empty surface (deselect, then place if a catalog item is armed).
  - **Held LMB:** updates the gizmo drag, or the continuous placement stroke. Per-frame traces happen only while one of those is active.
  - **RMB:** placed item → delete (unchanged). Empty space → clear the actor selection and disarm the catalog item (`UStageItemSubsystem::ClearSelection`).
  - **New `ToggleGizmoModeAction`** (default Space). The built-in action consumes legacy keys, so Space doesn't also trigger `ADefaultPawn`'s "fly up" axis. An assigned asset should enable "Consumes Action And Axis Mappings".
- **Build and config:** private dependency on `ProceduralMeshComponent` (engine plugin, enabled by default). `Gizmo` trace channel added in DefaultEngine.ini.
- **Housekeeping:** STATE.md #1–#3 commit lines filled in with the real hashes (they had gone stale).

**Files changed**
- `Source/ModularSceneBuilder/Public/Components/SelectionComponent.h`, `Private/Components/SelectionComponent.cpp`: new
- `Source/ModularSceneBuilder/Public/Actors/ModularTransformGizmo.h`, `Private/Actors/ModularTransformGizmo.cpp`: new
- `Source/ModularSceneBuilder/Public/Player/ModularPlayerController.h`, `Private/Player/ModularPlayerController.cpp`: selection, gizmo, click arbitration, Space action
- `Source/ModularSceneBuilder/Public/Interaction/StageCraftCollision.h`: `GizmoChannel`
- `Source/ModularSceneBuilder/ModularSceneBuilder.Build.cs`: `ProceduralMeshComponent`
- `Config/DefaultEngine.ini`: Gizmo trace channel
- `tasks/todo.md`: Phase 4 checklist and backlog
- `STATE.md`: this entry and the commit-hash fix

**Verification**
- The **Game** target (`Build.bat ModularSceneBuilder Win64 Development`) compiles clean: 0 errors, 0 warnings, all module sources. Done by Claude.
- The **Editor** target was not rebuilt, because Live Coding was active in an open editor session and UBT refused. New classes, a module dependency and header changes can't be live-patched. Gevor needs to close the editor and rebuild (IDE or `Build.bat ModularSceneBuilderEditor Win64 Development`).
- Runtime is implemented, awaiting manual verification in PIE:
  1. Place an item.
  2. LMB it: selected overlay plus gizmo appear at its pivot.
  3. Drag each arrow: it moves along that world axis only, and the arrow turns yellow.
  4. Press Space: rings appear. Drag a ring to rotate around that axis. Space should not move the camera up.
  5. LMB empty floor: deselects (and places if a catalog item is armed).
  6. RMB empty: deselects and disarms.
  7. RMB the selected item: it is deleted and the gizmo hides.
  8. The gizmo stays a similar on-screen size when flying closer or farther with WASD.

**Commit:** uncommitted (working tree)

**Known issues / follow-ups**
- **Unverified assumptions, check in PIE:**
  - BasicShapes Cone's pivot is at its centre (tip +Z). If the arrowheads are offset, adjust the head placement in `ModularTransformGizmo.cpp`.
  - `GizmoMaterial` renders correctly on static and procedural meshes in game.
  - Legacy-key consumption actually stops Space from moving the default pawn.
- The gizmo is depth-tested, so it can be hidden inside large meshes. Drawing it on top needs a custom material. Tracked in the backlog.
- A plain LMB on an existing item now selects it instead of stacking a new item on top. Placing onto items still works mid-stroke; a modifier to force placement is in the backlog.
- Hover highlight is still not wired (backlog).
- No Obsidian spec or session log was written: the `S:\` vault is still unreachable. Plan kept in `tasks/todo.md`.

---

## #5 — Coding standard: no elaborated type specifiers in pointer templates (2026-09-29)

**What & why**
Gevor made this a hard rule: never write `class` or `struct` inside `TObjectPtr<...>`. Always forward-declare at the top of the header and use `TObjectPtr<UType> Name;`. This keeps dependencies visible in one place, and an inline `class X` can silently declare a new type in the wrong namespace.

- `Rules.md` → Code Standards: added the rule. It also covers `TWeakObjectPtr`, `TSoftObjectPtr`, `TSoftClassPtr`, `TSubclassOf`, and inline elaborated specifiers in function signatures.
- Audit of every `.h` and `.cpp` under `Source/`: **no** `TObjectPtr<class ...>` or `TObjectPtr<struct ...>` (or other pointer templates) existed. All `TObjectPtr` members already used forward-declared or included types.
- Fixed the one related violation: `UBaseItemData::IsDataValid(class FDataValidationContext&)` now uses a top-of-header `class FDataValidationContext;` forward declaration. The class key matches the engine's `class FDataValidationContext`, so there's no C4099.

**Files changed**
- `Rules.md`: new Code Standards bullet
- `Source/ModularSceneBuilder/Public/Data/BaseItemData.h`: forward declaration plus clean signature

**Verification**
- The Game target compiles clean. Done by Claude.
- The Editor target is still blocked by Live Coding in the open editor. `IsDataValid` is `WITH_EDITOR`-only, so the changed line itself is compiled only by the Editor build. It is awaiting an editor rebuild after Gevor closes the editor. This is the same pending rebuild as #4.

**Commit:** uncommitted (working tree)

**Known issues / follow-ups**
- None beyond the pending editor rebuild from #4.
- **Superseded by #6** on the same day: the rule was reversed.

---

## #6 — Coding standard reversed: inline elaborated type specifiers, no forward declarations (2026-09-29)

**What & why**
Gevor reversed the #5 rule. Headers must no longer contain top-of-file forward declarations. Every UObject type inside a pointer template is written with an inline elaborated specifier (`TObjectPtr<class USpawnSystemComponent> SpawnSystem = nullptr;`). Types from includes are elaborated too, so the style is uniform. This follows the common Epic engine style and keeps each declaration self-contained.

- `Rules.md` → Code Standards: replaced the #5 bullet with the new rule and examples.
  - It covers `TObjectPtr`, `TSubclassOf`, `TSoftObjectPtr`, `TSoftClassPtr` and `TWeakObjectPtr`.
  - Non-included types must be elaborated at every use in the header: parameters, returns and dynamic delegate macro parameters. Otherwise the header doesn't compile once the forward declaration is gone.
  - Single `TObjectPtr` members get `= nullptr`; static arrays can't.
  - Caveat: only use elaborated specifiers for global-namespace types, because inside a `namespace` they declare a new type.
- Refactor, applied mechanically with a Perl script to every header under `Source/ModularSceneBuilder/Public/` (no `.cpp` files contained forward declarations):
  - Removed all 25 forward declarations from 8 headers: `ModularBaseActor.h`, `ModularTransformGizmo.h`, `SpawnSystemComponent.h`, `BaseItemData.h`, `InteractableInterface.h`, `ModularPlayerController.h`, `StageItemSubsystem.h`, plus `SelectionComponent.h`, which had no forward declarations but gained an elaborated `TObjectPtr`.
  - Elaborated every pointer-template argument and every remaining use of a formerly forward-declared type (e.g. `const struct FHitResult&`, `class UBaseItemData* GetSelectedItem()`, `DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(..., class UBaseItemData*, NewItem, ...)`).
  - `IsDataValid` is back to `class FDataValidationContext&`.
  - Added `= nullptr` to all single `TObjectPtr` members.
  - Comment lines were left untouched.

**Files changed**
- `Rules.md`
- `Source/ModularSceneBuilder/Public/Actors/ModularBaseActor.h`, `Actors/ModularTransformGizmo.h`, `Components/SelectionComponent.h`, `Components/SpawnSystemComponent.h`, `Data/BaseItemData.h`, `Interaction/InteractableInterface.h`, `Player/ModularPlayerController.h`, `Subsystems/StageItemSubsystem.h`

**Verification**
- Full rebuild (`-Rebuild`) of the Game target by Claude: UHT regenerated all reflection code and all 11 module sources plus the generated module compiled. 0 errors, 0 warnings. UHT accepts elaborated specifiers in `UPROPERTY` template arguments, `UFUNCTION` parameters and return types, and dynamic delegate macros.
- The Editor target is still blocked by Live Coding in the open editor. The only editor-only line touched is `IsDataValid(class FDataValidationContext&)`, which matches the engine's own declaration in `UObject`. Awaiting the editor rebuild already pending from #4.

**Commit:** uncommitted (working tree)

**Known issues / follow-ups**
- New headers must follow the rule by hand. No lint or CI check enforces it yet.
