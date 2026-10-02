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
- The **Editor** target is up to date. Rebuilt after the editor was closed (DLL 15:49, newer than all sources). Confirmed by Claude with an up-to-date `Build.bat ModularSceneBuilderEditor Win64 Development` run: Succeeded.
- Runtime is implemented, awaiting manual verification in PIE:
  1. Place an item.
  2. LMB it: selected overlay plus gizmo appear at its pivot.
  3. Drag each arrow: it moves along that world axis only, and the arrow turns yellow.
  4. Press Space: rings appear. Drag a ring to rotate around that axis. Space should not move the camera up.
  5. LMB empty floor: deselects (and places if a catalog item is armed).
  6. RMB empty: deselects and disarms.
  7. RMB the selected item: it is deleted and the gizmo hides.
  8. The gizmo stays a similar on-screen size when flying closer or farther with WASD.

**Commit:** `2187677` (together with #5 and #6)

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
- The Editor target is up to date and includes the `WITH_EDITOR` `IsDataValid` line (see #4 verification).

**Commit:** `2187677` (content superseded by #6 in the same commit)

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
- The Editor target is up to date and includes the `WITH_EDITOR` `IsDataValid(class FDataValidationContext&)` line (see #4 verification).

**Commit:** `2187677`

**Known issues / follow-ups**
- New headers must follow the rule by hand. No lint or CI check enforces it yet.

---

## #7 — UnrealNGGMCP (Claude Code ↔ editor MCP bridge) wired into the project (2026-09-29)

**What & why**
Gevor asked why the `UnrealNGGMCP` MCP server wasn't registered in Claude Code.

Root cause: the plugin was only installed at engine level (`UE_5.8/Engine/Plugins/Marketplace/AIEditor162bfba9fb91V2`) and was never enabled in `ModularSceneBuilder.uproject`. So it never loaded, and never ran its first-start step that writes `.mcp.json`. Its own source (`NGGMcpConfig.cpp`) also says an engine-level install "is not a supported layout". It expects `<Project>/Plugins/<folder>`, because the MCP server finds the project root by walking up from its own directory.

How it works:
- Claude Code launches `node .../unrealngg-mcp/bootstrap.js` over stdio. That server talks HTTP to the editor-side bridge.
- The bridge picks a free port between 6776 and 6800 and publishes it in `Saved/UnrealNGGMCP/bridge.json`.
- The auth token lives in `Config/DefaultEngine.ini` and is read by both ends.

Changes:
- **Plugin copy:** copied the plugin into `Plugins/UnrealNGGMCP/` (uplugin, Source including the bundled `node_modules`, Content, Resources, and the prebuilt 5.8 Binaries; not Intermediate). Per `PluginManager.cpp`, a project plugin takes priority over a disabled engine plugin of the same name, so there's no conflict.
- **`.uproject`:** enabled `UnrealNGGMCP` (Editor targets only; it is an Editor-type module). Its descriptor also enables EnhancedInput, Niagara, PythonScriptPlugin, GeometryScripting, GameplayAbilities, StateTree, PropertyBindingUtils and PCG as dependencies.
- **`.mcp.json`:** written at the project root with the exact entry the plugin itself generates for this layout: server key `ue5-ngg`, `node Plugins/UnrealNGGMCP/Source/ThirdParty/unrealngg-mcp/bootstrap.js`, cwd `.`, env `NGG_BRIDGE_TIMEOUT_MS=15000`, and an empty `MESHY_API_KEY`. There is no URL or token by design. On editor start the plugin sees the entry as `AlreadyCorrect` and leaves it alone.
- **`.gitignore`:** added `node_modules/`. `Plugins/*/Binaries` is already covered by `Binaries/`.
- **`.claude/`:** the MCP server's first run installed its bundled skills and agents there (7 files: ue5-niagara, ue5-umg-widgets, ue5-senior-dev, ue5-build-engineer, …).

**Files changed**
- `Plugins/UnrealNGGMCP/**` (new; binaries and node_modules ignored)
- `ModularSceneBuilder.uproject`
- `.mcp.json` (new)
- `.gitignore`
- `.claude/skills/**`, `.claude/agents/**` (new, installed by the MCP server)

**Verification**
- Node v22.22.2 and npm 10.9.7 are on PATH.
- `node bootstrap.js` smoke test by Claude: the server started, loaded its rules (`62718 bytes -> 9668 bytes in instructions (OK)`), installed the skills and agents, and exited cleanly when stdin closed.
- **Not yet connected.** The running editor (PID 8860) was started before the plugin was enabled, so the bridge isn't up: there is no `Saved/UnrealNGGMCP/bridge.json` and no token in DefaultEngine.ini yet. Remaining steps:
  1. Gevor saves and closes the editor.
  2. Rebuild the Editor target so the plugin module builds with the project.
  3. Relaunch the editor. The bridge starts and writes `bridge.json` plus the token.
  4. Restart Claude Code (or run `/mcp`) and approve the project-scoped `ue5-ngg` server.

**Commit:** `37d46bd`

**Known issues / follow-ups**
- The engine-level copy stays in place. It's harmless while disabled, but it's a second copy to keep in sync on plugin updates.
- Enabling the plugin pulls in heavy dependencies (PCG, GameplayAbilities, StateTree, Python). Expect a longer first editor start.
- Decide whether to commit `Plugins/UnrealNGGMCP/Source` (the plugin's README intends it to live in the project) and the installed `.claude/` assets.

---

## #8 — Phase 4 wiring: config audit, clean rebuild of both targets, PIE checklist (2026-09-29)

**What & why**
Closes out Phase 4 integration after the interrupted session. No code changes were needed; this entry records the audit and a clean-build baseline for both targets after the #6 header refactor.

- **Default GameMode:** `DefaultEngine.ini` → `[/Script/EngineSettings.GameMapsSettings] GlobalDefaultGameMode=/Script/ModularSceneBuilder.StageCraftGameModeBase`. Already correct. `AStageCraftGameModeBase` sets `AModularPlayerController` and `ADefaultPawn` in its constructor, so every map without a World Settings override uses it. (`GameDefaultMap` is still the engine `OpenWorld` template.)
- **Enhanced Input:** `DefaultInput.ini` sets `EnhancedPlayerInput` and `EnhancedInputComponent` as defaults, which `SetupInputComponent` requires.
- **Asset-free input fallback, verified in code:** `SetupInputComponent` calls `BuildDefaultInputMapping()` if *any* of `EditorMappingContext`, `PlaceAction`, `DeleteAction` or `ToggleGizmoModeAction` is unset.
  - It creates the missing `UInputAction`s at runtime, plus a fresh `UInputMappingContext` (LMB = Place, RMB = Delete, Space = ToggleGizmoMode). The Space action consumes legacy keys so the pawn doesn't fly up.
  - It logs `input assets not fully assigned, using built-in LMB/RMB/Space mapping.`
  - `GizmoClass` defaults to the native `AModularTransformGizmo`.
  - So PIE works with zero content. Assigning assets (e.g. via `Content/Python/stagecraft_setup_content.py`) is optional and only needed for key rebinding. If you assign assets, assign all four; a partial set gets replaced by the fallback context.

- **PIE test content** (created by Claude through the editor MCP bridge):
  - `/Game/StageCraft/Items/DA_Test_Crate`: Prop, Continuous, `/Engine/BasicShapes/Cube`, 100 cm grid, +50 Z offset.
  - `/Game/StageCraft/Items/DA_Test_Light`: Light, Single, `/Engine/BasicShapes/Sphere`, +50 Z offset.
  - `/Game/StageCraft/Maps/L_StageTest`: a classic (non-World-Partition) level with a 40 m floor plane, directional light, sky light, fog (disabled) and a PlayerStart looking down at the floor.
  - `/Game/StageCraft/Debug/BP_StageTestArmer`: an Actor with custom events `ArmCrate` and `ArmLight`, each calling `StageItemSubsystem::SelectItem`. BeginPlay arms the crate. It's an actor rather than level-Blueprint logic because the MCP bridge can't edit level Blueprints. Delete it once the Phase 5 catalog UI exists.

**Files changed**
- `Content/StageCraft/Items/DA_Test_Crate.uasset`, `DA_Test_Light.uasset`, `Content/StageCraft/Maps/L_StageTest.umap`, `Content/StageCraft/Debug/BP_StageTestArmer.uasset`: new
- `STATE.md`: this entry; #7 commit line corrected (it was committed in `37d46bd`)
- `tasks/todo.md`: Phase 4 build item

**Verification**
- Clean `-Rebuild` of the **Game** target (`Build.bat ModularSceneBuilder Win64 Development`) by Claude: 21/21 actions, all 11 module sources plus generated code compiled, `ModularSceneBuilder.exe` linked. Result: Succeeded, 0 errors, 0 warnings.
- Clean `-Rebuild` of the **Editor** target (`Build.bat ModularSceneBuilderEditor Win64 Development`) by Claude with the editor closed: 23/23 actions, the same module sources plus the `UnrealNGGMCP` project plugin, `UnrealEditor-ModularSceneBuilder.dll` linked. Result: Succeeded, 0 errors, 0 warnings.
- Runtime is **not yet verified in PIE**. Manual checklist (Gevor):

  **Setup** (test content already exists, see below)
  1. Open `/Game/StageCraft/Maps/L_StageTest` and press Play. `BP_StageTestArmer` arms the Test Crate on BeginPlay.
  2. To switch items, open the console (`~`) and run `ke * ArmLight` or `ke * ArmCrate`.
  3. The Output Log shows `Game class is 'StageCraftGameModeBase'`, the input fallback line, and `Stage item catalog loaded: 2 items.` (confirmed by Claude in an automated PIE run).

  **Spawning**
  4. Hold LMB and drag on the floor: one prop per 1 m cell, with no stacking while the cursor rests on the new item.
  5. With the Light item armed, a single LMB click places exactly one.

  **Selecting**
  6. LMB a placed item: selected overlay (if assigned) plus the gizmo at its pivot.
  7. LMB empty floor: deselects (and places if an item is armed).
  8. RMB empty space: deselects and disarms.

  **Gizmo**
  9. Drag each arrow: movement is along that world axis only, and the arrow turns yellow while dragged.
  10. Press **Space**: arrows swap for rings, and the camera does **not** fly up. Drag a ring: the item rotates around that axis. Press Space again to return to translate.
  11. Fly closer and farther with WASD: the gizmo stays roughly the same size on screen.

  **Deleting**
  12. RMB the selected item: it is destroyed and the gizmo hides. RMB on the floor deletes nothing.

  Also check: the arrowheads sit on the shafts (Cone pivot assumption), and the gizmo is visible on both arrows and rings (GizmoMaterial on procedural mesh).

**Commit:** `6810e66`

**Known issues / follow-ups**
- Open items from #4 still stand: overlay materials are a content task, the gizmo is depth-tested, and there's no hover highlight or camera orbit yet.
- `GameDefaultMap` still points at the engine `OpenWorld` template. Set a StageCraft map once one exists.

---

## #9 — Phase 5 architecture: concert production data model, parameter inspector, show control & DMX boundary (2026-09-29)

**What & why**
StageCraft grows from a prop placer into a concert production and pre-vis tool. This entry lays down the C++ core that every later Phase 5 feature builds on. It does **not** include Widget Blueprints, catalog content, or a DMX transport; those are listed as open tasks in `tasks/todo.md`.

Design principle: **one self-describing parameter model** feeds the inspector, the cue system, DMX and (later) save files. No system needs to know concrete equipment classes.

```
UBaseItemData (catalog, const)          AModularBaseActor (placed instance, live state)
 |- ULightingFixtureData  --spawns-->    |- ALightingFixtureActor --registers--> UShowControlSubsystem <--> UStageDMXBridge <--> Art-Net/sACN (future module)
 |- UAudioEquipmentData   --spawns-->    |- AAudioEquipmentActor                 (fixture registry, patch,
 '- UStageTrussData       --spawns-->    '- AStageTrussActor                      cue list, fades, DMX I/O)
                                                 | IStageParameterInterface + OnParameterChanged
                                                 v
                    USelectionComponent --> UStageInspectorPanel --> UStageParameterSectionWidget --> UStageParameterRowWidget
```

**1. Equipment data model (catalog = `UBaseItemData` subclasses; instance state = actor subclasses)**
- `UBaseItemData` gains `FStageEquipmentSpecs Specs` (Manufacturer, WeightKg, PowerDrawWatts). It sits on the base class because rigging load and power distribution calculations need it for every item.
- `EStageItemType` gains `Truss` and `Audio`, appended at the end so existing assets keep their values.
- `ULightingFixtureData`:
  - **Kind and color:** FixtureKind (spot/beam/wash moving head, LED par, strobe, laser) and ColorSystem (None/RGB/RGBW/CMY). ColorTemperatureK is used by fixed-white fixtures.
  - **Optics:** luminous flux, beam angle min/max (zoom), beam range.
  - **Motion:** pan/tilt limits, plus yoke and head meshes with pivot offsets.
  - **DMX mode:** DMXModeName plus `TArray<FStageFixtureDMXChannel>`: an attribute tag, a 1-based channel, and a 16-bit flag.
  - **Capability queries:** `SupportsAttribute`, `GetAttributeRange` (physical range, also the DMX normalization range), `GetAttributeDefault`, and `GetDMXFootprint`.
  - **Editor support:** the `GenerateDefaultDMXLayout` button builds 16-bit pan/tilt, then dimmer, shutter, color and zoom. The constructor runs the same builder, so new assets start with a sensible mode.
  - **Validation:** overlapping channels, channels past 512, channels mapped to an attribute the fixture can't do, and bad ranges.
- `UAudioEquipmentData`: AudioKind (line array element, point source, sub, monitor, console, amp, processor), H/V coverage, max SPL, frequency response, impedance, MaxSplayAngle, and ChannelCount. `IsLoudspeaker()` decides which inspector rows appear.
- `UStageTrussData`: PieceKind (straight, corner, junction, tower, base plate, hoist), profile and width, length, max point load, `FStageRiggingPoint`s (local transform and safe working load) and `FStageTrussConnector`s (for future snap-to-truss). Each length is its own asset, as in a rental inventory, so truss instances have nothing to tune.
- Each data subclass constructor presets its ItemType, ActorClass, a category tag and Single placement.
- Per-instance runtime state, all `SaveGame`:
  - `AModularBaseActor`: `InstanceLabel`.
  - `ALightingFixtureActor`: `FixtureId`, `FStageDMXPatch` (universe/address) and `TMap<FGameplayTag,float> Attributes`.
  - `AAudioEquipmentActor`: gain dB, mute, delay ms, polarity and splay (clamped to the catalog max).
- `ALightingFixtureActor` components: the root base mesh, then PanPivot → YokeMesh, then TiltPivot → HeadMesh + `USpotLightComponent` BeamLight (lumens).
  - The beam leaves along the head's +Z, which points up for a standing fixture and down when the actor is flipped to hang.
  - All writes go through `SetAttribute`/`SetAttributes`: clamp, update the components once, broadcast.
  - Colors are edited as RGB(W). C/M/Y exist only as DMX channel encodings (C = 1 − R).
  - The moving parts are selectable too, via the StageItem channel, and they mirror the selection overlay.

**2. Parameter model and inspector (GrandMA3-style)**
- `Data/StageParameterTypes.h` defines the model:
  - `FStageParameterValue`: a tagged union of Float, Integer, Bool, Color, Vector, Rotator and Text.
  - `FStageParameterDescriptor`: id tag, name, value, min/max, DisplayScale (e.g. 0..1 shown as %), step, units, read-only.
  - `FStageParameterSection`: a feature group tag, a title and its rows.
- Native tags:
  - `StageCraft.FeatureGroup.*` (Info, Transform, Patch, Dimmer, Position, Color, Beam, Audio, Rigging).
  - `StageCraft.Param.*` for edit-only values.
  - `StageCraft.Attribute.*` for Dimmer, Pan, Tilt, Zoom, Shutter, ColorR/G/B/W, the composite ColorRGB, and ColorC/M/Y. Attributes are what cues record and what DMX addresses.
- `IStageParameterInterface` has `GetParameterSections`, `GetParameterValue` and `SetParameterValue`. It is a BlueprintNativeEvent, separate from `IInteractableInterface`.
- `AModularBaseActor` implements the interface through three virtual hooks (`GatherParameterSections`, `ReadParameter`, `WriteParameter`). Subclasses call Super, so every item gets Info (label, model, weight, power) and Transform (location, rotation) at the top.
- `AModularBaseActor::OnParameterChanged(Actor, ParameterId)` covers every change source, because gizmo moves arrive through the root component's `TransformUpdated` (bound in BeginPlay). Inspector edits, the gizmo, cue fades and DMX input therefore all update the panel live. An invalid tag means "rebuild".
- **UI (C++ bases, layout in Widget Blueprints):**
  - `UStageInspectorPanel`: follows the controller's `USelectionComponent` and builds sections and rows from the descriptors. Rows are chosen by type, with a separate read-only row class. Values refresh in place on `OnParameterChanged`. A rejected or clamped edit snaps the row back to the actual value.
  - `UStageParameterSectionWidget`: `RowContainer` is required; the header and color strip are optional.
  - `UStageParameterRowWidget`: optional named widgets (`ValueSpinBox`, `SpinX/Y/Z`, `ValueCheckBox`, `ValueTextBox`, `ColorSwatch`, `LabelText`, `UnitsText`, `ValueText`, `GroupColorStrip`) are wired automatically: ranges in display units, live drag, and read-only state. Types with no native editor (Color) call `CommitValue` from Blueprint.
  - `UStageCraftUITheme` (data asset): a dark console palette with an amber accent (#FFB300), panel #15171C and row #1A1D23. Feature groups get colors: Dimmer yellow, Position blue, Color magenta, Beam green, Patch orange, Audio teal, Rigging brown.
  - `AModularPlayerController::HUDWidgetClass` creates the root editor UI for the local player. Visible UMG panels consume clicks, so UI clicks never place or select items.
  - Planned WBP layout: a left dock for the catalog (category tabs driven by the new tags), a right dock for the inspector (Info / Transform / Patch / Dimmer / Position / Color / Beam sections, each with a 4 px color strip), a bottom dock for the show panel (cue list, Go / Back, fade time, DMX source switch) and a top bar (show name, DMX status, FPS).

**3. Show control and DMX foundation**
- `UShowControlSubsystem` (`UTickableWorldSubsystem`, Game/PIE worlds only). It ticks only while a fade runs or DMX output is active.
  - **Fixture registry and patch:** fixtures register themselves on BeginPlay, with no world iteration. Registration assigns the next free fixture ID and, if `bAutoPatchNewFixtures` is set, the next free address range. Other queries: `FindNextFreePatch`, `FindPatchConflicts` (overlaps are allowed as on a console, and reported) and `GetPatchedUniverses`.
  - **Cues:** `FShowCue` holds a number (decimals allowed), label, fade time and per-FixtureId attribute maps. `StoreCue` records a full snapshot ("cue only"). Tracking needs no format change later, because unrecorded attributes are simply absent.
    - Playback: `GoToCue`, `Go` (next), `FinishFade`, `SetCues` (load), with linear crossfades from the live values. A new Go during a fade takes over from where the fixtures are.
    - Delegates: `OnCueStarted`, `OnCueFinished` and `OnCueListChanged`.
  - **Control source:** `Internal` means cues drive the fixtures and DMX goes out; StageCraft acts as the console. `External` means incoming DMX drives the fixtures and cues are disabled; StageCraft is the visualiser for an MA3/ChamSys desk.
- **DMX codec:** `ALightingFixtureActor::WriteDMX` / `ReadDMX` convert attributes to and from a 512-slot buffer, with 8/16-bit coarse+fine and CMY inversion. `UShowControlSubsystem::RenderDMXUniverse(Universe)` exposes the encoded frame (useful for debugging a patch).
- `UStageDMXBridge` (abstract, Blueprintable) is the transport boundary.
  - **Output:** the subsystem calls `SendUniverse` at `DMXOutputRateHz` (44 Hz).
  - **Input:** the bridge calls `ReceiveUniverse`.
  - **Why no plugin dependency:** the core module does **not** depend on the DMX Engine plugin. UE 5.8 ships `Engine/Plugins/VirtualProduction/DMX` (DMXEngine, DMXProtocol, DMXFixtures, DMXGDTF, DatasmithMVR). A future `StageCraftDMX` module will depend on `DMXProtocol` and implement the bridge with Art-Net/sACN input and output ports. This keeps cook and startup light for users without DMX, and isolates engine-plugin API churn.

**4. Catalog categories**
- New native tags:
  - `StageCraft.Category.Stage.Deck` and `.Stage.Riser`
  - `.Truss.Straight`, `.Truss.Corner` and `.Truss.Tower`
  - `.Rigging` and `.Rigging.Motor`
  - `.Lighting.MovingHead`, `.Lighting.Par`, `.Lighting.Strobe` and `.Lighting.Laser`
  - `.Audio.LineArray`, `.Audio.Subwoofer`, `.Audio.Monitor`, `.Audio.Console` and `.Audio.Amplifier`
- The parent tags stay valid filters, because `UStageItemSubsystem::GetCatalogByCategory` matches child tags.

**Files changed**
- `Source/ModularSceneBuilder/ModularSceneBuilder.Build.cs`: `UMG` (public) plus `Slate` and `SlateCore` (private)
- `Public/Data/StageItemTypes.h`, `Private/Data/StageItemTypes.cpp`: `EStageItemType::Truss/Audio`, `FStageEquipmentSpecs`, 16 category tags
- `Public/Data/BaseItemData.h`: `Specs`
- `Public/Data/StageParameterTypes.h`, `Private/Data/StageParameterTypes.cpp`: new
- `Public/Data/LightingFixtureData.h`, `Private/Data/LightingFixtureData.cpp`: new
- `Public/Data/AudioEquipmentData.h`, `Private/Data/AudioEquipmentData.cpp`: new
- `Public/Data/StageTrussData.h`, `Private/Data/StageTrussData.cpp`: new
- `Public/Interaction/StageParameterInterface.h`: new
- `Public/Actors/ModularBaseActor.h`, `Private/Actors/ModularBaseActor.cpp`: implements `IStageParameterInterface`; adds `OnParameterChanged`, `InstanceLabel`, the root `TransformUpdated` binding and BeginPlay
- `Public/Actors/LightingFixtureActor.h`, `Private/Actors/LightingFixtureActor.cpp`: new
- `Public/Actors/AudioEquipmentActor.h`, `Private/Actors/AudioEquipmentActor.cpp`: new
- `Public/Actors/StageTrussActor.h`, `Private/Actors/StageTrussActor.cpp`: new
- `Public/Show/ShowControlTypes.h`: new (`FStageDMXPatch`, `FShowCue`, `FShowCueFixtureState`, `EStageControlSource`)
- `Public/Show/StageDMXBridge.h`, `Private/Show/StageDMXBridge.cpp`: new
- `Public/Subsystems/ShowControlSubsystem.h`, `Private/Subsystems/ShowControlSubsystem.cpp`: new
- `Public/UI/StageCraftUITheme.h`, `Private/UI/StageCraftUITheme.cpp`: new
- `Public/UI/StageParameterRowWidget.h`, `Private/UI/StageParameterRowWidget.cpp`: new
- `Public/UI/StageParameterSectionWidget.h`, `Private/UI/StageParameterSectionWidget.cpp`: new
- `Public/UI/StageInspectorPanel.h`, `Private/UI/StageInspectorPanel.cpp`: new
- `Public/Player/ModularPlayerController.h`, `Private/Player/ModularPlayerController.cpp`: `HUDWidgetClass` and `GetHUDWidget()`
- `STATE.md`, `tasks/todo.md`

**Verification**
- **Game** target (`Build.bat ModularSceneBuilder Win64 Development`) built by Claude: 26/26 actions, UHT with -WarningsAsErrors, `ModularSceneBuilder.exe` linked. Result: Succeeded, 0 errors, 0 warnings.
- **Editor** target not built: the editor was open (Live Coding). Close the editor and run `RunEditor.bat` (it detects the stale module and rebuilds), or build `ModularSceneBuilderEditor` manually.
- Runtime is **not verified**. It needs content first (fixture/audio/truss data assets and the inspector Widget Blueprints; see `tasks/todo.md` Phase 5).
- The Phase 4 PIE checklist (#8) is also still open. Phase 5 changes `AModularBaseActor` (a new BeginPlay binding and the parameter interface), so run that checklist on this build.

**Commit:** `6810e66`

**Known issues / follow-ups**
- **Strobe:** the Shutter attribute is recorded and sent over DMX but not animated. It needs one central strobe clock in `UShowControlSubsystem`, not per-actor ticks.
- **Beams:** a beam is a spot light only. A visible shaft needs volumetric fog in the level or a beam-cone mesh/material. Lasers need their own Niagara-based actor (FixtureKind `Laser` currently renders as a spot).
- **Color editing:** there is no native UMG color wheel. The Color row needs a WBP color picker that calls `CommitValue` (this is the Phase 5 "light color picker" item).
- **Show persistence:** cues and instance state are `SaveGame`-tagged but nothing saves a show yet (a `USaveGame`-based show file comes later).
- **Inspector rebuilds:** the inspector rebuilds every widget on each selection change. Pool rows if selection switching shows up in profiling.
- **Audio:** audio parameters are design data only; coverage and SPL visualisation come later. Line array hang building (splay chaining) comes later.
- **Rigging:** load calculations (hung weight per rigging point against the safe working load) and snap-to-truss via `Connectors` / `RiggingPoints` are not built yet.
- No Obsidian session log: the vault `S:\OBSIDIAN VAULTS\...` is not reachable from this machine (same as #1).

---

## #10 — Phase 5 Part 2: inspector Widget Blueprints, test equipment, volumetric fog stage (2026-09-29)

**What & why**
This entry makes the Phase 5 core (#9) visible and usable in PIE: a working GrandMA-style inspector, one test asset per equipment family, and a dark, fogged test stage so beams read.

- **Generic row via `EditorSlot` (C++).** The MCP widget tool cannot create `SpinBox` widgets, and per-type row Blueprints would multiply maintenance. `UStageParameterRowWidget` now builds any value editor the Blueprint did not bind at runtime, inside an optional `EditorSlot` panel:
  - SpinBox for Float/Integer, X/Y/Z boxes for Vector/Rotator, a CheckBox for Bool, an EditableTextBox for Text.
  - Color gets a swatch plus R/G/B percent boxes, an interim editor until the colour-wheel picker lands.
  - `ValueText` collapses when an editable editor exists; read-only rows show only `ValueText`.
  - The result is that one row Blueprint serves every parameter type.
- **Widget Blueprints** (all `/Game/StageCraft/UI/`, dark console palette from `DA_StageCraftTheme`):
  - `Inspector/WBP_StageParameterRow` (parent `UStageParameterRowWidget`): a Border (#1A1D23) around a HorizontalBox. The box holds `GroupColorStrip` (3 px), `LabelText` (grey, fill 0.42), `EditorSlot` (fill 0.58), `ValueText` and `UnitsText`.
  - `Inspector/WBP_StageParameterSection` (parent `UStageParameterSectionWidget`): a Border (#15171C) around a VerticalBox. Inside is a `HeaderBar` (#1E2128) with `GroupColorStrip` (4 px) and `HeaderText` (bold, upper case), followed by `RowContainer`.
  - `Inspector/WBP_StageInspectorPanel` (parent `UStageInspectorPanel`): a Border (#0B0C0F, slightly translucent) around a VerticalBox.
    - The box holds a `TitleBar` (amber "INSPECTOR" caption, `TitleText`, `SubtitleText`), then the `EmptyState` hint, then `SectionContainer` (a ScrollBox, fill).
    - Class defaults: `Theme` = `DA_StageCraftTheme`, `SectionWidgetClass` = `WBP_StageParameterSection`, `FallbackRowWidgetClass` = `WBP_StageParameterRow`.
  - `WBP_StageCraftHUD` (plain UserWidget): a CanvasPanel with the inspector docked top-right, 380 × 820 px, 12 px margin.
  - `DA_StageCraftTheme` (`UStageCraftUITheme`): the C++ default palette.
- **HUD wiring:**
  - `/Game/StageCraft/Blueprints/BP_StageCraftPlayerController` (child of `AModularPlayerController`) sets `HUDWidgetClass` = `WBP_StageCraftHUD`.
  - `BP_StageCraftGameMode` (child of `AStageCraftGameModeBase`) sets `PlayerControllerClass` = that controller.
  - `L_StageTest` World Settings → GameMode Override = `BP_StageCraftGameMode`. The global C++ default in `DefaultEngine.ini` is unchanged.
- **Test equipment** (`/Game/StageCraft/Data/`, created by Claude through the editor MCP bridge, not with a script):
  - `DA_MovingHead_Test` (`ULightingFixtureData`): MovingHeadSpot, RGBW, 25 000 lm, zoom 4–40°, pan ±270°, tilt ±135°.
    - Base Cylinder with a Sphere head (placeholder engine meshes, 1 m scale).
    - Pivots: pan 50, tilt 60, beam 52 cm.
    - Mode "Standard 12ch", with the auto-generated layout: Pan16, Tilt16, Dim16, Shutter, R, G, B, W, Zoom. 24.5 kg, 470 W.
  - `DA_LineArray_Test` (`UAudioEquipmentData`): LineArrayElement, 110 × 10°, 141 dB, 55 Hz–18 kHz, 8 Ω, max splay 10°, 58 kg, Cube mesh.
  - `DA_Truss_Test` (`UStageTrussData`): straight F34-class box truss.
    - The length is **1.0 m** so it matches the 100 cm placeholder cube; 900 kg max point load, 7 kg.
    - Rigging points End_A / Centre / End_B (450 / 900 / 450 kg SWL); connectors Face_A and Face_B.
  - `/Game/StageCraft/Data` is added to the `StageItem` Asset Manager scan in `DefaultGame.ini`, otherwise these items never reach the catalog.
- **`L_StageTest` stage:**
  - **Fog:** `EnvironmentFog` enabled with **Volumetric Fog on**: density 0.004, falloff 0.2, dark inscattering, scattering distribution 0.6, volumetric distance 80 m.
  - **Lighting:** a night look (directional light 0.3, cool tint; sky light 0.15) so beams dominate.
  - **Floor fix:** the `Floor` plane was actually at its unscaled 1 m size (contrary to #8). It is now scaled 40 × 40 so placement traces hit it everywhere.
  - **Pre-placed instances:** `MH_Test_1` "Spot 101" (open white, beam up), `MH_Test_2` "Spot 102" (blue, tilt 45°, zoom 20°), `LA_Test_1` "Main L 01" and `Truss_Test_1` "Truss DS 01".

**Files changed**
- `Source/ModularSceneBuilder/Public/UI/StageParameterRowWidget.h`, `Private/UI/StageParameterRowWidget.cpp`: `EditorSlot`, runtime editor creation, Color R/G/B editor
- `Config/DefaultGame.ini`: `/Game/StageCraft/Data` added to the StageItem scan directories
- `Content/StageCraft/UI/DA_StageCraftTheme.uasset`, `Content/StageCraft/UI/WBP_StageCraftHUD.uasset`: new
- `Content/StageCraft/UI/Inspector/WBP_StageInspectorPanel.uasset`, `WBP_StageParameterSection.uasset`, `WBP_StageParameterRow.uasset`: new
- `Content/StageCraft/Blueprints/BP_StageCraftPlayerController.uasset`, `BP_StageCraftGameMode.uasset`: new
- `Content/StageCraft/Data/DA_MovingHead_Test.uasset`, `DA_LineArray_Test.uasset`, `DA_Truss_Test.uasset`: new
- `Content/StageCraft/Maps/L_StageTest.umap`: GameMode override, volumetric fog, night lighting, floor scale, 4 test instances
- `STATE.md`, `tasks/todo.md`

**Verification**
- **Editor build:** after the `EditorSlot` change, the **Editor** target (`ModularSceneBuilderEditor`) was rebuilt by Claude with the editor closed: Succeeded, 0 errors. A first attempt failed on C4458 (a local named `Slot` shadowed `UWidget::Slot`); renaming it to `ChildSlot` fixed it.
  - The Game target was not rebuilt after the row change. It is the same module code, so rebuild it before a packaged test.
- **PIE run by Claude:** the mouse was driven on the PC and the editor window captured, because a scene-capture screenshot can't show UMG.
  - **Startup log:** `Game class is 'BP_StageCraftGameMode_C'`, the controller is `BP_StageCraftPlayerController_C`, `Fixture 1 registered (...), patch 1.001`, `Fixture 2 registered (...), patch 1.013` (auto-patch honours the 12-channel footprint), and `Stage item catalog loaded: 5 items.` No new warnings during PIE.
  - **Empty state:** the HUD shows INSPECTOR / "No Selection" plus the hint text.
  - **Selection:** LMB on Spot 102 selects it and shows the gizmo. The inspector fills with Info (label, model, 24.5 kg, 470 W), Transform (400, 300, 50), Patch (ID 2, Universe 1, Address 13, "Mode: Standard 12ch", 12 ch), Dimmer (100 / 0), Position (Pan 0, Tilt 45), Color (blue swatch, 10 / 30 / 100, White 0) and Beam (Zoom 20). Each section has its feature-group colour strip.
  - **Write path:** typing 25 into Dimmer and pressing Enter updates the row to 25.0, and the blue beam visibly dims in the viewport.
- **Editor viewport:** screenshots confirm the volumetric beams (white up, blue tilted) and that the fixture meshes and floor are present.
- **Not yet verified by Gevor.** Still open: gizmo drag with live Transform rows, the Color R/G/B edit, text-label edit, and selecting the speaker and truss (Audio / Rigging sections). The Phase 4 checklist (#8) is still open too.

**Commit:** uncommitted (working tree)

**Known issues / follow-ups**
- **Level-placed items need a construction rerun.** Setting `ItemData` on a level-placed actor through the MCP tool doesn't rerun construction. The actor showed defaults until the level was reloaded (OnConstruction runs on load). In the editor Details panel this is not an issue. Consider applying ItemData in `PostEditChangeProperty` explicitly for scripted edits.
- **Default styling of runtime editors:** the EditableTextBox is white with a large font, and SpinBoxes use the light default look, so the Label row stands out. Next step: style them from `UStageCraftUITheme` (add SpinBox/TextBox styles to the theme), or bind styled widgets in the row Blueprint.
- **Panel sizing:** the panel is a fixed 380 × 820 px canvas slot (the tool can't set stretch anchors), and units text clips at the right edge. Next step: stretch-anchor it top-to-bottom in the WBP designer and widen the units column.
- **Placeholder meshes:** everything uses 1 m engine shapes, so the moving head is about 2 m tall and the truss is a solid cube. Swap in real meshes (or add a mesh scale to `UBaseItemData`).
- **VC++ redistributable:** the editor warns that it is outdated (14.44 installed, 14.50 wanted). Install `D:\UE_5.8\Engine\Extras\Redist\en-us\vc_redist.x64.exe`. The warning dialog blocks unattended editor launches until dismissed.
- **No test script:** the test assets were created directly through MCP. There is no Python regeneration script; re-create them from the values above if needed.


---

## #11 — Editor-style RMB fly camera & always-on-top transform gizmo (2026-10-01)

**What & why**
Two UX problems made the tool feel unlike a professional editor. The camera could not be steered: look input was disabled so clicks would not swing the view, and the `ADefaultPawn` legacy bindings moved the camera on WASD at all times. The gizmo was also hidden whenever the selected item sat inside another mesh, because the engine `GizmoMaterial` is depth-tested.

- **RMB fly navigation (Unreal Editor style), in `AModularPlayerController`:**
  - **Press:** every RMB press starts navigating, because click vs. hold is only known on release. The controller saves the cursor position, hides the cursor and switches to `FInputModeGameOnly`. Slate then delivers raw high-precision mouse deltas with the cursor locked, so the view keeps turning past the screen edge. It also adds `CameraNavigationMappingContext` (priority 1) with `bIgnoreAllPressedKeysUntilRelease = false`, so a key held before RMB (W, then RMB) flies immediately.
  - **While held:**
    - Mouse turns the view: yaw, plus pitch clamped to ±89°. `LookSensitivity` (0.2°/count) is applied directly to the control rotation; engine look input stays ignored.
    - W/S move along the full view direction, A/D move right/left, E/Q move along world up/down. All of these go through `Pawn->AddMovementInput`.
    - The mouse wheel scales `FlySpeed` by `FlySpeedStepFactor` (1.25 per notch), clamped to 50–20000 cm/s. `ApplyFlySpeed` writes it to the pawn's `UFloatingPawnMovement`, with acceleration and deceleration at 8× speed for a snappy start and stop.
  - **Release:** removes the fly context, restores `FInputModeGameAndUI` and the cursor, and moves the cursor back to its saved position.
  - **Click vs. drag:** a release counts as a *click* (the old RMB behaviour: delete the item under the cursor, or else deselect and disarm) only if all of these hold:
    - mouse travel ≤ `SecondaryClickDragThreshold` (4 raw counts);
    - no fly key was used;
    - the button was held ≤ `MaxSecondaryClickDuration` (0.35 s). Holding longer never deletes, so deletion stays deliberate.
  - **No conflicts:**
    - LMB is ignored while navigating, because the cursor is hidden and frozen.
    - RMB is ignored during a gizmo drag or a placement stroke.
    - RMB on a UMG panel is consumed by UMG, as before.
    - `EndPlay` ends any active navigation.
  - **Input slots:**
    - `DeleteAction` is renamed to **`SecondaryAction`**. No Blueprint had it assigned (checked `BP_StageCraftPlayerController`).
    - New slots: `CameraNavigationMappingContext`, `CameraLookAction` (Axis2D, Mouse2D), `CameraMoveAction` (Axis3D; Swizzle/Negate modifiers map W/S/A/D/Q/E) and `CameraSpeedAction` (Axis1D, MouseWheelAxis). When the slots are empty, `BuildDefaultCameraMapping` builds all of them in code.
- **`AStageCameraPawn`** (new, `ADefaultPawn` subclass) is now the `DefaultPawnClass` of `AStageCraftGameModeBase`; `BP_StageCraftGameMode` inherits it.
  - `bAddDefaultMovementBindings = false`, so WASD/QE do nothing unless RMB is held.
  - Collision is off on the sphere and the mesh, so the camera flies through trusses and decks like the editor viewport.
- **Always-on-top gizmo:**
  - **New material `/Game/StageCraft/Gizmo/M_GizmoHandle`:**
    - Surface, Translucent, Unlit, Two Sided, **Disable Depth Test**.
    - Translucency pass **After DOF**, Responsive AA on.
    - Vertex fog, per-pixel fog and cloud fog off, so the `L_StageTest` volumetric fog does not wash it out.
    - VectorParameter `GizmoColor` drives Emissive; Opacity is 1.
  - **Gizmo material and render settings:**
    - The gizmo constructor loads `M_GizmoHandle` and falls back to the engine `GizmoMaterial` (depth-tested) only if the asset is missing. The existing per-axis DMIs (`GizmoColor`, highlight colour) work unchanged.
    - Handles get translucent sort priority 1000, so they draw over other translucency such as beams.
    - Handles are also kept out of decals, indirect lighting, distance-field lighting, ray tracing, reflection captures and sky captures.
  - **Selectable when buried:** the new `AModularTransformGizmo::TraceHandles(RayOrigin, RayDir, OutHit)` line-traces each active, collision-enabled handle component directly with `LineTraceComponent` and keeps the closest hit. A handle inside another mesh is therefore always grabbable, independent of other actors' collision responses. `GetGizmoHitUnderCursor` now uses it, and the controller's `GizmoTraceChannel` property is removed (unused). Handles still block only the Gizmo channel for any world trace.

**Files changed**
- `Source/ModularSceneBuilder/Public/Player/ModularPlayerController.h`, `Private/Player/ModularPlayerController.cpp`: RMB fly navigation, `SecondaryAction`, camera input slots and defaults, `IsNavigatingCamera()`, gizmo picking through `TraceHandles`
- `Source/ModularSceneBuilder/Public/Player/StageCameraPawn.h`, `Private/Player/StageCameraPawn.cpp`: new
- `Source/ModularSceneBuilder/Private/Game/StageCraftGameModeBase.cpp`: `DefaultPawnClass = AStageCameraPawn`
- `Source/ModularSceneBuilder/Public/Actors/ModularTransformGizmo.h`, `Private/Actors/ModularTransformGizmo.cpp`: `TraceHandles`, on-top material default with fallback, handle render flags
- `Content/StageCraft/Gizmo/M_GizmoHandle.uasset`: new. It was created headlessly by Claude with a Python commandlet (`UnrealEditor-Cmd -run=pythonscript -EnablePlugins=PythonScriptPlugin`), because the MCP bridge was taken by another project's editor.
- `STATE.md`, `tasks/todo.md`

**Verification**
- **Build:** `Build.bat ModularSceneBuilderEditor Win64 Development` Succeeded (Claude), with 0 errors and 0 warnings in project sources. The Game target was not rebuilt.
- **Headless commandlet check (Claude), 0 errors / 0 warnings:**
  - The gizmo CDO resolves `HandleMaterial = /Game/StageCraft/Gizmo/M_GizmoHandle`.
  - `StageCraftGameModeBase` and `BP_StageCraftGameMode` both use `StageCameraPawn`, with `bAddDefaultMovementBindings = False`.
  - The material reports `disable_depth_test = True`, Translucent, Unlit, and the `GizmoColor` parameter.
  - The first run, which created the asset, logged the expected one-time "CDO Constructor: Failed to find M_GizmoHandle".
- **Runtime behaviour is implemented, awaiting manual verification (Gevor).** PIE checklist:
  1. Hold RMB and move the mouse. The view turns, the cursor is hidden, and it is not stopped at the screen edge. On release the cursor reappears where it was.
  2. Hold RMB with W/A/S/D/Q/E: the camera flies and passes through geometry. The wheel while holding RMB changes speed. Without RMB, WASD does nothing.
  3. A quick RMB click on an item deletes it; on empty space it deselects and disarms. RMB with a drag or a hold over 0.35 s never deletes.
  4. LMB select, place and gizmo drag behave as before, with no camera movement.
  5. Move a selected item halfway into the floor or a truss. Arrows and rings stay fully visible, and dragging a buried handle works.
  6. The gizmo stays crisp and correctly coloured inside the volumetric fog in `L_StageTest`.

**Commit:** `b75a4ae`

**Known issues / follow-ups**
- Depth-test-free handles draw their far side through their near side. The colour is flat, so this reads as a solid shape, but overlapping axes in rotate mode all show (as in the UE editor). Hover highlight is still in the backlog.
- If the window loses focus mid-navigation, recovery relies on Enhanced Input flushing the RMB release. Check alt-tab during RMB in PIE.
- No orbit (Alt+LMB) or pan (MMB) yet. Only fly mode was in scope.
- Rebuild the Game target before a packaged test.


---

## #12 — Fix: placement dead after RMB delete; RMB camera rebuilt on the engine's native capture path (2026-10-01)

**What & why**
- **Bug: no placement after deleting (root cause found by reading the code; Gevor's PIE log showed no errors).**
  - In this build the only way to arm an item is `BP_StageTestArmer`. It calls `SelectItem(DA_Test_Crate)` once, on BeginPlay, and there is no catalog UI to re-arm.
  - `HandleSecondaryClick` (#3, kept in #11) called `UStageItemSubsystem::ClearSelection()` on every RMB click that did not hit a stage item. One right-click slightly off a cube, or a delete that missed, disarmed the crate. LMB then fell through to `BeginPlacement` with `ActiveItem == nullptr`, and placement was dead for the rest of the session.
  - #11 made a miss more likely. The click threshold was 4 raw mouse counts, and normal hand jitter on a high-DPI mouse exceeds that. The click trace also ran at a cursor position restored by `SetMouseLocation`, not at the press point.
  - **Fix:**
    - An RMB click on empty space now **only deselects**. The armed item stays armed.
    - Disarming is an explicit new **`CancelAction` (Esc)**, which deselects and disarms. In PIE, the editor uses Esc to stop play, so test Esc in Standalone or with the PIE stop key rebound.
    - The click trace runs at the **press position** (`PressCursorPosition`, captured on RMB Started) through `GetHitResultAtScreenPosition`.
    - The click threshold is now 12 counts.
- **Deterministic teardown on delete:**
  - The controller clears the selection *before* `TryDeleteActor` when the target is the selected actor, so the gizmo and inspector never see a selected actor that is mid-destruction. The `OnDestroyed` path stays as a backstop.
  - `USelectionComponent::SelectActor` and `USpawnSystemComponent::TryDeleteActor` reject actors already being destroyed (`IsActorBeingDestroyed`).
  - `TryDeleteActor` logs `Deleting stage item X.`
  - The audit found no stale state in `USpawnSystemComponent`: `BeginPlacement` always calls `EndPlacement` first, the stroke closes on Completed/Canceled, and `ActiveItem` changes only through the subsystem delegate.
- **RMB camera rebuilt: no input-mode switching and no context churn.**
  - **What #11 did, and why it was fragile:** every RMB press called `SetInputMode(GameOnly)`, toggled `bShowMouseCursor` and added the fly mapping context; release reversed all of it and called `SetMouseLocation`. This ran from Enhanced Input handlers, a tick after the Slate mouse events. It changed viewport focus and flush rules, and rebuilt key mappings while buttons were held. Those are the usual causes of a lost release (stuck in fly mode: cursor hidden, LMB ignored) and of the "camera locks" symptom.
  - **New `UStageCraftGameViewportClient`** (`GameViewportClientClassName` in `DefaultEngine.ini`, also used by PIE):
    - It tracks mouse buttons in `InputKey` and returns true from `HideCursorDuringCapture()` only while RMB is the only button held.
    - The engine's `FSceneViewport` then does what the Unreal Editor viewport does: at mouse-down it hides the cursor and uses high-precision raw mouse deltas, and at mouse-up it restores the cursor to the press point and releases capture.
    - LMB drags (gizmo, painting) keep a visible, moving cursor. Button state resets in `LostFocus` (alt-tab).
    - The controller warns at BeginPlay if this viewport client is not configured.
  - **Controller:**
    - It keeps one input mode for the whole session (GameAndUI).
    - The camera mapping context is applied once, in `SetupInputComponent`. Look, move and speed handlers act only while `bIsNavigatingCamera`.
    - `Begin/EndCameraNavigation` just set state.
    - Look, fly, wheel speed, the pawn and the click-vs-drag rules are otherwise as in #11.
- **Gizmo (request item 3):** re-checked. `M_GizmoHandle` is Translucent, Unlit, with `GizmoColor`; `bDisableDepthTest` was confirmed True in #11, and the gizmo CDO uses it. No change needed.

**Files changed**
- `Source/ModularSceneBuilder/Public/Player/StageCraftGameViewportClient.h`, `Private/Player/StageCraftGameViewportClient.cpp`: new
- `Config/DefaultEngine.ini`: `[/Script/Engine.Engine] GameViewportClientClassName=/Script/ModularSceneBuilder.StageCraftGameViewportClient`
- `Source/ModularSceneBuilder/Public/Player/ModularPlayerController.h`, `Private/Player/ModularPlayerController.cpp`: `CancelAction` (Esc), press-position click trace, no disarm on RMB miss, explicit deselect before delete, input-mode juggling removed, camera context always applied, threshold 12
- `Source/ModularSceneBuilder/Private/Components/SelectionComponent.cpp`: reject actors being destroyed
- `Source/ModularSceneBuilder/Private/Components/SpawnSystemComponent.cpp`: reject actors being destroyed; delete log
- `STATE.md`, `tasks/todo.md`

**Verification**
- **Build:** `Build.bat ModularSceneBuilderEditor Win64 Development` Succeeded (Claude), with 0 errors and 0 warnings. The editor was closed with `ue5_kill_editor`, rebuilt, and relaunched.
- **PIE startup (Claude):**
  - The log shows the built-in `LMB/RMB/Esc/Space` and RMB fly mappings, both fixtures registered, and the catalog loaded with 5 items.
  - The "not StageCraftGameViewportClient" warning did **not** appear, so the custom viewport client is active in PIE.
  - No new warnings or errors.
- **Not tested by Claude:** real mouse interaction. Synthetic mouse input from Claude's shell does not reach the desktop (a taskbar click had no effect), so the camera feel and the delete → place loop could not be driven automatically.
- **Awaiting manual verification (Gevor), PIE on `L_StageTest`:**
  1. LMB places a crate. RMB-click the crate: it is deleted (log `Deleting stage item ...`). LMB on the floor immediately places again.
  2. Right-click empty floor several times, then LMB: it still places.
  3. Hold RMB and move: the view turns with no cursor visible and no stop at the screen edge. Release: the cursor is back where you pressed.
  4. RMB + W/A/S/D/Q/E flies, the wheel changes speed, and WASD alone does nothing.
  5. Select a crate and LMB-drag a gizmo arrow: the cursor stays visible and the camera does not move. Pressing RMB during the drag does nothing.
  6. Alt-tab while holding RMB, then come back: the cursor is visible and LMB works.
  7. Push a selected crate into the floor: the gizmo stays visible and grabbable.

**Commit:** uncommitted (working tree)

**Known issues / follow-ups**
- Esc stops PIE in the editor. Rebind the PIE stop key or test Cancel in Standalone. The real catalog UI should also offer a "pointer / no item" button.
- `BP_StageTestArmer` is the only arming path until the UMG catalog panel exists (5.3).


---

## #13 — Editor-viewport fly camera rewrite (damped, world-Z vertical) & RMB made navigation-only (2026-10-01)

**What & why**
Gevor reported that the camera still felt clunky, especially when moving vertically and looking around. There were two root causes:
1. **Movement went through `UFloatingPawnMovement`** (inherited from `ADefaultPawn`). It normalises the *summed* input vector: E plus W or A/D shared one unit of input, so climbing slowed when strafing. Its acceleration/braking model, with `TurningBoost`, is tuned for pawns, not cameras.
2. **Look applied raw high-precision mouse deltas straight to the control rotation.** Raw deltas arrive in uneven per-frame bursts, and every burst showed up as a jerk.

**Changes:**
- **`AStageCameraPawn` rewritten as a plain `APawn`** (root `USceneComponent`, eye height 0, no movement component, no collision). Motion is integrated directly (`AddActorWorldOffset`, no sweep), so no clamp, normalisation or sweep can distort it.
  - **Look:** `AddLookInput(raw counts)` moves a **target** rotation: `LookSensitivity` 0.2°/count, pitch clamped to ±`MaxPitch` (89°), roll always 0. The view eases toward the target with frame-rate-independent exponential smoothing (`RotationSmoothing` 25/s, about 40 ms of glide), along the shortest path across the ±180° yaw seam. The pawn writes the result to the controller's control rotation. When it is at rest, it adopts rotations set by others (spawn, game-mode restart) instead of fighting them.
  - **Fly:** `AddFlyInput(x = forward, y = right, z = up)` is accumulated per frame.
    - W/S move along the full look vector and A/D along the view's horizontal right vector, as in the UE editor.
    - **E/Q move strictly along world Z on a separate axis.** The forward/strafe part is clamped to unit length on its own, so vertical speed never depends on pitch or on other held keys.
    - Velocity eases toward the target with the same exponential model (`MovementSmoothing` 8/s, about a 0.12 s ease-in/out) and snaps to rest below 0.5 cm/s, so idle costs nothing.
    - `AdjustFlySpeed` (wheel): ×1.25 per notch, clamped 50–20000 cm/s.
    - `SetViewRotation(rot, bSnap)` is available for future "focus selection".
  - The pawn ticks after its controller (`AddTickPrerequisiteActor` in `PossessedBy`), so input applies in the same frame. All tuning properties live on the pawn. The input API is BlueprintCallable (gamepad and UI later).
- **`AModularPlayerController` simplified to pure input routing.** All camera maths and the `FloatingPawnMovement` dependency are removed.
  - **RMB is navigation only.** `NavigateAction` (was `SecondaryAction`): Started sets `bIsNavigatingCamera`, Completed/Canceled clears it. While it is held, look/move/speed are forwarded to the pawn. It never selects, places or deletes, and it is ignored while a gizmo drag or placement stroke is active.
  - **LMB is ignored while navigating.**
  - **Delete moved to the Delete key** (`DeleteAction`): deletes the *selected* item (the selection is cleared first). This **supersedes #12's RMB-click delete** and removes the click-vs-drag thresholds and the press-position trace.
  - Esc (Cancel: deselect + disarm) and Space (gizmo mode) are unchanged. The Space "consume legacy keys" workaround is removed, because no legacy bindings exist any more.
  - Space/Ctrl are **not** mapped as up/down: Space is the gizmo toggle, and a statically mapped key would steal it at any priority (Enhanced Input claims keys when it rebuilds mappings). Ctrl is reserved for multi-select. Q/E match the UE editor defaults.
- `UStageCraftGameViewportClient` (#12) still owns cursor hide, raw deltas and cursor restore on RMB.

**Files changed**
- `Source/ModularSceneBuilder/Public/Player/StageCameraPawn.h`, `Private/Player/StageCameraPawn.cpp`: rewritten (`APawn`, damped look/fly)
- `Source/ModularSceneBuilder/Public/Player/ModularPlayerController.h`, `Private/Player/ModularPlayerController.cpp`: `NavigateAction` / `DeleteAction` (Delete key); RMB-click delete, the thresholds, `LookSensitivity`/`FlySpeed` (moved to the pawn) and `ApplyFlySpeed` removed
- `STATE.md`, `tasks/todo.md`

**Verification**
- **Build:** `Build.bat ModularSceneBuilderEditor Win64 Development` Succeeded twice (Claude), with 0 errors and 0 warnings. The editor was closed, rebuilt and relaunched.
- **Automated in-PIE flight test (Claude).** A Python slate-tick script called the pawn's real `AddFlyInput`/`AddLookInput` every frame on `L_StageTest` and recorded the pose. It ran at about 110 fps; the editor's background CPU throttle was turned off for the run and restored afterwards.

  | Phase (view pitch −45°) | Result |
  |---|---|
  | E only, 1 s | dX = dY = 0.0 exactly. vZ rises smoothly and monotonically to 1,200 cm/s (5-sample windows: 227 → 528 → 742 → 883 → 933 → 1,085 → … → 1,200) |
  | Release | Coasts 144 cm (model: v/k = 150), then 2 cm/s → rest |
  | Q only | Pure −Z, symmetric with E |
  | W + E | Horizontal 848 cm/s along the look vector, plus a full-strength climb (net vZ = 1,200 − 848 = 351), not normalised away |
  | Look far past vertical | Pitch eases with a per-frame decay of 0.80 (= e^(−25·0.009)), stops at **89.0°**, roll 0.00, no flip |
  | Large yaw input | Wraps across ±180° without spinning the long way; roll stays 0 |

  - The PIE log shows no StageCraft warnings or errors.
- **Not tested by Claude:** the feel with a physical mouse. Synthetic OS mouse input does not reach the desktop from Claude's shell.
- **Awaiting manual verification (Gevor), PIE:**
  1. **Look:** hold RMB and look around; it should feel 1:1 with a soft glide, with no jitter and no flip at the top or bottom.
  2. **Climb:** RMB + E/Q gives straight vertical moves at any pitch; RMB + W+E climbs at full speed while moving forward.
  3. **Stop:** releasing RMB eases to a stop in about 0.1–0.2 s.
  4. **Speed:** RMB + wheel changes speed.
  5. **No interference:** quick RMB clicks never select, place or delete. LMB does nothing while RMB is held. RMB does nothing during an LMB gizmo drag.
  6. **Delete:** select an item, press Delete; it is removed, and LMB places again immediately.

**Commit:** uncommitted (working tree, together with #12)

**Known issues / follow-ups**
- **Tuning:** `RotationSmoothing`, `MovementSmoothing`, `LookSensitivity` and `FlySpeed` are on `AStageCameraPawn`. Set them in a BP subclass, then point `DefaultPawnClass` at it.
- **Not yet built:** orbit (Alt+LMB), pan (MMB), wheel dolly without RMB, and focus-selection (F; `SetViewRotation` is ready for it).
- **PIE caveat:** Esc stops PIE in the editor (see #12).


---

## #14 — Mouse wheel sets fly speed at any time, uniform on all axes, with feedback (2026-10-01)

**What & why**
Gevor reported that scrolling did not change camera speed, or did not seem to apply to every direction.

**Root cause:** `AModularPlayerController::HandleCameraSpeed` (#13) acted only while RMB was held, and nothing showed the current speed. Scrolling on its own did nothing, and a change made during flight was invisible. The speed itself was already uniform: `AStageCameraPawn::TickMovement` multiplies the combined forward/strafe/vertical target by one `FlySpeed`. The automated test below confirms this per axis.

- **Wheel works with or without RMB.** The wheel has no other job in the stage view. UMG panels still consume wheel input over their scroll boxes, so the inspector scrolls normally.
- **`AStageCameraPawn::AdjustFlySpeed` now returns the new speed and broadcasts `OnFlySpeedChanged(float)`** (BlueprintAssignable) when the value actually changes, for a HUD readout.
  - Only the target speed changes. The real velocity eases toward it through the existing `MovementSmoothing`, so scrolling mid-flight speeds up or slows down smoothly instead of jumping.
- **Range widened:** `MinFlySpeed` 50 → **10 cm/s** (close-up rigging), `MaxFlySpeed` stays 20,000 cm/s (200 m/s, arena layouts). Steps stay geometric at ×1.25 per notch (about 34 notches end to end), so each notch feels the same at any speed.
- **Interim feedback:** in non-shipping builds the controller shows `Camera speed: X.XX m/s` with `AddOnScreenDebugMessage`. It uses a fixed key, so each notch replaces the previous line, and the line fades after 1.5 s. The HUD should bind `OnFlySpeedChanged` and replace this.

**Files changed**
- `Source/ModularSceneBuilder/Public/Player/StageCameraPawn.h`, `Private/Player/StageCameraPawn.cpp`: `FOnStageCameraFlySpeedChanged` / `OnFlySpeedChanged`, `AdjustFlySpeed` returns float, `MinFlySpeed` 10, comments
- `Source/ModularSceneBuilder/Public/Player/ModularPlayerController.h`, `Private/Player/ModularPlayerController.cpp`: wheel no longer gated on RMB, on-screen speed readout (`#if !UE_BUILD_SHIPPING`), `Engine/Engine.h` include
- `STATE.md`, `tasks/todo.md`

**Verification**
- **Build:** `Build.bat ModularSceneBuilderEditor Win64 Development` Succeeded (Claude), with 0 errors and 0 warnings. The editor was closed, rebuilt and relaunched.
- **Automated in-PIE speed test (Claude).**
  - **Method:** wheel notches were injected with `EnhancedInputLocalPlayerSubsystem.InjectInputVectorForAction(IA_CameraSpeed_Default)`, which is the real controller path, **with RMB not held**. Fly input went through `AddFlyInput`. Velocity was computed from the world delta time at about 114 fps, with the background throttle off for the run and restored afterwards.

  | Check | Result |
  |---|---|
  | 3 notches up, no RMB | `FlySpeed` 1,200 → **2,343.75** (= 1200·1.25³) |
  | Forward only | steady 2,343 cm/s on X, Y = Z = 0 |
  | Right only | steady 2,343 cm/s on Y |
  | Up (E) only | steady 2,343 cm/s on Z, so the speed applies uniformly to all three axes |
  | 5 notches down **mid-flight** | 2,343.75 → **768.00**. Velocity eases down; the largest single-frame change is 103 cm/s at 8.8 ms frames (model: 1,575·(1−e^(−8·0.0088)) = 107), so no jump |
  | −60 / +60 notches | clamps at exactly **10** and **20,000** cm/s |

  - The PIE log shows no StageCraft warnings or errors.
- **Awaiting manual verification (Gevor), PIE:**
  1. Scroll without RMB: the on-screen speed changes.
  2. Hold RMB and fly with W, A/D, E/Q while scrolling: all directions speed up and slow down together and smoothly.
  3. Scroll over the inspector panel: the panel scrolls and the camera speed does not change.

**Commit:** `364686c` (together with #12 and #13)

**Known issues / follow-ups**
- Bind `OnFlySpeedChanged` in `WBP_StageCraftHUD` for a proper speed indicator, then remove the debug-message readout.
- The speed is not saved between sessions. If wanted, store it in a `USaveGame` or user settings later.


---

## #15 — Look speed follows fly speed (square-root curve, clamped) (2026-10-01)

**What & why**
Gevor reported that the wheel sped up flying but looking around stayed fixed, so a fast flight across the stage turned "painfully slowly". He asked to scale look sensitivity with `FlySpeed`.

- **Not strictly proportional.** Fly speed spans 10 cm/s – 200 m/s (2,000×, #14). A 1:1 link would give about 0.001°/count at the slow end (look frozen) and about 400°/count at the fast end (a full spin per twitch).
- **Implemented curve:** `AStageCameraPawn::GetLookSpeedScale()` = clamp((FlySpeed / `LookSpeedReferenceFlySpeed`)^`LookSpeedScaleExponent`, `MinLookSpeedScale`, `MaxLookSpeedScale`).
  - Defaults: reference 1,200 cm/s (the default fly speed, scale 1.0), exponent **0.5** (square root: each doubling of fly speed turns about 1.41× faster), clamp **×0.5 … ×3.0**.
  - With the defaults, the ×0.5 floor applies at and below 300 cm/s (finer aim for close-up rigging), and the ×3.0 ceiling at and above 10,800 cm/s.
- **Smoothing is unchanged.** `AddLookInput` multiplies the raw delta by `LookSensitivity * GetLookSpeedScale()` into the **target** rotation, and `RotationSmoothing` (25/s) still eases the view toward it. A faster look is as smooth as before, and a wheel notch mid-turn changes the rate without a jump.
- **`bScaleLookWithFlySpeed`** (default on) turns this off for stock UE-editor behaviour, where look speed is fixed.
- The interim on-screen readout now shows `Camera speed: X.XX m/s | look xN.NN`.

**Files changed**
- `Source/ModularSceneBuilder/Public/Player/StageCameraPawn.h`, `Private/Player/StageCameraPawn.cpp`: `GetLookSpeedScale()` (BlueprintPure); `bScaleLookWithFlySpeed`, `LookSpeedReferenceFlySpeed`, `LookSpeedScaleExponent`, `Min/MaxLookSpeedScale`; `AddLookInput` uses the scale
- `Source/ModularSceneBuilder/Private/Player/ModularPlayerController.cpp`: readout includes the look scale
- `STATE.md`, `tasks/todo.md`

**Verification**
- **Build:** `Build.bat ModularSceneBuilderEditor Win64 Development` Succeeded (Claude), with 0 errors and 0 warnings. The editor was closed, rebuilt and relaunched.
- **Automated in-PIE look test (Claude).**
  - **Method:** fly speed was set by injecting wheel notches into `IA_CameraSpeed_Default`, the real controller path. An identical sweep of 300 counts of yaw (10 counts/frame × 30 frames) was then applied through `AddLookInput`, and the settled turn measured. The background throttle was off for the run and restored afterwards.

  | Fly speed | Look scale | Turned | Expected (300 · 0.2 · scale) |
  |---|---|---|---|
  | 1,200 cm/s | 1.000 | 60.00° | 60.00° |
  | 393 cm/s | 0.572 (= √(393/1200)) | 34.34° | 34.35° |
  | 10 / 93 / 182 cm/s | 0.500 (floor) | 30.00° | 30.00° |
  | 20,000 cm/s | 3.000 (ceiling) | 180.00° | 180.00° |

  - **Smoothness:** per-frame yaw steps ease in (0.19° → 0.97°) and out (→ 0.04°) with no jumps.
  - The test's case labels used wrong notch arithmetic. The speeds above are the measured ones; the high-middle of the curve (for example 3,600 cm/s → ×1.73) follows from the same verified formula but was not sampled.
  - The PIE log shows no StageCraft warnings or errors.
- **Awaiting manual verification (Gevor), PIE:** scroll up until the readout shows about ×2–3 and check that hold-RMB looking is clearly faster but still controllable. Scroll down to about 1–3 m/s and check that look is finer for precise aiming. If the link is too strong or too weak, tune `LookSpeedScaleExponent` (0 = off, 1 = proportional) or the clamps in a BP subclass of `AStageCameraPawn`.

**Commit:** `364686c` (together with #12–#14)

**Known issues / follow-ups**
- Unlike the stock UE editor, look speed is tied to fly speed. This is intentional, at Gevor's request, and can be disabled with `bScaleLookWithFlySpeed`.
- Working tree also holds `Content/StageCraft/Blueprints/BP_StageCameraPawn.uasset` (new BP child of `AStageCameraPawn`, all defaults) and a modified `BP_StageCraftGameMode.uasset` (`DefaultPawnClass` = `BP_StageCameraPawn`). They were saved in the editor at 17:06 and were not created by Claude. They are left **unstaged** for Gevor to review. The #15 PIE test ran with this BP pawn; its values equal the C++ defaults, so the results hold.


---

## #16 — Phase 5 Part 2 UI: shared parameter binding, dropdown rows, fader & encoder bank (2026-10-01)

**What & why**
The inspector (#9/#10) already built typed rows from `IStageParameterInterface` sections and refreshed them from `AModularBaseActor::OnParameterChanged`. The gaps against the GrandMA3-style brief were:
- no dropdown/enum rows;
- live updates overwrote a spin box the user was typing into or dragging (only the Text row had a focus guard);
- structural changes rebuilt immediately, even from inside the widget that caused them;
- no fader/encoder bar.

The binding logic was also private to the inspector, so a second panel would have had to copy it.

**UI architecture (all `Source/ModularSceneBuilder/.../UI/`)**

```
UStageParameterViewWidget  (abstract)       UStageParameterControlWidget  (abstract)
  selection follow, bind target,              one parameter: descriptor, RefreshValue,
  commit + read-back, deferred rebuild        CommitValue, focus-safe ApplyValue
   ├─ UStageInspectorPanel   (sections+rows)   ├─ UStageParameterRowWidget  (inspector row)
   └─ UStageFaderBankWidget  (strips)          ├─ UStageFaderWidget        (fader / toggle / color channel)
                                               └─ UStageEncoderWidget      (painted dial)
```

- **`UStageParameterViewWidget`** (new base of every parameter panel):
  - **Selection:** follows the owning `AModularPlayerController`'s `USelectionComponent` (`bFollowSelection`), or shows whatever `Inspect()` is given.
  - **Live refresh:** binds `OnParameterChanged`. Each change refreshes every registered control for that id directly, with no tick or polling, so gizmo, cue, DMX and other-panel edits appear the same frame.
  - **Commit:** `CommitParameter(Id, Value)` (BlueprintCallable) writes through the interface, then **always reads back**. An edit that clamps to the current value fires no event, but still snaps the control to the truth.
  - **Structural changes** (invalid tag) go through `RequestRebuild()`: next tick, coalesced, never inside the triggering widget's callback.
  - Owns `Theme` and `GetGroupColor`. Exposes `GetNumControls()` / `FindControl()`, which are used by the tests.
  - The subclass hooks are `ClearView`, `BuildView`, `OnViewUpdated` and `OnParameterRefreshed`.
- **`UStageParameterControlWidget`** (new base of every control):
  - Owns the descriptor, group color, the `OnCommitted` delegate, `RefreshValue` and `CommitValue`.
  - **Focus guard:** `ApplyValue(Value, bForce)` skips widgets for which `IsUserInteracting()` is true (keyboard focus inside, or mouse capture), so a cue or DMX frame never yanks a dragged fader or overwrites half-typed text. The value is still stored. `FinishInteraction()` re-applies it, forced, when the edit ends.
  - `FormatValue` (shared) and `ToDisplay` / `FromDisplay` (display-scale and integer rounding).
- **`UStageInspectorPanel`** is now a thin `BuildView` (sections and rows by type) plus header and empty state. All its UPROPERTY and BindWidget names are unchanged; `Theme` moved to the base under the same name. `WBP_StageInspectorPanel` keeps its settings.
- **`UStageParameterRowWidget`:**
  - **New: Enum rows** — a `ValueComboBox` (`UComboBoxString`), bound or auto-created in `EditorSlot`. It is filled from `Descriptor.Options`. Only user picks commit (`ESelectInfo::Direct` is ignored), and an open dropdown is never overwritten.
  - **Focus guard on spin boxes:** the single, Vector, Rotator and Color spin boxes are focus-guarded. `OnValueCommitted` / `OnEndSliderMovement` call `FinishInteraction`.
- **Parameter model (`StageParameterTypes`):**
  - `EStageParameterType::Enum` (appended last, so serialized values stay stable).
  - `FStageParameterValue::MakeEnum`, `FStageParameterDescriptor::Options` / `WithOptions()`.
  - New tag `StageCraft.Param.Info.Type`.
- **Item type selector (`AModularBaseActor`):**
  - **Type row:** the Info section gets a **Type** dropdown when more than one catalog item has the same data class and `ItemType` (`GetSwappableItems`; the current item is always listed, even if it is outside the catalog).
  - **Swap:** choosing a type calls `InitializeFromItemData` in place. Transform, label, fixture ID and patch are kept; `ApplyItemData` adapts meshes and the attribute set.
  - **Rebuild:** `InitializeFromItemData` now broadcasts a rebuild (invalid tag) after BeginPlay, so every open view rebuilds.
  - *DMX modes:* `ULightingFixtureData` has a single `DMXModeName`, so there is no mode list to offer yet (see follow-ups).
- **`UStageFaderWidget`** (new, non-abstract): a console channel strip showing name, vertical `USlider` and readout.
  - **Ranged numbers:** Float/Integer with a range span the display range (Dimmer 0–100 %, Gain −60–12 dB). The mouse wheel nudges by 1 % of the range (Shift: 0.1 %) and is **Handled**, so it never also changes camera fly speed.
  - **Bool:** shown as a toggle (Mute, Polarity).
  - **Color channel:** `SetColorComponent(0..2)` edits one channel of a Color parameter, with the bar tinted red, green or blue.
  - Builds a default tree in `NativeOnInitialized` when the class has no designed tree. Optional BindWidget names: `NameText`, `ValueSlider`, `ValueToggle`, `ValueText`, `GroupColorStrip`.
- **`UStageEncoderWidget`** (new, non-abstract): a painted 270° dial (ring, value arc in the group color, pointer) via `NativePaint`.
  - Left-drag anywhere on it: right/up increases, full range = `PixelsPerFullRange` (300 px), Shift ×0.1. The drag accumulates its own value, so clamping never makes it sticky.
  - Mouse wheel: step = descriptor `Step` or 1 % of the range (Shift ×0.1).
  - `FinishInteraction` runs on release or capture loss. A default tree is built when needed; optional names: `NameText`, `ValueText`, `DialArea`.
- **`UStageFaderBankWidget`** (new, non-abstract): the quick-access bar for the selection.
  - **What it shows:** sections whose feature group is in `FeatureGroups` (default Dimmer, Color, Position, Beam, Audio, in item order, with `GroupSpacing` between groups).
  - **Controls per parameter:** ranged numbers get faders, or encoders for `EncoderParameters` (default Pan, Tilt). Bools get toggles; Color gets three channel faders bound to the one color parameter. Read-only, text, vector and enum parameters stay in the inspector.
  - **Header and empty state:** `TitleText` reads "FADERS | <label>"; `EmptyState` shows a hint.

**Content**
- **`/Game/StageCraft/UI/Faders/WBP_StageFaderBank`** (new, parent `UStageFaderBankWidget`):
  - Layout: `Border_Root` (dark, 92 % opaque) → `BarColumn` → `TitleText` (amber, 9 pt), `EmptyState` (grey, 8 pt), `FaderScroll` (horizontal) → `FaderContainer`.
  - Theme: `DA_StageCraftTheme`.
- **`/Game/StageCraft/UI/WBP_StageCraftHUD`** rebuilt with `Inspector` (unchanged slot: top-right, −12/12, 380 × 820) plus **`FaderBank`** (bottom-left, 12/−12, auto-size).
  - **Why rebuilt:** `ue5_add_widget_to_blueprint` ignores `user_widget_class` and silently inserts a TextBlock (plugin bug, see follow-ups).
  - **How:** the HUD was recreated through `ue5_create_widget_blueprint` at a temporary path. `BP_StageCraftPlayerController.HUDWidgetClass` was re-pointed, then the old HUD was deleted and the new one renamed back to `WBP_StageCraftHUD`. The controller now references `/Game/StageCraft/UI/WBP_StageCraftHUD.WBP_StageCraftHUD_C`, with no redirector left.
  - The old HUD's graph held only the default placeholder events.
- **`/Game/StageCraft/Data/DA_MovingHead_Wash_Test`** (new): a duplicate of `DA_MovingHead_Test`, "Test Moving Head Wash", zoom 8–55° (the spot is 4–40°). It gives the Type dropdown a real choice.

**Files changed**
- **New:**
  - `Public/UI/StageParameterViewWidget.h`, `Private/UI/StageParameterViewWidget.cpp`
  - `Public/UI/StageParameterControlWidget.h`, `Private/UI/StageParameterControlWidget.cpp`
  - `Public/UI/StageFaderWidget.h`, `Private/UI/StageFaderWidget.cpp`
  - `Public/UI/StageEncoderWidget.h`, `Private/UI/StageEncoderWidget.cpp`
  - `Public/UI/StageFaderBankWidget.h`, `Private/UI/StageFaderBankWidget.cpp`
- **Rewritten / modified:**
  - `Public/UI/StageInspectorPanel.h`, `Private/UI/StageInspectorPanel.cpp`
  - `Public/UI/StageParameterRowWidget.h`, `Private/UI/StageParameterRowWidget.cpp`
  - `Public/Data/StageParameterTypes.h`, `Private/Data/StageParameterTypes.cpp`
  - `Public/Actors/ModularBaseActor.h`, `Private/Actors/ModularBaseActor.cpp`
- **Content:**
  - New: `Content/StageCraft/UI/Faders/WBP_StageFaderBank.uasset`, `Content/StageCraft/Data/DA_MovingHead_Wash_Test.uasset`
  - Modified: `Content/StageCraft/UI/WBP_StageCraftHUD.uasset`, `Content/StageCraft/Blueprints/BP_StageCraftPlayerController.uasset`
- `STATE.md`, `tasks/todo.md`

**Verification**
- **Builds (Claude), editor closed:**
  - `Build.bat ModularSceneBuilderEditor Win64 Development`: Succeeded, 0 errors, 0 warnings. The first attempt failed on UHT `Units = "px"` (not a valid unit) and was fixed by removing that meta.
  - **Game target** `Build.bat ModularSceneBuilder Win64 Development`: Succeeded, 0 errors, 0 warnings.
- **Automated in-PIE UI test (Claude), `L_StageTest`, against the real HUD instances (`WBP_StageInspectorPanel_C`, `WBP_StageFaderBank_C`):**

  | Check | Result |
  |---|---|
  | Select Spot 102 | Inspector 18 rows (including Type); fader bank 9 strips: Dimmer, Strobe faders, **Pan/Tilt encoders**, R/G/B channel faders, White, Zoom |
  | Fader → fixture (`CommitValue` 0.42) | fixture 0.42, inspector row 0.42, fader 0.42 |
  | Fixture → UI (`SetAttribute` 0.8, the cue/DMX path) | inspector row 0.8, fader 0.8 |
  | Commit 5.0 (out of range) | fixture clamps to 1.0; fader and row read back 1.0 |
  | Focus guard | focused Dimmer spin box keeps 100 while the fixture goes to 0.25 (row still tracks 0.25); after focus moves, it shows the live 30 |
  | Type dropdown | options [Spot, Wash]; a row commit swaps to `DA_MovingHead_Wash_Test`. **Same frame:** old widgets, Zoom max 40 (no rebuild inside the callback). **Next tick:** a *new* Zoom fader with max 55. Swap back restores 40 |
  | Select line array | Gain, Delay, Splay faders; Mute, Polarity toggles |
  | Clear selection | both panels have 0 controls |

  - The PIE log has no StageCraft/UMG warnings or errors during the test runs.
  - The background CPU throttle was off for the run and restored afterwards.
- **Not verified by Claude (it cannot see or drive the UI):** the look of the bar, actual mouse drag on faders and encoders, and the wheel being consumed over the strips.
- **Awaiting manual verification (Gevor), PIE:**
  1. Select a moving head. The bar appears bottom-left; drag Dimmer and R/G/B and the light changes live.
  2. Drag the Pan/Tilt encoders (Shift = fine); the head moves.
  3. Scroll over a fader: it nudges, and camera speed does not change.
  4. While dragging a fader, the inspector row follows (and the reverse).
  5. Change Type to Wash: the beam range changes and both panels rebuild.
  6. Select the speaker: Mute and Polarity toggles work.

**Commit:** 1ed42e8

**Known issues / follow-ups**
- **Plugin bug (UnrealNGGMCP):** `HandleAddWidgetToBlueprint` in `Plugins/UnrealNGGMCP/Source/UnrealNGGMCP/Private/NGGWidgets.cpp` resolves types with a local `ResolveWidgetClass(TypeName)` that ignores `user_widget_class`, so `type: UserWidget` silently becomes a TextBlock. The create handler uses `NGGWidgetPriv::ResolveWidgetClass(TypeName, UserWidgetClassPath)` correctly. This was not patched (plugin code); it is a one-line fix if wanted.
- **DMX modes:** `ULightingFixtureData` has one `DMXModeName`. A mode dropdown needs a `TArray` of modes (name + channel layout) and a per-instance mode index in the patch. The Enum row type is ready for it.
- **Color:** the inspector Color row is still swatch + R/G/B percent; the bank gives R/G/B faders. An HSV wheel / colour picker WBP remains in 5.3.
- **Multi-select:** the views show one object. GrandMA-style multi-fixture editing needs the selection component to hold a set and the view to fan out commits.
- **Strip layout and style:** the default strip layout is C++-built (58 px faders, 70 px encoders, built-in colours). For designer control, create WBP subclasses of `UStageFaderWidget` / `UStageEncoderWidget` with the named widgets and assign them in `WBP_StageFaderBank` (`FaderWidgetClass` / `EncoderWidgetClass`).

## #17 — Pan/Tilt encoder usability: vertical delta drag, fine mode, value popup, double-click reset (2026-10-01)

**Request:** make the Pan/Tilt dials easy to use with a mouse while keeping their look. Requirements:
- free-form vertical drag;
- Shift for 5x finer control;
- a live value popup ("Pan: 45°") and hover/active feedback;
- double-click to reset;
- smooth easing, safe clamping, and no value or cursor jump when a drag starts.

**What changed (`UStageEncoderWidget`)**
- **Drag:**
  - Press anywhere on the strip and drag vertically (up increases).
  - `PixelsPerFullRange` (300 px) covers the whole range. On a Pan of -270..270 that is 1.8° per px.
  - Only the Y delta counts, so the arc no longer needs tracing and where you press does not matter.
- **Click vs drag:** a press stays a click until it travels past Slate's drag-trigger distance (5 px), and that travel is not applied, so starting a drag never jumps the value. Then:
  - The encoder switches to the high-precision mouse: the cursor is hidden and raw deltas mean screen edges never stop a drag.
  - On release the cursor reappears exactly where it vanished. The anchor is taken from the OS cursor, so it lands on the exact pixel.
  - Switching to high precision needs a re-capture, which reports our own capture as lost. A one-shot `bIgnoreNextCaptureLost` guard keeps the drag alive.
- **Fine mode:** `FineScale` = 0.2 (5x finer) while Shift is held, for both drag and wheel. Toggling Shift mid-drag causes no jump, because the drag is delta-based.
- **Clamping:** the drag accumulator is clamped on every step, so pushing past a limit builds no dead zone and the first move back responds at once. Integer parameters round only on commit. The value the object holds is always read back (`FinishInteraction`).
- **Double-click:** `ResetToDefault()` (BlueprintCallable). It uses the new descriptor default (`FStageParameterDescriptor::DefaultValue` / `bHasDefault` / `WithDefault()`), which fixtures fill from `ULightingFixtureData::GetAttributeDefault`: Pan/Tilt home at 0, Zoom at its widest. Without a default it falls back to 0 clamped into range.
- **Feedback:**
  - Hovering brightens the ring and arc.
  - An active drag adds a group-colour halo and a thicker arc and pointer.
  - The cursor shows `ResizeUpDown` over editable encoders.
  - The needle eases toward the value using `1 - e^(-k·dt)` with `NeedleSmoothing` = 28, so cue and DMX jumps read as motion. The readout text is always exact.
- **Value popup:**
  - It shows "Pan: 45.0°", plus "fine" while Shift is held. It is painted above the dial in a rounded box with a group-colour outline.
  - It appears while dragging and for `PopupLingerTime` (0.9 s) after a release, wheel step or reset.
  - It clips to its own zone without intersecting the parent, so the fader bank's scroll box does not cut it off. It is drawn 100 layers up so neighbouring strips do not cover it.
  - New queries: `IsDragging()`, `IsPopupVisible()`, `GetPopupText()`.
- **Wheel during a drag:** it is consumed and ignored, so it cannot fight the drag accumulator.
- **Degree unit:** now "°" attached to the number (`45.0°`) everywhere: Pan/Tilt/Zoom, speaker Splay and the inspector rotation row. `FormatValue` adds no space for "°" only.

**Files changed**
- `Source/ModularSceneBuilder/Public/UI/StageEncoderWidget.h`, `Private/UI/StageEncoderWidget.cpp`
- `Source/ModularSceneBuilder/Public/Data/StageParameterTypes.h` (descriptor default)
- `Source/ModularSceneBuilder/Private/UI/StageParameterControlWidget.cpp` (degree formatting)
- `Source/ModularSceneBuilder/Private/Actors/LightingFixtureActor.cpp` (defaults, °), `AudioEquipmentActor.cpp` (°), `ModularBaseActor.cpp` (°)
- `STATE.md`, `tasks/todo.md`

**Verification**
- **Builds (Claude), editor closed:** `ModularSceneBuilderEditor` and **Game target** `ModularSceneBuilder` (Win64 Development) both succeeded with 0 errors and 0 warnings.
- **Automated in-PIE input test (Claude):**
  - Setup: `L_StageTest`, Spot 102 selected, real HUD encoder instances.
  - Method: synthetic pointer events were sent through `FSlateApplication` (the real routing path: hit test, capture, high-precision mouse, double-click), using a temporary console command.
  - Cleanup: the harness file was deleted afterwards and both targets were rebuilt without it.
  - Results for Pan (-270..270):

  | Step | Result |
  |---|---|
  | Press | value unchanged, mouse captured, not dragging |
  | Move 2 px (below the 5 px threshold) | value unchanged, not dragging |
  | Move 6 px more (crosses the threshold) | dragging, **value unchanged (no jump)**, capture kept, high-precision on, popup on |
  | Drag up 30 px | +54.0° (1.8°/px) |
  | Shift + up 10 px | +3.6° (5x finer) |
  | Down 5000 px | clamps at -270 |
  | Up 10 px right after | -252 (no dead zone) |
  | Release | not dragging, capture released, high-precision off, popup lingers |
  | Click without moving | value unchanged |
  | Double-click | resets to 0.0° |
  | Wheel +1 / Shift + wheel +1 | +5.4° / +1.08° |
  | Popup text | `Pan: 54.0°` etc.; still visible a few frames after the input, gone after ~3 s |

  - Tilt (-135..135) gave matching results: 0.9°/px, 0.18°/px with Shift, clamp, reset to 0.
  - The first run returned the cursor 1 px off, because the anchor was rounded from the fractional synthetic position. The anchor now comes from the OS cursor and was rebuilt, but that build was not re-tested.
  - Log: no StageCraft/UMG warnings or errors. The background CPU throttle was off for the run and restored to True afterwards.
- **Not verified by Claude:** the look of the popup and highlights (the screen grab came back black) and the feel with a physical mouse.
- **Awaiting manual verification (Gevor), PIE:**
  1. Hover a Pan/Tilt dial: the ring brightens and the cursor shows up/down arrows.
  2. Drag vertically anywhere on the strip: the head moves smoothly, the cursor hides, and on release it reappears where it started.
  3. Hold Shift mid-drag: the speed drops 5x with no jump, and the popup says "fine".
  4. The popup "Pan: …°" shows above the dial and is not cut off by the bar.
  5. Double-click: the head returns home (0°).
  6. Drag past a limit and reverse: it responds at once.

**Commit:** d5f6fff

**Known issues / follow-ups**
- Faders (`UStageFaderWidget`) still use the stock slider drag. The same delta drag, popup and reset could move into the control base if wanted.
- The popup sits above the strip. That suits the bar at the bottom of the screen; a bar docked at the top would need a "below" placement option.

## #18 — Core architecture: profile/economy/session subsystems, GameMode rules, controller request bridge, shop foundation (2026-10-01)

**Request:** lay down the core architecture before marketplace work:
- **GameInstanceSubsystem:** persistent profile, currency, inventory and unlocks that survive level travel.
- **World or LocalPlayer subsystem:** live stage state.
- **GameMode:** the rules.
- **PlayerController:** the secure bridge from UI to the subsystems.
- **Economy foundation:** data assets and structs for costs, requirements, ownership and unlock conditions, with validation before any purchase or parameter change.
- **Docs:** Rules.md and STATE.md. The request said "stat.md"; this project's status file is STATE.md.

**Layout**

| Layer | Class | Responsibility |
|---|---|---|
| GameInstance | `UStageProfileSubsystem` (new) | Profile (name, level), wallet `TMap<Currency tag, int64>`, entitlements, purchase history. Saved to slot `StageCraftProfile`: synchronous load, async save with a queued re-save, final save on shutdown. Integrity hash. Read-only to everyone except its friend the economy. |
| GameInstance | `UStageEconomySubsystem` (new) | Product catalog (Asset Manager type `StageProduct`, `/Game/StageCraft/Shop`), ownership queries, `ValidateItemUse` / `ValidateParameterChange`, `CanPurchase` / `RequestPurchase`, `OnPurchaseCompleted`. Settles through a configurable `UStageCommerceBackend`. |
| GameInstance | `UStageItemSubsystem` (existing) | Item catalog and the armed item. Unchanged. |
| World | `UStageSessionSubsystem` (new) | Placed-item registry (actors register on BeginPlay/EndPlay), live totals (`FStageSessionStats`: items, W, kg), current `FStageSessionRules`, `EvaluateSessionLimits`, `ApplyParameterChange` (the single write point for UI edits), dirty flag. |
| World | `UShowControlSubsystem` (existing) | Fixtures, cues, DMX. Unchanged. |
| GameMode | `AStageCraftGameModeBase` | Holds `SessionRules`, passes them to the session at `StartPlay`. Decides `EvaluatePlacement` (ownership if enforced, then session limits) and `EvaluateParameterChange` (ownership if enforced). Both are BlueprintNativeEvents. |
| PlayerController | `AModularPlayerController` | `RequestParameterChange`, `RequestPurchase`, `CanPlaceItem`, `DecorateParameterSections`, `OnRequestRejected`. Binds the spawn component's `PlacementValidator`. Interim on-screen messages for refusals and purchases. |
| LocalPlayer | none | Deliberately not added. This is a single-user editor; per-player tool state stays in controller components. A `ULocalPlayerSubsystem` is reserved for per-user settings (Rules.md table). |

**Economy data model** (`Public/Economy/`)
- **`StageEconomyTypes.h`:**
  - `EStageEconomyResult` (Success, InvalidRequest, UnknownProduct, AlreadyOwned, RequirementsNotMet, InsufficientFunds, PurchasePending, BackendRejected, IntegrityViolation, Locked, SessionRuleViolation) and `FStageEconomyResultInfo` (code + user-facing message).
  - `EStageOwnershipState` (Free, Owned, Purchasable, Locked).
  - Structs: `FStageCurrencyAmount`, `FStagePurchaseRecord`, `FStagePlayerProfile`, `FStagePurchaseRequest`.
  - Native tags: `StageCraft.Currency(.Credits)`, `StageCraft.Entitlement(.Item/.Feature)`.
- **`UStageProductData`** (PrimaryDataAsset): DisplayName, Description, Icon (UI bundle), `Price[]`, `GrantedEntitlements`, instanced `Requirements[]`. `IsDataValid` rejects products that grant nothing, bad or duplicate currencies, and empty conditions.
- **`UStageUnlockCondition`** (abstract, EditInlineNew, Blueprintable, `IsMet` BlueprintNativeEvent), with subclasses `_Entitlements` and `_ProfileLevel`.
- **`UStageCommerceBackend`** (abstract; `ProcessPurchase` must answer once) and **`UStageLocalCommerceBackend`** (offline, approves immediately). Selected by `CommerceBackendClass` in DefaultGame.ini; empty means local.
- **`UStageProfileSaveGame`**: Version, Profile, IntegrityHash.
- **`UBaseItemData`** (new Economy category):
  - `RequiredEntitlement` gates placing an item and switching to it.
  - `ParameterEntitlements` (parameter → entitlement) gates editing.
  - Empty means free, so existing content is unaffected. Validation requires `StageCraft.Entitlement` tags.
- **`FStageParameterDescriptor::bLocked`** is set together with `bReadOnly` for display.

**Security points:**
- The economy is the single writer of profile money and entitlements.
- Purchases follow validate, then backend, then re-validate, then an atomic commit.
- The price is summed per currency, so duplicate price lines can't slip past the funds check.
- Only catalog products can be sold; forged objects are rejected.
- One purchase can be in flight per product, and each carries a `TransactionId`.
- Money is `int64` with overflow and negative checks.
- Requests are denied by default.
- **Integrity:** a salted SHA-1 over a canonical string of balances, entitlements, history IDs and level. On mismatch the profile still loads, but spending and grants are frozen and the save is never re-hashed. It is documented as tamper-deterrence only; real-money sales need a server backend.

**Data flow:**
- **UI edit:** `UStageParameterViewWidget::CommitParameter` → `AModularPlayerController::RequestParameterChange` → `GameMode::EvaluateParameterChange` (→ `Economy::ValidateParameterChange`, including the Type dropdown resolving to the target item) → `Session::ApplyParameterChange` → actor → `OnParameterChanged` → views refresh.
- **Placement:** `USpawnSystemComponent::SpawnItemAt` → `PlacementValidator` (controller) → `GameMode::EvaluatePlacement`. A refusal ends the stroke.
- **Purchase:** `RequestPurchase` (controller) → economy → backend → `CommitPurchase` → `OnBalanceChanged` / `OnEntitlementsChanged` → views rebuild, so a purchase unlocks rows without reselecting.
- **Gizmo moves:** unchanged. Transform is never economy-gated; the session only marks dirty from the transform change events.

**Dev tools** (non-Shipping):
- `DevResetProfile`, `DevReloadProfile`, `DevGrantCurrency`.
- Console commands, run in PIE:
  - `StageCraft.Shop.List`
  - `StageCraft.Shop.Buy <ProductAssetName>`
  - `StageCraft.Profile.Status`
  - `StageCraft.Profile.Reset`
  - `StageCraft.Profile.Grant <Amount> [CurrencyTag]`

**Test content:**
- `DA_Product_MovingHeadWash` (1500 Credits, grants `StageCraft.Entitlement.Item.MovingHeadWash`).
- `DA_MovingHead_Wash_Test.RequiredEntitlement` is set to that tag. **The Wash test fixture is now locked until bought.**
- New tags in `Config/DefaultGameplayTags.ini` (new file): that entitlement, plus `StageCraft.Entitlement.Feature.TestOptics`, which nothing sells and is kept for lock tests.
- Starting balance: 5000 Credits (DefaultGame.ini).

**Files changed**
- New C++:
  - `Public/Economy/` `StageEconomyTypes.h`, `StageProductData.h`, `StageUnlockCondition.h`, `StageCommerceBackend.h`, `StageProfileSaveGame.h`
  - `Private/Economy/` `StageEconomyTypes.cpp`, `StageProductData.cpp`, `StageUnlockCondition.cpp`, `StageCommerceBackend.cpp`, `StageEconomyConsoleCommands.cpp`
  - `Public/Subsystems/` + `Private/Subsystems/` `StageProfileSubsystem`, `StageEconomySubsystem`, `StageSessionSubsystem`
  - `Public/Game/StageSessionTypes.h`
- Modified C++:
  - `Game/StageCraftGameModeBase.h/.cpp`, `Player/ModularPlayerController.h/.cpp`
  - `Components/SpawnSystemComponent.h/.cpp` (validator)
  - `Actors/ModularBaseActor.h/.cpp` (session registration; `GetSwappableItems` made public)
  - `Data/BaseItemData.h/.cpp` (economy fields and validation), `Data/StageParameterTypes.h` (`bLocked`)
  - `UI/StageParameterViewWidget.h/.cpp` (commits through the controller, decoration, rebuild on entitlement change)
- Config: `Config/DefaultGame.ini` (StageProduct scan, profile and economy sections), `Config/DefaultGameplayTags.ini` (new).
- Content:
  - New: `Content/StageCraft/Shop/DA_Product_MovingHeadWash.uasset`.
  - Modified: `Content/StageCraft/Data/DA_MovingHead_Wash_Test.uasset` (RequiredEntitlement).
  - Re-saved with the new properties at their defaults (no value changes): `Content/StageCraft/Data/DA_MovingHead_Test.uasset`, `Content/StageCraft/Blueprints/BP_StageCraftGameMode.uasset`. During the test, the GameMode power limit was set to 1 W and restored to 0, and `ParameterEntitlements` was set on the Spot test fixture and cleared again.
- Docs: `Rules.md` (new "Architecture — Subsystems, Data Binding, Economy Security" section), `STATE.md`, `tasks/todo.md`.

**Verification**
- **Builds (Claude), editor closed:** `ModularSceneBuilderEditor` Development, `ModularSceneBuilder` (Game) Development, and `ModularSceneBuilder` **Shipping** (proves the dev-only code compiles out). All succeeded with 0 errors and 0 warnings.
- **Automated in-PIE test (Claude), `L_StageTest`, real subsystems, controller and HUD inspector:**

  | Check | Result |
  |---|---|
  | New profile | 5000 Credits, no entitlements, integrity ok; product catalog `[DA_Product_MovingHeadWash]` |
  | Ownership states | Spot `Free`, Wash `Purchasable` |
  | Session | started; 4 items, 940 W, 114 kg in the level |
  | Inspector Type options | `Test Moving Head Spot`, `Test Moving Head Wash (locked)` |
  | Swap to Wash (controller, and the inspector row's own commit) | `Locked` "Test Moving Head Wash is locked. Unlock it in the shop."; item stays Spot |
  | `CanPlaceItem(Wash)` | `Locked` |
  | Forged product (`new_object`) / null | `UnknownProduct` / `InvalidRequest` |
  | Buy | Success; 3500 left; entitlement owned; history 1 record (1500 paid); Wash now `Owned` |
  | Buy again | `AlreadyOwned`; balance unchanged |
  | After purchase | inspector rebuilt without "(locked)"; swap to Wash succeeds and back; session dirty |
  | Parameter lock (Zoom → TestOptics on the Spot data) | `Locked`, Zoom stays 20; inspector Zoom row `bLocked` + read-only, Dimmer row unaffected; after clearing, the edit applies (20 → 27) |
  | Funds (balance 1000, price 1500) | `InsufficientFunds` "Needs 1,500 …; you have 1,000."; still `Purchasable`; request refused, nothing granted; overdraw grant refused |
  | Power rule (GameMode `MaxTotalPowerWatts` = 1 W, test only) | `SessionRuleViolation` "Not enough power: 940 W of 1 W in use, Test Moving Head Spot needs 470 W." |
  | Level travel (`open L_StageTest`) | balance 3500 and entitlement kept (GameInstance); the session restarted (dirty reset to false, stats recomputed) |
  | Reload from disk | 3500, entitlement, integrity ok, 1 purchase |
  | Tamper: the save's balance bytes 3500 → 999999 | loads 999999, integrity **false**, purchase → `IntegrityViolation`, dev grant refused; reset restores a valid 5000 profile |
  | Cleanup | save slot deleted, so the next PIE starts with a fresh profile; GameMode rules restored to 0; throttle restored |

  - Test-script slip, not a code issue: the "new session object" check compared path names, which are identical after reloading the same map. A fresh session is shown instead by the dirty flag resetting.
- **Not verified by Claude:** how the on-screen refusal and purchase messages look, and the flow by hand.
- **Awaiting manual verification (Gevor), PIE:**
  1. Select a moving head. The Type dropdown shows "Test Moving Head Wash (locked)". Choosing it shows an orange "…is locked" message and nothing changes.
  2. Console: `StageCraft.Profile.Status`, then `StageCraft.Shop.List`, then `StageCraft.Shop.Buy DA_Product_MovingHeadWash`. A green "Purchased Moving Head Wash" appears, and the dropdown entry loses "(locked)" without reselecting.
  3. Switch to Wash, stop PIE, play again: still owned, 3500 Credits.
  4. `StageCraft.Profile.Reset` locks it again.

**Commit:** `9fa6948`

**Known issues / follow-ups**
- **No shop or wallet UI yet.** Bind `UStageEconomySubsystem::GetProducts` / `CanPurchase` / `OnPurchaseCompleted`, `UStageProfileSubsystem::OnBalanceChanged` and `AModularPlayerController::OnRequestRejected` in a shop widget and a HUD toast, then remove the interim `AddOnScreenDebugMessage` feedback.
- **Catalog filtering:** the catalog UI still lists locked items. Use `UStageEconomySubsystem::GetItemState` for a lock badge and buy button.
- **Networking:** decisions run on the GameMode (authority only). For multi-user editing, the controller requests become Server RPCs, and the profile/economy authority moves to a backend.
- **Gaps in the rules:**
  - Session limits are checked on placement only. A Type swap to a heavier or more power-hungry model is not limit-checked yet.
  - Parameter names in lock messages show the tag (`StageCraft.Attribute.Zoom`); a display-name lookup is pending.
- **Profile save:** on Windows it writes to `Saved/SaveGames/StageCraftProfile.sav`. Profile level has no XP system feeding it yet.
- **Test leftovers:** `StageCraft.Entitlement.Feature.TestOptics` is a test-only tag. `DA_MovingHead_Wash_Test` is now locked by default by design; clear its `RequiredEntitlement` if that gets in the way.

---

## #19 — Rules.md: Senior++ UE Production Engineering Standard added; CLAUDE.md now loads Rules.md (2026-10-02)

**Request:** add Gevor's "Unreal Engine Production Engineering Standard" (48 sections) to Rules.md so it covers everything, and always refer to it when writing code.

**What changed and why**
- **Rules.md:** the standard was added as a new `## Unreal Engine Production Engineering Standard` section, between "Architecture" and "Workflow Orchestration".
  - Its headings were demoted (`###` sections, `####` subsections) to fit the file's hierarchy. The text is otherwise verbatim.
  - A short preamble says the project-specific rules take precedence where they are more specific.
- **Conflicts reconciled:**
  - **Forward declarations.** The standard's "forward declare wherever possible" (§24, §25, §38) now names this project's form: inline elaborated type specifiers, never a block of `class X;` lines at the top of a header.
  - **Blueprint vs C++.** The section said "default to Blueprints". It now follows §15: Blueprints for UI, designer configuration and orchestration; C++ for rules, networking, save/load, the economy, performance and complex state.
  - **Pointers.** "Use `TObjectPtr<>` over raw pointers" now follows §9: `TObjectPtr`, `TWeakObjectPtr`, soft pointers, and raw pointers only for locals.
  - **UPROPERTY exposure.** "Always use UPROPERTY/UFUNCTION" now asks for the narrowest specifiers that fit (§11).
  - **§9 example** uses `TObjectPtr<class UObjectType> Object = nullptr;` to match Code Standards.
- **CLAUDE.md (new):** contains only `@Rules.md`. Before this, no CLAUDE.md existed, so Rules.md was never loaded into Claude sessions automatically. It now is, every session.
- **STATE.md hygiene:** entries #14, #15 and #18 still said "uncommitted". They now show their commits (`364686c`, `364686c`, `9fa6948`).

**Files changed:** `Rules.md`, `CLAUDE.md` (new), `STATE.md`. Outside the repo: Claude project memory `rules-md-engineering-standard.md`.

**Verification:** documentation only, so there is no build or PIE test. Checked that Rules.md keeps CRLF line endings and `git diff --check` is clean.

**Commit:** uncommitted (working tree)

**Known issues / follow-ups**
- The "MCP Tools — Use Them" section still lists the older UnrealClaude tool names (`get_level_actors`, `blueprint_modify`, …). The connected server is `ue5-ngg` (`ue5_*` tools). It was not changed because it was out of scope.
- Rules.md grew from 335 to about 2,000 lines, and it now loads into every session's context.
