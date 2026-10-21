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

**Commit:** `b74f523`

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

**Commit:** `364686c` (together with #13)

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

**Commit:** `364686c` (together with #12)

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

**Commit:** `e827b5c`

**Known issues / follow-ups**
- The "MCP Tools — Use Them" section still lists the older UnrealClaude tool names (`get_level_actors`, `blueprint_modify`, …). The connected server is `ue5-ngg` (`ue5_*` tools). It was not changed because it was out of scope.
- Rules.md grew from 335 to about 2,000 lines, and it now loads into every session's context.

---

## #20 — Design: dockable multi-window workspace, detachable viewport, preferences (2026-10-02)

**Request:** design the architecture, core manager classes and implementation steps for an Unreal-Editor-style workspace:
- dockable, splittable, tabbed and floating panels;
- a 3D viewport that can be detached to another monitor;
- a preferences system (layouts, UI scale, window configuration, key bindings, viewport and graphics);
- serialized named layouts.

This entry covers the design only. No code was written.

**What was produced**
- `Docs/ADR/0001-dockable-workspace.md`. It holds the verified engine facts (F1–F11, with UE 5.8 source refs), the class layering, the state model, an API sketch, the layout file format and validation rules, the viewport-detach rules, the alternatives rejected, the consequences, a 6-phase plan and the test strategy.
- Key decisions (proposed; awaiting Gevor's approval):
  - Main window = docking root. `FGlobalTabmanager` is hosted the way Trace Insights does it.
  - The game viewport goes into a dock tab via `UStageCraftGameEngine::CreateGameViewportWidget` with `RenderDirectlyToWindow(false)`. The engine default renders straight to the window backbuffer, so it can't be docked.
  - `UStageWorkspaceSubsystem` (GameInstance) owns `FStageWorkspaceShell` (all Slate) and `FStageLayoutStore` (pure, unit-testable).
  - Panels are data (`UStagePanelDefinition`, `StageCraft.Panel.*` tags) with UMG content.
  - Preferences live in `UStageCraftUserSettings : UGameUserSettings`. Key bindings use `UEnhancedInputUserSettings`.
  - Layouts are saved to our own JSON under `Saved/StageCraft/Layouts`, because the editor layout ini is not loaded in game builds.
  - The workspace works only in Standalone Game or packaged builds. PIE keeps the current HUD.
- Engine risks found (spike items):
  - `UGameEngine::OnViewportResized` saves the viewport size as the screen resolution.
  - `ResizeFrame` resizes whichever window holds the viewport.
  - `SwitchGameWindowToUseGameViewport` (startup and loading-movie end) resets the main window's content.
  - Closing the main window quits the app.
  - The separate render target has a cost that must be measured.

**Files changed:** `Docs/ADR/0001-dockable-workspace.md` (new), `tasks/todo.md` (Phase 6 checklist), `STATE.md`.

**Verification:** design only, so nothing to build or test. Engine claims were checked in `C:/Program Files/Epic Games/UE_5.8/Engine/Source`. The ADR marks the ones not yet confirmed as Phase 0 spike items.

**Commit:** `86bc1b7` (together with #21)

**Known issues / follow-ups**
- Waiting on Gevor to approve the ADR decisions before the Phase 0 spike.
- Rules.md housekeeping:
  - The Obsidian vault (`S:\...`) is not reachable on this machine, so the ADR went into the repo under `Docs/ADR/`.
  - `RunEditor.bat` / `Scripts/Launch/RunEditor.ps1` do not exist in this project; that Rules.md section references `DamenTools8.uproject`.

---

## #21 — Phase 0 spike: dockable workspace with a detachable 3D viewport (2026-10-02, branch `spike/dockable-workspace`)

**What and why.** The spike proves the design in ADR 0001 (#20). In Standalone Game:
- The 3D viewport renders inside a Slate dock tab, with the Inspector and Faders as dock panels around it.
- The viewport can float into its own window on another monitor.
- Cursor picking works in both cases.
- Layouts can be replaced, and level travel keeps the workspace.

Verdict: **GO** (ADR §8). The code is the start of Phase 1, not throwaway.

**Engine findings that changed the design** (ADR F12–F14):
- **F12.** Every frame, `FEngineLoop::Tick` calls `GetMoviePlayer()->WaitForMovieToFinish(true)` (`LaunchEngineLoop.cpp:5902`). With no movie playing, that runs `UGameEngine::SwitchGameWindowToUseGameViewport()`, which resets the main window's content to `UGameEngine::GameViewportWidget`. This also happens in Shipping.
  - Fix: `UStageCraftGameEngine::SetMainWindowContent` puts the dock root inside a host `SViewport` and points the public `GameViewportWidget` member at that host.
  - `GetGameViewportWidget()` is overridden to keep returning the real viewport.
  - For the one first `UGameEngine::Tick` (`GameEngine.cpp:2022`, a one-time `RegisterGameViewport`), the member is the real viewport. Without that, Slate ensures (`SlateApplication.cpp:2547`).
- **F13.** `FTabManager::SpawnTab` skips a tab while the spawner's previous tab is still alive. `CloseAllAreas` only defers window destruction, and for an embedded area it would target the main window.
  - Fix: `FStageWorkspaceShell::TearDownPanels` releases the Workspace tab's content, destroys floating panel windows immediately, and rebuilds a fresh panel tab manager for each layout.
- **F14.** Nomad tabs in `FGlobalTabmanager`'s primary area did not dock into the main window.
  - Fix: the structure Slate's standalone tools use. One major tab ("StageCraft.Workspace", tab well hidden) owns a panel `FTabManager`, and the panels are panel tabs in it. The host window is found with `FindWidgetWindow`, because `SDockTab::GetParentWindow` is null there.
- **Install timing.** The first attempt installed during the first map's `BeginPlay`. The startup window switch (`LaunchEngineLoop.cpp:4893`) then removed it. With the F12 host, installing at controller registration is safe.

**Files changed**
- New (`Source/ModularSceneBuilder`):
  - `Public/Workspace/StageCraftGameEngine.h`, `Private/Workspace/StageCraftGameEngine.cpp`: separate-render-target viewport, main-window content host, first-tick registration, removal of resize-to-resolution handling.
  - `Public/Workspace/StageWorkspaceSubsystem.h`, `Private/Workspace/StageWorkspaceSubsystem.cpp`: GameInstance subsystem; states `Uninitialized`/`Unsupported`/`Ready`/`ShuttingDown`; panels built against the registered local controller (UMG `CreateWidget` + `TakeWidget`); `OnPanelHostChanged`.
  - `Private/Workspace/StageWorkspaceShell.h/.cpp`: all Slate (root layout, Workspace major tab, panel tab manager, viewport tab that cannot close, relocation and close events, `TearDownPanels`).
  - `Public/Workspace/StageWorkspaceTypes.h`, `Private/Workspace/StageWorkspaceTypes.cpp`: `LogStageWorkspace`, `EStageWorkspaceState`, `EStagePanelHost`, native tags `StageCraft.Panel(.Viewport|.Inspector|.FaderBank)`.
  - `Private/Workspace/StageWorkspaceConsoleCommands.cpp` (non-Shipping): `StageCraft.Workspace.Status`, `.FloatViewport [MonitorIndex]`, `.Reset`, `.ViewportOnly`, `.TestUnknownTab`, `.ProbePick`, `.Delay <s> <cmd>`.
- Modified:
  - `Private/Player/ModularPlayerController.cpp`: register/unregister with the workspace in BeginPlay/EndPlay; the fixed HUD is not shown while the workspace is active, because it holds only the same two panels today.
  - `ModularSceneBuilder.Build.cs`: private `ApplicationCore`.
  - `Config/DefaultEngine.ini`: `GameEngine=/Script/ModularSceneBuilder.StageCraftGameEngine`.
  - `Config/DefaultGame.ini`: interim `InspectorPanelClass` / `FaderBankPanelClass`.
  - `Docs/ADR/0001-dockable-workspace.md`: status, F9 superseded, F12–F14, §3 updated, §8 results.
  - `tasks/todo.md`.

**Verification**
- Builds: the Editor target compiles clean (0 errors, 0 warnings). The Development Game target compiles clean. Shipping Game target: compiles clean (0 errors, 0 warnings), which confirms the `#if !UE_BUILD_SHIPPING` code (console commands, `-StageDirectViewport`, `GetShellForDiagnostics`) is compiled out without breaking the build.
- Tested by Claude in Standalone Game (scripted via `-ExecCmds`, 3 monitors, logs plus per-window captures):
  - Docked viewport at tab size (1212×637) with live Inspector and Faders panels.
  - Picking MATCH in the docked viewport.
  - Float to monitor 3 at (3890, 50) with picking MATCH (OS cursor at 4533, 429).
  - A layout naming an unknown tab is handled.
  - Reset destroys the floating window.
  - Level travel keeps the layout and rebuilds the panels; the main window does not move.
  - `GSystemResolution` and the saved resolution are untouched.
  - No ensures or warnings.
- Frame cost (one run each, 1600×900, RTX 3060 Ti): GPU 5.65 → 5.97 ms (+0.32 ms, +5.7 %), render thread +0.35 ms, game thread +0.08 ms. See ADR §8 for caveats.
- **PIE not re-run.** It is expected to be unchanged: the subsystem is not created when `GIsEditor` is true, the editor ignores `GameEngine=`, and the controller then shows the HUD as before.
- **Awaiting manual verification (Gevor), in Play ▸ Standalone Game:**
  1. Drag tabs to split, re-dock and tear off.
  2. Drag the floating viewport between monitors.
  3. RMB fly, gizmo drag and Delete in a floated viewport.
  4. Alt+Enter / F11 with a floated viewport.
  5. OS-close a floating viewport window.
  6. Resize the main window by hand.
  7. Quick PIE check that the HUD is unchanged.

**Commit:** `86bc1b7` (spike merged to `main`, together with #20)

**Known issues / follow-ups**
- The viewport tab shows a close button, though closing it is refused. Floating windows have no title.
- Fullscreen toggles (Alt+Enter / F11) are not routed through the workspace yet (ADR §3.5).
- In workspace mode the fixed HUD is suppressed, so it is not yet a viewport-overlay layer. Interim debug messages still draw on the viewport.
- Panel widget classes are interim config soft references, loaded synchronously. `UStagePanelDefinition` assets with async loading come in Phase 1. The classes are cooked today only because `WBP_StageCraftHUD` references them.
- The frame-cost measurement is a single sample. Repeat at 4K and with several runs.
- Nothing is persisted yet. Layout files come in Phase 2.

---

## #22 — Phase 1: core window manager and workspace layout system (2026-10-06)

**Request (Gevor).** "Phase 1: Core Window Manager & Workspace Layout System" for Standalone mode, meaning ADR 0001's Phase 1 (shell) and Phase 2 (layouts) together:
- a C++ window manager for docking, undocking, tabs and floating windows;
- custom layouts saved to disk and restored (panel positions, sizes, active tabs);
- panels bound to backend systems through the existing view pattern, with no new plugins.

The Phase 0 spike (#21) is the base. Design details and the reasons for each refinement are in `Docs/ADR/0001-dockable-workspace.md` §9.

**What changed and why**

- **Panels are data.** `UStagePanelDefinition` is a `StagePanel` primary asset under `/Game/StageCraft/UI/Panels`.
  - Its primary asset ID name is the panel tag. The workspace registers dock-tab spawners from the Asset Manager scan alone, so a saved layout never drops a panel whose definition is still loading.
  - Definitions and their widget class (`UI` bundle) load asynchronously. Until they arrive, tabs show "Loading…" and their label updates when they do.
  - This replaces the interim `InspectorPanelClass` / `FaderBankPanelClass` config, which is removed. Adding a panel is now a tag plus an asset, with no code.
  - The viewport stays a built-in panel.
- **Layouts persist.** `FStageLayoutStore` is pure C++ with no world or Slate.
  - Named user layouts go to `Saved/StageCraft/Workspace/Layouts/<Name>.json` and are written only by "Save Layout As".
  - The live arrangement is `Session.json`. It is autosaved (Slate's deferred persist hook, with the write on a `UE::Tasks` background task) and restored on launch.
  - The JSON is versioned: `FormatVersion` plus the panel layout key `StageCraft_Workspace_v1`. Writes are atomic (`.tmp`, then move).
  - A corrupt or incompatible file is renamed to `.bak`, never deleted, and the Default layout is used.
  - Names are validated (1–64 safe characters, no path escape or device names). Built-in names ("Default", "Viewport Only") are reserved.
- **Monitor clamping.**
  - The main window rect is saved and applied in physical pixels, in windowed mode only.
  - Floating panel windows are clamped inside Slate's JSON against work areas divided by DPI scale, because that is how Slate saves them (ADR F17).
  - A window whose monitor is gone moves to the primary monitor.
- **Final save happens before anything is destroyed** (ADR F15). Slate destroys child windows before their parent, so by `Deinitialize` the floating windows are gone. The shell hooks the main window's `RequestDestroyWindowOverride`, which both the OS close button and Alt+F4 go through. It saves there and seals the session. The `quit`/`exit` path still saves in `Deinitialize`, while the windows are alive.
- **Window manager API** (`UStageWorkspaceSubsystem`, BlueprintCallable, contracts in the header):
  - Panels: `GetAvailablePanels`, `GetPanelDisplayName`, `OpenPanel`, `ClosePanel`, `GetPanelHost`.
  - Layouts: `ApplyLayout`, `SaveCurrentLayoutAs`, `DeleteLayout`, `ResetToDefaultLayout`, `GetBuiltInLayouts`, `GetUserLayouts`, `IsBuiltInLayout`, `GetActiveLayoutName`.
  - Every layout operation returns `EStageLayoutResult`.
  - Delegates: `OnPanelHostChanged`, `OnLayoutApplied`, `OnLayoutsChanged`.
- **View binding.** `SStageWorkspaceMenuBar` sits above the panels in the main window.
  - Window menu: panel toggles with check state, Reset Layout.
  - Layout menu: built-ins, user layouts with the active one marked, Save Layout As (inline name box pre-filled with the active user layout, errors shown on the box), Delete Layout.
  - It is stateless and built from the getters when a menu opens. It commits only through the validated subsystem API. No tick and no polling.
  - The UMG panels keep their existing binding: `UStageParameterViewWidget` through the controller's request bridge.
- **Fix in spike code.** `UStageCraftGameEngine::RestoreMainWindowViewport` re-registered the game viewport after `UGameEngine::OnGameWindowClosed` had released `SceneViewport`. That ensured at `SlateApplication.cpp:2547` whenever the main window was closed with the workspace installed. It now only hands the viewport back while `SceneViewport` is valid. Root cause: the spike never closed the main window with the workspace installed.
- **Dev console commands** (non-Shipping): `StageCraft.Workspace.OpenPanel` / `ClosePanel <Tag>`, `SaveLayout` / `LoadLayout` / `DeleteLayout <Name>`, `ListLayouts`, `CloseMainWindow` (same call as the OS close button). Existing commands use the shared panel layout key.

**Files changed**
- New, in `Source/ModularSceneBuilder`:
  - `Public/Workspace/StagePanelDefinition.h`, `Private/Workspace/StagePanelDefinition.cpp`
  - `Private/Workspace/StageLayoutStore.h/.cpp`
  - `Private/Workspace/SStageWorkspaceMenuBar.h/.cpp`
  - `Private/Workspace/Tests/StageLayoutStoreTests.cpp`
- Modified:
  - `Public/Workspace/StageWorkspaceSubsystem.h`, `Private/Workspace/StageWorkspaceSubsystem.cpp` (no longer `Config=Game`)
  - `Private/Workspace/StageWorkspaceShell.h/.cpp`
  - `Public/Workspace/StageWorkspaceTypes.h` (`EStageLayoutResult`)
  - `Private/Workspace/StageCraftGameEngine.cpp`
  - `Private/Workspace/StageWorkspaceConsoleCommands.cpp`
  - `ModularSceneBuilder.Build.cs` (private `Json`)
- Content (new): `Content/StageCraft/UI/Panels/DA_Panel_Inspector.uasset` (→ `WBP_StageInspectorPanel`, "Inspector") and `DA_Panel_FaderBank.uasset` (→ `WBP_StageFaderBank`, "Faders").
- Config: `Config/DefaultGame.ini` gains the `StagePanel` AssetManager scan entry (AlwaysCook). The interim `[/Script/ModularSceneBuilder.StageWorkspaceSubsystem]` section is removed.
- Docs: `Docs/ADR/0001-dockable-workspace.md` §9 (F15–F17, refinements, API), `tasks/todo.md`, `STATE.md`.

**Verification**
- **Builds:** Editor, Game Development and Game Shipping all compile clean (0 errors, 0 warnings).
- **Automation:** `StageCraft.Workspace.LayoutStore.*` ran headless, 5/5 passed:
  - RoundTrip
  - BadFiles (not JSON, empty, missing fields, bad main window, future format and old panel version → `.bak`)
  - Names
  - ListAndDelete
  - ClampToMonitors (unplugged monitor, far off-screen and oversized, straddling, DPI-scaled Slate units)
- **Standalone Game, tested by Claude** (scripted with `-ExecCmds`, 3 monitors, logs `Saved/Logs/WS_Run1-4.log`):
  1. First launch, no session. Default layout; both definitions loaded. Float viewport to monitor 3, close Faders, Save As "Float Test", Load Default, Load "Float Test" (floating viewport and closed Faders restored). Save As "Default" → ReservedName. "../bad" → InvalidName. `CloseMainWindow` → session saved with the floating window and the closed tab. `GSystemResolution` / saved resolution unchanged.
  2. Restart. Restored from the session (floating viewport window, now titled "Viewport"; Faders closed). Picking in the floated viewport: MATCH. Level travel (`open L_StageTest`) kept the layout. Faders reopened. Close: no ensure (the fix above).
  3. Session edited off-screen (main window X=20000, floating X=30000). Both clamped onto the primary monitor (320 / 640). The `quit` path saved the session in `Deinitialize`.
  4. Corrupt `Session.json` → `.bak` and Default. User layout with FormatVersion 7 → VersionMismatch, `.bak`, dropped from the list, current arrangement kept. Delete "Default" → ReservedName. Delete missing → NotFound.
  - No ensures, warnings or crashes in runs 2–4. Run 1's ensure is the one fixed above.
- **Not verified** (needs Gevor, by mouse, in Standalone Game):
  - Visual check of the menu bar. An OS screenshot was blocked by another window on that monitor, and Claude did not steal focus.
  - Window menu toggles and Reset Layout; Layout menu Save As (Enter, inline error on a bad name) and Delete.
  - Dragging tabs and windows, then restart restores them. This includes moving a floating window without any tab change, which is saved only on close (F16).
  - Maximized main window round-trip.
  - The #21 checklist (drag split/re-dock/tear-off, RMB fly, gizmo and Delete in a floated viewport, Alt+Enter/F11, OS-close of a floating viewport window, manual resize of the main window).
  - PIE quick check: the subsystem is not created in the editor, so the HUD should be unchanged.
- Status: **implemented, awaiting manual verification** of the items above.

**Commit:** `aed9e9c`

**Known issues / follow-ups**
- The Obsidian vault (`S:\`) is still unreachable on this machine. The design record is ADR §9 in the repo.
- Still open from the ADR: viewport-overlay HUD layer, fullscreen routing through the workspace, viewport tab close button (closing is refused but the button shows), DPI rule for overlays (Phase 5).
- A built-in "Dual Monitor" layout and panel icons are deferred to Phase 5 (they need monitor-aware built-ins and the dock style set).
- The frame-cost measurement from #21 is still a single sample.

---

## #23 — Placement mode (ghost + landing marker), Move/Rotate/Scale editing, audio manager (2026-10-06)

**Request (Gevor).** Editor-grade placement and layout planning, in three parts:
- A Placement Mode toggle, with a ghost of the item picked from the library that follows the cursor, and a highlighted landing spot. One click places exactly one object; holding the button must not keep spawning.
- Placed objects are selectable but never follow the mouse when clicked. Transforms are edited explicitly: Move, Rotate, Scale, a gizmo, and numeric XYZ applied on Enter.
- A central audio manager for selection, placement, snapping and error sounds, with Master volume, Effects volume and Mute in settings and UI.

The design record, with engine facts, alternatives and rollback, is `Docs/ADR/0002-placement-and-audio.md`.

**What changed and why**

- **Root cause of "holding spawns".** `USpawnSystemComponent` had a stroke mode (`EStageItemPlacementMode::Continuous`, the default for props) that kept stamping on held LMB. The mode enum, the `PlacementRules.PlacementMode` field, the stroke API and the light-placement validation warning are removed. `USpawnSystemComponent` is now a stateless executor: `SpawnItem(Item, Transform)` (still validated by `PlacementValidator`) and `TryDeleteActor`.
- **Placement tool.**
  - `UStagePlacementToolComponent` sits on the controller and owns:
    - `EStageEditMode { Select, Place }`;
    - the armed item, cached from `UStageItemSubsystem::OnSelectedItemChanged`;
    - `EStagePlacementPreviewState { Hidden, Valid, Refused }`.
  - **One shared transform.** The preview and the spawn both use `StagePlacementMath::ComputePlacementTransform` (pure: grid snap, normal alignment, pivot offset), so the ghost is the exact landing transform.
  - **After a place:** the ghost hides and the mode returns to Select. The new item is selectable but not auto-selected.
  - **Events:** `OnEditModeChanged`, `OnArmedItemChanged`, `OnItemPlaced`, `OnPlacementSnapped` (the landing point moved to another grid point), `OnPlacementFailed` (no item / no surface).
  - **Preview colour** comes from the GameMode rules without reporting: a locked item shows red, and the click is still validated.
- **`AStagePlacementPreview`** has no tick and no collision.
  - **Ghost:** the item's mesh with `M_PlacementGhost` (translucent unlit, fresnel edge, `GhostColor`).
  - **Marker:** a decal ring (`M_PlacementMarker`, `MarkerColor`) at the snapped landing point, aligned to the surface and sized to the footprint.
  - Missing materials degrade gracefully with one log line each.
- **Controller input (`AModularPlayerController`).**
  - **Handlers take a viewport position.** Enhanced Input reads the cursor once; the same handlers serve the dev simulation.
  - **Place mode:** a press calls `TryPlace`. A miss gives the "no surface" error cue.
  - **Select mode:** gizmo handle > select item > deselect. It never places, and a selected item never follows the mouse.
  - **Held LMB** only continues a gizmo drag.
  - **The ghost follows without a tick.** It refreshes on these events, coalesced into one `SetTimerForNextTick` trace (at most one per frame, only in Place mode):
    - new `UStageCraftGameViewportClient::OnCursorMoved` (`MouseMove`/`CapturedMouseMove`);
    - the camera pawn root's `TransformUpdated`;
    - a `SetControlRotation` override (the fly camera turns through it);
    - mode and armed-item changes.
  - **Keys:**
    - **P** toggles Place mode (new `TogglePlaceModeAction`).
    - **Esc** leaves Place mode first; a second Esc deselects and disarms.
    - **Space** cycles Move → Rotate → Scale.
    - Entering Place mode clears the selection.
  - **`RequestPlaceItem(Item)`** is the Library entry point: rules check (a refusal is reported and plays the error cue), then `SelectItem`, then `EnterPlaceMode`.
- **Transform editing.**
  - **Gizmo Scale mode:** axis shafts with cube heads, plus a centre cube for uniform scale. Dragging one handle length doubles the scale.
  - **One clamp.** `StageTransformRules` (pure) clamps scale to [0.01, 100] and repairs NaN to 1, for both the gizmo and the inspector.
  - **Inspector:** a new `StageCraft.Param.Transform.Scale` row in the base actor's Transform section. Numeric Location / Rotation / Scale XYZ apply on Enter through the validated `RequestParameterChange`, then read back.
  - **Session:** scale edits mark the session dirty.
- **Item Library panel.**
  - `UStageItemLibraryPanel` and `UStageItemLibraryEntry` are C++ widgets that build a default tree in code, so no WBP is needed (a WBP can restyle them through `BindWidgetOptional`).
  - Items are grouped by category with icons, and the armed item is highlighted. A click calls `RequestPlaceItem`.
  - Registered as `StageCraft.Panel.Library` / `DA_Panel_Library` and docked on the left in the Default layout.
- **Audio.**
  - **`UStageAudioSubsystem`** (GameInstance) is the single API: `PlayCue(Tag)`, Master/Effects/Mute setters and getters, and `OnAudioSettingsChanged`.
    - **Master and Mute** are a transient `USoundMix` override on the project's default (Master) sound class. It is pushed once per audio device, re-applied after each map load, and popped on shutdown so PIE never leaves the editor muted. `SetTransientPrimaryVolume` was rejected: the engine resets it on world cleanup and camera fades.
    - **Effects** is a per-play gain on cues.
  - **Persistence.** `UStageCraftUserSettings : UGameUserSettings` (ADR 0001's planned preferences class, introduced early) stores the values in `GameUserSettings.ini`. Its setters clamp and loading sanitises, and saves are debounced to 1 s.
  - **Cue table.** `UStageAudioDeveloperSettings` (Project Settings > Game > StageCraft Audio) maps `StageCraft.Sound.{Select, Place, Snap, Error}` to `SFX_*`, each with a level and a retrigger interval.
  - **Feedback.** `UStageEditorAudioFeedbackComponent` (controller) maps selection / placed / snapped / failed / refused events to cues.
- **Workspace menu bar.**
  - New **Edit** menu: Select / Place mode, and the Move / Rotate / Scale tool, as radio entries.
  - New **Audio** menu: Mute All, plus Master and Effects sliders that apply live and read back.
- **Content (generated by scripts in the repo; the editor's MCP bridge was attached to another project).**
  - `Scripts/Content/make_feedback_wavs.ps1` synthesises four WAVs.
  - `Scripts/Content/create_placement_audio_content.py`, run headless through `UnrealEditor-Cmd -run=pythonscript`, builds the materials, imports the sounds and creates `DA_Panel_Library`.
  - `PythonScriptPlugin` is enabled for the Editor target only.
- **Dev console** (non-Shipping):
  - `StageCraft.Edit.{Status, Arm, PlaceMode, PointerAt, PointerAtActor, ReleasePointer, Click, Hold, GizmoMode, SetLocation, SetScale}` drive the real click handlers at a simulated cursor.
  - `StageCraft.Audio.{Status, Master, Effects, Mute, Play}`.
- **STATE.md housekeeping.** The stale "uncommitted" lines in #10, #12, #13, #19, #20, #21 and #22 now carry their commit hashes (`b74f523`, `364686c`, `e827b5c`, `86bc1b7`, `aed9e9c`).

**Files changed**
- New, in `Source/ModularSceneBuilder`:
  - Placement: `Public/Placement/StagePlacementTypes.h`, `StagePlacementMath.h`, `StagePlacementPreview.h`, `StagePlacementToolComponent.h`, and `Private/Placement/StagePlacementMath.cpp`, `StagePlacementPreview.cpp`, `StagePlacementToolComponent.cpp`, `StagePlacementConsoleCommands.cpp`, `Tests/StagePlacementTests.cpp`.
  - Audio: `Public/Audio/StageAudioTypes.h`, `StageAudioDeveloperSettings.h`, `StageAudioSubsystem.h`, `StageEditorAudioFeedbackComponent.h`, and `Private/Audio/StageAudioTypes.cpp`, `StageAudioSubsystem.cpp`, `StageEditorAudioFeedbackComponent.cpp`, `StageAudioConsoleCommands.cpp`.
  - Settings: `Public/Settings/StageCraftUserSettings.h`, `Private/Settings/StageCraftUserSettings.cpp`.
  - `Public/Interaction/StageTransformRules.h`.
  - UI: `Public/UI/StageItemLibraryPanel.h`, `Private/UI/StageItemLibraryPanel.cpp`.
- Modified, in `Source/ModularSceneBuilder`:
  - Player: `ModularPlayerController.h/.cpp`, `StageCraftGameViewportClient.h/.cpp`.
  - `Components/SpawnSystemComponent.h/.cpp`.
  - Actors: `ModularTransformGizmo.h/.cpp`, `ModularBaseActor.cpp`.
  - Data: `StageItemTypes.h`, `BaseItemData.h/.cpp`, `AudioEquipmentData.cpp`, `LightingFixtureData.cpp`, `StageTrussData.cpp`, `StageParameterTypes.h/.cpp`.
  - `Subsystems/StageSessionSubsystem.cpp`.
  - Workspace: `StageWorkspaceTypes.h/.cpp`, `StageWorkspaceShell.cpp`, `SStageWorkspaceMenuBar.h/.cpp`.
  - `ModularSceneBuilder.Build.cs` (private `DeveloperSettings`).
- Content (new):
  - `Content/StageCraft/Placement/M_PlacementGhost`, `M_PlacementMarker`
  - `Content/StageCraft/Audio/Feedback/SFX_Select`, `SFX_Place`, `SFX_Snap`, `SFX_Error`
  - `Content/StageCraft/UI/Panels/DA_Panel_Library`
- Config:
  - `Config/DefaultEngine.ini`: `GameUserSettingsClassName`.
  - `Config/DefaultGame.ini`: the StageCraft Audio cue table, plus `DirectoriesToAlwaysCook` for `/Game/StageCraft/Audio` and `/Game/StageCraft/Placement`.
  - `ModularSceneBuilder.uproject`: `PythonScriptPlugin`, Editor only.
- Scripts: `Scripts/Content/make_feedback_wavs.ps1`, `Scripts/Content/create_placement_audio_content.py`.
- Docs: `Docs/ADR/0002-placement-and-audio.md` (new), `tasks/todo.md`, `STATE.md`.

**Verification**
- **Builds:** Editor Development, Game Development and Game Shipping, all with 0 errors and 0 warnings. Shipping proves the `StageCraft.Edit.*` / `StageCraft.Audio.*` dev commands and the simulation hooks are compiled out.
- **Automation (headless): 10/10 passed.**
  - New:
    - `StageCraft.Placement.Math` (snap incl. negatives, zero grid, offset, normal alignment, zero normal)
    - `StageCraft.Placement.ToolModes` (mode transitions, idempotency, no place in Select mode or without an item, a failed place keeps Place mode)
    - `StageCraft.Gizmo.ScaleRules` (drag factor, floor, NaN, clamp)
    - `StageCraft.Audio.Math` (clamp, mute wins, cue gain)
    - `StageCraft.Audio.UserSettings` (clamping setters, change reporting, `SetToDefaults`)
  - Existing: `StageCraft.Workspace.LayoutStore.*` (5) still pass.
- **Content:** a headless Python check confirmed:
  - `DA_Panel_Library` has tag `StageCraft.Panel.Library` → `StageItemLibraryPanel`;
  - `M_PlacementGhost` is translucent unlit, `M_PlacementMarker` is a translucent deferred decal;
  - the four SFX run 0.02–0.25 s.
- **Standalone Game, tested by Claude** (scripted, simulated cursor, logs `Saved/Logs/PL_Run1.log`, `PL_Run2.log`):
  1. Workspace Reset → Library panel docked. Arm `DA_Test_Crate` → Place mode, preview Valid at the snapped point (-300, 0, 50).
     - Cursor moved one cell → Snap cue, preview at (-300, 100, 50).
     - Click → exactly one crate at the previewed transform, Place cue, back to Select, ghost hidden.
     - `PlaceMode on` + **Hold for 90 frames → exactly +1 item** (5 → 6).
     - Click on the placed crate in Select mode → selected + Select cue. After the cursor moves away, its location is unchanged.
     - `SetScale 2 3 0.001` → (2, 3, 0.01); `SetScale 500 1 1` → (100, 1, 1) (clamped). Gizmo Scale → cycle → Move.
     - Arm the locked `DA_MovingHead_Wash_Test` → refused (Locked), toast + Error cue, nothing armed.
     - Audio: Master 0.5 + Mute → `ListSoundClassVolumes` Master 0.00. Unmute → 0.50. Effects 0.8. Debounced save wrote `[/Script/ModularSceneBuilder.StageCraftUserSettings]` to `Saved/Config/.../GameUserSettings.ini`.
     - Close → session saved, 0 ensures.
  2. Restart → preferences reloaded (0.50 / 0.80), Master class at 0.50 at startup, Library restored from the session.
     - Click on empty sky in Place mode → Error cue, nothing placed, still in Place mode.
     - `open L_StageTest` → Master class still 0.50, new controller works (arm light, place it).
     - Audio reset to 1.0 / 1.0 at the end.
- **Not verified** (needs Gevor, by mouse, in Standalone Game):
  - How the ghost and decal marker look on the stage (colours, size, the decal on uneven decks). OS screenshots were not taken; Claude did not steal focus.
  - That a real mouse move drives the ghost (`OnCursorMoved`; the scripted runs used the simulated cursor). The same for real click versus hold, and flying with RMB in Place mode (the ghost follows the camera).
  - The Library panel's look and clicks, the Edit and Audio menus, the sliders while dragging, and hearing the cues.
  - Dragging the scale gizmo (axis and centre), and Space cycling the visible handles.
  - The inspector Scale row (Enter applies, out-of-range snaps back).
  - P / Esc in the real keyboard path.
- Status: **implemented, awaiting manual verification** of the items above.

**Commit:** `9ec2c13`.

**Known issues / follow-ups**
- **PIE has no Library panel.** `WBP_StageCraftHUD` needs a `UStageItemLibraryPanel` dropped into its left side (designer task). In PIE, `StageCraft.Edit.Arm <Item>` or `BP_StageTestArmer` still arm items, then P enters Place mode.
- **Behaviour changes to retest.** In Select mode, a click on empty ground no longer places (use P or the Library), and "hold LMB to paint props" is gone by design. The old PIE checklists (#3, #8) mention painting, so read those steps as one click = one item.
- **Committed while focus moves.** The inspector spin boxes also commit when focus moves away (Unreal Editor behaviour), not only on Enter.
- **Not built yet:** gizmo scale snapping, local-space gizmo, undo/redo (hook `OnItemPlaced` / `OnItemDeleted` / gizmo drag end), shift-click to keep placing, and a Preferences panel page for audio (ADR 0001 Phase 3).
- **Ghost limits.** The ghost shows only the item's `Mesh`. Fixtures whose yoke/head are separate meshes show their base only.
- **Obsidian vault.** `S:\` is still unreachable on this machine. The design record is ADR 0002 in the repo.

## #24 — Library panel redesign, stamping placement, workspace status bar, viewport keyboard focus fix (2026-10-06)

**Request (Gevor).** A UI/UX pass on placement to match the dark GrandMA3 / Unreal Editor look of the Inspector and Fader panels:
1. **Library.** Replace the raw floating text and the plain unstyled buttons on the left with a professional, dockable asset-library panel: themed borders and margins, category collapsers, hover states and typography. Hints should sit in tooltips and a status bar, not across the viewport.
2. **Stamping.** In Place mode with an item armed, each click places a copy and **stays** in Place mode with the ghost. Esc or the Place toggle explicitly returns to Select (Move / Rotate / Scale).
3. Rules.md standards: delegates, validated paths, explicit ownership, no tick or polling.

This reverses #23's "after a place, return to Select". The design amendment is `Docs/ADR/0002-placement-and-audio.md` §7.

**What changed and why**

- **Stamping** (`UStagePlacementToolComponent::TryPlace`).
  - A successful place no longer calls `EnterSelectMode`. Place mode and the ghost stay until an explicit exit (Esc, P, the Library PLACE toggle, the Edit menu).
  - Each click is still exactly one item, and a held button still places only the item of its press: the 90-frame hold test placed +1.
  - After each stamp:
    - The armed item's verdict is re-evaluated, because a session limit may now be reached and the ghost must turn red.
    - `LastLandingPoint` resets, so the ghost moving onto the new item does not play the Snap cue on top of the Place cue.
    - The controller requests a preview refresh, so the ghost immediately shows where the next click lands. With the cursor still over the new item, that is on top of it (stacking, WYSIWYG).
- **Library panel redesign** (`UStageItemLibraryPanel`, still code-built, still overridable by a WBP via `BindWidgetOptional`):
  - **Toolbar:** "LIBRARY", an item count, and a **PLACE P** toggle. The toggle is solid amber while in Place mode and outlined otherwise. It follows `OnEditModeChanged` (push) and calls `TogglePlaceMode`.
  - **Search field:** filters by every word, matching the item name or category. It only changes visibility and never rebuilds. The rule is `StageItemLibrary::MatchesSearch` (pure, tested).
  - **Category groups:** new `UStageItemLibraryCategory`. Each has a chevron, an uppercase name and a visible count, and collapses on click. The collapsed state is kept across catalog rebuilds. A group with no matches hides, and while searching every matching group shows its items.
  - **Rows:** a thumbnail tile (the icon, or the item's initial when it has none), the name, and the item type below it. Hover and press surfaces come from the theme. The armed item gets a selection-blue outline and a bold name. The tooltip gives the description plus how stamping works.
  - "Loading library…" / "No items match …" show as an italic empty state instead of a raw status line.
  - **Theme:** every colour comes from `UStageCraftUITheme` (the panel's `Theme`, or the palette class defaults, which are the authored look), so the Library matches the Inspector and Faders. The shared builders (flat button, rounded box, input field, scroll bar, fonts, Starship icons) are in `Private/UI/StageCraftWidgetStyle.h/.cpp`.
- **`UStageToolButton`** (new, a `UButton` that is never keyboard-focusable). It is used for Library rows, category headers and the PLACE toggle. A plain UMG button takes keyboard focus on click, so pressing Esc or P right after picking an item would go to the panel instead of the game. Focusability can only be set before the Slate widget exists, hence a subclass.
- **Workspace status bar** (new `UStageStatusBarWidget`, under the panels in the main window):
  - Left: a **SELECT / PLACE** chip and a context hint (`StageStatusBar::MakeHint`, pure and tested), e.g. "Placing Test Crate: each click places a copy · Esc or P: finish".
  - Right: a message that clears after 4 s (one-shot timer): refusals in warning red (`OnRequestRejected`), purchases (`UStageEconomySubsystem::OnPurchaseCompleted`) and camera speed (`AStageCameraPawn::OnFlySpeedChanged`, re-bound on `OnPossessedPawnChanged`).
  - It is a view only: owned by the local controller, every delegate bound in `NativeConstruct` and unbound in `NativeDestruct`.
  - The shell holds an `SBox` slot filled through a new `CreateStatusBarContent` callback and refreshed with the panels (`RefreshPanelContent`), so a new controller after travel gets a fresh bar.
- **No more text over the viewport.** The controller's three `AddOnScreenDebugMessage` calls (camera speed, purchase, refusal) are removed. Their information is in the status bar, and refusals still play the error cue. (The red "Video memory has been exhausted" line seen in test screenshots is the engine's own non-Shipping warning, caused by sharing the GPU with another open editor. It is not project code.)
- **Bug found and fixed: keyboard dead after any layout change.**
  - **Symptom.** After `Workspace.Reset`, loading a layout, or floating or docking the viewport, Esc, P and Space did nothing until the viewport was clicked. Found with the injected-key test below; it predates this task (Phase 1, #22).
  - **Root cause.** Re-parenting the viewport into new tabs leaves Slate's per-user focus path running through the destroyed tabs. Key routing walks that path and reaches nothing. Simply setting focus to the viewport again is a no-op, because Slate skips a focus change to the widget that is already focused (`FSlateApplication::SetUserFocus`, "Focus Has Not Changed").
  - **Fix.** `FStageWorkspaceShell::HandleViewportMoved`, which every viewport move already passes through, now clears all user focus and then focuses the game viewport, rebuilding the path through its new parents.
- **Wording.** The Edit menu tooltip, `StagePlacementTypes.h` and the controller and tool header docs describe stamping instead of "one click, back to Select".
- **Dev tooling** (non-Shipping, compiled out):
  - `StageCraft.Edit.Key <Key>` injects a key through `FSlateApplication` with the platform's default keyboard device and holds it for 4 frames. It follows the OS route (focused widget → viewport → Enhanced Input → action bindings) and logs the focused widget.
  - `StageCraft.Workspace.Screenshot` saves the whole window with panels to `Saved/Screenshots`. Engine `shot showui` does not reach the viewport client from `GEngine->Exec`, which is what `Delay` uses.

**Files changed**
- New:
  - `Source/ModularSceneBuilder/Public/UI/StageStatusBarWidget.h`, `Private/UI/StageStatusBarWidget.cpp`
  - `Public/UI/StageToolButton.h`, `Private/UI/StageToolButton.cpp`
  - `Private/UI/StageCraftWidgetStyle.h/.cpp`
  - `Private/UI/Tests/StageUITests.cpp`
- Rewritten: `Public/UI/StageItemLibraryPanel.h`, `Private/UI/StageItemLibraryPanel.cpp`. `StatusText` is renamed to `EmptyStateText`; no WBP used it.
- Modified:
  - Placement: `Private/Placement/StagePlacementToolComponent.cpp`, `Public/Placement/StagePlacementToolComponent.h`, `Public/Placement/StagePlacementTypes.h`, `Private/Placement/StagePlacementConsoleCommands.cpp`
  - Controller: `Private/Player/ModularPlayerController.cpp`, `Public/Player/ModularPlayerController.h`
  - Workspace: `Private/Workspace/StageWorkspaceShell.h/.cpp`, `Private/Workspace/StageWorkspaceSubsystem.cpp`, `Public/Workspace/StageWorkspaceSubsystem.h`, `Private/Workspace/SStageWorkspaceMenuBar.cpp`, `Private/Workspace/StageWorkspaceConsoleCommands.cpp`
- Docs: `Docs/ADR/0002-placement-and-audio.md` (§3.1 superseded note, §4 wording, §7 amendment), `tasks/todo.md` (Phase 7b; shop item reworded), `STATE.md` (#23 commit hash `9ec2c13`, this entry).
- No content assets, maps, input assets or Blueprints were changed. `DA_Panel_Library` still points at `UStageItemLibraryPanel`.

**Verification**
- **Build note.** The open editor belongs to a different project (Kmidden), and its Live Coding session holds the engine-wide `UnrealEditor.exe` mutex that UBT checks. Builds therefore ran with `-NoHotReloadFromIDE`, which skips only that check; none of this project's DLLs are loaded there.
- **Builds (Claude):** Editor Development, Game Development and Game Shipping all Succeeded with 0 errors and 0 warnings. The final incremental builds include the snap-cue fix, and Shipping proves the new dev commands compile out.
- **Automation (headless):** `StageCraft` filter, 12/12 passed (rerun after the last change):
  - the new `UI.LibrarySearch` and `UI.StatusHint`;
  - `Placement.Math`, `Placement.ToolModes`, `Gizmo.ScaleRules`, `Audio.Math`, `Audio.UserSettings`;
  - 5× `Workspace.LayoutStore`.
- **Standalone `-game`, scripted** (`Saved/Logs/UI24_Run7.log`, screenshots by `StageCraft.Workspace.Screenshot`). The Default layout was reset first, the exact case that used to kill the keyboard.
  - **Stamping:**
    - Arm Test Crate → Place.
    - 3 clicks at different points plus a 90-frame hold: items 4 → 8 (+1 per click, +1 for the hold). Mode stayed Place throughout.
    - After each stamp the ghost re-landed on the new crate (Z 97.35).
  - **Esc and P through the real key path:**
    - Esc → Place to Select (ghost hidden).
    - P → Select to Place, and P again → Place to Select.
    - Esc in Select clears the armed item (`armed None`).
    - After `FloatViewport`, P → Place; after `Reset`, Esc → Select.
    - Before the focus fix, every one of these key presses was lost after the Reset (Run6). Without a Reset they worked (Diag2), which isolated the cause.
  - **Refusal:** arming the locked Moving Head Wash → refused + error cue. The screenshot shows "Test Moving Head Wash is locked. Unlock it in the shop." in red at the right of the status bar, and nothing over the viewport.
  - **Screenshots checked by Claude:**
    - Place: dark Library with toolbar, count, lit PLACE toggle, search, collapsible groups with counts, armed row outlined, ghost on the stage, PLACE chip and hint.
    - Select: ghost gone, toggle outlined, SELECT chip with "P: place Test Crate".
    - Refusal: the message on the status bar.
  - **Snap-cue fix** (`UI24_Run8.log`): each stamp plays exactly one Place cue, and Snap plays only when the pointer moves to a new grid point; before the fix every stamp was followed by a Snap. Esc after the Reset → Select.
- **Not verified by Claude (needs Gevor, by mouse and keyboard in Standalone Game via RunEditor.bat → Play → Standalone Game):**
  - hover and press feel of rows and headers; collapsing and expanding a category; typing in the search field;
  - clicking the PLACE toggle;
  - stamping with real mouse clicks;
  - Esc and P on the physical keyboard right after clicking a Library item;
  - the camera-speed message when scrolling the wheel;
  - that the status bar reads well at your resolution.
- Status: **implemented, awaiting manual verification** of the items above.

**Commit:** `ecc908b`.

**Known issues / follow-ups**
- **PIE has neither the Library nor the status bar** (fixed HUD). Refusals in PIE are logged and play the error cue but are not shown. Add `UStageStatusBarWidget` and `UStageItemLibraryPanel` to `WBP_StageCraftHUD` (designer step).
- **Test catalog items have no icons,** so rows show an initial tile ("T" for every "Test …" item). Real thumbnails come from each item's `Icon`.
- Locked items are not marked in the Library yet; a lock badge is a follow-up. Clicking one is refused with a status-bar message and the error cue.
- STATE #23's manual checklist still applies, except "one click returns to Select", which is now stamping.

## #25 — Undo/redo command history, object-to-object snapping with alignment guides (2026-10-06)

**Request (Gevor).**
1. **Undo/redo.** A Command Pattern history (an `ICommand` interface plus stacks) for Spawn, Transform (move, rotate, scale) and Delete. Shortcuts: Ctrl+Z, and Ctrl+Y or Ctrl+Shift+Z.
2. **Object snapping.** Bounding-box snapping for placing and moving: items lock side by side or stacked onto nearby placed items, with real-time alignment guides.
3. **Standards.** Strict Rules.md: encapsulation, `TSharedPtr` / `TObjectPtr` memory management, small functions, no per-frame tick polling.

The design record, with engine facts, alternatives and rollback, is `Docs/ADR/0003-undo-redo-and-object-snapping.md`.

**What changed and why**

- **Command model: record after doing.**
  - Every tool still acts through its existing validated path. Only afterwards is an `IStageEditCommand` (`GetDescription` / `Undo` / `Redo`, plain C++ held in a `TSharedRef`) recorded.
  - There is no second execute path, so the rules are never bypassed and no tool changed its behaviour.
  - The three commands are `FStagePlaceItemCommand`, `FStageDeleteItemCommand` and `FStageTransformItemCommand`.
  - Transform descriptions say Move, Rotate, Scale or Transform, e.g. "Move Test Crate".
- **Commands act only through `IStageItemEditor`** (`CaptureItem`, `RestoreItem`, `RemoveItem`, `SetItemTransform`).
  - The live implementation is a short-lived adapter in `UStageEditHistoryComponent`; tests use an in-memory fake.
  - `EStageCommandResult`:
    - `Succeeded` moves the step to the other stack.
    - `Refused` (the rules said no, already reported) leaves the stacks unchanged so the user can retry.
    - `Invalid` (the item or its catalog asset is gone) drops the step and is reported.
    - `NothingToDo` means the stack is empty.
- **Stable item identity.**
  - `AModularBaseActor::GetInstanceId()` is an `FGuid` (Transient), assigned at BeginPlay.
  - Undoing a delete creates a new actor with the **same** id (`AssignInstanceId` is only valid before BeginPlay), so older Move steps still find it.
  - `UStageSessionSubsystem::FindItemById` searches the existing registry (no second map), and `RegisterItem` ensures ids are unique.
- **Snapshots.**
  - `FStageItemSnapshot` holds the id, a soft catalog item reference, the transform, the display name, and the actor's `SaveGame` properties as bytes (label, fixture ID, DMX patch, attributes, audio settings), written with `UObject::SerializeScriptProperties` and `ArIsSaveGame`.
  - **Restore order.** A restore writes them before `InitializeFromItemData`, so `ApplyItemData` and BeginPlay see the restored state and a fixture keeps its ID.
  - **Re-capture before removal.** Place and Delete re-capture the snapshot just before each removal, so edits made after placing survive undo + redo.
- **Ownership.**
  - **`UStageEditHistorySubsystem` (`UWorldSubsystem`, Game and PIE only).** It owns `FStageCommandHistory` (pure stacks; depth 100; recording clears redo; not re-entrant) and broadcasts `OnHistoryChanged`. It is per world because the history names one level's items.
  - **`UStageEditHistoryComponent` (on the controller).** It records `UStagePlacementToolComponent::OnItemPlaced`. It provides `DeleteItem`, which the Delete key now uses and which releases the selection first, and `RecordTransformChange`.
  - **`USpawnSystemComponent::RestoreItem`.** It shares one spawn routine with `SpawnItem`, **including `PlacementValidator`**. Undoing a delete past a session limit is therefore refused and reported (deny by default).
- **Recording points: one user action is one step.**
  - a placement click;
  - a Delete;
  - a whole gizmo drag, recorded once from the new `AModularTransformGizmo::OnDragFinished`, which fires once per drag including drags cut short by a mode or target change;
  - an accepted Location / Rotation / Scale edit in `RequestParameterChange`, recording the clamped value actually stored.
  - Undo and redo never pass through these points, so they cannot record themselves.
- **Request bridge.** `AModularPlayerController::RequestUndo` / `RequestRedo` serve the keys, the Edit menu and the console.
  - They are ignored during a gizmo drag or RMB navigation.
  - They report `Invalid` on `OnRequestRejected`, so the status bar shows a warning and the error cue plays.
  - They refresh the ghost.
- **Shortcuts (Enhanced Input chords).**
  - The default editor context gains Ctrl (left/right) and Shift modifier actions. The mappings are Z + Ctrl + Shift → Redo, then Z + Ctrl → Undo, then Y + Ctrl → Redo.
  - Mapping Ctrl+Shift+Z first matters: Enhanced Input injects a chord blocker into the later Z mapping, so Ctrl+Shift+Z redoes **without** also undoing. Verified below.
  - `UndoAction` / `RedoAction` are assignable input slots like the others.
  - Focused text fields (inspector, Library search) keep the keys for their own text undo.
- **Object snapping.**
  - **`StageSnapMath` (pure, tested).** Per allowed axis, the moving AABB locks onto the closest neighbour candidate within 20 cm: flush after or before (side by side, stacked), or align min / max / centre (edges and centres line up).
    - A neighbour only counts on an axis when the boxes overlap, or are within the threshold, on both other axes, so items across the stage never pull.
    - Results are deterministic (ties keep the first neighbour and anchor).
    - The module also produces guide segments.
  - **`UStageSnappingComponent` (on the controller)** supplies item bounds (colliding components only, never the gizmo), the user preference, the guide thickness (by camera distance) and `OnSnapEngaged` (plays the existing Snap cue).
  - **Placement** (`UStagePlacementToolComponent::PlacementSnapper`).
    - X and Y snaps are measured at the cursor's free landing point, so off-grid neighbours are reachable. Snapped axes win; other axes keep the item's grid; Z stays with the surface trace.
    - The preview and `TryPlace` share one `ComputeLandingTransform`, so the ghost is still exactly where the item lands.
  - **Gizmo Move** (`AModularTransformGizmo::TranslationSnapper`) snaps along the dragged axis, Z included (stack by dragging up).
    - Neighbour boxes are cached once per drag (`OnDragStarted` → `BeginMove`, `OnDragFinished` → `EndMove`), so the per-frame drag update does no searching.
  - **Guides.** `UStageSnapGuidesComponent` draws up to three world-space magenta lines with the always-on-top gizmo material. One is on the placement preview and one on the gizmo. It has no tick and no collision.
  - **Preference.** Edit > **Snap to Items** (checkbox) is stored in `UStageCraftUserSettings` (`bSnapToItems`, default on) and saved on toggle. The snapping component is its only writer.
- **UI.**
  - The Edit menu gains a History section: "Undo Move Test Crate (Ctrl+Z)" and "Redo … (Ctrl+Y)", greyed when empty.
  - It also gains a Snapping section.
  - The status bar shows "Undo: …" / "Redo: …" through `OnHistoryChanged`.
- **Dev console** (non-Shipping):
  - `StageCraft.Edit.{History, Undo, Redo, Delete, Snap [on|off], DragGizmo <X|Y|Z> <cm>}`.
  - `StageCraft.Edit.Key` now takes chords (`Ctrl+Z`, `Ctrl+Shift+Z`), pressed through Slate like the keyboard.
  - `Status` reports the visible snap guide count.
- **STATE.md housekeeping.** #24's commit line said "uncommitted"; it now carries `ecc908b`.

**Files changed**
- New, in `Source/ModularSceneBuilder`:
  - History: `Public/History/StageItemSnapshot.h`, `StageEditCommand.h`, `StageItemCommands.h`, `StageCommandHistory.h`, `StageEditHistorySubsystem.h`, `StageEditHistoryComponent.h`; `Private/History/StageItemCommands.cpp`, `StageCommandHistory.cpp`, `StageEditHistorySubsystem.cpp`, `StageEditHistoryComponent.cpp`, `Tests/StageHistoryTests.cpp`.
  - Snapping: `Public/Placement/StageSnapTypes.h`, `StageSnapMath.h`, `StageSnappingComponent.h`, `StageSnapGuidesComponent.h`; `Private/Placement/StageSnapMath.cpp`, `StageSnappingComponent.cpp`, `StageSnapGuidesComponent.cpp`, `Tests/StageSnapTests.cpp`.
- Modified, in `Source/ModularSceneBuilder`:
  - Actors: `ModularBaseActor.h/.cpp` (instance id, snapshot capture/restore), `ModularTransformGizmo.h/.cpp` (drag delegates, translation snapper, guides, dev handle position).
  - `Components/SpawnSystemComponent.h/.cpp` (`RestoreItem`, shared validated spawn).
  - `Subsystems/StageSessionSubsystem.h/.cpp` (`FindItemById`, id-uniqueness ensure); `Data/StageParameterTypes.h/.cpp` (`StageCraftTags::IsTransformParameter`).
  - Placement: `StagePlacementToolComponent.h/.cpp` (`PlacementSnapper`, `ComputeLandingTransform`), `StagePlacementPreview.h/.cpp` (guides), `StagePlacementConsoleCommands.cpp`.
  - Controller: `Player/ModularPlayerController.h/.cpp` (components, Undo/Redo actions and chords, request bridge, recording, snapping bindings).
  - `Audio/StageEditorAudioFeedbackComponent.h/.cpp` (object-snap cue).
  - `Settings/StageCraftUserSettings.h/.cpp` (`bSnapToItems`).
  - `UI/StageStatusBarWidget.h/.cpp` (undo/redo messages); `Workspace/SStageWorkspaceMenuBar.cpp` (History and Snapping sections).
- Docs: `Docs/ADR/0003-undo-redo-and-object-snapping.md` (new), `tasks/todo.md`, `STATE.md` (#24 commit hash, this entry).
- No content assets, maps, input assets or Blueprints were changed. The guides reuse `/Game/StageCraft/Gizmo/M_GizmoHandle`.

**Verification**
- **Builds (Claude):** Editor Development, Game Development and Game Shipping all Succeeded with 0 errors and 0 warnings. The only failure on the way was a missing include, which was fixed. Shipping proves the new dev commands and `DevGetMoveHandleLocation` compile out.
- **Automation (headless):** `StageCraft` filter, **15/15 passed**:
  - New: `History.Stack`, `History.ItemCommands`, `Snap.Math`.
  - **`History.Stack` covers:** LIFO, redo cleared by a new action, depth cap, Refused kept, Invalid dropped.
  - **`History.ItemCommands` covers:**
    - place / delete / transform undo and redo;
    - edits surviving undo + redo;
    - an older Move step still applying after undo of a delete;
    - refused restore;
    - missing item / asset → Invalid.
  - **`Snap.Math` covers:** flush, align, inclusive threshold, relevance, stacking, closest neighbour and tie order, degenerate input, grid merge, guide geometry and engagement signature.
  - All 12 earlier tests still pass.
- **Standalone `-game`, scripted by Claude** (`L_StageTest`, simulated cursor, keys injected through Slate, 0 ensures in every run):
  - **Undo/redo (`Saved/Logs/UR_Run1.log`).**
    - Placed 2 crates → 2 steps. Moved B with the inspector-equivalent `SetLocation 0 0 50` → "Move Test Crate".
    - **Ctrl+Z** (key path, focus `SViewport`) → B back at (-200, 200, 50).
    - **Ctrl+Shift+Z** → redo only, ending at 3 undo / 0 redo (Ctrl+Z did not also fire).
    - **Ctrl+Z** then **Ctrl+Y** → undo + redo.
    - Delete → 5 items. **Ctrl+Z** → "Restored ModularBaseActor_2 at (0, 0, 50)", 6 items.
    - Then the older "Move" undo applied to the restored actor, through the same instance id.
    - Undo ×4 → both crates removed, the 4th = `NothingToDo`. Redo ×3 → both crates restored at their positions, the Move re-applied.
  - **Gizmo snap (`SN_Run1.log`).** Crate A at y -100 (spans -150..-50); B at y 200.
    - `DragGizmo Y -185` (a real handle press, 20 held frames, release) would leave a 15 cm gap. Instead B locked **flush at Y = 0**.
    - The drag recorded exactly one "Move Test Crate" step.
  - **Placement snap (`SN_Run4.log`, screenshot `Saved/Screenshots/WindowsEditor/ScreenShot00006.png`).** Crate A was moved off-grid to (-163, 37). The cursor was placed 10 cm from its side.
    - With snapping on: ghost at **Y = 137 (flush)**, 1 guide drawn, and the click placed exactly there.
    - With snapping off: grid **Y = 100**, 0 guides.
    - The screenshot shows the magenta guide at the contact face and the ghost flush against the crate.
  - **Stacking (`SN_Run1.log`).** The ghost over an off-grid crate landed on its top with Y aligned to it (37; the grid gives 0). With snapping off, Y = 0.
- **Not verified (needs Gevor, by mouse and keyboard in Standalone Game):**
  - **Shortcuts:** Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z on the physical keyboard; and that they do nothing to the stage while typing in an inspector field or the Library search.
  - **Snapping by hand:** a real mouse gizmo drag that snaps and shows the guide; guide readability at other camera distances; hearing the Snap cue when a snap engages.
  - **Menus and status bar:** the Edit menu History labels and the Snap to Items checkbox; the "Undo: …" status messages.
  - **Untested paths:** a restore refused by a session item limit (unit test only; no test map has a limit); undo of a fixture delete (fixture ID, patch and attributes restored), which is untested at runtime.
- Status: **implemented, awaiting manual verification** of the items above.

**Commit:** `1ff9177`.

**Known issues / follow-ups**
- **Undo scope.** Non-transform parameter edits (label, patch, attributes, Type swap) are not undo steps yet; they do survive undo/redo of Place/Delete through the snapshot. They would be one more command type at `RequestParameterChange`.
- **Placement snapping needs an item `Mesh`.** Items whose visuals come only from a Blueprint snap to the grid while placing; once placed, they do snap when moved.
- **No hold-to-bypass key.** Snapping can only be switched off with the Edit menu toggle.
- **History is per level** (max 100 steps) and is cleared by level travel.
- **Obsidian vault.** `S:\` is still unreachable on this machine; the design record is ADR 0003 in the repo.

## #26 — Multi-selection and batch edits, Clear Stage, scene save/load, render subsystem and panel (2026-10-08)

**Request (Gevor).** A milestone in four parts, under Rules.md:
1. **Batch selection.** Ctrl+Click multi-select; Delete removes the selected item(s); deletion and transforms act on the whole selection.
2. **Clear All.** Remove every placed item, with a safety check against accidental wipes.
3. **Save / load.** Serialize the stage (item classes, transforms, instance ids, metadata) to disk, with UI to save and reload at runtime.
4. **Render.** A render subsystem and panel: 4K / 1080p / square presets, anti-aliasing and post-processing options, optional watermark, one-click non-blocking renders into a project folder.

The design record, with verified engine facts, alternatives, measurements and rollback, is `Docs/ADR/0004-selection-scenes-and-rendering.md`.

**What changed and why**

- **Selection set** (`USelectionComponent`).
  - An ordered set; the last entry is the primary (gizmo target, inspector subject). One private mutation path (`ApplySelection`) keeps overlays, destroy bindings and notifications consistent.
  - New: `ToggleActorSelection` (Ctrl+Click), `DeselectActor`, `SelectActors`, `GetSelectedActors`, `GetSelectionCount`, `IsActorSelected`, and `OnSelectionSetChanged(Count)`. `OnSelectionChanged` keeps its meaning for the primary, so the inspector, gizmo and audio feedback are unchanged.
  - Controller: Ctrl (read from Slate's modifier state) turns a click on an item into a toggle; Ctrl+Click on empty space keeps the selection.
- **Group transforms.**
  - `StageGroupTransform::ApplyLeaderChange` (pure): followers move by the leader's offset, orbit its pivot by its rotation, and scale in place by its ratio (shared clamp).
  - `UStageGroupTransformComponent` (controller) applies it during a gizmo drag and around a numeric Location / Rotation / Scale edit of a selected item, so both tools move the group.
  - Snapping ignores followers as neighbours (`UStageSnappingComponent::BeginMove(Leader, MovingWith)`).
- **One action, one undo step.** `FStageBatchCommand` is all or nothing: a part-way failure reverts what it applied. `UStageEditHistoryComponent::DeleteItems`, `ClearStage` and `RecordTransformChanges` record one step ("Delete 2 items", "Clear Stage (4 items)", "Transform 2 items"). `UStageEditHistorySubsystem::ClearHistory` serves loads.
- **Clear Stage.** Edit > Clear Stage... opens a confirmation submenu with the item count; the clear is undoable; the console form needs `confirm`. It removes every stage item, including level-authored ones (never level geometry).
- **Edit menu.** New Selection section: Select All, Deselect All, Delete N Selected Items, Clear Stage....
- **Scene files.**
  - `UStageSceneComponent` (controller): async save and load (`UE::Tasks` + `AsyncTask(GameThread)`, weakly held component, one operation at a time), delete, list.
  - `FStageSceneStore`: JSON format v1 in `Saved/StageCraft/Scenes/<Name>.json` (GUID, catalog path, label, location, quaternion, scale, base64 SaveGame state), atomic writes, and validation of untrusted files (structure, version, item cap, ids, paths, finite and in-range transforms, state size; scale clamped; duplicate ids repaired; damaged files left untouched).
  - Loading replaces the stage through `USpawnSystemComponent::RestoreItem`, so the placement rules and entitlements decide; refusals are counted silently and summarised. It keeps instance ids, accepts only catalog items, clears the undo history and marks the session clean.
  - `UStageSessionSubsystem` gains the scene name and an edit serial; a save marks the session clean only if nothing was edited while it wrote.
  - `AModularBaseActor` instance ids are now persisted by scene files (doc comment updated).
  - New **File** menu: the scene name and unsaved state, Save, Save Scene As... (warns before replacing another scene), Open Scene (asks before discarding unsaved changes), Delete Scene (confirm submenu), Open Scenes Folder.
- **Render subsystem** (`UStageRenderSubsystem`, world subsystem).
  - A job runs Capturing → Reading → Encoding: a transient scene capture of the player camera renders a few warm-up frames, the image is read back through a GPU readback polled on the render thread, and a worker filters (supersampling), compresses and atomically writes the PNG to `Saved/Renders`.
  - The capture matches the viewport's lighting: Lumen GI and reflections are overridden on, and temporal AA is enabled with a persistent view state. Scene captures default both off.
  - The gizmo, the placement ghost and selection/hover overlays (`AModularBaseActor::SetHighlightSuppressed`) are left out of the image.
  - Settings: resolution (4K 3840x2160, 1080p, Square 2160x2160), anti-aliasing (Off, FXAA, Temporal, Supersampled), post-processing (Clean, Standard, Cinematic), watermark footer (scene, date, size, item count). Stored in `UStageCraftUserSettings`; the subsystem is the only writer.
  - Supersampling never renders more than one 4K frame of pixels (see Verification for why).
  - One job at a time; cancelled with its world; nothing ticks while idle.
- **Render UI.**
  - `UStageRenderPanel`: code-built and themed; `StageCraft.Panel.Render` / `DA_Panel_Render`; a second tab next to the Inspector in the Default layout. It has `UStageChoiceButton` segmented options, a summary, RENDER IMAGE, status and Open Folder.
  - New **Render** menu: Render Image, Render Settings..., Open Renders Folder.
  - The status bar shows a selection-aware hint, scene save/load results and render progress/results.
- **Dev console** (non-Shipping):
  - `StageCraft.Edit.{Click [ctrl], Items, SelectAll, ClearStage confirm}`; `Delete` now acts on the whole selection.
  - `StageCraft.Scene.{Status, Save, Load, Delete}`.
  - `StageCraft.Render [4K|HD|Square] [Off|FXAA|Temporal|Super] [Clean|Standard|Cinematic] [wm|nowm]` and `StageCraft.Render.Status`.
- **STATE.md housekeeping.** #25's commit line said "uncommitted"; it now carries `1ff9177`.

**Files changed**
- New, in `Source/ModularSceneBuilder`:
  - `Public/Interaction/StageGroupTransform.h`, `Private/Interaction/StageGroupTransform.cpp`
  - `Public/Components/StageGroupTransformComponent.h`, `Private/Components/StageGroupTransformComponent.cpp`
  - `Public/History/StageBatchCommand.h`, `Private/History/StageBatchCommand.cpp`
  - `Public/Scene/StageSceneTypes.h`, `StageSceneComponent.h`; `Private/Scene/StageSceneStore.h/.cpp`, `StageSceneComponent.cpp`, `StageSceneConsoleCommands.cpp`, `Tests/StageSceneStoreTests.cpp`
  - `Public/Render/StageRenderTypes.h`, `StageRenderSubsystem.h`; `Private/Render/StageRenderTypes.cpp`, `StageRenderSubsystem.cpp`, `Tests/StageRenderTests.cpp`
  - `Public/UI/StageRenderPanel.h`, `Private/UI/StageRenderPanel.cpp`
- Modified, in `Source/ModularSceneBuilder`:
  - `ModularSceneBuilder.Build.cs` (private `RenderCore`, `RHI`, `ImageCore`)
  - `Components/SelectionComponent.h/.cpp` (rewritten as a set)
  - `Player/ModularPlayerController.h/.cpp` (Ctrl+Click, group edits, the Delete / SelectAll / ClearStage / Save / Load / DeleteScene / Render requests, scene and group components)
  - `History/StageEditHistoryComponent.h/.cpp`, `StageEditHistorySubsystem.h/.cpp`
  - `Placement/StageSnappingComponent.h/.cpp` (`MovingWith`), `StagePlacementToolComponent.h` (`GetPreviewActor`), `Private/Placement/StagePlacementConsoleCommands.cpp`
  - `Subsystems/StageSessionSubsystem.h/.cpp` (scene name, edit serial)
  - `Actors/ModularBaseActor.h/.cpp` (`SetHighlightSuppressed`, id doc)
  - `Settings/StageCraftUserSettings.h/.cpp` (render settings)
  - `UI/StageStatusBarWidget.h/.cpp`, `UI/StageToolButton.h/.cpp` (`UStageChoiceButton`), `Private/UI/Tests/StageUITests.cpp`
  - `Workspace/StageWorkspaceTypes.h/.cpp` (`Panel_Render`), `Private/Workspace/StageWorkspaceShell.cpp` (Default layout), `Private/Workspace/SStageWorkspaceMenuBar.h/.cpp` (File, Render, Edit Selection)
  - `Private/History/Tests/StageHistoryTests.cpp` (batch tests)
- Content (new): `Content/StageCraft/UI/Panels/DA_Panel_Render.uasset`, made headless by `Scripts/Content/create_render_panel_content.py`; its tag reads back as `StageCraft.Panel.Render`.
- Scripts (new): `Scripts/Content/create_render_panel_content.py`.
- Docs: `Docs/ADR/0004-selection-scenes-and-rendering.md` (new), `tasks/todo.md`, `STATE.md` (#25 commit hash, this entry).
- No maps, input assets or Blueprints were changed.

**Verification**
- **Build note.** The Kmidden editor was open (Live Coding mutex), so builds used `-NoHotReloadFromIDE`; its MCP bridge was not used.
- **Builds (Claude):** Editor Development, Game Development and Game Shipping all Succeeded with 0 errors and 0 warnings. Shipping proves the new dev console commands compile out.
- **Automation (headless):** `StageCraft` filter, **20/20 passed**, rerun after the supersampling-budget change. New:
  - `Selection.GroupTransform`: move, orbit, in-place scale, clamp, zero-scale axis, determinism.
  - `History.Batch`: group delete undo/redo, all-or-nothing rollback on a part-way refusal, group move, missing member → Invalid.
  - `Scene.Format`: round trip (ids, transforms, state bytes, label), corrupt / wrong format / future version, 8 kinds of damaged items dropped one by one, oversized state, clamping, duplicate ids, item cap.
  - `Scene.Files`: names, list filtering and order, atomic save, load, missing, invalid name, damaged file kept, delete.
  - `Render.Rules`: presets, supersample budget, internal sizes, warm-up frames, file names, watermark text, sanitising.
  - `UI.StatusHint` extended for multi-selection.
- **Standalone `-game`, scripted by Claude** (`L_StageTest`, simulated cursor; logs `Saved/Logs/R26_Run1`-`Run6.log`; 0 ensures, 0 errors):
  - **Run1:** the Default layout has the Render tab; 4 panel definitions loaded. The screenshot shows the Render panel, the File/Render menus and the Ctrl+Click hint.
  - **Run2, selection and batch:**
    - Click Spot 101, Ctrl+Click Spot 102 → 2 selected (primary Spot 102). The status bar reads "2 items selected: the gizmo moves them together · Delete: remove all · Ctrl+Click: add / remove" (screenshot).
    - Gizmo drag X → both moved +98.6 cm, recorded as one step "Transform 2 items"; undo/redo move both.
    - Numeric Location on the primary (+101.4, +100, +100) → the follower moved by the same delta. Scale 2 → the follower is scaled 2 in place. Two undos restore both.
    - Ctrl+Click toggles Spot 101 out and back in. Delete → "deleted 2 items", one step "Delete 2 items"; undo restores both with the same instance ids.
    - Select All → 4. `ClearStage` without `confirm` → usage only; with `confirm` → 4 removed as "Clear Stage (4 items)"; undo restores all 4 with their ids.
  - **Run2, scenes:** Save "Run26 A" → file written, session clean. Clear → unsaved. Load → 4 items with their saved ids and transforms, history cleared, clean. Loading a missing scene and saving as "bad/name" are refused with messages.
  - **Run3:** after a restart, Load "Run26 A" replaced the level's 4 items with the saved 4 (same ids, moved positions). The file is readable JSON with tagged property state.
  - **Renders** (RTX 3060 Ti 8 GB, shared with the open Kmidden editor). Job is total time; frame time is the average game frame while the job ran:

    | Preset | Internal | Job | Frame time |
    |---|---|---|---|
    | 1080p Temporal Standard | 1920x1080 | 1.33 s | ~20 ms |
    | Square FXAA Clean | 2160x2160 | 0.69 s | ~6 ms |
    | 1080p Supersampled Cinematic | 3840x2160 | 2.17 s | ~28 ms |
    | 4K Supersampled Cinematic (after the budget fix) | 3840x2160 | 2.80 s | ~13 ms |
    | 4K Temporal Standard | 3840x2160 | 1.75 s | ~9 ms |
    | Square Supersampled Clean | 2880x2880 | 1.27 s | ~11 ms |
    | 1080p Off | 1920x1080 | 0.34 s | ~7 ms |

    - About 50 frames ran during a 0.3 s 4K PNG encode, so the encode does not block the game thread.
    - A second render during a job was refused ("A render is already in progress."). Closing the window mid-job cancelled it cleanly.
    - **Bug found and fixed:** with the first supersampling budget (20 MP), 4K Supersampled Cinematic rendered at 5962x3354, ran out of video memory and dropped the app to ~1 fps (16 frames in 18 s). The budget is now one 4K frame (8.3 MP); the same preset then took 2.8 s at ~13 ms per frame.
    - **Images checked by Claude:** the gizmo and selection overlay are absent although Spot 102 was selected; colours and gamma match the viewport; the image is opaque. The watermark footer reads e.g. "StageCraft 3D · Run26 A · 2026-10-08 17:46 · 1920×1080 · 4 items". The footer text was undersized at first; it is now sized from its measured height (Run6).
- **Not verified (needs Gevor, by mouse and keyboard, Standalone Game via RunEditor.bat):**
  - Physical Ctrl+Click on items (only the simulated path was driven).
  - A real mouse gizmo drag of a group, and rotating a group with the gizmo (the orbit is unit-tested, not driven at runtime).
  - The File, Edit (Selection, Clear Stage confirm) and Render menus by mouse; the Save As overwrite warning; Open Scene's "discard unsaved changes" submenu.
  - Render panel clicks, Open Folder, and the status bar messages at your resolution.
  - Renders on another GPU.
  - Loading a scene that contains locked items (the refusal summary is not exercised: the test level has no locked items placed).
- Status: **implemented, awaiting manual verification** of the items above.

**Commit:** `37af368`.

**Known issues / follow-ups**
- **Banding in renders.** Wide smooth gradients (beam cones, back wall) band visibly in the Square framing, independent of Clean/Standard and of supersampling (A/B in Run5).
  - The root cause is not established. Candidates: the 8-bit LDR capture path (no tonemapper dither), or the capture view's volumetric fog.
  - Next step: compare with a viewport screenshot at the same framing, then capture HDR and quantise with dithering on the worker.
- **No cancel button** for a running render (jobs take 0.3-3 s at the current budget).
- **Group edits are not rule-checked per follower** (same as gizmo moves). Non-transform inspector edits apply to the primary only.
- **No keyboard shortcuts** for Select All or Save: A and S belong to the higher-priority camera context.
- **PIE has no File/Edit/Render menus or Render panel** (fixed HUD); the console commands work there.
- **Square renders** keep the camera's horizontal field of view.
- **Obsidian vault.** `S:\` is still unreachable on this machine; the design record is ADR 0004 in the repo.


---

## #27 — Parametric spans, Outliner (visibility, lock, groups), Bill of Materials (2026-10-13)

**Request (Gevor).** Under Rules.md:
1. **Spans.** A procedural placement tool for linear elements (truss, railings, cables) by start/end point, repeating whole mesh segments along the path instead of scaling one mesh.
2. **Outliner.** A dockable panel with placed objects grouped by category (Truss, Lighting, Audio, Props), group selection, show/hide, and transform locking.
3. **Bill of Materials.** A lightweight summary subsystem with item counts, combined weight and power (W), shown in a professional panel for production planning.

The design record, with verified engine facts, alternatives and rollback, is `Docs/ADR/0005-spans-outliner-and-bill-of-materials.md`.

**What changed and why**

- **Spans** (ADR 0005 §3).
  - `UStageSpanData`: fit (Whole Segments / Stretch), segment length, max segments (hard cap 500), angle snap, segment parts and joint parts (mesh + material + transform). Specs (weight, power) are per segment.
  - `StageSpanMath` (pure): layout for a requested length, clamping of untrusted lengths, level end point with polar angle snap, run transform, per-segment and per-joint instance transforms.
  - `UStageSpanGeometryComponent` builds a run as instanced meshes, one instanced component per distinct mesh + material.
  - `AStageSpanActor`: one SaveGame value (`SpanLength`, normalised), so undo and scene files need no format change. Inspector "Span" section (Length editable; Segments, Segment, Fit read-only). Spans refuse Scale.
  - `UStageSpanToolComponent` (controller): state machine Inactive → PickStart → PickEnd. Click the start; the ghost (`AStageSpanPreview`) shows the exact run; click the end. The end becomes the next start (chained runs); Esc drops the start, then leaves Place mode. The status bar shows e.g. "6.00 m, 3 segments".
  - **Quantity:** `AModularBaseActor::GetQuantity / PredictQuantity / SupportsScaling`. Session totals and limits count specs × quantity. `FStagePlacementValidator` and `AStageCraftGameModeBase::EvaluatePlacement` take a quantity. `USpawnSystemComponent` now validates the prepared deferred actor (restore, or the new `SpawnConfiguredItem`) and destroys refused ones unfinished. Lengthening checks `UStageSessionSubsystem::EvaluateQuantityIncrease`; shrinking is always allowed.
  - **Undo:** span length edits are one step ("Change Length of …"): `FStageParameterItemCommand`, `IStageItemEditor::SetItemParameter`, `UStageEditHistoryComponent::RecordParameterChange` + `ParameterApplier` (undo/redo go back through the GameMode). Span placements are recorded from `UStageSpanToolComponent::OnItemPlaced`.
- **Visibility and lock** (ADR 0005 §4).
  - `AModularBaseActor` SaveGame flags `bHiddenOnStage` / `bTransformLocked`, exposed as Bool parameters `Visible` / `Locked` (inspector rows, request bridge, scene files, undo snapshots).
  - Hidden: not drawn, no collision, not snapped to, skipped by Select All, deselected when hidden; still saved and billed.
  - Locked: the GameMode (`EvaluateItemEdit`) refuses Location / Rotation / Scale / span length (`SessionRuleViolation`, not `Locked`, which means "not owned"). Deleting is allowed.
  - A gizmo drag or numeric group transform is checked for **every** selected item first (`AModularPlayerController::EvaluateSelectionTransform`): one locked item, or a span under Scale, refuses the whole edit.
- **Outliner.**
  - `StageOutliner` model (pure): discipline = direct child of `StageCraft.Category`; build order Stage, Truss, Rigging, Lighting, Audio, Screen, Cable, Prop, others, Other; natural item order with id tie-break; tri-state hidden/locked per group; filter.
  - `UStageOutlinerPanel` + `UStageOutlinerRow` (code-built, themed): group rows (chevron, eye, lock, name, count) and item rows (eye, lock, label, model / segments), filter box, footer counts. Clicks select through the new `RequestSelectItems` (Ctrl adds/removes; a group click selects the group). Eye/lock write `Visible`/`Locked` through `RequestParameterChange`.
  - Push only: `UStageSessionSubsystem::OnItemListChanged` (new; register, unregister, structure, label/visible/locked/quantity) → rebuild on the next tick; selection changes only restyle.
  - `USelectionComponent::AddActorsToSelection / RemoveActorsFromSelection` (one notification each).
- **Bill of Materials** (ADR 0005 §5).
  - `StageBillOfMaterials` (pure): merge by catalog item (units, placements), group by discipline, subtotals, order-independent totals, defensive against bad specs; CSV with RFC 4180 escaping, UTF-8 BOM, CSV-injection guard; file names.
  - `UStageBillOfMaterialsSubsystem` (world): derived cache marked stale on `OnItemListChanged`, rebuilt lazily; async atomic CSV export to `Saved/StageCraft/Reports` (`UE::Tasks`, weak result, one at a time).
  - `UStageBillOfMaterialsPanel`: tiles (items, units, weight, power), table by discipline with subtotals, Export CSV, Open Folder. `RequestExportBillOfMaterials` on the controller. File menu: Production section (Bill of Materials..., Export Bill of Materials (CSV), Open Reports Folder). The status bar shows export results.
- **Workspace.** Tags `StageCraft.Panel.Outliner` / `StageCraft.Panel.BillOfMaterials`. Default layout: Outliner under the Library; Bill of Materials as a third tab next to Inspector and Render.
- **Tags.** Catalog `StageCraft.Category.Cable`, `StageCraft.Category.Stage.Railing`; parameters `Param.Info.Visible/Locked`, `Param.Span.Length/Segments/SegmentLength/Fit`, `FeatureGroup.Span`; helpers `IsQuantityParameter`, `IsListingParameter`.
- **Dev console** (non-Shipping): `StageCraft.Span.{Status, SetLength}`, `StageCraft.Outliner.{List, Hide, Lock, Select}` (targets: actor name, label or `group:<Discipline>`), `StageCraft.Report.{Status, Export}`.
- **STATE.md housekeeping.** #26's commit line said "uncommitted"; it now carries `37af368`.

**Files changed**
- New, in `Source/ModularSceneBuilder`:
  - `Public/Data/StageSpanData.h`, `Private/Data/StageSpanData.cpp`
  - `Public/Placement/StageSpanMath.h`, `StageSpanPreview.h`, `StageSpanToolComponent.h`; `Private/Placement/StageSpanMath.cpp`, `StageSpanPreview.cpp`, `StageSpanToolComponent.cpp`, `StageSpanConsoleCommands.cpp`, `Tests/StageSpanTests.cpp`
  - `Public/Components/StageSpanGeometryComponent.h`, `Private/Components/StageSpanGeometryComponent.cpp`
  - `Public/Actors/StageSpanActor.h`, `Private/Actors/StageSpanActor.cpp`
  - `Public/Outliner/StageOutlinerModel.h`; `Private/Outliner/StageOutlinerModel.cpp`, `StageOutlinerConsoleCommands.cpp`, `Tests/StageOutlinerTests.cpp`
  - `Public/Report/StageBillOfMaterials.h`, `StageBillOfMaterialsSubsystem.h`; `Private/Report/StageBillOfMaterials.cpp`, `StageBillOfMaterialsSubsystem.cpp`, `StageReportConsoleCommands.cpp`, `Tests/StageReportTests.cpp`
  - `Public/UI/StageOutlinerPanel.h`, `StageBillOfMaterialsPanel.h`; `Private/UI/StageOutlinerPanel.cpp`, `StageBillOfMaterialsPanel.cpp`
- Modified, in `Source/ModularSceneBuilder`:
  - `Actors/ModularBaseActor.h/.cpp` (Visible/Locked, quantity hooks, `GetHighlightOverlay`)
  - `Data/StageParameterTypes.h/.cpp`, `Data/StageItemTypes.h/.cpp` (tags, helpers)
  - `Subsystems/StageSessionSubsystem.h/.cpp` (quantity totals, `OnItemListChanged`, quantity-aware limits)
  - `Game/StageCraftGameModeBase.h/.cpp` (`EvaluatePlacement` quantity, `EvaluateItemEdit`)
  - `Components/SpawnSystemComponent.h/.cpp` (quantity validator, `SpawnConfiguredItem`, validate the prepared actor)
  - `Components/SelectionComponent.h/.cpp` (add/remove sets)
  - `History/StageEditCommand.h`, `StageItemCommands.h/.cpp`, `StageEditHistoryComponent.h/.cpp`, `Private/History/Tests/StageHistoryTests.cpp`
  - `Player/ModularPlayerController.h/.cpp` (span tool routing, selection-wide transform rules, `RequestSelectItems`, `RequestExportBillOfMaterials`, length undo, deselect on hide, Select All skips hidden, span ghost hidden from renders)
  - `Private/Placement/StagePlacementToolComponent.cpp`, `StageSnappingComponent.cpp` (hidden items are not neighbours), `Private/Scene/StageSceneComponent.cpp`
  - `UI/StageStatusBarWidget.h/.cpp` (`MakeSpanHint`, export messages), `Private/UI/Tests/StageUITests.cpp`
  - `Workspace/StageWorkspaceTypes.h/.cpp`, `Private/Workspace/StageWorkspaceShell.cpp`, `Private/Workspace/SStageWorkspaceMenuBar.h/.cpp`
- Content (new, made headless by `Scripts/Content/create_span_outliner_report_content.py`):
  - `Content/StageCraft/UI/Panels/DA_Panel_Outliner.uasset`, `DA_Panel_BillOfMaterials.uasset` (tags read back correctly)
  - `Content/StageCraft/Spans/M_SpanMetal.uasset`, `M_SpanCoupler.uasset`, `M_SpanCable.uasset`
  - `Content/StageCraft/Items/DA_Span_TrussF34.uasset` (whole 2 m sections, 20 parts from engine BasicShapes + a coupler), `DA_Span_Railing.uasset` (1 m stretch, posts), `DA_Span_CableRun.uasset` (1 m stretch)
- Scripts (new): `Scripts/Content/create_span_outliner_report_content.py`.
- Docs: `Docs/ADR/0005-spans-outliner-and-bill-of-materials.md` (new), `tasks/todo.md`, `STATE.md` (#26 commit hash, this entry).
- No maps, input assets or Blueprints were changed. No Blueprint referenced the changed `EvaluatePlacement` / `EvaluateSessionLimits` signatures (checked before changing them).

**Verification**
- **Build note.** The Kmidden editor was open (Live Coding mutex; its MCP bridge rejects this project's token), so builds used `-NoHotReloadFromIDE` and verification was headless and scripted.
- **Builds (Claude):** Editor Development, Game Development and Game Shipping all Succeeded with 0 errors and 0 warnings. Shipping proves the new dev console commands compile out.
- **Automation (headless), StageCraft filter: 29/29 passed** (20 existing + 9 new). The first full run (`R27_Tests.log`) logged all 29 and TEST COMPLETE. The two full re-runs on the final code (`R27_Tests2/3.log`) stop logging during `LayoutStore.Names` with exit code 0 and no failure; re-running `StageCraft.Workspace.LayoutStore` with stdout captured showed all 5 layout tests passing and `TEST COMPLETE. EXIT CODE: 0`, so the gap is an unflushed `-abslog` tail, not a failure. New tests:
  - `Span.Layout` (whole/stretch rounding, clamps, NaN/∞/negative, caps, invalid rules, determinism), `Span.Placement` (free end, 15°/90° polar snap with projected length, level end, fallback yaw, run transform), `Span.Instances` (pitch, a stretched cable gets longer along its own axis without thickening, joints N+1, invalid layout).
  - `Outliner.Model` (discipline mapping, build order, Other last, natural order, id tie-break, tri-state flags, filter, order independence).
  - `Report.Build` (merge by item, units vs placements, subtotals/totals, order independence, bad data), `Report.Csv` (escaping, CSV injection, title block, rows, totals, UTF-8 BOM, file names), `Report.Format`.
  - `History.Parameter` (undo/redo of a length, a refusal keeps the step, a missing item drops it, meaningful-change filter), `UI.SpanHint`.
- **Standalone `-game`, scripted by Claude** (`L_StageTest`, simulated cursor; logs `Saved/Logs/R27_Run1`–`Run4.log`; 0 errors, 0 ensures):
  - **Run1, spans:** Arm F34 truss → PickStart, ghost visible → click → PickEnd, ghost "600 cm, 3 segments" → click places StageSpanActor_0 (quantity 3) → a second click chains StageSpanActor_1 from the first run's end (quantity 2, 90° turn) → Esc back to PickStart → Esc to Select. Screenshot: ghost along the run; status bar "F34 Truss Run (2 m sections): 6.00 m, 3 segments · click to place; the next run starts at its end".
  - **Run1, BOM:** truss 5 sections × 14 kg in 2 runs = 70 kg. SetLength 1000 → 5 segments, 106 instances; the BOM updated to 7 sections, 98 kg; total 212 kg / 940 W. Export wrote `BOM_Untitled_20261013-142846.csv` (UTF-8 BOM, subtotals, `TOTAL,,,11,6,,212.00,,940.00`).
  - **Run1, lock / scale / hide / groups:** Lock group:Truss (3 items) → SetLength refused "…is locked. Unlock it in the Outliner or the inspector…"; a gizmo press on the Move handle was refused with the same message and consumed. Unlocked → SetScale on a span refused "…is sized by its Length, not by scale." Hide group:Lighting → both hidden; Select All → 4 (6 minus 2 hidden). Select group:Truss → 3; Ctrl group:Audio → 4; Ctrl again → 3.
  - **Run1, scenes:** a span locked, Save "R27 A", Load → 6 restored with quantities 5 and 2, the lock and both hidden lights intact, 0 refused.
  - **Bug found and fixed (Run1):** undoing the length increase was refused with "No item." `EvaluateQuantityIncrease` rejected a negative change (a shorter run) as an invalid request, so shortening a span from the inspector would have been refused too. It now allows zero and negative changes. Run2: SetLength 400 → undo → 1000 → redo → 400 → undo → 1000, all Succeeded.
  - **Bug found and fixed (Run1 screenshot):** the Outliner list was empty while its footer counted 6 items. Rows were added to the list before their widget tree was built, and a `UUserWidget` added to a live panel builds its Slate widget from its tree at that moment. Rows are now built first (Run2 screenshot: rows, icons, greyed hidden rows, solid lock).
  - **Run2:** a railing drawn 875 cm → 9 stretched sections with posts; a cable run of 4 units; undoing the cable placement removed its 4 units from the BOM (266.0 kg) and redo restored it. The BOM panel screenshot shows tiles (8 items, 24 units, 267.6 kg, 940 W), the table by discipline with subtotals, and Export CSV.
  - **Run3/4:** outliner rows with long names overlapped their detail text; both texts now share the width, clip and truncate with an ellipsis (Run4 screenshot, cropped and checked).
- **Not verified (needs Gevor, by mouse and keyboard, Standalone Game via RunEditor.bat):**
  - Drawing spans with the physical mouse (only the simulated cursor path was driven), and Esc from the keyboard.
  - Outliner clicks: eye / lock on items and groups, chevrons, Ctrl+Click rows, typing in the filter box.
  - The inspector's Visible / Locked checkboxes and the Span Length field by hand.
  - Bill of Materials panel buttons (Export CSV, Open Folder), the File > Production menu entries, opening the CSV in a spreadsheet.
  - A gizmo **Scale** drag on a span (the refusal shares the path exercised by the locked Move drag, but the dev command only drags Move handles).
  - Session limits with quantities: the test GameMode has no power/weight limits, so a refused long run (red ghost, "not allowed by this stage's limits") was not exercised at runtime.
- Status: **implemented, awaiting manual verification** of the items above.

**Commit:** `3313e01`.

**Known issues / follow-ups**
- Runs of the same item share the catalog name as label (as every item does); no auto-numbering ("Truss Run 1, 2…").
- No hover/selection overlay on spans or other items: the overlay materials are still unassigned (existing backlog item).
- Stretched runs approximate rotated parts (diagonal lacing); whole-segment runs are exact. Spans are drawn level; tilt them with the gizmo.
- Hide/Lock are not undo steps (like the Unreal Editor's eye); they mark the scene unsaved.
- The Type dropdown on a span is not checked against quantity limits (same as other type swaps).
- No keyboard shortcuts for hide/lock; no user-defined layers beyond the catalog disciplines.
- BOM: no rigging point loads or per-phase power distribution (amps) yet.
- PIE has no dock panels (fixed HUD); the console commands work there.
- **Obsidian vault.** `S:\` is still unreachable on this machine; the design record is ADR 0005 in the repo.

---

## #28 — Inspector visual polish, runtime model import (drag and drop, Import dialog, Library thumbnails) (2026-10-13)

**What & why**
Gevor asked for a dark, aligned GrandMA3 / Unreal-style inspector, and for importing `.obj` / `.fbx` / `.gltf` / `.glb` / `.datasmith` models at runtime, by dragging files from Explorer or through an Import button using the OS file dialog, straight into the Library and Place mode. Design record: `Docs/ADR/0006-inspector-polish-and-runtime-model-import.md`.

- **Inspector (root cause):** the value editors are created in C++ inside the row Blueprint's `EditorSlot` and used UMG's default (light) styles. A combo box takes its font only before its Slate widget exists (default Roboto Bold 16). Rounded-box brushes have no size, and the units column ("cm", "°", "×") had content-dependent width, so field columns didn't line up from row to row.
  - `UStageParameterRowWidget::SetTheme` (called by the inspector) themes every editor it creates:
    - Numeric fields: dark, an amber outline while active, a faint fill for ranged values.
    - Checkboxes: an outlined box with an amber check.
    - Dropdowns: `UStageThemedComboBox`, dark with a dark menu.
    - Text fields.
  - Every editor sits in a 22 px box, and vector, rotator and colour components carry X/Y/Z (R/G/B) tags. The theme has new `AxisX/Y/ZColor` entries.
  - Read-only rows collapse the empty editor slot and don't repeat their units.
  - Widget Blueprints (edited through the editor bridge):
    - `WBP_StageParameterRow`: units column min width 18 px, 8 px right padding.
    - `WBP_StageParameterSection`: header text amber `#FFB300`.
- **Import:** `UStageMeshImportSubsystem` (GameInstance) imports through Interchange at runtime.
  - Pipeline: static meshes only, all meshes combined into one, no Nanite, material instances. OBJ files are assumed Y-up metres and corrected (×100, quarter turn).
  - One file at a time, queued; results are collected on the game thread.
  - Every file is validated first: format, exists, ≤ 512 MB, not a duplicate.
  - Each model becomes a `UStageImportedItemData`:
    - It lives in a never-saved package under `/Temp/StageCraft/Imported/IMP_<hash of path>`.
    - It spawns as an `AModularBaseActor`, is free, has no specs, and its category is `StageCraft.Category.Imported`.
  - The subsystem registers it with `UStageItemSubsystem::AddRuntimeItem` (friend-only), which broadcasts `OnCatalogChanged`. The Library rebuilds and the item is armable at once.
  - Imports are remembered in `Saved/StageCraft/Imports.json`, re-imported at startup, and can be named in scene files.
- **Rules path:**
  - `AModularPlayerController::RequestImportModelFiles` / `RequestBrowseModelFiles` go through the new GameMode check `EvaluateModelImport` (session rule `bAllowModelImport`, default on).
  - Refusals are reported once per drop on `OnRequestRejected`. The status bar shows "Importing…", "Imported X (GLB) into the Library. Click it in the Library to place it." and failures.
- **Drag and drop:** `SStageFileDropTarget` wraps every dock tab's content (viewport and panels, docked or floating) in `FStageWorkspaceShell`. It shows an amber "Drop to import into the Library" or a red refusal while files hover, and hands drops to the controller.
- **Import button:** a Library toolbar IMPORT button and File > Library > Import Models… open `IDesktopPlatform::OpenFileDialog`. The filter lists the formats this build supports, and the last folder is remembered for the session.
  - `DesktopPlatform` is a Developer module that the engine licence doesn't allow in shipped builds. It is linked only when `Target.bBuildDeveloperTools`. In Shipping the button and menu entry are hidden, and drag and drop still works.
- **Library cards:** imported items show their format and size ("GLB · 2.00 × 0.80 × 1.05 m") and a 128 px thumbnail. It is rendered once in a private game preview world, drawn with a uniform clay material (`M_ThumbnailClay`, set in `DefaultGame.ini`) for consistent, readable cards.
- **Engine workarounds (see ADR E5/E6/E9):**
  - In Standalone Game from editor binaries, `UStageInterchangeGameUtilities` makes Interchange use its runtime path. Without it, OBJ materials hit `check(GIsEditor)`.
  - Packages go under `/Temp/`, not `/Memory/`; `/Memory/` crashed in `LongPackageNameToFilename`.
  - `FPreviewScene` must be created with `SetEditor(false)`, or a game never renders it.
- **Dev commands** (compiled out of Shipping):
  - `StageCraft.Import.File <path>[|<path>…]`, `StageCraft.Import.Status`, `StageCraft.Import.SaveThumbnails`.
  - `StageCraft.Import.DragEnter <x> <y> <files>` / `StageCraft.Import.DragDrop` drive Slate's real OS drag-and-drop entry points.
  - `StageCraft.Edit.Arm` also accepts Library names.

**Files changed**
- **Build and config:**
  - `ModularSceneBuilder.uproject`: `Interchange` plugin enabled.
  - `ModularSceneBuilder.Build.cs`: InterchangeCore/Engine/Pipelines; DesktopPlatform plus `WITH_STAGECRAFT_FILE_DIALOG` for builds with developer tools.
  - `Config/DefaultGame.ini`: `StageMeshImportSubsystem.ThumbnailMaterial`; `DirectoriesToAlwaysCook` += `/Game/StageCraft/UI/Thumbnails`, `/InterchangeAssets/Materials`, `/InterchangeAssets/gltf`.
- **New:**
  - `Public/Import/StageMeshImportTypes.h`, `Private/Import/StageMeshImportTypes.cpp`
  - `Public/Import/StageMeshImportSubsystem.h`, `Private/Import/StageMeshImportSubsystem.cpp`
  - `Private/Import/StageFileDialog.h/.cpp`, `StageInterchangeGameUtilities.h`, `StageModelThumbnailRenderer.h/.cpp`, `StageMeshImportConsoleCommands.cpp`, `Tests/StageMeshImportTests.cpp`
  - `Public/Data/StageImportedItemData.h`, `Private/Data/StageImportedItemData.cpp`
  - `Private/UI/StageThemedComboBox.h/.cpp`
  - `Private/Workspace/SStageFileDropTarget.h/.cpp`
- **Modified (data):**
  - `BaseItemData.h/.cpp`: `RequiresAssetStreaming`, `GetCatalogSubtitle`, `GetCatalogIcon`.
  - `StageItemTypes.h/.cpp`: `Category_Imported`.
  - `StageItemSubsystem.h/.cpp`: `AddRuntimeItem`, `RuntimeItems`, `OnCatalogChanged`; no streaming for runtime items.
  - `StageSceneStore.cpp`: accepts imported item paths.
- **Modified (rules):**
  - `StageSessionTypes.h`: `bAllowModelImport`.
  - `StageCraftGameModeBase.h/.cpp`: `EvaluateModelImport`.
  - `ModularPlayerController.h/.cpp`: import requests, `CanBrowseModelFiles`, `LastImportDirectory`.
- **Modified (UI):**
  - `StageCraftWidgetStyle.h/.cpp`: `FieldHeight`, `MakeNumericField`, `MakeCheckBox`, `MakeComboBox`, `MakeListRow`.
  - `StageCraftUITheme.h/.cpp`: axis colours.
  - `StageParameterRowWidget.h/.cpp`: theming, axis tags, swatch, read-only layout.
  - `StageInspectorPanel.cpp`: passes the theme.
  - `StageItemLibraryPanel.h/.cpp`: IMPORT button, catalog-change rebuild, subtitle and icon via virtuals.
  - `StageStatusBarWidget.h/.cpp`: import messages.
- **Modified (workspace and placement):**
  - `StageWorkspaceShell.h/.cpp`: drop targets.
  - `StageWorkspaceSubsystem.cpp`: drop callbacks.
  - `SStageWorkspaceMenuBar.cpp`: File > Library section.
  - `StagePlacementConsoleCommands.cpp`: Arm by Library name.
- **Content:**
  - `Content/StageCraft/UI/Inspector/WBP_StageParameterRow.uasset`, `WBP_StageParameterSection.uasset` (modified).
  - `Content/StageCraft/UI/Thumbnails/M_ThumbnailClay.uasset` (new).
- **Test models:** `Scripts/TestModels/make_test_models.ps1` and its outputs:
  - `Speaker_Cabinet.gltf` (embedded buffer)
  - `DJ_Booth.glb` (two meshes to combine)
  - `Stage_Riser.obj` / `.mtl`
  - `Not_A_Model.dae` (refusal path)
- **Docs:**
  - `Docs/ADR/0006-inspector-polish-and-runtime-model-import.md`
  - `STATE.md` (this entry; #27 commit set to `3313e01`)
  - `tasks/todo.md` (Phase 11)

**Verification**
- **Builds:** Editor Development, Game Development and Game Shipping are clean (0 errors, 0 warnings). The Shipping exe contains the drop hint but none of the `StageCraft.Import.*` commands and no DesktopPlatform.
- **Automation:** 33/33 pass (29 existing + 4 new):
  - `Import.Formats`: format filtering incl. no FBX translator, extension rules, the OBJ correction.
  - `Import.Identity`: path normalization, stable `IMP_` names, the import root, display names.
  - `Import.DialogFilter`.
  - `Import.List`: round trip, invalid documents, mixed/duplicate/relative/over-long entries, 256 cap.
  - The first run failed on my own wrong assumption about the OBJ rotation sense; the test now asserts the authored value and the runtime check below.
- **Standalone `-game`, scripted by Claude** (`L_StageTest`; logs `Saved/Logs/R28_*.log`; screenshots checked):
  - **Inspector, before vs after:** before, white fields, a huge label font, clipped units and a centred read-only value. After (Spot 101): dark fields, amber headers, X/Y/Z tags, aligned columns, a colour swatch, units once, fill bars on Pan/Tilt.
  - **Imports:** results by format:

    | File | Size | Detail |
    |---|---|---|
    | `Speaker_Cabinet.gltf` | 0.60 × 0.50 × 1.10 m | 1 slot, standing on Z 0 |
    | `Stage_Riser.obj` | 2.00 × 1.00 × 0.40 m | Z 0–40 cm after the OBJ correction |
    | `DJ_Booth.glb` | 2.00 × 0.80 × 1.05 m | two meshes combined, 2 slots |
    | `Cube_Blue.fbx` (engine test file) | 1 m cube | through the Slate drop path |

  - **Refusals:** `.dae` → "cannot be imported. Supported formats: .glb, .gltf, .obj, .fbx"; a missing file → "was not found"; re-importing the cabinet → "already in the Library"; four refusals in one drop give one message "(and 1 more files…)".
  - **Library:** "Imported" category with format/size subtitles and clay thumbnails (PNG export checked); the IMPORT button is in the toolbar.
  - **Place mode:** each imported model armed and placed at once (ModularBaseActor_0–2); the BOM lists them under Imported (0 kg / 0 W).
  - **Scenes:** "R28 Import" saved 7 items and loaded 7, 0 unknown.
  - **Drop overlay:** the screenshot shows "Drop to import into the Library" over the viewport while an FBX hovered; the drop imported it.
  - **Restart:** `Imports.json` listed 3 files; at startup all 3 re-imported, and scene "R28 Restore" loaded 5 placed, 0 unknown.
  - **Bugs found and fixed during the runs:**
    - Formats were read before the translators registered: every import was refused.
    - A `/Memory` package crash.
    - `check(GIsEditor)` on OBJ materials.
    - OBJ came in 100× small and on its side.
    - Thumbnails were black: an editor preview world, then models with uncompiled materials.
    - Axis tags and the swatch were invisible (zero-size brushes).
- **Not verified (needs Gevor, Standalone Game via RunEditor.bat):**
  - A real drag from Windows Explorer with the mouse (the scripted path calls the same Slate entry points Windows does, but not OLE itself). The scripted hover reported drop effect 0 because the OS cursor isn't really dragging; the drop itself was accepted.
  - The IMPORT button / File > Library > Import Models… opening the native dialog and picking several files (the dialog is modal and was not driven by script).
  - Typing in, dragging and tabbing between the restyled inspector fields, and opening the Type dropdown's menu.
  - Real-world models (Blender/SketchUp exports, textured glTF, large FBX); only generated test models were used.
  - A packaged (cooked) build: runtime import, FBX absence, cooked Interchange parent materials, thumbnails.
  - PIE: imports are expected to work (editor utilities answer "PIE"), not run.
- Status: **implemented, awaiting manual verification** of the items above.

**Commit:** `5409813`.

**Known issues / follow-ups**
- **Datasmith (`.udatasmith`) is not supported:** the only runtime path is the beta DatasmithRuntime plugin, which loads whole scenes as actors (ADR 0006 §4.3).
- **No FBX in packaged builds:** the engine's FBX translator is editor-only. glTF/GLB and OBJ work there.
- **This machine's engine install is modified:** `/InterchangeAssets/Materials/PBRSurfaceMaterial_MR` references `/Game/KMidden/Items/Props/Textura_full_pg` and fails to compile, so OBJ materials render with the default material. Check or repair the UE 5.8 install (Verify in the Epic launcher).
- No way to remove an imported model from the Library yet (workaround: delete `Saved/StageCraft/Imports.json` and restart). Imported items have no weight or power.
- OBJ is always treated as Y-up metres; there is no per-import option for other conventions.
- OBJ files without texture coordinates trigger an engine ensure inside the OBJ translator; the import still succeeds.
- A model whose source file moved is reported at startup and stays listed.
- In Shipping there is no Import button: drag and drop only. A Windows-native dialog could be added.
- `StageCraft.Import.DragEnter` cannot make Windows report a copy cursor (synthetic drag).
- **Obsidian vault:** `S:\` is still unreachable; the design record is ADR 0006 in the repo.

---

## #29 — Housekeeping: two commits made after #28 had no STATE.md entry (2026-10-13)

**What & why**
STATE.md was out of sync with `git log`: #28 still said "uncommitted", and two later commits by Gevor had no entry. Per Rules.md this is fixed before any new work.

- **`085cc16` (Gevor): inspector label column.**
  - `WBP_StageParameterRow`: the label sits in a fixed 112 px column (an Auto slot instead of a percentage share) with an 8 px gap. Label, fields, read-only value and units are centred vertically. Fields and the read-only value share the rest of the row.
  - `StageParameterRowWidget.cpp` (`AddToEditorSlot`): the first created editor has no left margin, so fields start where read-only values start. There is a 4 px gap only between neighbouring editors (X / Y / Z).
- **`b8448a5` (Gevor): "Remove exposed Anthropic API key from docs".**
  - Moved `*.uasset` / `*.umap` to Git LFS (`.gitattributes`). The working tree has the real binaries (checked: `L_StageTest.umap` is 35 402 bytes; git-lfs 3.5.1).
  - Added about 20 project plugins under `Plugins/`: AdvancedFadeObjects, BuildManagerMath, CameraPresets, CraneControl, DamenFileManager, DynamicDatatable, FileDropUMG, GizmoTool, NGGVideoRecorder, OverCrowd, PlacementGizmo, PreciseTransform, RealTimeImport, SkyCreator, SkyWeatherControls, SpaceCalculator, SplineRouteTool, DlFileManager and UMGInSequence. Also added the UnrealNGGMCP docs and config.

**Files changed:** `STATE.md` only (#28 commit hash set to `5409813`, plus this entry).

**Verification:** documentation only. Claude checked `git log`, `git show --stat` and the LFS working tree.

**Commit:** `c3936d9` (together with #30).

**Known issues / follow-ups**
- **Rotate the Anthropic API key.** Removing it in a later commit does not remove it from git history, or from any remote that already received it.
- The new plugins broke the build and were disabled in #30. `RunEditor.bat`, which Rules.md mentions, does not exist in this project: Rules.md was written for another project.

---

## #30 — Inspector column grid, professional fixtures (gobo, CTC, iris, shutters, strobe, photometrics), DMX patch sheet, multi-point span paths, import and render hardening (2026-10-13)

**Request (Gevor).** A "professional upgrade" under Rules.md:
1. A strict two-column inspector.
2. Blender-style procedural paths for linear elements.
3. grandMA3-style fixtures: gobo, Kelvin, beam angle, shutter/blades, live photometrics; patch and attributes mapped to the inspector and faders with no polling.
4. Finished runtime import (including `.datasmith`) and render with hidden UI.
5. Rules.md standards throughout.

Spans (#27), render (#26) and import / inspector theming (#28) already existed, so this entry covers the gaps. Gevor chose (2026-10-13):
- multi-point paths;
- projected gobos and blades;
- Datasmith left unsupported, with a native dialog in Shipping;
- disabling the plugins that broke the build.

Design record: `Docs/ADR/0007-lighting-patch-paths-and-pipeline-hardening.md`.

**What changed and why**

- **Build unblocked (`.uproject`).**
  - The 19 plugins from `b8448a5` are set to `"Enabled": false`. They were compiled into the project by default, and `PlacementGizmo` declares `UENUM EGizmoMode`, the same reflected name as StageCraft's gizmo enum.
  - UnrealNGGMCP is also disabled. `b8448a5` reverted its `NGGHttpServer.cpp`/`.h` to an older all-in-one version while the newer split files remain: the header lost two version macros and every handler links twice. Repairing it means choosing between Gevor's two plugin versions.
  - No plugin files were deleted or edited.
- **Inspector column grid** (ADR §3). Root causes, from screenshots:
  - a label longer than its column ("Mode: Standard 12ch") pushed its value right;
  - read-only values started at the field outline while field text starts 7 px inside it;
  - long read-only values ran into the units column.

  The fix:
  - `StageCraftWidgetStyle` holds the grid next to `FieldHeight`: `InspectorLabelWidth` 112 (Gevor's value), `InspectorUnitsWidth` 22, `FieldTextInset` 6, `FieldOutlineWidth` 1. Field styles use the inset constant.
  - `UStageParameterRowWidget::ApplyColumnGrid` (`NativeOnInitialized`, before Slate is built) gives label and units fixed-width size boxes: the Blueprint's own, or one wrapped around them with `StageWidgetTree::WrapInPlace`. The engine reorders live UMG panels only on a full rebuild, hence this timing (ADR F4).
  - Long labels and values end in an ellipsis with a tooltip. Read-only values are inset to the field text.
  - Fixture patch rows: "DMX Mode" (text) and "Channels" replace the "Mode: …" label (new tag `Param.Patch.Mode`).
- **Professional fixtures** (ADR §5).
  - Data:
    - `ULightingFixtureData` gains a gobo wheel (`FStageGoboSlot`: name, texture, transmission), CTC (`bHasColorTemperatureControl`, Kelvin range), iris (`IrisMinFraction`), framing shutters and a strobe rate range.
    - Capabilities, ranges, defaults, default DMX layout and validation all derive from the data.
    - New attributes: `ColorTemp`, `Gobo` (stepped: stored rounded, equal DMX share per slot, cue fades snap), `GoboRotation`, `Iris`, `BladeA-D`, `BladeRotation`.
    - DMX encode/decode moved into pure `NormalizeForDMX` / `DenormalizeFromDMX` on the data.
  - Projection: `M_StageBeamLightFunction` (light-function domain) draws gobo × iris × four rotated blades in cone space (ADR F1). One dynamic instance per fixture with beam shaping, updated only on attribute changes; no light function on pars and washes. CTC drives the light's temperature.
  - Strobe clock: `UShowControlSubsystem::SetFixtureStrobing`. It ticks only while one or more fixtures strobe, and gates each fixture at its own rate with a 30 % duty cycle.
  - Photometrics: `StagePhotometry` (pure) computes beam angle, peak intensity, throw, illuminance and beam diameter, with transmission from dimmer, colour filter, gobo, iris and blades. The throw is one line trace, cached until pan, tilt or the transform changes (new base hook `AModularBaseActor::OnStageTransformChanged`).
  - Inspector: read-only "Photometrics" section; new "Shapers" section; Gobo is a dropdown. Theme colours exist for Shapers and Photometrics, with a fallback to class defaults for theme assets saved before a group existed.
  - Faders: descriptors may carry a short name for narrow strips ("CTC", "Gobo Rot", "Blade T"…), with the full name as the tooltip. The bar gets a Shapers group and scrolls sideways whatever container its Blueprint gives.
- **DMX patch sheet** (ADR §6): `UStagePatchPanel` (`StageCraft.Panel.Patch`, `DA_Panel_Patch`), a tab next to the faders in the Default layout.
  - Fixture table: FID / universe / address fields with read-back, name, type and mode, footprint, red "!" on overlapping patches, and a header with counts and PATCH UNPATCHED.
  - Live channel map of the selected fixture: absolute address, attribute, 8/16-bit, the value the fixture encodes.
  - Push only: `OnFixturesChanged`, per-fixture `OnParameterChanged` (patch and label rebuild the table; attributes refresh the channel values) and the selection set.
  - All writes go through `RequestParameterChange`.
- **Multi-point span paths** (ADR §4).
  - Math: `StageSpanMath` path layout normalises each leg to whole segments (later points follow), supports closed loops (the closing leg must be whole for whole-segment items) and shares the segment cap across legs. Corner frames sit on the bisector, sag is parabolic with chords that meet, and untrusted input is sanitised.
  - Actor: `AStageSpanActor` stores `PathPoints` / `bClosedPath` / `SagDepth` (SaveGame). The old `SpanLength` is read once to migrate older scene files into a one-leg path. Length is editable on one-leg paths and read-only otherwise; new "Legs" and "Sag" rows. `IsShapeParameter` makes Sag an undo step that Lock refuses.
  - Data: `UStageSpanData` gains `CornerParts` and `MaxSagPercent`.
  - Tool: click points; finish with Enter, a double-click or a click on the start point (closes the loop); Backspace removes a point; Esc drops the path. A loop that cannot close refuses the click with a red ghost and a hint, instead of adding a stray point. New controller input `FinishPathAction` (Enter) and `RemovePathPointAction` (Backspace). The status bar shows legs.
  - Content: the F34 truss gets a round hub at bends; the cable may sag 15 %.
- **Import:**
  - `StageFileDialog` opens the Windows `IFileOpenDialog` through COM in builds without DesktopPlatform, so Shipping has the IMPORT button.
  - `.udatasmith` / `.datasmith` get their own refusal ("export as glTF/GLB or FBX"), via `StageMeshImport::IsDatasmithScene`.
- **Render:**
  - Editor helpers (gizmo, placement and span ghosts) register with `UStageRenderSubsystem` on BeginPlay and unregister on EndPlay, so every capture hides them. The controller's hand-kept list is gone.
  - The banding follow-up from #26 was investigated with A/Bs and **not changed** (ADR §7). An HDR capture uses the HDR output device and did not match the viewport. The banding is volumetric-fog depth slicing in Off/FXAA renders, and it does not appear with Temporal/Supersampled (the default). 16 warm-up frames without temporal AA did not help.
- **Library header:** the item count no longer draws under the IMPORT button (ellipsis + tooltip).
- **Dev console** (compiled out of Shipping):
  - `StageCraft.Show.{Set, Photometrics, Patch}`
  - `StageCraft.Span.{Finish, Back, SetSag}` (Status/SetLength updated)
  - `StageCraft.Workspace.{ScrollPanels, DumpWidget}`
- **STATE.md housekeeping:** #29's known-issue line about the plugins is updated.

**Files changed**
- Project and config: `ModularSceneBuilder.uproject` (20 plugins disabled), `Config/DefaultGame.ini` (`LightingFixtureActor.BeamShapingMaterial`, cook `/Game/StageCraft/Lighting`), `Source/ModularSceneBuilder/ModularSceneBuilder.Build.cs` (`WITH_STAGECRAFT_NATIVE_FILE_DIALOG`, ole32/shell32 for Win64 without developer tools).
- New source:
  - `Public/Show/StagePhotometry.h`, `Private/Show/StagePhotometry.cpp`, `Private/Show/StageShowConsoleCommands.cpp`, `Private/Show/Tests/StageLightingTests.cpp`
  - `Public/UI/StagePatchPanel.h`, `Private/UI/StagePatchPanel.cpp`
  - `Private/UI/StageWidgetTree.h/.cpp`
- Modified source (all under `Source/ModularSceneBuilder/`):
  - Actors: `Actors/LightingFixtureActor.h/.cpp`, `Actors/ModularBaseActor.h/.cpp`, `Actors/StageSpanActor.h/.cpp`, `Actors/ModularTransformGizmo.h/.cpp`
  - Data and rules: `Data/LightingFixtureData.h/.cpp`, `Data/StageParameterTypes.h/.cpp`, `Data/StageSpanData.h/.cpp`, `Game/StageCraftGameModeBase.cpp`
  - Spans: `Components/StageSpanGeometryComponent.h/.cpp`, `Placement/StageSpanMath.h/.cpp`, `Placement/StageSpanToolComponent.h/.cpp`, `Placement/StageSpanPreview.h/.cpp`, `Placement/StagePlacementPreview.h/.cpp`, `Private/Placement/StageSpanConsoleCommands.cpp`, `Private/Placement/Tests/StageSpanTests.cpp`
  - Controller and subsystems: `Player/ModularPlayerController.h/.cpp`, `Subsystems/ShowControlSubsystem.h/.cpp`, `Render/StageRenderSubsystem.h/.cpp`
  - Import: `Private/Import/StageFileDialog.h/.cpp`, `Private/Import/StageMeshImportSubsystem.cpp`, `Import/StageMeshImportTypes.h/.cpp`, `Private/Import/Tests/StageMeshImportTests.cpp`
  - UI: `UI/StageParameterRowWidget.h/.cpp`, `Private/UI/StageCraftWidgetStyle.h/.cpp`, `Private/UI/StageCraftUITheme.cpp`, `Private/UI/StageFaderWidget.cpp`, `StageEncoderWidget.cpp`, `StageFaderBankWidget.cpp`, `StageItemLibraryPanel.cpp`, `StageStatusBarWidget.cpp`, `Private/UI/Tests/StageUITests.cpp`
  - Workspace: `Workspace/StageWorkspaceTypes.h/.cpp`, `Private/Workspace/StageWorkspaceShell.cpp`, `Private/Workspace/StageWorkspaceConsoleCommands.cpp`
- Content:
  - New: `Content/StageCraft/Lighting/M_StageBeamLightFunction.uasset`, `Content/StageCraft/Lighting/Gobos/T_Gobo_{Dots,Star,Bars,Ring,Fan,Breakup,Triangles,Grid}.uasset`, `Content/StageCraft/UI/Panels/DA_Panel_Patch.uasset`
  - Modified: `Content/StageCraft/Data/DA_MovingHead_Test.uasset` (profile: CTC, 8-gobo wheel, iris, shutters, "Profile 21ch"), `DA_MovingHead_Wash_Test.uasset` (CTC, "Wash 13ch"), `Content/StageCraft/Items/DA_Span_TrussF34.uasset` (corner hub), `DA_Span_CableRun.uasset` (15 % sag)
- Scripts (new): `Scripts/Content/make_gobos.ps1` + `Scripts/Content/Gobos/T_Gobo_*.png`, `Scripts/Content/create_lighting_content.py`, `Scripts/Content/update_span_path_content.py`.
- Docs: `Docs/ADR/0007-lighting-patch-paths-and-pipeline-hardening.md` (new), `tasks/todo.md`, `STATE.md` (#29 line, this entry).
- No maps, input assets or Widget Blueprints were changed. The row and fader Blueprints are restructured at runtime only.

**Verification**
- **Builds (Claude, UE 5.8):** Editor Development, Game Development and Game Shipping all Succeeded with 0 errors and 0 warnings. The Shipping exe contains none of the new dev command names (ASCII or UTF-16), and does contain the patch sheet and Datasmith strings. Shipping is the build that compiled the native dialog path; its first compile failed on a const cast (fixed).
- **Automation (headless), StageCraft filter: 37/37 passed** (33 existing + 4 new):
  - `Lighting.Photometry`: solid angle, a 20 klm / 20° known case, inverse square, transmission, iris keeps the peak intensity, shutters, colour filters, NaN/negative input.
  - `Lighting.FixtureData`: capabilities, ranges, defaults, gobo names and transmission, overlap-free 21-channel default layout, stepped gobo DMX round trip and share boundaries.
  - `Span.Path`: normalisation keeps directions, corners on the bisector, segments meet across legs, a closed 6 × 4 m rectangle (4 corners, start not doubled), non-whole closures stay open, stretch closes anything, NaN / duplicates / point cap / shared segment cap, `ConstrainPathPoint`, determinism.
  - `Span.Sag`: lowest point at mid-span, chords meet, pitch signs, clamps, rigid and whole-segment items never sag, per-leg limit.
  - Extended: `Span.Instances` (now on paths), `UI.SpanHint` (legs, Enter only after two points, closing, blocked close not blamed on limits), `Import.Formats` (Datasmith detection).
  - The first run failed on my own test data: a "skewed" quad whose third leg normalised so that it closed exactly. The data was corrected.
- **Standalone `-game`, scripted by Claude** (`L_StageTest`, logs `Saved/Logs/R30/R30_Run1`–`Run8.log`, screenshots checked; Gevor's workspace session file was backed up and restored):
  - Patch: Address 10 on Spot 101 → both fixtures flagged CONFLICT (red rows, "2 conflicts"); back to 1 → none. The channel map shows 1.010 Pan 16-bit 32768.
  - Strobe: Shutter 0.5 → "1 strobing"; 0 → "0 strobing".
  - Photometrics match hand calculations:
    - 25 klm at 40° → 65 977 cd.
    - Iris 0.4 + Star gobo + blades + 20° zoom → 57 653 cd (computed 57 650).
    - Tilted at the floor, 14°: throw 3.27 m, 534 kcd, 50 klx, beam 0.80 m.
  - Projection: the Star gobo is projected on the floor (visible at 15 % dimmer; at full the spot over-exposes to white). The light function reports domain `MD_LightFunction`. CTC 3200 K warms the beam.
  - Inspector after the grid fix: labels in a fixed column; read-only values (Model, Weight, DMX Mode, Channels) start where field text starts; the long model name has an ellipsis (screenshots 18–20, 30).
  - Spans:
    - A drawn 4-click truss path placed one item: 10 sections, 4 legs, hub corners, 211 instances.
    - A 2-leg path rendered with the hub at the bend.
    - Clicking the start of a non-closable loop left 3 points and a red ghost; Backspace removed a point; Esc dropped the path.
    - Cable Sag 40 → one undo step; undo → 0, redo → 40; locked → refused "…is locked…".
    - Scene save, clear and load restored both paths (quantities 6 and 4) and every other item.
  - Render: the span ghost is absent from renders. Square 0.69 s, 4K Supersampled Cinematic 2.10 s, 1080p Temporal 0.43 s.
- **Not verified (needs Gevor; Standalone Game, mouse and keyboard):**
  - Drawing paths with the physical mouse, Enter/Backspace from the keyboard, double-clicking the last point, closing a real rectangle by clicking its start point.
  - Inspector by hand: tooltips on cut labels, the Gobo dropdown, typing into fields.
  - Patch sheet by hand: editing FID / universe / address, PATCH UNPATCHED, row clicks, Ctrl+Click.
  - Fader strips: short names and sideways scrolling with a mouse.
  - The native file dialog in a packaged Shipping build (compiled, never opened; Shipping cannot run from uncooked content).
  - Loading a scene file written before this change (no such file on this machine; the migration is reviewed, not run).
  - Strobe flashing on screen; gobo rotation and blade rotation visually.
- Status: **implemented, awaiting manual verification** of the items above.

**Commit:** `c3936d9`.

**Known issues / follow-ups**
- **Plugins disabled:** 19 from `b8448a5` plus UnrealNGGMCP (half-reverted, duplicate symbols). Enable any one in the `.uproject` once it compiles against StageCraft (`PlacementGizmo` needs its `EGizmoMode` renamed). Without UnrealNGGMCP the Claude ↔ editor MCP bridge is unavailable.
- **Workspace session:** Gevor's saved layout now contains a floating Patch window. My first run opened the panel before the backup was taken; close it or use Layout > Reset.
- **Render banding** remains in Off/FXAA renders with haze (fog depth slices); Temporal/Supersampled are clean. A possible fix is forcing temporal AA or a finer fog Z grid for haze renders, measured first.
- **Sag** hangs a run below its supports: a cable drawn on the floor sinks below it. Lift the run first.
- Photometrics assume a uniform cone; the beam angle is the full zoom angle (no separate field angle). The throw ignores a surface that moves under a fixture that did not move, until the next pan, tilt or move.
- Patch fields commit on Enter or focus loss, not while dragging.
- No undo for patch or attribute edits (unchanged behaviour).
- Fader centred ellipsis did not take effect in the strip; the short names avoid it.
- Rules.md mentions `RunEditor.bat` / `Scripts/Launch/RunEditor.ps1`, which do not exist in this project.
- **Obsidian vault:** `S:\` is still unreachable on this machine; the design record is ADR 0007 in the repo.

---

## #31 — Milestone audit: inspector, lighting, patch/faders, BOM; idle encoder tick removed, span totals, photometrics "No surface", fader name ellipsis, instanced material usage, Shipping unity fix (2026-10-19)

**Request (Gevor).** Under Rules.md, the next milestone:
1. Strict two-column inspector alignment for fixtures, props and spans.
2. Projected gobo and framing-shutter materials updated only on parameter changes, without Tick abuse; refined DMX patch attributes and fader integration.
3. A BOM panel with placed-item counts, combined weight and power (W).
4. Memory safety, encapsulation, modular subsystems, zero per-frame polling; STATE.md updated.

Most of this already existed: the BOM came in #27, and the inspector grid, gobo/iris/shutter projection, patch sheet and fader short names in #30. This entry is an **audit of those features against the request and Rules.md**, plus fixes for everything the audit found. No new subsystems or panels were added.

**Audit (code), no change needed**
- **Tick / polling.** Every Tick in the module was reviewed:
  - `UShowControlSubsystem` ticks only while a fade, DMX output or the strobe clock is active (`IsTickable`).
  - `AModularTransformGizmo` ticks only while attached.
  - `AStageCameraPawn` returns early at rest; it integrates fly input, which is its job.
  - `UStageCraftGameEngine::Tick` does first-frame window setup only.
  - `GetAllActorsOfClass` / `TActorIterator` appear only in development console commands, which are compiled out of Shipping.
  - The only defect was the encoder (below).
- **Gobo / iris / shutters.** `ALightingFixtureActor::ApplyBeamShaping` runs only from `ApplyAttributesToComponents`, which is driven by attribute writes (`NotifyAttributeChanged` path). It writes the one dynamic material instance per fixture; there is no per-frame update. The throw trace is cached until pan, tilt or the transform changes.
- **Patch ↔ faders.** The patch sheet and the fader bank both bind to `OnParameterChanged`, and all writes go through `RequestParameterChange`. Unchanged.
- **BOM.** `UStageBillOfMaterialsSubsystem` is a derived cache marked stale on `OnItemListChanged`. Verified at runtime (below); unchanged.

**What changed and why**
- **Encoder idle tick removed** (`UStageEncoderWidget`).
  - Before: `NativeTick` re-read the value and eased the needle every frame on every Pan/Tilt encoder, even when nothing changed. This broke the "no per-frame polling" rule.
  - Now: the easing and the popup linger run on a Slate active timer (`SWidget::RegisterActiveTimer`).
    - The timer starts when a value arrives (`ApplyValue`) or the popup opens, and stops itself once the needle has settled and the popup has expired.
    - The first value of a parameter snaps the needle.
    - The handle is a `TWeakPtr`: the Slate widget owns the timer, and the lambda is a weak UObject lambda.
  - The class carries `meta=(DisableNativeTick)`, so Blueprint subclasses do not tick either. Per `UUserWidget::UpdateCanTick`, code-built instances keep only the engine's empty base tick.
- **Span Info totals** (`AModularBaseActor`, `AStageSpanActor`).
  - Before: the inspector's Weight / Power showed the catalog spec per unit, so a 3-segment F34 run read "14.0 kg" while the BOM and session limits counted 42 kg.
  - Now: the rows show spec × `GetQuantity()`, and are readable through `ReadParameter`.
  - New protected `NotifyQuantityChanged()` refreshes them; spans call it from `NotifyShapeChanged`, so the row follows length, undo and redo live.
  - Single items are unchanged (quantity 1). The tag comments are updated.
- **Photometrics without a surface** (`ALightingFixtureActor`).
  - Before: a beam that hit nothing in range showed Throw "0.0 m", Illuminance "0.0 lx" and Beam Size "0.0 m", which reads as real values.
  - Now: these three read-only rows are text: "2.68 m", "9,186 lx", "1.95 m" (centimetre precision), or "No surface" when `FResult::bHasThrow` is false.
  - Beam Angle and Intensity stay numeric. Nothing else reads these tags as numbers; they are not stored in cues or scenes. The tag comments are updated.
- **Fader / encoder strip names** (`UStageFaderWidget`, `UStageEncoderWidget`). This closes the #30 known issue "Fader centred ellipsis did not take effect".
  - Root cause, from engine source: Slate draws no ellipsis on centre-justified single-line text. `SlateTextLayout.cpp` sets `NoOverflow` for Center, so a long name was clipped at both ends ("olarity Inve").
  - The default strip tree now uses a left-justified name in an `HAlign_Center` slot:
    - a name that fits looks centred;
    - a longer one is clamped to the strip by the slot (`LayoutUtils.h` `AlignChild`), so the ellipsis applies.
  - Audio "Polarity Invert" also gets the short name "Polarity".
- **Instanced material usage** (content).
  - `M_SpanMetal`, `M_SpanCoupler`, `M_SpanCable` and `M_PlacementGhost` were drawn on instanced static meshes without `bUsedWithInstancedStaticMeshes`. Every span run since #27 logged "missing usage flag InstancedStaticMeshes! Default Material will be used in game". A cooked build would have drawn spans with the default material.
  - The flag is now set and saved headless by the new idempotent `Scripts/Content/fix_instanced_material_usage.py`, which reads the flag back after saving.
- **Shipping build fix** (`StageFileDialog.cpp`, latent since #30).
  - `Microsoft/COMPointer.h` (`<Unknwn.h>`) and `<shobjidl.h>` included the raw `windows.h` without `NOMINMAX`. That leaked `min`/`max` macros into every later file in the unity blob.
  - Today's edits changed the adaptive-unity grouping, and Shipping failed in `HittestGrid.h`.
  - `Windows/WindowsHWrapper.h` is now included first.

**Not changed, by design (checked)**
- Read-only rows keep their units inline ("24.5 kg"), as designed in #28 (`StageParameterRowWidget.cpp`). Their values start at the field-text inset; the units column stays for editable rows.
- A span's read-only Scale row shows "(1, 1, 1) ×"; spans are sized by Length (#27).

**Files changed**
- Source (all under `Source/ModularSceneBuilder/`):
  - `Public/UI/StageEncoderWidget.h`, `Private/UI/StageEncoderWidget.cpp`
  - `Private/UI/StageFaderWidget.cpp`
  - `Public/Actors/ModularBaseActor.h`, `Private/Actors/ModularBaseActor.cpp`
  - `Private/Actors/StageSpanActor.cpp`
  - `Private/Actors/LightingFixtureActor.cpp`
  - `Private/Actors/AudioEquipmentActor.cpp`
  - `Private/Data/StageParameterTypes.cpp` (tag comments)
  - `Private/Import/StageFileDialog.cpp`
- Content (modified): `Content/StageCraft/Spans/M_SpanMetal.uasset`, `M_SpanCoupler.uasset`, `M_SpanCable.uasset`, `Content/StageCraft/Placement/M_PlacementGhost.uasset`.
- Scripts (new): `Scripts/Content/fix_instanced_material_usage.py`.
- Docs: `STATE.md` (#29 / #30 commit hash `c3936d9`, this entry), `tasks/todo.md`.
- No maps, input assets, Widget Blueprints or config were changed.

**Verification (Claude, 2026-10-19; logs in `Saved/Logs/R31/`)**
- **Builds (UE 5.8), all with 0 errors and 0 warnings:**
  - Editor Development and Game Development succeeded.
  - Game Shipping succeeded on its final run, with `-DisableAdaptiveUnity`, so the full unity blob that had failed was compiled. The first Shipping run failed as described above (`Build_Game_Shipping.log`, `_2.log`); `_3.log` succeeded.
- **Automation (headless), StageCraft filter: 37/37 passed**, `TEST COMPLETE. EXIT CODE: 0` (`R31_Tests.log`).
- **Content script:** all 4 materials report "usage flag set, saved", then `used_with_instanced_static_meshes = True` on read-back (`Content_InstancedUsage.log`).
- **Standalone `-game`, scripted by Claude** (`L_StageTest`, `R31_Run1`–`Run4.log`, screenshots `Saved/Screenshots/WindowsEditor/ScreenShot00031`–`00047`; Gevor's workspace session file backed up and restored):
  - **Inspector grid on every item type:** prop (Test Crate), span (F34 run), audio (Main L 01), truss and fixture (Spot 101, scrolled through all sections).
    - Labels sit in the 112 px column.
    - Field outlines, checkboxes and dropdowns start at the same x, and read-only values start at the field-text inset.
    - Editable units sit in the units column; long values end in an ellipsis.
  - **Encoder:** `Show.Set Pan 180` → the needle reached 180° and the readout said 180.0° (screenshots 38–39); the timer path runs and settles.
  - **Span totals:** Weight 42.0 kg (6 m, 3 segments) → `Span.SetLength 1000` → 70.0 kg live → undo → 42.0 kg (screenshots 41–43).
  - **Audio faders:** the strip reads "Polarity"; Gain, Mute, Delay and Splay stay centred (screenshot 44).
  - **Photometrics:** with the beam up, the rows read "No surface" (log "throw none"). At Tilt 120: Throw 2.68 m, Illuminance 9,186 lx, Beam Size 1.95 m, 66.0 kcd, matching `Show.Photometrics` (screenshots 45–46).
  - **BOM:** 6 items, 8 units, 156.0 kg, 940 W. The table by discipline matches `Report.Status`: truss 4 units / 49 kg, lighting 2 / 49 kg / 940 W, audio 1 / 58 kg, prop 1 (screenshot 40).
  - **Materials:** Run4 placed a cable span (6 units) and a truss span (3 units) with their ghosts: 0 "missing usage flag" warnings (each earlier span run had 3–4).
- **Not verified (needs Gevor, by mouse and keyboard):**
  - Encoder drag / wheel / double-click feel after the timer change, and the popup linger after a wheel step.
  - A fader name long enough to need the ellipsis (all current names fit, so the ellipsis path is proven from engine source, not on screen).
  - A packaged (cooked) build showing span materials. The usage flag is saved, but nothing was cooked.
  - The #27 / #30 manual checklists still stand.
- Status: **implemented, awaiting manual verification** of the items above.

**Commit:** `efb3605`.

**Known issues / follow-ups**
- The encoder's timer starts on value pushes. A Blueprint subclass that paints anything else per frame would need its own trigger (none exist).
- Inspector Weight / Power are totals; the per-unit spec stays visible in the Library and the BOM ("3 x 14.00 kg").
- BOM still has no current (A) or per-phase power distribution, and no rigging point loads (from #27).
- Patch / attribute edits are still not undo steps (from #30).
- **Obsidian vault** `S:\` is still unreachable on this machine, so no session log or lessons were written there. Rules.md's `RunEditor.bat` does not exist in this project.

---

## #32 — Viewport toolbar: orthographic projections, Lit / Unlit / Lines view modes, reference grid, global grid and angle snap (2026-10-19)

**Request (Gevor).** Under Rules.md:
1. A viewport projection selector: Perspective plus orthographic Top (XY), Front (XZ), Left/Right (YZ) and Back, with orthographic zoom.
2. View modes: Lit, Unlit, and a wireframe/line view for CAD alignment. The grid must read clearly in the orthographic and unlit modes.
3. A viewport header toolbar with projection, view mode, Move/Rotate/Scale, grid snap (10/50/100 cm) and angle snap (5°/15°/45°).
4. Event-driven updates, no per-frame polling.

**Decisions (Gevor, 2026-10-19), asked because the engine forces them:**
- **"Wireframe" is a CAD line view** (post-process outlines). The engine's `Wireframe` show flag is a compile-time 0 in Shipping, so true wireframe cannot ship.
- **The toolbar grid is global.** It replaces the per-item placement grids and drives placement, gizmo moves and the drawn grid.

Design record, with verified engine facts F1–F11: `Docs/ADR/0008-viewport-toolbar-projections-and-view-modes.md`.

**What changed and why**
- **State and ownership** (Rules.md "Where state lives").
  - `UStageViewportComponent` (new, on `AModularPlayerController`) owns projection, view mode, grid visibility and ortho width.
  - It applies them through their owners:
    - the camera manager: `bIsOrthographic`, `DefaultOrthoWidth`, explicit planes, pitch limits;
    - `AStageCameraPawn`'s new fixed view;
    - `UStageCraftGameViewportClient::ApplyStageViewMode`;
    - `UStageLineViewCameraModifier`;
    - `AStageGridActor`.
  - Grid and angle snap live in `UStageSnappingComponent` (new `GetGridSnap/SetGridSnap/GetAngleSnap/SetAngleSnap`, `OnSnapSettingsChanged`). They are stored in `UStageCraftUserSettings` as presets only; a hand-edited ini falls back to the defaults.
  - Projection and view mode are not saved: every session starts Lit Perspective.
- **Projections.**
  - Unreal Editor orientations (F8).
  - Entering ortho centres on where the perspective view meets the floor, at the same scale (`FitOrthoWidth`), and remembers the camera. Perspective restores it exactly.
  - Ortho → ortho keeps centre and zoom. A front or side view entered from above lifts the centre so the floor is on screen.
  - The ortho camera stands 200 m back with explicit planes.
  - The pawn's fixed view holds the exact axis rotation (look ignored, WASD pans in the view plane). The camera manager's pitch limit is widened to ±90 while ortho (F10), otherwise Top was 0.1° off vertical.
  - **Ortho navigation:** right-drag grab-pans; the wheel zooms around the cursor (1–500 m). Both use the pixel size measured by deprojection, because the engine maps ortho width to the viewport height by default (F9). The first implementation assumed width and missed the zoom anchor by 149 cm; it now holds within a pixel.
  - Gizmo and snap-guide screen sizes use the ortho-equivalent distance (`StageViewMath::GetScreenScaleDistance`).
- **View modes.**
  - `ApplyStageViewMode` sets `ViewModeIndex` and the show flags itself: `SetViewMode` forces Lit in Shipping and cooked builds (F1). Every mode starts from the viewport's original flags.
  - **Unlit** = `VMI_Unlit`. It needs development shaders in cooked builds; `r.CompileShadersForDevelopment=1` is pinned in `DefaultEngine.ini` (F4).
  - **Lines** = Unlit flags, with post-processing back on and base colour, TAA, eye adaptation, bloom and motion blur off. `M_StageLineView` is a post-process material after tonemapping:
    - depth edges from the Laplacian of an affine depth (linear in ortho, inverse in perspective, so grazing floors are not edges), nearer side only;
    - creases from GBuffer normals;
    - a flat dark background, with scene colour (grid, emissive) added back.

    It is delivered by a camera modifier enabled only in Lines, so it never reaches renders (F7).
  - **Every projection or view-mode switch is a camera cut** (F11). Without it, Lit came back dark after Lines and took about 10 s to recover (floor brightness measured 22 → 44 → 119 → 144); with it, 144 at once.
- **Grid** (`AStageGridActor`).
  - 1-pixel batched lines with the batch's tick switched off; rebuilt only when plane, offset or cell changes.
  - The floor plane (1 cm above Z=0; at 0 it z-fought with the floor and drew dashed) in Perspective and Top. Front/Back and Left/Right use a vertical plane 50 m behind the origin.
  - Minor lines at the grid snap (100 cm when Off), major every 10 cells, axis lines red/green/blue, ±50 m (at most 1 001 lines per direction). Hidden from renders.
  - Toggle on the toolbar, saved per machine.
- **Global grid and angle snap.**
  - `StagePlacementMath::WithGlobalGrid` replaces the item's X/Y grid (Z, offset and alignment kept). The placement tool and the span tool (which reads the placement tool's value) use it.
  - Gizmo moves now snap the **absolute** world coordinate on the dragged axis (CAD alignment); before, they snapped the drag distance. Object snapping still wins.
  - Gizmo rotation uses the angle snap. The new `AModularTransformGizmo::SetDragSnapping` replaces direct writes to its protected snap members.
  - The controller pushes the values on `OnSnapSettingsChanged`.
  - Defaults: 50 cm (the old truss/span grid) and 15°.
- **Toolbar** (`UStageViewportToolbar`, code-built UMG).
  - Placement: in the viewport tab above the 3D view, through the new shell callback `CreateViewportToolbarContent`, like the status bar. It moves with the viewport when floated and is rebuilt for a new controller.
  - Controls: projection dropdown, LIT / UNLIT / LINES, GRID toggle, MOVE / ROTATE / SCALE, GRID and ANGLE snap dropdowns.
  - It writes through the owners' setters, reads back after every write, and redraws from `OnViewChanged`, `OnSnapSettingsChanged` and `OnModeChanged`. Bound in NativeConstruct, unbound in NativeDestruct; no tick; non-focusable buttons.
- **Renders.**
  - The capture follows the projection, so an orthographic Top renders as a plan.
  - FOV and ortho width are converted for the output aspect (F9), so renders frame like the viewport.
  - **Pre-existing defect found and fixed:** since #26, perspective renders were cropped about 1.33× against the viewport. The viewport keeps the vertical angle of a FOV defined at the camera manager's default 4:3 aspect; a 16:9 capture read the FOV horizontally. Measured after the fix: the same features sit at the same frame-height fractions in viewport and render (spots ±0.157 vs ±0.157, floor edge −0.280 vs −0.281).
- **Dev console** (non-Shipping): `StageCraft.View.{Status, Projection, Mode, Grid, Zoom, Pan}`, `StageCraft.Snap.{Grid, Angle}`. Zoom logs the deprojected anchor before and after.

**Files changed**
- New source (`Source/ModularSceneBuilder/`):
  - `Public/Viewport/StageViewportTypes.h`, `StageViewMath.h`, `StageViewportComponent.h`, `StageGridActor.h`, `StageLineViewCameraModifier.h`
  - `Private/Viewport/StageViewMath.cpp`, `StageViewportComponent.cpp`, `StageGridActor.cpp`, `StageLineViewCameraModifier.cpp`, `StageViewportConsoleCommands.cpp`, `Tests/StageViewportTests.cpp`
  - `Public/UI/StageViewportToolbar.h`, `Private/UI/StageViewportToolbar.cpp`
- Modified source:
  - `Player/ModularPlayerController.h/.cpp` (component, snap push, ortho look/wheel routing, render aspect flag)
  - `Player/StageCameraPawn.h/.cpp` (fixed view, `TeleportView`)
  - `Player/StageCraftGameViewportClient.h/.cpp` (`ApplyStageViewMode`)
  - `Actors/ModularTransformGizmo.h/.cpp` (`SetDragSnapping`, absolute grid moves, ortho screen size)
  - `Placement/StageSnappingComponent.h/.cpp`, `Placement/StagePlacementToolComponent.h/.cpp`, `Placement/StagePlacementMath.h/.cpp`, `Private/Placement/StageSpanToolComponent.cpp`
  - `Settings/StageCraftUserSettings.h/.cpp`
  - `Render/StageRenderSubsystem.h/.cpp`, `Render/StageRenderTypes.h/.cpp`, `Private/Render/Tests/StageRenderTests.cpp`
  - `Private/Workspace/StageWorkspaceShell.h/.cpp`, `Workspace/StageWorkspaceSubsystem.h/.cpp`
- Config:
  - `Config/DefaultGame.ini`: `StageViewportComponent.LineViewMaterial`, and cook `/Game/StageCraft/Viewport`.
  - `Config/DefaultEngine.ini`: `[SystemSettings] r.CompileShadersForDevelopment=1`.
- Content (new): `Content/StageCraft/Viewport/M_StageLineView.uasset`.
- Scripts (new): `Scripts/Content/create_viewport_content.py`.
- Docs: `Docs/ADR/0008-viewport-toolbar-projections-and-view-modes.md` (new), `tasks/todo.md`, `STATE.md` (#31 commit `efb3605`, this entry).
- No maps, input assets or Widget Blueprints were changed. Catalog data is unchanged (per-item `GridSize` X/Y is now overridden at placement).

**Verification (Claude, 2026-10-19; logs in `Saved/Logs/R32/`)**
- **Builds, all with 0 errors and 0 warnings:**
  - Editor Development.
  - Game Development.
  - Game Shipping with `-DisableAdaptiveUnity`. The Shipping exe contains the toolbar strings and none of the `StageCraft.View.*` / `StageCraft.Snap.*` command names (UTF-16 search).
- **Automation: 42/42 passed** (37 existing + 5 new), `TEST COMPLETE. EXIT CODE: 0`:
  - `Viewport.Projection`: axes of every view, grid planes and offsets.
  - `Viewport.OrthoMath`: fit, steps and limits, zoom anchor at four cursor offsets, pan, screen-scale distance, NaN.
  - `Viewport.Grid`: counts, axes, majors, vertical planes, line cap, invalid input.
  - `Viewport.Snap`: presets, sanitising, global grid rules and landing.
  - `Render.Framing`: FOV and ortho conversion.
- **Content:** `M_StageLineView` saved and read back as `MD_POST_PROCESS` / `BL_SCENE_COLOR_AFTER_TONEMAPPING`. It compiled and rendered at runtime.
- **Standalone `-game`, scripted by Claude** (`L_StageTest`, `R32_Run1`–`Run7.log`, screenshots `ScreenShot00048`–`00079`, renders `Saved/Renders/*20261019-23*`; Gevor's workspace session and `GameUserSettings.ini` backed up and restored):
  - **Toolbar** drawn above the viewport in the dark theme. Its selection follows console changes (read-back).
  - **Projections:**
    - Top / Front / Left / Back work.
    - Top pitch is exactly −90.00 after the F10 fix.
    - Perspective is restored exactly (−800, 0, 600, P −35).
    - **Zoom anchor:** after the F9 fix the point under the cursor moves 1.2 cm and 1.8 cm (under one pixel at 2–4 cm per pixel). It was 149 cm before.
    - **Pan:** 100 px right / 50 px up moves the camera −454 / −227 cm at a measured 4.54 cm/px (1:1 grab).
  - **View modes:**
    - Unlit draws flat base colours.
    - Lines draws clean outlines in perspective, Top and Left, with the grid.
    - Lit after Lines is at full exposure at once (camera cut).
  - **Grid:** 402 lines at 50 cm, 2 002 at 10 cm, hidden/shown by toggle. No z-fighting after the 1 cm lift. A vertical grid in Front/Left.
  - **Snaps:**
    - 25 cm and 7° are refused as non-presets.
    - Gizmo and placement grids follow the setting (status log).
    - A gizmo drag of +137 cm from X −450 landed on −300 (50 cm grid).
  - **Renders:** an ortho Top render is a plan framed like the viewport (spots ±0.144 vs ±0.1435 of height). The perspective render matches the viewport after the framing fix. Gizmo and grid are absent from renders.
- **Not verified (needs Gevor, mouse and keyboard, Standalone Game):**
  - Clicking every toolbar control, including the dropdown lists.
  - A physical right-drag pan, the wheel zoom and WASD in ortho (the same component calls were driven from the console).
  - Placing items in ortho Top with the mouse.
  - Rotate snapping by hand.
  - A cooked or packaged build: Unlit depends on development shaders being cooked (pinned), and the Lines material must cook (directory added). Nothing was cooked.
  - PIE: it has no workspace, so no toolbar; the console commands work there.
- Status: **implemented, awaiting manual verification** of the items above.

**Commit:** `d1fdf79`.

**Known issues / follow-ups**
- **Behaviour changes to be aware of:**
  - Per-item placement grids are replaced by the global grid (Gevor's choice).
  - Gizmo moves now land on absolute grid positions; an off-grid item jumps onto the grid at its first snapped move.
  - Angle snap now defaults to 15° (rotation was free).
  - Perspective renders are framed wider than before (now matching the viewport).
- The grid does not fade with distance in perspective; far lines crowd near the horizon. Batched lines cannot fade.
- Lit ortho uses the same lighting as perspective. Lumen in orthographic views is the engine's, so it may differ slightly.
- Lines mode outlines come from depth and normals. Very thin parts below a pixel and coplanar touching faces draw no line.
- The ortho camera always stands 200 m back. Layouts larger than that behind the view centre would be clipped (none exist).
- No keyboard shortcuts for projections or view modes yet.
- **Obsidian vault** `S:\` is still unreachable; the design record is ADR 0008.

## #33 — Viewport toolbar: grid and angle snap as one-click button segments beside the view modes (2026-10-20)

**What and why**
- **Request (Gevor):** the finalized design has one flat horizontal toolbar in the viewport header, with no dropdowns for primary view modes:
  - LIT / UNLIT / LINES / GRID as single-click buttons;
  - grid snap (10 / 50 / 100 cm) and angle snap directly beside them.
- **Already in place from #32:** LIT / UNLIT / LINES and GRID were already single-click buttons.
- **What changed:**
  - Grid snap and angle snap were themed dropdowns placed after the Move / Rotate / Scale segment. They are now button segments, generated from `StageViewMath::GetGridSnapPresets()` / `GetAngleSnapPresets()` so labels and presets can never disagree: `SNAP  OFF 10 cm 50 cm 100 cm` and `ANGLE  OFF 5° 15° 45°`.
  - Order is now: Projection ▾ | LIT UNLIT LINES · GRID | SNAP … | ANGLE … | MOVE ROTATE SCALE.
  - The snap label is "SNAP" instead of "GRID", so it no longer reads like the grid toggle next to it.
  - The projection dropdown stays: it is a camera selector with six options, not a view mode.
- **Binding is unchanged:**
  - Buttons write through `UStageSnappingComponent::SetGridSnap` / `SetAngleSnap`, then read back.
  - Highlights redraw only from `OnSnapSettingsChanged` / `OnViewChanged` / `OnModeChanged`. No tick and no polling.
  - The two combo `UFUNCTION` handlers and their `OnSelectionChanged` bindings were removed. Snap buttons are `UStageChoiceButton` (never keyboard-focusable) and are held in `UPROPERTY(Transient) TArray<TObjectPtr<UStageChoiceButton>>`.

**Files changed**
- `Source/ModularSceneBuilder/Public/UI/StageViewportToolbar.h`
- `Source/ModularSceneBuilder/Private/UI/StageViewportToolbar.cpp`
- `Docs/ADR/0008-viewport-toolbar-projections-and-view-modes.md` (§4.6 controls and layout)
- `tasks/todo.md`, `STATE.md` (this entry; #32 commit hash `d1fdf79` filled in)
- No Widget Blueprints, maps, input assets or content.

**Verification (Claude, 2026-10-20)**
- Editor Development build: succeeded, 0 errors, 0 warnings. It was built with `-NoHotReloadFromIDE`, because the Kmidden editor was open.
- Automation `StageCraft`: all passed, `TEST COMPLETE. EXIT CODE: 0`.
- Standalone `-game` run scripted by Claude (`Saved/Screenshots/WindowsEditor/ScreenShot00001.png`):
  - the toolbar is drawn as one row in the designed order;
  - the stored settings (50 cm, 15°) show highlighted on launch, so read-back from user settings works.
- **Not verified:** clicking each snap button with the mouse (Gevor, Standalone Game). The write path is the same setter the dropdowns used, which #32 verified.
- Status: **implemented, awaiting manual verification** of the click behaviour.

**Commit:** `40a0459`.

**Known issues / follow-ups**
- The row is about 890 px wide (measured in the screenshot). In a narrower viewport tab the right end (gizmo segment) is clipped; there is no overflow menu yet.
- The #32 "Not verified" list still applies.

## #34 — Viewport toolbar: Unreal-style View menu (six view modes + grid), toggling SNAP / ANGLE segments (2026-10-20)

**What and why**
- **Request (Gevor):**
  1. SNAP and ANGLE buttons should toggle instead of sticking.
  2. The flat LIT / UNLIT / LINES / GRID buttons should become one Unreal-style dropdown, with Lit, Unlit, Wireframe / CAD Lines, Lighting Only, Detail Lighting and Reflections. Picking an entry applies it and closes the menu.
- **Snap toggle, root cause:** a snap button was a plain radio choice. Clicking the active step re-set the same value, so nothing changed, and clicking OFF while off did nothing. The buttons looked stuck.
- **Snap toggle, new rule:** `StageViewMath::ResolveSnapPick`, which is pure and tested:
  - clicking the active step turns snapping off;
  - clicking OFF while on turns it off;
  - clicking OFF while off restores the last step used this session, or the default (50 cm / 15°) when there is none;
  - any other click selects that step.
- **Snap toggle, where it lives:**
  - `UStageSnappingComponent::PickGridSnap` / `PickAngleSnap` apply the rule through the existing validating setters.
  - The component remembers the last non-zero step (`LastGridSnapOn` / `LastAngleSnapOn`, transient) inside `SetGridSnap` / `SetAngleSnap`, so console changes count too.
  - The toolbar only calls `Pick*` and reads back.
- **View menu:**
  - `UStageMenuButton` (new, `Private/UI`) is a never-focusable UMG wrapper around `SComboButton`. It is styled like the projection dropdown, and its owner builds the popup on every open.
  - `UStageViewportToolbar::BuildViewMenu` builds that popup with `FMenuBuilder`, the same menu system and look as the menu bar. It has a View Mode section (radio entries) and Show > Grid (a toggle).
  - The menu reflects the state at the moment it opens and closes on a pick, so it holds no bindings. Its lambdas hold the viewport through `TWeakObjectPtr`.
  - The button label reads back on `OnViewChanged`: Lit, Unlit, CAD Lines, Lighting Only, Detail Lighting, Reflections.
- **New view modes:** `EStageViewMode` gains `LightingOnly`, `DetailLighting` and `Reflections`, applied by `UStageCraftGameViewportClient::ApplyStageViewMode` as `VMI_LightingOnly`, `VMI_Lit_DetailLighting` and `VMI_ReflectionOverride`.
  - **Shipping:** their show flags are `SHOWFLAG_FIXED_IN_SHIPPING(0)` (engine `ShowFlagsValues.inl`), so in Shipping they would silently render Lit.
  - Gevor chose to show them disabled. `StageViewMath::IsViewModeAvailable` returns false when `UE_BUILD_OPTIMIZED_SHOWFLAGS`. The menu greys them out with a "Not available in this build" tooltip, and `UStageViewportComponent::SetViewMode` refuses them with a warning.
- "Wireframe / CAD Lines" is the existing Lines post-process mode. Engine wireframe is impossible in Shipping (ADR 0008 F2).
- **Removed:** the view-mode and grid-toggle button segments, with their handlers and members.

**Files changed**
- New: `Source/ModularSceneBuilder/Private/UI/StageMenuButton.h`, `Source/ModularSceneBuilder/Private/UI/StageMenuButton.cpp`
- `Source/ModularSceneBuilder/Public/UI/StageViewportToolbar.h`, `Source/ModularSceneBuilder/Private/UI/StageViewportToolbar.cpp`
- `Source/ModularSceneBuilder/Public/Viewport/StageViewportTypes.h` (enum), `Source/ModularSceneBuilder/Public/Viewport/StageViewMath.h`, `Source/ModularSceneBuilder/Private/Viewport/StageViewMath.cpp`
- `Source/ModularSceneBuilder/Public/Viewport/StageViewportComponent.h`, `Source/ModularSceneBuilder/Private/Viewport/StageViewportComponent.cpp`
- `Source/ModularSceneBuilder/Private/Player/StageCraftGameViewportClient.cpp`
- `Source/ModularSceneBuilder/Public/Placement/StageSnappingComponent.h`, `Source/ModularSceneBuilder/Private/Placement/StageSnappingComponent.cpp`
- `Source/ModularSceneBuilder/Private/Viewport/StageViewportConsoleCommands.cpp` (usage text)
- `Source/ModularSceneBuilder/Private/Viewport/Tests/StageViewportTests.cpp`
- `Docs/ADR/0008-viewport-toolbar-projections-and-view-modes.md` (§4.6), `tasks/todo.md`, `STATE.md` (this entry; #33 commit hash `40a0459`)
- No Widget Blueprints, maps, input assets or content.

**Verification (Claude, 2026-10-20)**
- Editor Development build: succeeded, 0 errors, 0 warnings.
- Game Shipping build: succeeded, 0 errors, 0 warnings. Confirms the `UE_BUILD_OPTIMIZED_SHOWFLAGS` path compiles.
- Automation `StageCraft`: 42/42 passed, `TEST COMPLETE. EXIT CODE: 0`. `Viewport.Snap` now covers:
  - every `ResolveSnapPick` case, including NaN and a four-click on/off/on/off sequence;
  - `IsViewModeAvailable` for all six modes against the build's show-flag setting.
- **Standalone `-game` runs scripted by Claude** (`ScreenShot00002`–`00005`):
  - `StageCraft.View.Mode LightingOnly`, `DetailLighting` and `Reflections` each switched (`ok`). Reflections renders mirror surfaces, and the View button label follows each mode.
  - The second run had no unrequested mode changes. In the first run, Unlit and CAD Lines were picked from the View menu during the run, most likely by hand. That exercised the menu's write path and the label read-back (screenshots `00003` Unlit and `00004` CAD Lines).
  - On exit the mode is restored to Lit.
- **Not verified:**
  - Opening the menu, picking an entry and closing it by mouse (Gevor).
  - SNAP / ANGLE toggling by mouse (Gevor).
  - Greyed-out entries in a packaged Shipping build.
- Status: **implemented, awaiting manual verification** of the items above.

**Commit:** `33c907d`.

**Known issues / follow-ups**
- The grid toggle is now two clicks (View > Grid), as requested. There is no keyboard shortcut for it or for the view modes yet.
- Lighting Only, Detail Lighting and Reflections are diagnostics: Development and editor builds only.
- The last-used snap step is remembered per session, not saved.

---

## #35 — Viewport toolbar sections with dividers; outdoor environment (Sky Creator): sky, time of day, sun path, clouds, rain, snow, fog, Environment panel (2026-10-20)

**What and why**
- **Request (Gevor):**
  1. Split the viewport toolbar into five sections with subtle vertical dividers and uniform padding: projection | view modes | grid snap | angle snap | gizmo.
  2. Integrate the available weather plugin for outdoor stages: sun angle, time of day, cloud density, rain and snow particles and atmospheric fog, edited in a dedicated Environment panel, with no per-frame polling.
  3. Rules.md standards, then update STATE.md.
- Design record: `Docs/ADR/0009-outdoor-environment.md`.
- **Toolbar sections:**
  - `BuildTree` adds a divider between every pair of sections, with `SectionGap` (8 px) on both sides. Captions (SNAP, ANGLE) sit 4 px from their buttons.
  - **Root cause of invisible dividers:** the dividers never drew, here or before. `UImage::SetDesiredSizeOverride` is dropped when called before the Slate widget exists (engine `Image.cpp` 122-127), and code-built trees are made in `NativeOnInitialized`. The divider had 0 px width (measured: a 16 px gap = 8 + 8).
  - New `StageCraftWidgetStyle::MakeLine` stores the size in the brush instead. The divider is 1 × 14 px at half-strength `TextDisabled`, because the `Divider` colour vanished at 1 px.
- **Which weather plugin:**
  - SkyWeatherControls only drives Ultra Dynamic Sky, which is not in the project, so it stays disabled.
  - Sky Creator (`Plugins/SkyCreat`) is a complete sky actor: sun, moon, atmosphere, volumetric clouds, sky light, height fog, Niagara rain and snow. It is enabled in the `.uproject` (it was disabled in #30). Its source compiles on 5.8 unchanged.
- **Sky Creator descriptor:**
  - Its descriptor said `"Installed": true, "EngineVersion": "5.7.0"`, so 5.8 skipped mounting its content ("Skipping load"). Its module still loaded through our link, and the sky actor's constructor then failed to find every asset.
  - `EngineVersion` is now `5.8.0`. This is the only change to the plugin.
- **Architecture (ADR 0009 §3):**
  - **Core module:**
    - `FStageEnvironmentSettings` plus pure `StageEnvironment::Sanitize / ApplyWeatherPreset / FindWeatherPreset`.
    - The abstract `UStageEnvironmentBackend`.
    - `UStageEnvironmentSubsystem`, a world subsystem (Game + PIE) that is the source of truth. `SetSettings` is the single write path: sanitize, refuse sky-on without a backend, skip if unchanged, store, apply, then broadcast `OnEnvironmentChanged`.
    - The backend class comes from `DefaultGame.ini`, the same pattern as DMX: the core never links to the marketplace plugin.
  - **New game module `StageCraftSkyCreator`:** `UStageSkyCreatorBackend` and `StageSkyCreator::MakeWeather`.
    - Sky on: spawns one transient `ASkyCreator` and hides the level's directional and sky lights, atmosphere, clouds and fog, in one pass when the sky turns on.
    - Each change sets the sun path, the time and the mapped weather.
    - Sky off: destroys the actor and restores the level.
    - At world teardown it leaves cleanup to the world.
  - **No tick in StageCraft code.** The sky actor ticks itself (clouds, captures, particles) and exists only while the sky is on.
- **Environment panel** (`StageCraft.Panel.Environment`, a tab after Render in the Default layout):
  - OUTDOOR SKY Off / On.
  - WEATHER presets: Clear, Cloudy, Rain, Snow, Fog.
  - SUN sliders: time of day as HH:MM, sun height 5–90°, sun direction 0–359°.
  - CONDITIONS sliders: clouds, rain, snow, fog (%).
  - Sliders write on every move and the panel redraws from the delegate only.
  - Slider tops stop at 23:59 / 359°, so the wrap never makes the handle jump.
  - Without a backend all controls are disabled and the panel says why. With the sky off, only Off / On is live.
  - `UStageValueSlider` is a never-focusable `USlider` that reports its index.
- **Fog scale:** the first maximum (0.08) was invisible at stage distance, because the engine applies density / 1000 per cm. The maximum is now 1.0 × Fog², from screenshots: 85 % washes out items 25 m away, 30 % is a light mist.
- **Packaging:** Sky Creator's Curves, Effects, Materials, Meshes, Textures and WeatherPresets folders go into `DirectoriesToAlwaysCook`, because the actor loads them by path.
- **Dev console commands** (not in Shipping): `StageCraft.Environment.Status`, `StageCraft.Environment.Set <field> <value>`, `StageCraft.Environment.Preset <name>`.

**Files changed**
- New, core:
  - `Source/ModularSceneBuilder/Public/Environment/StageEnvironmentTypes.h`, `Private/Environment/StageEnvironmentTypes.cpp`
  - `Public/Environment/StageEnvironmentBackend.h`
  - `Public/Environment/StageEnvironmentSubsystem.h`, `Private/Environment/StageEnvironmentSubsystem.cpp`
  - `Private/Environment/StageEnvironmentConsoleCommands.cpp`
  - `Private/Environment/Tests/StageEnvironmentTests.cpp`
  - `Public/UI/StageEnvironmentPanel.h`, `Private/UI/StageEnvironmentPanel.cpp`
  - `Public/UI/StageValueSlider.h`, `Private/UI/StageValueSlider.cpp`
- New module: `Source/StageCraftSkyCreator/StageCraftSkyCreator.Build.cs`, plus these in `Private/`:
  - `StageCraftSkyCreatorModule.cpp`
  - `StageSkyCreatorBackend.h`, `StageSkyCreatorBackend.cpp`
  - `StageSkyCreatorWeather.h`, `StageSkyCreatorWeather.cpp`
  - `Tests/StageSkyCreatorTests.cpp`
- Changed:
  - `Source/ModularSceneBuilder/Private/UI/StageViewportToolbar.cpp`, `Public/UI/StageViewportToolbar.h` (sections, dividers)
  - `Private/UI/StageCraftWidgetStyle.h/.cpp` (`MakeLine`, `MakeSlider`)
  - `Public/Workspace/StageWorkspaceTypes.h`, `Private/Workspace/StageWorkspaceTypes.cpp` (`Panel_Environment` tag)
  - `Private/Workspace/StageWorkspaceShell.cpp` (Default layout tab)
  - `Source/ModularSceneBuilder.Target.cs`, `Source/ModularSceneBuilderEditor.Target.cs` (`StageCraftSkyCreator`)
  - `ModularSceneBuilder.uproject` (module entry; `SkyCreatorPlugin` enabled)
  - `Config/DefaultGame.ini` (`BackendClass`, cook directories)
  - `Plugins/SkyCreat/SkyCreatorPlugin.uplugin` (`EngineVersion` 5.8.0)
- Content (new): `Content/StageCraft/UI/Panels/DA_Panel_Environment.uasset`, made headless by the new `Scripts/Content/create_environment_panel_content.py`. Its tag reads back as `StageCraft.Panel.Environment`.
- Docs: `Docs/ADR/0009-outdoor-environment.md` (new); `Docs/ADR/0008-viewport-toolbar-projections-and-view-modes.md` (§4.6 history); `tasks/todo.md`; `STATE.md` (this entry; #34 commit hash `33c907d`).
- No maps, input assets or Widget Blueprints.
- **Not in the repo:** Gevor's `Saved/StageCraft/Workspace/Session.json`.
  - A first scripted run opened the Environment panel as a floating window on his second monitor. Claude moved the tab into the right-hand stack after Render and removed the floating window.
  - Nothing else in his session layout changed.

**Verification (Claude, 2026-10-20)**
- Editor Development build: succeeded, 0 errors, 0 warnings. The C4305 warnings from Sky Creator's header are silenced in our includes only.
- Game Shipping build (`-DisableAdaptiveUnity`): succeeded, 0 errors, 0 warnings, with the new module, Sky Creator, and the console commands compiled out.
- Automation `StageCraft`: 46/46 passed (42 existing + 4 new), `TEST COMPLETE. EXIT CODE: 0`, on the final code.
  - `Environment.Sanitize`: wrap, clamp, NaN / infinity → defaults, idempotence.
  - `Environment.Presets`: each preset round-trips, keeps time and sun, stays in range; Custom.
  - `Environment.SkyCreatorMapping`: cloud layers, stratus threshold, rain and wetness, snow, fog curve, Sky Creator's own tuning preserved.
  - `Environment.SkyCreatorWorld`, a real game world with a level sun and fog:
    - sky on spawns exactly one `ASkyCreator` and hides the level sun and fog;
    - time and rain reach the actor;
    - further changes reuse it, and the Snow preset reaches it;
    - sky off removes it and restores the sun and fog, and on again spawns one;
    - teardown is clean, with no world warnings.
- **Standalone `-game` runs scripted by Claude** (`ScreenShot00013`–`00022`):
  - Panel off: indoor night look, controls disabled.
  - On: 14:00 daylight, the level's 3 sky components hidden.
  - Rain preset: overcast, rain streaks. 19:12: warm low sun.
  - Snow: falling snow. Fog 85 % / 30 %: heavy fog / light mist.
  - Off: indoor look restored.
  - The panel's preset buttons and sliders follow every change.
  - The toolbar crop shows the four dividers with equal gaps.
- **First scripted run:** the stored time of day changed between commands (19.2 → 7.56 → 21.03 h) while the panel was floating on the second monitor. The second, hands-off run with a status line before every change showed no drift. Sliders only commit under mouse capture, keyboard focus or touch, and these are never focusable. So the first run's changes were mouse drags on the floating panel.
- **One later run showed the on-screen message "Video memory has been exhausted (737 MB over budget)".** The Kmidden editor was open at the same time (16 GB RAM). Earlier runs did not show it. Not measured further.
- **Not verified:**
  - Dragging the sliders and clicking the presets by mouse (Gevor).
  - A packaged build: cook rules bring every Sky Creator asset; Shipping performance.
- Status: **implemented, awaiting manual verification** of the items above.

**Commit:** `4f310c2` (hash set by #36).

**Known issues / follow-ups**
- Environment settings are not saved in scene files yet (scene format version 2).
- The sky's GPU time and VRAM are not measured (`stat GPU`, `stat RHI`).
- The 1 px header lines in the Render, Outliner, Library, Bill of Materials and Patch panels and the status bar are invisible for the same reason the toolbar dividers were (`SetDesiredSizeOverride` before Slate). Fix with `MakeLine`.
- `Rules.md` "Where state lives" does not list `UStageEnvironmentSubsystem` yet. Claude did not edit Rules.md.
- Sky Creator's lightning, rainbow, wind and moon settings are not exposed; they keep the plugin's defaults.

---

## #36 — Project audit and technical discovery: PROJECT_AUDIT.md, CURRENT_SYSTEMS.md (2026-10-20)

**Request (Gevor).** Run an evidence-based audit of the whole existing project before any new feature. Cover structure, every system, real execution paths, the architecture and dependency map, a status for every feature, ranked findings, a roadmap and tests. Produce `PROJECT_AUDIT.md` and `CURRENT_SYSTEMS.md`. Do not change architecture or implementation code.

**What was done**
- Read `Source/` (both modules, 250 files, about 42,400 lines), `Config/`, the `Content/` folder listing, `Scripts/`, `Rules.md`, this file, `tasks/todo.md` and ADR 0001–0009.
  - The reading was split into six areas: UI and workspace; scene and CAD; lighting, show control and DMX; persistence, import and BOM; architecture and performance; documentation against code.
  - The highest-impact findings were confirmed by hand in the source.
- Ran the full automation suite once: 46/46 passed.
- No `.uasset` or `.umap` was opened. The MCP bridge was attached to the Kmidden editor for the whole session.
- No build, PIE or packaged run.
- No code, config or content was changed.

**Main findings.** Full table in `Docs/PROJECT_AUDIT.md` §10.

- **R1. A secret is committed.** `Config/UserEngine.ini` (tracked) holds the UnrealNGGMCP `AuthToken`. The Anthropic key from #29 is still in git history.
- **R2. A partial scene load is marked clean, so the next save drops the skipped items.**
  - `ApplyDocument` calls `MarkClean()` after skipping items (`StageSceneComponent.cpp:265-297`).
  - `LoadScene` does not wait for startup re-imports to finish.
- **R3. Undo and redo of a transform ignore the item lock and the GameMode.** `FLiveItemEditor::SetItemTransform` calls `SetActorTransform` directly (`StageEditHistoryComponent.cpp:80-94`).
- **R4. Dragging an inspector spin box records one undo step per value change.**
  - `OnValueChanged` → `RequestParameterChange` → `RecordTransformChange`.
  - The history has no merging and a 100-step cap.
- **R5. No unsaved-changes prompt on quit, and no autosave** (`StageWorkspaceShell.cpp:572-581`).
- **R6. SaveGame state is not sanitised when a scene is restored** (fixture attributes, patch, loudspeaker gain and delay).
- **R7. No scene format migration.**
- **R8. A corrupt `Imports.json` is overwritten.**
- **R9. The profile is reset on a version mismatch.**
- **R10. Show control is unreachable.**
  - The cue engine has no UI, no C++ caller and no persistence.
  - `UStageDMXBridge` has no transport.
- **R11. No person-confirmed run is recorded for #1–#35.**
  - These have no tests: GameMode rules, the request bridge, selection, the session, economy/profile and show control.
- **R12. Documentation drift.**
  - This entry fixes the #35 hash.
  - Rules.md still has the RunEditor, DamenTools8 and UnrealClaude sections from another project.
  - The Rules.md subsystem table is missing 7 subsystems.
  - ADR 0001 §3 has a stale layout path and built-in layout names.
  - Stale boxes in todo.md: lines 7, 32, 47, 49, 92 and 198.
  - `Content/Python/stagecraft_setup_content.py` is stale.

**Files changed**
- `Docs/PROJECT_AUDIT.md` (new)
- `Docs/CURRENT_SYSTEMS.md` (new)
- `STATE.md`: this entry, and the #35 commit hash set to `4f310c2`.
- `tasks/todo.md`: the audit follow-ups section.

**Verification (Claude, 2026-10-20)**
- Automation `StageCraft`: 46/46 Success, exit code 0.
- Findings R2, R3, R4, R5 and R8 were re-read in source after the area reports came in. The runtime effects are inferred from code and have not been reproduced.
- Status: **audit complete; findings awaiting review by Gevor.** Nothing was implemented.

**Commit:** `a28dbf6` (together with #37; hash set 2026-10-21).

**Housekeeping.** Commit `960f5da` ("ngg-assets.json") only changed `.claude/.ngg-assets.json`, which is MCP tooling state. It had no entry; it is recorded here.

**Known issues / follow-ups**
- The open questions are in `Docs/PROJECT_AUDIT.md` §10:
  - Do any Blueprints call the cue or DMX functions? This needs the editor with this project open.
  - Which manual checklists have actually been run?
  - Are the shop/economy and hardware DMX in scope?
  - What rig-size performance target applies?
  - Has the key been rotated?
  - Should the test map and assets ship?
- Recommended next task: roadmap step 1 (scene data safety and edit integrity: R2–R6, R8).

---

## #37 — Undo/redo obeys locks and rules, one undo step per inspector drag, autosave and exit prompt, scene format v2 with environment, packaged builds and sky cost measured (2026-10-20)

**Request (Gevor).** Phase 2: undo and redo must respect item locks and GameMode rules, with every item of a group checked. An inspector slider or numeric drag must make exactly one undo step. Add autosave and a Save / Discard / Cancel prompt on exit. Store the environment in scenes, with v1 → v2 migration tests. Phase 3: verify the UE 5.8 Standalone and Shipping builds, check that the Sky Creator assets package cleanly, and measure the sky's GPU and video memory cost. Design: `Docs/ADR/0010-edit-rules-autosave-scene-v2.md`.

**What changed and why**
- **Undo/redo checks the rules (audit R3).**
  - **Root cause:** `FLiveItemEditor::SetItemTransform` wrote `SetActorTransform` directly, unlike parameter undo, which went through the GameMode.
  - **New GameMode rule:** `AStageCraftGameModeBase::EvaluateTransformChange` first requires a sane transform. It then judges each changed part (Location, Rotation, Scale) through `EvaluateParameterChange`, so the lock, a span's refusal of Scale, ownership and Blueprint overrides all apply.
  - **New history hook:** `UStageEditHistoryComponent::TransformValidator`, bound by the controller (`ValidateHistoryTransform`), which reports refusals.
  - **Outcomes:** a refused step stays in the history so it can be retried. A step whose transform is not sane (non-finite, more than 10 km out, degenerate rotation) is dropped as Invalid.
  - **Groups:** a group step (`FStageBatchCommand`) checks every item and is all-or-nothing; a locked item refuses the step, and the items already moved are rolled back.
  - **New shared limit:** `StageTransformRules::IsSaneTransform` and `MaxCoordinate`, which the scene store now uses too.
- **Live group edits.** Drag start and numeric group edits already asked the GameMode about every selected item (`EvaluateSelectionTransform`). `UStageGroupTransformComponent::FollowLeader` now also sanity-checks every follower's computed transform before writing it.
- **One undo step per drag (audit R4).**
  - **Root cause:** each spin-box `OnValueChanged` went through `RequestParameterChange`, which recorded one step per change, and the history has no merging.
  - **New mechanism:** `UStageEditHistoryComponent::BeginTransaction` / `EndTransaction`. While open, recorded transform and parameter changes keep the first "before" per item (and per parameter). When it ends, they become one step; items that ended where they started are left out.
  - **Wiring:**
    - `UStageParameterRowWidget` binds `OnBeginSliderMovement`, which calls `UStageParameterControlWidget::BeginInteraction`; `FinishInteraction` fires `OnInteractionFinished` once.
    - `UStageParameterViewWidget` calls `AModularPlayerController::BeginEditTransaction` / `EndEditTransaction`. It also ends a transaction it opened when its controls go away (`bOwnsEditTransaction`).
    - Undo or redo ends an open transaction first, and so does `EndPlay`.
  - **Unchanged:** every value is still validated and applied live.
- **Autosave and recovery (audit R5).**
  - **Timer:** `UStageSceneComponent` runs a looping timer only while the session is dirty (started and stopped by `OnDirtyChanged`). `AutosaveIntervalSeconds` defaults to 120 s; 0 turns it off.
  - **Write:** it saves only if the edit serial changed since the last autosave, never during a user save or load. The file is `Saved/StageCraft/Scenes/Autosave/<scene>.json`, or `(Untitled).json` for a stage never saved. It is written by a separate `FStageSceneStore` on a background task. It never touches the user's file and never marks the stage clean.
  - **Cleanup:** a successful Save deletes the stage's autosave, and the autosave under its previous name. Don't Save on exit deletes it too.
  - **Recovery:** File › Recover Autosave (`RequestRecoverAutosave`) opens it through the normal load path. The stage keeps its scene name and stays unsaved.
- **Exit prompt.**
  - **Hook:** `FStageWorkspaceShell` asks `Callbacks.ConfirmMainWindowClose` before closing the main window. `UStageWorkspaceSubsystem::ConfirmMainWindowClose` shows `SStageUnsavedChangesDialog` ("Save changes before closing?" with Save / Don't Save / Cancel) when the session is dirty.
  - **Display:** the dialog sits on a dimmed overlay (`FStageWorkspaceShell::ShowDialog`) that takes every click and the keyboard focus. Escape cancels.
  - **Untitled stages:** the dialog shows a name field; the first Save on an existing name only warns.
  - **Save:** waits for `OnSceneOperationFinished`, then calls `CloseMainWindow`. A failed save keeps the dialog open with the reason.
  - **Why not a modal window:** a Slate modal loop would block the save's game-thread continuation.
  - **Scope:** Standalone and packaged builds only. PIE and `exit` / `RequestExit` are not affected.
- **Scene format v2 (audit R7).**
  - **Versions:** `FStageSceneStore::FormatVersion = 2`, `MinReadableVersion = 1`. Version 3 and above, and version 0, are refused as `VersionMismatch`.
  - **New data:** the `environment` object (sky, time, sun height and direction, clouds, rain, snow, fog). Missing fields keep their defaults, and values are sanitized.
  - **Migration:** version 1 files load with the default environment (sky off); re-saving writes version 2.
  - **Capture and apply:** `CaptureStage` reads the environment, and `ApplyDocument` sets it before `MarkClean`.
  - **Dirty tracking:** `UStageSessionSubsystem` binds `OnEnvironmentChanged` (`InitializeDependency<UStageEnvironmentSubsystem>`), so environment edits mark the scene unsaved.
- **Packaging.**
  - **Missing engine asset:** the packaged game logged "Failed to find `/Engine/EngineSky/VolumetricClouds/m_SimpleVolumetricCloud_Inst`" when the sky turned on. The engine's `UVolumetricCloudComponent` loads it as its default material, and it was not cooked. Fixed with `+DirectoriesToAlwaysCook=(Path="/Engine/EngineSky/VolumetricClouds")` (19 packages).
  - **Dead reference, left alone:** Sky Creator's `MF_SC_BlueNoiseDithering` references `STBlueNoise_scalar_128x128x64`, which UE 5.8 no longer ships. Nothing that renders uses that function, so the plugin content is not edited.
- **Dev console commands** (compiled out of Shipping): `StageCraft.Workspace.ExitPrompt Save [Name]|Discard|Cancel`, `StageCraft.Scene.AutosaveNow`, `StageCraft.Scene.Recover <Name>`. `StageCraft.Scene.Status` now lists autosaves.

**Files changed**
- **New:**
  - `Private/Workspace/SStageUnsavedChangesDialog.h/.cpp`
  - `Private/History/Tests/StageHistoryWorldTests.cpp`
  - `Docs/ADR/0010-edit-rules-autosave-scene-v2.md`
- **History and rules:**
  - `Public/Interaction/StageTransformRules.h`
  - `Public/Game/StageCraftGameModeBase.h`, `Private/Game/StageCraftGameModeBase.cpp`
  - `Public/History/StageEditHistoryComponent.h`, `Private/History/StageEditHistoryComponent.cpp`
  - `Private/Components/StageGroupTransformComponent.cpp`
  - `Public/Player/ModularPlayerController.h`, `Private/Player/ModularPlayerController.cpp`
- **Inspector:**
  - `Public/UI/StageParameterControlWidget.h`, `Private/UI/StageParameterControlWidget.cpp`
  - `Public/UI/StageParameterRowWidget.h`, `Private/UI/StageParameterRowWidget.cpp`
  - `Public/UI/StageParameterViewWidget.h`, `Private/UI/StageParameterViewWidget.cpp`
- **Scenes and session:**
  - `Public/Scene/StageSceneTypes.h`
  - `Private/Scene/StageSceneStore.h/.cpp`
  - `Public/Scene/StageSceneComponent.h`, `Private/Scene/StageSceneComponent.cpp`
  - `Private/Scene/StageSceneConsoleCommands.cpp`
  - `Private/Scene/Tests/StageSceneStoreTests.cpp`
  - `Public/Subsystems/StageSessionSubsystem.h`, `Private/Subsystems/StageSessionSubsystem.cpp`
- **Workspace:**
  - `Private/Workspace/StageWorkspaceShell.h/.cpp`
  - `Public/Workspace/StageWorkspaceSubsystem.h`, `Private/Workspace/StageWorkspaceSubsystem.cpp`
  - `Private/Workspace/SStageWorkspaceMenuBar.h/.cpp`
  - `Private/Workspace/StageWorkspaceConsoleCommands.cpp`
- **Config:** `Config/DefaultGame.ini` (engine cloud cook directory).
- **Docs:**
  - `Docs/ADR/0009-outdoor-environment.md` (§7 open items closed)
  - `Docs/PROJECT_AUDIT.md` (follow-up note)
  - `Docs/CURRENT_SYSTEMS.md` (updated rows)
  - `tasks/todo.md`
  - `STATE.md` (this entry)
- **Not changed:** no content assets, Blueprints, maps or plugin files.

All `Source/...` paths above are under `Source/ModularSceneBuilder/`.

**Verification (Claude, 2026-10-20)**
- **Builds (UE 5.8):** Editor Development, Game Development and Game Shipping (`-DisableAdaptiveUnity`) all succeeded with no warnings or errors.
- **Automation:** 49/49 StageCraft tests passed, exit code 0. New tests:
  - `StageCraft.History.Rules` (world test):
    - the rule matrix: locked, unchanged, moved, turned, 20 km away, NaN;
    - undo and redo of a locked item refused with nothing moved, then succeeding once unlocked;
    - a group step with a locked item refused, and the item already undone rolled back;
    - an out-of-range redo dropped as Invalid.
  - `StageCraft.History.Transaction` (world test):
    - 50 moves recorded as one step, with undo returning to before the drag;
    - a group drag as one step;
    - a drag back to its start recording nothing;
    - undo during an open drag closing it and reverting it;
    - a stray End doing nothing.
  - `StageCraft.Scene.Migration`:
    - version 2 environment round trip;
    - version 1 migrated to the default environment, and a stray environment in a version 1 file ignored;
    - versions 0 and 3 refused;
    - hostile values wrapped or clamped, a wrong type or missing field defaulted;
    - a version 1 file on disk re-saved as version 2.
  - `StageCraft.Scene.Format` updated: the "future version" case is now `FormatVersion + 1`.
- **Scripted Standalone runs** (`-game -windowed`, logs and screenshots `ScreenShot00023`/`00024`):
  1. Placed two crates and made a group move. Sky on, 19:00, clouds 0.8. `AutosaveNow` wrote `(Untitled).json`. Closing the window showed the prompt (screenshot), and a second close kept it open. Save without a name was refused; Save "R37 Test" saved and quit. The file is version 2 with the environment, and the `(Untitled)` autosave was deleted.
  2. Loaded "R37 Test": environment restored, scene clean. Group move, then locked the Prop group: Undo was refused with "Test Crate is locked…" and nothing moved; after unlocking, Undo succeeded. Loaded the real version 1 file "R27 A": 6 items, default environment, file still version 1 on disk. Fog 0.5 marked the scene unsaved. The prompt showed "R27 A" (screenshot). Cancel kept the app open; an autosave was written; Don't Save quit and deleted it.
  3. and 4. Edited, autosaved, then quit through `quit`, which bypasses the prompt the way a crash would. The next launch listed the autosave. Recover restored the moved crates and rain 0.6, and the stage stayed unsaved. Don't Save then deleted the autosave.
- **Packaging:** `BuildCookRun` Development and Shipping both succeeded: 918 packages, no warnings or errors in the cook. All six Sky Creator content folders are in the container (259 assets). The packaged Development run with the sky on is warning-free after the cook fix. The Shipping package (949 MB) ran for 25 s and quit cleanly on a window close.
- **Sky cost** (packaged Development; 1184×657 view in a 1600×900 window; 300-frame CSV per state, two runs; **the Kmidden editor was open on the same GPU**, so absolute numbers are only indicative):

| State | GPU time | Video memory in use |
|---|---|---|
| Sky off | 4.4–4.5 ms | about 1.9 GB |
| Sky on, clear | 6.6–6.9 ms | +180 MB |
| Sky on, Rain | 9.0–9.6 ms | +285 MB |

  Moving-head beams stay visible with the sky on (screenshot), which disproves the audit's inference.
- **Not verified by a person:** one undo step per spin-box drag by mouse (the transaction itself is tested), and the dialog's buttons and name field by mouse.
- Status: **implemented, awaiting manual verification** of those items.
- **2026-10-21:** Gevor reported the features verified ("I've verified the features") and accepted the 120 s autosave default in the controller defaults; no user setting for the interval for now. Status: **done.**

**Commit:** `a28dbf6` (together with #36; hash set 2026-10-21).

**Known issues / follow-ups**
- Audit R2 is still open: a partial load is marked clean, so re-saving drops the skipped items, and a load does not wait for pending imports. R6 (restored state not sanitized) and R8 (corrupt `Imports.json` overwritten) are also open.
- Fixture attribute, patch, visibility, lock and label edits are still not undoable.
- Mouse-wheel steps on a spin box are one undo step each (discrete edits).
- `exit` / `RequestExit` and PIE never show the prompt (by design). An autosave left behind by such an exit stays recoverable.
- Re-measure the sky with no second editor open. The local video memory budget the OS reported fell from 7.2 GB to 4.3–4.5 GB during the runs, most likely because the other editor was using the GPU.
- Cues are still not saved (needs format version 3).

## #38 — Cue list panel, cues saved in scenes (format v3) and undoable, DMX patch sheet export as PDF + CSV (2026-10-21)

**Request (Gevor).** Next milestone after #37: a Show Control / Cue panel and professional documentation export. Scope chosen by Gevor (2026-10-21): the full cue list (Store/Update, Go/Back, Delete, label and fade time, Internal/External), cues saved with scenes, cue edits undoable; and a patch sheet as PDF + CSV. Design: `Docs/ADR/0011-cue-list-and-patch-sheet-export.md`.

**Housekeeping first.** #36 and #37 still said "uncommitted"; both now carry `a28dbf6`. #37 records Gevor's verification and his decision to keep the 120 s autosave default without a user setting.

**What changed and why**
- **Cue rules (`StageCueRules`, `Show/ShowControlTypes.h/.cpp`).**
  - **Root cause addressed:** the Phase 5 cue engine accepted any number (clamped to 0), any fade, and kept duplicates if two numbers were within the 0.001 lookup tolerance.
  - **Rules:** a cue number is finite and 0.001 to 9999.999, stored rounded to 0.001; a fade is clamped to 0..600 s; a label is trimmed and at most 256 characters. `SanitizeCueList` repairs untrusted lists: invalid and duplicate numbers dropped (the first wins), the rest sorted.
  - **Float pitfall found by the tests:** near 10000 a float step is about 0.001, so float rounding let 10000 pass as 9999.999 and turned 1.5 into 1.5000001. Rounding now goes through whole thousandths in double precision (`ToThousandths`), and divides by 1000 instead of multiplying by 0.001.
- **Show control (`UShowControlSubsystem`).**
  - `StoreCue` now returns false for an invalid number instead of clamping it to cue 0, and inserts in order.
  - New: `UpdateCue` (label and fade, the recorded look kept; no event when nothing changed), `Back` (the previous cue with its own fade), `FindCue`, and `ReplaceCues` (undo/redo: replaces the list but keeps playback). `SetCues` (loading) still resets playback and now repairs the list.
- **Request path.**
  - New GameMode rule `AStageCraftGameModeBase::EvaluateCueRequest(Player, EStageCueRequest, CueNumber)` (BlueprintNativeEvent). It refuses when there is no show control, an invalid number for Store, a missing cue for Update or Delete, and playback while the source is External. Storing under External is allowed, because it captures the look the external console sends. Unknown request kinds are denied by default.
  - New controller requests: `RequestStoreCue`, `RequestUpdateCue`, `RequestDeleteCue`, `RequestGoToCue`, `RequestCueGo`, `RequestCueBack`, `RequestSetControlSource`. Each asks the GameMode, calls the subsystem and reports refusals on `OnRequestRejected` (status bar) with a reason ("This is the last cue.", "An external console is driving the fixtures...").
- **Undo.**
  - `FStageCueListCommand` (new, `History/StageCueCommands`) holds the whole cue list before and after one edit. Undo and Redo write it through a new `IStageItemEditor::SetCueList`, which the live editor implements with `ReplaceCues`. It has a default implementation (Invalid), so test fakes are unchanged.
  - `UStageEditHistoryComponent::RecordCueListChange` ends any open inspector transaction first, so a drag and a cue edit never merge.
  - Store, Update and Delete are undoable. Playback and the control source are not, like a lighting console. An Update that changes nothing records nothing.
- **Dirty and scene format v3.**
  - `UStageSessionSubsystem` binds `OnCueListChanged` (after play has begun) and marks the stage dirty, so autosave and the exit prompt (#37) cover cue edits.
  - `FStageSceneStore::FormatVersion` is 3. A `cues` array holds number, label, fade and per-fixture attributes (tag names, values). Version 1 and 2 files load with no cues; a stray `cues` in a v2 file is ignored.
  - Reading is tolerant and bounded: at most 2000 cues, 4096 fixtures per cue and 256 attributes per fixture. Unknown tags, non-finite values, bad fixture ids and bad cue numbers are dropped and counted (`FStageSceneParseReport::InvalidCueEntries`, logged on load).
  - `UStageSceneComponent` captures the cue list with the stage and applies it after the items, so the fixture IDs exist, and before `MarkClean`.
- **Cue panel (`UStageCuePanel`, new).**
  - A code-built UMG dock panel like the Patch panel; `DA_Panel_Cues` (tag `StageCraft.Panel.Cues`) was created headlessly by `Scripts/Content/create_cue_content.py`. It is a tab in the default layout's bottom stack, after Faders and Patch, and in File › Production › Cue List...
  - **Transport row:** GO, BACK, the active cue, label and "(fading)", the cue count, and an INTERNAL / EXTERNAL DMX segment with the active source lit.
  - **Record row:** cue number, which suggests the next whole number; label; fade; and STORE, which reads RE-RECORD when that number exists.
  - **Cue list:** number, label and fade (edited in place, committed through `RequestUpdateCue`), fixture count, GO TO and DELETE. The active row is highlighted.
  - It reads the subsystem only and rebuilds on the next tick after `OnCueListChanged`, `OnCueStarted`, `OnCueFinished` or `OnControlSourceChanged`; there is no tick and no polling. Every write goes through the controller.
- **Patch sheet export.**
  - **`FStagePdfDocument` (new, `Report/StagePdfWriter`):** a dependency-free PDF 1.4 writer (written by a subagent; reviewed and tested here). It uses the standard Helvetica and Helvetica-Bold fonts with WinAnsi encoding and real Adobe metrics for truncation, and writes a correct xref table. Non-Latin text prints as "?" in the PDF; the CSV keeps full Unicode.
  - **`StagePatchSheet` (new, pure):** sorts and counts the rows (patched, unpatched, conflicts, universes) and renders a print-ready A4 landscape PDF (title block on every page, repeated header row, zebra rows, "Page n of m" footer) and a CSV (UTF-8 BOM, CSV-injection-safe through `StageBillOfMaterials::EscapeCsvField`).
  - **`UStagePatchSheetSubsystem` (new, world):** gathers the rows on the game thread from show control (FID, label, type, mode, universe/address, footprint, position in metres, conflict). It renders and writes both files on a worker to `Saved/StageCraft/Reports/PatchSheet_<Scene>_<time>.pdf/.csv` (both or neither), one export at a time, and the result goes to the status bar.
  - **`StageReportFiles` (new, private):** the report folder, unique file stems and atomic byte writes, now shared by the bill of materials export.
  - **Entry points:** File › Production › Export Patch Sheet (PDF + CSV), an EXPORT PDF button on the Patch panel, `AModularPlayerController::RequestExportPatchSheet`.
- **Dev console commands** (compiled out of Shipping): `StageCraft.Show.Cue Store <n> [fade] [label] | Update <n> <fade> [label] | Delete <n> | Go | Back | GoTo <n> | Source Internal|External | List`, and `StageCraft.Report.ExportPatch`.

**Files changed**
- **New:**
  - `Docs/ADR/0011-cue-list-and-patch-sheet-export.md`
  - `Scripts/Content/create_cue_content.py`
  - `Content/StageCraft/UI/Panels/DA_Panel_Cues.uasset`
  - `Source/ModularSceneBuilder/Private/Show/ShowControlTypes.cpp`
  - `Source/ModularSceneBuilder/Public/History/StageCueCommands.h`, `Private/History/StageCueCommands.cpp`
  - `Source/ModularSceneBuilder/Public/UI/StageCuePanel.h`, `Private/UI/StageCuePanel.cpp`
  - `Source/ModularSceneBuilder/Public/Report/StagePdfWriter.h`, `Private/Report/StagePdfWriter.cpp`
  - `Source/ModularSceneBuilder/Public/Report/StagePatchSheet.h`, `Private/Report/StagePatchSheet.cpp`
  - `Source/ModularSceneBuilder/Public/Report/StagePatchSheetSubsystem.h`, `Private/Report/StagePatchSheetSubsystem.cpp`
  - `Source/ModularSceneBuilder/Private/Report/StageReportFiles.h/.cpp`
  - Tests: `Private/Show/Tests/StageCueRulesTests.cpp`, `Private/Report/Tests/StagePatchSheetTests.cpp`
- **Show, rules, history:** `Public/Show/ShowControlTypes.h`, `Public/Subsystems/ShowControlSubsystem.h`, `Private/Subsystems/ShowControlSubsystem.cpp`, `Public/Game/StageCraftGameModeBase.h`, `Private/Game/StageCraftGameModeBase.cpp`, `Public/Player/ModularPlayerController.h`, `Private/Player/ModularPlayerController.cpp`, `Public/History/StageEditCommand.h`, `Public/History/StageEditHistoryComponent.h`, `Private/History/StageEditHistoryComponent.cpp`, `Private/Show/StageShowConsoleCommands.cpp`
- **Scenes and session:** `Public/Scene/StageSceneTypes.h`, `Private/Scene/StageSceneStore.h`, `Private/Scene/StageSceneStore.cpp`, `Private/Scene/StageSceneComponent.cpp`, `Public/Subsystems/StageSessionSubsystem.h`, `Private/Subsystems/StageSessionSubsystem.cpp`
- **Reports and UI:** `Private/Report/StageBillOfMaterialsSubsystem.cpp`, `Private/Report/StageReportConsoleCommands.cpp`, `Public/UI/StagePatchPanel.h`, `Private/UI/StagePatchPanel.cpp`, `Public/UI/StageStatusBarWidget.h`, `Private/UI/StageStatusBarWidget.cpp`
- **Workspace:** `Public/Workspace/StageWorkspaceTypes.h`, `Private/Workspace/StageWorkspaceTypes.cpp`, `Private/Workspace/StageWorkspaceShell.cpp`, `Private/Workspace/SStageWorkspaceMenuBar.cpp`
- **Tests changed:** `Private/History/Tests/StageHistoryWorldTests.cpp` (new `History.Cues`), `Private/Scene/Tests/StageSceneStoreTests.cpp` (new `Scene.Cues`; labels for v3)
- **Docs:** `Docs/CURRENT_SYSTEMS.md`, `tasks/todo.md`, this file
- **Not changed:** no Blueprints, maps, input assets or plugins. Config unchanged: panel assets are cooked by the existing `StagePanel` Asset Manager rule.

**Verification (Claude, 2026-10-21)**
- **Builds (UE 5.8):** Editor Development, Game Development and Game Shipping (`-DisableAdaptiveUnity`) all succeeded with no warnings or errors.
- **Automation:** 54/54 StageCraft tests passed, exit code 0. New tests:
  - `Show.CueRules`: number range including 0.0004, 10000, NaN and infinity; rounding; fade and label limits; list repair.
  - `History.Cues` (real game world): the GameMode cue rules; Store, Update and re-store ordering; Go/Back/GoTo and the end of the list; the External refusal; ReplaceCues keeping playback; SetCues repairing and resetting; undo/redo of a delete; a cue edit marking the stage dirty; a cue edit closing an open drag as its own step.
  - `Scene.Cues`: exact round trip; v1 and v2 migrate with no cues; a damaged list keeps 2 good cues and counts all 8 dropped entries.
  - `Report.PatchSheet` and `Report.Pdf`: sorting and counts, sanitizing, CSV injection and quoting, PDF structure (xref offsets, stream lengths, page count for 0 and 100 rows), escaping, non-Latin to "?", text metrics.
  - The first run caught the float rounding bug above (2 failures), fixed before the final run.
- **Scripted Standalone runs** (`-game -windowed`, logs `Saved/Logs/R38_Run1-4.log`, screenshots `ScreenShot00025`-`00031`):
  1. **Run 1:**
     - Store 1, 2 and 1.5 sorted. Update 2 changed the label and fade. Store 0 was refused with "Cue numbers run from 0.001 to 9,999.999".
     - Go → 1 → 1.5, Back → 1. External dropped the running fade and refused Go with the reason.
     - Ctrl+Z undid "Edit Cue 2" and redo re-applied it; Delete 1.5 was undone.
     - The patch sheet was exported: `PatchSheet_R37_Test_20261021-102508.pdf` (3.6 KB, checked visually: title block, both fixtures, 1.001–1.021 / 1.022–1.042, positions, footer) and `.csv`.
     - Saved "R38 Cues" as a version 3 file with 3 cues. Loading v2 "R37 Test" gave 0 cues; loading "R38 Cues" again gave the 3 cues and a clean stage.
  2. **Runs 2 and 3:** the default layout shows the Cues tab beside Faders and Patch. In the screenshots the active cue row is highlighted, "Cue 2 Dim look" shows, External shows "External DMX is driving the fixtures" with GO/BACK disabled, and the Patch panel shows EXPORT PDF. Run 2 showed the active Internal/External segment greyed (it read as disabled); that is fixed: the active segment is now lit (run 3). Gevor's layout was saved and restored around these runs.
  3. **Run 4 (playback on real fixtures):** cue 1 had both spots at dimmer 0.2 and cue 2 at 1.0 with a 2 s fade. GoTo 1 gave 5000 lm; 1 s into Go to cue 2 it read 15028 lm, and 25000 lm at the end; Back gave 5000 lm again.
- **Not run:** a new packaged build (the panel asset is cooked by the existing `StagePanel` rule, the same as the other panels).
- **Not verified by a person:** the Cue panel by mouse (buttons, inline label and fade edits, STORE / RE-RECORD), the File › Production entries, the EXPORT PDF button, and printing the PDF.
- Status: **implemented, awaiting manual verification.**

**Commit:** `274f059`.

**Known issues / follow-ups**
- **Saved layouts:** a layout saved before this change (including the restored session) has no Cues tab. The panel opens floating from File › Production › Cue List... or the Window menu, and Layout › Reset puts it in the bottom stack.
- **PDF text:** non-Latin characters print as "?" (standard fonts). Embedding a Unicode font is a follow-up if needed.
- **Cue keys:** there is no GO keyboard shortcut yet; the panel button and the console command only.
- **Undo gaps (from #37):** fixture attribute, patch, visibility, lock and label edits are still not undoable.
- **Cue features not built:** tracking, per-attribute timing, follow/wait times and a timeline. Clear Stage keeps the cue list (cues are keyed by fixture ID).
- **Test coverage:** `History.Cues` runs without fixtures (no fixture data in a bare world); fixture playback is covered by scripted run 4.
- **Audit risks still open:** R2, R6 and R8.

## #39 — Bill of materials as PDF, cable schedule and power plan (PDF + CSV), fixture benchmark at 50/200/500 and the fixes it found, Shipping validation (2026-10-21)

**Request (Gevor).** The next milestone after #38 had four parts:
1. Expand File › Production on top of `FStagePdfDocument` with a bill of materials report (PDF + CSV: equipment counts, total weight, power) and a cable schedule & electrical loads report (wiring list, load per phase/circuit).
2. Run automated benchmarks with 50, 200 and 500 active fixtures through cues and crossfades, aiming for zero frame drops and efficient CPU/GPU time.
3. Verify that the Weather/Sky assets and the PDF writer compile and package without warnings in the UE 5.8 Shipping target.
4. Keep TObjectPtr memory safety, clean modules and no per-frame Tick polling, and update STATE.md.

The design is in `Docs/ADR/0012-production-reports-and-fixture-benchmark.md`.

**Housekeeping first.** #38 said "uncommitted"; it now carries `274f059`.

**What changed and why**
- **Report composer (`FStageReportPdfComposer`, new, private `Report/StageReportPdfComposer`).**
  - The patch sheet (#38) hand-placed every line. Two more reports would have copied that layout code, so the shared A4 landscape layout now lives in one composer.
  - It provides a title block, a summary line, headings kept with their table, notes, tables with weighted columns, alignment, Normal/Muted/Strong tones and Body/Subtotal/Total rows. Header rows repeat after a page break, and every page gets a frame and "Page n of m" at `Finish` (new `FStagePdfDocument::SetCurrentPage`).
  - **Bug found by its test:** a table that started at the bottom of a page drew its header twice, once on each page. `BeginTable` now makes room before it opens the table.
- **Bill of materials PDF (`FStageBillOfMaterials::ToPdf`).**
  - Lines grouped by category with quantity, unit and total weight and power, category subtotals and a grand total.
  - The export now writes the PDF and the CSV side by side (new `StageReportFiles::WritePdfAndCsv`, also used by the patch sheet). The PDF is rendered on the game thread and written on a worker.
  - The BOM panel button reads EXPORT PDF.
- **Cable schedule and power plan (`StageCableSchedule`, new, pure; `UStageCableScheduleSubsystem`, new, world).**
  - **Scope (option B in the ADR):** StageCraft has no cable or circuit data, so the report is a generated proposal, labelled as such, not a record of a real installation.
  - **Supply (`UStagePowerDistributionSettings`, Project Settings › StageCraft Power Distribution, `Config/DefaultGame.ini`):** 230 V, 3 phases, 16 A circuits, 80% usable share and a power factor of 1 by default, so a circuit takes 2944 W. Values are sanitized (V/A have no engine units, so the fields use display names).
  - **Algorithm:** loads are sorted largest first and placed onto the least-loaded phase, then onto circuits of that phase (first fit). A load larger than a circuit gets its own circuit and a warning. Power home-runs go from the distro position. DMX chains run per universe in address order, with each length rounded up to the next stock length (1–50 m, 20% routing slack); a run over 50 m gets a warning.
  - **Warnings:** overloaded circuits, phase imbalance over 15%, runs longer than stock, and fixtures with no DMX patch.
  - **Names:** DMX endpoints print as "#ID label" (or "#ID").
  - **Request path:** File › Production › Export Cable Schedule & Power (PDF + CSV) → `AModularPlayerController::RequestExportCableSchedule` → the subsystem's `StartExport`. The subsystem gathers the session's placed items and fixtures on the game thread, builds on a worker, and finishes on the game thread through a weak pointer and a serial. The status bar shows the result.
- **Fixture benchmark (`StageCraft.Bench.Fixtures`, new, `Diagnostics/StageFixtureBenchmark.cpp`, Development only).**
  - `StageCraft.Bench.Fixtures 50 200 500 Seconds=10 Fade=2 Zoom=1` spawns N fixtures in a grid, waits for pending model imports and a settle time, then measures idle and continuous cue crossfades. It writes `Saved/StageCraft/Benchmarks/FixtureBench_<ts>.csv` and a summary per phase (mean/p95/p99 frame, game, GPU and show-tick time, and dropped frames over 16.7 ms). `StageCraft.Bench.Cancel` stops it.
  - It runs from an `FTSTicker` only while active, not from a component Tick. Phases are marked as Insights trace regions ("Bench N idle|crossfade").
  - `FStageFrameStats` (new, pure) computes the statistics. `Scripts/Bench/RunFixtureBench.ps1` runs it in a `-game` window and prints the summary.
  - **Bug found by a scripted run:** the harness left the active cue at its own cue 9001 and changed the user's fixture looks. It now snapshots the cue list, the active cue, the looks and the dirty flag, and restores them all when it finishes.
  - **Render-thread time is not reported:** `GRenderThreadTime` reads 0 in this multi-window app, so the render thread was measured in Unreal Insights instead.
- **Performance fixes (each measured before and after; see the table below).**
  - **Light function only while shaped:** the gobo/iris/blade light function cost about 28 ms of GPU at 500 fixtures while doing nothing. `ApplyBeamShaping` now attaches it only when a gobo, an iris below 1 or a blade is in use. Crossfade GPU at 500 fixtures went from 42.8 to 14.2 ms.
  - **Batched light rebuild for moving heads:** each moving light paid about 87 µs on the render thread in `FScene::UpdateLightTransform`. Pan/tilt now sets the pivots with `SetRelativeRotation_Direct`, updates the pan pivot once, and marks the light's render state dirty, so the engine rebuilds the light proxy in a batch (about 11 µs). With zoom fixed, 500 crossfading fixtures went from 58.7 to 27.0 ms a frame.
  - **No navigation updates:** fixture meshes no longer affect navigation (`SetCanEverAffectNavigation(false)`).
  - **Driven outputs only:** `ApplyOutputs(EFixtureOutputs)` writes only the outputs a changed attribute drives (dimmer and colour, beam, position, shaping) instead of all of them.
  - **Changing-only fades:** `GoToCue` records which attributes actually change, and `TickFades` lerps only those. Show tick at 200 fixtures went from 6.2 to 3.5 ms. `UShowControlSubsystem::GetTickCostStats` measures it in non-Shipping builds.
- **Shipping and packaging.** Shipping and Development packages built, cooked and staged with no errors or warnings. The Sky Creator and EngineSky assets, the new panels and the light function material are in the pak, and the Shipping exe contains no dev command strings.

**Files changed**
- **New:**
  - `Docs/ADR/0012-production-reports-and-fixture-benchmark.md`
  - `Scripts/Bench/RunFixtureBench.ps1`
  - `Source/ModularSceneBuilder/Private/Report/StageReportPdfComposer.h/.cpp`
  - `Source/ModularSceneBuilder/Public/Report/StageCableSchedule.h`, `Private/Report/StageCableSchedule.cpp`
  - `Source/ModularSceneBuilder/Public/Report/StageCableScheduleSubsystem.h`, `Private/Report/StageCableScheduleSubsystem.cpp`
  - `Source/ModularSceneBuilder/Public/Report/StagePowerDistributionSettings.h`, `Private/Report/StagePowerDistributionSettings.cpp`
  - `Source/ModularSceneBuilder/Public/Diagnostics/StageFrameStats.h`, `Private/Diagnostics/StageFrameStats.cpp`
  - `Source/ModularSceneBuilder/Private/Diagnostics/StageFixtureBenchmark.cpp`
  - Tests: `Private/Report/Tests/StageReportTestUtils.h`, `Private/Report/Tests/StageProductionReportTests.cpp`, `Private/Diagnostics/Tests/StageFrameStatsTests.cpp`, `Private/Show/Tests/StageFixturePlaybackTests.cpp`
- **Reports:** `Public/Report/StagePdfWriter.h`, `Private/Report/StagePdfWriter.cpp`, `Public/Report/StageBillOfMaterials.h`, `Private/Report/StageBillOfMaterials.cpp`, `Public/Report/StageBillOfMaterialsSubsystem.h`, `Private/Report/StageBillOfMaterialsSubsystem.cpp`, `Private/Report/StagePatchSheetSubsystem.cpp`, `Private/Report/StageReportFiles.h/.cpp`, `Private/Report/StageReportConsoleCommands.cpp`, `Private/Report/Tests/StagePatchSheetTests.cpp`
- **Fixtures and show control:** `Public/Actors/LightingFixtureActor.h`, `Private/Actors/LightingFixtureActor.cpp`, `Public/Subsystems/ShowControlSubsystem.h`, `Private/Subsystems/ShowControlSubsystem.cpp`
- **Request path and UI:** `Public/Player/ModularPlayerController.h`, `Private/Player/ModularPlayerController.cpp`, `Private/Workspace/SStageWorkspaceMenuBar.cpp`, `Public/UI/StageStatusBarWidget.h`, `Private/UI/StageStatusBarWidget.cpp`, `Public/UI/StageBillOfMaterialsPanel.h`, `Private/UI/StageBillOfMaterialsPanel.cpp`
- **Config:** `Config/DefaultGame.ini` (power distribution defaults)
- **Docs:** `Docs/CURRENT_SYSTEMS.md`, `tasks/todo.md`, this file
- **Not changed:** no Blueprints, maps, input assets, content assets or plugins.

**Verification (Claude, 2026-10-21)**
- **Builds (UE 5.8):** Editor Development and Game Shipping (`-DisableAdaptiveUnity`) succeeded with 0 warnings and 0 errors.
- **Packages:** `BuildCookRun` Shipping and Development: 1081 packages, 0 errors, 0 warnings. The pak lists 259 Sky Creator assets, 25 EngineSky cloud assets, `DA_Panel_Cues`, `DA_Panel_BillOfMaterials`, `M_StageBeamLightFunction` and `DA_MovingHead_Test`. The Shipping package ran for 25 s and closed cleanly. The packaged Development build exported the BOM, cable schedule and patch sheet PDFs and CSVs.
- **Automation:** 63/63 StageCraft tests passed. New tests:
  - `Report.CableSchedule.Power`: phase balance, circuit packing, an oversize load, bad input data, sanitized settings and determinism (any input order gives the same circuits).
  - `Report.CableSchedule.Dmx`: chains per universe in address order, stock lengths, over-length and unpatched warnings.
  - `Report.CableSchedule.Export`: 92 loads and 90 links: CSV headers, quoting and formula injection, PDF structure, title and footer on every page, and the empty report.
  - `Report.BomPdf` and `Report.Composer`: PDF structure, totals, page breaks and repeated headers. This test caught the double-header bug.
  - `Bench.FrameStats`: mean, percentiles, non-finite samples, dropped frames.
  - `Lighting.BeamShapingOnDemand`, `Lighting.PanTiltTransform`, `Lighting.FadeChangedOnly`: the fixture output path behind the performance fixes.
- **PDFs checked visually:** `BOM_R38_Cues_20261021-114135.pdf` and `CableSchedule_R38_Cues_20261021-114136.pdf` (6 pages, 12 circuits, 5% imbalance, 3 universes). The "#ID label" DMX names came after that check and are covered by the tests.
- **Benchmark** ("R38 Cues" scene, editor `-game`, D3D12, RTX 3060 Ti, 1600×900, `Zoom=1`; frame ms are mean/p95, before → after):

  | Fixtures | Phase | Frame ms | GPU ms | Show tick ms | Frames over 16.7 ms |
  |---|---|---|---|---|---|
  | 50 | crossfade | 15.6/16.6 → 11.1/12.5 | 12.8 → 9.7 | 1.56 → 0.77 | 13 → 1 of 901 |
  | 200 | crossfade | 26.0/29.2 → 14.2/15.8 | 24.0 → 10.7 | 6.2 → 3.5 | 370 → 19 of 704 |
  | 500 | idle | 33.7/55.0 → 12.7/14.6 | 31.3 → 10.4 | — | 3 → 1 |
  | 500 | crossfade | 56.2/63.3 → 29.0/31.2 | 53.5 → 14.2 | 13.5 → 7.7 | all → all |

  - A packaged Development build showed crossfade means of 6.7 ms (50), 14.8 ms (200, with one 651 ms spike that looks like garbage collection) and 32.7 ms (500).
- **Not met: zero frame drops at 500.** 500 fixtures all crossfading at once are bound by the render thread at about 27–33 ms a frame, mostly light proxy updates for 500 moving shadowed spot lights. 50 and 200 fixtures are within a 60 Hz frame almost all the time, but not with zero drops.
- **Not verified by a person:**
  - Both File › Production entries and the EXPORT PDF button, used with the mouse.
  - Printing the PDFs.
  - Watching moving heads during a crossfade for flicker or lag after the batched light rebuild.
  - Checking that a gobo, iris or blade still projects once a beam becomes shaped.
- Status: **implemented, awaiting manual verification.**

**Commit:** `565287e`.

**Known issues / follow-ups**
- **Render thread at 500 fixtures:** needs a different approach (fewer shadowed lights, light proxies shared per group, or beam meshes instead of real lights for distant fixtures). ADR 0012 §3.1 has the measurements.
- **`OnParameterChanged` broadcasts:** about 2 ms of the 200-fixture show tick goes to per-attribute broadcasts during fades.
- **Startup re-import (R2):** the `Imports.json` re-import of `AG1.FBX` at start-up causes frame spikes; the benchmark waits for it.
- **VSM warning:** "One Pass Projection max lights overflow" with hundreds of shadowed lights.
- **Cable schedule:** circuit assignments are not saved and power cables have no lengths. The patch sheet is not yet on the composer.
- **Undo gaps, cue features and audit risks** from #38 are unchanged.

## #40 — Preferences and key bindings, audit risks R2/R6/R8 fixed, DMX patch now saved (2026-10-21)

**Request (Gevor).** The final polish and Phase 6 milestone:
1. A preferences and key-bindings system that lets users remap the viewport, navigation and editing actions.
2. The open audit findings:
   - **R2:** partial scene loads must mark the dirty state correctly.
   - **R6:** restored state must be sanitized against invalid values and UObject references.
   - **R8:** `Imports.json` must be safe from corruption during unexpected shutdowns.
3. Keep TObjectPtr safety, encapsulation and no per-frame Tick polling; run every test, verify a clean package, and update STATE.md.

The design is in `Docs/ADR/0013-preferences-key-bindings-and-audit-fixes.md`.

**Housekeeping first.** #39 said "uncommitted"; it now carries `565287e`.

**What changed and why**
- **Key map (`FStageKeyBindings`, new, pure, `Input/StageKeyBindings`).**
  - **Commands:** 24 commands, each with two slots.
    - Editing: Undo, Redo, Delete, Select All, Cancel.
    - Tools: Cycle, Move W, Rotate E, Scale R, Place P, Finish Path, Remove Point.
    - View: Perspective / Top / Front / Back / Left / Right as Alt+G/J/H/Shift+H/K/Shift+K, and Grid G.
    - Fly: W/S/A/D/E/Q.
  - **Rules:**
    - Keyboard keys only; the console key is reserved.
    - Fly keys take no modifiers.
    - No two commands share a chord, and no two fly directions share a key.
    - A fly key may equal a command key (W = Move tool, and fly forward while the right button is held), as in the Unreal Editor.
    - A refusal names the conflicting command.
  - **Storage:** only overrides of the defaults are stored (GameUserSettings.ini). `FromOverrides` is the trust boundary: it clears every overridden command first so swaps load in any order, and drops bad entries.
- **Owner (`UStageInputPreferencesSubsystem`, new, the project's first `ULocalPlayerSubsystem`).**
  - It holds the key map, look sensitivity (×0.1–5) and invert look.
  - It is the single writer of the new `UStageCraftUserSettings` fields: validate, store, save, broadcast `OnInputPreferencesChanged`.
  - The ini key map is validated when read, not in `LoadSettings`, because settings load before the engine registers its keys.
- **Dispatch (`AModularPlayerController`).**
  - `InputKey` passes everything to Enhanced Input, then looks up the exact chord of a key press and runs `ExecuteCommand`.
  - Commands are suspended while flying.
  - The modifiers come from the viewport's key state.
    - The first scripted run showed the player-input state lags a frame: every Ctrl chord failed during a slow import frame.
    - The viewport records keys as Slate delivers them and clears them on focus loss.
  - The fly context is rebuilt from the key map only when it changes (`bIgnoreAllPressedKeysUntilRelease`).
  - Look sensitivity and invert are applied where look input reaches the pawn.
  - **Removed:** the eight per-shortcut `UInputAction` properties and the Ctrl/Shift chord-action machinery. No asset used them.
- **UI.**
  - The Edit menu labels ("Undo (Ctrl+Z)", tool and view entries, "Transform Tool (Space cycles)") and the status bar hints name the user's keys and leave unbound ones out. The status bar refreshes on change.
  - Edit › Preferences… opens the new **Preferences panel** (`UStagePreferencesPanel`; `DA_Panel_Preferences` created headlessly by `Scripts/Content/create_preferences_content.py`). It has:
    - Camera: look speed, vertical look.
    - Viewport: snap to items, grid.
    - Audio: master, effects, mute.
    - Keyboard shortcuts: Primary and Secondary per command, RESET, RESET ALL, CLEAR. Clicking a slot captures the next key with its modifiers. A refusal is shown in the warning colour. The mouse buttons are listed as fixed.
  - Every value is written through its owner and read back on that owner's event. No tick.
- **R2 (`UStageSceneComponent`, `FStageSceneStore`, `UStageSessionSubsystem`).**
  - **Kept items:** skipped unknown or refused items are kept as snapshots and appended to every save and autosave (`AppendUnplacedItems`; placed ids win) until another scene is loaded.
  - **Unsaved after a partial load:** a load that skipped or repaired anything now calls the new `MarkUnsaved` instead of `MarkClean`. The message lists the counts and says the stage is unsaved.
  - **Damaged entries:** they cannot be kept. The first save over that scene's file copies it to `<Name>.json.bak` (`BackupScene`), and refuses to save if the copy fails.
  - **Imports first:** loads are refused with the new `ImportsPending` result while `UStageMeshImportSubsystem::HasImportsInProgress()`: the list is still being read, or files are queued.
- **R6 (`AModularBaseActor` and subclasses).**
  - **No package loads:** `RestoreSnapshotState` reads with `bLoadIfFindFails = false`, so saved state can never load a package; references resolve only to loaded objects.
  - **Repairs:**
    - Labels are capped at 256 characters (also for inspector edits).
    - Fixture attributes are made finite, clamped and stepped as live writes are; the patch is clamped as `SetPatch` does; a negative fixture ID becomes automatic.
    - Audio gain, delay and splay are kept in range. A splay above a swapped-in cabinet's maximum is still clamped, but not counted as damage.
    - A broken span path counts as a repair.
  - **Counting:** each repair is logged (`ReportStateRepair`) and counted (`GetStateRepairCount`). Scene loads add the counts to their message and stay unsaved.
- **Bug found by the R6 test: the DMX patch was never saved.**
  - `FStageDMXPatch`'s Universe and Address were not `SaveGame`. SaveGame serialization skips such struct members, so undo snapshots and scene files stored an empty patch, and every load or undo re-auto-patched the fixture.
  - Existing scene files confirm it: their state shows `StructProperty StageDMXPatch … None`.
  - The fields are now `SaveGame`. Patches saved from now on round-trip. Older files still have no patch to recover.
- **R8 (`StageAtomicFile`, new; `StageMeshImport::LoadImportListFile`, new).**
  - **Damaged list:** an import list that does not parse is moved to `Imports.json.bak` before anything can overwrite it.
  - **Unreadable list:** one that exists but cannot be read leaves the subsystem `ReadOnly` for the session; new imports work but are not remembered.
  - **Durable writes:** every save (scenes, autosaves, layouts, the session, the import list, CSV/PDF reports, render PNGs) now flushes the temporary file to disk (`Flush(true)`) before the rename. A power cut can no longer leave a renamed but empty file.
  - Three duplicated writers were removed: `FStageLayoutStore::WriteFileAtomic`/`MoveAside`, the render writer and `StageReportFiles::WriteBytesAtomic`.
- **Unity-build fix:** the new files changed unity grouping, so the PDF composer's file-scope `RowHeight` now hid a local in the Environment panel (C4459). The panel locals were renamed.
- **Dev console commands** (compiled out of Shipping): `StageCraft.Input.List`, `.Bind <Command> <Chord|None> [1|2]`, `.Reset [Command|All]`, `.Look <Sensitivity> [Invert]`.

**Files changed**
- **New:**
  - `Docs/ADR/0013-preferences-key-bindings-and-audit-fixes.md`
  - `Scripts/Content/create_preferences_content.py`
  - `Content/StageCraft/UI/Panels/DA_Panel_Preferences.uasset`
  - `Source/ModularSceneBuilder/Public/Input/StageKeyBindings.h`, `Private/Input/StageKeyBindings.cpp`
  - `Source/ModularSceneBuilder/Public/Input/StageInputPreferencesSubsystem.h`, `Private/Input/StageInputPreferencesSubsystem.cpp`
  - `Source/ModularSceneBuilder/Private/Input/StageInputConsoleCommands.cpp`
  - `Source/ModularSceneBuilder/Public/UI/StagePreferencesPanel.h`, `Private/UI/StagePreferencesPanel.cpp`
  - `Source/ModularSceneBuilder/Private/IO/StageAtomicFile.h/.cpp`
  - Tests: `Private/Input/Tests/StageKeyBindingsTests.cpp`, `Private/IO/Tests/StageAtomicFileTests.cpp`, `Private/Actors/Tests/StageRestoredStateTests.cpp`
- **Input and settings:** `Public/Player/ModularPlayerController.h`, `Private/Player/ModularPlayerController.cpp`, `Public/Settings/StageCraftUserSettings.h`, `Private/Settings/StageCraftUserSettings.cpp`
- **UI and workspace:** `Public/UI/StageStatusBarWidget.h`, `Private/UI/StageStatusBarWidget.cpp`, `Private/UI/StageEnvironmentPanel.cpp`, `Private/Workspace/SStageWorkspaceMenuBar.cpp`, `Public/Workspace/StageWorkspaceTypes.h`, `Private/Workspace/StageWorkspaceTypes.cpp`, `Private/Workspace/StageLayoutStore.h`, `Private/Workspace/StageLayoutStore.cpp`, `Private/Workspace/StageWorkspaceSubsystem.cpp`
- **Scenes and session:** `Public/Scene/StageSceneTypes.h`, `Public/Scene/StageSceneComponent.h`, `Private/Scene/StageSceneComponent.cpp`, `Private/Scene/StageSceneStore.h`, `Private/Scene/StageSceneStore.cpp`, `Public/Subsystems/StageSessionSubsystem.h`, `Private/Subsystems/StageSessionSubsystem.cpp`
- **Actors and show:** `Public/Actors/ModularBaseActor.h`, `Private/Actors/ModularBaseActor.cpp`, `Public/Actors/LightingFixtureActor.h`, `Private/Actors/LightingFixtureActor.cpp`, `Private/Actors/AudioEquipmentActor.cpp`, `Private/Actors/StageSpanActor.cpp`, `Public/Show/ShowControlTypes.h`
- **Import, reports, render:** `Public/Import/StageMeshImportTypes.h`, `Private/Import/StageMeshImportTypes.cpp`, `Public/Import/StageMeshImportSubsystem.h`, `Private/Import/StageMeshImportSubsystem.cpp`, `Private/Report/StageReportFiles.h`, `Private/Report/StageReportFiles.cpp`, `Private/Render/StageRenderSubsystem.cpp`
- **Tests changed:** `Private/UI/Tests/StageUITests.cpp` (hints from the key map, remapped keys), `Private/Scene/Tests/StageSceneStoreTests.cpp` (new `Scene.PartialLoad`), `Private/Workspace/Tests/StageLayoutStoreTests.cpp`
- **Docs:** `Docs/CURRENT_SYSTEMS.md`, `Docs/PROJECT_AUDIT.md`, `Rules.md` (the state table now names the local-player subsystem), `tasks/todo.md`, this file
- **Not changed:** no Blueprints, maps, input assets or plugins. Config is unchanged: the panel asset is cooked by the existing `StagePanel` Asset Manager rule.

**Verification (Claude, 2026-10-21)**
- **Builds (UE 5.8):** Editor Development and Game Shipping (`-DisableAdaptiveUnity`): 0 warnings, 0 errors.
- **Package:** `BuildCookRun` Shipping: 1082 packages, 0 errors, 0 warnings. The pak contains `DA_Panel_Preferences`; the Shipping exe has no `StageCraft.Input.*` command strings. The package ran for 25 s and closed cleanly.
- **Automation:** 69/69 StageCraft tests passed. New tests:
  - `Input.KeyBindings`:
    - The defaults pass their own rules.
    - Exact lookup: Ctrl+Z is Undo, Ctrl+Shift+Z is Redo, plain Z is nothing, W is the Move tool.
    - Every refusal kind.
    - A command may share a fly key.
    - Clear, Reset with a taken default, ResetAll, descriptions, chord parsing.
  - `Input.SavedBindings`: override round trip; a swap loads in either order; 6 damaged ini entries are dropped and counted; the settings fields clamp, refuse NaN and reset.
  - `IO.AtomicFile`: content, no .tmp left, KeepExisting, UTF-8 with the text's own BOM, bytes, MoveAside.
  - `IO.ImportListFile`: Missing, Read, Damaged (moved to .bak byte for byte), Unreadable (locked file; the list is untouched).
  - `Scene.PartialLoad`: kept items are appended once; a 3-item file still holds 3 items after a save of 2 placed; backup byte for byte and never listed as a scene.
  - `Scene.RestoredState` (world test):
    - A clean fixture restores with 0 repairs, and its DMX patch comes back (regression for the SaveGame bug).
    - A tampered fixture gets its NaN dimmer defaulted, zoom clamped, gobo stepped, universe clamped, ID made automatic and label shortened: 6 repairs counted.
    - Audio NaN gain, 5000 ms delay and −3° splay are repaired: 3 counted. A cabinet swap is clamped but not counted.
  - `UI.StatusHint` / `UI.SpanHint`: the hints follow remapped and unbound keys.
  - The first run caught the patch bug.
- **Scripted Standalone runs** (`-game -windowed`; logs `Saved/Logs/R40_RunA`–`D.log`):
  - **Run A (R8 + R2):**
    - A truncated `Imports.json` was moved to `.bak` and 0 models were restored.
    - Loading "R40 Partial" (a copy of R37 Test with one item pointing at a missing asset and one item with a broken location) placed 4, kept 1 and reported 1 damaged. The stage was marked unsaved.
    - The save wrote 5 items, "including 1 not placed", and kept `R40 Partial.json.bak`; the stage became clean.
    - Re-loading still found the unknown item in the file.
    - The user's real `Imports.json` was restored afterwards.
  - **Run B:**
    - With the real import list (AG1.FBX takes about 50 s to re-import), both scene loads at 1 s and 30 s were refused with "Imported models are still loading".
    - Binding SelectAll to Ctrl+U was refused ("already used by Undo"), and so was FlyUp Ctrl+E (no modifiers).
    - E set the Rotate tool and G hid the grid.
    - This run exposed the modifier-timing bug: every Ctrl/Alt chord failed.
  - **Run C (after the fix):**
    - Ctrl+A selected all 4 items, Delete removed them, and Ctrl+Z restored them.
    - After `Bind Undo Ctrl+U`, Ctrl+Z did nothing and Ctrl+U ran Undo.
    - Ctrl+Shift+Z redid "Delete 4 items", and Ctrl+Y (nothing left) reported NothingToDo.
    - Alt+J switched to Top, Alt+G back to Perspective, and R set the Scale tool (screenshot `ScreenShot00038`).
  - **Run D:** the Preferences panel opened in its own window. A desktop capture, cropped to it, shows the sections, the bindings and RESET disabled on defaults.
  - The runs changed the user's preferences: the bindings were reset by the script, and GameUserSettings.ini was restored from a copy taken before the runs.
- **Not verified by a person:**
  - The Preferences panel by mouse: clicking a slot and pressing keys, CLEAR, RESET, RESET ALL, the refusal texts, look speed and invert while flying.
  - Edit menu labels after a rebind.
  - Real-keyboard chords.
  - Opening a scene that references a missing imported model.
- Status: **implemented, awaiting manual verification.**

**Commit:** `7e23626`.

**Known issues / follow-ups**
- **Kept items are invisible:** nothing lists them (only the load and save messages), and Clear Stage does not discard them. An Outliner group with a discard action is the follow-up.
- **Damaged entries** are not carried; they survive only in the `.bak` copy.
- **Old scene files** have no DMX patch to recover (the SaveGame bug); fixtures in them are auto-patched on load, as before.
- **Mouse buttons are not remappable:** the viewport's cursor capture and the gizmo depend on them.
- **Keyboard commands no longer run while the right mouse button is held** (before, Delete and Ctrl+Z did).
- **Start-up model re-import** still blocks the game thread for tens of seconds (AG1.FBX); scene loads now wait for it instead of losing items.
- **From #39:** the render-thread limit at 500 fixtures, the per-attribute broadcasts during fades, and the cable schedule follow-ups.
- **Audit risks still open:** R1 and R9–R14.

---

## #41 — Shopping & assembly list: measured cable runs, stock lengths with safety margin, distros, DMX parts, clamps and safety bonds in the cable schedule PDF + CSV (2026-10-21)

**What & why**
- **Request (Gevor, 2026-10-21, "Step 1: Detailed Shopping & Cable Assembly Export"):**
  - real 3D distances between fixtures, power distros and control nodes;
  - stock cable lengths (2/5/10/15/20 m) with a configurable +10 % margin, grouped ("XLR 5-pin 10 m — 12x");
  - clamps, safety bonds and power distro units scaled to fixture count, wattage and weight;
  - a "Shopping & Assembly List" section in the PDF and CSV under File › Production.
  - Design: `Docs/ADR/0014-shopping-and-assembly-list.md`.
- **Before:** power home-runs had no length ("site"). DMX links were the straight line × 1.2 on a fixed 1–50 m list, with the first link "site" and anything over 50 m "split". There was no hardware and no count by length.
- **Measuring** (`Report/StageShoppingList`, pure, any thread):
  - Route = horizontal distance + height difference (along the floor or truss, then up or down a drop).
  - Required = route × (1 + margin).
  - Then the shortest stock length that covers it. A longer run is N × the longest stock plus the shortest covering tail, printed as "2 x 20 m + 5 m" and flagged "Joined cables".
  - Coordinates are clamped to ±10 km and NaN reads 0, so one run's cable count stays bounded.
- **Positions:**
  - The distros stand together at the centre of the powered equipment on the floor (Automatic), or at a Fixed position from Project Settings.
  - The DMX control node stands at the centre of the patched fixtures, or at a fixed position.
  - Every power run (distro → load) and DMX run (node → first fixture, then fixture → fixture) is now measured.
- **Distros:**
  - Circuits (planned as before) become outputs of distros in circuit order. A new distro opens when the outputs (12) are used or a phase of the feed (63 A) would be exceeded.
  - Cables name their distro ("PD1 C03 / L2"). A new "Distros" table shows circuits and the highest phase current.
- **Rigging:**
  - A fixture is rigged when it is at least 2 m high (world Z) or tilted more than 30° from upright.
  - Clamps: ⌈weight / 20 kg⌉, at least 1. One safety bond each, the lightest rated for the weight (10/25/50/100 kg).
  - Heavier than every bond → "Secondary suspension, rigger to specify" (bold) plus a warning. Missing weight → 1 clamp and the lightest bond, plus a warning.
  - The rigged fixture weight is totalled.
- **DMX:** one node output and one terminator per universe.
- **Shopping list:** printed first after the warnings in the PDF, and as its own block in the CSV (Section, Item, Size, Quantity, Note), with cable totals.
- **Settings:** `UStageCablingSettings` (Project Settings › Game › StageCraft Cabling & Rigging, DefaultGame.ini). `ToRules()` sanitizes everything:
  - bad list entries are dropped, and lists sorted and de-duplicated;
  - an empty list or a blank name takes the default.
- **Build fix during the work:** `StageShoppingList::Build` was first declared with `const struct FStageCableSchedule&` inside the namespace. That declared a new namespaced type (C2027), the documented pitfall. It is now declared in `StageCableSchedule.h` after the type.
- No tick, no polling, no new UObject members. The subsystem still gathers plain values on the game thread and builds and writes on a worker.

**Files changed**
- New:
  - `Source/ModularSceneBuilder/Public/Report/StageShoppingList.h`, `Private/Report/StageShoppingList.cpp`
  - `Public/Report/StageCablingSettings.h`, `Private/Report/StageCablingSettings.cpp`
  - `Private/Report/Tests/StageShoppingListTests.cpp`
  - `Docs/ADR/0014-shopping-and-assembly-list.md`
- Modified:
  - `Public/Report/StageCableSchedule.h`:
    - `FStagePowerLoad::LocationMeters`, `FStagePowerCircuit::DistroNumber`, `FStagePowerDistro`;
    - `FStageCableRun::Cable` replaces `StockLengthMeters`, with the distance now the route;
    - schedule `Cabling`, anchors, `Distros`, `Shopping`;
    - removed `OverlongDmxLinks`, `DmxRoutingSlack`, `GetStockLength`;
    - `Build` takes fixtures and rules; `StageShoppingList::Build` is declared here.
  - `Private/Report/StageCableSchedule.cpp`:
    - `AssignDistros`, `ResolveAnchors`, `MeasureRun`;
    - new warnings;
    - CSV and PDF sections (shopping list, distros, route and cables columns).
  - `Public/Report/StageCableScheduleSubsystem.h`, `Private/Report/StageCableScheduleSubsystem.cpp`: gather positions, fixture weight and up vector; read `UStageCablingSettings`; new export message.
  - `Private/Workspace/SStageWorkspaceMenuBar.cpp`: menu entry "Export Cable Schedule, Power & Shopping List (PDF + CSV)" and its tooltip.
  - `Private/Report/Tests/StageProductionReportTests.cpp`: `BuildPlan` helper, DMX test rewritten for measured and joined runs, new CSV and PDF expectations.
  - `Docs/ADR/0012-production-reports-and-fixture-benchmark.md` (superseded note), `Docs/CURRENT_SYSTEMS.md`, `tasks/todo.md` (Phase 18), `STATE.md` (#40 hash `7e23626`, this entry).
- No content assets, Blueprints, maps or input assets changed.

**Verification**
- Editor build: 0 warnings. Game **Shipping** build (`-DisableAdaptiveUnity`): succeeded, 0 warnings.
- Automation `StageCraft`: **71/71 pass**, including the new `Report.ShoppingList.Rules` and `Report.ShoppingList.Build`.
- Scripted `-game` run (Claude, 2026-10-21): loaded "R38 Cues" (2 moving heads), then `StageCraft.Report.ExportCables`.
  - It wrote `Saved/StageCraft/Reports/CableSchedule_R38_Cues_20261021-184408.pdf/.csv`.
  - The shopping list showed:
    - PowerCON TRUE1 5 m 2x;
    - XLR 5-pin DMX 5 m 1x and 10 m 1x;
    - 1 distro;
    - 1 node output, 1 terminator;
    - no rigging (both fixtures stand on the floor).
  - The PDF read back cleanly over 2 pages (title block, shopping table, distros, measured routes).
  - The user's `Imports.json` and GameUserSettings.ini were compared after the run and are unchanged.
- **Not verified by a person:**
  - the menu entry on a rigged stage;
  - the Project Settings page;
  - whether the counts match a real rental pick list.
- Status: **implemented, awaiting manual verification.**

**Commit:** `80f055d`.

**Known issues / follow-ups**
- **Positions are assumed:** automatic means the centre of the equipment on the floor. Placeable distro and DMX node items (and per-distro placement near its loads) are the follow-up. The PDF prints the assumed positions.
- **Not modelled:** TRUE1 daisy-chains (still one home-run per load), truss and speaker rigging, mains feed cables, spares.
- **Rigged is decided by height (world Z ≥ 2 m) or tilt;** a fixture on a high deck counts as rigged.
- The phase imbalance warning for very small rigs (2 loads → 100 %) is unchanged from #39.

---

## #42 — Sketch to Stage: 2D drawing window and procedural 3D generation (decks, truss, fixtures), one undo step (2026-10-21)

**Request (Gevor, 2026-10-21, "Step 2: Drawing Window & Interactive 2D Sketch-to-3D Reconstruction").** A dockable 2D canvas (freehand, polygon / stage shape, straight truss line, fixture markers; smoothing; erase and clear); a generator subsystem (closed polygons → decks/risers with a height, lines → truss spans with corner joints, points → fixtures on grid-snapped positions); opened from the main menu and a toolbar button; generation undoable through the existing history; TObjectPtr, no tick, tests, 0 warnings. Mid-task (2026-10-21) Gevor asked for the window to open **as its own pop-up window, like Unreal Engine's floating windows**, instead of being docked into the main layout. Design: `Docs/ADR/0015-sketch-to-stage.md`.

**Housekeeping first.** #41 said "uncommitted"; it carries `80f055d`.

**What changed and why**
- **No deck item existed.** No actor could take an arbitrary floor outline, so closed shapes had nothing to become. New:
  - `UStageDeckData` (catalog): default footprint, default/min/max height, height step, panel size (the quantity unit), surface and skirt materials (Game bundle). Specs are per panel.
  - `AStageDeckActor`: SaveGame `Outline` (actor-local, cm) and `DeckHeight`, so undo snapshots and scene files carry decks with **no scene format change** (still v3). A `UProceduralMeshComponent` builds the prism: a top triangulated by ear clipping, and flat-shaded skirt walls with outward normals (no bottom face). Collision is query-only on the stage item channel, so decks can be selected and items placed on them.
  - Quantity = ⌈area ÷ panel area⌉, so session limits and the BOM count rental panels. Decks do not scale.
  - Inspector "Deck" section: Height (`Param_Deck_Height`; in `IsShapeParameter`, so it is undoable and refused while locked), plus Area, Panels and Corners (read-only).
  - Restored values are untrusted. A self-crossing, too-small or non-finite outline becomes the default footprint, and an out-of-range height is clamped. Each repair is counted (`ReportStateRepair`).
  - Triangle winding follows the engine's own face normal, `(P1 − P2) × (P0 − P2)` (`UKismetProceduralMeshLibrary`). Every triangle is checked against its intended normal, and the tests assert it.
- **Pure models (no world):**
  - `StageDeckMath`: signed area, centroid, simple-polygon test, point in polygon, ear clipping, outline sanitizing, rectangle, panel count, mesh.
  - `StageSketchMath`: lazy-brush stabiliser, iterative Ramer–Douglas–Peucker, grid snap, distance to a stroke, closed-gesture test, stroke and settings sanitizing. Limits: 256 strokes, 512 points, ±100 m.
  - `StageSketchGenerator::BuildPlan` (deterministic):
    - Deck corners snap to the grid and the outline is sanitized; the origin is at the centroid on the floor, and the height is clamped to the item.
    - Truss-tool points of a whole-section truss snap to the **section length** (2 m for F34), so a drawn rectangle has whole sides and closes; freehand curves snap to the grid. Truss is laid out with `StageSpanMath::ComputePathLayout` exactly as the span actor will normalise it.
    - A fixture within 75 cm of a planned truss is projected onto it, its distance along the leg snapped, and **hung** (truss height, rolled 180°). Otherwise it stands on the **highest deck** that contains it, otherwise on the floor. Duplicates are skipped.
    - Every skip is a user-facing warning: a crossing shape, a clamped height, a loop that cannot close, a path cut by the section cap, a duplicate, a missing catalog item.
- **Generator subsystem `UStageSketchSubsystem` (UWorldSubsystem, Game/PIE):**
  - It is the source of truth for the sketch (strokes, settings, the catalog item chosen per element), so closing, reopening or re-docking the window keeps the drawing.
  - Validated setters fire `OnSketchChanged`; `MakePlan` plans the current sketch.
  - `RequestPlanAssets` streams the chosen items' Game bundles, with one generation in flight; the handle is cancelled in `Deinitialize`.
  - It never ticks, never iterates the world and never changes the stage.
- **Generation = request bridge, all or nothing, one undo step:**
  - `AModularPlayerController::RequestGenerateFromSketch` refuses during a gizmo drag or flight, while a generation is pending, or for an empty plan (the reason goes to the status bar).
  - Once the assets are loaded, `StageSketchBuilder::Execute` checks each element with the GameMode's placement preview (not reported) and spawns it through `USpawnSystemComponent::SpawnConfiguredItem` / `SpawnItem` (the validator asks the rules again with the real quantity).
  - **If any element is refused, every item from that request is deleted again**, and "Nothing was generated: <reason>" is reported.
  - On success, `UStageEditHistoryComponent::RecordPlacements` records one `FStageBatchCommand` of `FStagePlaceItemCommand`s ("Generate from Sketch (N items)"). Undo removes everything; redo restores it through the rules.
- **Window:**
  - `UStageSketchCanvasWidget` (panel, code-built, `DisableNativeTick`):
    - tool segment (FREEHAND / STAGE SHAPE / TRUSS LINE / FIXTURE / ERASE), UNDO STROKE / CLEAR / FRAME, and a hint line per tool;
    - DECK and TRUSS height sliders, GRID 25/50/100 cm, SNAP, SMOOTH OFF/LOW/HIGH;
    - the item per element (click for the next candidate; long names end in an ellipsis, with the full name in the tooltip);
    - a live "Will build: …" line with the warnings, the last result, and GENERATE 3D.
  - `UStageSketchSurfaceWidget` (the canvas):
    - a top view like the viewport's Top projection (up +X, right +Y), with the grid, axes, filled deck shapes labelled with their height, truss lines, fixture markers, the stroke in progress with a rubber band, the snapped cursor and its coordinates;
    - freehand drag (stabilised and simplified), clicks for shapes and truss (first point, double-click or Enter finishes; Backspace, Esc), erase by click, wheel zoom about the cursor, right/middle drag pan;
    - it repaints only on input or `OnSketchChanged`.
  - **Pop-up window (Gevor's mid-task request):** Sketch to Stage is not in the default layout and opens as its own floating window, **1100 × 820** (`FTabManager::RegisterDefaultTabWindowSize` in `FStageWorkspaceShell::RegisterPanelSpawners`; Slate's fallback was 1000 × 600). It can still be docked by dragging its tab.
- **Entry points:** Edit › Draw › Sketch to Stage..., the Window menu, and a SKETCH button as the sixth section of the viewport toolbar.
- **Content** (headless, `Scripts/Content/create_sketch_content.py`): `DA_Panel_Sketch`, `M_DeckSurface`, `M_DeckSkirt`, and `DA_Deck_Riser` (2 × 1 m panels, 20–200 cm, 28 kg per panel; also placeable from the Library under Stage / Deck).
- **Dev console** (compiled out of Shipping): `StageCraft.Sketch.Add Freehand|Shape|Truss|Fixture x,y … [closed] [h=cm]` (x:y inside -ExecCmds), `Status`, `Set`, `Clear`, `UndoStroke`, `Generate`, and `Screenshot` (saves the Sketch window through Slate wherever it is, and logs its size).
- **Bug found in the scripted run and fixed:** the dev `Sketch.Add … closed` appended `Points[0]` to the same array (container aliasing assert in Standalone). Only the test command was affected.
- **Bug found in the screenshots and fixed:**
  - the canvas background drew white: a rounded-box brush painted directly ignored its fill colour, so it now uses the white brush tinted with the theme;
  - item names ran into the next label: they are now clipped with an ellipsis.

**Files changed**
- New:
  - `Docs/ADR/0015-sketch-to-stage.md`
  - `Scripts/Content/create_sketch_content.py`
  - `Content/StageCraft/UI/Panels/DA_Panel_Sketch.uasset`, `Content/StageCraft/Decks/M_DeckSurface.uasset`, `M_DeckSkirt.uasset`, `Content/StageCraft/Items/DA_Deck_Riser.uasset`
  - `Source/ModularSceneBuilder/Public/Sketch/StageSketchTypes.h`, `StageSketchMath.h`, `StageSketchGenerator.h`, `StageSketchSubsystem.h`, `StageSketchBuilder.h`
  - `Private/Sketch/StageSketchTypes.cpp`, `StageSketchMath.cpp`, `StageSketchGenerator.cpp`, `StageSketchSubsystem.cpp`, `StageSketchBuilder.cpp`, `StageSketchConsoleCommands.cpp`, `Tests/StageSketchTests.cpp`, `Tests/StageSketchWorldTests.cpp`
  - `Public/Placement/StageDeckMath.h`, `Private/Placement/StageDeckMath.cpp`
  - `Public/Data/StageDeckData.h`, `Private/Data/StageDeckData.cpp`, `Public/Actors/StageDeckActor.h`, `Private/Actors/StageDeckActor.cpp`
  - `Public/UI/StageSketchCanvasWidget.h`, `Private/UI/StageSketchCanvasWidget.cpp`, `Public/UI/StageSketchSurfaceWidget.h`, `Private/UI/StageSketchSurfaceWidget.cpp`
- Modified:
  - `Public/Data/StageParameterTypes.h`, `Private/Data/StageParameterTypes.cpp` (`FeatureGroup_Deck`, `Param_Deck_*`; Height in `IsShapeParameter`)
  - `Private/UI/StageCraftUITheme.cpp` (deck group colour)
  - `Public/History/StageEditHistoryComponent.h`, `Private/History/StageEditHistoryComponent.cpp` (`RecordPlacements`)
  - `Public/Player/ModularPlayerController.h`, `Private/Player/ModularPlayerController.cpp` (`RequestGenerateFromSketch`, `HandleSketchAssetsReady`)
  - `Public/Workspace/StageWorkspaceTypes.h`, `Private/Workspace/StageWorkspaceTypes.cpp` (`Panel_Sketch`)
  - `Private/Workspace/StageWorkspaceShell.cpp` (pop-up window size)
  - `Private/Workspace/SStageWorkspaceMenuBar.cpp` (Edit › Draw › Sketch to Stage...)
  - `Public/UI/StageViewportToolbar.h`, `Private/UI/StageViewportToolbar.cpp` (SKETCH section)
  - `Docs/CURRENT_SYSTEMS.md`, `tasks/todo.md` (Phase 19), this file
- No maps, Blueprints, input assets, plugins or config changed. The panel and deck item are found by the existing `StagePanel` / `StageItem` Asset Manager rules.

**Verification (Claude, 2026-10-21)**
- **Builds (UE 5.8):** Editor Development, Game Development and Game Shipping (`-DisableAdaptiveUnity`) all succeeded with 0 warnings in project code. The only warnings in the logs are C4305 inside the third-party Sky Creator plugin's own module (`SkyCreatorWeatherSettings.h`), which recompiled; they are not from this change. The Shipping exe contains no `StageCraft.Sketch.*` command strings.
- **Automation:** 75/75 `StageCraft` tests passed (71 + 4 new):
  - `Sketch.Math`: stabiliser, RDP, snap, segment and stroke distance, freehand loop → deck and open line → truss, bad points, caps, settings clamps.
  - `Sketch.Deck`: area and orientation, bow tie and spike, convex/concave/clockwise triangulation covering the exact area, outline repair, panel counts, mesh faces by the engine formula, the −Y wall facing −Y.
  - `Sketch.Plan`: 32 m² deck → 16 panels at the centroid, a closed 14-section truss loop from off-grid clicks, a hung fixture at (400, −1000, 600) rolled 180°, a fixture on the 60 cm deck top, a floor fixture, duplicate / bow tie / clamped-height warnings, an unsnapped loop that cannot close, missing items, determinism.
  - `Sketch.Generate` (real game world): 4 items placed; deck 8 m², 80 cm, 4 panels, 10 triangles; truss 4 sections at 6 m; one undo removes all and redo restores all with no repairs; a refused 4th element leaves the stage unchanged with the rule's reason; a missing item spawns nothing; a bow-tie outline is repaired to the default footprint.
- **Scripted Standalone `-game` runs** (`Saved/Logs/R42_Run2–6.log`):
  - Run 2: a 6 × 10 m deck at 1 m, a 3 × 4 m riser at 40 cm, an 8 × 12 m truss loop, a freehand curve, 3 fixtures by the truss and 2 on the deck. The plan read "2 decks (36 panels), 2 truss runs (25 sections), 7 fixtures"; Generate placed all 11 (hung fixtures at Z 600, deck fixtures at Z 100) and the BOM counted 36 deck units (1008 kg). Undo removed all 11 as "Generate from Sketch (11 items)", and redo restored them. The scene was saved as "R42_Sketch" and reloaded: 15 placed, 0 refused, 0 repaired, decks intact.
  - Screenshots `ScreenShot00080` (Top) and `00081` (Perspective): solid decks with outward-facing walls (winding correct), fixtures on the deck, the truss loop with hung fixtures, the curved truss, and the SKETCH toolbar button.
  - Runs 5 and 6: `StageCraft.Sketch.Screenshot` → the window "Sketch to Stage" opened **floating, 1106 × 826**, in front of the main window (`Saved/Screenshots/Sketch_215131.png`, `Sketch_215358.png`): dark canvas, filled deck shapes with heights, truss, markers, the plan line, the result message and the self-crossing warning.
  - Gevor's workspace session was backed up and restored around every run (compared byte for byte).
- **Not verified by a person:**
  - drawing with the real mouse and keyboard: freehand drag, shape and truss clicks, double-click / Enter / Backspace / Esc, erase, wheel zoom and right-drag pan;
  - the buttons, sliders and item choice;
  - GENERATE 3D, then Ctrl+Z / Ctrl+Shift+Z from the keyboard;
  - deck Height in the inspector;
  - placing `DA_Deck_Riser` from the Library;
  - docking and undocking the pop-up window.
- Status: **implemented, awaiting manual verification.**

**Commit:** `2e97ef7`.

**Known issues / follow-ups**
- The sketch is not saved in scene files and is dropped on level change. There is no 3D preview of the plan before generating.
- Truss legs are level at one height (the Truss slider). Towers, motors and ground supports are not generated, so a generated truss floats.
- A deck's outline cannot be edited after generating: redraw it, Ctrl+Z, generate again. Deck legs, stairs and handrails are not modelled.
- Generating twice builds twice: the sketch is kept on purpose so it can be corrected; undo the previous generation first.
- A fixture on a deck 2 m or higher counts as rigged in the cable schedule (existing rule, ADR 0014).
- A Sketch window opened in an earlier layout is restored where it was; Layout › Reset closes it again.

## #43 — AI Assistant: chat panel and natural-language commands through the stage's rules and undo history (2026-10-22)

**Request (Gevor, 2026-10-21, "Step 3: AI Conversational Assistant & Natural Language Command Subsystem").** A dockable or pop-up chat panel (`UStageAIAssistantWidget`) for questions about the stage (equipment counts, power loads, cue lists). A `UStageAICommandSubsystem` that maps requests to safe actions (spawn fixtures, set up cues, weather, export reports) through the existing transaction and rule checks, and answers scene statistics (weight, power, fixture counts) clearly. TObjectPtr, encapsulation, no per-frame tick, all tests, 0 warnings, STATE.md. Design: `Docs/ADR/0016-ai-assistant.md`.

**Housekeeping first.** #42 said "uncommitted"; it carries `2e97ef7`. The `Rules.md` edit from the end of #42 (commit commands are one `-m`, never `Co-Authored-By:` / `Claude-Session:` trailers; Gevor 2026-10-21) was not in that commit and is staged with this one.

**What changed and why**
- **The model never acts; commands do (ADR 0016 §2.1).** Everything the assistant can do is a closed set of commands, `FStageAICommand` (`AI/StageAITypes.h`): an `EStageAIIntent` with bounded parameters.
  - Text becomes a command in two ways:
    - the **local parser** `StageAICommandParser::Parse`: a deterministic, offline English grammar, always available and the default;
    - the optional **online model** `UStageClaudeAssistantBackend`, which sends the prompt to the Claude Messages API with one strict `stage_command` tool mirroring the command.
  - Whichever produced it, a command passes `StageAICommandSchema::Validate` before it runs:
    - the intent is known;
    - counts are 1–24 fixtures; cue numbers 0.001–9999.999 (rounded like the cue list); fades 0–600 s; hours 0–24;
    - strings are trimmed and capped; unknown tool fields are rejected.
  - A model command that fails validation is reported and never re-interpreted. Model output is treated as untrusted input (§20, §66).
- **What it understands (§2.3):**
  - questions: stage summary, item count, total weight and power (against the session limits when there are any), fixtures by name, items matching a name ("how many truss"), the cue list with the active cue, and the environment;
  - actions:
    - add N fixtures by name or type ("add 6 moving heads", "place a led par", "a couple of pars"; slang such as movers or par cans);
    - store a cue (number, label, fade; with no number, the next whole number after the last cue);
    - go, back, go to cue N;
    - weather presets, time of day ("18:30", "6 pm", "sunset"), sky on or off;
    - export the bill of materials, patch sheet, cable schedule or all three;
    - undo and redo.
  - Destructive whole-stage operations (clear, delete, open, save) are deliberately **not commands**: the assistant says where they are in the menus, which ask first.
- **`UStageAICommandSubsystem` (UWorldSubsystem, Game/PIE):**
  - It is the source of truth for the conversation, capped at 200 lines, per stage session and not saved. One prompt is in flight at a time (`EStageAIAssistantState` Idle / Interpreting / Acting); a second prompt is refused, not queued.
  - Questions are answered read-only from the session, show control and environment subsystems (`CaptureScene` → `FStageAISceneView` → `StageAIAnswers::AnswerQuestion`, which is pure and tested).
  - Actions are **never run here**. They go to the executor of the player who asked. Added fixtures stream their Game bundle first (`RequestItemAssets`); the online request and the asset load are cancelled in `Deinitialize`. Nothing ticks.
- **Actions go through the existing request bridge (§2.2, §2.4):**
  - `AModularPlayerController::RequestAssistantPrompt` is the panel's only entry point. It binds the executor weakly to the controller.
  - `StageAICommandRunner::Run` holds no rules; it only calls the controller's public requests:
    - cues: `RequestStoreCue` / `RequestGoToCue` / `RequestCueGo` / `RequestCueBack` (GameMode `EvaluateCueRequest`, one undo step each);
    - reports: `RequestExport*`;
    - history: `RequestUndo` / `RequestRedo`.
  - The new `RunCapturingRejections` lets the runner quote a refusal's own reason (for example "Test Moving Head Wash is locked. Unlock it in the shop.") while it is still broadcast to the status bar as before.
  - **Fixtures:**
    - the catalog fixture is matched by name and type, then `CanPlaceItem` asks the rules first;
    - the fixtures go in a row 1 m apart along Y, 2 m beyond the lowest X of the items already placed (X 0 on an empty stage), on the 50 cm grid;
    - they become fixture strokes for `StageSketchGenerator::BuildPlan`, so snapping and duplicate handling are the sketch's.
  - The new **`RequestBuildPlan`** places the plan through the GameMode rules, all or nothing, as one undo step ("Assistant: add 4 × …"). It holds the former body of the Sketch window's `HandleSketchAssetsReady`, which now calls it, so both paths share one implementation. It now also refuses during a gizmo drag or camera flight after the assets have loaded. Its status-bar refusal reads "Nothing was placed: …"; the Sketch window's own message is unchanged.
  - **Weather, time and sky:** the new **`RequestSetEnvironment`** writes through `UStageEnvironmentSubsystem::SetSettings` and records **`FStageEnvironmentCommand`** (whole settings before and after). `IStageItemEditor::SetEnvironment` is new (default `Invalid`, like `SetCueList`). This makes assistant weather, time and sky changes **undoable**. Weather turns the sky on when a sky system is installed. Replies describe what was stored, never what was asked for.
- **The optional online model (§2.5):**
  - Off by default: Project Settings › Game › **StageCraft AI Assistant** (`UStageAIAssistantSettings`, DefaultGame.ini). When on, the prompt, a short scene summary and the last 10 lines are sent to Anthropic; the panel footer says so. The API key is read from `ANTHROPIC_API_KEY` at request time and is never stored or logged.
  - Raw HTTP `POST /v1/messages` (new `HTTP` module dependency):
    - headers `anthropic-version: 2023-06-01` and `anthropic-beta: server-side-fallback-2026-07-01`;
    - body `model` `claude-opus-5-5`, `max_tokens` 16000, `output_config.effort` `low`, no `thinking` field (adaptive by default), `fallbacks: "default"` (a safety decline is retried server-side on a fallback model);
    - `tool_choice` `auto` with `disable_parallel_tool_use` (forced tool choice is rejected by this model);
    - plain-text history only, so no earlier turn is ever edited.
  - `stop_reason` is checked before content: a refusal is reported as a decline; a transport error, timeout, non-200 status or malformed body falls back to the local parser and says so.
- **Panel `UStageAIAssistantWidget` (`StageCraft.Panel.Assistant`, `DA_Panel_Assistant`):**
  - It opens as a floating pop-up window (default 560 × 720), like the Sketch window, from Edit › Assistant › AI Assistant..., the Window menu and the new **ASSIST** button next to SKETCH in the viewport toolbar.
  - Contents:
    - the transcript as YOU / ASSISTANT bubbles (· DONE in the accent colour, · NOT DONE outlined in the warning colour, · WORKING while fixtures load) and a "Thinking…" line while the online model answers;
    - quick chips (Stage summary, Power draw, Weight, Fixtures, Cues, Help), a text field with Enter / SEND, CLEAR;
    - a footer naming the interpreter.
  - Built in code from the theme. It binds `OnConversationChanged` in `NativeConstruct`, unbinds in `NativeDestruct`, rebuilds on the next tick (never inside the event), and has no tick. The list scrolls to the end once more on the following tick, because new rows have no size until Slate lays them out; without that the newest reply was hidden (found in Run 2's screenshot).
- **Development commands** (`#if !UE_BUILD_SHIPPING`): `StageCraft.Assistant.Ask <request>` (same path as SEND), `.Transcript`, `.Clear`, `.Screenshot` (Slate capture of the assistant window only).

**Files**
- New:
  - `Docs/ADR/0016-ai-assistant.md`
  - `Source/ModularSceneBuilder/Public/AI/`: `StageAITypes.h`, `StageAICommandParser.h`, `StageAICommandSchema.h`, `StageAIAssistantBackend.h`, `StageAIAssistantSettings.h`, `StageAICommandSubsystem.h`
  - `Source/ModularSceneBuilder/Private/AI/`: `StageAITypes.cpp`, `StageAICommandParser.cpp`, `StageAICommandSchema.cpp`, `StageClaudeAssistantBackend.cpp`, `StageAIAssistantSettings.cpp`, `StageAICommandSubsystem.cpp`, `StageAICommandRunner.h/.cpp`, `StageAIConsoleCommands.cpp`, `Tests/StageAITests.cpp`
  - `Public/History/StageEnvironmentCommands.h`, `Private/History/StageEnvironmentCommands.cpp`
  - `Public/UI/StageAIAssistantWidget.h`, `Private/UI/StageAIAssistantWidget.cpp`
  - `Scripts/Content/create_assistant_content.py`, `Content/StageCraft/UI/Panels/DA_Panel_Assistant.uasset`
- Changed:
  - `ModularSceneBuilder.Build.cs` (HTTP)
  - `Public/Player/ModularPlayerController.h`, `Private/Player/ModularPlayerController.cpp` (`RequestAssistantPrompt`, `RequestBuildPlan`, `RequestSetEnvironment`, `RunCapturingRejections`; the sketch uses `RequestBuildPlan`)
  - `Public/History/StageEditCommand.h` (`SetEnvironment`)
  - `Public/History/StageEditHistoryComponent.h`, `Private/History/StageEditHistoryComponent.cpp` (`RecordEnvironmentChange`, world editor `SetEnvironment`)
  - `Private/History/Tests/StageHistoryWorldTests.cpp` (History.Environment)
  - `Public/Workspace/StageWorkspaceTypes.h`, `Private/Workspace/StageWorkspaceTypes.cpp` (`Panel_Assistant`)
  - `Private/Workspace/StageWorkspaceShell.cpp` (floating size)
  - `Private/Workspace/SStageWorkspaceMenuBar.cpp` (Edit › Assistant)
  - `Public/UI/StageViewportToolbar.h`, `Private/UI/StageViewportToolbar.cpp` (SKETCH / ASSIST)
  - `Docs/CURRENT_SYSTEMS.md`, `tasks/todo.md` (Phase 20), `Rules.md` (commit rule, from #42)

**Verification**
- **Builds:** Editor, Game Development and Game Shipping (`-DisableAdaptiveUnity`) succeed with **0 warnings and 0 errors**, rebuilt after the last change. The Shipping exe contains no `StageCraft.Assistant.*` or `StageCraft.Sketch.*` command strings; it does contain the panel tag and the API-key variable name.
- **Tests: 80/80** (`Automation RunTests StageCraft`). New:
  - `StageCraft.AI.Parser`: about 90 phrases across every intent, including number words, a dozen / a couple, labels quoted and unquoted, 6 pm / 9pm / 12 am / 18:30 / sunset, stop the rain, destructive operations explained, an over-long prompt.
  - `StageCraft.AI.Schema`:
    - validation ranges and normalization;
    - 20 tool inputs, valid and hostile: unknown intent or field, fractional or text counts, 500 fixtures, missing weather / hour / sky, a bad report, cue 0;
    - the tool definition (strict, no additional properties, every intent listed once);
    - the request body (model, effort, fallbacks, no thinking, auto tool choice with one call, history alternating and merged, the scene and prompt in the new turn);
    - responses: tool call, invalid tool call flagged, text after a thinking block, refusal, 401 with the API message, malformed body, max_tokens, another tool's call ignored.
  - `StageCraft.AI.Answers`: fixture matching (generic, empty, wash / pars / par cans / movers / by name / none / empty catalog), the row layout (centred, beyond placed items on the grid, clamped), every answer from a scene snapshot (kW / kg / limits / cues with the active one / indoor and outdoor / empty stage), the hour format.
  - `StageCraft.AI.Conversation` (real game world, recording executor): questions without the executor, unsupported operations explained, an action executed once with the parsed command, an invalid action and a missing player never executed, a pending action blocking prompts and clear until it finishes, a second finish ignored, the 200-line cap.
  - `StageCraft.History.Environment`: undo / redo of the whole settings, the stage marked dirty, no step for no change, an open drag closed first.
- **Scripted runs** (Standalone `-game` with `StageCraft.Assistant.Ask` through the controller; Gevor's workspace session backed up and restored, compared byte for byte):
  - **R43_Run2** went through, in order:
    - status;
    - "add 4 moving heads" (placed after streaming: 6 fixtures);
    - "store cue 1 called Intro fade 3", then list cues ("Q1 Intro (3 s)");
    - "make it rain" (sky turned on), "set time to 18:30", then "whats the weather" ("Outdoor sky at 18:30, rain: clouds 95%, rain 80% …");
    - power draw 2.8 kW and weight 212 kg, matching the BOM export's 2820 W / 212.0 kg;
    - four undos, named "Assistant: time 18:30", "Assistant: weather rain", "Store Cue 1" and "Assistant: add 4 × Test Moving Head Spot" (fixtures back to 2), then redo;
    - "add 500 lights" refused ("1 to 24"), "clear the stage" explained, "add 2 lasers" refused with the available fixtures;
    - "export bill of materials" (PDF and CSV written).
  - **R43_Run3:** help; "add a couple of moving head washes" refused by the shop rules with the rule's reason (the Wash is locked); go outdoors, sunset (19:30), status; undo, undo (the sky back off at 14:00), and a third undo answered "There is nothing to undo."
  - Screenshots (Slate capture of the app's own windows only):
    - `Saved/Screenshots/Assistant_000444.png`, `Assistant_000606.png` and `Assistant_000613.png`: the floating 566 × 726 "AI Assistant" window with bubbles, captions, chips, field, SEND and footer; after the scroll fix the newest reply is visible;
    - `ScreenShot00085` (main window): the four added fixtures in a row in front of the existing gear, and SKETCH / ASSIST in the toolbar, which still fits.
  - The BOM files exported by the test run were deleted afterwards.
- **Not verified:**
  - by a person: typing in the field (Enter, SEND, focus kept), the chips and CLEAR with the real mouse, docking and undocking the pop-up, Ctrl+Z from the keyboard after assistant actions;
  - **the online model with a real API key**: none is available on this machine, so the HTTP path is covered only by the request and response tests.
- Status: **implemented, awaiting manual verification.**

**Commit:** uncommitted (working tree).

**Known issues / follow-ups**
- The online model is untested against the live API (no key here). The `server-side-fallback-2026-07-01` beta and `fallbacks: "default"` follow the current API reference. If the account rejects them, the reply falls back to the local parser and the HTTP error is quoted.
- One command per prompt: "add 4 pars and store cue 1" runs only the first rule that matches. Added fixtures stand on the floor; hanging them on truss is not supported.
- Export completion is not reported back into the chat; the status bar shows it.
- Environment panel edits are still not undoable (only assistant changes are). Undoing an assistant change restores the whole environment as it was before, including panel edits made since.
- "moving heads" picks the first matching fixture when several types tie (the Spot before the Wash); name the type ("washes") to choose.
- The conversation is not saved with scenes. The grammar is English only.
