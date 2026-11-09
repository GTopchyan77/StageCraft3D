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

**Commit:** `23b71ba`.

**Known issues / follow-ups**
- The online model is untested against the live API (no key here). The `server-side-fallback-2026-07-01` beta and `fallbacks: "default"` follow the current API reference. If the account rejects them, the reply falls back to the local parser and the HTTP error is quoted.
- One command per prompt: "add 4 pars and store cue 1" runs only the first rule that matches. Added fixtures stand on the floor; hanging them on truss is not supported.
- Export completion is not reported back into the chat; the status bar shows it.
- Environment panel edits are still not undoable (only assistant changes are). Undoing an assistant change restores the whole environment as it was before, including panel edits made since.
- "moving heads" picks the first matching fixture when several types tie (the Spot before the Wash); name the type ("washes") to choose.
- The conversation is not saved with scenes. The grammar is English only.

## #44 — Armenian & English assistant, voice prompts (MIC), and photo / scan import for Sketch to Stage (2026-10-22)

**Request (Gevor, 2026-10-22, "Multilingual (Armenian & English) Voice/Text Assistant Support and Real-World Photo/Sketch Import for Sketch-to-3D").**
1. `UStageAICommandSubsystem` and `UStageAIAssistantWidget` fully process queries and commands in Armenian, using the Claude backend when a key is present.
2. A microphone / speech-to-text hook in the assistant panel.
3. Sketch to Stage accepts PNG / JPEG drawings and floor plans; image processing extracts the structure for `StageSketchGenerator` to rebuild decks and truss.
4. `TObjectPtr`, encapsulation, no per-frame tick, all tests, 0 warnings, STATE.md.

**Gevor's decisions (asked before design, 2026-10-22):** voice through OpenAI transcription; an offline Armenian grammar plus Armenian replies when there is no key; photos read by a local edge pipeline with an optional Claude reading, landing as editable strokes before GENERATE 3D. Design: `Docs/ADR/0017-multilingual-voice-and-photo-import.md`.

**Housekeeping first.** #43 said "uncommitted"; it carries `23b71ba`.

**What changed and why**
- **Replies follow the prompt's language (ADR 0017 §2.1).**
  - `EStageAILanguage` and `StageAILanguage::Detect` (Armenian against Latin letters) are new; `FStageAICommand::Language` is the reply language.
  - Every reply has an Armenian rendition next to its English `LOCTEXT`, through `StageAILanguage::Pick`. This covers help, answers, action outcomes, validation errors, notes and "not understood".
  - Culture-invariant on purpose: the game's culture is global, while the language changes per prompt.
  - Not translated: undo step names (Edit-menu entries), catalog names, and refusals worded by the GameMode or economy (quoted as they are).
- **Offline Armenian (§2.2): `StageAIArmenian::TranslateToGrammar`** rewrites an Armenian prompt into the English grammar's words, and `StageAICommandParser::Parse` runs the same rules.
  - Stems match longest first, so endings need no rules; short words match exactly.
  - Number words and ordinals become digits, «»/"" labels are kept as typed, and `՞` or a question word makes a question.
  - Part of the day plus an hour gives a clock time (`ժամը 8 երեկոյան` → 20:00); "turn on cue 4" plays it; undo and redo move first.
  - Unknown Armenian words are kept, so `ավելացրու 2 վիշապ` is refused instead of adding the first catalog fixture.
  - `FPrompt` now treats Armenian letters as word characters (and lowercases them itself; the CRT does not).
- **Online Armenian (§2.3).** The `stage_command` tool has an optional, validated `reply_language` (en/hy; other values refused).
  - The system prompt covers Armenian (also in Latin letters), voice transcripts, and English `item` words.
  - `ParseResponseBody` and `CommandFromToolInputJson` take the prompt's language as the default.
  - The Messages API transport (endpoint, version and beta headers, key) moved to `StageClaudeHttp`, shared with the sketch reader.
- **Armenian on screen (§2.4).** No engine font has Armenian glyphs (Roboto, DroidSansFallback and NotoSansThai checked); Armenian drew as boxes.
  - `StageCraftWidgetStyle::MakeFont` now builds one composite font: the engine's Roboto plus a **Noto Sans Armenian** sub-font (Regular/Bold, SIL OFL 1.1) for U+0530..058F and U+FB13..FB17, lazily loaded.
  - The files are in `Source/ModularSceneBuilder/Resources/Fonts` with `OFL.txt`, staged by `RuntimeDependencies` (UFS). Under `Content/` the editor's auto-import would make assets of them.
- **Voice prompts (§2.5).**
  - `UStageVoiceInputComponent` is new, on `AModularPlayerController`: Idle → Recording → Transcribing → Idle, a status line, and a length-limit timer with a handle. No tick.
  - It owns `FStageVoiceRecorder`, which wraps `Audio::FAudioCapture`. The device callback only appends mono samples under a lock; Stop and Abort close the stream first.
  - It also owns `UStageOpenAITranscriptionBackend`: `POST /v1/audio/transcriptions`, multipart with a 16 kHz mono WAV, a bilingual vocabulary hint, and the language fixed or detected.
  - Pure helpers in `StageSpeechAudio`: resample (a box filter when downsampling), RMS, WAV, multipart body, response.
  - Short or silent recordings are not sent.
  - Off by default (Project Settings › StageCraft AI Assistant › Voice Input; key from `OPENAI_API_KEY`).
  - The transcript goes into the prompt field to be checked (Enter sends it), or straight through `RequestAssistantPrompt` when the settings say so.
  - The panel gets MIC / STOP / … left of the field and a voice status line. Closing the panel cancels recording.
  - The AudioCapture plugin is listed explicitly in the .uproject (it was already on by default).
- **Photo / scan import (§2.6).** `UStageSketchSubsystem::ImportImage(Path, Local | Claude)` adds what a drawing shows to the sketch as ordinary strokes: closed outlines become decks (a nested one a riser), lines become truss, dots and small circles become fixtures. The stage is not touched: the user checks the strokes and generates as before.
  - `StageSketchImageTrace` (pure, deterministic):
    1. adaptive threshold from an integral image, read inverted when the image is dark, then a 3 × 3 closing and speck removal;
    2. enclosed paper regions traced (Moore) and simplified (RDP);
    3. compact marks become fixtures;
    4. the remaining ink, less a band around each deck, thinned (Zhang–Suen) and followed end to end;
    5. the result scaled to the chosen photo width and centred on the origin.
  - Text and smudges are left out and counted. Caps: 32 decks, 64 runs, 96 fixtures.
  - File checks (`StageSketchImageImport`): .png / .jpg / .jpeg only, 64 MB, format from the content, dimensions from the header before decoding (12000 px, 60 MP), alpha onto white.
  - Threads: reading, decoding and tracing run on a `UE::Tasks` worker with a plain job struct. The result returns as a game-thread task and is applied only if its serial is current, so cancel, a newer import or the world ending drops it.
  - The photo is shown at 40 % under the grid (a transient `UTexture2D` owned by the subsystem), with SHOW / REMOVE; FRAME and a finished import include it.
  - `FStageSketchSettings::PhotoWidth` is new (5–100 m, default 20 m).
- **Optional Claude reading (§2.7).** `UStageClaudeSketchVisionBackend` sends a JPEG (≤ 1568 px) with the strict `sketch_plan` tool, in image fractions; effort is never below medium.
  - `StageSketchVision::ParseResponseBody` validates everything: unknown fields refused, bad or far-out points dropped, near ones clamped, elements with too few points dropped, counts capped, every drop counted.
  - The local reading is always made and is the fallback (decline, "not a stage plan", empty plan, network error), and the message says why.
- **Sketch window:** a PHOTO row with IMPORT PHOTO... (file dialog), REMOVE, the PHOTO WIDTH slider, READ THIS PC / CLAUDE (CLAUDE disabled with its reason when unavailable) and SHOW OFF / ON. The import's message goes on the result line.
- **Development commands** (compiled out of Shipping): `StageCraft.Assistant.Voice start|stop|cancel|status|devices`, `StageCraft.Sketch.Import <path> [claude]`, and `StageCraft.Sketch.Set PhotoWidth`. `Sketch.Status` shows the photo and the import.
- **Bug found in the scripted run and fixed:** the newest assistant reply could sit cut off below the panel. Wrapped text finds its height a frame or two after it is built, so the single extra scroll on the next tick was not enough. The list now follows its end for three ticks (`FollowEnd`).
- **Defects found by an independent review of this change and fixed before handing over:**
  - **Deck outline tracing could loop forever.** It stopped only when the start pixel was entered from the west. A start that can only be re-entered from the east (a deck drawn wide side up, a slanted top-left corner) circled until the step cap, repeating the outline. It now stops by Jacob's criterion (back at the start, about to leave it the same way). A test for a trapezoid with sides slanting 2 px per row was added.
  - **Armenian stems:**
    - `ցանկ` (list) also matched ցանկանում (I want), so "I want to add 4 spots" became a question. It is now whole-word, and ցանկան is a filler.
    - `կրկն` (redo) also matched կրկնակի (double); redo is now its imperative and infinitive only.
    - A prohibitive "մի" before a verb ("մի՛ ավելացրու" = don't add) read as "add one". Such a prompt is now not understood rather than run.
    - A part of the day in an action ("ավելացրու 4 սփոթ երեկոյան") set the time instead; it is dropped from such prompts.
    - Tests were added for each.
  - **Voice recorder:** a callback still delivered while the stream stops now finds a flag cleared under the lock and touches nothing (the earlier comment relied on the engine joining its thread).
  - **Sketch window:** a window opened after an import no longer announces the old import again.
  - **Claude reading:** the backend is made when first needed and availability is read live, so turning the online model on applies without reloading the level.

**Files changed**
- New:
  - `Docs/ADR/0017-multilingual-voice-and-photo-import.md`
  - `Source/ModularSceneBuilder/Resources/Fonts/NotoSansArmenian-Regular.ttf`, `NotoSansArmenian-Bold.ttf`, `OFL.txt` (Noto Project, SIL OFL 1.1, from notofonts.github.io; glyph coverage of U+0531..0587 checked)
  - `Public/AI/StageAILanguage.h`, `StageAIArmenian.h`, `StageSpeechTypes.h`, `StageSpeechToTextBackend.h`, `StageVoiceInputComponent.h`
  - `Private/AI/StageAILanguage.cpp`, `StageAIArmenian.cpp`, `StageSpeechTypes.cpp`, `StageSpeechToTextBackend.cpp`, `StageVoiceInputComponent.cpp`, `StageVoiceRecorder.h/.cpp`, `StageClaudeHttp.h/.cpp`, `Tests/StageAIArmenianTests.cpp`, `Tests/StageAIVoiceTests.cpp`
  - `Public/Sketch/StageSketchImageTrace.h`, `StageSketchVision.h`, `StageSketchVisionBackend.h`
  - `Private/Sketch/StageSketchImageTrace.cpp`, `StageSketchImageImport.h/.cpp`, `StageSketchVision.cpp`, `StageSketchVisionBackend.cpp`, `Tests/StageSketchImageTests.cpp`
- Changed:
  - `ModularSceneBuilder.uproject` (AudioCapture plugin listed)
  - `ModularSceneBuilder.Build.cs` (AudioCaptureCore, ImageWrapper, font RuntimeDependencies)
  - AI:
    - `Public/AI/StageAITypes.h` (`Language`), `StageAICommandParser.h` (answers take a language), `StageAICommandSchema.h` (default language), `StageAIAssistantBackend.h`
    - `StageAIAssistantSettings.h` + `Private/AI/StageAIAssistantSettings.cpp` (Voice Input)
    - `StageAICommandSubsystem.h` + `Private/AI/StageAICommandSubsystem.cpp`
    - `Private/AI/StageAICommandParser.cpp`, `StageAICommandRunner.cpp`, `StageAICommandSchema.cpp`, `StageClaudeAssistantBackend.cpp`, `StageAIConsoleCommands.cpp`
  - `Public/Player/ModularPlayerController.h`, `Private/Player/ModularPlayerController.cpp` (`VoiceInput` component)
  - Sketch:
    - `Public/Sketch/StageSketchTypes.h` (`PhotoWidth`, `EStageSketchReader`, `EStageSketchImportState`), `StageSketchMath.h` + `Private/Sketch/StageSketchMath.cpp` (photo width range)
    - `StageSketchSubsystem.h` + `Private/Sketch/StageSketchSubsystem.cpp` (import, reference photo)
    - `Private/Sketch/StageSketchConsoleCommands.cpp`
  - UI:
    - `Private/UI/StageCraftWidgetStyle.h/.cpp` (composite font)
    - `Public/UI/StageAIAssistantWidget.h` + `Private/UI/StageAIAssistantWidget.cpp` (MIC, voice line, scroll follow, Armenian welcome line)
    - `Public/UI/StageSketchCanvasWidget.h` + `.cpp` (PHOTO row)
    - `Public/UI/StageSketchSurfaceWidget.h` + `.cpp` (reference photo)
  - `Docs/CURRENT_SYSTEMS.md`, `tasks/todo.md` (Phase 21), this file
- No maps, Blueprints, content assets or input assets changed. No settings changed in DefaultGame.ini: every new option has a code default and is off.

**Verification (Claude, 2026-10-22)**
- **Builds (UE 5.8):** Editor Development, Game Development and Game Shipping (`-DisableAdaptiveUnity`) succeed with 0 warnings and 0 errors, rebuilt after the last change. The Shipping exe has no `StageCraft.Assistant.Voice`, `.Ask` or `StageCraft.Sketch.Import` command strings; it does contain the font file name, `sketch_plan`, `reply_language` and the key variable names.
- **Tests: 86/86** (`Automation RunTests StageCraft`; 80 before). New:
  - `StageCraft.AI.Armenian`:
    - script detection and lowercasing;
    - about 70 Armenian prompts across every intent: fixtures with number words, slang, Latin brand names, "a few", twelve, polite filler; an unknown fixture refused; questions with `՞`;
    - cues with «labels», ordinals and endings; go / next / back; rain, stop rain, fog, snow, cloudy; 18:30, 8 in the evening, 9 in the morning, sunset, noon; sky on and off;
    - reports, undo / redo, unsupported operations, help, nonsense;
    - every one of the 23 examples in the Armenian help text;
    - the review's cases (want, double, don't, evening);
    - Armenian replies (weight, kW, summary, not understood);
    - `reply_language` (hy, en override, default, unknown refused, refusal text in Armenian), the response default, the tool definition and the system prompt.
  - `StageCraft.AI.Voice`:
    - resampling 48→16 kHz keeps length and loudness, a 15 kHz tone is damped, up and same-rate cases;
    - RMS; the WAV header byte by byte, clamping and NaN;
    - the multipart body (field framing, Armenian as UTF-8, file part, closing boundary);
    - transcription responses: text, Armenian, 401 with the message, error on 200, not JSON, no or empty text, the length cap;
    - the settings, and the component refusing while voice input is off.
  - `StageCraft.Sketch.Image.Trace`:
    - pixel → plan mapping;
    - a deck outline (placed within 25 cm, area within 8 %, simple after sanitizing, deterministic, scales with the width);
    - the wide-side-up trapezoid; a riser one level higher; an open bent line → a truss with 3 points;
    - dots and a ring → 4 fixtures at their centres; a deck with a dot inside and a line leaving it;
    - a shadow gradient with dust; a blackboard read inverted; a handwritten "Z" left out; blank and tiny images;
    - scaling down a 1600 × 1200 image.
  - `StageCraft.Sketch.Image.Files`: PNG with transparency onto white, JPEG, garbage refused, extensions, the whole worker job on a file (preview, JPEG for the online reading), a missing file.
  - `StageCraft.Sketch.Image.Vision`:
    - the strict tool;
    - the request (image block, fallbacks, auto tool choice, effort at least medium);
    - a valid plan (height, clamping near the edge, notes, placement equal to the local reading);
    - hostile answers: bad corners and points dropped, a negative height ignored, unknown field, 120 fixtures capped;
    - a text answer, a refusal, 401, malformed.
  - `StageCraft.Sketch.Image.Import` (real game world):
    - an import through the worker: a deck, a truss and a fixture; the photo 12 m × 8 m under the canvas;
    - a second import refused while busy; hide and remove the photo with the strokes kept;
    - a cancel drops the late result; unsupported and missing files refused; Claude refused while off, with the reason;
    - a fake .jpg failing with its name.
- **Scripted Standalone `-game` runs** (logs `Saved/Logs/R44_Run1–7.log`; Gevor's workspace session backed up and restored around every run, compared identical):
  - **Run 1 (photo):**
    - The input was a 1600 × 1200 JPEG made to look like a phone photo of a hand drawing: off-white paper with a lamp fall-off, 2.5° tilt, wobbly pen lines, sensor noise, handwritten labels.
    - At 21 m wide it read as **2 decks** (the stage with its thrust, 8 corners, 84.75 m²; the drum riser at 120 cm), **3 truss runs** (upstage and both legs) and **8 fixtures** (six under the upstage truss, two on the riser). The 6 text marks were left out.
    - GENERATE 3D built all of it in one step: 47 deck panels, 13 truss sections.
    - Screenshots: `Saved/Screenshots/Sketch_105106.png` (the strokes exactly on the faint photo, the PHOTO row, the message) and `WindowsEditor/ScreenShot00039.png` (the generated stage in 3D).
    - Run 5 read the same photo the same way after the fixes.
  - **Run 2 (Armenian):**
    - Help; "Ավելացրու 4 շարժական գլուխ" (4 fixtures added after streaming); "քանի՞ սարք կա" → "6 լուսային սարք…"; total weight 212 kg.
    - Cue 1 «Ներածություն» stored with a 3 s fade; rain (the sky turned on); "ժամը 8 երեկոյան" → 20:00; the weather described in Armenian; undo named the step.
    - "ավելացրու 2 վիշապ" refused, listing the catalog's fixtures; "how many fixtures" answered in English; "ջնջիր ամեն ինչ" explained.
    - `Saved/Screenshots/Assistant_105301.png`: Armenian prompts and replies render correctly with the Noto sub-font, beside English; MIC is disabled with the reason on the voice line.
  - **Run 3 (voice):** voice input on through an `-ini` override. `Voice devices` saw 1 recording device without opening it; MIC refused for the missing `OPENAI_API_KEY`, with the reason on the voice line (`Assistant_105540.png`).
  - **Runs 5 and 7 (overflow):** a long conversation that overflows the panel; after the scroll fix the newest reply is fully visible (`Assistant_111332.png`; it was cut off in Run 2).
  - The test photo copy (`Saved/R44/`) and the automation test files were removed afterwards.
- **Not verified:**
  - by a person: IMPORT PHOTO... with the real file dialog, READ / SHOW / REMOVE and PHOTO WIDTH by mouse; typing Armenian with a real keyboard layout; MIC with a real microphone;
  - **the online services:** no Anthropic or OpenAI key is available on this machine. Armenian through Claude, the photo reading by Claude and a real transcription are covered only by the request and response tests. Recording the microphone and sending it out was deliberately not done without Gevor.
- Status: **implemented, awaiting manual verification.**

**Commit:** 10d3104.

**Known issues / follow-ups**
- The offline Armenian lexicon is finite. Unusual wording gets "not understood"; free Armenian needs the online model. The Armenian texts were written by Claude and should be read by a native speaker.
- Refusals worded by the GameMode or economy, catalog names and undo step names stay in English inside Armenian replies.
- Photo reading:
  - Phone photos with EXIF rotation are read as stored, and perspective is not corrected: take the photo from straight above.
  - A closed truss loop reads as a deck (the canvas's convention), and letters such as "o" can become fixtures.
  - The photo width applies to the next import (CLEAR and import again to rescale).
- Voice: one recording at a time, no level meter; the transcript is checked in the field by default.
- Observed while testing, not from this change: quitting within about 35 s of start waits for the runtime model import of `AG1.FBX` from the import list (ADR 0006). In Run 5 that pushed the exit past the script's timeout.
- PDF exports still print Armenian as "?" (WinAnsi only, ADR 0011).

## #45 — Unreal Editor camera controls, collapsible viewport toolbar, fixture shadow budget, stairs and curved truss, assistant delete / move / replace / build shapes (2026-10-23)

**Request (Gevor, 2026-10-22, pasted phase spec, "strictly follow Rules.md").**
1. Authentic UE5 editor mouse controls, speeds and smooth up/down movement in the viewport; no frame drops during crossfades, with many fixtures, or while editing.
2. The viewport toolbar (Perspective, Lit, Snap, Angle, Move/Rotate/Scale, Sketch, Assist) collapses and expands.
3. `UStageAICommandSubsystem`: targeted delete, move and replace of selected or named items (fixtures, decks, truss), in Armenian and English.
4. The assistant builds circular risers, custom stairs and curved truss arcs from a description.
5. `TObjectPtr`, encapsulation, no per-frame tick polling, all tests, 0 warnings, STATE.md.

**Gevor's decisions (asked before design, 2026-10-22):**
- Frame rate through a **fixture shadow budget**: the N most important fixtures cast beam shadows, default 64, set in Project Settings.
- A **procedural curved truss** item, and a **procedural stairs** item.
- The **wheel dollies as in UE**; RMB + wheel changes the speed. This supersedes #14.
- **Fixed look as in UE**: 0.2°/count × the sensitivity. The #15 speed link stays as an option, off by default.

Design: `Docs/ADR/0018-ue-camera-shadow-budget-shapes-and-assistant-edits.md`.

**What changed and why**
- **Camera as in the Unreal Editor 5.8 (ADR 0018 §2).** The numbers come from the engine source (`EditorViewportClient.cpp`, `CameraController.h`, `EditorViewportSettings.h`).
  - `StageCameraNavigation` (new, pure) holds the rules:
    - `ClassifyDrag`: LMB fly-and-turn, RMB look, MMB pan, LMB+RMB vertical pan, Alt+LMB orbit, Alt+RMB dolly, Alt+MMB orbit pan.
    - Speed: continuous 0.01..32, ±10 % per RMB+wheel notch. Flying at speed S: 2000·S cm/s.
    - Drags: 1 cm/count × S, 0.2°/count. Wheel dolly: 96 cm/notch.
    - F framing: the `FocusViewportOnBox` maths, a 0.25 s cubic-out glide.
  - `UStageCameraNavigationComponent` (new, on the controller) owns the input state:
    - the buttons held and the active drag;
    - a pending left click: a click until it travels 3 px, then a drag with the whole travel;
    - the orbit pivot: F's target, else the surface under the view centre, else 10 m ahead;
    - whether a press hides the cursor (`FStageCursorCapturePolicy` on the viewport client).
  - `AStageCameraPawn` was rewritten:
    - `CameraSpeed` with `OnCameraSpeedChanged` replaces FlySpeed.
    - It has eased translation, orbit, the F glide and fixed views.
    - **It ticks only while moving and turns its tick off at rest.**
  - `UStageCraftGameViewportClient` tracks L / M / R and Alt, and broadcasts `OnMouseButtonsLost` on focus loss.
  - `UStageCraftUserSettings` stores the camera speed (saved when the buttons are released) and the look link.
  - New key-map commands:
    - **Focus Selection (F)**. In the orthographic views it centres and fits (`UStageViewportComponent::FocusOrtho`).
    - **Show / Hide Viewport Toolbar (Ctrl+Shift+T)**.
  - The status bar shows "Camera speed S · flying m/s". Preferences has a Fixed / Linked "Look with speed" row and new mouse-controls text.
- **The gizmo no longer ticks (§2.5).** `AModularTransformGizmo` gets the view through `SetView(FStageGizmoView)` on camera, view and selection changes. It follows its target through the target's transform-updated event.
- **Collapsible toolbar (§3).** A « segment collapses `UStageViewportToolbar` to a 14 px "» VIEWPORT TOOLS" strip.
  - The state is `UStageViewportComponent::IsToolbarCollapsed` / `SetToolbarCollapsed`, saved per user and broadcast on `OnViewChanged`.
- **Fixture shadow budget (§4).**
  - `StageShadowBudget::SelectCasters` (new, pure):
    - Importance is the flux at the dimmer ÷ the distance² in metres. The Budget most important fixtures cast.
    - Dark fixtures never cast.
    - Current casters get 25 % hysteresis.
    - Ties keep the registration order.
  - `UShowControlSubsystem::MarkShadowBudgetDirty` is called on fixture registration, intensity output and camera moves.
    - A one-shot 0.25 s timer re-ranks. It is cleared in Deinitialize.
    - Only fixtures whose choice changed get `SetCastShadows`. Nothing runs per frame.
  - Settings: `UStageShowPerformanceSettings`, in Project Settings › Game › StageCraft Performance (on, 64, 0.25 s, 0.25).
- **Procedural shapes (§5).** `StageProceduralShapes` (new, pure) holds the geometry:
  - `MakeCircle` (16..96 corners within 1 cm of the curve) and `MakeArcRing` (≤ 359°) are deck outlines.
  - `LayoutStairs` / `BuildStairsMesh`: equal steps of at most 20 cm, optional handrails.
  - `LayoutArcTruss` / `BuildArcTrussMesh` / `BuildArcTrussCollision`:
    - four chords swept inside the 29 cm outside profile;
    - rungs and zigzag diagonals per bay, and end caps;
    - one convex hull per 2 m section.
  - **`AStageStairsActor` + `UStageStairsData`** (new):
    - SaveGame rise, width and handrails, sanitized with `ReportStateRepair`.
    - Undoable shape parameters. Collision is complex as simple.
  - **`AStageArcTrussActor` + `UStageArcTrussData`** (new):
    - SaveGame radius and angle, sanitized.
    - The quantity is the curved sections. `PredictQuantity` answers for radius and angle, which are quantity parameters.
    - Collision is convex hulls, because item traces are simple-collision queries.
  - New tags: the Stairs and ArcTruss feature groups, the Stairs.* and Arc.* parameters, and `StageCraft.Category.Truss.Curved`.
  - The build plan carries the new shapes:
    - `FStageSketchBuildPlan` gains `Stairs` and `ArcTrusses`.
    - `StageSketchBuilder::Execute` spawns them all or nothing.
    - The outcome and `FStageSketchGenerateResult` count them, and `DescribePlan` names them.
  - Content, from `Scripts/Content/create_procedural_shapes_content.py` (headless):

    | Asset | What it is |
    |---|---|
    | `DA_Stairs_Stage` | 20-200 cm rise, 60-400 cm wide, 35 kg |
    | `DA_Truss_Arc_F34` | 1-20 m radius, 15-360°, 2 m sections, 13 kg per section |
    | `M_Handrail`, `M_TrussArc` | materials |

    Read back afterwards: the classes, ranges, tags and materials are as set.
- **Assistant edits and shapes (§6).**
  - New intents: `delete_items`, `move_items`, `replace_items`, `build_shape`.
    - New fields: `target` (selection / named), `replacement`, `direction`, `distance_cm`, `shape`, `radius_cm`, `width_cm`, `height_cm`, `angle_degrees`, `handrails`.
    - All are validated in `StageAICommandSchema::Validate` and in the tool JSON. Unknown enum values are refused, and an item without a target is named.
  - New controller requests:
    - `RequestDeleteItems`: one step, like Delete.
    - `RequestMoveItems`: every new transform is checked by `EvaluateTransformChange` first, then written as Location edits and recorded as one step.
    - `RequestReplaceItems`: the old items are removed first and each new one is checked and spawned. Any refusal rolls everything back. It is one step, through the new `UStageEditHistoryComponent::RecordReplacement`.
  - Targets:
    - The asking player's selection, optionally narrowed by name. It is passed into `SubmitPrompt` and into the model's scene summary as "Selected: …".
    - Or the placed items whose name contains every word ("lights" means every fixture): latest first, with an optional count, at most 200.
  - Directions are camera-relative, snapped to the stage axes. Up and down are world Z.
  - Shapes are clamped to their item's ranges and placed in front of (−X) everything already placed. A curved truss hangs at 6 m unless the request says otherwise.
  - `UStageAICommandSubsystem::RequestItemAssets` now streams several items.
  - English grammar:
    - Sizes and units: m, cm, mm, ft, °, and glued forms such as "3m".
    - Roles: wide / high / radius / diameter / at. An unlabelled riser or circle size is the diameter.
    - "remove / clear the fog" still clears the weather.
    - "clear the selection" never deletes.
    - Fixtures "in a circle" stay fixtures.
    - Questions are not edits; polite requests are.
  - Armenian lexicon:
    - New words: verbs, selection, directions, shapes, sizes and units.
    - The instrumental -ով is read as "with" (for replace).
    - աստիճան after a number in a curved request is read as degrees.
  - Clearing the whole stage and deleting cues are still explained, with new wording. The system prompt no longer says deleting is impossible. Help lists the new commands in both languages.
- **Unity-build fix found on the way.**
  - `StageReportPdfComposer.cpp` had anonymous-namespace globals (`Ink`, `MutedInk`, …).
  - Once the new files reshuffled the unity blobs, they collided with locals in `StageSketchImageTrace.cpp` (C4459, an error here).
  - They are now `ReportInk`, `ReportMutedInk`, `ReportHeaderFill`, `ReportStripeFill`, `ReportRuleInk`.
- **Arc truss width fix found by the tests.** The chord centre lines first sat on the profile width, which made F34 34 cm wide. They now sit one chord radius inside.

**Files**
- New code:
  - `Public|Private/Player/StageCameraNavigation`, `Player/StageCameraNavigationComponent`
  - `Show/StageShadowBudget`, `Placement/StageProceduralShapes`
  - `Actors/StageStairsActor`, `Actors/StageArcTrussActor`, `Data/StageStairsData`, `Data/StageArcTrussData`
- New tests:
  - `Private/Player/Tests/StageCameraNavigationTests.cpp`
  - `Private/Show/Tests/StageShadowBudgetTests.cpp`
  - `Private/Placement/Tests/StageShapeTests.cpp`
  - `Private/Sketch/Tests/StageShapeWorldTests.cpp`
  - `Private/AI/Tests/StageAIEditTests.cpp`
- New content and docs:
  - `Scripts/Content/create_procedural_shapes_content.py`
  - `Content/StageCraft/Items/DA_Stairs_Stage.uasset`, `DA_Truss_Arc_F34.uasset`
  - `Content/StageCraft/Shapes/M_Handrail.uasset`, `M_TrussArc.uasset`
  - `Docs/ADR/0018-ue-camera-shadow-budget-shapes-and-assistant-edits.md`
- Changed code (Public and Private unless noted):
  - Player and input: `Player/StageCameraPawn`, `Player/ModularPlayerController`, `Player/StageCraftGameViewportClient`, `Input/StageKeyBindings`, `Input/StageInputPreferencesSubsystem`, `Settings/StageCraftUserSettings`
  - Actors and show: `Actors/ModularTransformGizmo`, `Actors/LightingFixtureActor`, `Subsystems/ShowControlSubsystem`
  - Viewport and UI: `Viewport/StageViewportComponent`, `UI/StageViewportToolbar`, `UI/StageStatusBarWidget`, `UI/StagePreferencesPanel`
  - Data and history: `Data/StageParameterTypes`, `Data/StageItemTypes`, `History/StageEditHistoryComponent`
  - Sketch: `Sketch/StageSketchTypes` (Public), `Sketch/StageSketchBuilder`, `Private/Sketch/StageSketchGenerator.cpp`
  - AI: `AI/StageAITypes`, `AI/StageAICommandParser`, `AI/StageAICommandSubsystem`, `Private/AI/StageAICommandSchema.cpp`, `Private/AI/StageAIArmenian.cpp`, `Private/AI/StageAICommandRunner.h/.cpp`
  - Reports: `Private/Report/StageReportPdfComposer.cpp`
- Changed tests: `Private/AI/Tests/StageAITests.cpp`, `StageAIArmenianTests.cpp`, `Private/UI/Tests/StageUITests.cpp`
- Changed docs: `Docs/CURRENT_SYSTEMS.md`, `tasks/todo.md`, `STATE.md`

**Verification**
- **Builds:** Editor, Game Development and Game Shipping (`-DisableAdaptiveUnity`) all **succeeded with 0 warnings**. They were built with `-NoHotReloadFromIDE` because Gevor's KMidden editor was open.
- **Automation: 96/96 `StageCraft.*` passed** (exit code 0).
  - 10 tests are new: Camera.Drags, Camera.Math, Show.ShadowBudget, Shapes.Outlines / Stairs / ArcTruss / World, AI.Edits.Grammar / Armenian / Schema.
  - Existing tests were changed on purpose:
    - "delete all lights" is now a targeted delete.
    - The Armenian help has 32 examples.
    - `UI.SpanHint` remaps "finish path" to K, because F is now Focus Selection.
- **Scripted Standalone `-game` run** (`Saved/Logs/R45_Run1.log`). Gevor's workspace session and GameUserSettings were backed up and restored, and compared identical.
  - Building shapes:
    - "build a 4 m round riser 60 cm high" → a round deck of 7 panels.
    - "add stairs 80 cm high" → stairs with handrails.
    - "add a 6 m circle truss at 7 m" and "make a 180° arc truss radius 5 m" → 18 curved F34 sections in 2 runs on the bill of materials.
    - Screenshots:
      - `WindowsEditor/ScreenShot00041.png`: perspective, the riser and the stairs.
      - `ScreenShot00042.png`: top view, the circular truss with its bracing.
      - `Saved/Screenshots/Assistant_003418.png`: the conversation.
  - "move the moving heads 2 m left" moved all 6 moving heads as one undo step.
  - "replace 2 moving heads with washes" was refused by the shop rule (the wash is locked), as it should be.
  - Armenian:
    - "տեղափոխիր ֆերման 1 մ վեր" moved the two curved truss runs and the box truss up 1 m.
    - "հեռացրու բոլոր վոշերը" answered that no wash is placed.
    - "ջնջիր ամեն ինչ" was explained in Armenian.
    - "undo" then undid that truss move ("Transform 3 items").
- **Not verified:**
  - **The frame-time gain of the shadow budget is not measured.**
    - The first benchmark baseline was invalid: Gevor's KMidden editor opened mid-run and the GPU was at 96 %. KMidden stayed open for the rest of the session.
    - The "zero FPS drops" goal is therefore unproven.
    - Next step: with KMidden closed, run `Scripts/Bench/RunFixtureBench.ps1 -Counts 50,200,500` with the budget on and with it off (Project Settings), and compare with #39.
  - By a person: the mouse feel of every drag, F, RMB+wheel speed, the toolbar collapse by mouse, the stairs and curved truss in the inspector, and the assistant edits on a mouse selection.
  - The online model with the new tool fields: there is no key on this machine.
- Status: **implemented, awaiting manual verification.**

**Commit:** `872efc2`.

**Known issues / follow-ups**
- The benchmark is pending (above).
- "Fewer per-frame broadcasts during fades" was not done. It should follow only if the benchmark shows a CPU cost there.
- Replacing a fixture does not carry its DMX patch to the new one.
- Shapes are always placed in front of the stage at yaw 0. Curved risers are limited to 359°.
- The undo step for an assistant move is named by the transform history ("Transform N items"), not "Assistant: move".
- A circle or arc can only be placed by the assistant or from the Library (at the default size), not drawn in the Sketch window.
- The Armenian additions were written by Claude and should be read by a native speaker.

## #46 — DMX patch subsystem, Art-Net / sACN input, Patch panel tools (2026-10-26)

**Request (Gevor, 2026-10-26, pasted phase spec, "strictly follow Rules.md").**
1. `UStageDMXPatchSubsystem`: DMX universes, channel allocation (start address, footprint per fixture type) and automated conflict-free patching. A DMX Patch table panel to inspect, re-address and assign fixtures.
2. A light background UDP listener for Art-Net and sACN (E1.31), so external consoles or visualisers drive dimmer, colour (RGBW), pan/tilt and gobo in real time.
3. `TObjectPtr`, encapsulation, no per-frame Tick (event-driven packets, efficient buffer updates), all tests, 0 warnings, STATE.md.

**Gevor's decisions (asked before design, 2026-10-26):**
- **Own small receiver** on the engine's sockets, not Epic's DMX Engine plugin. This replaces the #9 plan of a `StageCraftDMX` module on `DMXProtocol`.
- **StageCraft universe 1 = Art-Net port-address 0 (0:0:0)** and sACN universe 1, with offsets in the settings.
- **Input off until switched on**, remembered on this PC.
- **Answer ArtPoll**, so consoles that send only to discovered nodes find StageCraft.

Design: `Docs/ADR/0019-dmx-patch-and-network-input.md`.

**What changed and why**
- **Pure patch allocation (ADR 0019 §2.1).** `StageDMXPatching` (new) works on slots {key, patch, footprint}:
  - `Fits` and `GetLastStartAddress`.
  - `FindFree`: the first gap at or after a universe and address, moving to later universes when the start universe is full. It uses sorted ranges and a binary search.
  - `FindConflicts`: a sorted sweep, O(n log n). The old pair loop was O(n²). A range past channel 512 is now a conflict too.
  - `PlanConsecutive` (all or nothing), `PlanConflictRepair` (the first fixture in each overlap stays) and `SummarizeUsage`.
- **`UStageDMXPatchSubsystem` (new, world, §2.2).**
  - Fixtures register in BeginPlay, after the show control has given them their ID, and unregister in EndPlay.
  - Auto-patch of new fixtures (`bAutoPatchNewFixtures`, moved here from the show control).
  - A per-universe index, derived from the fixtures' own patch (still the source of truth). It is rebuilt lazily after any reported change: `ALightingFixtureActor::SetPatch`, `ApplyItemData` (a new footprint or a restored state), registration. `GetPatchRevision` and `OnPatchChanged` report changes.
  - It also provides conflicts as fixture IDs, universe usage and the plans `PlanUnpatched`, `PlanConsecutive` and `PlanConflictRepair`.
  - `UShowControlSubsystem` lost `FindNextFreePatch`, `FindPatchConflicts`, `GetPatchedUniverses` and `bAutoPatchNewFixtures`. It now takes the patch subsystem as an `InitializeDependency` and renders and decodes only the fixtures of the universe in question.
- **Rules and undo for patching (§2.3).**
  - The GameMode refuses an address a fixture does not fit: "… uses 21 channels, so its address can be 1 to 492".
  - `StageCraftTags::IsPatchParameter` covers the fixture ID, universe and address. Single patch edits from the inspector or the Patch panel are now undoable.
  - New `AModularPlayerController::RequestPatchFixtures(Assignments, EmptyReason)`:
    - It checks every new universe and address with the GameMode before anything changes.
    - It applies them through the session in one edit transaction, so the plan is one undo step.
    - It rolls back if a write fails.
    - It refuses an empty plan with the planner's reason.
  - The old PATCH UNPATCHED sent the universe and the address as two separate requests; that is gone.
- **Art-Net / sACN receiver (§3).**
  - `StageDMXNetProtocol` (new, pure) parses and validates packets:
    - Art-Net 4 ArtDmx: protocol 14+, the length present in the datagram, 15-bit port-address.
    - ArtPoll.
    - E1.31 data packets: every vector, the PDU flags, priority ≤ 200, universe 1..63999, DMP fields, property count, START code 0, Stream_Terminated.
    - Builders for ArtDmx, ArtPoll, ArtPollReply (239 bytes, four ports per bind index, grouped by Net/Sub-Net) and sACN data. The ArtPollReply builder is used to answer polls; the others serve the tests and SendTest.
  - `FStageDMXSourceArbiter` picks one source per universe:
    - the highest priority wins (Art-Net counts as 100);
    - a source is gone after 2.5 s of silence, or at once when it sends Stream_Terminated;
    - sACN frames up to 20 behind in sequence are dropped.
  - `StageDMXNetworkTypes` (new) holds:
    - `FStageDMXInputSettings`, saved per PC in `UStageCraftUserSettings` (GameUserSettings.ini), always sanitized;
    - the states Off / Listening / Failed and the status struct;
    - wire↔StageCraft universe numbering.
  - `FStageDMXNetworkReceiver` / `FStageDMXInbox` (new, private):
    - two sockets, each read by an engine `FUdpSocketReceiver` thread;
    - parsing, mapping and arbitration on the socket thread, keeping only the newest 512 slots per universe under one lock;
    - the first frame after a drain queues one game-thread task (`AsyncTask` holding a weak pointer), so there is one hand-off per batch and nothing at all while the wire is quiet;
    - ArtPoll is answered to the poller, at most 10 reply rounds per second;
    - the sACN socket joins the multicast group of each patched universe on the chosen card;
    - Stop joins the threads before it destroys the sockets.
  - `UStageNetworkDMXBridge` (new, private, a `UStageDMXBridge`):
    - It owns the receiver. Output is off, so the show does not render frames for it.
    - It drains on the game thread into `ReceiveUniverse`.
  - `UStageDMXNetworkSubsystem` (new, world):
    - It switches the input on and off and remembers the choice.
    - On `OnWorldBeginPlay` it re-opens a remembered "on" and gives the show to External.
    - It keeps the multicast groups and ArtPollReply in step with the patch, updating once on the next tick.
    - Status goes out on `OnInputChanged`: at once for every state change, at most every 0.5 s while frames arrive.
  - New `AModularPlayerController::RequestSetDMXInput(bool)`. On opens the ports and switches the show to External through the cue rules. Off closes them and switches back to Internal. A port that cannot be opened is reported on OnRequestRejected.
  - `Sockets` and `Networking` are new private module dependencies. The Build.cs comment about the DMX Engine plugin was updated.
- **Efficient decode (§4).** `UShowControlSubsystem::ReceiveDMXUniverse` now returns the number of fixtures it updated. It decodes only:
  - the fixtures of that universe (from the index);
  - and among them, only those whose own channels differ from the last frame of that universe.

  The last frames are forgotten when the patch or the control source changes. Consoles repeat unchanged frames about 44 times a second, and those now cost nothing.
- **Patch panel (§5).**
  - A second header bar with:
    - **DMX IN: ON / OFF** and the input status: ports, live universes and sources, Art-Net / sACN frames, invalid datagrams, or the error in the warning colour;
    - **FIX CONFLICTS**;
    - **PATCH SELECTION AT** with universe and address fields;
    - the usage of each universe (`U1 63/512`, `!` on conflicts).
  - PATCH UNPATCHED is now **AUTO PATCH**.
  - Every tool goes through `RequestPatchFixtures`. The panel rebuilds on `OnPatchChanged` and updates the input line on `OnInputChanged`. There is still no tick.
- **Console (development builds only).**
  - `StageCraft.DMX.Input On|Off|Status`
  - `StageCraft.DMX.Settings [Bind=…] [ArtNetFirst=n] [SACNFirst=n] [ArtNet|SACN|ArtPoll=On|Off]`: the network card and universe offsets. They are saved on this PC, and a running input restarts with them. There is no panel field for them yet.
  - `StageCraft.DMX.AutoPatch`, `StageCraft.DMX.FixConflicts`, `StageCraft.DMX.PatchSelection <u> <a>`
  - `StageCraft.DMX.SendTest ArtNet|sACN <wire universe> <channel> <value> [count]`: sends a real frame to this PC.

**Files**
- New code:
  - `Public/Show/StageDMXPatching.h`, `Private/Show/StageDMXPatching.cpp`
  - `Public/Show/StageDMXNetProtocol.h`, `Private/Show/StageDMXNetProtocol.cpp`
  - `Public/Show/StageDMXNetworkTypes.h`, `Private/Show/StageDMXNetworkTypes.cpp`
  - `Private/Show/StageDMXNetworkReceiver.h/.cpp`, `Private/Show/StageNetworkDMXBridge.h/.cpp`
  - `Public/Subsystems/StageDMXPatchSubsystem.h`, `Private/Subsystems/StageDMXPatchSubsystem.cpp`
  - `Public/Subsystems/StageDMXNetworkSubsystem.h`, `Private/Subsystems/StageDMXNetworkSubsystem.cpp`
  - `Private/Show/StageDMXConsoleCommands.cpp`
- New tests:
  - `Private/Show/Tests/StageDMXPatchingTests.cpp`
  - `Private/Show/Tests/StageDMXNetProtocolTests.cpp`
  - `Private/Show/Tests/StageDMXReceiverTests.cpp`
- Changed code:
  - `ModularSceneBuilder.Build.cs`
  - `Public|Private/Subsystems/ShowControlSubsystem`
  - `Public|Private/Actors/LightingFixtureActor`
  - `Public|Private/Player/ModularPlayerController`
  - `Private/Game/StageCraftGameModeBase.cpp`
  - `Public|Private/Data/StageParameterTypes`
  - `Public|Private/Settings/StageCraftUserSettings`
  - `Public|Private/UI/StagePatchPanel`
  - `Public/Report/StagePatchSheetSubsystem.h`, `Private/Report/StagePatchSheetSubsystem.cpp`
  - `Private/Show/StageShowConsoleCommands.cpp`
- New and changed docs: `Docs/ADR/0019-dmx-patch-and-network-input.md` (new), `Docs/CURRENT_SYSTEMS.md`, `tasks/todo.md`, `STATE.md`
- No content assets changed. The Patch panel is a C++ widget and `DA_Panel_Patch` is unchanged.

**Verification**
- **Builds:** Editor, Game Development and Game Shipping (`-DisableAdaptiveUnity`) all **succeeded with 0 warnings**. They were built with `-NoHotReloadFromIDE` because Gevor's KMidden editor was open.
- **Automation: 108/108 `StageCraft.*` passed** (exit code 0). 12 tests are new:
  - DMX.Patching: Fit, FindFree, Conflicts, Plans, Usage.
  - DMX.Protocol: ArtNet, ArtPollReply, SACN, Sources, Numbering.
  - **DMX.Receiver:** real Art-Net and sACN datagrams over loopback, on test ports 16454 / 15568:
    - mapped onto universes 1 and 2, with the newest frame kept;
    - hand-offs coalesced;
    - garbage counted, not shown;
    - ArtPoll answered with a 239-byte reply naming the node;
    - a receiver with no protocol refuses to start and leaves nothing open;
    - Stop is idempotent.
  - **DMX.PatchWorld:** a game world with real fixtures:
    - auto-patch packs them 1.001 / 1.022 / 1.043;
    - the index follows a re-patch;
    - a conflict is found and repaired with only the later fixture moving;
    - consecutive plans are in fixture-ID order;
    - a received universe updates both fixtures the first time, nothing on a repeat, and one fixture for one changed channel, and is ignored while Internal;
    - after a re-patch the next frame applies in full;
    - auto-patch off leaves a new fixture unpatched, and AUTO PATCH plans it into the first gap.
- **Scripted Standalone `-game` runs** (`Saved/Logs/R46_Run1..4.log`). Gevor's workspace session and GameUserSettings were backed up and restored, and compared identical after every run.
  - **Run 1 (his saved layout):**
    - DMX IN on listened on Art-Net 6454 and sACN 5568 on every card.
    - Art-Net frames from `SendTest` arrived on universe 1.
    - An sACN frame for the same universe sent 0.5 s later was ignored (equal priority, Art-Net still live: "ignored 1").
    - After 5 s of Art-Net silence, sACN took over ("live universes [1] from 1 sources").
    - DMX IN off closed the ports.
  - **Run 3 (default layout, Patch tab docked):**
    - Address 10 on Spot 102 made a conflict: red "!", "2 conflicts", "U1 30/512 !" (`ScreenShot00049.png`).
    - Address 500 was refused: "Spot 102 uses 21 channels, so its address can be 1 to 492".
    - `FixConflicts` moved Spot 102 back to 1.022 as one step.
    - Undo brought the conflict back; redo fixed it again. Edit.History named the step "Change Address of Spot 102".
    - DMX IN on plus one Art-Net frame put Spot 101's dimmer at 65535 and every other channel at 0 (pan −270, tilt −135, black). The beam lit.
    - The status line read "Listening · Art-Net 6454, sACN 5568 on every network card · 1 universe live from 1 source · 1 Art-Net, 0 sACN frames" (`ScreenShot00050.png`, `Saved/Screenshots/WindowsEditor`).
  - **Run 4:**
    - `StageCraft.DMX.Settings ArtNetFirst=1 SACNFirst=101 Bind=bogus` restarted the running input with Art-Net 1 = universe 1 and sACN 101 = universe 1. The bogus address fell back to every card.
    - An Art-Net frame on port-address 1 arrived as universe 1.
    - The settings were set back to the defaults.
- **Not verified:**
  - A real console or visualiser on the LAN (another PC, multicast sACN from another machine), the Windows firewall prompt on first listen, and discovery by a real controller through ArtPollReply.
  - By a person: the DMX IN button, FIX CONFLICTS / AUTO PATCH / PATCH SELECTION AT and the start fields by mouse; the inspector Ctrl+Z on a patch edit.
- Status: **implemented, awaiting manual verification.**

**Commit:** `0cbaaa7`.

**Known issues / follow-ups**
- The DMX patch is not carried over when the assistant replaces a fixture (from #45; still open).
- No DMX output (Art-Net / sACN send), no HTP / LTP merge, no RDM, no sACN universe discovery, one DMX mode per fixture.
- While External, a fixture changed by hand is not overwritten until the console changes one of its channels. This is the price of skipping repeated frames (ADR 0019 §4).
- ArtPollReply announces the address the OS prefers when listening on every card. A PC with several cards should set `Bind=`.
- The network card and universe offsets can only be set with the console command; the Patch panel has no fields for them.

---

## #47 — Viewport camera fixed to match the Unreal Editor exactly: raw mouse counts, Alt+RMB dolly direction, no easing, the editor's flight (2026-10-27)

**Request (Gevor, 2026-10-27, pasted, "strictly follow Rules.md").**
1. Invert the Y axis: moving the mouse up must look / move up.
2. Camera speed, acceleration, deceleration and mouse sensitivity must mirror the UE5 editor viewport exactly, with no sluggishness or lag.
3. No per-frame Tick at rest, `TObjectPtr`, 0 warnings, STATE.md.

**Housekeeping first.** #45 and #46 said "uncommitted"; they now carry `872efc2` and `0cbaaa7`.

**What was measured.**
- A new dev command, `StageCraft.View.MouseDrag <Left|Right|Middle> <dx> <dy> [Frames] [Alt|Key]`, presses a mouse button over the viewport and feeds raw motion through `FSlateApplication::OnRawMouseMove`, the path Windows raw input takes. It then logs how the camera turned and moved. Sign and scale are therefore what a real mouse produces.
- **Before the fix:**
  - **Scale was the real fault.** A 100-count right-drag turned the view 1.40°; the editor turns 20° (0.2°/count). Every drag moved 7 cm per 100 counts instead of 100 cm.
    - Cause: `Config/DefaultInput.ini` kept the engine's axis sensitivity of **0.07** for `MouseX`, `MouseY` and `Mouse2D`. Enhanced Input applies it to `Mouse2D` (`UPlayerInput::MassageVectorAxisInput`).
    - The camera rules expect raw counts, so every drag was 14 times too slow. The orthographic pan also lagged the cursor, since it maps counts to pixels.
  - **The vertical direction already matched the editor's defaults** (measured, and checked against `FEditorViewportClient::ConvertMovementToDragRot` in the UE 5.8 source):
    - RMB, mouse up: pitch up.
    - LMB, mouse up: forward.
    - MMB and LMB+RMB, mouse up: the camera moves up.
    - Alt+LMB, mouse up: pitch up.
    - Gevor's own editor settings have no invert options set (`bInvertMouseLookYAxis`, `bInvertOrbitYAxis`, `bInvertMiddleMousePan` and `bInvertRightMouseDollyYAxis` are all off).
  - **One gesture was reversed: Alt+RMB dolly.** The editor moves closer by X − Y (`IsOrbitZoomMode`: right or down closer, up away); #45 used X + Y.
  - **Easing the editor does not have:**
    - The pawn eased look (`RotationSmoothing` 25/s, about 40 ms behind the mouse), drags and the wheel (`TranslationSmoothing` 25/s), and flight (`MovementSmoothing` 8/s).
    - The editor applies mouse rotation and drags at once (`bUsePhysicsBasedRotation` is off by default).
    - The editor flies with its own discrete integrator (`FEditorCameraController::UpdatePosition`).

**What changed.**
- **Input config:** the `MouseX` / `MouseY` / `Mouse2D` sensitivity in `Config/DefaultInput.ini` is now **1.0**. A new test, `StageCraft.Camera.InputConfig`, fails if their sensitivity, exponent, dead zone or invert flag changes again.
- **`StageCameraNavigation`:**
  - `StepFlightVelocity` is the editor's flight step: `v += impulse · 20000 · S · dt`, then `v −= v · min(10 · dt, 0.75)`, with dt at most 1 s.
  - Impulses are not normalised (a diagonal is faster, as in the editor), one impulse per held direction.
  - `GetDollyDistance` uses X − Y.
  - New constants `FlyAccelerationPerSpeed`, `FlyDampingPerSecond`, `MaxFlyDampingPerStep` and `MaxFlightStepSeconds`.
- **`AStageCameraPawn` reworked:**
  - Look, LMB yaw, drags, the wheel dolly and orbit / dolly / orbit-pan are applied the moment the input arrives. `RotationSmoothing`, `TranslationSmoothing`, `MovementSmoothing` and their target / pending state are gone.
  - The view rotation's single source of truth is the controller's control rotation: read, change, clamp pitch to ±89°, write back.
    - This replaces the old Target / Current rotation pair and `AdoptControlRotation`.
    - Rotations set by others (spawn, projection switch) are simply where the next input starts.
  - **Tick:** the pawn starts with tick off and ticks only during a key flight or the F glide, switching off when both have settled. It no longer ticks while the mouse turns or drags it.
  - `SetViewRotation(FRotator)` lost its `bSnap` flag, since every rotation is now immediate. No Blueprint calls it; `BP_StageCameraPawn` has no overrides of the removed properties.
  - Flight input cancels a running focus glide, as a drag already did.
- **`UStageCameraNavigationComponent`:** uses `APawn::GetViewRotation()` (the control rotation) where it used `GetTargetViewRotation()`.
- **`StageCraft.View.MouseDrag`** (dev only, compiled out of Shipping) was added to `StageViewportConsoleCommands.cpp`, as described above.

**Files changed**
- `Config/DefaultInput.ini`
- `Source/ModularSceneBuilder/Public/Player/StageCameraNavigation.h`, `Private/Player/StageCameraNavigation.cpp`
- `Source/ModularSceneBuilder/Public/Player/StageCameraPawn.h`, `Private/Player/StageCameraPawn.cpp`
- `Source/ModularSceneBuilder/Private/Player/StageCameraNavigationComponent.cpp`
- `Source/ModularSceneBuilder/Private/Player/Tests/StageCameraNavigationTests.cpp`
- `Source/ModularSceneBuilder/Private/Viewport/StageViewportConsoleCommands.cpp`
- `Docs/ADR/0018-ue-camera-shadow-budget-shapes-and-assistant-edits.md` (new §2.6, §2.2 pawn line), `Docs/CURRENT_SYSTEMS.md`, `tasks/todo.md`, `STATE.md`
- No content assets changed.

**Verification**
- **Builds:** Editor, Game Development and Game Shipping (`-DisableAdaptiveUnity`) all **succeeded with 0 warnings**.
- **Automation: 109/109 `StageCraft.*` passed** (exit code 0).
  - New test `Camera.InputConfig`.
  - `Camera.Math` gained:
    - the dolly direction cases (right/down closer, up away, right+up cancel);
    - the flight step (first step 277.8 cm/s at 60 fps, settles at 1666.7, ×2 speed doubles the acceleration, brakes below 0.5 cm/s within a second, diagonal √2, damping cap 0.75, a 5 s hitch integrates 1 s, NaN stops).
- **Scripted Standalone `-game` runs** (`Saved/Logs/R47_Drag1..3.log`, `R47_Fly.log`). Gevor's workspace session and GameUserSettings were backed up and compared identical afterwards.

  | Drag (100 raw counts) | Before | After (R47_Drag3) |
  |---|---|---|
  | RMB up | pitch +1.40° | pitch **+20.00°** |
  | RMB right | yaw +1.40° | yaw **+20.00°** |
  | LMB up | 7.0 cm forward | **100.0 cm** forward along the yaw, no pitch |
  | MMB up | 7.0 cm up the view | **100.0 cm** up the view |
  | Alt+LMB up | pitch +1.40° | pitch **+20.00°** (orbit) |
  | Alt+RMB up / down | 7 cm closer / away | **100 cm away / 100 cm closer**, as in the editor |
  | Top view RMB right | lagged the cursor | 466–593 cm (the cursor's world distance at that zoom) |

  - **R47_Fly** (about 120 fps):
    - RMB + W for 60 frames flew along the full view vector (pitch −35°) and coasted 168 cm after release. That is the editor's coast at 120 fps (1833 cm/s · (11/12) / 120 / (1/12)).
    - RMB + E climbed straight up 742 cm.
    - Top and back to Perspective restored the camera.
    - A 50-count look down after that turned exactly −10°, and the pawn's tick was off at rest.
- **Not verified:** the feel by hand with a real mouse, and a touchpad. Status: **implemented, awaiting manual verification.**

**Commit:** `66f1098`.

**Known issues / follow-ups**
- **Y direction:** the code matches the UE editor's default directions (measured). If Gevor still sees one gesture inverted, it needs naming (RMB look, LMB, MMB, Alt+LMB orbit or Alt+RMB dolly). Making it differ from the editor would be an option in Preferences (like the existing Invert look), not a change of default.
- **Flight:** like the editor's, it depends on the frame rate: 1667 cm/s at 60 fps, 1833 at 120. This is deliberate, for parity.
- **Pitch limit:** ±89° here; the editor allows ±(90 − ε). Unchanged.

---

## #48 — Viewport crawl fixed at its cause: the reference grid redrawn as an anti-aliased material; shadow budget minimum hold; in-engine stability measurement (2026-10-30)

**Request (Gevor, 2026-10-30, pasted, "strictly follow Rules.md").** Part 1 of two, with the assistant upgrade (#49) to follow:
- Eliminate flickering and shimmering: tune VSM and Lumen, make `StageShadowBudget` switch shadows without jitter, and tune TSR/TAA so thin meshes (truss, stairs, cables) do not crawl during camera moves.
- Zero per-frame Tick, `TObjectPtr`, 0 warnings, STATE.md.

**Gevor's choices (2026-10-30, asked before planning):**
- The symptom he sees is **thin geometry crawling**. Shadow popping and Lumen noise were not reported.
- Fix the causes at a **measured** cost; no blind quality increase.
- Rendering first as #48, the assistant as #49.

The plan was approved in plan mode.

**How it was measured.**
- **New dev command `StageCraft.View.MeasureStability [Frames] [TimingFrames]`** in `StageViewportConsoleCommands.cpp`, compiled out of Shipping. It logs:
  - the resolution the scene is really rendered at (the engine's own `FStaticResolutionFractionHeuristic`, so an automatic screen percentage is resolved);
  - the AA method and quality, the TSR history size and thin-geometry flag, and the shadow and GI quality;
  - the mean GPU frame time over TimingFrames (`RHIGetGPUFrameCycles`);
  - over Frames frames with the camera still, the per-pixel temporal standard deviation of luma (0–255), as mean, p99, p99.9, max, and the share of pixels above 1 and above 4. It also saves a heat map to `Saved/Screenshots/WindowsEditor/Stability_*.png`.
- With a still camera, every bit of that deviation is TSR jitter not being resolved, which is the crawl. No Python is installed on this PC, so the measurement runs in the engine rather than in the planned `MeasureCrawl.py`.
- **Setup:** scripted Standalone `-game` runs on the default stage (`L_StageTest`) with an arc truss and stairs added by the assistant. The viewport was 1184×674 (docked), on an RTX 3060 Ti with 1920×1080 monitors. Logs are `Saved/Logs/R48_Base.log`, `R48_Matrix.log`, `R48_Grid1..3.log` and `R48_SP.log`.
- **Gevor's state:** his workspace session and GameUserSettings were backed up first and restored after every run.

**What the measurements showed (root cause).**

| Still camera, Lit perspective | Mean | p99 | Pixels > 4 | GPU |
|---|---|---|---|---|
| Baseline (1-pixel line grid) | 2.22 | 35.0 | 7.02 % | 6.34 ms |
| Same, grid hidden | 0.39 | 2.15 | 0.24 % | — |
| Same, `r.TSR.ThinGeometryDetection 1` | 2.23 | 35.1 | 7.03 % | 6.16 ms |
| Unlit view (no TSR) | 0.01 | 0.06 | 0.00 % | 1.33 ms |

- **The reference grid caused nearly all of the crawl.** `AStageGridActor` drew 1-pixel `ULineBatchComponent` lines, hard primitives that cover a pixel fully or not at all.
  - Under TSR's sub-pixel jitter every line pixel toggled on and off from frame to frame. TSR rejects that as flicker, so the whole grid crawled, near lines as much as far ones, with moiré toward the horizon.
  - The heat map shows every grid line; the truss, stairs and fixtures barely register.
- **Not the cause, so not changed:**
  - **Screen percentage.** At this viewport size the engine already renders at 100 %.
  - **AA quality.** Gevor's settings are already at quality 3 (TSR history 200 %).
  - **`r.TSR.ThinGeometryDetection`:** no effect.
  - **Lumen and VSM:** they did not show in the heat map.

**What changed.**
1. **The grid is now one translucent unlit quad with a new material, `/Game/StageCraft/Viewport/M_StageGrid`.** The material is built headless by the new `Scripts/Content/create_grid_content.py` (idempotent; it reads back and checks blend mode, shading, opacity channel and emissive path). The material:
   - computes each line's coverage per pixel from the screen-space derivative of the plane coordinate (`fwidth`). A line is therefore a 1-pixel anti-aliased ramp at any distance and zoom, which TSR resolves. This is the method of the Unreal Editor's own grid;
   - fades each line family out once its cells get smaller than about 8 px (gone at about 3 px), per direction, so the minor lines leave before they alias toward the horizon;
   - sends its colour through `EyeAdaptationInverse`, so the lines keep the old grey at any exposure. Without it, the dark default stage's auto exposure made them glow white;
   - pulls the quad toward the camera by 2 mm per metre plus 0.5 cm (world position offset), so it stays in front of a floor it lies on;
   - reads the plane's axes (`AxisU`, `AxisV`) and the axis colours (`ColorU`, `ColorV`) as parameters, so one material serves the floor and the vertical front and side grids.
   - A pitfall the script now guards against: a new `ComponentMask` starts with R and G on, so the first version's opacity read red and drew a grey sheet. All four channels are now set explicitly and read back.
2. **`AStageGridActor`** (Config = Game):
   - The `ULineBatchComponent` is replaced by a `UProceduralMeshComponent` quad (±50 m, built once in BeginPlay) and a `UMaterialInstanceDynamic` (a `TObjectPtr` UPROPERTY).
   - The material is a `Config` soft reference, `GridMaterial` in `DefaultGame.ini`. `/Game/StageCraft/Viewport` is already always cooked.
   - A change of plane, offset or cell size only moves the quad and sets parameters. It still never ticks and is still hidden from renders.
   - If the material is missing, the grid stays hidden and a warning is logged once.
   - `GetLineCount()` became `GetCellSize()`, used by `StageCraft.View.Status`.
3. **`StageViewMath`:**
   - `BuildGridLines`, `FGridLine` and `MaxGridLinesPerDirection` were removed, since they would be dead code.
   - New pure `GetGridSurface(Plane, Offset, Out)` gives the plane's origin, its two in-plane axes and which world axis runs along each.
   - `StageCraft.Viewport.Grid` now tests that function: every plane, unit perpendicular axes, the origin projecting to (0, 0), and a NaN offset refused.
4. **Shadow budget minimum hold** (from the original brief; ADR 0018 §4).
   - Hysteresis alone did not stop popping. With more than 64 lit fixtures, a camera move changes distances by more than 25 % within one 0.25 s re-ranking, so a fixture at the cut-off could switch every time.
   - **New pure `StageShadowBudget::SelectCastersWithHold`:**
     - A lit fixture keeps its shadow state for at least `MinCasterHoldSeconds` (new setting, default 1 s, 0–10) after a switch, and held casters fill the budget first.
     - Going dark still drops shadows at once, and the budget stays a hard limit (the dimmest held casters drop if it shrank).
     - It returns `RetryAfterSeconds` when a held fixture is ranked the other way.
   - **`UShowControlSubsystem`:**
     - It records each switch time in `TMap<TWeakObjectPtr<ALightingFixtureActor>, double> ShadowSwitchTimes`, pruned on unregister.
     - When the selection asks for a retry, it arms the existing one-shot timer for that moment (new `ArmShadowBudgetTimer`). Still event-driven, no tick.
   - **New test `StageCraft.Show.ShadowBudgetHold`:**
     - Two fixtures trading places every 0.25 s for 10 s: no state shorter than the hold, never over budget.
     - Dark drops at once.
     - A held non-caster is not picked, and the retry time is right.
     - A shrunken budget keeps the brightest held caster.
     - Unheld equals `SelectCasters`; a hold of 0 or NaN turns it off.
   - The name is deliberately not `ShadowBudget.Hold`. That would make `ShadowBudget` a group node and hide the existing `StageCraft.Show.ShadowBudget` test, which happened in the first run (109 found instead of 110) and was caught. A scan of all test names found no other such collision.

**Results after the change** (same setup):

| Still camera | Mean | p99 | Pixels > 4 | GPU |
|---|---|---|---|---|
| Lit perspective, new grid (R48_Grid3) | 0.41 | 1.53 | 0.14 % | 6.29 ms |
| Lit perspective, new grid (R48_SP, two runs) | 0.47–0.52 | 2.4–2.6 | 0.20–0.31 % | 5.8–6.0 ms |
| Same at `r.ScreenPercentage 75` | 0.51 | 2.11 | 0.28 % | 4.79 ms |
| Top view, new grid | 0.30 | 0.52 | 0.11 % | 7.19 ms |

- **Unstable pixels:** from 7.0 % down to the level of no grid at all, at the same GPU cost.
- **Internal resolution:** 75 % is now about as stable as 100 %. The planned default of native resolution and its "Viewport resolution" preference were therefore **not** added: no measured benefit, and about 25 % more GPU. A maximised 1920×1080 viewport will render at about 75 % (engine default). If Gevor still sees crawl there, that is the next thing to try.
- **Screenshots:** Perspective Lit, Top, Front (vertical grid behind the stairs) and CAD Lines were checked. Lines are grey, 1 px, anti-aliased, with the red, green and blue axes; Lines mode still shows the grid. They are `Saved/Screenshots/WindowsEditor/ScreenShot00056..62.png`.
- **During R48_Grid3, two Sketch-to-Stage generations and a camera move happened that the script did not issue.** That was most likely real input reaching the focused game window. The measurement ran with the camera still and agrees with the other runs. No scene or autosave file was written by any run.

**Files changed**
- `Source/ModularSceneBuilder/Public/Viewport/StageGridActor.h`, `Private/Viewport/StageGridActor.cpp`
- `Source/ModularSceneBuilder/Public/Viewport/StageViewMath.h`, `Private/Viewport/StageViewMath.cpp`
- `Source/ModularSceneBuilder/Private/Viewport/Tests/StageViewportTests.cpp`
- `Source/ModularSceneBuilder/Private/Viewport/StageViewportConsoleCommands.cpp` (`MeasureStability`, Status shows the cell size)
- `Source/ModularSceneBuilder/Public/Show/StageShadowBudget.h`, `Private/Show/StageShadowBudget.cpp`, `Private/Show/Tests/StageShadowBudgetTests.cpp`
- `Source/ModularSceneBuilder/Public/Subsystems/ShowControlSubsystem.h`, `Private/Subsystems/ShowControlSubsystem.cpp`
- `Content/StageCraft/Viewport/M_StageGrid.uasset` (new), `Scripts/Content/create_grid_content.py` (new)
- `Config/DefaultGame.ini` (`[/Script/ModularSceneBuilder.StageGridActor] GridMaterial`)
- `Docs/ADR/0008-viewport-toolbar-projections-and-view-modes.md`: §4.4 amended with the measurements; §5 now notes that the rejected "grid as a material" was revisited, and why the objections no longer hold; F6 note.
- `Docs/ADR/0018-ue-camera-shadow-budget-shapes-and-assistant-edits.md` §4 (minimum hold), `Docs/CURRENT_SYSTEMS.md`, `tasks/todo.md` (Phase 25), `STATE.md`
- `Config/DefaultEngine.ini` is unchanged: none of the planned cvars earned their place.

**Verification**
- **Builds:** Editor, Game Development and Game Shipping (`-DisableAdaptiveUnity`) all **succeeded with 0 warnings**. They ran through UBT with `-NoHotReloadFromIDE`, because Gevor's Kmidden2 editor was open.
- **Automation: 110/110 `StageCraft.*` passed** (exit code 0). That is 109 plus `Show.ShadowBudgetHold`, with `Viewport.Grid` rewritten.
- **The stability runs above.**
- **Fixture bench** (`Scripts/Bench/RunFixtureBench.ps1 -Counts 50,200,500`, CSV `Saved/StageCraft/Benchmarks/FixtureBench_20261030-124756.csv`):

  | Fixtures | Idle frame (mean / p95) | Crossfade frame (mean / p95) | GPU in crossfade | Dropped in crossfade |
  |---|---|---|---|---|
  | 50 | 7.3 / 9.3 ms | 7.9 / 10.0 ms | 6.7 ms | 0 |
  | 200 | 8.1 / 10.1 ms | 10.2 / 11.8 ms | 8.0 ms | 1 |
  | 500 | 9.5 / 11.3 ms | 15.9 / 18.6 ms | 10.1 ms | 100 of 631 |

  - Kmidden2 was open on the same GPU, so this is a regression check (it runs, nothing new in the log), not a clean before/after.
  - #39 measured 27–33 ms per frame at 500 crossfading fixtures, before the #45 budget.
  - The known VSM "One Pass Projection max lights overflow" warning (#39) appeared once.
- **Not verified by hand:** the crawl while orbiting and flying with a real mouse. The metric is for a still camera; motion is the same mechanism, but Gevor's eye is the final check. Status: **implemented, awaiting manual verification.**

**Commit:** `9193448`.

**Known issues / follow-ups**
- **Remaining unstable pixels (0.1–0.3 %):** these are the moving-head beams (volumetric) and the arc truss lattice, as the heat map shows. The truss lattice is a procedural mesh with no Nanite or distance field. It only matters if Gevor sees it crawl.
- **Maximised viewport:** it renders at about 75 % by the engine's automatic rule (see the table). Not changed, since there is no measured benefit at 100 %.
- **Shadow budget frame-time gain:** still not measured cleanly. The #45 follow-up stands: bench with Kmidden closed, budget on vs off.
- **Lumen and VSM** were not tuned: no measured symptom. This is the honest outcome of "no blind quality cranking".

## #49 — Professional fixture suite: every fixture type, full optics, true colour mixing, gel libraries, dimensions and photometrics, inspector and assistant control (2026-10-30)

**Request (Gevor, 2026-10-30, pasted, "strictly follow Rules.md"):**
- Fixture types: moving head spot, wash, beam and hybrid; LED pars; strobes; blinders; pixel bars; profiles with framing shutters.
- Optics: beam/zoom, static and rotating gobo wheels, prisms, frost, barndoors.
- Colour: true RGBW, CMY subtractive, CTO correction, Lee/Rosco gel presets.
- Physical data: dimensions, weight, lumens, candela, throw.
- UI and assistant fine-tuning.
- Standards: `TObjectPtr`, no tick, tests, 0 warnings, performance stability, STATE.md #49.

**Gevor's answers before design:**
- Pixel bars get per-cell colour (cues and DMX).
- Size means catalog dimensions per model; per-placement editing is beam and accessories.
- Assistant fixture commands come now; the bigger assistant upgrade planned as #49 moves to #50 (tasks/todo.md Phase 26).

**Design:** Docs/ADR/0020-professional-fixture-suite.md.

**What existed before:**
- Spot, beam, wash, par, strobe and laser kinds; RGB, RGBW and CMY systems.
- CTC, one gobo wheel, iris, framing shutters, zoom and cone photometrics.
- One attributes map that cues, scenes, DMX and the inspector all carry.

**What was wrong or missing:**
- Bodies were 1 m engine spheres.
- The inner cone was 0.7 × the outer, so there was no beam versus field angle.
- RGBW white added pure (1, 1, 1), and CMY ignored the lamp's white.
- Nothing else in the request existed.

### What changed

**Data model (`ULightingFixtureData`).**
- **New kinds** (appended): Hybrid, Profile, Fresnel, Blinder, Pixel Bar.
- **Colour:**
  - RGBA and RGBWA colour systems.
  - `Emitters` (colour × relative flux; defaults R/G/B, white at `WhiteEmitterKelvin`, amber at about 590 nm).
  - CTO (`bHasCTO`, +159 mired at full) and a colour wheel.
  - Gel frame (`bAcceptsGel`) for white-only fixtures.
- **Optics:**
  - Static gobo wheel and rotating-gobo spin.
  - Prisms (facets, circular or linear, spread, transmission).
  - Frost (`FrostFieldMultiplier`) and `FieldAngleRatio`.
  - `EStageBeamShaper {None, FramingShutters, Barndoors}` replaces the `bHasFramingShutters` bool. `PostLoad` migrates old assets through the `_DEPRECATED` property.
- **Cells:** `CellCount` (up to 32) and `CellControl` (colour, dimmer, or both).
- **Body:** `FStageFixtureBody` (base, yoke, head, lens, cell strip). `FStageEquipmentSpecs::DimensionsCm` is added for every item.
- **DMX:** layouts are generated in console order with a block per cell. Validation covers wheel and DMX limits, emitter sets, colour cells without a colour system, and a body larger than the stated size.

**Cells as attributes.**
- `StageCraft.Attribute.Cell.01..32.{Dimmer, ColorR/G/B/W/A, ColorRGB}` are native tags (`StageFixtureCells`).
- Cues, fades, scene files (still format v3), DMX and the inspector handle cells with no new path.

**Colour science (`StageColorScience`, pure).**
- Additive emitter mixing with a luminous fraction.
- Subtractive lamp × filters with a CTO mired shift.
- Filters and a combined-cells mix.
- `MatchAdditive`: the brightest reproduction of a colour. White and amber amounts are found by ternary search on a convex peak, so white is every emitter at full.

**The fixture actor (`ALightingFixtureActor`).**
- **Beam mix:** `GetBeamMix` drives the light colour. Intensity is rated flux × dimmer × mix fraction ÷ the light colour's luminance (floor 0.05).
- **Cones:**
  - The outer cone is the field angle and the inner cone is 2 × beam − field.
  - Frost widens both angles.
  - A prism widens the cone so its copies fit.
  - While a projection is on, the light function draws the edge.
- **Light function v2** (`M_StageBeamLightFunction`): both gobo wheels, gobo and prism spin on the material's clock (phase and start reset on a speed change, so no tick), prism copies, frost blur, soft barndoors, and `ConeToBeam`.
- **Lens and cell glow** (`M_StageFixtureEmitter`, one MID):
  - A cell texture is uploaded only when a cell's colour changes.
  - Dimmer and strobe are scalars, so fades and flashes never re-upload.
  - A multi-cell fixture casts one light.
- **Yoke and head** are scaled to the body from their mesh bounds.
- **Gel picker:** a library dropdown with an `AppliedGel` label (SaveGame), cleared by any other colour edit.
- **Colour rows:** the master row, All Cells and per-cell rows solve over every emitter and read back the mix.
- **New inspector content:** Cells, Fixture (type, size, rated output) and Photometrics field angle; barndoor labels.
- **Open white defaults to W = 100 %** (amber stays at 0).

**Gel library.**
- `Scripts/Content/Data/StageGels.csv`: 118 gels (72 Lee, 46 Rosco). **The colours are sRGB approximations of the swatches, not measured spectra.**
- It becomes `DT_StageGels` (`FStageGelRow`), set in `UStageLightingSettings::GelTable` (Project Settings > Game > StageCraft Lighting).
- `FStageGelLibrary` validates the rows, sorts them, and finds gels by code, brand + number, unique bare number, or name / unique part of a name.

**Fixture library** (`Scripts/Content/create_fixture_library_content.py`, headless; it also builds both materials and the gel table): eleven generic models in `/Game/StageCraft/Fixtures`.

| Model | DMX channels | Notable features |
|---|---|---|
| Spot 470 | 21 | CMY + CTO, colour wheel, spinning and static gobos, 3-facet prism, frost, iris |
| LED Wash 19x40W RGBW | 14 | |
| Beam 280 | 12 | 8-facet and linear prisms |
| Hybrid 440 | 21 | |
| Profile 600 (moving, framing shutters) | 22 | |
| LED Par 12x15W RGBWA | 8 | |
| LED Strobe 1000 | 3 | |
| LED Blinder 4-Cell 3200 K | 7 | |
| LED Pixel Bar 1 m 16x RGBW | 67 | |
| Ellipsoidal 26° 750 W | 1 (dimmer) | Gel frame, shutters, gobo slot, iris |
| Fresnel 1 kW | 1 (dimmer) | Gel frame, barndoors |

- Every model has dimensions, weight, power, flux and a base mesh built at its real size (`/Game/StageCraft/Lighting/FixtureMeshes`).
- New categories: Lighting.Conventional and Lighting.Pixel.

**Assistant `set_fixture`** (offline English and Armenian; Claude schema `fixture_settings`).
- **Controls:** dimmer, colour by name, Lee/Rosco gel, zoom (degrees or narrow/wide), frost, iris, CTO, prism and its spin, gobo (slot or name) and its spin, strobe Hz, pan and tilt. Several are allowed in one prompt.
- **Targets:** the selection, or named fixtures with a count.
- **Execution:** one `RequestParameterChange` per fixture and attribute, inside an edit transaction.
- **Grammar boundaries:** "remove the red pars" still deletes, "change the spots to beams" still replaces, and "purple elephant" is not a command.
- **Armenian:** the lexicon gained the colour and control words; "դարձրու" now reads as "make".

### Files

**Code**
- **Changed:**
  - `Public/Data/LightingFixtureData.h`, `Private/Data/LightingFixtureData.cpp`
  - `Public/Data/StageItemTypes.h`, `Private/Data/StageItemTypes.cpp` (DimensionsCm, two categories)
  - `Public/Data/StageParameterTypes.h`, `Private/Data/StageParameterTypes.cpp` (new attributes and fixture rows)
  - `Public/Actors/LightingFixtureActor.h`, `Private/Actors/LightingFixtureActor.cpp`
  - `Public/Show/StagePhotometry.h`, `Private/Show/StagePhotometry.cpp` (frost, field, inner cone)
  - `Public/AI/StageAITypes.h`, `Private/AI/StageAITypes.cpp`
  - `Public/AI/StageAICommandParser.h`, `Private/AI/StageAICommandParser.cpp`
  - `Private/AI/StageAICommandSchema.cpp`, `Private/AI/StageAICommandRunner.cpp`, `Private/AI/StageAIArmenian.cpp`
- **New:**
  - `Public/Show/StageColorScience.h`, `Private/Show/StageColorScience.cpp`
  - `Public/Show/StageFixtureCells.h`, `Private/Show/StageFixtureCells.cpp`
  - `Public/Show/StageGelLibrary.h`, `Private/Show/StageGelLibrary.cpp`
  - `Public/Show/StageLightingSettings.h`

**Tests**
- **New:** `Private/Show/Tests/StageFixtureSuiteTests.cpp`, `Private/AI/Tests/StageAIFixtureTests.cpp`.
- **Changed:**
  - `Private/Show/Tests/StageLightingTests.cpp` and `StageFixturePlaybackTests.cpp`: `BeamShaper`; the beam lumens now follow the mix.
  - `Private/AI/Tests/StageAIArmenianTests.cpp`: 40 help examples.

**Content**
- **New:**
  - `Content/StageCraft/Fixtures/DA_Fixture_*` (11)
  - `Content/StageCraft/Lighting/DT_StageGels`, `M_StageFixtureEmitter`
  - `Content/StageCraft/Lighting/FixtureMeshes/SM_FixtureBase_*` (11)
- **Rebuilt:** `Content/StageCraft/Lighting/M_StageBeamLightFunction`.

**Scripts and config**
- **New:** `Scripts/Content/create_fixture_library_content.py`, `Scripts/Content/Data/StageGels.csv`.
- **Changed:**
  - `Scripts/Content/create_lighting_content.py`: no longer builds the light function; uses `beam_shaper`.
  - `Scripts/Bench/RunFixtureBench.ps1`: new `-Item` switch.
  - `Config/DefaultGame.ini`: Fixtures scanned as StageItems, the emitter material and lens / strip meshes, the gel table.

**Docs:** `Docs/ADR/0020-professional-fixture-suite.md` (new), `Docs/CURRENT_SYSTEMS.md`, `tasks/todo.md` (Phase 27; Phase 26 moved to #50), STATE.md (#48 commit hash `9193448` recorded).

### Verification
- **Builds:** Editor, Game Development and Game Shipping (`-DisableAdaptiveUnity`) succeeded with **0 warnings**. They were built with `-NoHotReloadFromIDE` while Kmidden2 was open.
- **Automation:** **117/117** `StageCraft.*` tests pass. 7 new tests:
  - Lighting.ColorScience, GelLibrary, FixtureSuiteData, FixtureSuiteActor, FixtureLibraryAssets.
  - AI.Fixtures (34 English forms, 13 Armenian, targets, grammar boundaries) and AI.FixtureSchema.
  - "Found 117" was checked, so no test name hides another.
- **Changes to older tests**, each from a deliberate behaviour change:
  - PanTiltTransform now expects the mix-based beam lumens.
  - The Armenian help count is 40, for 8 new examples.
- **Tests caught and fixed during the work:**
  - The first, greedy RGBW match gave half the output for white.
  - "purple elephant" parsed as a colour command.
  - `_DEPRECATED` properties are registered under their old name.
- **Content script:** all 11 fixtures saved and read back; the gel table has 118 rows matching the CSV. The library asset test validates every model with 0 errors and 0 warnings.
- **Scripted `-game` runs** (R49, R49b; workspace session and GameUserSettings backed up and restored):
  - The assistant placed all 11 models.
  - The English fixture commands in the run were accepted, among them tilt, gobo + prism, colours, frost, iris, CTO, both gels and dimmer. Armenian was covered by the automation tests only.
  - R49 found "prism 2 on the beam 280" refused: "beam" was a filler word. Fixed, with a regression test.
  - The screenshot shows the star gobo projected three times by the prism, plus the coloured beams. The fixtures were about 26 m from the camera, so the lens glow, pixel cells and barndoor edges were **not** judged by eye.
  - `StageCraft.View.MeasureStability` on that frame: GPU 5.98 ms, 0.43 % of pixels unstable. #48's still frames were 0.14–0.31 %, but this frame has prisms, gobos and volumetric beams.
  - No scene or autosave was written.
- **Fixture bench** (`RunFixtureBench.ps1 -Counts 50,200,500`, 10 s idle + 10 s crossfading). Kmidden2 was open on the same GPU, so these are regression checks, not a clean A/B:

  | Fixture | 50 crossfade | 200 crossfade | 500 crossfade | 500 idle |
  |---|---|---|---|---|
  | DA_MovingHead_Test (same as #48) | 8.17 ms | 10.19 ms | 16.95 ms (game 16.96, GPU 10.15) | 9.74 ms |
  | #48, for reference | 7.9 ms | 10.2 ms | 15.9 ms | |
  | DA_Fixture_Spot470 (CMY + CTO, lens, real body) | 8.17 ms | 12.18 ms | 20.95 ms (GPU 10.33) | 13.51 ms (GPU 10.25) |

  - The richer fixture costs about 4 ms more at 500 crossfading. Most of it is visible at idle (GPU 10.25 vs 8.0 ms: 500 more lens meshes and the bigger body).
  - Caching the beam mix (computed once per colour change) was tried and measured: 21.08 vs 20.95 ms, no gain. It was reverted under Rules §22.
- **No tick:** fixtures still never tick (asserted in a test). Spin runs on the material's clock. The cell texture uploads only on a colour change.
- **Not verified by hand:**
  - The library in the Library panel.
  - The inspector rows (gel picker, colour wheel, prism, cells, Fixture section).
  - Spinning gobos and prisms up close.
  - The lens and pixel glow.
  - Barndoors.
  - Typing the assistant commands in the panel.

  Status: **implemented, awaiting manual verification.**

**Commit:** `3f598a0`.

**Known issues / follow-ups**
- **Visible change:** an RGBW fixture saved with W = 0 is now about half as bright (true mixing). New placements default to W = 100 %.
- **Bodies are simplified:** a box base, a box yoke crossbar, a cylinder or box head. Real meshes can replace them per model without code changes.
- **Gel colours** are approximations.
- **A multi-cell fixture casts one light.**
- **A prism copy's intensity** is an even share.
- **Assistant limits:**
  - "strobe" is a control word, so the strobe fixture must be named "the strobes" or selected.
  - Negative pan/tilt values cannot be typed ("-" is dropped).
  - Attribute edits are not undo steps (as before; cues record them).
- **Content-script warning:** headless runs create a transient `StaticMeshEditorSubsystem`, which UE deprecates; it is the only headless path.
- **Pending:** the assistant upgrade (knowledge base, multi-step, memory) is #50, and #48's manual crawl check is still pending.

---

## #50 — Community marketplace Phase A: `.stage` packages, marketplace backend and subsystem, credit purchases of listings (2026-11-01)

**Request (Gevor, 2026-11-01, pasted, "strictly follow Rules.md"):** the Community Marketplace and Cloud Showcase.
1. `.stage` project packages: fixtures, multi-cell channels, DMX patch, truss geometry, cues.
2. A marketplace panel: browse, filter by category, popularity and price, read-only 3D preview.
3. Assistant marketplace navigation: search, preview, import.
4. Standards: `TObjectPtr`, encapsulation, no tick polling, 0 warnings, all tests, STATE.md.

**Gevor's answers before design:**
- **Backend:** a backend seam plus a local "community folder" backend. An HTTP backend comes once a server API exists. There is no server today.
- **Money:** credits only. Real-money prices exist in the data but are refused unless the commerce backend is an authoritative server (Rules.md Economy security 7, §66).
- **Delivery:** phased. This entry is **Phase A**: format, backend, subsystem, purchases, tests. Phase B (panel and read-only preview) and Phase C (assistant navigator) are planned in the ADR and in tasks/todo.md Phase 28.

**Design:** Docs/ADR/0021-community-marketplace.md. ADR 0021 and #50 were reserved for the assistant upgrade (Phase 26), which now takes the next free entry and ADR 0022, as it did when the fixture suite took #49.

**What & why**
- **A `.stage` package (`StagePackage`) is a container around the existing scene format (v3), not a second serializer.**
  - **It holds everything a stage needs.** Item SaveGame state already holds fixture cells and attributes, the DMX patch and span/truss geometry, and the scene holds the cues and environment.
  - **The container** is the magic `STAGEPKG`, a version, and a table of sections, each with a FourCC, a codec (stored or zlib), sizes and a SHA-1.
  - **The sections** are `MNFT` (manifest JSON, 256 KiB cap), `SCEN` (the scene JSON, 64 MiB cap) and an optional `THMB` (PNG, 2 MiB cap). A whole package is capped at 96 MiB.
  - **Browsing reads only the manifest.** The scene is never inflated for a query.
  - **The reader treats every package as untrusted.** It refuses the whole package on:
    - a bad magic, version, section count or repeated section;
    - an out-of-bounds or wrapping size, trailing bytes, or an unknown codec;
    - any size over its cap, checked before allocating (no decompression bombs);
    - a size or hash mismatch;
    - a scene the parser had to repair, or stats that do not match the scene;
    - imported models (`NotPortable`, since those exist only on the machine that imported them).
  - **What the manifest holds.** Id, title, description, author, kind (StageDesign / LightingRig / CueList), category, keywords, price (credits plus a real-money amount and ISO code), creation date, stats (items, fixtures, cues) and the required catalog items.
  - **How it is validated.** Text is normalized, and over-long fields are refused, never cut. Required items are recomputed from the scene, never trusted. Categories are native tags `StageCraft.Marketplace.Category.*`, and unknown ones read as Other.
- **`StageMarketplaceRules`** (pure, tested) holds four things:
  - **Query filtering:** every search word must appear; kinds, exact category and Free/Paid filters apply.
  - **Sorting and paging:** sort by Popular, Newest, price either way or Title, with deterministic tie-breaks, and real money sorted after credits. Paging is clamped.
  - **Ownership and purchase checks:** listing ownership (Free / Owned / Purchasable / Locked) and the listing-purchase validation order.
  - **Import names:** unique scene names for imports ("Title", "Title (2)", ...). An import never replaces a scene.
- **`UStageMarketplaceBackend`** (abstract) has three calls: Query, FetchPackage (Acquire or Preview; only acquisitions count as downloads) and Publish. Each answers exactly once on the game thread.
  - **`UStageLocalMarketplaceBackend`** runs `FStageLocalMarketplaceStore` on one `UE::Tasks::FPipe` and answers through `AsyncTask(GameThread)` with a weak `this`. `BeginDestroy` waits for the pipe.
  - **The folder** is `Saved/StageCraft/Marketplace/Community`, or `CommunityDirectory` in DefaultGame.ini (a shared folder makes a LAN showcase). Popularity is kept in `downloads.json`.
  - **Browsing** skips unreadable files and files whose manifest id differs from their file name (a renamed copy).
  - **Publishing** re-reads the whole package and writes atomically with KeepExisting.
- **`UStageMarketplaceSubsystem`** (GameInstance) does three things:
  - **Queries.** A newer query supersedes an older one by serial number. Results are pushed on `OnListingsUpdated`.
  - **`RequestAcquire`.**
    - Free or owned listings: fetch, verify, then write a new scene into the scenes folder and keep a copy in `Saved/StageCraft/Marketplace/Library`.
    - Paid listings: buy through the economy first.
    - At most one acquisition per listing is in flight. The result arrives on `OnAcquireCompleted`.
    - It never spawns actors. Opening an import uses the existing scene load, so catalog, entitlements, rules, undo and the unsaved-changes prompt all apply.
  - **`RequestPublish`** publishes a saved scene (what is on disk is what gets published).
    - On the game thread: the text, price, thumbnail and catalog-loaded checks, and the light-item paths for the fixture count.
    - On the work pipe: load the scene, check portability, build the package. The backend then publishes it.
    - A real-money price without a server is refused with `ServerRequired`. One publish runs at a time.
- **Economy:**
  - **Listing purchases.** New `CanPurchaseListing` / `RequestListingPurchase` / `OnListingPurchaseCompleted`. The offer is read from `UStageMarketplaceSubsystem::FindListing`, never from the caller, and snapshotted per transaction, so a newer query cannot change or lose a pending purchase.
  - **Settlement** goes through the commerce backend with `ProductId` `StageListing:<guid>` and a fresh TransactionId. The snapshot is validated again when the backend answers, then committed all or nothing.
  - **New result and backend flag.** `EStageEconomyResult::ServerRequired` (appended) and `UStageCommerceBackend::CanSettleRealMoney()`, which is false for the local backend.
  - **Catalog-only rule kept.** Products are still catalog-only; listings use this separate validated operation (Economy security 4 intact).
- **Profile:**
  - **New field.** `FStagePlayerProfile::OwnedListings` (SaveGame), with `FStagePurchaseRecord::ListingId` and `OwnsListing()`. `CommitPurchase` adds the listing.
  - **Integrity hash.** It adds one sorted `O:<guid>` part per owned listing. A profile that owns none hashes exactly as before, so there is no version bump and no wallet reset (verified with Gevor's 2026-10-06 save).
- **Smaller changes:**
  - `FStageSceneStore::GetDefaultDirectory()` is now the one source for the scenes folder (used by `UStageSceneComponent`).
  - New dev-only console commands `StageCraft.Marketplace.Query/Status/Get/Publish`, compiled out of Shipping.

**Files changed**
- New:
  - `Docs/ADR/0021-community-marketplace.md`
  - `Source/ModularSceneBuilder/Public/Marketplace/StageMarketplaceTypes.h`, `StageMarketplaceBackend.h`, `StageMarketplaceSubsystem.h`
  - `Source/ModularSceneBuilder/Private/Marketplace/StageMarketplaceTypes.cpp`, `StagePackage.h/.cpp`, `StageMarketplaceRules.h/.cpp`, `StageLocalMarketplaceStore.h/.cpp`, `StageMarketplaceBackend.cpp`, `StageMarketplaceSubsystem.cpp`, `StageMarketplaceConsoleCommands.cpp`, `Tests/StageMarketplaceTests.cpp`
- Changed:
  - `Source/ModularSceneBuilder/Public/Economy/StageEconomyTypes.h` (ServerRequired, ListingId, OwnedListings)
  - `Public/Economy/StageCommerceBackend.h` (CanSettleRealMoney)
  - `Public/Subsystems/StageEconomySubsystem.h`, `Private/Subsystems/StageEconomySubsystem.cpp` (listing purchases)
  - `Public/Subsystems/StageProfileSubsystem.h`, `Private/Subsystems/StageProfileSubsystem.cpp` (OwnsListing, commit, hash)
  - `Private/Scene/StageSceneStore.h/.cpp` (GetDefaultDirectory), `Private/Scene/StageSceneComponent.cpp`
  - `Config/DefaultGame.ini` (MarketplaceBackendClass, CommunityDirectory)
  - `tasks/todo.md` (Phase 28 added; Phase 26 renumbered to ADR 0022)
- No content assets, maps or input actions changed.

**Verification**
- **Builds:** Editor, Game and Shipping (`-DisableAdaptiveUnity`) all succeeded with 0 warnings. The Shipping executable contains no `StageCraft.Marketplace` command strings; the Development game executable does.
- **Automation:** 124/124 pass (117 before plus 7 new `StageCraft.Marketplace.*` tests):
  - Package.RoundTrip
  - Package.Untrusted (magic, version, counts, truncation, trailing bytes, wrapping size, repeated section, codec, caps, hash, damaged scene while browsing still works, oversize file)
  - Manifest
  - Query
  - PurchaseRules (every refusal branch, no real purchase)
  - SceneName
  - LocalStore (temporary folder, removed afterwards)
- **Scripted Standalone `-game` runs by Claude.** The profile save and the workspace session were backed up and restored; the community folder, library and imported scenes were removed.
  - **Run 1:**
    - The old profile loaded with "integrity ok", 5000 credits.
    - R42_Sketch was published free and for 150 credits: 15 items and 9 fixtures counted.
    - The query listed 2 listings, shown as Free and Purchasable.
    - Getting the free one imported "R42_Sketch (2)".
    - Getting the paid one bought it through the local commerce backend (5000 → 4850, integrity still ok, listing then Owned) and imported "(3)".
    - Getting both again re-imported them with no charge.
    - Each listing showed 1 download after the first acquisition (the counter after the re-acquisition was not logged).
    - "festival nothing" returned 0 listings.
    - A publish sent while another was running was refused as Busy.
  - **Run 2:**
    - The free listing was imported as "R42_Sketch (2)".
    - `StageCraft.Scene.Load "R42_Sketch (2)"` placed 15 items with 0 refused, unknown, invalid or repaired; fixtures re-registered and DMX-patched.
    - The screenshot (Saved/Screenshots/WindowsEditor/ScreenShot00087.png) shows the deck, riser, truss loop with beams and the freehand truss.
    - Publishing "No Such Scene" was refused as NotFound. Its message was wrong ("no longer in the marketplace"); it was fixed to say the scene is not saved, and the builds and 124/124 above were run after that fix.
- **Not verified:**
  - No UI exists yet (Phase B), so nothing was verified by hand in a panel.
  - The real-money refusal is covered by tests only; the console cannot set a real-money price.
  - Status: **implemented, awaiting manual verification** (console commands in a `-game` or PIE session).

**Incident: R42_Sketch.json deleted and regenerated**
- My runtime harness deleted Gevor's `Saved/StageCraft/Scenes/R42_Sketch.json` twice.
  - First, a regex edit emptied the step list, and the cleanup by folder diff treated the existing scene as new.
  - Second, I sent a run in parallel with a rewrite that the tool had refused, so the old script ran again.
- `Saved/` is gitignored, and `Remove-Item` skips the Recycle Bin.
- **Recovery:** the scene was regenerated by replaying the original sketch commands from `Saved/Logs/R42_Run2.log`. It is the same layout (2 decks with 36 panels, 2 truss runs with 25 sections, 7 fixtures; 15 items).
- **Difference:** the fixtures are now `DA_Fixture_Beam`, the Sketch default since #49, where the original had `DA_MovingHead_Test`. New instance ids; 27,962 bytes, the original was 34,357.
- **Prevention:** harness cleanup now deletes only names the run's log reports as imported and refuses the source scene. A lesson was saved to Claude's memory.

**Commit:** `00330fb` (committed by Claude at Gevor's one-time request, 2026-11-01).

**Known issues / follow-ups**
- **Phase B:** the Marketplace panel, publish dialog and read-only preview in a separate `GamePreview` world (needs an ADR addendum first). Controller requests `RequestMarketplaceAcquire` / `RequestMarketplacePublish`.
- **Phase C:** assistant intents, with an explicit confirmation turn before any purchase.
- **Imports are whole scenes:** every kind imports as a new scene. Merging a LightingRig or CueList package into the open stage needs fixture-id remapping.
- **`NotPortable`:** stages with imported models cannot be published until model files are embedded.
- **Browsing reads whole files:** a query loads each package file to read its manifest. Use a prefix read or a cache if folders get large, measured first.
- **The local backend pays no seller and has no authentication.** It handles credits only; real money needs the HTTP/server backend, and the request must then carry the real-money price (ADR 0021 §11).
- **Engine log on damaged packages:** a damaged zlib section makes the engine log its own LogCompression error before the package is refused.

---

## #51 — Community marketplace Phase B: Marketplace panel, publish workflow, isolated read-only 3D preview (2026-11-01)

**Request (Gevor, 2026-11-01, pasted, "strictly follow Rules.md"):** Phase B of the Community Marketplace.
1. A performant marketplace panel: search, filters by category, kind and price, publisher, rating and credits. A publish workflow: pick a saved scene, add a description and tags, set a credit price, publish to the community folder.
2. A safe, isolated, read-only 3D preview of `.stage` packages before getting them.
3. Standards: `TObjectPtr`, encapsulation, no per-frame Tick, all tests, 0 warnings, STATE.md, Editor/Game/Shipping builds.

**Gevor's answers before design:**
- **Rating:** downloads now, ratings later. Cards show downloads as popularity. Real ratings need accounts on a server, because a shared folder lets anyone stuff the votes.
- **Paid preview:** yes, read-only. Any listing can be inspected before buying.

**Design:** Docs/ADR/0021-community-marketplace.md §12 (Phase B addendum). It replaces §6–7 where they differ: the panel is UMG, not Slate, and the preview is a render target, not an `SViewport`.

**What & why**
- **`UStageMarketplaceWidget`** (`StageCraft.Panel.Marketplace`, `DisableNativeTick`).
  - **The panel:** a code-built UMG panel, like every workspace panel.
    - Registered by `DA_Panel_Marketplace` (made by `Scripts/Content/create_marketplace_content.py`).
    - Opens floating at 1180 × 760, like Sketch.
    - Listed in the Window menu automatically, plus Edit › Community Marketplace....
  - **BROWSE page:**
    - Search on Enter. Kind chips ALL / STAGE / RIG / CUES and price chips ANY / FREE / PAID. Category and sort dropdowns.
    - 24 cards per page with PREV / NEXT. A page needs no list virtualization.
    - Each card shows the title, publisher, kind, category, counts, downloads, and the price or ownership badge.
    - A details column shows the publisher, publish date, stats, size, tags and description, with three actions:
      - PREVIEW 3D
      - GET (FREE) / BUY · N CREDITS / GET AGAIN (OWNED), disabled with a reason when Locked
      - OPEN IN STAGE
    - The header shows the backend and the credit balance.
  - **PUBLISH page:** saved-scene dropdown, title, description (multi-line), comma-separated tags, kind, category, price in credits and PUBLISH. The browser queries again on success.
  - **Binding:**
    - Reads come from the marketplace subsystem, the profile and the session.
    - Writes go only through the controller.
    - Delegates are bound in NativeConstruct and unbound in NativeDestruct; there is no polling.
    - The card list is rebuilt only by query answers (async, a later frame), never inside a card's own click.
- **Controller:** new `RequestMarketplaceAcquire(FGuid)` and `RequestMarketplacePublish(Request)`. Refusals take the common `ReportRejection` path (status bar and error cue), mapped to `FStageEconomyResultInfo`.
  - OPEN IN STAGE uses `RequestLoadScene`.
  - With unsaved changes, the first click turns the button into "DISCARD CHANGES AND OPEN" and a second click is required. This is the same duty File › Open has.
- **Subsystem:** new `RequestPreview(FGuid)` plus the native `OnPreviewReady(ListingId, Result, TSharedPtr<const FStageSceneDocument>)`.
  - It takes only listings the backend returned.
  - It fetches with purpose Preview, which is not counted as a download.
  - It verifies the whole package on the work pipe with `StagePackage::Read`, the same checks as an import, and requires the id to match.
  - A newer preview supersedes an older one. Nothing is written.
  - The panel shows whichever preview is newest, so a console or a future assistant request appears in it too.
- **`FStageMarketplacePreview`** (an `FGCObject` owned by the panel):
  - It builds an `FPreviewScene`: a GamePreview world with its own light and sky, a plain grey floor, and a `USceneCaptureComponent2D` drawing into a 1280 × 720 render target.
  - It renders only on change: a new scene, an orbit drag or a wheel zoom. FRAME resets the view. There is no tick and no timer.
  - `StageMarketplacePreview::SpawnScene` spawns catalog items the way `RestoreItem` does: deferred spawn, `RestoreSnapshotState`, `InitializeFromItemData`, `FinishSpawning`. Items this build lacks are counted, not spawned.
  - **Read-only by construction:**
    - No StageCraft world subsystem exists in a GamePreview world.
    - `FPreviewScene` never begins play, so actors never register with anything.
    - There is no controller, spawn system, selection or history in that world.
  - It is destroyed with the panel.
- **`UStageMarketplacePreviewWidget`** shows the image and turns a drag (orbit) or the wheel (zoom) into calls on the preview. `DisableNativeTick`.
- **New dev console commands:** `StageCraft.Marketplace.Preview <n|id>` and `StageCraft.Marketplace.Screenshot` (Slate capture of the floating window), compiled out of Shipping.

**Files changed**
- New:
  - `Source/ModularSceneBuilder/Public/UI/StageMarketplaceWidget.h`, `Private/UI/StageMarketplaceWidget.cpp`
  - `Public/UI/StageMarketplacePreviewWidget.h`, `Private/UI/StageMarketplacePreviewWidget.cpp`
  - `Private/Marketplace/StageMarketplacePreview.h/.cpp`
  - `Scripts/Content/create_marketplace_content.py`
  - `Content/StageCraft/UI/Panels/DA_Panel_Marketplace.uasset` (made by that script)
- Changed:
  - `Public/Marketplace/StageMarketplaceSubsystem.h`, `Private/Marketplace/StageMarketplaceSubsystem.cpp` (RequestPreview, OnPreviewReady)
  - `Private/Marketplace/StageMarketplaceConsoleCommands.cpp` (Preview, Screenshot)
  - `Private/Marketplace/Tests/StageMarketplaceTests.cpp` (PreviewIsolation)
  - `Public/Player/ModularPlayerController.h`, `Private/Player/ModularPlayerController.cpp` (marketplace requests)
  - `Public/Workspace/StageWorkspaceTypes.h`, `Private/Workspace/StageWorkspaceTypes.cpp` (Panel_Marketplace tag)
  - `Private/Workspace/StageWorkspaceShell.cpp` (floating window size)
  - `Private/Workspace/SStageWorkspaceMenuBar.cpp` (Edit › Community Marketplace...)
  - `Docs/ADR/0021-community-marketplace.md` (§12 addendum, status)
  - `tasks/todo.md` (Phase B)

**Verification**
- **Builds:** Editor, Game and Shipping (`-DisableAdaptiveUnity`) all succeeded with 0 warnings. The Shipping executable contains no `StageCraft.Marketplace` command strings.
- **Automation:** 125/125 pass (124 before plus 1 new). The new `StageCraft.Marketplace.PreviewIsolation` test checks that:
  - a GamePreview world has no session, show control, DMX patch or edit history;
  - two catalog items spawn and one unknown item is counted;
  - the preview actors never begin play and live in the preview world;
  - the fixture is at its saved place.
- **Scripted Standalone `-game` runs by Claude** (R51, R51b, R51c; the safe harness only removes scenes the log reports as imported; the profile and session were restored):
  - Opening `StageCraft.Panel.Marketplace` gave a floating "Community Marketplace" window.
  - Two publishes of R42_Sketch (free and 150 credits) appeared in the panel's cards as soon as a query answered.
  - `Preview 1` showed the read-only preview: 15 placed, 0 not in the library. The screenshot shows the decks, riser, truss loop, freehand truss and fixtures. Previewing the paid listing bought nothing.
  - The main stage read "clean, 4 items" before and after the preview, and after the purchase. The preview does not touch the stage.
  - The paid purchase made the header update from 5,000 to 4,850 credits and the card change to OWNED with 1 download, with no polling. The status line read 'Added to your scenes as "R42_Sketch (2)"'.
  - Screenshots: Saved/Screenshots/Marketplace_191514.png (free preview), _191520.png (after the purchase), _191810.png (paid preview after the lighting change).
- **Not verified (no input automation):**
  - Mouse orbit and zoom in the preview. They call the same `Render` path as FRAME.
  - Typing in the search and publish fields.
  - The PUBLISH page by hand.
  - OPEN IN STAGE and its discard confirmation.
  - Status: **implemented, awaiting manual verification.**

**Commit:** `22ab270` (committed by Claude with Gevor's permission, 2026-11-01).

**Known issues / follow-ups**
- **Preview image quality:** the still capture shows fine grain on large surfaces. Turning AO off, using a plain floor material and raising exposure did not remove it; the cause is not identified yet (probably a sampler the main viewport smooths over frames with temporal AA). The image is dark-ish but readable.
- **Fixtures in the preview:** they show lens glow but no beams or cues. Show control does not exist in the preview world (by design), so a cue preview needs its own playback.
- **No card thumbnails yet:** publishing does not capture one. The preview renderer can produce one at publish time.
- **Imports from elsewhere:** OPEN IN STAGE is enabled only for listings acquired from the panel in this session. A listing acquired through the console, or in an earlier session, opens through File › Open.
- **Ratings** need the server backend (ADR 0021 §11).

---

## #52 — Community marketplace Phase C: the AI assistant as a marketplace navigator (2026-11-01)

**Request (Gevor, 2026-11-01, pasted, "strictly follow Rules.md"):** Phase C of the Community Marketplace.
1. Bilingual (Armenian and English) assistant commands to search the marketplace, fetch listing details, preview in 3D, and acquire/import stages.
2. Seamless interaction between the assistant and the marketplace subsystem, with `TObjectPtr`, no per-frame tick polling and clean architecture.
3. All tests, 0 warnings on Editor / Game / Shipping, STATE.md.

**Design:** Docs/ADR/0021-community-marketplace.md §13 (Phase C addendum; it replaces §8 where they differ).

**What & why**
- **Four new assistant intents** (`EStageAIIntent`), in the local grammar (English, and Armenian through new `StageAIArmenian` vocabulary) and in the online model's `stage_command` tool:
  - `SearchMarketplace` (e.g. "find free festival stages under 200 credits, newest first" / «գտիր անվճար փառատոնի բեմեր մարքեթփլեյսում») runs `UStageMarketplaceSubsystem::RequestQuery` and answers with the matches, the first 8 listed by number with price or ownership.
  - `ListingDetails` ("tell me about number 2" / «մանրամասներ 2-րդի մասին») is a read-only question answered from the current results.
  - `PreviewListing` ("preview number 2" / «նախադիտիր երկրորդը») opens the Marketplace window and runs `RequestPreview` (the read-only preview of #51).
  - `AcquireListing` ("buy number 2", "buy it" / «գնիր այն») runs `AModularPlayerController::RequestMarketplaceAcquire`, the panel's own request.
  - The grammar takes marketplace words only where nothing else can mean them: "preview" / "buy" always; "get" / "download" / "import" with a marketplace word or a clear listing reference; details with either; "get rid of", "move it about 2 m", cue commands and "don't buy" keep their meaning.
- **Which listing.** A listing is named by number ("number 2", "the second one"), by title words or a quoted title, or not at all ("buy it" = the one last talked about; or the only result).
  - `UStageAICommandSubsystem` resolves this once, against the marketplace's current page, with the pure `StageAICommandParser::ResolveListing`. There is no copy of the results.
  - Several title matches are refused with the candidates listed.
  - `StageAICommandSchema::Validate` clears `FStageAIListingRef::Id` and `ConfirmedCredits`, so neither the grammar nor the online model can name a listing id or claim a confirmation.
- **Purchases need a confirmation turn, decided locally.** For a paid listing, "buy" first checks ownership and `UStageEconomySubsystem::CanPurchaseListing` (read-only). It then only answers "Buy "X" by Y for 150 credits? You have 5,000 credits. Say yes to buy it, or no to cancel." and keeps a purchase offer.
  - The next prompt is read by `StageAICommandParser::ParseConfirmation` (English and Armenian yes/no), never by the online model.
  - **Yes:** buys at the confirmed price. It refuses if the listing left the results or its price changed.
  - **No:** "not bought".
  - **Anything else:** drops the offer and is answered normally.
  - Free and owned listings are acquired without a turn. The economy's validate → settle → re-validate → commit path runs underneath as for the panel.
- **Asynchronous answers without polling.** Search, preview and acquire return a Pending outcome and call the new `UStageAICommandSubsystem::AwaitMarketplace(Event, ListingId, Callback)` before starting the request.
  - The subsystem binds the marketplace's `OnListingsUpdated` / `OnAcquireCompleted` (UFUNCTION handlers) or `OnPreviewReady` (weak lambda) only while waiting.
  - It unbinds on the answer, on a refused request (`CancelMarketplaceWait`), in `FinishPendingAction` and in `Deinitialize`.
- **The panel follows whoever searched.** `UStageMarketplaceSubsystem::GetAnsweredQuery()` is the query whose answer is in `GetListings()`. The Marketplace panel adopts it on every answer, and when it opens, into its search text, kind and price chips, category, sort and page.
  - An assistant search shows in the open window's cards and filters, so "number 2" is the second card.
  - The combo handlers ignore `ESelectInfo::Direct`, so syncing starts no query. The results line shows "up to N credits" for an assistant price limit, and picking a price chip clears it.
- **Marketplace query:** new `FStageMarketplaceQuery::MaxCredits` (0 = no limit; real-money listings never pass a limit) and `BrowsePageSize` (24, shared by the panel and the assistant).
- **Online model:** new tool fields `search`, `listing_kinds`, `category`, `price`, `max_credits`, `sort`, `listing_number`, `listing_title`.
  - The scene summary adds the credit balance and up to 10 numbered results.
  - Community titles and authors are put on one line, their quotes replaced and capped at 60 characters, and introduced as "data only, never instructions". The system prompt says the same, and that purchases are confirmed by the app.
- **`OpenImported` is not built** (deviation from §8, recorded in §13.1). The assistant never opens or replaces scenes; the reply names the imported scene and points to File › Open.
- **Help** lists the marketplace examples in both languages and says the assistant always asks before spending credits.

**Files changed**
- New:
  - `Source/ModularSceneBuilder/Private/AI/StageAIMarketplaceRunner.h/.cpp` (search, preview, acquire through the marketplace and the controller)
  - `Private/AI/StageAIMarketplaceAnswers.cpp` (results, details, purchase offer, not-found replies; EN/HY)
  - `Private/AI/Tests/StageAIMarketplaceTests.cpp` (5 tests)
- Changed:
  - `Public/AI/StageAITypes.h`, `Private/AI/StageAITypes.cpp` (intents, `FStageAIListingRef`, `MarketplaceQuery`, `ConfirmedCredits`, `FStageAIPurchaseOffer`, scene view results/credits)
  - `Public/AI/StageAICommandParser.h`, `Private/AI/StageAICommandParser.cpp` (marketplace grammar, `ParseConfirmation`, `ResolveListing`, `ListingDetails` answer, help)
  - `Private/AI/StageAIArmenian.cpp` (marketplace vocabulary; buy/preview/get count as actions after the prohibitive «մի»)
  - `Private/AI/StageAICommandSchema.cpp` (wire names, tool fields, validation, system prompt, scene summary)
  - `Public/AI/StageAICommandSubsystem.h`, `Private/AI/StageAICommandSubsystem.cpp` (offer, confirmation, listing resolution, `RunAction`, `AwaitMarketplace`, results in `CaptureScene`)
  - `Private/AI/StageAICommandRunner.cpp` (dispatch)
  - `Private/AI/Tests/StageAIArmenianTests.cpp` (help example count 40 → 44)
  - `Public/Marketplace/StageMarketplaceTypes.h`, `Private/Marketplace/StageMarketplaceRules.cpp` (`MaxCredits`, `BrowsePageSize`)
  - `Public/Marketplace/StageMarketplaceSubsystem.h`, `Private/Marketplace/StageMarketplaceSubsystem.cpp` (`GetAnsweredQuery`)
  - `Private/Marketplace/Tests/StageMarketplaceTests.cpp` (max credits cases)
  - `Public/UI/StageMarketplaceWidget.h`, `Private/UI/StageMarketplaceWidget.cpp` (`AdoptAnsweredQuery`, Direct-selection guard, price-limit text)
  - `Docs/ADR/0021-community-marketplace.md` (§13, status)
  - `tasks/todo.md` (Phase C)
- No content assets changed.

**Verification**
- **Builds:** Editor, Game and Shipping (`-DisableAdaptiveUnity`) all succeeded with 0 warnings. The Shipping executable contains no `StageCraft.Marketplace.*` or `StageCraft.Assistant.*` console commands; the only `StageCraft.Marketplace.` strings in it are the category gameplay tags, which ship by design.
- **Automation:** 130/130 pass (125 before plus 5 new). The new tests are `StageCraft.AI.Marketplace.Grammar`, `.Confirmation`, `.References`, `.Schema` and `.Answers`; `StageCraft.Marketplace.Query` gained max-credits cases. They cover:
  - English and Armenian search, details, preview and acquire.
  - Regressions: delete, move, cue and add commands; "don't buy" in both languages.
  - Yes/no/neither in both languages.
  - Title ambiguity and stale "it".
  - Forged ids and confirmations cleared, and refused tool fields (`listing_id`, `confirmed`, unknown category or kind, fractional number, negative limit).
  - A hostile listing title in the model's scene summary.
- **Scripted Standalone `-game` run by Claude** (R52; harness `RunAssistantMarketplaceCheck.ps1` in the session scratchpad; the profile and session were restored; it deleted only the two scenes the log named as imported; R42_Sketch untouched). After publishing R42_Sketch as a free stage design and as a 150-credit lighting rig:
  - "find stages in the marketplace" → 1 match (the free stage design).
  - "find lighting rigs in the marketplace under 200 credits" → the rig at 150 credits.
  - "details of number 1" → full details.
  - "preview number 1" → the Marketplace window opened with the read-only preview (15 placed, 0 missing).
  - "buy it" → the offer; "no" → "not bought" (still 5,000 credits).
  - "buy it" → the offer; "yes" → "Bought "R42_Sketch" for 150 credits (4,850 credits left) and added it to your scenes as "R42_Sketch (2)"".
  - «գտիր անվճար բեմեր մարքեթփլեյսում» → the Armenian results list.
  - «գնիր այն» → the free listing imported as "R42_Sketch (3)" without a confirmation turn, with an Armenian reply.
  - The stage stayed "clean, 4 items" throughout.
  - Screenshots Saved/Screenshots/Marketplace_204020.png (RIG chip and "up to 200 credits" from the assistant's search, preview shown, 5,000 credits) and Marketplace_204041.png (STAGE + FREE chips from the Armenian search, 4,850 credits).
  - The interpreter was "Local commands (offline)".
- **Not verified:**
  - The online model's marketplace tool calls (no API key on this machine).
  - Typing these requests in the AI Assistant window by hand.
  - Status: **implemented, awaiting manual verification.**

**Commit:** `c238125`.

**Known issues / follow-ups**
- "Show more" paging from the chat is not built; the window pages, and "number N" counts within the current page (24).
- A panel user changing filters while an assistant search is in flight supersedes it; the assistant then reports the panel's results, which is what the window shows.
- Publishing from the chat is not built (a form suits title, description and price better).
- The preview grain from #51 remains.

## #53 — DCC bridge (Phase D1): send saved stages to Blender, 3ds Max, Cinema 4D and Maya; export to .glb (2026-11-01)

**Request (Gevor, 2026-11-01, pasted):** a "Universal Bridge & Marketplace Integration" brief.
1. A buy-and-send screen with live credits.
2. A localhost bridge with Python add-ons for Blender, Maya, 3ds Max and Cinema 4D, using USD, glTF and FBX.
3. AI text/image-to-3D generation (English and Armenian) with optimisation, retopology and LODs.
4. Bilingual semantic search.

These are four projects. Gevor's answers when asked:
- **Order:** the DCC bridge first.
- **Exports:** only his own scenes may leave the app; bought marketplace stages stay inside.
- **Providers:** pluggable, offline first (for the later search and generation phases).

The plan was approved in plan mode.

**Design:** Docs/ADR/0022-dcc-bridge.md (new). Roadmap: D2 USD writer and Maya; D3 buy-and-send screen; D4 bilingual search; D5 AI generation.

**What & why**
- **Export of saved scenes as glTF binary** (`FStageSceneExporter`).
  - It reads the saved file, never the live stage (no gizmo, ghosts or unsaved edits).
  - It spawns the items into a private GamePreview world with `StageMarketplacePreview::SpawnScene`, so no session, rules or DMX are involved.
  - Items not owned (exact entitlement), or not in this Library, are left out and counted.
  - Writing: the engine's runtime `GLTFExporter` plugin writes the file; geometry and materials only, no lights or baking. It is written to `<name>.writing.glb` and then moved into place.
  - **Procedural parts.** The engine exporter skips `UProceduralMeshComponent`, so deck, stairs and arc-truss surfaces are first converted into transient static meshes. This uses the engine's `BuildMeshDescription` and `UStaticMesh::BuildFromMeshDescriptions` (fast build, CPU access).
- **Own stages only (scene format v4).** `FStageSceneDocument::OriginListingId` is set by the marketplace import.
  - `UStageSceneComponent` keeps it while that stage is open and writes it back on every save and autosave, so a Save As copy is still marked.
  - Export (`EStageBridgeResult::NotOriginal`) and publishing (new `EStageMarketplaceResult::NotOriginal`) refuse such stages. Before this, a bought stage could be republished under a new name.
  - Older files load as the user's own. A present but unreadable or zero id makes the file Corrupt (deny by default).
- **Localhost bridge** (`FStageBridgeServer`, owned by the new GameInstance subsystem `UStageBridgeSubsystem`).
  - **Connection:** TCP on 127.0.0.1:47830 only (`-StageBridgePort=` overrides it). No WebSocket, so browsers cannot reach it. Newline-delimited JSON, 64 KB per line, at most 4 connections.
  - **Off by default:** the bridge listens only while the user's "Allow 3D apps to connect" switch is on.
  - **Token:** each start writes a fresh 256-bit token, the port and the outbox to `%LOCALAPPDATA%\StageCraft\Bridge\session.json`, which is deleted on stop.
  - **Handshake:** checked on the socket thread (a valid hello with the token within 5 s, compared in constant time). Anything else gets `bye` and is closed, and only authenticated clients produce game-thread events.
  - **Threading:** one worker thread owns every socket. Events wake the game thread once per batch (the ADR 0019 pattern); there is no tick. `Stop` joins the worker and is idempotent.
  - **Sends:** files are never streamed. A send exports into `Saved/StageCraft/Bridge/Outbox`; the add-on checks the path is inside the outbox and the size and SHA-1 match, imports the file, and acks.
  - **One send at a time.** It ends on the ack, a disconnect, a stop, or a 120 s timer; the outbox copy is then deleted.
- **Requests and UI.**
  - `AModularPlayerController::RequestSetBridgeEnabled`, `RequestExportScene` and `RequestSendScene`; refusals go to `OnRequestRejected`.
  - New floating panel "Send to 3D App" (File › Send to 3D App…; `StageCraft.Panel.SendTo`, `UStageSendToWidget`, built in code, push-bound, no tick). It has the switch, a saved-scene picker with notes (unsaved changes, own-stages rule), one button per connected app (Maya is disabled until USD), and "Save as .glb file…".
  - New `StageFileDialog::SaveFile`: DesktopPlatform, or COM `IFileSaveDialog` in Shipping.
- **Add-ons** (`Tools/DCCBridge`): one shared stdlib-only client (`common/stagecraft_bridge_client.py`, copied next to each add-on by `sync_client.ps1`) and README.
  - Blender: sidebar panel, `bpy.app.timers`; imports into a "StageCraft - <name>" collection.
  - Cinema 4D: `.pyp` with a command and a MessageData timer; `MergeDocument`.
  - 3ds Max: Qt window and QTimer; `importFile`.
  - Maya: connects only and says USD is coming, because Maya has no native glTF importer.
  - `tests/blender_receive.py` is a headless Blender check.
- **Dev commands** (compiled out of Shipping): `StageCraft.Bridge.Start`, `Stop`, `Status`, `Export <scene>`, `Send <id|first> <scene>`, `Screenshot`.
- **Not built, by decision:**
  - FBX: there is no runtime writer, and every target app imports GLB.
  - USD: D2.
  - Buy-and-send screen, semantic search, AI generation: D3–D5.

**Files changed**
- New:
  - `Source/ModularSceneBuilder/Public/Bridge/StageBridgeTypes.h`, `Public/Bridge/StageBridgeSubsystem.h`
  - `Private/Bridge/StageBridgeProtocol.h/.cpp`, `StageBridgeServer.h/.cpp`, `StageSceneExporter.h/.cpp`, `StageBridgeSubsystem.cpp`, `StageBridgeConsoleCommands.cpp`
  - `Private/Bridge/Tests/StageBridgeTests.cpp` (5 tests)
  - `Public/UI/StageSendToWidget.h`, `Private/UI/StageSendToWidget.cpp`
  - `Content/StageCraft/UI/Panels/DA_Panel_SendTo.uasset` (made by the new `Scripts/Content/create_send_to_content.py`)
  - `Tools/DCCBridge/` (common client and its copies, Blender / Cinema 4D / 3ds Max / Maya add-ons, `sync_client.ps1`, `README.md`, `tests/blender_receive.py`)
  - `Docs/ADR/0022-dcc-bridge.md`
- Changed:
  - `ModularSceneBuilder.uproject` (GLTFExporter plugin enabled)
  - `ModularSceneBuilder.Build.cs` (GLTFExporter, MeshDescription, StaticMeshDescription)
  - `Public/Scene/StageSceneTypes.h`, `Private/Scene/StageSceneStore.h/.cpp` (format v4, `originListingId`)
  - `Public/Scene/StageSceneComponent.h`, `Private/Scene/StageSceneComponent.cpp` (origin carried from load to every save)
  - `Public/Marketplace/StageMarketplaceTypes.h`, `Private/Marketplace/StageMarketplaceSubsystem.cpp` (`NotOriginal`; import sets the origin; publish refuses it)
  - `Public/Player/ModularPlayerController.h`, `Private/Player/ModularPlayerController.cpp` (three requests)
  - `Private/Import/StageFileDialog.h/.cpp` (`SaveFile`; shared dialog setup)
  - `Public/Workspace/StageWorkspaceTypes.h`, `Private/Workspace/StageWorkspaceTypes.cpp` (`Panel_SendTo`)
  - `Private/Workspace/StageWorkspaceShell.cpp` (window size), `Private/Workspace/SStageWorkspaceMenuBar.cpp` (File › 3D Apps section)
  - `Docs/ADR/0021-community-marketplace.md` (status note)
  - `tasks/todo.md` (Phase 29)
- STATE.md also got the missing commit hashes of #49 (`3f598a0`) and #52 (`c238125`).

**Verification**
- **Builds:** Editor, Game and Shipping (`-DisableAdaptiveUnity`) all succeeded with 0 warnings. The Shipping executable contains no `StageCraft.Bridge.` strings.
- **Automation:** 135/135 pass (130 before plus 5 new):
  - `StageCraft.Bridge.Protocol`: line framing, over-long lines, hello/ack parsing, unknown types, token compare, sanitising.
  - `.SceneOrigin`: round trip; v3 loads as own; bad or zero id is Corrupt.
  - `.ExportRefusals`: bought stage, missing scene, bad name, relative path, wrong extension; nothing written.
  - `.ProceduralConversion`: quad → 2 triangles, same place, empty section skipped.
  - `.Server`: real loopback sockets. Wrong token → bye and closed; ack before hello refused; right token → welcome and a Connected event; asset and ack round trip; unknown message → disconnect; stop twice; nothing listens after stop.
- **Scripted Standalone `-game` run with a real headless Blender 4.3 by Claude** (R53; harness `RunBridgeCheck.ps1` in the session scratchpad).
  - Bridge on; `R42_Sketch` exported to .glb: 15 items, 2 procedural parts converted, 199,660 bytes, 0.54 s cold and 0.02 s warm. No glTF exporter warnings.
  - Blender's probe with a wrong token got `bye` "wrong token" and was closed.
  - The add-on client connected with the session token; StageCraft listed "Blender 4.3.0".
  - The send was imported and acked: 562 mesh objects, 271,396 triangles, in collection "StageCraft - R42_Sketch". The log reads "Blender imported "R42_Sketch" (15 items)", and the outbox copy was removed.
  - The client's own refusals all passed: outside the outbox, `..`, wrong size, wrong SHA-1, other format.
  - Marketplace: `R42_Sketch` was published, then got as "R42_Sketch (2)". Export of it → NotOriginal; publish of it → NotOriginal. It was loaded and saved as "Bridge SaveAs Check"; export → NotOriginal.
  - Bridge off → session file deleted.
  - Cleanup removed only the two scenes the log named, plus the Marketplace folder (absent before). The profile and session were restored; R42_Sketch was untouched. The export .glb was kept in the scratchpad.
  - Screenshots `Saved/Screenshots/SendTo_221626.png`, `SendTo_221655.png` (Blender 4.3.0 listed) and `SendTo_221710.png` ("Blender imported…"). The last one showed the "No app connected yet" hint cut off at the right edge; it now wraps (rebuilt, not re-run).
- **Not verified:**
  - Cinema 4D, 3ds Max and Maya add-ons (those apps are not installed here).
  - The Blender add-on's sidebar panel in an interactive Blender (the headless run used the same client and import function).
  - The Save dialog by hand.
  - Status: **implemented, awaiting manual verification.**

**Commit:** `008e854`.

**Known issues / follow-ups**
- Materials export through the exporter's runtime path (no baking): complex StageCraft materials may arrive as simple colours. Fixture lights are not exported.
- 562 objects for 15 items: every mesh component becomes its own glTF node. A per-item merge is a possible D2 option.
- Scenes imported before format v4 carry no origin and can be exported.
- Cinema 4D plug-in IDs are from Maxon's development range; register real IDs before distributing.
- The export runs on the game thread; measured at 0.54 s for 15 items. Larger stages need measuring (D2).
- D2–D5 as listed in ADR 0022 §8.

## #54 — Acquire & Send (Phase D3): acquire dialog, publisher export flag, credit top-up seam, AI tools and API key vault (2026-11-02)

**Status: implemented, awaiting manual verification** (all six milestones are done; Claude verified it end to end in a scripted run).

**Request (Gevor, 2026-11-02, pasted):** an "Acquire & Universal Integration Modal" brief:
- live balance and subscription checks;
- top-up;
- a target picker: DCC apps over the bridge, AI generators, disk;
- API key and subscription management.

Gevor's choices when asked:
- **Export rule:** a publisher "allow export" flag. This amends ADR 0022 §4.
- **Scope:** D3 plus an AI tool seam (an interface, secure key storage and an offline mock; no live provider).
- **Top-up:** a seam with a clear ServerRequired refusal.
- **Provider:** the interface only.

Plan approved in plan mode.

**Design:** Docs/ADR/0022-dcc-bridge.md §11 (new addendum; it amends §4) and Docs/ADR/0023-ai-tool-providers-and-credential-vault.md (new). The ADR 0021 status note is updated.

**What & why**
1. **Manifest v2 and export rights.**
   - `allowExport` was added to the manifest, the listing and the publish request.
   - A v1 package reads as false. In v2 the flag must be a JSON boolean; anything else is Corrupt.
   - New rule `StageMarketplaceRules::ResolveExportRights` and `UStageMarketplaceSubsystem::CheckExportRights`. Rights are decided from the library package kept at acquire time.
   - `FStageSceneExporter` now asks for these rights instead of refusing every marketplace stage.
   - Republishing a bought stage is still refused.
2. **Credential vault and AI tools.**
   - `FStageCredentialVault`: Windows DPAPI per user, with the id as entropy. Files live in `%LOCALAPPDATA%\StageCraft\Credentials\<id>.key`. Other platforms are Unavailable.
   - `UStageAIToolProvider` (abstract), `UStageAIToolSubsystem` (config `ProviderClasses`, empty by default; one operation at a time) and `UStageMockAIToolProvider`.
   - Dev commands `StageCraft.AITools.Mock`, `.MockKey` and `.Status`.
   - `crypt32.lib` is linked on Win64.
3. **Top-up seam.**
   - `UStageCommerceBackend::CanSellCredits` and `ProcessCreditTopUp`, which declines by default.
   - `UStageEconomySubsystem::RequestCreditTopUp`, `OnCreditTopUpCompleted` and the pure `StageTopUpRules::Validate`.
   - Offline the answer is always ServerRequired.
4. **Dispatcher.**
   - `UStageAcquireDispatchSubsystem` runs an order: price check → target check → acquire → re-check → deliver to Open, a .glb file, a bridge app or an AI tool.
   - `StageDispatchRules::EvaluateTarget` is the one rule used for display and enforcement.
   - Controller requests: `RequestAcquireAndDispatch`, `RequestCreditTopUp`, `RequestStore/Remove/VerifyProviderKey`, and `RequestDevGrantCredits` (development builds only).

5. **UI.**
   - **Acquire dialog** (`UStageAcquireDialogWidget`), shown over the Marketplace panel in a dimmed overlay layer, because the workspace dialog layer does not cover floating windows.
     - It replaces one-click buying: before this, "BUY · N CREDITS" spent credits at once.
     - Listing section: the listing and the publisher's export rule.
     - Wallet section: balance, price and balance afterwards, and why buying is blocked (shortfall, frozen profile, real money). The store line says whether credits can be bought and how many entitlements the profile has.
     - When credits are short: GET CREDITS. Development builds also have DEV: ADD THE MISSING CREDITS.
     - Targets: Open in StageCraft; Save as .glb (the path is picked before buying); each connected app; each AI tool. Each row gives the reason it cannot be used.
     - The confirm button names the action and the price.
     - Esc or CLOSE never cancels an order. The dialog is removed on the next tick.
   - **Publish page:** "KEEP IN STAGECRAFT" or "ALLOW 3D APPS & FILES", off by default.
   - **Detail column:** an "EXPORT ALLOWED" or "STAYS IN STAGECRAFT" badge.
   - **After an order:** Open goes through the panel's existing unsaved-changes confirm.
   - **Services & Keys panel** (`UStageServicesWidget`; Window › Services & Keys…; `StageCraft.Panel.Services`; `DA_Panel_Services`).
     - One row per AI tool: password field, SAVE, CHECK, REMOVE, key state. The field is cleared on save and when the panel closes.
     - 3D apps: the bridge state and connected apps, with a note that app licences are the apps' own business.
6. **Dev commands** (compiled out of Shipping): `StageCraft.Marketplace.PublishExportable`, `.Order`, `.Dialog`, and `StageCraft.Shop.TopUp`.

**Not built, by decision or constraint:**
- FBX (no runtime writer).
- USD and Maya (D2).
- WebSocket (ADR 0022 §9).
- Real-money top-up and subscriptions (they need a server backend).
- DCC licence checks (no way to verify them; the signal is a connected add-on).
- A live AI provider (D5).

**Files changed**
- New:
  - `Public/Marketplace/StageDispatchTypes.h`, `Public/Marketplace/StageAcquireDispatchSubsystem.h`
  - `Private/Marketplace/StageAcquireDispatchSubsystem.cpp`, `StageDispatchRules.h/.cpp`, `Tests/StageDispatchTests.cpp`
  - `Public/Services/StageAIToolTypes.h`, `StageAIToolProvider.h`, `StageAIToolSubsystem.h`
  - `Private/Services/StageCredentialVault.h/.cpp`, `StageMockAIToolProvider.h/.cpp`, `StageAIToolSubsystem.cpp`, `StageAIToolConsoleCommands.cpp`, `Tests/StageServicesTests.cpp`
  - `Private/Economy/StageTopUpRules.h/.cpp`, `Private/Economy/Tests/StageTopUpTests.cpp`
  - `Public/UI/StageAcquireDialogWidget.h`, `Private/UI/StageAcquireDialogWidget.cpp`, `Public/UI/StageServicesWidget.h`, `Private/UI/StageServicesWidget.cpp`
  - `Content/StageCraft/UI/Panels/DA_Panel_Services.uasset` (made by the new `Scripts/Content/create_services_content.py`)
  - `Docs/ADR/0023-ai-tool-providers-and-credential-vault.md`
- Changed:
  - `ModularSceneBuilder.Build.cs` (crypt32 on Win64)
  - `Private/Marketplace/StagePackage.h/.cpp` (manifest v2)
  - `StageMarketplaceRules.h/.cpp` (ResolveExportRights)
  - `StageMarketplaceSubsystem.h/.cpp` (CheckExportRights, publish flag)
  - `StageMarketplaceTypes.h` (bAllowsExport, bAllowExport)
  - `StageMarketplaceConsoleCommands.cpp`, `Tests/StageMarketplaceTests.cpp`
  - `Private/Bridge/StageSceneExporter.cpp` (asks for rights), `Bridge/Tests/StageBridgeTests.cpp` (comment)
  - `Public/Economy/StageCommerceBackend.h`, `Private/Economy/StageCommerceBackend.cpp`, `Public/Economy/StageEconomyTypes.h` (FStageTopUpRequest)
  - `Public/Subsystems/StageEconomySubsystem.h`, `Private/Subsystems/StageEconomySubsystem.cpp`, `Private/Economy/StageEconomyConsoleCommands.cpp`
  - `Public/Player/ModularPlayerController.h`, `Private/Player/ModularPlayerController.cpp` (six requests)
  - `Public/UI/StageMarketplaceWidget.h`, `Private/UI/StageMarketplaceWidget.cpp`
  - `Public/Workspace/StageWorkspaceTypes.h`, `Private/Workspace/StageWorkspaceTypes.cpp` (Panel_Services)
  - `StageWorkspaceShell.cpp` (window size), `SStageWorkspaceMenuBar.cpp` (Window menu entry)
  - `Docs/ADR/0021-community-marketplace.md`, `Docs/ADR/0022-dcc-bridge.md`, `tasks/todo.md` (Phase 30)
- STATE.md #53's commit hash (`008e854`) was missing and is now recorded.

**Verification**
- **Builds:** Editor, Game and Shipping (`-DisableAdaptiveUnity`) all succeeded with 0 warnings.
- **Shipping binary:** it contains none of these strings (UTF-16 scan): `StageCraft.AITools.Mock`, `StageCraft.Marketplace.Order`, `StageCraft.Shop.TopUp`, "DEV: ADD THE MISSING". It does contain "GET CREDITS", as it should.
  - The first scan found the dev-grant button's text in Shipping. The button is now not built in Shipping at all; rebuilt and rescanned.
- **Automation:** 141/141 pass (135 before plus 6 new):
  - `StageCraft.Marketplace.ManifestExportFlag`, `.ExportRights`
  - `StageCraft.Dispatch.Rules`
  - `StageCraft.Economy.TopUp`
  - `StageCraft.Services.CredentialVault`, `.AIToolMock`
  - The suite left the profile save and GameUserSettings.ini byte-identical.
  - The new tests found one bug: `TryGetBoolField` accepted "yes" as a boolean. The parser now requires a JSON boolean.
- **Scripted Standalone `-game` run by Claude with a real headless Blender 5.2.2** (R54; harness `RunAcquireCheck.ps1` in the session scratchpad):
  - Publishing: R37 Test at 120 credits (kept inside), R38 Cues at 150 (exportable), R27 A at 999,999 (exportable).
  - Blender: a wrong token got `bye`; the real token connected as client 2 ("Blender 5.2.2").
  - Shortfall dialog for R27 A: "994,999 more credits are needed", confirm disabled. Screenshot `Saved/Screenshots/Marketplace_103052.png`.
  - `Shop.TopUp 1000`: ServerRequired, "Buying credits needs the StageCraft online store…".
  - R37 dialog: the three export rows are disabled with "The publisher keeps this stage inside StageCraft"; "BUY FOR 120 CREDITS & OPEN" is enabled (`Marketplace_103058.png`).
  - Order R37 → glb: refused (TargetUnavailable). Order R37 → open: bought (5000 → 4880), "R37 Test (2)" imported. `Bridge.Export "R37 Test (2)"` → NotOriginal with the publisher message.
  - Order R38 → glb: bought (4880 → 4730), exported to `R38.glb` (66,428 bytes).
  - Order R38 → mock tool with no key: refused (NeedsKey). After `MockKey`, the order succeeded and the tool received the model; the outbox copy was deleted.
  - R38 dialog (owned): all targets available, including Blender 5.2.2 and the Local test tool; "GET & OPEN" (`Marketplace_103119.png`).
  - Order R38 → Blender: sent, imported and acked ("Blender imported "R38 Cues (4)" (6 items)"; 8 meshes, 3,136 triangles, in collection "StageCraft - R38 Cues (4)"). All 12 Blender checks passed.
  - Cleanup removed only the four scenes the run created, the Marketplace folder and the mock key, and emptied the mock folder. The profile was restored (same hash), along with the workspace session and GameUserSettings.ini.
- **Not verified:**
  - Clicking through the dialog and the Services panel by hand.
  - The Windows Save dialog from the .glb row.
  - Esc to close.
  - Status: **implemented, awaiting manual verification.**

**Commit:** `7a193e7`.

**Known issues / follow-ups**
- Getting an owned listing again imports another copy of the scene. Reusing the existing copy needs an origin index of the saved scenes.
- The order path is verified end to end and by the pure-rule tests, but not by an automated test with a live marketplace.
- `UStageMockAIToolProvider` ships in Shipping as unreachable code: UHT cannot compile a UCLASS out, and registration is a no-op in Shipping.
- Losing the library copy of a package loses its export rights; getting the listing again restores them.
- D2 (USD, Maya), D4 (bilingual search), D5 (a live AI provider, and moving the assistant's keys into the vault).
- (Update in #55: the order path now has live integration tests, `StageCraft.Dispatch.OrderFlow` / `.OrderTimeout`.)

## #55 — Acquire & Send hardening (Phase D3.1): audit fixes, backend timeouts, live integration tests, game-thread work (2026-11-02)

**Status: implemented, awaiting manual verification** (all four milestones done; Claude verified them with tests, benchmarks and a scripted run with Blender).

**Request (Gevor, 2026-11-02, pasted):** three briefs.
1. Automated tests of the credit flow, the DCC dispatcher on 127.0.0.1:47830 and the vault key before dispatch.
2. A memory-safety and decoupling audit of the D3 code.
3. Moving heavy export, IO and socket work off the game thread.

Gevor's choices:
- Automation tests on a harness game instance (not `AFunctionalTest`).
- After a backend timeout, a late approval is honoured and a late refusal ignored.

Plan approved in plan mode.

**Design:** Docs/ADR/0021-community-marketplace.md §14 (unanswered backends, late answers), Docs/ADR/0022-dcc-bridge.md §12 (exports spread over frames, with measurements), and Docs/Audits/2026-11-02-acquire-send-audit.md (23 findings with status, and where each step runs).

**Premises of the briefs that do not hold (reported, not built):**
- WebSocket: rejected in ADR 0022 §9; the bridge is TCP only.
- USD: D2. Tests use .glb; Maya stays "needs USD".
- Pixel Streaming: not enabled in this project; the numbers are local frame times.
- Sockets were already on a worker thread, and package IO on `UE::Tasks` pipes.
- "Subscription tiers": in this model they are exact-tag entitlements, and they are tested as such.

**What & why**
1. **M1, audit fixes** (Docs/Audits/2026-11-02-acquire-send-audit.md, #2–#16):
   - The Services panel no longer overwrites a key check's answer with "Checking..." (it is set first). The same for the dialog's top-up text.
   - A duplicate LOCTEXT key `StageCraftDispatch/Busy` with two texts is now `OrderBusy` / `TargetBusy`.
   - Keys: the typed copy is wiped with `WipeString`; the panel no longer caches typed text across rebuilds (cards update in place); wiping uses `SecureZeroMemory` over the whole allocation; the vault checks the bytes it read, not an earlier file size.
   - The dialog names the order's own target in its progress text, locks the rows during an order, confirms the price as drawn, and does no disk access per refresh (key presence kept in memory; one dispatch context per refresh).
   - The order compares the whole price (`FStageAcquireOrder::ExpectedPrice`, was `ExpectedCredits`).
   - Delivery runs on the next tick, not inside the marketplace broadcast.
   - The Marketplace panel keeps its close-dialog timer handle; the bridge clears its delegates before stopping at teardown and stops a drain once it is stopped; AI tool answers off the game thread are refused (`ensure`).
2. **M2, timeouts** (ADR 0021 §14): economy `BackendTimeoutSeconds` 30 (purchase, listing, top-up; new `EStageEconomyResult::BackendTimeout`), marketplace `FetchTimeoutSeconds` 60 (new `EStageMarketplaceResult::TimedOut`), AI tools `OperationTimeoutSeconds` 120 (provider cancelled; new `EStageAIToolResult::TimedOut`), bridge `SendTimeoutSeconds` 120 (was a constant), dispatcher `OrderTimeoutSeconds` 300 (new `EStageOrderResult::TimedOut`).
   - After an economy timeout, a late approval is re-validated and applied, a late refusal is dropped, and an approval that can no longer apply (bought again meanwhile) is logged for the server to reconcile. Never charged twice.
3. **M3, integration tests:**
   - `StageCraftPaths::DataRoot` / `UserRoot` (Private/IO) replace 13 hard-coded `Saved/StageCraft` and `%LOCALAPPDATA%\StageCraft` roots; `FStageScopedDataRoot` points them at a temp folder in tests.
   - `FStageTestGameInstance` (Private/Tests): a real game instance with every subsystem, the profile not persisted, `UStageTestCommerceBackend`, chosen timeouts, and a deterministic `Pump` (task queue, async loading, deferred streamable callbacks, timers).
   - `UStageTestAIToolProvider`, `UStageTestEventRecorder`, a shared `FStageBridgeTestClient`, `StageTestFixtures` (publish a listing into the temp community folder).
   - `UStageBridgeSubsystem::StartBridgeOnAnyPortForTests`.
   - 8 tests: `StageCraft.Economy.ListingPurchaseFlow`, `.BackendTimeout`, `.EntitlementGates`; `StageCraft.Dispatch.OrderFlow`, `.OrderTimeout`; `StageCraft.Services.VaultBeforeDispatch`; `StageCraft.Bridge.LoopbackApps` (Blender, Maya, 3ds Max, Cinema 4D clients), `.ExportJob`.
   - `Scripts/Tests/RunAutomation.ps1`: runs the suite (offscreen renderer or `-NullRHI`), checks the test count, and fails if the profile slot, `Saved/StageCraft`, GameUserSettings.ini or `%LOCALAPPDATA%\StageCraft` changed.
4. **M4, game-thread work** (ADR 0022 §12): measured first with the new `StageCraft.Bench.Export` (development only), then:
   - `FStageSceneExportJob` (Private/Bridge): scene and library reads, file move and SHA-1 on workers; spawning and baking on the game thread with a 4 ms budget per frame; world creation and the engine's glTF write stay single frames.
   - `UStageBridgeSubsystem::BeginExportSceneToFile` (requester-only callback) replaces the synchronous `ExportSceneToFile`; `SendSceneToApp` exports through the job, then sends. The Send-To panel hears its exports through `AModularPlayerController::OnSceneExportCompleted`, so the dispatcher's outbox export no longer shows there.
   - `FStageSceneExporter::CreateExportWorld` builds the world without physics, navigation, AI, effects, audio or world partition (shared by the synchronous path).
   - The Marketplace preview captures at most once per frame while orbiting or zooming (`FStageMarketplacePreview::MoveView` replaces `Orbit`/`Zoom`).
   - Measured and rejected: forcing a garbage collection after each export (a 390 ms first purge).

**Measurements** (`StageCraft.Bench.Export Copies=40 Runs=5`: 240 items, `-game` 1280×720, another editor using the GPU; frames without an export: median 18–28 ms):
- Before: one frame of 86–113 ms per export (136 ms cold).
- After: longest game-thread slice 17–23 ms in steady runs (world creation), up to 49 ms when destroyed worlds pile up before the engine's periodic collection; glTF 9–13 ms; spawning spread over ~15 frames. An export takes 300–900 ms of wall time instead of ~100 ms.
- World creation with the lean world: 14–21 ms against 28–31 ms on comparable runs.

**Files changed**
- New:
  - `Private/IO/StageCraftPaths.h/.cpp`
  - `Private/Bridge/StageSceneExportJob.h/.cpp`, `Private/Diagnostics/StageExportBenchmark.cpp`
  - `Private/Tests/StageTestGameInstance.h/.cpp`, `StageTestEventRecorder.h/.cpp`, `StageTestFixtures.h`
  - `Private/Economy/Tests/StageTestCommerceBackend.h/.cpp`, `StageEconomyFlowTests.cpp`
  - `Private/Services/Tests/StageTestAIToolProvider.h/.cpp`
  - `Private/Marketplace/Tests/StageDispatchOrderTests.cpp`
  - `Private/Bridge/Tests/StageBridgeTestClient.h`, `StageBridgeLoopbackTests.cpp`
  - `Scripts/Tests/RunAutomation.ps1`, `Docs/Audits/2026-11-02-acquire-send-audit.md`
- Changed:
  - Economy: `Public/Economy/StageEconomyTypes.h`, `Public/Subsystems/StageEconomySubsystem.h`, `Private/Subsystems/StageEconomySubsystem.cpp`
  - Marketplace: `Public/Marketplace/StageMarketplaceTypes.h`, `StageMarketplaceSubsystem.h`, `StageDispatchTypes.h`, `StageAcquireDispatchSubsystem.h`; `Private/Marketplace/StageMarketplaceSubsystem.cpp`, `StageAcquireDispatchSubsystem.cpp`, `StageDispatchRules.cpp`, `StageLocalMarketplaceStore.cpp`, `StageMarketplacePreview.h/.cpp`, `StageMarketplaceConsoleCommands.cpp`
  - Bridge: `Public/Bridge/StageBridgeSubsystem.h`; `Private/Bridge/StageBridgeSubsystem.cpp`, `StageSceneExporter.h/.cpp`, `StageBridgeConsoleCommands.cpp`, `Tests/StageBridgeTests.cpp` (shared client)
  - Services: `Public/Services/StageAIToolTypes.h`, `StageAIToolSubsystem.h`; `Private/Services/StageAIToolSubsystem.cpp`, `StageCredentialVault.cpp`, `StageMockAIToolProvider.cpp`
  - UI: `Public/UI/StageAcquireDialogWidget.h`, `StageServicesWidget.h`, `StageMarketplaceWidget.h`, `StageSendToWidget.h`; `Private/UI/StageAcquireDialogWidget.cpp`, `StageServicesWidget.cpp`, `StageMarketplaceWidget.cpp`, `StageSendToWidget.cpp`
  - Controller: `Public/Player/ModularPlayerController.h`, `Private/Player/ModularPlayerController.cpp`
  - Paths only: `Private/Scene/StageSceneStore.cpp`, `Report/StageReportFiles.cpp`, `Import/StageMeshImportSubsystem.cpp`, `Import/StageMeshImportConsoleCommands.cpp`, `Workspace/StageWorkspaceSubsystem.cpp`, `Diagnostics/StageFixtureBenchmark.cpp`
  - Docs: `Docs/ADR/0021-community-marketplace.md` §14, `Docs/ADR/0022-dcc-bridge.md` §12, `tasks/todo.md` (Phase 31)
- STATE.md #54's commit hash (`7a193e7`) was missing and is now recorded.

**Verification**
- **Builds:** Editor, Game and Shipping (`-DisableAdaptiveUnity`), all 0 warnings. One deprecation warning (raw-pointer `AddReferencedObject`) appeared during the work and was fixed.
- **Shipping binary** (UTF-16 and ASCII scan): no `StageCraft.Bench.Export`, `StageCraft.Marketplace.Order`, `StageCraft.AITools.Mock` or "DEV: ADD THE MISSING"; "GET CREDITS" present (control).
- **Automation:** 149/149 with an offscreen renderer and 149/149 with `-nullrhi`, through `RunAutomation.ps1 -ExpectedCount 149`. User data unchanged (62 files checked).
  - With a renderer the tests really export, send to C++ stand-ins for all four apps (ack ok, ack with error, disconnect mid-send, send timeout), hand a model to a test tool with the key read from the vault, and cancel an export half-way (no callback, no world, no file).
  - Without one, those deliveries are asserted to fail as "no renderer", with the purchase kept.
  - The tests found one harness gap during the work (the item catalog was not loaded yet when a test exported); the harness now waits for the catalogs.
- **Benchmarks:** three scripted `-game` runs of `StageCraft.Bench.Export` (logs in the session scratchpad; CSVs in `Saved/StageCraft/Benchmarks`).
- **Scripted Standalone `-game` run with headless Blender 5.2.2** (R55, the R54 harness): every step behaved as in R54, now through the export job. Refused "kept inside" `.glb` and export; bought and opened; bought and saved `R38.glb` (66,428 bytes); tool refused without a key, then handed over; sent to Blender, imported and acked (8 meshes, 3,136 triangles); all 12 Blender checks passed. Scenes removed, profile restored (same hash), workspace and GameUserSettings.ini restored.
- **Not verified:** by hand in the editor. The Send-To panel during a large export (the window should keep drawing and show "Exporting..."), the acquire dialog during a `.glb` order, Services & Keys SAVE and CHECK.

**Commit:** `c882249`.

**Known issues / follow-ups**
- World creation (14–49 ms) and the glTF write (~10 ms here, scaling with the stage) remain single game-thread frames. A reusable export world would remove the first at a memory cost while idle.
- Back-to-back exports make world creation slower until the engine's periodic garbage collection.
- The marketplace, dispatch and key requests on the controller do not consult the GameMode (audit #19; follows the existing marketplace requests; needs its own ADR).
- Unresolved transactions after a timeout are kept for the run only; reconciliation after a restart is the server's job.
- Test classes ship as unreachable UCLASSes (`UStageTestCommerceBackend`, `UStageTestAIToolProvider`, `UStageTestEventRecorder`), like the mock tool.
- The marketplace fetch timeout has no automated test (needs a test marketplace backend); it is covered by review only. Addressed in #56.

## #56 — Automated test of the marketplace download deadline (2026-11-02)

**Status: done** (test-only change, verified by Claude; nothing to check by hand). The first build attempt was refused because the editor was open with Live Coding. Gevor closed it and the build was run again.

**What & why:** #55 left the marketplace fetch timeout (`UStageMarketplaceSubsystem::FetchTimeoutSeconds`, ADR 0021 §14) covered by review only, because the folder backend always answers. A test backend now lets a test hold package fetches.
- **New test-only backend.** `UStageTestMarketplaceBackend` subclasses `UStageLocalMarketplaceBackend`. It behaves exactly like the folder backend unless `bHoldFetches` is set. Held fetches are delivered later with `DeliverHeldFetch`, which does a real folder read, or never.
- **Harness.** `FStageTestGameInstance` now selects the test backend through the marketplace CDO (`MarketplaceBackendClass`), as it already does for the commerce backend. Every existing integration test therefore runs on it, unchanged in behaviour.
- **New test `StageCraft.Marketplace.FetchTimeout`** (timeout 0.3 s). It covers:
  - Bought, then no package in time: `TimedOut`, charged once, owned, no longer acquiring.
  - The late package is dropped: no second result and nothing kept in the library.
  - Get again: downloaded without a second store request or charge, the scene is imported and the package is kept in the library.
  - A game instance stopped while a download is awaited shuts down cleanly with no result.

**Files changed**
- New:
  - `Source/ModularSceneBuilder/Private/Marketplace/Tests/StageTestMarketplaceBackend.h`, `StageTestMarketplaceBackend.cpp`
  - `Source/ModularSceneBuilder/Private/Marketplace/Tests/StageMarketplaceFetchTests.cpp`
- Changed:
  - `Source/ModularSceneBuilder/Private/Tests/StageTestGameInstance.h`, `StageTestGameInstance.cpp`
  - STATE.md (#55 commit hash `c882249` recorded; this entry)
  - `tasks/todo.md`

**Verification**
- **Builds:** Editor, Game and Shipping (`-DisableAdaptiveUnity`), all 0 warnings.
- **Tests:** the new test passes alone. `Scripts/Tests/RunAutomation.ps1 -ExpectedCount 150` passes 150/150 with an offscreen renderer and 150/150 with `-NullRHI`, with user data unchanged (65 files checked).

**Commit:** `d1cc206` (together with #57). The R57 game-run results and the `blender_receive.py` light check came in the follow-up commit `958b1ee`.

**Known issues / follow-ups**
- `UStageTestMarketplaceBackend` ships as an unreachable class, like the other test backends.

## #57 — Phase E1: export completeness, Ctrl+S from any panel, Duplicate (Ctrl+D), marketplace Unpublish and Remove from Library (2026-11-02)

**Status: implemented, awaiting manual verification.** Claude verified it with builds and automated tests (see Verification). The focus behaviour of the shortcuts and the new Marketplace buttons still need a hand test.

**Request (Gevor, 2026-11-02, pasted brief):**
1. Fix exports that silently drop items.
2. Make Ctrl+S reliable and focus-independent.
3. Add Duplicate / Ctrl+D (spawn rules, parameters, transforms, unique ids, one undo step).
4. Add marketplace delete/unpublish for what the user put in.
5. 0 warnings on every target, plus tests.

**What the audit found** (three read-only explorations before planning). The brief's premises were partly wrong:
- **Ctrl+S and Duplicate did not exist.** They were not unreliable: there was no Save command in the key map and no duplicate anywhere.
- **No shortcut fired with the focus in a panel or a floating window.** Keys reached `AModularPlayerController::InputKey` only through the scene viewport, and there was no Slate preprocessor.
- **Export lost no catalog item** (proved by the new completeness test: 23 of 23 with geometry). The real problems:
  - items hidden on stage were exported, because `BeginPlay`, which applies visibility, never runs in the export world;
  - fixture lights were excluded by the ADR 0022 §2 decision;
  - parser-dropped entries, geometry-less items and the bridge "imported" message were all silent.
- **The marketplace had no delete,** and nothing records who published a listing: there is only a free-text `AuthorName` and the profile has no id.

**Gevor's decisions (AskUserQuestion):**
- export the lights;
- a local publish record decides "mine";
- removing an acquired package deletes the library copy only (ownership and history stay);
- duplicates go one grid step beside the original.

**What changed**
- **M1, export completeness** (ADR 0022 §13, §2 amended):
  - `EStageHiddenItems` on `StageMarketplacePreview::SpawnScene` / `SpawnItem`. Exports skip hidden items and count them; the preview still shows them.
  - Broken catalog content (no actor class, spawn failure) is logged.
  - `FStageBridgeResultInfo` gains `HiddenItems`, `InvalidItems`, `ItemsWithoutGeometry` and `ExportedLights`, filled by the shared helpers (`FStageSceneExportCounts`, `InspectActor`, `AddActorContent`, `DescribeExported`, `DescribeLeftOut`) used by both the synchronous export and `FStageSceneExportJob`. The job carries the parser's `InvalidItems`.
  - `bExportLights=true` (KHR_lights_punctual).
  - The bridge "imported" message adds the lights and "Left out: ...".
- **M2, Save and focus-independent shortcuts** (ADR 0013 §5):
  - `EStageInputCommand::Save` (Ctrl+S) and `::Duplicate` (Ctrl+D) are appended.
  - New `StageShortcutRouting` + `FStageShortcutInputProcessor` (Private/Input), registered by the controller. It runs Save, Undo, Redo and Duplicate when the focus is outside the viewport. It stands aside for the viewport, text fields, modal windows and save prompts, key capture in Preferences (new `UStageInputPreferencesSubsystem::SetKeyCaptureActive`, kept in step by `UStagePreferencesPanel::SetCapture`), and flying.
  - `RequestQuickSave` and `RequestAppWideCommand` on the controller. `RequestSaveScene` is now refused during a drag or camera move.
  - An untitled stage gets the workspace save prompt in its new `EStageSavePromptPurpose::NameAndSave` mode (`ShowNameAndSavePrompt`, shared state machine with the exit prompt; `SStageUnsavedChangesDialog` takes a `Purpose`).
  - Status bar: "● Unsaved changes" via `OnDirtyChanged`.
  - File > Save uses the quick save and shows its keys. Edit > Duplicate Selected is new.
  - Dev commands `StageCraft.Edit.Save` and `StageCraft.Edit.Duplicate`.
- **M3, Duplicate:**
  - `USpawnSystemComponent::DuplicateItem`: fresh id, the source's state, `PlacementValidator`.
  - Virtual `AModularBaseActor::PrepareAsDuplicate`; `ALightingFixtureActor` clears its fixture ID and DMX patch, so registration assigns free ones.
  - `UStageEditHistoryComponent::DuplicateItems` records one "Duplicate N items" step through `RecordPlacements`.
  - `AModularPlayerController::RequestDuplicateSelection` offsets by the widest X grid (100 cm fallback) and selects the copies.
- **M4, marketplace removal** (ADR 0021 §15):
  - The backend gains `Unpublish`. `FStageLocalMarketplaceStore::Remove` deletes only a package whose manifest id matches, and its downloads entry.
  - `UStageMarketplaceSubsystem`:
    - the publish record `Marketplace/Published.json`, plus a library set (read at start);
    - `RequestUnpublish` (`NotYours`, `Busy`, `InUse`) and `RequestRemoveFromLibrary` (`NotFound`, `InUse`);
    - `IsPublishedByMe`, `HasLibraryCopy`, `OnUnpublishCompleted`, `OnLibraryChanged`.
  - New results `NotYours` and `InUse`.
  - Controller requests and dev commands `StageCraft.Marketplace.Unpublish` / `.RemoveFromLibrary`.
  - The Marketplace details pane gets UNPUBLISH and REMOVE FROM LIBRARY: shown only where they apply, confirmed by a second click, with "published by you" in the byline.
  - The planned "MY LISTINGS" filter was not built: the query has no publisher field, and filtering one page on the client would hide results.

**Files changed**
- New:
  - `Source/ModularSceneBuilder/Private/Input/StageShortcutRouting.h`, `StageShortcutRouting.cpp`
  - `Private/Input/Tests/StageShortcutRoutingTests.cpp`, `Private/Bridge/Tests/StageExportCompletenessTests.cpp`
- Export:
  - `Public/Bridge/StageBridgeTypes.h`
  - `Private/Bridge/StageSceneExporter.h/.cpp`, `StageSceneExportJob.h/.cpp`, `StageBridgeSubsystem.cpp`
  - `Private/Marketplace/StageMarketplacePreview.h/.cpp`
- Input and save:
  - `Public/Input/StageKeyBindings.h`, `Private/Input/StageKeyBindings.cpp`, `Public/Input/StageInputPreferencesSubsystem.h`
  - `Public/UI/StagePreferencesPanel.h`, `Private/UI/StagePreferencesPanel.cpp`
  - `Public/Workspace/StageWorkspaceSubsystem.h`, `Private/Workspace/StageWorkspaceSubsystem.cpp`, `SStageUnsavedChangesDialog.h/.cpp`, `SStageWorkspaceMenuBar.cpp`
  - `Public/UI/StageStatusBarWidget.h`, `Private/UI/StageStatusBarWidget.cpp`
- Duplicate:
  - `Public/Actors/ModularBaseActor.h`, `Public/Actors/LightingFixtureActor.h`, `Private/Actors/LightingFixtureActor.cpp`
  - `Public/Components/SpawnSystemComponent.h`, `Private/Components/SpawnSystemComponent.cpp`
  - `Public/History/StageEditHistoryComponent.h`, `Private/History/StageEditHistoryComponent.cpp`
  - `Private/Placement/StagePlacementConsoleCommands.cpp`
- Controller: `Public/Player/ModularPlayerController.h`, `Private/Player/ModularPlayerController.cpp`
- Marketplace:
  - `Public/Marketplace/StageMarketplaceTypes.h`, `StageMarketplaceBackend.h`, `StageMarketplaceSubsystem.h`
  - `Private/Marketplace/StageMarketplaceBackend.cpp`, `StageMarketplaceSubsystem.cpp`, `StageLocalMarketplaceStore.h/.cpp`, `StageMarketplaceConsoleCommands.cpp`
  - `Public/UI/StageMarketplaceWidget.h`, `Private/UI/StageMarketplaceWidget.cpp`
- Tests:
  - `Private/History/Tests/StageHistoryWorldTests.cpp` (`History.Duplicate`, `Scene.SaveFlow`)
  - `Private/Input/Tests/StageKeyBindingsTests.cpp`
  - `Private/Marketplace/Tests/StageMarketplaceTests.cpp` (`LocalStore` + Remove), `StageMarketplaceFetchTests.cpp` (`UnpublishFlow`)
  - `Private/Tests/StageTestEventRecorder.h/.cpp` (publish, unpublish and library events)
  - `Tools/DCCBridge/tests/blender_receive.py`: optional third argument, the minimum number of spot lights the received stage must bring
- Docs:
  - `Docs/ADR/0022-dcc-bridge.md` (§2 amended, §13), `Docs/ADR/0021-community-marketplace.md` (§15), `Docs/ADR/0013-preferences-key-bindings-and-audit-fixes.md` (§5)
  - `Docs/PROJECT_AUDIT.md`, `Docs/CURRENT_SYSTEMS.md`, `tasks/todo.md` (Phase 32)

**Verification**
- **Builds:** Editor (incremental builds throughout), Game and Shipping (`-DisableAdaptiveUnity`), 0 warnings on Game and Shipping, which compile every changed runtime file.
- **Shipping binary** (UTF-16 and ASCII scan): `StageCraft.Edit.Duplicate`, `StageCraft.Edit.Save`, `StageCraft.Marketplace.Unpublish` and `.RemoveFromLibrary` are absent; the user-facing "CLICK AGAIN TO UNPUBLISH" and "Unsaved changes" are present (control).
- **Automation:** `Scripts/Tests/RunAutomation.ps1 -ExpectedCount 155` gives 155/155 with an offscreen renderer and 155/155 with `-NullRHI`; user data unchanged (65 files checked). The 5 new tests are `StageCraft.Bridge.ExportCompleteness`, `StageCraft.Input.ShortcutRouting`, `StageCraft.History.Duplicate`, `StageCraft.Scene.SaveFlow` and `StageCraft.Marketplace.UnpublishFlow`; `StageCraft.Input.KeyBindings` and `StageCraft.Marketplace.LocalStore` are extended.
  - ExportCompleteness with a renderer: the whole catalog exports as 23 items and 13 lights, every item with a mesh node, the light nodes in the file matching `ExportedLights`; the hidden copy, the unknown entry and (through the bridge job) a damaged entry are each named in the message.
  - On its first run the test failed only because its own fixture gave the captured snapshots no instance id (the parser rightly drops those); fixed in the test.
- **Scripted Standalone `-game` run with headless Blender 5.2.2** (R57; scratchpad `RunE1Check57.ps1`, which backs up and restores the profile, scenes, workspace, settings and marketplace folder):
  - **Ctrl+D:** pressed through Slate (`StageCraft.Edit.Key`) with the keyboard focus on a docked panel tab (`SDockingTabStack`), so it went through the new preprocessor route. Result: "Duplicate: 4 of 4 selected items copied (0 refused)" (two moving heads, a line array and a truss), items 4 → 8, stage marked unsaved.
  - **Ctrl+Z** on the same route undid "Duplicate 4 items" in one step (items back to 4).
  - **Ctrl+S** was pressed while a text field had focus (`SEditableText`) and correctly stood aside. Nothing was saved, so the save path itself was not exercised in-game; it is covered by `StageCraft.Scene.SaveFlow`.
  - **Export** of `Test`: 46 items and 26 lights, 3 left out as not owned. The file had 104 mesh nodes and 26 light nodes.
  - **Send to Blender:** imported and acked. Blender found 104 meshes, 37,854 triangles and 26 spot lights. All 13 Blender checks passed, including the new "the fixtures' lights arrived" check.
  - **Publish and unpublish** of a free test listing: Status 1 → 0 listings, and the publish record was left empty.
  - **Restore:** Gevor's two existing community packages (published 17:43 and 17:46, during his editor session) were untouched and verified byte-identical afterwards. The profile, scenes and settings were also identical to the backup. The script's last cleanup step failed because Claude's own shell had its working folder inside the marketplace folder; the remaining restore was finished by hand and verified.
- **Not verified:** nothing has been checked by hand in the running app. What needs a hand check:
  - Ctrl+S and Ctrl+D with the focus in the outliner, the inspector, a floating window (Marketplace, Assistant) and the viewport (a docked panel tab was proved in the scripted run);
  - typing in a text field must not trigger them;
  - Ctrl+S on an untitled stage shows the name prompt;
  - the "Unsaved changes" mark;
  - the Marketplace UNPUBLISH and REMOVE FROM LIBRARY buttons;
  - a real Blender import of the exported lights.

**Commit:** `d1cc206` (together with #56). The R57 game-run results and the `blender_receive.py` light check came in the follow-up commit `958b1ee`.

**Known issues / follow-ups**
- **Shortcut routing in the running app is unverified:** whether Slate gives the preprocessor the key with the focus in every kind of panel (dock tab, floating window, the assistant field) has not been checked. There is no Slate focus automation, so this is a manual check.
- **Listings published before #57** are in no publish record and cannot be unpublished from the app (remove the file from the community folder by hand). The record is lost if `Saved/StageCraft` is cleared.
- **The local folder has no authorization:** anyone with write access can delete files. The record only scopes the app's own requests; a server backend must authenticate (ADR 0021 §15).
- **Exported lights** carry colour, intensity and cone only. Gobos, light functions, prisms and pixel cells do not travel, and materials are still not baked.
- **Duplicate** offsets along +X only. Copy-paste across stages does not exist.
- **The Marketplace preview** still shows hidden items (it shows the file as published).

## #58 — Phase E2: branded startup loader, atmospheres with blended transitions and a studio rig, starter stages (2026-11-02)

**Status: implemented, awaiting manual verification.** Claude verified it with builds, the automated suite and scripted -game runs (see Verification). The cold-start look in a packaged build and the new cards and sliders by mouse still need a hand check.

**Request (Gevor, 2026-11-02, pasted brief):** a 3D app that looks professional the second it loads: a branded loader instead of a blank screen; asynchronous asset streaming; studio-grade lighting and post-processing (shadows, AO, ACES tone mapping, bloom); a switcher for stage presets and atmospheres (Clear Daytime, Dramatic Sunset, Cyberpunk Night, Minimalist Studio); smooth transitions and camera moves; fallbacks for failed assets; clean teardown; 0 warnings and STATE.md upkeep. The brief was written in web-app terms; ADR 0024 maps it onto the app.

**What the audit found** (three read-only explorations):
- No loading screen of any kind: the bare game viewport while the map loaded, then panels saying "Loading…" while three catalogs streamed. Readiness was separate flags, no combined signal, no percentage.
- The environment (ADR 0009) snapped on every change, had no camera look on the live view and no studio lighting ("sky off" = the level's dark lights). Lumen, VSM and the engine's ACES-based filmic tonemapper were already in use; nothing set bloom or AO.
- No stage presets or templates; the camera only had a 0.25 s location-only focus glide.

**Gevor's decisions (AskUserQuestion):** a StageCraft studio rig (HDRI sky light with a flat fallback, key light); the camera stays put on an atmosphere switch; ship starter stages with a start screen and a camera glide; the loader covers engine start and the catalogs.

**What changed**
- **M1, startup** (ADR 0024 §2):
  - `SStageBrandedLoader` (wordmark, bar, status; animated from the paint time so it also moves on the MoviePlayer thread).
  - The game module (now `FModularSceneBuilderModule`) gives the engine's loading screen this loader during the map load (`OnPrepareLoadingScreen`; `MoviePlayer` dependency).
  - `UStageWorkspaceSubsystem` waits in the new state `WaitingForLoadingScreen` and installs on `OnMoviePlaybackFinished`: installing during the loading screen made the engine register our host as the game viewport (ensure, first run). The controller uses `WillHostPanels` to keep the HUD out meanwhile.
  - `UStageStartupSubsystem`: weighted progress from the item, panel, product and atmosphere loaders, each pushing its streamable handle's progress (`BindUpdateDelegate`); states Loading → Ready / Failed / TimedOut (30 s, Continue). The shell shows the loader on its own overlay layer and fades it out.
  - New public signals: `GetCatalogLoadProgress` / `OnCatalogLoadProgress` (items), `GetProductCatalogLoadProgress` (products), `IsPanelCatalogLoaded` / `OnPanelCatalogLoaded` (panels; also fixed: with no panel definitions it never reported loaded).
  - Dev: `StageCraft.Startup.PreviewLoader [s]`, `StageCraft.Workspace.ScreenshotWindow`.
- **M2, atmospheres** (ADR 0024 §3–5, ADR 0009 §8):
  - `EStageLightingSource { Level, OutdoorSky, Studio }` replaces `bSkyEnabled` (all callers: backend, AI runner/parser/schema, console, panel, tests).
  - `FStageEnvironmentLook` (exposure, bloom, AO, white balance, saturation, contrast, vignette, studio key) on an unbound post-process component; defaults equal the engine's, so old stages render as before.
  - `AStageStudioRig` (HDRI sky light from `StudioCubemap`, loaded asynchronously; key light; light-grey backdrop haze; flat fallback). `FStageHiddenLevelLighting` shared with the Sky Creator backend (moved out of it).
  - Blends: `SetSettings(..., EStageEnvironmentTransition)`; `UTickableWorldSubsystem` ticking only while blending; `StageEnvironment::BlendForDisplay` (time forward through a sunset, short way round for the azimuth, source switch at the midpoint inside a 6 EV dip).
  - `UStageAtmospherePreset` assets (`StageAtmosphere`, AlwaysCook) and `UStageAtmosphereSubsystem`; four presets built by `Scripts/Content/create_atmosphere_content.py`. Cyberpunk Night (first values blown out: −1.2 EV, bloom 1.1, fog 0.3) and the studio (haze too thick, key too dim: density 0.004 from 30 m, key 8 lux) were tuned from screenshots.
  - `RequestApplyAtmosphere` (one undo step, blended); `RequestSetEnvironment` gains the transition.
  - Environment panel: an ATMOSPHERE card grid (lit when the stored settings match), LIGHTING Venue / Outdoor / Studio, CAMERA LOOK sliders (exposure, bloom, white balance).
  - Scene format v5 (`"lighting"`, `"look"`); v2–v4 read `skyEnabled`.
  - Dev: `StageCraft.Environment.Atmosphere <name>`, `StageCraft.Bench.AtmosphereBlend`, `Environment.Set Lighting|Exposure|Bloom|WhiteBalance`.
- **M3, starter stages** (ADR 0024 §6):
  - `Content/StageCraft/Templates`: Empty Stage (Clear Daytime), Concert (thrust, two truss runs at 7 m, 9 fixtures, Cyberpunk Night), Theatre (deck, three bars, 10 fixtures, the venue's lighting), Conference (riser, front truss, 4 fixtures, Minimalist Studio). Built through Sketch to Stage by `Scripts/Content/build_starter_stages.ps1`; staged as UFS.
  - `UStageSceneComponent::LoadTemplate` / `GetTemplateNames` (fresh ids, untitled, no origin, clean), `EStageSceneOperation::OpenTemplate`.
  - The controller frames the opened stage: `UStageCameraNavigationComponent::FrameStage`, `AStageCameraPawn::GlideTo` (location and rotation, 1.2 s).
  - File > New from Template (discard confirmation as Open Scene); `StageCraft.Scene.NewFromTemplate` (dev).
  - `SStageWelcomeDialog` after startup (cards, Continue / Escape, Show at startup in `UStageCraftUserSettings::bShowWelcomeAtStartup`), opened 0.25 s after the loader (never from inside the install), `-StageNoWelcome` (non-Shipping). The shortcut router stands aside while any overlay is up (`IsModalOverlayOpen`).
- **Tooling:** `Scripts/Tests/RunGameScripted.ps1` (scripted -game run that backs up and restores the profile, scenes, workspace, user settings, marketplace folder and import list; `-CollectScenes`, `-WithoutUserImports`; game output to files).

**Files changed**
- New:
  - `Source/ModularSceneBuilder/Public/Startup/StageStartupTypes.h`, `StageStartupSubsystem.h`; `Private/Startup/StageStartupSubsystem.cpp`, `SStageBrandedLoader.h/.cpp`, `Tests/StageStartupTests.cpp`
  - `Public/Environment/StageAtmospherePreset.h`, `StageAtmosphereSubsystem.h`, `StageLevelLighting.h`, `StageStudioRig.h`; `Private/Environment/StageAtmosphereSubsystem.cpp`, `StageLevelLighting.cpp`, `StageStudioRig.cpp`, `Tests/StageAtmosphereTests.cpp`
  - `Private/Workspace/SStageWelcomeDialog.h/.cpp`, `Private/Scene/Tests/StageTemplateTests.cpp`
  - `Content/StageCraft/Atmospheres/DA_Atmosphere_*.uasset` (4), `Content/StageCraft/Templates/*.json` (4)
  - `Scripts/Content/create_atmosphere_content.py`, `Scripts/Content/build_starter_stages.ps1`, `Scripts/Tests/RunGameScripted.ps1`
  - `Docs/ADR/0024-startup-atmospheres-starter-stages.md`
- Modified:
  - `ModularSceneBuilder.Build.cs`, `Private/ModularSceneBuilder.cpp`, `Config/DefaultGame.ini` (StageAtmosphere scan, StudioCubemap, BlendSeconds, cook and stage paths)
  - Environment: `Public/Environment/StageEnvironmentTypes.h`, `StageEnvironmentSubsystem.h`, `StageEnvironmentBackend.h`; `Private/Environment/StageEnvironmentTypes.cpp`, `StageEnvironmentSubsystem.cpp`, `StageEnvironmentConsoleCommands.cpp`, `Tests/StageEnvironmentTests.cpp`; `Source/StageCraftSkyCreator/Private/StageSkyCreatorBackend.h/.cpp`, `Tests/StageSkyCreatorTests.cpp`
  - UI: `Public/UI/StageEnvironmentPanel.h`, `Private/UI/StageEnvironmentPanel.cpp`
  - Catalogs: `Public/Subsystems/StageItemSubsystem.h`, `StageEconomySubsystem.h`; `Private/Subsystems/StageItemSubsystem.cpp`, `StageEconomySubsystem.cpp`
  - Workspace: `Public/Workspace/StageWorkspaceSubsystem.h`, `StageWorkspaceTypes.h`; `Private/Workspace/StageWorkspaceSubsystem.cpp`, `StageWorkspaceShell.h/.cpp`, `SStageWorkspaceMenuBar.h/.cpp`, `StageWorkspaceConsoleCommands.cpp`; `Private/Input/StageShortcutRouting.cpp`
  - Scene: `Public/Scene/StageSceneComponent.h`, `StageSceneTypes.h`; `Private/Scene/StageSceneComponent.cpp`, `StageSceneStore.h/.cpp`, `StageSceneConsoleCommands.cpp`, `Tests/StageSceneStoreTests.cpp`
  - Camera and controller: `Public/Player/ModularPlayerController.h`, `StageCameraPawn.h`, `StageCameraNavigation.h`, `StageCameraNavigationComponent.h`; `Private/Player/ModularPlayerController.cpp`, `StageCameraPawn.cpp`, `StageCameraNavigationComponent.cpp`
  - Settings: `Public/Settings/StageCraftUserSettings.h`, `Private/Settings/StageCraftUserSettings.cpp`
  - AI: `Private/AI/StageAICommandParser.cpp`, `StageAICommandRunner.cpp`, `StageAICommandSchema.cpp`, `Tests/StageAITests.cpp`; `Private/Bridge/Tests/StageBridgeTests.cpp` (version-3 file built from the current version, not a hard-coded 4)
  - Docs: `Docs/ADR/0009-outdoor-environment.md` (§8), `Docs/CURRENT_SYSTEMS.md`, `Docs/PROJECT_AUDIT.md`, `tasks/todo.md` (Phase 33); STATE.md #56/#57 commit lines (follow-up commit `958b1ee`)

**Verification**
- **Builds:** Editor 0 warnings (one C4701 in the new ScreenshotWindow command was fixed). Game and Shipping (`-DisableAdaptiveUnity`): 0 warnings, both succeeded.
- **Shipping binary:** UTF-16 and ASCII scan of `ModularSceneBuilder-Win64-Shipping.exe`: `StageCraft.Bench.AtmosphereBlend`, `StageCraft.Scene.NewFromTemplate`, `StageCraft.Startup.PreviewLoader`, `StageCraft.Workspace.ScreenshotWindow`, `StageCraft.Environment.Atmosphere` and `StageNoWelcome` are absent; the user-facing "Start a stage", "Show at startup" and "Opening the stage…" are present (control).
- **Automation:** `Scripts/Tests/RunAutomation.ps1 -ExpectedCount 160` gives 160/160 with an offscreen renderer and 160/160 with `-NullRHI` on the final code, user data unchanged (65 files). New: `StageCraft.Startup.Readiness`, `StageCraft.Environment.AtmosphereBlend`, `.AtmospherePresets`, `.StudioWorld`, `StageCraft.Scene.Templates`; `Scene.Format` gained v5 and hostile-look cases. The first full run failed `Bridge.SceneOrigin` (it turned a file into v3 by replacing a hard-coded `"version": 4`) and `Scene.Templates` (Theatre deliberately uses the venue's lighting, not a preset); both tests were corrected. That run also reported `GameUserSettings.ini` changed: an existing test saves user settings, and the bytes only differed because the new `bShowWelcomeAtStartup=True` default was written; the next runs reported user data unchanged (follow-up below).
- **Scripted -game runs** (`RunGameScripted.ps1`, user data restored and hash-checked every time):
  - The MoviePlayer loader played during the map load; the workspace installed after it with no ensure (the first attempt, before the wait, hit `RegisterViewport`'s ensure).
  - Loader preview and welcome screen captured with `ScreenshotWindow` (cards Concert, Conference, Empty Stage, Theatre; Show at startup; Continue).
  - Each atmosphere applied through the controller and screenshot mid-blend and after; the four starter stages opened with every item placed (13, 14, 6, 0), untitled and clean, with the three-quarter camera glide; Ctrl+S on an opened starter stage shows the "Save Stage" name prompt.
  - `StageCraft.Bench.AtmosphereBlend` (all four atmospheres, Development editor binaries, 1600×900): frame 8.37 ms average / 20.0 ms worst while blending (715 frames) against 7.64 / 11.9 ms at rest (401 frames); game thread 4.64 / 8.95 ms against 4.34 / 7.44 ms.
- **Not verified by hand:** a cold start in a packaged build (the loader is visible longest there); the welcome and atmosphere cards, the Lighting choice and look sliders by mouse; fixture beams under Cyberpunk Night in a full show.

**Commit:** `eece06e`.

**Known issues / follow-ups**
- **An existing automated test saves `GameUserSettings.ini`** during the suite (same bytes before #58, so the runner never noticed). Find it and make it use a scoped settings object.
- **Gevor's imported FBX models re-import at every start** and block the game thread for about 50 s (`AG1.FBX`); loads, including starter stages, are refused as ImportsPending meanwhile. Pre-existing; the startup loader does not wait for imports.
- **Scripted runs** stalled when the runner was started through a nested `powershell -File` from the tool shell (clean with `&`); not a product issue, recorded in the tooling notes.
- The Environment panel's sliders and Lighting choice still write the environment directly, not through the controller (pre-existing, PROJECT_AUDIT); only atmosphere cards are undoable.
- No scalability or quality settings; the main window title is still the project name.
- Starter stages use one fixture type (the Sketch tool's Beam 280).
- v5 scene files are refused by builds older than #58 (VersionMismatch), as with every format bump.

## #59 — Phase F1: lighting-console gap analysis; programmer, output mix, multi-fixture editing with fan, fixture groups (2026-11-03)

**Status: implemented, awaiting manual verification.** Claude verified it with builds, the automated suite, scripted -game runs and a same-session benchmark against the previous commit (see Verification). Nobody has used the Groups panel, the fan or Highlight with a mouse yet.

**Request (Gevor, 2026-11-03, pasted brief):** a gap analysis of StageCraft against professional lighting consoles (grandMA3, MagicQ, Avolites Titan, Daslight 5), then implement the missing lighting control logic, UI patterns and workflow, step by step, with Rules.md, STATE.md and tasks/todo.md kept.

**Gap analysis** (read-only scan of the show-control, cue, patch, panel and selection code; full table in Docs/ADR/0025 §1):
- **Already at console level:**
  - undockable floating windows and saved layouts (ADR 0001);
  - patch with conflicts and auto-patch (ADR 0019);
  - Art-Net/sACN input;
  - the fixture model (ADR 0020).
- **Missing:**
  - G1, a programmer: edits wrote straight onto the fixture; the Inspector and Faders edited only the last-selected fixture; no Clear, Highlight or Fan; attribute edits not undoable.
  - G2, mixing: a fade overwrote a manual edit on the next frame.
  - G3, groups.
  - G4, palettes.
  - G5, cue timing beyond one fade.
  - G6, tracking.
  - G7, playback faders, flash, Grand Master, Blackout and Go/Back keys.
  - G8, an effects engine.
  - G9, DMX output and pan/tilt invert or swap.

**Gevor's decisions (AskUserQuestion):**
- the full roadmap, in steps (Phase F1–F5, tasks/todo.md Phase 34);
- tracking cues (F3);
- the main cue list plus 10 playback faders with Grand Master and Blackout (F3).

The plan was approved in plan mode.

**What changed (F1, ADR 0025)**
- **Output mix** (§2).
  - Each fixture's output (`Attributes`, unchanged for every reader) is mixed by `UShowControlSubsystem` from these layers:
    - its home look: new SaveGame `HomeAttributes`, sanitized in `ApplyItemData`; a pre-#59 state takes its restored output, a duplicate its current look;
    - the playing cue: `PlaybackValues` per fixture ID;
    - the programmer;
    - the highlight.
  - Pure rules: `StageShowMix` (ResolveAttribute, ResolveFixture, IsHeldAbovePlayback, FanFactor).
  - Fades start from the playback layer and never write held attributes (`RemoveHeldAttributes`).
  - `ResolveFixtures` runs on demand: programmer change, highlight, `RestoreActiveCue`, back to Internal, registering a fixture the show has data for.
  - Under External DMX only programmer and highlight values are written.
  - Cue store records the look under the highlight (`GetRecordableValues`).
  - New `RestoreActiveCue` (no fade).
- **Programmer** (§3). New `UStageProgrammerSubsystem` (world): values per fixture ID plus the highlight set; sanitized and bounded entries; a native fixtures-changed delegate and a dynamic `OnValuesChanged`.
- **Controller** (new `Private/Player/ModularPlayerControllerProgrammer.cpp`).
  - `RequestParameterChange` sends a fixture look row (`StageCraftTags::IsFixtureLookParameter`) to the new `RequestProgrammerChange`. That request writes every selected fixture through the GameMode's `EvaluateParameterChange`, takes what each write changed into the programmer, and records one undo step.
  - Fan modes Off, Linear, V, Inverted V spread the primary fixture's change (float rows).
  - New requests: `RequestClearProgrammer`, `RequestSetHighlight` (follows the selection), `RequestSetFanMode`, `RequestRecordGroup` / `UpdateGroup` / `RenameGroup` / `DeleteGroup` / `SelectGroup`.
  - `RequestStoreCue` now makes the stored cue the playing one and clears the programmer, in the same undo step as the store.
- **Undo.**
  - New `History/StageShowCommands` (`FStageProgrammerCommand`, `FStageFixtureGroupsCommand`).
  - New `IStageItemEditor::SetProgrammerValues` and `SetFixtureGroups`.
  - Programmer edits merge per fixture inside an edit transaction (a fader drag is one step).
  - `UStageEditHistoryComponent::RecordAsOneStep` is now public. The old private helper is renamed `RecordCommands`.
- **Groups** (§5). `FStageFixtureGroup` and `StageFixtureGroupRules` (ShowControlTypes), held by the new `UStageShowLibrarySubsystem` (world).
- **Scene format v6** (§7).
  - Adds `"programmer"`, `"groups"` and `"activeCue"`.
  - Load clears the old show before the new fixtures register, restores the cue without a fade, then the programmer and groups.
  - Bad entries are dropped and counted (`InvalidShowEntries`); marketplace packages treat them as Corrupt.
- **Session.** `HandleCueListChanged` is renamed `HandleShowDataChanged`. Programmer values and groups now mark the stage unsaved.
- **Keys** (§6), under a new SHOW category, routed from any panel:

  | Command | Default key |
  |---|---|
  | Go | Ctrl+Enter |
  | Back | Ctrl+Backspace |
  | Clear Programmer | Ctrl+Shift+C |
  | Highlight | Ctrl+Shift+H |

  A plain H was tried first: a new default takes its chord from a user override that already uses it, which `StageCraft.Input.SavedBindings` caught.
- **UI.**
  - New Groups panel (`StageCraft.Panel.Groups`, `UStageGroupsPanel`, Window > Groups & Programmer..., floating 560×420): programmer bar (RECORD, programmer count, CLEAR, HIGHLIGHT, FAN), group tiles, and the current group's rename / UPDATE / DELETE.
  - `DA_Panel_Groups` is made by the new `Scripts/Content/create_groups_content.py`.
  - The Inspector and Faders titles show "(+N)" when an edit applies to N other selected fixtures.
- **Dev commands** (non-Shipping):
  - `StageCraft.Show.Select <ids>`
  - `StageCraft.Show.Programmer Clear|Highlight|Fan|List`
  - `StageCraft.Show.Group Record|Select|Update|Delete|List`
  - `StageCraft.Workspace.ScreenshotWindow All`, which also saves floating panel windows
- **Docs:**
  - new ADR 0025;
  - CURRENT_SYSTEMS (programmer and groups rows, undo coverage, state owners, 164 tests);
  - PROJECT_AUDIT §4.4 update;
  - tasks/todo.md Phase 34.

**Files changed**
- **New:**
  - `Public/Show/StageShowMix.h`, `Private/Show/StageShowMix.cpp`
  - `Public/Subsystems/StageProgrammerSubsystem.h` / `.cpp`
  - `Public/Subsystems/StageShowLibrarySubsystem.h` / `.cpp`
  - `Public/History/StageShowCommands.h` / `.cpp`
  - `Private/Player/ModularPlayerControllerProgrammer.cpp`
  - `Public/UI/StageGroupsPanel.h` / `.cpp`
  - `Private/Show/Tests/StageProgrammerTests.cpp`
  - `Content/StageCraft/UI/Panels/DA_Panel_Groups.uasset`
  - `Scripts/Content/create_groups_content.py`
  - `Docs/ADR/0025-programmer-and-output-mix.md`
- **Modified:**
  - Show and fixtures: `Show/ShowControlTypes.h/.cpp`, `Actors/LightingFixtureActor.h/.cpp`, `Subsystems/ShowControlSubsystem.h/.cpp`, `Subsystems/StageSessionSubsystem.h/.cpp`.
  - History: `History/StageEditCommand.h`, `History/StageEditHistoryComponent.h/.cpp`.
  - Controller and input: `Player/ModularPlayerController.h/.cpp`, `Data/StageParameterTypes.h/.cpp`, `Input/StageKeyBindings.h/.cpp`, `Input/StageShortcutRouting.cpp`.
  - Scenes and marketplace: `Scene/StageSceneTypes.h`, `Scene/StageSceneStore.h/.cpp`, `Scene/StageSceneComponent.cpp`, `Marketplace/StagePackage.cpp`, `Marketplace/StageMarketplaceSubsystem.cpp`.
  - Console commands: `Show/StageShowConsoleCommands.cpp`, `Workspace/StageWorkspaceConsoleCommands.cpp`.
  - Workspace: `Workspace/StageWorkspaceTypes.h/.cpp`, `Workspace/SStageWorkspaceMenuBar.cpp`, `Workspace/StageWorkspaceShell.cpp`.
  - UI: `UI/StagePreferencesPanel.cpp`, `UI/StageParameterViewWidget.h/.cpp`, `UI/StageInspectorPanel.cpp`, `UI/StageFaderBankWidget.cpp`.
  - Docs and records: `Docs/CURRENT_SYSTEMS.md`, `Docs/PROJECT_AUDIT.md`, `tasks/todo.md`, `STATE.md` (#58 commit line).

**Verification**
- **Builds:** Editor, Game (Development) and Shipping all built with 0 warnings and 0 errors.
- **Shipping scan:**
  - absent: `StageCraft.Show.Programmer`, `StageCraft.Show.Group`, `StageCraft.Show.Select`, `ScreenshotWindow`;
  - present (UTF-16): "Programmer empty", "Clear Programmer", "FAN: INV V".
- **Automation:** `RunAutomation.ps1 -ExpectedCount 164` gave 164/164 with an offscreen renderer and 164/164 with `-NullRHI`, user data unchanged (65 files).
  - New tests: `StageCraft.Show.OutputMix`, `StageCraft.Show.Programmer`, `StageCraft.Show.FixtureGroups`, `StageCraft.Scene.ShowData`.
  - The first run failed `StageCraft.Input.SavedBindings` because of the plain-H default; fixed by moving Highlight to Ctrl+Shift+H.
- **Scripted -game runs** (`RunGameScripted.ps1 -WithoutUserImports`, user data restored and hash-checked). On the Concert starter stage:
  - **R59a:**
    - select fixtures 1–5, fan Linear, Pan 60: programmer -60/-30/0/30/60;
    - group "Front" recorded (1–5);
    - Highlight on/off;
    - Dimmer 0.4 then Edit.Undo: the dimmer left the programmer and pan stayed;
    - Cue Store 1: programmer emptied, cue 1 active;
    - Dimmer 0.3, then save, open Empty Stage, reload: programmer (5 × Dimmer 0.3), group "Front" and active cue 1 all restored.
  - **R59b:** two groups, Group Select 1, fan V, Tilt 40 and Dimmer 0.8. Captures show the Groups panel (floating window with tiles, "Programmer: 5 fixtures", CLEAR and FAN: V lit) and the Inspector and Faders titles "Beam 280 (Moving Head) (+4)".
  - Artefacts: `%TEMP%\StageCraftRuns\R59a`, `R59b`.
- **Benchmark (Rules §22), same session.** The previous commit `eece06e` was built in a temporary worktree (since removed); `RunFixtureBench.ps1 -Counts 200,500` was run there and then on F1.
  - Show tick while crossfading:
    - 200 fixtures: 4.78 ms mean / 9.90 max before, 3.96 / 8.74 after;
    - 500 fixtures: 10.07 / 22.3 before, 8.93 / 17.7 after.
  - So there is no regression from the mix. This is not claimed as a speed-up, because the runs differ by noise.
  - Frame and GPU times are not comparable: the F1 run also loaded the user's 3 imported models.
  - CSVs: `Saved/StageCraft/Benchmarks/FixtureBench_20261103-192152.csv`; the baseline CSV went with the worktree.
- **Not verified by hand:**
  - the Groups panel, the FAN button and HIGHLIGHT by mouse;
  - a fader drag over several fixtures (one undo step);
  - the new keys from a docked panel.

**Commit:** `d8a50e5` (together with the Rules.md change that lets Claude commit when Gevor says so, 2026-11-03).

**Known issues / follow-ups**
- **Behaviour changes (intended, ADR 0025 §10):**
  - a manual edit now survives a cue fade until Clear or a store;
  - switching from External DMX back to Internal restores the show's look.
- **Fan applies to float rows only.** Colour and wheel rows set the same value on every selected fixture.
- **Cue store still records every fixture's full look.** Tracking and Cue Only come in F3.
- **The Groups panel's current-group row** appears only after a tile click or RECORD in the panel itself, not after a console-made group.
- **Next steps:**
  - F2 palettes;
  - F3 cue timing, tracking, playbacks and masters (ADR 0026, scene v7);
  - F4 effects;
  - F5 DMX output.
- **The pre-existing follow-ups from #58 remain open.**

---

## #60 — Phase F2: palettes and palette references; colour-wheel fixtures follow colours (2026-11-03)

**What & why**

Gevor approved the Phase F plan (#59) and said "Go" for F2. F2 closes gap G4: StageCraft had no palettes. A look had to be set by hand on every fixture, and changing it meant re-recording every cue that used it. grandMA3, MagicQ and Titan all have palettes that cues refer to. Design: `Docs/ADR/0028-palettes-and-references.md`.

**Palettes** (`FStagePalette`, held in `UStageShowLibrarySubsystem` next to the groups):
- Each palette has a kind: Intensity, Position, Color, Beam, Focus or All. `StagePaletteRules::GetAttributeKind` sorts every attribute, cell attributes included, into one kind. A palette holds only attributes of its kind.
- **Global values** apply to any fixture that has the attribute.
  - A colour palette holds **one colour**, and each fixture mixes it its own way through `MatchColor`. Colour attribute values are never global, because ColorR means an emitter level on an RGB fixture but a filter's transmission on a CMY one.
- **Per-fixture values** are exact; they come from RECORD PER FIXTURE.
- Resolution order for a fixture and attribute: its own value, then the global value, then the colour match, then nothing.
- **Starter palettes** with fixed ids: White, Red, Green, Blue, Amber, Magenta, Cyan, UV, Home (pan/tilt 0) and Open (beam).
  - A new stage starts with them (the library's `Initialize`).
  - Files older than v7, including every starter stage, load with them.
  - A v7 file keeps exactly its own palettes.

**References, not copies:**
- `FShowCueFixtureState` gains `PaletteRefs` (attribute → palette id). Every referenced attribute keeps its plain value as a fallback, and `StagePaletteRules::SanitizeReferences` drops any reference without one.
- **The programmer** stores references with its values (`SetReferencedValues`, `FindReferences`). A plain edit replaces a reference. Undo captures both, and `FStageProgrammerCommand::IsMeaningfulChange` compares references too.
- **The mix** (`UShowControlSubsystem::GetProgrammerLayer` / `ApplyReferences`) resolves the programmer's references.
- **Cues:**
  - `GoToCue` and `RestoreActiveCue` resolve a cue's references once, so the per-frame fade path is unchanged.
  - `StoreCue` keeps the programmer's references for attributes the programmer holds, and the playing cue's for the rest (not mid-fade).
- **When a palette changes**, the show control (bound to `OnPalettesChanged`):
  1. finishes a running fade;
  2. re-resolves the playing cue's references into the playback layer (`RefreshPlaybackReferences`);
  3. re-mixes every fixture.

  A fixture that registers later also refreshes its references.
- **Deleting a palette that is in use** first turns its references into their current values in every cue and in the programmer. Palette, cue and programmer changes are one undo step, so nothing on stage changes.

**Requests** (`ModularPlayerControllerProgrammer.cpp`), each one undo step:
- `RequestApplyPalette`: the GameMode judges each attribute. A fixture with a refused attribute is left out entirely, so it never shows half a palette.
- `RequestRecordPalette(kind, name, EStagePaletteScope Global | PerFixture)`: records what the selection shows, without the highlight.
- `RequestUpdatePalette`, `RequestRenamePalette`, `RequestDeletePalette`.

**Colour-wheel fixtures:**
- **Problem.** R60a showed that "Red" had nothing for the Concert stage's moving heads: they have only a colour wheel, and `MatchColor` returned nothing for them.
- **Fix.** `ALightingFixtureActor::MatchColorValues` now gives a wheel-only fixture its **nearest wheel slot**, comparing colours at full brightness (`FindNearestColorWheelSlot`), as a console's colour picker does.
- This also makes the highlight open the wheel (white → Open), and the assistant's colours now reach wheel fixtures.

**Scene format v7** (`FStageSceneStore::FormatVersion` 7):
- `"palettes"` (`{id, name, kind by name, color?, values, fixtures}`);
- `"palettes": {tag: id}` on cue and programmer fixture entries.
- Damaged palettes, values and references are dropped and counted in `InvalidShowEntries`, which marketplace packages already treat as Corrupt.
- Palettes are set before the active cue and the programmer are restored, because both resolve references.
- **F3 now takes v8** (recorded in ADR 0028 and todo.md).

**UI:**
- `UStagePalettesPanel` (`StageCraft.Panel.Palettes`, Window > Palettes..., floating 600×460):
  - kind tabs, colour swatches, RECORD and RECORD PER FIXTURE;
  - a tile click applies the palette;
  - current-palette row: rename, UPDATE, DELETE, and "Used by N cues …".
- `DA_Panel_Palettes`, made headless by `Scripts/Content/create_palettes_content.py`.
- The session's unsaved mark follows palette edits.

**Development commands** (compiled out of Shipping):
- `StageCraft.Show.Palette Record <kind> [PerFixture] [name] | Apply | Update | Rename | Delete | List`
- `StageCraft.Show.Output <ids>` (logs a fixture's output values)
- `StageCraft.Show.Programmer List` now shows `@palette` on referenced values.

**Files changed**
- **New:**
  - `Docs/ADR/0028-palettes-and-references.md`
  - `Public/UI/StagePalettesPanel.h`, `Private/UI/StagePalettesPanel.cpp`
  - `Private/Show/Tests/StagePaletteTests.cpp`
  - `Scripts/Content/create_palettes_content.py`
  - `Content/StageCraft/UI/Panels/DA_Panel_Palettes.uasset`
- **Modified:**
  - Show and fixtures: `Public/Show/ShowControlTypes.h`, `Private/Show/ShowControlTypes.cpp`, `Public/Actors/LightingFixtureActor.h`, `Private/Actors/LightingFixtureActor.cpp`.
  - Subsystems: `Public/Subsystems/StageShowLibrarySubsystem.h`, `Private/Subsystems/StageShowLibrarySubsystem.cpp`, `Public/Subsystems/StageProgrammerSubsystem.h`, `Private/Subsystems/StageProgrammerSubsystem.cpp`, `Public/Subsystems/ShowControlSubsystem.h`, `Private/Subsystems/ShowControlSubsystem.cpp`, `Private/Subsystems/StageSessionSubsystem.cpp`.
  - History: `Public/History/StageEditCommand.h`, `Public/History/StageShowCommands.h`, `Private/History/StageShowCommands.cpp`, `Private/History/StageEditHistoryComponent.cpp`.
  - Controller: `Public/Player/ModularPlayerController.h`, `Private/Player/ModularPlayerControllerProgrammer.cpp`.
  - Scenes: `Public/Scene/StageSceneTypes.h`, `Private/Scene/StageSceneStore.h`, `Private/Scene/StageSceneStore.cpp`, `Private/Scene/StageSceneComponent.cpp`.
  - Workspace: `Public/Workspace/StageWorkspaceTypes.h`, `Private/Workspace/StageWorkspaceTypes.cpp`, `Private/Workspace/StageWorkspaceShell.cpp`, `Private/Workspace/SStageWorkspaceMenuBar.cpp`.
  - Console commands: `Private/Show/StageShowConsoleCommands.cpp`.
  - Docs and records: `Docs/CURRENT_SYSTEMS.md`, `tasks/todo.md`, `STATE.md` (#59 commit line, this entry).

**Verification**
- **Builds:** Editor, Game (Development) and Shipping built with 0 warnings and 0 errors, all on the final code.
- **Shipping UTF-16 scan:**
  - absent: `StageCraft.Show.Palette`, `StageCraft.Show.Output`, `StageCraft.Show.Group`;
  - present: "RECORD PER FIXTURE", "Not used by any cue".
- **Automation:** `RunAutomation.ps1 -ExpectedCount 167`: 167/167 with an offscreen renderer and 167/167 with `-NullRHI`, both on the final code. User data unchanged (66 and 67 files).
  - **New tests:**
    - `StageCraft.Show.PaletteRules` (pure): kinds, resolution order, reference and pool sanitizing, cues using a palette, starter palettes.
    - `StageCraft.Show.PaletteReferences` (world): a programmer and a cue follow a re-recorded palette; a Go fades to the palette's current value; undo and redo; a missing palette falls back to the stored value; wheel fixture slot matching.
    - `StageCraft.Scene.PaletteData`: v7 round trip, hostile palettes and references, an empty v7 list stays empty, v6 gets the starter palettes.
  - The first run failed one check of my own test (cue 1 was stored with a 0 s fade, so the Go had no halfway point). The test was fixed, not the code.
- **Scripted -game runs** on the Concert starter stage (`RunGameScripted.ps1 -WithoutUserImports`, user data restored and hash-checked):
  - **R60a:**
    1. Fixtures 1–5 at pan 30 recorded as "Front Pos". Clear, then Apply put pan/tilt `@Front Pos` in the programmer.
    2. Cue Store 1: the palette list shows cue 1 using it.
    3. Set pan −40 and Update: after Clear, cue 1 shows pan −40.
    4. Delete: pan stays −40.
    5. Undo: the palette is back and used by cue 1.
    6. Save, open Empty Stage, reload: the palette, cue 1 and pan −40 are all restored.
  - **R60b** (after the wheel fix):
    - Red and Home applied to the wheel movers: `ColorWheel=1@Red`, `Pan=0@Home`.
    - Cue 1 stored using Home.
    - A per-fixture colour palette was recorded.
    - The Palettes panel was captured as a floating window: kind tabs, swatches for the eight colours and "Mixed: 5 fixtures".
  - Artefacts: `%TEMP%\StageCraftRuns\R60a`, `R60b`.
- **Benchmark** (`RunFixtureBench.ps1 -Counts 200,500`, same machine as #59). Show tick while crossfading:

  | Fixtures | #59 (mean / max) | #60 (mean / max) |
  |---|---|---|
  | 200 | 3.96 / 8.74 ms | 3.64 / 8.50 ms |
  | 500 | 8.93 / 17.7 ms | 8.39 / 14.8 ms |

  - No regression; this is not claimed as a speed-up (noise).
  - The idle show tick is 0.
  - CSV: `Saved/StageCraft/Benchmarks/FixtureBench_20261103-210141.csv`.
- **Not verified by hand:** the Palettes panel by mouse (tabs, record buttons, tile apply, rename field, UPDATE/DELETE and the "used by" note), and Undo from the panel.

**Commit:** `d1c1efb` (committed by Claude with Gevor's permission, 2026-11-03).

**Known issues / follow-ups**
- **A global colour palette holds a colour only.** Colour temperature, CTO and exact wheel slots need RECORD PER FIXTURE.
- **Recording a global colour while the primary fixture is highlighted** takes the highlight's white.
- **A palette edit during a fade** snaps the fade to its end (ADR 0028 §3).
- **The current-palette row** appears only after a tile click or a RECORD made in the panel itself, as with the Groups panel.
- **The assistant** has no palette intents yet.
- **v7 files are refused by builds before #60** (VersionMismatch).
- **Next:** F3 (cue timing, tracking, playbacks and masters; ADR 0026; scene v8).

---

## #61 — Phase F3a: cue timing (delay, In/Out, feature times, Follow/Wait, S-curve) and tracking cues (2026-11-03)

**What & why**

Gevor asked for Phase F3: cue timing, tracking, playbacks and masters. He approved the plan with "Go". F3a closes gaps G5 and G6 (`Docs/ADR/0026-cue-timing-tracking-playbacks.md` §2–5):
- **G5:** a cue had one fade time.
- **G6:** every cue was a full snapshot, so nothing tracked.

F3b (playbacks, HTP/LTP, Grand Master and Blackout) follows as #62.

**Timing** (`FStageCueTiming` on `FShowCue`; `FadeTime` stays the In time, so every existing reader is unchanged):
- **Delay.**
- **Out time** for intensity going down.
- **Feature times** for Intensity, Position, Color, Beam and Focus (the F2 palette kinds).
- **Trigger:** Go, Follow (starts when the previous cue finishes) or Wait n s (starts n s after the previous cue's Go).
- **Curve:** Linear or S-curve.
- **The fade engine** (`UShowControlSubsystem::PlayCue` / `TickFades`) gives each changing attribute its own delay and duration (`StageCueTiming::GetAttributeTiming`). A cue finishes when its longest move ends.
- **Follow/Wait chains:**
  - an armed automatic Go (`PendingAutoGo`) fires from the tick, never inside the call that started a cue, so zero-time chains cannot recurse;
  - Back never arms one;
  - a manual Go, Go To, a show load or switching to External DMX cancels it;
  - the subsystem ticks only while a fade or an armed Go exists.

**Tracking** (`Show/StageCueTracking.h`, pure):
- A cue stores only what it changes. `StateAt` works out a cue's look from the nearest **blocked** cue forward; references carry through, and a later plain value drops them.
- **Playing a cue** fades every fixture in its look or in the playback layer:
  - values the look holds go to the look's value;
  - values the look doesn't hold go back to the home look.

  When the fade ends, the playback layer is exactly the look.
- **Block** writes the inherited look into the cue, so nothing changes on stage, and stops inheritance. **Unblock** only clears the flag.
- **Store modes** (`EStageCueStoreMode`):
  - **Track:** merge what the programmer holds; refused when the programmer is empty.
  - **Cue Only:** Track, plus the next cue keeps the old values, or home values.
  - **Look:** a blocked snapshot of everything; this is the old store and stays the API default for the assistant and existing callers.
  - The Cues panel defaults to TRACK.
  - Every store still clears the programmer and plays the cue as one undo step.

**Requests** (judged by `EvaluateCueRequest`, one undo step each, nothing recorded when nothing changed):
- `RequestStoreCue(..., Mode)`
- `RequestUpdateCueTiming`
- `RequestSetCueBlocked`

**Scene format v8:**
- Each cue saves `delay`, `out`, `times`, `trigger`, `wait`, `curve` and `block`.
- Bad values are dropped and counted in `InvalidCueEntries`.
- **v3–v7 cues were snapshots, so they load blocked** and play as they did. The one difference: a fixture added after such a cue was recorded now goes home in that cue.
- F3b adds its data to the same v8, because nothing between #61 and #62 is committed.

**Cues panel:**
- record row: IN, and the store-mode segments LOOK / TRACK / CUE ONLY; STORE becomes UPDATE (or RE-RECORD for LOOK) on an existing number;
- columns: IN, OUT, DELAY, TRIGGER (a cycle button), BLK;
- a "+" detail line per cue: INT, POS, COL, BEAM and FOC times (a time equal to In means "follow In"), WAIT and LINEAR/S-CURVE.

**Dev command:** `StageCraft.Show.Cue Track|CueOnly <n>`, `Timing <n> delay= out= wait= trigger= curve= int= pos= col= beam= foc=`, `Block <n> On|Off`. List shows the timing.

**Files changed**
- **New:**
  - `Docs/ADR/0026-cue-timing-tracking-playbacks.md`
  - `Public/Show/StageCueTracking.h`, `Private/Show/StageCueTracking.cpp`
  - `Private/Show/Tests/StageCueTrackingTests.cpp`
- **Modified:**
  - Show: `Public/Show/ShowControlTypes.h` (`EStagePaletteKind` moved above `FShowCue`; trigger, curve and store-mode enums; `FStageCueTiming`; `bBlock`; `SanitizeTiming`), `Private/Show/ShowControlTypes.cpp`.
  - Subsystem: `Public/Subsystems/ShowControlSubsystem.h`, `Private/Subsystems/ShowControlSubsystem.cpp` (the cue section rewritten).
  - Controller: `Public/Player/ModularPlayerController.h`, `Private/Player/ModularPlayerController.cpp`.
  - Scenes: `Private/Scene/StageSceneStore.h`, `Private/Scene/StageSceneStore.cpp`.
  - Cues panel: `Public/UI/StageCuePanel.h`, `Private/UI/StageCuePanel.cpp`.
  - Console commands: `Private/Show/StageShowConsoleCommands.cpp`.
  - Docs and records: `Docs/CURRENT_SYSTEMS.md`, `tasks/todo.md`, `STATE.md` (#60 commit line, this entry).

**Verification**
- **Builds:** Editor, Game and Shipping built with 0 warnings and 0 errors.
- **Shipping UTF-16 scan:**
  - absent: `StageCraft.Show.Cue`, the timing usage text, `StageCraft.Show.Palette`;
  - present: "CUE ONLY", "Show or hide this cue".
- **Automation:**
  - All 167 earlier tests passed on the new engine before any new test was written.
  - With the new tests, `RunAutomation.ps1 -ExpectedCount 170` gives 170/170 with an offscreen renderer and 170/170 with `-NullRHI`. User data unchanged (67 files).
  - **New tests:**
    - `StageCraft.Show.CueTracking` (pure): track-through, block, references through tracking, merge, Cue Only (old value, home value, fixture not on stage), timing per attribute, progress and S-curve, timing sanitizing.
    - `StageCraft.Show.CueTiming` (world):
      - delay with In and Position times mid-fade;
      - the Out time for intensity down while pan tracks;
      - a Follow then a Wait chain;
      - Back arms nothing;
      - a blocked cue sends the rest home;
      - a manual Go cancels a chain;
      - Track, Cue Only and Look stores; Block keeps the look; timing update.
    - `StageCraft.Scene.CueTimingData`: v8 round trip, 9 hostile values dropped and counted, v7 cues load blocked.
- **Scripted run R61a** on the Concert starter stage (user data restored and hash-checked):
  - Three Track stores; then cue 2 got delay 1, a 4 s Position time and an S-curve; cue 3 got Follow and a 1 s Out time.
  - Go from cue 1, pan measured against the S-curve: 9.26° at 2 s (expected about 9.4°), 40.9° at 3.5 s (expected about 41°).
  - Cue 3 followed by itself: dimmer 0.3, pan 60 tracked from cue 2.
  - Block 3 kept the look.
  - Save, open Empty Stage, reload: cues, timing, block and the look are all restored.
  - The Cues panel capture shows the new columns, FOLLOW lit, B lit on the blocked cue, and TRACK chosen.
  - Artefacts: `%TEMP%\StageCraftRuns\R61a`.
- **Benchmark** (`RunFixtureBench.ps1 -Counts 200,500`; the fade loop changed). Show tick while crossfading:

  | Fixtures | #60 (mean / max) | #61 (mean / max) |
  |---|---|---|
  | 200 | 3.64 / 8.50 ms | 3.74 / 6.51 ms |
  | 500 | 8.39 / 14.8 ms | 8.38 / 20.8 ms |

  - Means are unchanged. The 500 maximum is a single frame (the eece06e baseline in #59 had 22.3).
  - Idle tick 0.
  - CSV: `Saved/StageCraft/Benchmarks/FixtureBench_20261103-213913.csv`.
- **Not verified by hand:** the panel by mouse (time fields, the trigger cycle, BLK, the detail line, the store-mode segments).

**Commit:** `3902468` (together with #62).

**Known issues / follow-ups**
- **The record row's cue number** is not refreshed after a store made outside the panel or a scene load, so it can offer UPDATE on an existing cue. This behaviour existed before (it said RE-RECORD).
- **A palette edit or a fixture arriving during a fade** snaps the fade to its end (ADR 0028 §3).
- **Next:** F3b (#62): playbacks, HTP/LTP, Grand Master and Blackout.

---

## #62 — Phase F3b: ten playback faders (HTP/LTP, Go, Flash, Record, Release), Grand Master and Blackout, Lighting Desk layout (2026-11-03)

**What & why**

This is the second half of Gevor's Phase F3 brief: the main stack plus 10 playback faders, HTP for dimmers and LTP for everything else, Flash, Grand Master and Blackout. It closes gap G7. Design: `Docs/ADR/0026-cue-timing-tracking-playbacks.md` §6, written out in full with this step.

**Playbacks** (`FStagePlayback {Name, Cues}`, ten of them, held by `UShowControlSubsystem`):
- Each owns its own tracking cue list.
  - **REC** appends what the programmer holds as the next cue and clears the programmer (one undo step).
  - **GO** steps through the list.
- **Live state** (`FPlaybackRuntime`: level, flash, current cue, activation order and the resolved look) is neither saved nor undoable.
  - A fader leaving 0, a flash going down or a GO activates the playback (at its first cue when released) and makes it the latest for LTP.
  - REL releases it; a show load releases all of them.
- *Deviation from the plan, recorded in the ADR:* there is no separate cue-list pool with an assignment picker. Each playback owns its list, which a console playback with its own stack covers. A playback steps at once (its fader is the fade); its cue timing is not played.

**Mix** (`StageShowMix`, pure), from the bottom up:
1. the home look;
2. the main list and the active playbacks:
   - **intensity HTP:** the highest of the main list and each playback × its level (1 while flashed);
   - **everything else LTP**, by activation order, playbacks above the main list;
3. the programmer;
4. the highlight;
5. intensity × Grand Master × (Blackout ? 0 : 1), the programmer and highlight included.

**The fade loop** keeps its direct path while no playback is up and the masters are at full (`IsMainOnlyMix`). Otherwise every fading fixture is re-mixed each frame.

**Masters and External DMX:**
- Under External, the masters scale the incoming intensity, and a change makes the next frames re-read in full.
- Playbacks are refused under External.

**Recording:** a Look store and a palette record take the look without the masters (`GetRecordableValues`).

**Requests** (`EvaluateCueRequest`):
- `PlaybackEdit`, with an index check: REC, CLR, rename (`FStagePlaybacksCommand`).
- `PlaybackLive`, refused under External: level, flash (letting go is never refused), GO, REL.
- `Masters`: GM, Blackout.
- **Key:** Blackout on Ctrl+Shift+B (SHOW category, works from any panel).

**Scene format v8** (shared with #61):
- `"playbacks"`: index, name and cues for each used playback.
- Damaged entries are dropped and counted in `InvalidCueEntries`.
- Levels and masters are not saved: a stage opens at full with everything released.

**UI:**
- **Playback panel** (`StageCraft.Panel.Playback`, Window > Playbacks & Masters..., floating 860×400; `DA_Panel_Playback` made by `Scripts/Content/create_playback_content.py`):
  - ten strips: name, cue position, vertical fader, level, GO, FLASH (held; new `UStageFlashButton`), REC, REL, CLR;
  - GM fader and BLACKOUT;
  - strips are refreshed in place, so a dragged fader is never rebuilt.
- **Built-in "Lighting Desk" layout** (Layout menu): viewport over the Playback, Cues and Faders tabs; Groups and Palettes on the right; Inspector and Outliner below them.

**Dev commands:**
- `StageCraft.Show.Playback <1..10> Record|Clear|Rename|Level|Flash|Go|Release|List`
- `StageCraft.Show.Master GM|Blackout`
- `StageCraft.Bench.Fixtures ... GM=<0..1>`, plus `RunFixtureBench.ps1 -GM`, to measure the fully mixed fade path.

**Files changed**
- **New:**
  - `Public/UI/StagePlaybackPanel.h`, `Private/UI/StagePlaybackPanel.cpp`
  - `Private/Show/Tests/StagePlaybackTests.cpp`
  - `Scripts/Content/create_playback_content.py`
  - `Content/StageCraft/UI/Panels/DA_Panel_Playback.uasset`
- **Modified:**
  - Show: `Public/Show/ShowControlTypes.h` (request kinds, `FStagePlayback`, `StagePlaybackRules`), `Private/Show/ShowControlTypes.cpp`, `Public/Show/StageShowMix.h`, `Private/Show/StageShowMix.cpp`.
  - Subsystems: `Public/Subsystems/ShowControlSubsystem.h`, `Private/Subsystems/ShowControlSubsystem.cpp`, `Private/Subsystems/StageSessionSubsystem.cpp`.
  - History: `Public/History/StageEditCommand.h`, `Public/History/StageShowCommands.h`, `Private/History/StageShowCommands.cpp`, `Private/History/StageEditHistoryComponent.cpp`.
  - Controller and rules: `Public/Player/ModularPlayerController.h`, `Private/Player/ModularPlayerController.cpp`, `Private/Player/ModularPlayerControllerProgrammer.cpp`, `Private/Game/StageCraftGameModeBase.cpp`.
  - Input: `Public/Input/StageKeyBindings.h`, `Private/Input/StageKeyBindings.cpp`, `Private/Input/StageShortcutRouting.cpp`.
  - Scenes: `Public/Scene/StageSceneTypes.h`, `Private/Scene/StageSceneStore.h`, `Private/Scene/StageSceneStore.cpp`, `Private/Scene/StageSceneComponent.cpp`.
  - Buttons: `Public/UI/StageToolButton.h`, `Private/UI/StageToolButton.cpp` (`UStageFlashButton`).
  - Workspace: `Public/Workspace/StageWorkspaceTypes.h`, `Private/Workspace/StageWorkspaceTypes.cpp`, `Public/Workspace/StageWorkspaceSubsystem.h`, `Private/Workspace/StageWorkspaceSubsystem.cpp`, `Private/Workspace/StageWorkspaceShell.h`, `Private/Workspace/StageWorkspaceShell.cpp`, `Private/Workspace/SStageWorkspaceMenuBar.cpp`.
  - Console commands and bench: `Private/Show/StageShowConsoleCommands.cpp`, `Private/Diagnostics/StageFixtureBenchmark.cpp`, `Scripts/Bench/RunFixtureBench.ps1`.
  - Docs and records: `Docs/ADR/0026-cue-timing-tracking-playbacks.md`, `Docs/CURRENT_SYSTEMS.md`, `tasks/todo.md`, `STATE.md`.

**Verification**
- **Builds:** Editor, Game and Shipping built with 0 warnings and 0 errors.
- **Shipping UTF-16 scan:**
  - absent: `StageCraft.Show.Playback`, `StageCraft.Show.Master`, `StageCraft.Show.Cue`, `StageCraft.Bench.Fixtures`;
  - present: "BLACKOUT", "Hold to bring this playback to full".
- **Automation:** `RunAutomation.ps1 -ExpectedCount 173` gives 173/173 with an offscreen renderer and 173/173 with `-NullRHI`. User data unchanged (68 files).
  - **New tests:**
    - `StageCraft.Show.PlaybackMix` (pure):
      - HTP against the main list;
      - LTP by order;
      - cell dimmers as intensity;
      - the programmer above the playbacks;
      - the Grand Master on the highlight and home look, but not on position;
      - Blackout;
      - the playback rules.
    - `StageCraft.Show.Playbacks` (world):
      - fader start and HTP;
      - LTP by raising; flash and let go;
      - GO through a tracking list and its end;
      - Grand Master and Blackout;
      - a Look store under GM records 0.6, not 0.3;
      - a main fade mixed with a fader up, and the fader pulled mid-fade;
      - release; clear and rename with undo and redo;
      - a load releases; bad levels refused.
    - `StageCraft.Scene.PlaybackData`: v8 round trip, 5 hostile entries dropped and counted, v7 gives ten empty playbacks.
- **Scripted run R62a** on the Concert starter stage (user data restored and hash-checked):
  1. Lighting Desk layout applied.
  2. Main cue 1 stored with dimmer 0.2.
  3. Playback 1 recorded (dimmer 1, pan 45); playback 2 recorded (pan −60), named Sweep.
  4. P1 at 50% → dimmer 0.5, pan 45. P2 at 30% → pan −60 (LTP), dimmer stays 0.5 (HTP).
  5. Flash P1 → 1.0, pan 45. GM 50% → 0.25. Blackout → 0, pan unchanged.
  6. Saved, opened Empty Stage, reloaded: both playbacks back, released. P2 up → pan −60 over the main cue's 0.2.
  - The capture shows the Lighting Desk with P1 and "2 Sweep" lit at 50% and 30%, GM 50% and BLACKOUT lit.
  - Artefacts: `%TEMP%\StageCraftRuns\R62a`.
- **Benchmark** (`RunFixtureBench.ps1 -Counts 200,500`). Show tick while crossfading:

  | Path | 200 fixtures (mean / max) | 500 fixtures (mean / max) |
  |---|---|---|
  | Main only (GM=1) | 3.56 / 6.90 ms | 7.99 / 15.6 ms |
  | Fully mixed (GM=0.9) | 4.00 / 8.92 ms | 8.92 / 18.2 ms |

  - Main only is no regression against #60 (3.64 / 8.39 means).
  - Fully mixed costs about 12% more, and only while a playback is up or the masters are down.
  - CSVs: `Saved/StageCraft/Benchmarks/FixtureBench_20261103-220506.csv` (GM 1), `FixtureBench_20261103-220700.csv` (GM 0.9).
- **Not verified by hand:** the Playback panel by mouse (fader drag, FLASH hold and release, REC/REL/CLR, GM, BLACKOUT), Ctrl+Shift+B, and the Lighting Desk from the Layout menu.

**Commit:** `3902468` (together with #61).

**Known issues / follow-ups**
- **Fixtures without a dimmer channel are not mastered:** their intensity is in their colour levels. A virtual dimmer is a follow-up.
- **A playback steps at once:** its cues' timing is not played.
- **No playback renaming in the panel** (the console command and the API exist).
- **Under External DMX**, a master change shows on the console's next frame.
- **Next:** F4 effects (#63), F5 DMX output (#64).

---

## #63 — Phase F4a: effects engine (waveforms, spread, wings, blocks), effects in the programmer, Effects panel (2026-11-04)

**What & why**

This is the first half of Gevor's Phase F4 brief ("Effects Engine & Motion Patterns"): procedural waveforms for intensity, position, colour and beam; spread, phase offset and fan across fixtures; an Effects panel with speed (Hz/BPM), size, phase, direction and chase/strobe patterns; FX state in the programmer. It closes most of gap G8. Storing effects in cues and playbacks (with tracking) is F4b (#64), because it touches the tracking engine and the cue format and is tested on its own. Design: `Docs/ADR/0027-effects-engine.md`.

**The effect** (`FStageEffect`, `Public/Show/StageEffectTypes.h`):
- **Target:** Intensity (every intensity attribute: dimmer, cell dimmers), Position (pan/tilt), Color, Beam (one beam or focus attribute).
- **Waveform:** Sine, Cosine, Square (with duty: chases and strobes), Ramp Up, Ramp Down, Triangle, Random, Circle, Figure 8.
- **Levels:** Intensity and Beam swing between Low and High as fractions of **each fixture's own range**; Position swings Size degrees either side of the static position; Color fades A→B or runs the rainbow, matched by each fixture (`MatchColor`: emitters, CMY, nearest wheel slot).
- **Spread:** Phase (start), Spread (degrees across the fixtures: 360 = a chase), Wings (mirrored parts), Blocks (fixtures moving together), Direction (Forward, Reverse, Bounce without a jump), and the existing fan modes on the size.
- **Rate** in Hz, shown as BPM; TAP in the panel.
- `EStageFanMode` moved into the new header (ShowControlTypes.h includes it), so effects reuse it.

**The maths** (`StageEffects`, pure, no world): pattern step after wings and blocks, offset, wave position with direction, waveform sample. Random is seeded by the fixture's step and the cycle, so it is the same on every run (Rules §54), shared within a block and symmetrical across wings.

**The mix** (`StageShowMix`): a new effect layer **above the programmer and below the highlight**; masters still last.
- Position effects are **relative** (added to the static value); intensity, colour and beam are **absolute** (replace it). Each value has a weight (fade in/out; always 1 in F4a, used by F4b).
- The highlight beats an effect; the Grand Master and Blackout scale an intensity effect.

**The runner** (`FStageEffectRunner`, owned by `UShowControlSubsystem`, which stays the only writer):
- Keeps each effect's clock in **cycles**, advanced by its own rate, so a rate change never jumps the pattern and an edit keeps the clock.
- Finds the driven fixtures on change and on fixture register/unregister, never per frame; per frame only the driven fixtures' driven attributes are mixed (`ResolveFixture` scope `EffectAttributes`).
- The subsystem ticks while an effect runs under Internal control (`AreEffectsLive`); with an effect live, fades go through the full mix (`IsMainOnlyMix` false). With none running nothing changes.
- A renumbered fixture leaves its effects; a destroyed one drops out. Under External DMX effects hold still and are not written.
- Recording (Look store, palette record) takes the static look, never an effect's momentary value.

**The programmer** (`UStageProgrammerSubsystem`) holds effect entries (`FStageEffectEntry {Id, Effect, FixtureIds}`): `AddEffect`, `UpdateEffect`, `RemoveEffect`, `SetEffects`, `OnEffectsChanged` (plus `OnValuesChanged` for dirty and panels). **Clear** removes values and effects as one undo step.

**Requests** (`AModularPlayerController`, judged by `EvaluateCueRequest` with the new `EStageCueRequest::Effects`, refused under External):
- `RequestStartEffect` (the selection, in selection order), `RequestUpdateEffect`, `RequestStopEffect` (invalid id = all): one undo step each (`FStageProgrammerEffectsCommand`, `IStageItemEditor::SetProgrammerEffects`).
- `RequestPreviewEffect`: live while a slider is dragged, no step; the release commits one step from before the first preview.
- Validation: `StageEffectRules::SanitizeEffect` / `SanitizeEntries` (finite, rate ≤ 25 Hz, size ≤ 270°, phase/spread ±3600°, wings 1–16, blocks 1–512, levels and colours 0–1, real beam attributes, ≤ 64 effects of ≤ 4096 unique positive fixture ids) on every entry: request, undo step, file.

**Scene format v9:** `"programmerEffects"` with every setting, enums by name. An entry without a valid id is dropped; an unknown name or bad number falls back to that setting's default; each counted in `InvalidShowEntries`. v1–v8 load with no effects; v9 files are refused by builds before #63.

**UI:**
- **Effects panel** (`StageCraft.Panel.Effects`, Window > Effects..., floating 900×460, a tab of the Lighting Desk layout; `DA_Panel_Effects` from `Scripts/Content/create_effects_content.py`):
  - 12 quick effects (Circle, Figure 8, Pan Sweep, Tilt Wave, Dimmer Sine, Dimmer Chase, Strobe, Random Sparkle, Rainbow, Colour Fade, Iris Pulse, Zoom Breathe) start on the selection in one click;
  - the editor of the chosen effect: WAVE, DIR, FAN, AXIS, COLOUR, A, B, BEAM choices (only those that apply), RATE (Hz · BPM, with TAP), SIZE, LOW, HIGH, PHASE, SPREAD, DUTY, WINGS, BLOCKS;
  - a live preview (`UStageEffectPreview` / `SStageEffectPreview`): one cycle of the wave and a dot per fixture (circle and figure 8 as their pan/tilt path, kept round);
  - the running list with STOP and STOP ALL.
- **Dev command** (compiled out of Shipping): `StageCraft.Show.Effect Start <quick effect> [key=value...] | Set <n> key=value... | Stop <n>|All | List`.
- **Bench:** `StageCraft.Bench.Fixtures ... Effects=0|1|2` and `RunFixtureBench.ps1 -Effects` (1: circle + dimmer sine on every bench fixture; 2: also a rainbow).

**Files changed**
- **New:**
  - `Docs/ADR/0027-effects-engine.md`
  - `Public/Show/StageEffectTypes.h`, `Private/Show/StageEffectTypes.cpp`
  - `Public/Show/StageEffects.h`, `Private/Show/StageEffects.cpp`
  - `Public/Show/StageEffectRunner.h`, `Private/Show/StageEffectRunner.cpp`
  - `Public/UI/StageEffectsPanel.h`, `Private/UI/StageEffectsPanel.cpp`
  - `Public/UI/StageEffectPreview.h`, `Private/UI/StageEffectPreview.cpp`, `Private/UI/SStageEffectPreview.h`, `Private/UI/SStageEffectPreview.cpp`
  - `Private/Show/Tests/StageEffectTests.cpp`
  - `Scripts/Content/create_effects_content.py`, `Content/StageCraft/UI/Panels/DA_Panel_Effects.uasset`
- **Modified:**
  - Show: `Public/Show/ShowControlTypes.h` (`EStageFanMode` moved out, `EStageCueRequest::Effects`), `Public/Show/StageShowMix.h`, `Private/Show/StageShowMix.cpp`.
  - Subsystems: `Public/Subsystems/ShowControlSubsystem.h`, `Private/Subsystems/ShowControlSubsystem.cpp`, `Public/Subsystems/StageProgrammerSubsystem.h`, `Private/Subsystems/StageProgrammerSubsystem.cpp`.
  - History: `Public/History/StageEditCommand.h`, `Public/History/StageShowCommands.h`, `Private/History/StageShowCommands.cpp`, `Private/History/StageEditHistoryComponent.cpp`.
  - Controller and rules: `Public/Player/ModularPlayerController.h`, `Private/Player/ModularPlayerControllerProgrammer.cpp`, `Private/Game/StageCraftGameModeBase.cpp`.
  - Scenes: `Public/Scene/StageSceneTypes.h`, `Private/Scene/StageSceneStore.h`, `Private/Scene/StageSceneStore.cpp`, `Private/Scene/StageSceneComponent.cpp`.
  - Workspace: `Public/Workspace/StageWorkspaceTypes.h`, `Private/Workspace/StageWorkspaceTypes.cpp`, `Private/Workspace/StageWorkspaceShell.cpp`, `Private/Workspace/SStageWorkspaceMenuBar.cpp`.
  - Console and bench: `Private/Show/StageShowConsoleCommands.cpp`, `Private/Diagnostics/StageFixtureBenchmark.cpp`, `Scripts/Bench/RunFixtureBench.ps1`.
  - Tests: `Private/History/Tests/StageHistoryWorldTests.cpp` (the Effects rule under External and Internal).
  - Records: `Docs/CURRENT_SYSTEMS.md`, `tasks/todo.md`, `STATE.md` (also the #61/#62 commit hash `3902468`).

**Verification**
- **Builds:** Editor, Game and Shipping built with 0 warnings and 0 errors (final Game/Shipping build after the last panel fix).
- **Shipping UTF-16 scan:**
  - absent: `StageCraft.Show.Effect`, `StageCraft.Show.Playback`, `StageCraft.Bench.Fixtures`, the Effect command's usage text;
  - present: "STOP ALL", "Tap in time: the rate follows your beat".
- **Automation:** `RunAutomation.ps1 -ExpectedCount 179` gives 179/179 with an offscreen renderer and 179/179 with `-NullRHI`. User data unchanged (70 files).
  - **New tests:**
    - `StageCraft.Show.EffectWaves` (pure): every waveform at its key points, wrap and negative positions; Random holds within a cycle, repeats, stays in range, changes per cycle and per fixture step; Circle stays on the unit circle; Figure 8's tilt at twice the rate.
    - `StageCraft.Show.EffectSpread` (pure): spread offsets, two wings mirrored, blocks of two, phase, single fixture, out-of-range index; Forward/Reverse chase order; Bounce without a jump at the turn; fan V; levels; rainbow and crossfade colours.
    - `StageCraft.Show.EffectRules` (pure): every clamp and fallback (rate, spread, NaN size, Low, wings/blocks, unknown enum, colour, Beam attribute, name); entry repair (bad id, duplicate id, bad and duplicate fixture IDs, empty entries; drops counted exactly); MaxEffects; BPM; quick effects valid and named apart.
    - `StageCraft.Show.EffectMix` (pure): relative on the programmer, absolute, weight for both, masters on an intensity effect, highlight above, no static value.
    - `StageCraft.Show.Effects` (world, 3 movers): a circle relative to a programmer pan (clock 0 and a quarter cycle); recording takes the static look; a dimmer chase on its own clock steps in selection order; the Grand Master scales it; the highlight beats it; an edit keeps the clock; undo and redo; a renumbered fixture leaves the effect; a destroyed fixture; External holds still and stops the tick; stop restores the static look and stops the tick; no valid fixtures, no effect.
    - `StageCraft.Scene.EffectData`: v9 round trip of every setting; a hostile file (bad ids, non-objects, bad fixtures, unknown names, out-of-range numbers, a non-beam attribute, a duplicate id, no fixtures) keeps one effect with defaults and counts ≥ 12 drops; a v8 file reads no effects.
    - `StageCraft.History.*` cue rules: `Effects` refused under External, allowed under Internal.
- **Scripted run R63a** on the Concert starter stage, Lighting Desk layout with the Effects tab (user data restored and hash-checked):
  1. Fixtures 1–5 selected; Circle started: fixture 1 pan 17.5° / tilt 17.8° at an eighth of a cycle, pan 25.0° / tilt 0.2° at a quarter.
  2. Dimmer Chase: one fixture on at a time, fixture 2 then fixture 3; set to 120 BPM, duty 0.2.
  3. Rainbow: the wheel-only movers on different wheel slots (3, 7, 4, 2, 7, 6, 5 over two reads).
  4. GM 0.5: the chased fixture at 0.5.
  5. Rainbow stopped; saved R63Fx; Empty Stage; reloaded: Circle and Dimmer Chase back with their settings; Stop All: fixture 1 back to pan 0, tilt 0, dimmer 1.
  - Captures: the panel with the CIRCLE wave lit, the running list and the viewport's rainbow beams; after reload the Dimmer Chase square wave with one dot high and four low.
  - **R63b** after two panel fixes (the circle preview drawn as a wide ellipse; the TAP button shortening the RATE slider): the circle is round with the five fixtures spread round it (spread 360°), and the sliders line up.
  - Artefacts: `%TEMP%\StageCraftRuns\R63a`, `R63b`.
- **Benchmark** (`RunFixtureBench.ps1 -Counts 200,500`), show tick mean:

  | Case | 200 fixtures | 500 fixtures |
  |---|---|---|
  | F3 commit `3902468` (worktree A/B, two runs today) | 4.25, 3.82 ms (crossfade) | 9.92, 9.53 ms (crossfade) |
  | F4a, no effect running (four runs today) | 3.91, 3.99, 3.78, 3.76 ms (crossfade) | 8.49, 8.85, 8.52, 8.11 ms (crossfade) |
  | F4a, `-Effects 1`: circle + dimmer sine on every fixture | 3.70 ms idle, 4.49 ms crossfade | 10.2 ms idle, 11.3 ms crossfade |
  | F4a, `-Effects 2`: also a rainbow | 4.11 ms idle, 4.63 ms crossfade | 9.82 ms idle, 11.2 ms crossfade |

  - **No regression** on the existing path: back to back today, F4a without effects is at or below the F3 commit. (Today's machine is slower than when #62 recorded 3.56 / 7.99 ms, which is why the A/B was run.)
  - **Effects cost about what a continuous crossfade of every fixture costs** (~19 µs per driven fixture per frame, most of it the fixture's own visual update), and only while effects run. The rainbow's colour matching adds little.
  - CSVs: `Saved/StageCraft/Benchmarks/FixtureBench_20261104-150631.csv` (none), `-150827` (Effects 1), `-151028` (Effects 2), `-152140` (A/B current); baseline CSVs were in the removed worktree.
- **Not verified by hand:** the Effects panel by mouse (a quick effect on a selection, slider drags as one undo step each, TAP, the choice rows, STOP / STOP ALL), Window > Effects..., and Ctrl+Z after each.

**Commit:** `065d0cf` (committed by Gevor, 2026-11-04).

**Known issues / follow-ups**
- **Effects live in the programmer only.** Cues and playbacks storing effects, with tracking and fades, is F4b (#64).
- **No saved effect pool and no global speed master** (ADR 0027 §8): the quick effects and, with F4b, cues cover the brief.
- **The rate slider tops out at 10 Hz** (quadratic, for fine slow control); faster rates (to 25 Hz) come from TAP, the Strobe quick effect or the console.
- **Colour A/B are eight preset colours**, not a picker.
- **Fixtures without a dimmer channel** still ignore the Grand Master (ADR 0026 §8).
- **Next:** F4b (#64), then F5 DMX output (#65).


---

## #64 — Phase F4b: effects in cues and playbacks (tracking, fades, morph, stops), scene format v10 (2026-11-04)

**What & why**

Gevor's Phase F4b brief: running effects recorded into cues, playback cues and submasters (a one-cue playback); effects that start, fade in and out, and track or block through cue lists; scene format v10. It completes F4 (gap G8). Design: `Docs/ADR/0027-effects-engine.md` §5.

**Data and tracking** (`Public/Show/ShowControlTypes.h`, `Public/Show/StageCueTracking.h`):
- `FShowCue` gains `Effects` (`FStageEffectEntry`, as in the programmer) and `EffectStops` (ids it ends). `StageCueRules::SanitizeCueEffects` (also run by `SanitizeCueList`) applies the effect rules; `StageEffectRules::SanitizeStops` keeps stops valid, unique, never an id the same cue starts, at most 64.
- `StageCueTracking::EffectsAt`: from the nearest blocked cue forward, a cue's effects start (the same id replaces and moves last, so it wins an attribute) and its stops end them. `MergeEffectsInto` (Track) and `KeepNextCueEffects` (Cue Only: the next cue gets the old version back, or a stop for a new effect) mirror the value functions. `StageCueTiming::GetFeatureTiming` gives an effect its feature's time.

**Stores** (`UShowControlSubsystem::StoreCue`, `RecordPlayback`; controller `RequestStoreCue`, `RequestRecordPlayback`):
- Track merges the programmer's effects and stops into the cue; Cue Only also keeps the next cue; Look records every running effect (`CaptureEffects`); Block writes the inherited effects into the cue and clears its stops; Record Playback takes the programmer's effects and stops.
- A tracking store or record needs values, an effect or a stop in the programmer. Afterwards the programmer is emptied of values, effects and stops in the same undo step (`AModularPlayerController::AddProgrammerClear`, also used by Clear). The cue becomes the playing one first, so a stored effect carries on under the same id and clock.

**Sources at run time** (`UShowControlSubsystem::RefreshEffectSources`): the main list's effects at the active cue (`MainEffects`, changed by Go, Back, Go To, restore, load), then each active playback's (`FPlaybackRuntime::Effects`) in activation order, then the programmer's; the same id runs once as its latest; the programmer's stops leave ids out. A playback's intensity effects are scaled by its fader (`FSourcedEntry::IntensityScale`); a fader move only rescales (no re-mix).

**Runner** (`FStageEffectRunner`, rewritten around sourced entries):
- `SetEffects(Entries, FindFixture, TimedBy)`: with a cue (Go, Back) an effect that starts fades its weight in, one that stops fades out and is then dropped (its fixtures re-mixed in full from the tick), and one whose settings change morphs (old and new settings evaluated on the same clock and blended, the rate blended too); timing from the cue's feature time, delay and curve, the Out time for intensity effects stopping. Without a cue changes are immediate. An unchanged effect keeps any fade it is in; one fading out finishes. Returns whether anything but a scale changed.
- `StageShowMix::ComposeEffect`: two effects on one attribute compose by weight (one fading in over another crossfades); at weight 1 it is F4a's "later wins".
- `GetRunning` (with source and whether stopping), `GetWeight`.

**The programmer over cue effects** (`UStageProgrammerSubsystem`): `FStageProgrammerEffects {Effects, Stops}` is the undo state (`FStageProgrammerEffectsCommand`, `IStageItemEditor::SetProgrammerEffects`) and is saved; `AddEffectStop`, `TakeEffect`, `GetEffectState` / `SetEffectState`.
- `RequestStopEffect` stops any running effect: the programmer's leaves, a cue's or playback's gets a stop (a store records it; Clear brings it back). Stop All does both for everything.
- New `RequestEditEffect`: takes a cue's or playback's effect into the programmer under its id (same clock, no jump), one undo step; a Track store writes it back.

**Scene format v10** (`Private/Scene/StageSceneStore.*`, `Public/Scene/StageSceneTypes.h`, `Private/Scene/StageSceneComponent.cpp`): each cue (main list and playbacks) may hold `"effects"` (as `"programmerEffects"`) and `"effectStops"`; root `"programmerEffectStops"`. Bad entries are dropped and counted (cue ones in `InvalidCueEntries`, programmer ones in `InvalidShowEntries`). v1–v9 load with no cue effects; v10 files are refused by builds before #64.

**UI:**
- Effects panel: the running list shows every source's effects with their source (PRG, CUE, PB n), EDIT for a cue's or playback's effect and STOP for any; a cue's effect shows read-only in the editor until EDIT; refreshes on cue start, cue list and desk changes too.
- Cues panel: an FX column (effects started or changed, stops as "-n") with the names in its tooltip.
- Playback panel: a strip whose playback holds effects shows "FX" after its cue position.
- Dev command: `StageCraft.Show.Effect` numbers the running effects of every source, adds `Edit <n>`, lists sources, weights, effects fading out and programmer stops; `StageCraft.Show.Cue List` shows each cue's effects and stops.
- Bench: `StageCraft.Bench.Fixtures ... Effects=3` (and `RunFixtureBench.ps1 -Effects 3`): cue A holds a circle and a dimmer sine, cue B a bigger circle, the sine and a rainbow, so every crossfade morphs, fades in and fades out effects. Effects=1|2 now start from an empty programmer (the user's effects come back afterwards).

**Files changed**
- **New:** `Private/Show/Tests/StageCueEffectTests.cpp` (Show.EffectTracking, Show.CueEffects, Scene.CueEffectData).
- **Modified:**
  - Show: `Public/Show/ShowControlTypes.h`, `Private/Show/ShowControlTypes.cpp` (cue effects, `SanitizeCueEffects`), `Public/Show/StageEffectTypes.h`, `Private/Show/StageEffectTypes.cpp` (`FStageProgrammerEffects`, `AreEntriesEqual`, `SanitizeStops`), `Public/Show/StageCueTracking.h`, `Private/Show/StageCueTracking.cpp`, `Public/Show/StageEffectRunner.h`, `Private/Show/StageEffectRunner.cpp`, `Public/Show/StageShowMix.h`, `Private/Show/StageShowMix.cpp` (`ComposeEffect`), `Private/Show/StageShowConsoleCommands.cpp`.
  - Subsystems: `Public/Subsystems/ShowControlSubsystem.h`, `Private/Subsystems/ShowControlSubsystem.cpp`, `Public/Subsystems/StageProgrammerSubsystem.h`, `Private/Subsystems/StageProgrammerSubsystem.cpp`.
  - History: `Public/History/StageEditCommand.h`, `Public/History/StageShowCommands.h`, `Private/History/StageShowCommands.cpp`, `Private/History/StageEditHistoryComponent.cpp`.
  - Controller: `Public/Player/ModularPlayerController.h`, `Private/Player/ModularPlayerController.cpp`, `Private/Player/ModularPlayerControllerProgrammer.cpp`.
  - Scenes: `Public/Scene/StageSceneTypes.h`, `Private/Scene/StageSceneStore.h`, `Private/Scene/StageSceneStore.cpp`, `Private/Scene/StageSceneComponent.cpp`.
  - UI: `Public/UI/StageEffectsPanel.h`, `Private/UI/StageEffectsPanel.cpp`, `Private/UI/StageCuePanel.cpp`, `Private/UI/StagePlaybackPanel.cpp`.
  - Bench: `Private/Diagnostics/StageFixtureBenchmark.cpp`, `Scripts/Bench/RunFixtureBench.ps1`.
  - Tests: `Private/Show/Tests/StageEffectTests.cpp` (the undo state type).
  - Records: `Docs/ADR/0027-effects-engine.md` (§5), `Docs/CURRENT_SYSTEMS.md`, `tasks/todo.md`, `STATE.md` (also the #63 commit hash `065d0cf`).
- No content assets, maps or input actions changed.


**Verification**
- **Builds:** Editor, Game and Shipping, 0 warnings and 0 errors (Game rebuilt after the last bench edit; Shipping after the last panel edit).
- **Shipping UTF-16 scan** (both byte alignments): absent `StageCraft.Show.Effect`, `StageCraft.Show.Cue`, `StageCraft.Bench.Fixtures`, the new console texts "fading out, weight" and "effect stops"; present "STOP ALL", "This playback holds effects", the EDIT tooltip and the `programmerEffectStops` field name.
- **Automation:** `RunAutomation.ps1 -ExpectedCount 182`: 182/182 with an offscreen renderer and 182/182 with `-NullRHI` (run again after the last edits); user data unchanged.
  - `StageCraft.Show.EffectTracking` (pure): tracking (start, track, stop, replace moves last, block, out of range); Track merge (effect drops its stop, stop drops its effect); Cue Only (old version back, new effect stopped, stopped effect restarted, next cue naming the id, blocked or no next cue); `SanitizeCueEffects` / `SanitizeStops` drop counts, MaxEffects, `SanitizeCueList`; feature timing (feature time, delay, Out time for intensity going down); `ComposeEffect` (absolute, relative, weight 1, mixed); the runner without fixtures (change flag, scale-only is no change, fade out and release, fade in, an unrelated change keeps the fade, stop and back from its weight, immediate change, delay, S-curve, morph keeps weight and blends the rate, zero time).
  - `StageCraft.Show.CueEffects` (world, 3 movers): a Track store of an effect only, then the cue runs it on the same clock without a jump; Go to 1 fades in (half the circle at half time); Go to 2 morphs (size 30 half way, weight never dips); Go to 3 fades the circle out and a chase in, then the pan back to static; Go to a blocked cue stops an intensity effect over the Out time, then no tick; Back fades with cue 3's time; restore is immediate; a programmer stop holds a cue effect off and Clear brings it back; TakeEffect keeps the clock, edit live, Track store writes it back; a stored stop; Cue Only stops a new effect at the next cue; Look store records the running effect; Block writes the inherited effects and no stops; a playback with an effect only: runs while up, intensity scaled by the fader (0.5, then 1), release and flash; External holds still.
  - `StageCraft.Scene.CueEffectData`: v10 round trip of cue effects, cue stops, a playback cue's effect and stop and the programmer's stops; a hostile file (bad ids, non-objects, a stop of the cue's own effect, a duplicate, unreadable stops) keeps the good entries and counts 6 cue and 2 programmer drops; a v9 file reads no cue effects or stops.
- **Scripted run R64a** (Concert starter stage, Lighting Desk layout, user data restored and hash-checked):
  1. A Circle on fixtures 1–5 stored with `Cue Track 11`: the programmer cleared, the circle then ran from the cue list on the same clock.
  2. A Dimmer Chase stored in cue 12; `Effect Stop 1` stopped the cue's circle with a programmer stop; `Cue Track 13` recorded it (cue 13: 0 effects, 1 stop).
  3. `GoTo 11` from 13: after 1 s the circle fading in at weight 0.50 and the chase fading out at 0.50. `Go` to 12: after 1 s the chase at 0.33 of its 3 s, the circle carrying on at 1.00, the chase's clock carrying on from before (4.5 cycles later). `Go` to 13: the circle fading out (0.51 after 0.5 s of 1 s), then gone and fixture 1 back to pan 0, tilt 0.
  4. `Effect Edit 1` took the chase into the programmer (same clock), `Set 1 bpm=120`, `Cue Track 12` wrote it back (cue 12 now 2 Hz).
  5. A Rainbow on fixtures 6 and 7 recorded on playback 1, level 0.5: it ran from "playback 1".
  6. Saved R64Fx, Empty Stage, reloaded: cues 11–13 with 1, 1 and 1-stop effects; `GoTo 12` ran the circle and the 2 Hz chase from the cue list.
  - **R64b** after one panel fix (in R64a the running list's longer lines ran under the EDIT button; the list is now 220 px wide with shorter lines clipped to their button): the Effects panel's running list (Circle · CUE, Dimmer Chase · CUE, Rainbow · PB 1, each with EDIT and STOP; the cue effect shown read-only with the EDIT hint), the Cues panel's FX column (1, 1, -1) and playback 1 showing "1/1 FX" at 50 %.
  - Artefacts: `%TEMP%\StageCraftRuns\R64a`, `R64b`.
- **Benchmark** (`RunFixtureBench.ps1 -Counts 200,500`, show tick mean; A/B against `065d0cf` in a worktree, alternating, the same afternoon):

  | Case | 200 fixtures | 500 fixtures |
  |---|---|---|
  | `065d0cf`, no effects, crossfade (two runs) | 3.78, 4.29 ms | 9.01, 9.09 ms |
  | F4b, no effects, crossfade (two runs) | 4.24, 3.69 ms | 8.32, 8.87 ms |
  | `065d0cf`, `-Effects 1`, idle / crossfade | 3.92 / 4.57 ms | 10.5 / 11.9 ms |
  | F4b, `-Effects 1`, idle / crossfade | 3.77 / 4.11 ms | 8.70 / 11.0 ms |
  | F4b, `-Effects 3` (effects in the cues: fade, morph, start and stop on every Go), idle / crossfade | 3.57 / 4.69 ms | 8.94 / 11.2 ms |

  - **No regression:** without effects F4b averages 3.97 / 8.60 ms against 4.03 / 9.05 ms; with programmer effects it is at or below F4a.
  - **Cue effects cost what programmer effects cost:** fading, morphing and starting or stopping effects on every Go adds nothing measurable over the same effects running steadily.
  - Frame, game-thread and GPU totals differ between the trees even idle (no show tick): the worktree has no saved workspace layout or user settings (fewer panels, other scalability), so only the show tick compares like with like.
  - **A bench bug found and fixed on the way:** the first `-Effects 3` run showed 36 ms frames at 500 fixtures with a 9 ms show tick; its CSV had 226 cues started in 225 frames. The bench stored its two looks while the effects ran, and a Look store records the static look under an effect, while the bench writes its looks straight onto the fixtures; so both cues held the same look, every Go finished at once and the bench went again every frame (a full re-mix each time, outside the show tick). The bench now stores the looks first and adds the effects with Track stores (5 Gos in the phase, as for the other modes).
  - CSVs: `Saved/StageCraft/Benchmarks/FixtureBench_20261104-1735*` to `-174722.csv`; the baseline's CSVs were in the removed worktree.
- **Not verified by hand:** the Effects panel's EDIT and STOP on a cue effect by mouse with Ctrl+Z, the FX column tooltip, the playback FX marker, STOP ALL over cue effects.


**Commit:** `f406ee5`.

**Known issues / follow-ups**
- **A changed fixture list snaps** during a morph (the new list at once); only the settings blend.
- **A playback's effects follow its fader at once** (its cues have no fade time, as its values); its intensity effects replace the intensity below them (latest wins), not HTP (ADR 0027 §5).
- **The main list's running effects change on Go, Back, Go To and restore only**, as its values do: editing the cue list (or undoing a store) does not restart them until the next cue.
- **A programmer stop or Clear is immediate**, not timed.
- **No effect pool and no speed master** (ADR 0027 §8).
- **Manual checks for Gevor:** the Effects panel's EDIT and STOP on a cue effect, the FX column and tooltip, the playback FX marker, by mouse with Ctrl+Z.
- **Next:** F5 DMX output (#65, #66).

## #65 — Phase F5a: Art-Net / sACN DMX output, rendered only on change (2026-11-04)

**What & why**
Gevor's F5 brief: DMX output so StageCraft can drive real fixtures, nodes and visualisers, then patch options and a fixture schedule import (#66).
- **Before.** The show had a send path (`TickDMXOutput` → `RenderDMXUniverse` → `UStageDMXBridge::SendUniverse`), but the only bridge (`UStageNetworkDMXBridge`) was input-only (`bOutputEnabled = false`). Had output been switched on, the show would have ticked and rendered every patched universe 44 times a second, even at rest.
- **Design (ADR 0019 §9):** one transport owner, network I/O on a thread of its own, and renders only for universes whose look changed.

**Transport** (`UStageDMXNetworkSubsystem`, `UStageNetworkDMXBridge`):
- The subsystem is now the world's Art-Net / sACN transport in both directions. Input and output are independent states, each with its own error, delegate (`OnInputChanged`, `OnOutputChanged`) and remembered on/off.
- One bridge holds the existing receiver and the new sender (`OpenInput` / `CloseInput`, `OpenOutput` / `CloseOutput`). It is installed in the show while either direction is on (`EnsureBridge`) and removed when both are off (`ReleaseBridgeIfIdle`). A bridge the show replaced is let go (`ForgetStoppedBridge`).
- `bOutputEnabled` follows the sender, so a bridge that only listens never makes the show render.
- **Under External** (DMX IN drives the show) the sent universes are set to none, which terminates the sACN streams: StageCraft never becomes a second source next to the console it follows. Back to Internal, every universe is sent again (`HandleControlSourceChanged`).

**`FStageDMXNetworkSender`** (new, `Private/Show/StageDMXNetworkSender.*`):
- **Threading.** One UDP socket and one `FRunnable` thread. `Post` puts a universe's frame into a mailbox that keeps only the newest frame per universe (lock + `FEvent`); it never waits on the network.
- **Packets.** The thread sends ArtDmx (sequence 1..255) and E1.31 data (its own sequence, the settings' CID, priority and source name).
- **Keep-alive.** An unchanged universe is sent again every second.
- **Universes.** `SetUniverses` takes the patched universes minus the muted ones. A universe that leaves gets three `Stream_Terminated` packets.
- **Close** terminates everything, joins the thread, then closes the socket. It is idempotent and also runs from the destructor.
- **Socket:** bound to the chosen card, broadcast on, multicast TTL 16 with the card as interface, multicast loopback on (a visualiser on this PC sees it).
- **Counters** are atomic.

**Settings** (`FStageDMXOutputSettings` in `UStageCraftUserSettings`, `StageDMXOutput::Sanitize`):
- `ArtNet` Off / Broadcast (255.255.255.255 from the card) / Unicast, default Broadcast.
- `SACN` Off / Multicast / Unicast, default Multicast.
- `UnicastAddress` (emptied unless it is IPv4), `SACNPriority` 0–200, `RateHz` 1–44, `MutedUniverses` (sorted, unique).
- `Cid`: made once per PC and kept by a reset to defaults.
- The network card and universe numbering stay the input's (`FStageDMXInputSettings`), so each is set in one place; changing them restarts a running output.
- A protocol set to unicast without an address is refused when the output opens.
- **Own-source guard.** The receiver ignores sACN carrying this PC's CID (`FStageDMXReceiverConfig::IgnoredSource`).

**Show control** (`UShowControlSubsystem`):
- Every fixture output write by the mix (fade frame, fade end, `ResolveFixture`) goes through `WriteFixtureOutput`. When anything changed and output is active, it marks the fixture's universe dirty.
- `RefreshDMXOutput` and a patch revision change mark every patched universe.
- `TickDMXOutput` renders only dirty universes, at most `DMXOutputRateHz` (from the settings), into one reused buffer (`RenderDMXUniverseInto`). An idle show sends its next change at once.
- `IsTickable` includes output only while a universe waits (`HasPendingDMXOutput`): **an idle show with output on does not tick.**

**Requests and UI:**
- `AModularPlayerController::RequestSetDMXOutput(bool)` and `RequestApplyDMXOutputSettings(Settings)`; refusals go to `OnRequestRejected`. Output is not undoable (a live device action, as DMX IN).
- **Patch panel DMX OUT line:**
  - ON/OFF;
  - ART-NET (OFF / BCAST / UNICAST) and sACN (OFF / MCAST / UNICAST), each cycled by a click;
  - unicast IPv4 field, PRI and HZ fields;
  - one chip per patched universe, lit while it is sent, a click mutes or unmutes it;
  - a status line on its own row: "Sending 1 universe · Art-Net to …, sACN to … · 189 Art-Net, 189 sACN packets", "Paused while DMX IN drives the show", or the error.
- The fields read the saved settings back. The line updates on `OnOutputChanged`: at once on a state change, and twice a second while sending (one looping timer that exists only while sending).

**Dev console and bench** (compiled out of Shipping):
- `StageCraft.DMX.Output On|Off|Status`.
- `StageCraft.DMX.OutputSettings [ArtNet=…] [SACN=…] [To=<IPv4>] [Priority=n] [Rate=n] [Mute|Unmute=<u>]`.
- `StageCraft.Bench.Fixtures … DMXOut=1`: a bench-owned bridge sending unicast to 127.0.0.1 on unused ports 26454 / 25568, refused while the user has DMX IN or OUT on. Script: `RunFixtureBench.ps1 -DMXOut`.

**Files changed**
- **New:**
  - `Private/Show/StageDMXNetworkSender.h/.cpp`
  - `Private/Show/Tests/StageDMXOutputTests.cpp` (DMX.OutputSettings, DMX.Output, DMX.OutputShow)
- **Modified:**
  - `Public/Show/StageDMXNetworkTypes.h`, `Private/Show/StageDMXNetworkTypes.cpp` (output settings, modes, state, status, `StageDMXOutput::Sanitize` / `IsMuted`)
  - `Private/Show/StageNetworkDMXBridge.h/.cpp`
  - `Private/Show/StageDMXNetworkReceiver.h/.cpp` (IgnoredSource)
  - `Public/Subsystems/StageDMXNetworkSubsystem.h`, `Private/Subsystems/StageDMXNetworkSubsystem.cpp`
  - `Public/Subsystems/ShowControlSubsystem.h`, `Private/Subsystems/ShowControlSubsystem.cpp`
  - `Public/Settings/StageCraftUserSettings.h`, `Private/Settings/StageCraftUserSettings.cpp`
  - `Public/Player/ModularPlayerController.h`, `Private/Player/ModularPlayerController.cpp`
  - `Public/UI/StagePatchPanel.h`, `Private/UI/StagePatchPanel.cpp`
  - `Private/Show/StageDMXConsoleCommands.cpp`
  - `Private/Diagnostics/StageFixtureBenchmark.cpp`, `Scripts/Bench/RunFixtureBench.ps1`
  - `Docs/ADR/0019-dmx-patch-and-network-input.md` (§9 amendment, status line)
  - `Docs/CURRENT_SYSTEMS.md`, `tasks/todo.md`

**Verification**
- **Builds:** Editor, Game Development and Game Shipping, 0 warnings and 0 errors (rebuilt after the last F5a and F5b edits).
- **Shipping UTF-16 scan** (both alignments): `StageCraft.DMX.Output`, `StageCraft.DMX.OutputSettings`, `DMXOut 0|1`, the bench's "DMX output sent" and "DMXOut=1 needs …" texts are absent. "DMX OUT: ON", "Paused while DMX IN drives the show" and "Sending {0}" are present.
- **Automation:** 185/185 with and without a renderer after F5a; 189/189 after F5b (#66).
  - **User data:** the first run with the new code added a `DMXOutput=` line to `Saved/Config/WindowsEditor/GameUserSettings.ini` (the defaults and this PC's CID). That is a one-time addition of a new setting; the next run left the file unchanged, with the same CID.
  - **`DMX.OutputSettings`:** clamping, bad and trimmed addresses, unknown modes, muted universes sorted and unique; the user settings always hold a CID, a change cannot replace it, and a reset keeps it.
  - **`DMX.Output`** (loopback sockets on 16454 / 15568):
    - refusals (no protocol, unicast without an address) leave nothing open;
    - universe 1 arrives as Art-Net 0 and sACN 1, with the newest of two posted frames and all 512 slots;
    - kept alive at least three times in 0.7 s at a 0.2 s interval, with Art-Net sequence 1..255 stepping and never 0;
    - an unset universe is never sent; our CID and priority 150 on the wire;
    - dropping universe 2 sends exactly three terminations, then silence; Close terminates universe 1 three times;
    - no send errors; status counters; the receiver drops our own CID but counts it.
  - **`DMX.OutputShow`** (world, two fixtures, a real bridge to a loopback listener):
    - no transport: never tickable;
    - a refresh ticks once and sends exactly the rendered universe, then the show is idle again with nothing more on the wire;
    - a Grand Master change sends one frame with the dimmed look;
    - under External nothing is ticked or sent;
    - removing the bridge closes it.
- **Scripted run R65a** (Concert, Patch panel, unicast to 127.0.0.1, user data restored and hash-checked):
  - one Art-Net and one sACN packet per second at rest;
  - muting universe 1: Art-Net stays at 8 and sACN goes 8 → 11 (the three terminations);
  - DMX IN on: "Paused while DMX IN drives the show", nothing sent;
  - Output Off: closed cleanly.
  - The cue Go step was refused (that template had no Go target there), so it is repeated in R65b.
- **R65b:**
  - at rest 1 → 3 packets in 2 s; a Grand Master change one extra frame;
  - a Circle on fixtures 1–5: 88 frames in 2 s (44 Hz exactly);
  - after Clear, back to one a second;
  - Off: 210 Art-Net, 213 sACN (three terminations).
  - The status line was moved onto its own row after R65a showed it cut short in a narrow window; R65b shows it in full. Artefacts: `%TEMP%\StageCraftRuns\R65a`, `R65b`.
- **Benchmark** (`RunFixtureBench.ps1 -Counts 200,500`, show tick mean in ms). A/B against `f406ee5` in a worktree, alternating, the same evening:

  | Case | 200 fixtures | 500 fixtures |
  |---|---|---|
  | Baseline, no effects, crossfade | 4.38, 4.36, 4.18 | 10.13, 9.55, 9.62 |
  | Current, no effects, crossfade | 4.18, 4.08, 3.59 | 9.99, 9.38, 9.68 |
  | Baseline, Effects=1 (idle / crossfade) | 4.75 / 5.24 | 11.42 / 12.87 |
  | Current, Effects=1 (idle / crossfade) | 4.01 / 5.12 | 11.15 / 13.44 |
  | Current, Effects=1 + DMXOut=1 (F5a; after F5b) | 4.33 / 4.74; 4.50 / 4.79 | 11.25 / 12.91; 11.34 / 13.33 |
  | Current, no effects + DMXOut=1 (idle / crossfade) | 0.14 (1 tick) / 4.39 | 0.27 (1 tick) / 10.15 |

  - **No regression in the show tick.**
  - **Output costs nothing measurable** (it renders only changed universes; the sending is on its own thread). Idle with output on, the show ticked once in 10 s and the sender kept the stream alive. With effects, 13 998 Art-Net and 13 998 sACN packets were sent without an error.
  - **Idle game-thread time.** The first A/B showed the current tree 0.7–1.2 ms higher at idle with no show ticks. The cause was the baseline worktree starting with the default workspace, while this tree opens Gevor's saved session of about 20 panels. With the same session copied in, 500-fixture idle game time was equal (8.52 / 8.54 ms). The remaining ~0.5 ms at 200 does not scale with fixtures.
  - CuesStarted was 5 in every crossfade run (real fades).
- **Not verified:** a real node, console or visualiser on the LAN receiving our broadcast, multicast or unicast; the panel by mouse (mode cycling, IP field, PRI / HZ, mute chips).

**Commit:** `1594875` (F5a and F5b in one commit, as proposed).

**Known issues / follow-ups**
- **No ArtPoll discovery of output targets.** Use broadcast or a typed unicast address. One unicast address serves both protocols.
- **No ArtSync, no per-universe destinations, no passthrough or merge** of DMX IN to OUT, no RDM (ADR 0019 §9.8).
- **Manual checks for Gevor:** a visualiser (sACNView, Capture, onPC) or a node receiving each mode; the DMX OUT line by mouse.

## #66 — Phase F5b: pan / tilt patch options on the wire and the fixture schedule import (2026-11-04)

**What & why**
The rest of F5 (ADR 0019 §10).
- **Patch options.** A moving head hung the other way round or sideways needs its pan or tilt inverted, or the two swapped, for what it receives. Consoles do this in the patch, not in the cues.
- **Schedule import.** A rig planned in a spreadsheet had to be re-typed address by address.

**Patch options** (`FStageDMXPatchOptions` in `ShowControlTypes.h`, SaveGame fields):
- Stored on `ALightingFixtureActor` (`PatchOptions`, SaveGame). `SetPatchOptions` notifies the patch subsystem (a new revision: output re-sends the universe, input re-reads it) and the three parameters.
- **Wire only.** `WriteDMX` and `ReadDMX` map each channel through `StageDMXPatchOptions::GetChannelSource`: a swap moves tilt onto the pan channel (in tilt's range) and back, and an invert belongs to the console axis on whichever channel it travels. It combines with the CMY inversion by XOR. The attributes, the beam, the programmer and the cues do not change.
- **Unusable options.** `GetEffective` drops what the mode cannot use (a swap needs pan and tilt). The stored options are kept as given, and with none set the codec skips the channel scan.
- **Undo for free.** `StageCraft.Param.Patch.InvertPan`, `InvertTilt` and `SwapPanTilt` are Bool patch parameters (`IsPatchParameter`), so edits are undoable through `RequestParameterChange`. The inspector offers them for the axes the DMX mode has.
- **No scene format change.** Item state is tagged SaveGame serialization, so older states load with no options.

**Fixture schedule** (`Public/Show/StagePatchSchedule.h`, `Private/Show/StagePatchSchedule.cpp`, pure):
- **`Parse`** reads the patch sheet's own CSV back, or a hand-made one:
  - BOM, CRLF or LF, comma or semicolon (from the header row), quoted fields;
  - the formula guard stripped; the header is the first row with a "Fixture ID" field; columns by name in any order;
  - optional Invert Pan / Invert Tilt / Swap Pan/Tilt columns;
  - both Universe and Address empty = unpatch;
  - limits: 1 M characters, 10 000 rows, 512 per field. Every bad field is reported on its line and the rest still read.
- **`Plan`** rejects, with the line:
  - an unknown fixture;
  - a fixture on more than one row (every such row);
  - a patch for a fixture without DMX;
  - an address the footprint does not fit at.
  
  Rows equal to the stage are counted as unchanged. Conflicts are reported over the whole stage as it would be.
- **`UStageDMXPatchSubsystem::PlanSchedule`** turns the plan into assignments (with options only where the file set them), a summary and the first four reasons. `LoadScheduleFile` refuses a missing or oversized file before reading it.
- **The patch sheet CSV** gains the three option columns (Yes or empty), so an export imports back exactly.

**Requests:**
- `RequestPlanPatchSchedule(Path, OutImport)` reads and plans; it changes nothing.
- `RequestPatchFixtures` now applies a list of patch parameters per fixture: universe and address when the patch moves, plus each option that differs when the assignment carries options. They are all evaluated by the GameMode first, applied and recorded as **one undo step**, and fully put back on a failure.

**UI** (Patch panel):
- each row has -P / -T / SW toggles (lit when on, greyed out without the axis), each one `RequestParameterChange`;
- IMPORT CSV opens the picker in the Reports folder; the plan shows in a bar above the table (summary, reasons, APPLY / CANCEL) and nothing changes before APPLY;
- the name cell now clips (ellipsis) in a narrow panel.

**Also fixed:** an edit transaction's undo step was named by its command count ("Edit 6 items" for a re-patch of 3 fixtures, as AUTO PATCH already showed). It now counts distinct items (`UStageEditHistoryComponent::EndTransaction`).

**Dev console:** `StageCraft.DMX.ImportSchedule <path> [Apply]` and `StageCraft.DMX.Render <universe> <first> [count]` (logs the rendered output channels).

**Files changed**
- **New:**
  - `Public/Show/StagePatchSchedule.h`, `Private/Show/StagePatchSchedule.cpp`
  - `Private/Show/Tests/StageDMXPatchOptionsTests.cpp` (DMX.PatchOptionsMap, DMX.PatchOptions, DMX.ScheduleParse, DMX.ScheduleImport)
- **Modified:**
  - `Public/Show/ShowControlTypes.h`
  - `Public/Show/StageDMXPatching.h`, `Private/Show/StageDMXPatching.cpp`
  - `Public/Actors/LightingFixtureActor.h`, `Private/Actors/LightingFixtureActor.cpp`
  - `Public/Data/StageParameterTypes.h`, `Private/Data/StageParameterTypes.cpp`
  - `Public/Subsystems/StageDMXPatchSubsystem.h`, `Private/Subsystems/StageDMXPatchSubsystem.cpp`
  - `Public/Player/ModularPlayerController.h`, `Private/Player/ModularPlayerController.cpp`
  - `Public/Report/StagePatchSheet.h`, `Private/Report/StagePatchSheet.cpp`, `Private/Report/StagePatchSheetSubsystem.cpp`, `Private/Report/Tests/StagePatchSheetTests.cpp`
  - `Private/History/StageEditHistoryComponent.cpp`
  - `Public/UI/StagePatchPanel.h`, `Private/UI/StagePatchPanel.cpp`
  - `Private/Show/StageDMXConsoleCommands.cpp`
  - `Docs/ADR/0019-dmx-patch-and-network-input.md` (§10), `Docs/CURRENT_SYSTEMS.md`, `tasks/todo.md`

**Verification**
- **Builds:** Editor, Game Development and Game Shipping, 0 warnings and 0 errors after the last edits.
- **Shipping scan:** `StageCraft.DMX.ImportSchedule` and `StageCraft.DMX.Render` are absent. "IMPORT CSV", "Swap Pan/Tilt (DMX)", "Fixture schedule: {0}" and "is on more than one row" are present.
- **Automation:** `RunAutomation.ps1 -ExpectedCount 189`: 189/189 with an offscreen renderer and 189/189 with `-NullRHI`, after the last edits; user data unchanged.
  - **`DMX.PatchOptionsMap`** (pure): all 8 combinations: which attribute each channel carries and its invert; other attributes untouched; `GetEffective` per axis.
  - **`DMX.PatchOptions`** (world): for all 8 combinations with pan 120 and tilt −40:
    - the beam never moves; each 16-bit channel carries the expected value (inverted where due, ±1);
    - `ReadDMX(WriteDMX)` gives the look back within 0.05°; options off gives the plain bytes again.
    
    Also:
    - the three parameters are offered, are patch parameters, write and read back as Bool, refuse a Float and move the patch revision;
    - recorded and undone and redone through the history;
    - restored from a snapshot;
    - a pan-only fixture ignores swap and tilt invert on the wire, keeps them as given, and offers only Invert Pan.
  - **`DMX.ScheduleParse`** (pure):
    - the patch sheet's own CSV (with a formula-guarded label, options and an unpatched row) reads back exactly;
    - a hand-made semicolon sheet with a comma in its title, a BOM, quotes, reordered and extra columns and an apostrophe guard;
    - refused whole: no header, no Universe/Address, over 1 M characters;
    - 11 hostile rows (non-numeric, too large, past 512, negative, half a patch, a bad flag, an open quote, an over-long field, a control character, fixture ID 0, a fraction), each reported on its line while the good row reads;
    - a NUL in a row; 10 005 rows reads 10 000 with one reason.
  - **`DMX.ScheduleImport`:**
    - pure plan: unchanged, a change with an option, takes-no-DMX, footprint overflow, unknown, duplicate rows (both), the overlap reported;
    - over real fixtures: assignments carry the fixture, carry options only when the file set them, explain the unknown fixture, and change nothing;
    - an options-only row;
    - missing and readable files.
- **Scripted run R66a/b** (Concert, user data restored and hash-checked), fixture 1 at pan 90°, tilt −30°:
  - the wire went `170 170 99 142`; with Invert Pan `85 85 99 142`; with Swap too `99 142 85 85`;
  - the fixture's output stayed Pan 90 / Tilt −30;
  - two Undos gave `170 170 99 142` back.
  - **Schedule import** (`R66Schedule.csv`): "3 fixtures to re-patch · 1 unchanged · 4 rows skipped · 0 conflicts" (no fixture 77; fixture 5's 12 channels do not fit at 1.510; fixture 6 on two rows, twice).
    - Apply: "3 fixtures re-patched in one step"; fixture 1 at 2.001 sending `85 85` (Invert Pan from the file).
    - One Undo, now named "Edit 3 items" (R66a showed "Edit 6 items" before the label fix), put fixtures 1–3 back at 1.001 / 1.013 / 1.025.
- **R66c/d (panel):**
  - R66b showed that the 104 px option column squeezed the name column to nothing in a narrow floating panel, and the names drew under the UNIV / ADDR fields.
  - Now: 66 px column, -P / -T / SW labels with tooltips, and a clipped name cell. R66d shows the rows clean, with -T lit on fixture 2 and SW on fixture 3.
  - Artefacts: `%TEMP%\StageCraftRuns\R66a`–`R66d`.
- **Not verified:** IMPORT CSV through the file picker and the import bar's APPLY / CANCEL by mouse (the console path ran the same plan and request), a real hung fixture.

**Commit:** `1594875` (F5a and F5b in one commit, as proposed).

**Known issues / follow-ups**
- **A schedule only re-patches fixtures already on the stage.** It does not create them, and has no mode or type columns. No GDTF / MVR import.
- **`StageCraft.DMX.ImportSchedule`** splits arguments on spaces (a path with spaces needs the panel).
- **Manual checks for Gevor:** IMPORT CSV with the patch sheet CSV and a hand-made one (bar text, APPLY, CANCEL, Ctrl+Z); the -P / -T / SW toggles by mouse with Ctrl+Z; a real fixture or visualiser following INV / SWAP.
- **Next:** Phase F is complete (F1–F5). Open follow-ups are in todo.md: merge or passthrough, ArtPoll output discovery, effect pool and speed master, the manual checks.

## #67 — Roadmap: audio-reactive lighting, assistant v2, realistic stages and crowds; proposed ADRs 0029 and 0030 (2026-11-05)

**What & why**
Gevor asked (2026-11-05) for a structured, multi-phase roadmap for three features: realistic default stages with crowd and band, an audio-reactive engine (file, line and streamed audio; four-band analysis; beat and BPM; driving the F4 effects), and an in-console AI assistant with full context, diagnostics and live actions. This entry is planning only: **no code changed.**

**Housekeeping first.** #65 and #66 said "uncommitted"; both now carry `1594875` (F5a and F5b went in as one commit).

**Findings from the scan that shaped the plan**
- **Phase 26 was never built.** The offline knowledge base, multi-step prompts and memory are still open, and Gevor's 2026-10-30 decision (offline is the full default, Claude only extends it) stands. Assistant v2 absorbs Phase 26. Phase 26 pointed at "ADR 0022", which became the DCC bridge; ADR 0030 is its home now.
- **No audio analysis exists.** `AudioCaptureCore` has no loopback capture, so "YouTube" means listening to the PC's own output (WASAPI loopback, our own code). Downloading or extracting YouTube streams is ruled out: it breaks YouTube's terms.
- **No 120 FPS baseline is recorded.** The measured numbers are about 110–120 fps camera flight on the test map, 14.2 ms at 200 crossfading fixtures, and 27–33 ms at 500 (render-thread bound). The roadmap keeps the "no regression A/B" gate, adds a budget per feature, and asks Gevor for a reference scene if he wants a hard frame-rate target.
- **Characters.** There are no crowd or band assets. The OverCrowd plugin (third-party VAT crowds, beta, built for 5.7) is in `Plugins/` but disabled. UE 5.8 ships AnimToTexture, MassCrowd and MetaHuman.
  - Realistic people depend on an art-source decision; code alone can't supply them.
  - Verified in the engine tree: `ISubmixBufferListener`, `Audio::FSpectrumAnalyzer` / `IFFTAlgorithm`, `UMediaSoundComponent` spectral data and the plugins above.

**Recommended order:** G (audio, builds on F4 and is testable with synthetic signals), then H (assistant; needs the features it describes), then I (realism). I0's asset decision starts now because sourcing has the longest lead time.

**Files**
- `Docs/Roadmap/2026-11-05-realism-audio-assistant.md` (new): state scan, baselines, order, phases G0–G3, H0–H4, I0–I4 with exit criteria, decisions needed, risks.
- `Docs/ADR/0029-audio-analysis-and-audio-reactive-effects.md` (new, **proposed**): one source at a time (File via WmfMedia to a tapped submix, Input, SystemAudio loopback, TestSignal), a lock-free tap, an analyser thread, pure bands / spectral-flux onsets / autocorrelation tempo / PLL beat phase. `FStageEffectAudio` (Sync, Modulate, Trigger) on the existing effect clocks; scene format v11; a flash limiter (3 Hz default) on audio-driven intensity because DMX OUT reaches real lights; privacy; performance gates; tests.
- `Docs/ADR/0030-assistant-knowledge-live-context-and-tools.md` (new, **proposed**):
  - knowledge generated from the code (UI anchor tables, key map, parameters, panels, help articles), offline BM25 search in EN/HY;
  - live state through bounded read tools, never a dump;
  - pure diagnostics and a per-fixture "why" explainer;
  - action tools collected into a proposal card and applied as one undo step with rollback;
  - a bounded online loop with an append-only history (amends ADR 0016 §2.5) and a cached prefix;
  - the key moves into the credential vault.
- `tasks/todo.md`: Phase 35 with every step unchecked except the roadmap.
- `STATE.md`: #65/#66 commit hash, this entry.

**Verification:** documents only; no build or test needed. API facts in ADR 0030 §8 were checked against the current Claude API reference. Engine facts in ADR 0029 §2 were checked in `C:/Program Files/Epic Games/UE_5.8`. Facts not yet proven are marked (G0) / (H0) for the spikes.

**Commit:** `54725ff`.

**Known issues / follow-ups**
- **Waiting on Gevor:**
  - approve ADR 0029 and ADR 0030;
  - the default online model and the key policy for a shipped product;
  - the crowd and band art source (MetaHuman, Fab or commissioned);
  - the performance target.
- ADR 0031 (stage realism and crowds) is written in I0, after the asset decision.

## #68 — Phase G0: audio spike (file formats, submix tap, WASAPI loopback, onset detectors) measured in Development and Shipping (2026-11-05)

**What & why**
Gevor: "Start Phase G0". G0 is the spike that ADR 0029 required before any audio code is designed for real.
- **A6:** do music files play in a Shipping build?
- **Latency:** how long each source takes.
- **Detectors:** is AudioSynesthesia's onset detector better than ours?

**Housekeeping first.** #67 said "uncommitted"; it carries `54725ff` (Gevor committed the roadmap).

**How it was measured (throwaway, not merged)**
- **Worktree and branch:** `E:/SC_G0`, branch `spike/g0-audio` from `54725ff`, uncommitted.
- **Code:** one file, `Source/ModularSceneBuilder/Private/Spike/StageAudioSpikeG0.cpp`, plus spike-only deps in its `Build.cs` (MediaAssets, SignalProcessing, AudioSynesthesiaCore; ole32, mfplat, mfreadwrite, mfuuid) and AudioSynesthesia and ElectraPlayer enabled in its `.uproject`.
- **Command line:** `-StageAudioSpikeFile=<file> [-StageAudioSpikeMF] [-StageAudioSpikeLoopback] [-StageAudioSpikeInput] -StageAudioSpikeOut=<json>`. One run:
  - plays the file (UMediaPlayer, or our Media Foundation decoder into a procedural sound);
  - taps the main submix;
  - optionally captures WASAPI loopback and the microphone at the same time;
  - finds every kick sample-accurately and runs four onset detectors on the tap;
  - writes JSON and quits.
- **Test signal:** a 20 s, 120 BPM synthetic track (a kick every 0.5 s from exactly 1.000 s, off-beat hi-hat, chord pad, noise), made with ffmpeg as WAV, FLAC, MP3 and M4A. The 0.94 ms bias of the kick truth was measured on the files with ffmpeg.
- **Runs:**
  - 9 Development `-game` runs from the worktree editor binaries;
  - 6 runs of a packaged **Shipping** build (`BuildCookRun`, `E:/SC_G0_Package`, BUILD SUCCESSFUL).
  - Script: scratchpad `RunG0.ps1`. Results: `%TEMP%\StageCraftRuns\G0\*.json`.

**Results** (details in ADR 0029 §2.1)
- **Formats.**
  - WmfMedia refuses `.mp3` and `.flac` by extension and plays only WAV of the four. Electra plays MP3 (not bare FLAC).
  - **Our Media Foundation source reader decodes WAV, FLAC, MP3 and M4A in Development and Shipping.** A 20 s file takes 54–133 ms.
  - Encoder priming is not trimmed (MP3 +39 ms, AAC +21 ms).
- **Start latency** (play to first sample rendered):

  | Path | Development | Shipping |
  |---|---|---|
  | Procedural wave fed from our decoder | 3 ms | 16 ms |
  | WMF | 20–36 ms after IsPlaying | |
  | Electra | 71–86 ms | |

- **Player clocks are unusable for beats.** WMF `GetTime` steps by 100 ms; Electra's runs about 250 ms ahead of the mixer.
- **Unfocused mute.** BaseEngine `[Audio] UnfocusedVolumeMultiplier=0.0` silences the app **and every submix tap** when the window loses focus.
  - Every run after a game window opened in the background read silence (peak 0) until `FApp::SetUnfocusedVolumeMultiplier(1)` / `-ini:Engine:[Audio]:UnfocusedVolumeMultiplier=1.0`.
  - The product needs this as a setting, because the operator will click into a browser.
- **Submix tap:** 1024 frames every 21.3 ms (19–31 ms).
- **Engine output delay:** the mixer renders a kick about 78 ms (73–83) before Windows' loopback stream carries it.
- **WASAPI loopback works**, in Development and Shipping: 48 kHz float, 10 ms packets, steady QPC stamps; 37–38 of 38 kicks detected from it.
- **Microphone:** 10 ms callbacks of 480 frames. The acoustic path could not be measured (the mic heard nothing of the speakers; peak 0.0007).
- **Detectors** (38 kicks + 38 hats per run):

  | Detector | Kicks | False positives | Mean latency | CPU per audio second |
  |---|---|---|---|---|
  | Ours, flux 2048/512 | 38/38 | 0–2 | 27 ms (20–41) | 1.2 ms |
  | AudioSynesthesia defaults (4096/1024, lag 4) | 38/38 | 1–4 | 64 ms | 0.4 ms |
  | Ours, 1024/256 | misses some | 4–6 | 13–16 ms | |
  | Synesthesia 2048/512, lag 1 | misses some | 4–6 | 25 ms | |

**Decisions recorded in ADR 0029 (still proposed; amended §2 A6/A7, new §2.1, §3 File row, §3 rejected list, §11):**
- **File source:** our Media Foundation decoder on a worker, played through our procedural sound, with the same PCM analysed **before** it is played. Features can then be aligned to what the audience hears (held back by the ~78 ms output delay minus detection) instead of arriving late. The song position is exact, and no media plugin is needed.
- **Detector:** our flux 2048/512. No AudioSynesthesia dependency.
- **Unfocused mute:** becomes a user setting, default "keep playing".

**Files**
- `Docs/ADR/0029-audio-analysis-and-audio-reactive-effects.md`: status line, A6 result, A7, §2.1 measurements and decisions, File row, rejected list, §11 dependencies.
- `tasks/todo.md`: G0 ticked.
- `STATE.md`: #67 commit hash, this entry.
- Spike only, not in this repository's main tree: `E:/SC_G0` (branch `spike/g0-audio`, uncommitted) and `E:/SC_G0_Package`.

**Verification:**
- Spike builds: worktree editor and Shipping package, 0 warnings in project code. The only warnings are the third-party SkyCreator header's C4305, which appear in any fresh compile of that plugin.
- All numbers come from the JSON files above.
- The main tree has documentation changes only (no build needed).

**Commit:** `beb6573`.

**Known issues / follow-ups**
- **Not measured:** the speaker-to-microphone acoustic latency, and real music (G1's tempo tests will use real tracks).
  - Manual check for Gevor: play music with the mic near a speaker in G1's Audio panel.
- **Spike leftovers:** `E:/SC_G0` and the about 1 GB `E:/SC_G0_Package` stay until Gevor says to remove them (`git worktree remove --force E:/SC_G0`, delete the package folder, `git branch -D spike/g0-audio`).
- **G1 must:**
  - stream the decode (a 5-minute song is about 115 MB as float), not queue it whole;
  - resample 44.1 kHz files to the device rate;
  - expose the unfocused-volume setting;
  - add a per-source latency offset with the measured defaults.

## #69 — Phase G1: audio sources and analyser (file, input, system audio, test signal; bands, onsets, tempo, beat phase) (2026-11-05)

**What & why**
- Gevor approved ADR 0029 and said "Start Phase G1 implementation".
- G1 is the audio half of audio-reactive lighting (ADR 0029 §3 and §5; roadmap Phase G):
  - music comes in from one of four sources;
  - one analyser thread turns it into features (four bands, level, onsets, BPM, beat phase and count, confidence, signal);
  - the game thread reads them through a world subsystem.
- No effect uses them yet. That is G2: the Effects AUDIO row, scene v11, the Audio panel and `Bench.Fixtures Audio=1`.
- Plan: `C:\Users\gevor\.claude\plans\flickering-nibbling-newell.md`, approved by Gevor.

**Design** (details in ADR 0029 §13)

*Sources* (`EStageAudioSource`), exactly one live:
- **File:** our own Media Foundation decoder (`FStageAudioFileDecoder`).
  - It reads WAV, FLAC, MP3, AAC/M4A and WMA on its own thread into a ring kept 1.5 s ahead. That is streaming, not the whole song.
  - The audio is played by `UStageAudioFileSound`, a `USoundWaveProcedural` at the file's own rate; the mixer converts to the device rate.
  - The same samples are written to the analysis tap at the moment the mixer renders them.
  - Transport: play, pause, stop, seek, loop, with an exact position counted in samples.
- **Input:** an AudioCaptureCore device (chosen id or the default), with an input gain. Analysed only.
- **SystemAudio:** WASAPI loopback of the default playback device on its own thread, with QPC-stamped packets. This is the YouTube path from ADR 0029 §3.
- **TestSignal:** a deterministic kick + hat + pad at a set BPM, generated by the analyser itself.

*Plumbing*:
- **Tap:** every source writes into an `FStageAudioTap`, a lock-free, allocation-free SPSC mono ring with clock markers. Overrun drops the oldest samples, seqlock-validated and counted.
- **Analyser:** `FStageAudioAnalyzer` runs on its own thread. It resamples to 48 kHz, runs the pure `StageAudioAnalysis::FFeatureExtractor`, and holds each frame until the audience hears it. It publishes through a lock-free triple buffer.
- **Heard delay:**
  - File: `FileOutputDelayMs`, 78 ms from G0, plus the user's offset.
  - Capture sources: only the user's offset, because that audio has already been heard.
  - Readers carry the beat phase forward with `FStageAudioFeatures::PhaseAt(now)`.

*Maths* (pure, deterministic for any block size):
- **FFT:** 2048/512 under a Hann window, through the engine's `Audio::IFFTAlgorithm`, normalised to its declared scaling.
- **Bands:** sub-bass, bass, mid and high, plus level. Each is normalised by an adaptive ceiling (30 dB range, falling over 8 s) with a -70 dBFS floor, attack 10 ms and release 150 ms. Signal is lost after 2 s below -60 dBFS.
- **Onsets:** G0's flux detector, plus an absolute floor of 0.1. Without the floor, a steady tone's rounding noise fired onsets.
- **Tempo:** autocorrelation of a low-band-weighted onset envelope over 6 s, every 0.5 s, 60–200 BPM.
  - A mild prior around 120 BPM breaks near ties; then an octave rule prefers the shorter of two octave-related periods.
  - A least-squares fit of the onsets on the beat grid refines the period and the beat.
  - The low band uses plain magnitude: log compression made 90 BPM with off-beat hats read as 180.
- **Beat:** phase pulled 20 % per frame; the counter never runs backwards.

*Ownership and rules*:
- **`UStageAudioReactiveSubsystem`** (world, Game + PIE) owns the source, tap, analyser and file player.
  - Sources that open on a thread report back through the task graph, tagged with a generation number.
  - Each start gets a fresh tap.
  - A 2 Hz status timer exists only while a source is starting or running.
  - Stop order: source, then analyser, then tap.
  - The last non-File source is remembered and reopened with the next stage, like DMX.
- **Requests:** `AModularPlayerController::RequestSetAudioSource`, `RequestOpenAudioFile`, `RequestAudioTransport`, `RequestApplyAudioSettings`, `RequestSetPlayAudioInBackground`. All are judged by the new BlueprintNativeEvent `AStageCraftGameModeBase::EvaluateAudioRequest`, and refusals go to `OnRequestRejected`. None is undoable: they are live device actions, like DMX.
- **Per-PC settings:** `FStageAudioReactiveSettings` (sanitised) in `UStageCraftUserSettings`. Also `bPlayAudioInBackground`, default on, applied by `UStageAudioSubsystem` through `FApp::SetUnfocusedVolumeMultiplier`. It fixes G0's A7 (an unfocused app went silent), and the previous value is restored on Deinitialize for PIE.
- **Dev console:** `StageCraft.Music.Source|Status|Transport|Settings|Devices` (`#if !UE_BUILD_SHIPPING`), all through the controller.
  - Not `StageCraft.Audio.*`: the UI-sound commands already own `StageCraft.Audio.Status`.
  - ADR 0029 §5 is updated.
- **Privacy (ADR §6):** captured audio stays in memory, and logs carry state changes only.

**Files**
- New:
  - `Public/Audio/StageAudioReactiveTypes.h`, `Private/Audio/StageAudioReactiveTypes.cpp`
  - `Public/Audio/StageAudioAnalysis.h`, `Private/Audio/StageAudioAnalysis.cpp`
  - `Private/Audio/StageAudioTap.h/.cpp`, `StageAudioAnalyzer.h/.cpp`, `StageAudioFileDecoder.h/.cpp`, `StageAudioFileSound.h/.cpp`, `StageAudioCaptureSources.h/.cpp`, `StageAudioReactiveConsoleCommands.cpp`
  - `Public/Subsystems/StageAudioReactiveSubsystem.h`, `Private/Subsystems/StageAudioReactiveSubsystem.cpp`
  - Tests: `Private/Audio/Tests/StageAudioAnalysisTests.cpp`, `StageAudioAnalyzerTests.cpp`
- Changed:
  - `ModularSceneBuilder.Build.cs`: SignalProcessing, AudioExtensions; Win64 ole32, mfplat, mfreadwrite, mfuuid.
  - `Public/Settings/StageCraftUserSettings.h` / `.cpp`
  - `Public/Audio/StageAudioSubsystem.h` / `Private/Audio/StageAudioSubsystem.cpp`
  - `Public/Game/StageCraftGameModeBase.h` / `.cpp`
  - `Public/Player/ModularPlayerController.h` / `.cpp`
- Docs:
  - `Docs/ADR/0029-...md`: status approved, the §5 dev console line, new §13 implementation notes.
  - `tasks/todo.md`: G1 ticked.
  - `STATE.md`: #68 commit hash, this entry.
- No content assets, maps or Blueprints changed.

**Verification**

*Builds:* Editor, Game Development and Game Shipping, with 0 warnings.

*Shipping UTF-16/ASCII scan:*
- All five `StageCraft.Music.*` commands are absent.
- The product strings are present.
- The non-Windows message is compiled out.

*Tests:* 197/197, with and without a renderer (189 + 8 new), user data unchanged.

| Test | What it covers |
|---|---|
| `StageCraft.Audio.Bands` | Tones in their bands (≥ 6 dB clear); quiet and loud signals use the full range; release; the signal timeout; hostile settings sanitised; `PhaseAt` |
| `StageCraft.Audio.Onsets` | 38/38 kicks, 0 false positives, onset times -5.7 ms, decision latency 26 ms; a steady tone makes no onsets |
| `StageCraft.Audio.Tempo` | 90, 120, 128, 140 and 174 BPM, kick only and kick + hat: worst error 0.07 BPM after 6 s, confidence ≥ 0.72, beats 5–8 ms from the kicks; 120 → 128 re-locks; noise confidence 0.14; identical frames for any block size |
| `StageCraft.Audio.Resampler` | 44.1 → 48 kHz |
| `StageCraft.Audio.RingBuffer` | Order across the wrap; block clock; overrun drop accounting; storage never moves; a real producer thread at 10× real time with a forced 120 ms consumer stall (every gap is a counted drop, and read + dropped = written); the triple buffer never tears or goes back |
| `StageCraft.Audio.Analyzer` | Test signal → 128 BPM on the game thread; frames released on time; analyser 0.3 % of a core; double shutdown safe; stale features have no signal |
| `StageCraft.Audio.FileDecoder` | A WAV decodes bit-exact (rate, channels, duration); loop wraps to the start; seek on open; missing, empty, wrong-extension and garbage files refused with reasons |
| `StageCraft.Audio.Subsystem` | GameMode gate; File via OpenFile only; bad paths change nothing; TestSignal at 128 BPM; settings sanitised and LastSource kept; system audio settles; a WAV plays and seeks (2.0 → 2.11 s); Off; a test settings object, so the user's settings are untouched |

*Scripted Development `-game` run* (`RunGameScripted`, user data restored; the G0 120 BPM track):

| Source | BPM | Notes |
|---|---|---|
| TestSignal | 127.95 | |
| WAV | 120.02 | seek to 5 s plays on from there; 0 underruns |
| MP3 at 44.1 kHz | 119.96 | |
| FLAC | 120.03 | |
| SystemAudio | 119.97 | another process played the track through Windows |

- Confidence 1.00 on every source.
- Analyser 0.27–0.36 % of a core.
- 0 dropped samples.
- Background audio on. The scripted window was not focused and still analysed: the G0 trap is gone.

*Performance A/B:* `RunFixtureBench -Counts 200,500`, Effects 0. The A side is the `beb6573` worktree `E:/SC_Baseline` (workspace copied in, editor built); the B side is this tree.

| Fixtures | Phase | Metric | beb6573 (2 runs) | G1 with imports (2 runs) | G1 without imports |
|---|---|---|---|---|---|
| 200 | idle | frame mean | 10.17 / 10.28 | 10.87 / 10.75 | 10.28 |
| 200 | idle | game | 6.67 / 6.68 | 7.06 / 6.90 | 6.87 |
| 200 | crossfade | show tick | 3.90 / 4.25 | 4.22 / 4.26 | 3.42 |
| 500 | idle | frame mean | 14.89 | 14.73 | 14.60 |
| 500 | idle | game | 8.64 | 8.28 | 8.61 |
| 500 | crossfade | show tick | 10.06 | 9.59 | 9.55 |

All times in ms.

- **The extra 0.5 ms at 200 idle with imports is the user's imported models.** The main tree loads them and the worktree does not (memory note). With `Imports.json` moved aside for one run, then restored and hash-checked, G1 matches the baseline.
- **No regression.** That is what the code predicts: with the source Off, nothing ticks, no timer exists and no thread runs.

**Commit:** `a85475f` (Gevor committed G1).

**Known issues / follow-ups**
- **Packaged Shipping not run at runtime.** The dev commands that drive the sources are compiled out of Shipping, and the Audio panel arrives in G2. G0 already ran Media Foundation decoding and WASAPI loopback in Shipping; G2 repeats it through the panel.
- **The file's heard delay** (78 ms) is G0's mixer-to-loopback figure. The procedural sound is tapped in its render callback, which may run a mixer buffer earlier. Measure it in G2 with lights on the beat, then adjust the default.
- **Seek restarts the analysis:** the tempo window refills, so there is no BPM for about 3 s after a seek.
- **Onset times are about 6 ms early** (window centre against attack). Beats land 5–8 ms early on the test kicks, inside tolerance; the latency offset covers it.
- **First run after this change:** the engine re-saves `GameUserSettings.ini` on every run, so the two new settings (`AudioReactive`, `bPlayAudioInBackground`) appear there once, with defaults. RunAutomation reported that run as a user-data change. A run with only a pure test proved the engine writes it, and every run after is unchanged.
- **Manual for Gevor:**
  - real music on each source (a song file, YouTube in a browser with System audio, a line or mic input), with `StageCraft.Music.Status` in the console;
  - the mic acoustic latency.
- **Spike leftovers:** `E:/SC_G0`, `E:/SC_G0_Package` and branch `spike/g0-audio` are still waiting on Gevor. The bench worktree `E:/SC_Baseline` was removed after the A/B.

## #70 — Phase G2: audio in effects (Sync, Modulate, Trigger), Audio panel, scene format v11, Bench Audio=1 (2026-11-05)

**Housekeeping first.** #69 said "uncommitted"; it now carries `a85475f` (Gevor committed G1).

**What & why**
- Gevor's brief (2026-11-05) asked for Phase G2: audio-reactive modes on `FStageEffect`, an AUDIO row and quick presets in the Effects panel, an Audio panel, markers in the Cues and Playback panels, scene format v11 and `Bench.Fixtures Audio=1`.
- ADR 0029 (approved) governs where the brief differed:
  - scene v11 stores the effect audio settings only; source, device, gain and transport stay per PC or per session (§5);
  - the flash limiter, AUDIO master and KILL, the Listening indicator and latency-offset controls stay in G3.
- Plan: `C:\Users\gevor\.claude\plans\flickering-nibbling-newell.md`, approved by Gevor.

**Design** (details in ADR 0029 §14)

*Data and maths:*
- `FStageEffectAudio` on `FStageEffect` (`Public/Show/StageEffectTypes.h`):
  - `Mode` (Off, Sync, Modulate, Trigger) and `Band` (G1's `EStageAudioBand`);
  - `Amount` 0..1 and `Modulates` (Level, Size, Rate);
  - `BeatsPerCycle` (¼ ½ 1 2 4 8) and `Response` (Fast, Medium, Slow).
- Sanitised in `SanitizeEffect` and compared in `Equals`, so undo, requests and files carry it unchanged.
- Pure helpers in `StageEffects` (`Public/Show/StageEffects.h`):
  - `FAudioInput` (one snapshot, the beat carried forward to the frame time);
  - `ApplyModulation`, `GetModulatedRate`, `GetSyncTarget`, `GetTriggerEvents`, `GetTriggerStep`, `SmoothBand`, `GetCycleError`.
- `StageEffectRules::MakeAudioQuickEffects`: Bass Pulse, Beat Chase, High Sparkle, Hue per Bar, Beat Swing, Mid Zoom.

*Runner and show:*
- `FStageEffectRunner::Advance(Delta, Released, Audio)` keeps per effect a smoothed band, the last good tempo and the trigger count.
- **Sync** runs the clock at BPM / 60 / beats, pulled to the beat by at most 0.05 cycle a frame. At confidence below 0.5 or with no signal it keeps the last good tempo, or its own rate if there never was one.
- **Trigger** steps one pattern step (a whole cycle for Random) per beat division, or per onset with the Level band.
  - At most 4 steps a frame.
  - It holds without a signal and primes again without catching up.
- **Modulate:**
  - Level moves High towards Low; Size and Rate scale those.
  - The band rises at once and falls at 30/120/400 ms.
  - No signal reads 1.
- `NeedsAudio()` is rebuilt only when the running set changes.
- `UShowControlSubsystem` reads `UStageAudioReactiveSubsystem::GetFeatures()` once per tick, only while an audio effect runs: a lock-free copy, no allocation. `IsTickable` is unchanged.
- Off takes the same arithmetic as before.

*Scene v11* (`Private/Scene/StageSceneStore.*`):
- An effect writes `"audio": {mode, band, amount, modulates, beatsPerCycle, response}` by name, only when its mode is not Off.
- Bad values fall back to defaults and are counted.
- v1–v10 load with audio Off; builds before #70 refuse v11.

*UI:*
- **Effects panel:**
  - AUDIO rows (MODE; BAND, MOD, BEATS, RESP and AMOUNT shown by mode); RATE greyed while Sync or Trigger drives the clock;
  - an AUDIO quick-effect group;
  - running effects show "♪ SYNC", "♪ TRIGGER", "♪ MODULATE" or "n Hz ♪".
- **Cues panel:** "n ♪" in the FX column. **Playback panel:** "FX ♪".
- **Audio panel** (new `UStageAudioPanel`, `StageCraft.Panel.Audio`, `DA_Panel_Audio` from `Scripts/Content/create_audio_content.py`; Window > Audio..., a Lighting Desk tab, floating 560×420):
  - OFF / FILE... (OS dialog) / INPUT / SYSTEM / TEST;
  - file name, PLAY/PAUSE, STOP, LOOP, seek bar and position;
  - input device and gain;
  - SUB, BASS, MID, HIGH and LEVEL meters, BPM with confidence and a beat lamp;
  - KEEP PLAYING IN THE BACKGROUND (G1's setting had no UI);
  - the analyser's cost.
  - Every write goes through the controller's audio requests.
  - Status pushes from `OnAudioStatusChanged`. The meters run from a 30 Hz timer that exists only while a source runs.
- `UStageAudioReactiveSubsystem::ListInputDevices`.

*Found and fixed on the way (shared widget):*
- **Cause:** in UE 5.8, `USlider::SetValue` broadcasts `OnValueChanged` when the value changes. `UStageValueSlider`'s contract ("read-backs do not fire OnMoved") no longer held.
- **Symptom:**
  - the Audio panel's position read-back looked like a user drag, so a phantom seek preview froze the shown position at 0:00 (run G2a);
  - the same read-back could send a preview request from the Effects, Environment, Preferences, Playback or Sketch panels when a value changed outside them (undo, a cue).
- **Fix:** `UStageValueSlider` reports `OnMoved` only between `OnMouseCaptureBegin` and `OnMouseCaptureEnd`. Begin fires before a click's own value change, so a click on the bar still counts.
- **Regression test:** `StageCraft.UI.ValueSlider`.

*Dev and bench:*
- `StageCraft.Show.Effect` keys `audio= band= mod= beats= resp= amount=`; `Start` finds the audio quick effects; `List` prints each effect's audio settings.
- `StageCraft.Music.MeasureDelay [s]` measures the File source's heard delay against a second analyser on the WASAPI loopback.
- `StageCraft.Bench.Fixtures ... Audio=1` (`RunFixtureBench.ps1 -Audio`), valid with Effects 1..3:
  - the test signal at 128 BPM;
  - the circle synced to two beats, the dimmer sine modulated by the bass, the rainbow synced to the bar;
  - refused unless the source is Off; switched off again at the end.

**Files**
- New:
  - `Public/UI/StageAudioPanel.h`, `Private/UI/StageAudioPanel.cpp`
  - `Private/Show/Tests/StageAudioEffectTests.cpp`
  - `Content/StageCraft/UI/Panels/DA_Panel_Audio.uasset`, `Scripts/Content/create_audio_content.py`
- Changed (all under `Source/ModularSceneBuilder` unless noted):
  - Show: `Public/Show/StageEffectTypes.h`, `Private/Show/StageEffectTypes.cpp`, `Public/Show/StageEffects.h`, `Private/Show/StageEffects.cpp`, `Public/Show/StageEffectRunner.h`, `Private/Show/StageEffectRunner.cpp`, `Private/Show/StageShowConsoleCommands.cpp`
  - Subsystems: `Public/Subsystems/ShowControlSubsystem.h`, `Private/Subsystems/ShowControlSubsystem.cpp`, `Public/Subsystems/StageAudioReactiveSubsystem.h`, `Private/Subsystems/StageAudioReactiveSubsystem.cpp`
  - Scene: `Private/Scene/StageSceneStore.h`, `Private/Scene/StageSceneStore.cpp`
  - UI: `Public/UI/StageEffectsPanel.h`, `Private/UI/StageEffectsPanel.cpp`, `Private/UI/StageCuePanel.cpp`, `Private/UI/StagePlaybackPanel.cpp`, `Public/UI/StageValueSlider.h`, `Private/UI/StageValueSlider.cpp`, `Private/UI/Tests/StageUITests.cpp`
  - Workspace: `Public/Workspace/StageWorkspaceTypes.h`, `Private/Workspace/StageWorkspaceTypes.cpp`, `Private/Workspace/StageWorkspaceShell.cpp`, `Private/Workspace/SStageWorkspaceMenuBar.cpp`
  - Audio and bench: `Private/Audio/StageAudioReactiveConsoleCommands.cpp`, `Private/Diagnostics/StageFixtureBenchmark.cpp`, `Scripts/Bench/RunFixtureBench.ps1`
- Docs:
  - `Docs/ADR/0029-...md`: status, new §14;
  - `Docs/CURRENT_SYSTEMS.md`: audio analysis and audio-reactive effects rows, test count;
  - `tasks/todo.md`: G2 sub-items, ticked;
  - `STATE.md`: #69 commit hash, this entry.
- No maps, Blueprints or input actions changed. One content asset is new (`DA_Panel_Audio`).

**Verification**

*Builds:* Editor, Game Development and Game Shipping, 0 warnings.

*Shipping UTF-16/ASCII scan:*
- **Absent:** `StageCraft.Music.MeasureDelay`, `StageCraft.Music.Source`, `StageCraft.Bench.Fixtures`, `StageCraft.Show.Effect` and the dev text "follows the music: %s".
- **Present:** "KEEP PLAYING IN THE BACKGROUND", "Hue per Bar", "beatsPerCycle", "Finding the tempo", the analysis detail.

*Tests:* 201/201, with and without a renderer (197 + 4 new), user data unchanged.

| Test | What it covers |
|---|---|
| `StageCraft.Show.AudioEffects` (pure) | Rules: hostile audio settings fall back; Amount clamped; `Equals`; six valid quick effects. Maths: modulation formulas, no signal = 1, smoothing time constants, cycle error, trigger events and steps, beat carried forward. Runner: Off identical with or without music; Sync pulls ≤ 0.05 cycle a frame and locks within a second, 120 → 128 without a jump; low confidence or no music keeps 128; never → own rate; 4 beats a cycle. Trigger: primes, steps on the beat, caps 14 beats to 4 steps, holds without tempo, primes again, restart; onsets for Random. Modulate Rate. `NeedsAudio` |
| `StageCraft.Show.AudioOnFixtures` (world, test signal) | The music is never read without an audio effect, then once per tick; Bass Pulse at full with no music; a synced swing at 127.94 BPM against 128; Bass Pulse 0.09 .. 0.94 with the kick; External holds and does not read |
| `StageCraft.Scene.EffectAudioData` | v11 round trip; only audio effects write `"audio"`; enums by name; hostile values fall back or clamp and are counted; v10 loads Off |
| `StageCraft.UI.ValueSlider` | A read-back is not a move; a move inside the capture is, with its index; quiet after release |

*Scripted Development `-game` runs on the Concert stage* (G2a, then G2b after the slider fix; user data restored):
- **Test signal 128:** Beat Chase on 1–6, Bass Pulse on 7–10, Beat Swing and Hue per Bar on all.
  - The analyser read 128.07 BPM at confidence 1.00, CPU 0.37 %.
  - The chase stepped 14 times in about 7 s.
- **Screenshots:**
  - Audio panel: TEST lit, "128.1 BPM · 100 % sure", five meters moving, background on, "Analysis 0.4 % of a CPU core · 0 samples dropped";
  - Effects panel: the AUDIO quick group, the AUDIO rows, the running list with ♪ modes.
- **Scene round trip:** cue 21 Track-stored with all four effects, saved as G2Audio, a fresh Concert stage loaded, then G2Audio again. Cue 21 came back with its four effects and every audio setting (Trigger, Modulate Fast, Sync 2, Sync 4), running from the cue list.
- **File:** the G0 120 BPM WAV read 120.04 BPM, 0 underruns.
  - G2b: the panel showed PAUSE lit and "0:09 / 0:20" with the seek handle moving.
  - G2a, before the fix: stuck at "0:00".
- Artefacts: `%TEMP%\StageCraftRuns\G2a`, `G2b`; the saved scene in the scratchpad `g2scenes`.

*The file's heard delay* (`MeasureDelay 15`, run G2delay):
- The file's beat trails what Windows plays by 7.4 ms (median of 1,596 frames, 10–90 % from −10.4 to −5.8 ms).
- That is inside the plan's 10 ms tolerance, so `FileOutputDelayMs` stays at 78.

*Performance A/B:*
`RunFixtureBench.ps1 -Counts 200,500`, show tick mean in ms. Baseline is a worktree at `a85475f` (workspace copied in, editor built). Imports were set aside for every run, then restored and hash-checked. 12 runs, in two orders.

| Case | 200 idle | 200 crossfade | 500 idle | 500 crossfade |
|---|---|---|---|---|
| `a85475f`, `-Effects 1` (4 runs) | 4.44 | 4.87 | 10.88 | 12.92 |
| G2, `-Effects 1` (4 runs) | 4.63 | 5.02 | 11.34 | 13.48 |
| G2, `-Effects 1 -Audio` (2 runs) | 4.59 | 4.97 | 11.29 | 13.45 |
| `a85475f` / G2, no effects (1 run each) | 0 / 0 | 4.48 / 4.36 | 0 / 0 | 9.52 / 10.22 |

- **Audio=1 costs nothing measurable** against `Effects=1` in the same tree, inside the 5 % gate. The test signal read 128.04 BPM; the analyser used 0.35–0.37 % of a core with 0 samples dropped.
- **Effects=1 against the baseline: the remaining difference is at most 4 %, within run-to-run spread.**
  - In the first order (the baseline first in each pair) G2 read 6–10 % slower. Reversed (G2 first) it read −2 % to +3 %, so the first gap was drift with run order.
  - The Off path gained a switch, a pointer test and an optional reset per effect per frame. That is too little to show at 200–500 fixtures.
- **Idle** with no effects: no show tick in either tree, and frame times match (10.66 vs 10.65 ms at 200).
- **CuesStarted** is 5 per crossfade phase in every CSV (a sound bench).
- The baseline worktree `E:/SC_Baseline` was removed afterwards. CSVs: `Saved/StageCraft/Benchmarks/FixtureBench_20261105-1800*` to `-181*` (the baseline's were in the worktree).


**Commit:** `9acfa3c` (committed by Gevor).

**Known issues / follow-ups**
- **The flash limiter is G3.** Until then an audio-driven chase or sparkle can reach real fixtures through DMX OUT without a cap. High Sparkle can step up to 10 times a second on onsets. Do G3 before using audio looks on real lights.
- **Packaged Shipping not run at runtime.** It needs the Audio panel by mouse (FILE... and SYSTEM). Gevor's check.
- **The panel's controls by mouse are not exercised by automation:** a C++ panel needs a local player. The scripted runs drove the sources by dev commands and checked the panel's read-back and screenshots. Not clicked: the FILE... dialog, transport buttons, the seek drag, the device list, the gain slider, the background toggle.
- **Mouse checks for Gevor:**
  - the AUDIO rows and quick effects;
  - the ♪ markers in the Cues and Playback panels (not on screen in the runs);
  - real music on each source.
- **Seek restarts the analysis** (as in G1): Sync keeps the last tempo meanwhile; Trigger holds.
- Effects whose Level modulation applies to Color have no visible effect: Level reads Low/High, which colour does not use. The panel still offers it.


## #71 — Phase G3: flash limiter, AUDIO master and KILL, LISTENING, latency offsets (2026-11-05)

**Housekeeping first.** #70 said "uncommitted"; it now carries `9acfa3c` (Gevor committed G2).

**What & why**
- Gevor's brief (2026-11-05) asked for Phase G3 "in auto mode":
  - a DMX flash limiter (3 Hz default) on audio-driven intensity and strobe;
  - an AUDIO master fader and an AUDIO KILL;
  - a Listening indicator and a per-source latency offset slider in the Audio panel;
  - tests, 0-warning builds, a benchmark with no regression, todo.md and STATE.md.
- ADR 0029 §6 and §7 (approved) govern the details, and §15 records how G3 realised them. Where the brief and the ADR differ:
  - **KILL** is the ADR's "no-signal value", not a blackout. A pulse goes fully open and a chase holds; BLACKOUT is still the way to dark.
  - **The limiter** follows the ADR's scope: effects that follow the music only. Programmed effects are the operator's responsibility.
  - **LISTENING** shows in the status bar (ADR §6) as well as in the Audio panel (brief).
- No plan-mode stop: the brief said auto mode. The plan is in `tasks/todo.md` (G3a–G3f).

**Design** (details in ADR 0029 §15)
- **Flash gate** (`StageEffects::FFlashGate`, `Public/Show/StageEffects.h`): a flash is a rise from below 20 % to above 80 % of the range.
  - At most `FlashLimitHz` flashes a second.
  - A refused rise **holds the last value shown** (at most 80 %). A clamp at 80 % would have let a 10 Hz square flicker between 0 and 80 % at full rate.
  - Values between the thresholds and a light that stays up pass unchanged.
  - Idempotent within a frame (the show can mix a fixture twice in a frame), on the runner's own clock.
- **Strobe cap** (`StageEffects::GetStrobeShutterCap`): a music-driven Shutter is capped so the fixture's strobe is no faster than the limit. A fixture whose slowest strobe is already faster stays open.
- **Runner** (`FStageEffectRunner`):
  - `Evaluate` collects the attributes that effects following the music give a fixture. After composing, it passes their intensities through a gate per fixture and attribute, and caps their Shutter (`LimitFlashes`).
  - Gates survive cue changes while the fixture stays driven, and are pruned in `RebuildDrives`.
  - `SetAudioMaster` (KILL = 0): Modulate reads `1 + (band − 1) × master`. At 0 every audio effect gets the silent input: Sync keeps its last tempo, Trigger holds and primes again without catching up.
  - `SetFlashLimitHz` is clamped to 1..10.
- **Show** (`UShowControlSubsystem`):
  - `SetAudioMaster` and `SetAudioKill` are session-only and broadcast `OnDeskStateChanged`. KILL is logged.
  - The flash limit is pushed from `UStageAudioReactiveSubsystem::OnAudioStatusChanged` (bound in Initialize, removed in Deinitialize).
  - `GetEffectFlashLimitHz` reads the limit.
- **Requests:** `AModularPlayerController::RequestSetAudioMaster` and `RequestSetAudioKill`, judged as `EStageCueRequest::Masters` (the GM's gate).
- **Setting:** `FStageAudioReactiveSettings::FlashLimitHz` (Config, default 3, sanitised 1..10).
- **Live latency:**
  - `FStageAudioAnalyzer::SetHeardDelaySeconds` is an atomic read per block.
  - `UStageAudioReactiveSubsystem::ApplySettings` now restarts the analysis only for band edges and the test tempo. Before, every settings change restarted it, and the tempo took about 3 s to come back.
- **UI:**
  - **Audio panel:**
    - "● LISTENING" in the header while Input or SystemAudio is Starting or Running;
    - LATENCY (−500..+500 ms in 5 ms steps, for the source playing; hidden for TEST and OFF);
    - FLASH (1..10 a second, with a seizure warning above 3).
    - Both sliders apply on release as one settings request. `DraggedSetting` replaces `bGainDragged`.
  - **Status bar:** "● LISTENING", pushed by `OnAudioStatusChanged`.
  - **Playback panel:** an AUDIO fader and KILL next to GM and BLACKOUT, built by the new `BuildMasterColumn`, which the GM column now uses too.
  - **Effects panel:** an AUDIO slider and KILL above the audio quick effects.
- **Dev console** (`#if !UE_BUILD_SHIPPING`):
  - `StageCraft.Music.Master [0..1]`, `StageCraft.Music.Kill [On|Off]`, `StageCraft.Music.Settings FlashLimit=<Hz>`;
  - `StageCraft.Workspace.ScrollPanels end horizontal`, to scroll the playback desk to its masters for screenshots.

**Files changed** (under `Source/ModularSceneBuilder` unless noted)
- `Public/Audio/StageAudioReactiveTypes.h`, `Private/Audio/StageAudioReactiveTypes.cpp`: `FlashLimitHz` and its bounds; Sanitize.
- `Public/Show/StageEffects.h`, `Private/Show/StageEffects.cpp`: `ApplyAudioMaster`, `FFlashGate`, `GetStrobeShutterCap`, thresholds.
- `Public/Show/StageEffectRunner.h`, `Private/Show/StageEffectRunner.cpp`: master, limit, clock, flash gates, `LimitFlashes`.
- `Public/Subsystems/ShowControlSubsystem.h`, `Private/Subsystems/ShowControlSubsystem.cpp`: AUDIO master and KILL, limit push.
- `Public/Player/ModularPlayerController.h`, `Private/Player/ModularPlayerControllerProgrammer.cpp`: the two requests.
- `Private/Audio/StageAudioAnalyzer.h`, `Private/Audio/StageAudioAnalyzer.cpp`: live heard delay.
- `Private/Subsystems/StageAudioReactiveSubsystem.cpp`: no restart for timing changes.
- `Private/Audio/StageAudioReactiveConsoleCommands.cpp`: Master, Kill, FlashLimit.
- `Public/UI/StageAudioPanel.h`, `Private/UI/StageAudioPanel.cpp`: LISTENING, LATENCY, FLASH.
- `Public/UI/StageStatusBarWidget.h`, `Private/UI/StageStatusBarWidget.cpp`: LISTENING.
- `Public/UI/StagePlaybackPanel.h`, `Private/UI/StagePlaybackPanel.cpp`: the AUDIO column, `BuildMasterColumn`.
- `Public/UI/StageEffectsPanel.h`, `Private/UI/StageEffectsPanel.cpp`: the AUDIO row.
- `Private/Workspace/StageWorkspaceConsoleCommands.cpp`: ScrollPanels horizontal.
- `Private/Show/Tests/StageAudioEffectTests.cpp`: three new tests, and `AudioOnFixtures` extended.
- Docs: `Docs/ADR/0029-audio-analysis-and-audio-reactive-effects.md` (status line and §15), `Docs/CURRENT_SYSTEMS.md` (a new row; 204 tests).
- Records: `tasks/todo.md` (G3 and G3a–f), `STATE.md` (#70 carries `9acfa3c`; this entry).
- No content assets, maps or input actions changed. The Audio panel definition is unchanged; the new rows are built in C++.

**Verification**
- **Tests:** 204/204 with NullRHI and 204/204 with the offscreen renderer (`RunAutomation.ps1 -ExpectedCount 204`), user data unchanged both times.
  - New: `Show.FlashLimiter`, `Show.AudioMaster`, `Show.FlashLimitOnFixtures`; `Show.AudioOnFixtures` extended.
  - Measured in the tests:
    - a 10 Hz square at limit 3 went from 30 flashes in 3 s to 9, shortest gap 0.333 s;
    - an 8 Hz beat-synced square on a real fixture gave 8 flashes at limit 3 (gap ≥ 0.367 s), 24 at limit 10 and 25 without music;
    - Bass Pulse with the bass silent read 0.000 at master 1, 0.500 at 0.5 and 1.000 at 0 (KILL);
    - a latency change kept the analyser's 128 BPM.
  - The first Show-filter run reported GameUserSettings.ini changed: the engine wrote the new `FlashLimitHz=3` key once (the known first-run effect of a new Config field). Both full runs afterwards: unchanged.
- **Builds:** Editor, Game Development and Game Shipping succeeded with 0 warnings.
- **Shipping UTF-16 scan:** absent: `StageCraft.Music.Master`, `StageCraft.Music.Kill`, `StageCraft.Music.Settings`, `[FlashLimit=<Hz>]`, `StageCraft.Bench.Fixtures`. Present: the seizure warning, LISTENING, the KILL tooltip, the latency tooltip.
- **Scripted run G3a** (Concert stage, `-WithoutUserImports`; user data restored):
  - TestSignal at 128 with a Strobe synced at a quarter beat and Bass Pulse;
  - FLASH at 5 shows the warning; LATENCY hidden for TEST;
  - Music.Master 0.5 and Kill On/Off logged;
  - SYSTEM shows LISTENING in the status bar and the panel header, with LATENCY +40 ms;
  - OFF removes LISTENING.
  - Screenshots: `%TEMP%\StageCraftRuns\G3a`.
- **Scripted run G3b:**
  - The Effects panel (in front) shows the AUDIO slider and a lit KILL above the audio quick effects.
  - The Playback panel, scrolled to its masters with `ScrollPanels end horizontal`, shows GM 100 % with BLACKOUT, and AUDIO 60 % with a lit KILL.
  - After that run the level text was changed to always show the fader's percent, because the lit KILL already says it is on and "KILL KILL" was redundant. Rebuilt, and the tests were run again afterwards.
  - Screenshots: `%TEMP%\StageCraftRuns\G3b`.
- **Benchmark** (`Bench.Fixtures 200 500`, imports set aside and restored, against a `9acfa3c` worktree with the same workspace layout, run order alternated). Show tick mean in ms:

  | Case | 200 idle | 200 crossfade | 500 idle | 500 crossfade |
  |---|---|---|---|---|
  | Base, Effects=1 Audio=1 (3 runs) | 4.37 | 4.90 | 10.88 | 12.76 |
  | G3, Effects=1 Audio=1 (2 runs) | 4.41 | 4.98 | 11.33 | 13.31 |
  | Base, Effects=1 (2 runs) | 4.44 | 5.09 | 10.68 | 12.52 |
  | G3, Effects=1 (2 runs) | 4.09 | 4.70 | 10.68 | 12.40 |
  | Base, no effects (2 runs, crossfade only) | | 4.21 | | 9.62 |
  | G3, no effects (2 runs, crossfade only) | | 4.21 | | 9.55 |

  - **Audio=1 against Audio=1** (the limiter and master at work on 200 or 500 music-driven dimmers) is within +1 % to +4 %, inside the 5 % gate.
    - The G3 run that went first matched the base exactly at 500 (10.72 against 10.70 idle, 12.63 against 12.63 crossfade).
    - The +4 % comes from the G3 run that followed a base run.
  - Without audio effects the limiter does no work, and G3 matches or beats the base. With no effects the show does not tick at rest in either tree.
  - The analyser used 0.34–0.36 % of a core at 128.04 BPM, with 0 samples dropped.
  - **Discarded:** one G3 Audio=1 run. Mid-run, at 500 fixtures, its log shows the "Theatre" starter stage opened from the welcome screen, replacing the bench stage (100 show ticks, then none). That takes a click or key in the game window, so it was outside input, not code. A replacement pair (G3 first, then base) was run.
  - A first no-effects pair was 10 % apart at 500 crossfade, with every frame time raised. A rerun of two pairs in alternating order matched (above).
  - The `E:/SC_Baseline` worktree was removed.
- **Awaiting Gevor (manual):**
  - by mouse: LATENCY and FLASH drags, the AUDIO fader and KILL in both panels;
  - real music on each source, with a strobe-like audio effect on DMX OUT watched on a visualiser (3 a second);
  - a packaged Shipping run through the Audio panel.

**Commit:** `18f3639` (committed by Gevor).

**Known issues / follow-ups**
- Colour effects that follow the music are not flash-limited (ADR §7 scope is intensity and strobe). An RGB fixture can still flash through a music-driven colour change.
- A limited attribute that also has a programmed effect on it is limited as a whole.
- Still awaiting Gevor: removing `E:/SC_G0`, `E:/SC_G0_Package` and branch `spike/g0-audio`; ADR 0030 decisions.
- **Phase G is complete in code.** H0 (assistant spike) is next on the roadmap.

## #72 — Phase H0 and H1: assistant streaming spike, offline knowledge index, SHOW ME, assistant docked right (2026-11-06)

**Housekeeping first.** #71 said "uncommitted"; it now carries `18f3639` (Gevor committed G3).

**What & why**
- Gevor's brief (2026-11-06) asks for Phase H (H0–H4) "fully aligned with ADR 0030", in auto mode, starting with the H0 spike and the H1 knowledge base. It asks for:
  - an assistant panel docked on the right of the workspace and the Lighting Desk;
  - offline knowledge (BM25, English and Armenian) indexed from the code: widgets, buttons, DMX parameters, shortcuts;
  - diagnostics (H2), proposal cards with one undo step (H3/H4);
  - 0-warning builds, all tests, todo.md and STATE.md.
- This entry is H0 and H1. H2–H4 follow.
- **ADR 0030 was "proposed".** The brief is taken as approval of its design. Still Gevor's: the default online model (§8) and the key policy for a shipped product (§9).
- **Where the brief and the ADR differ, the ADR wins:**
  - Console commands are not indexed. Development commands are compiled out of Shipping and the assistant never runs them (ADR §2, §3).
  - Live show state is H3's read tools; H2 adds diagnostics. H1 indexes what is static: the key map, panel controls, fixture attributes and help articles.
- No plan-mode stop: the brief said auto mode. The plan is in `tasks/todo.md` (H1a–H1f).

**H0 (ADR 0030 §13)**
- **Streaming:** UE 5.8 delivers a response body as it arrives through `IHttpRequest::SetResponseBodyReceiveStreamDelegateV2`, called on the HTTP thread.
  - Checked against `Scripts/Assistant/MockSseServer.ps1`, a local TcpListener that answers with a Messages-API-shaped SSE stream, one event every 150 ms.
  - Result: 9 chunks over 1.32 s and 1.29 s, one per event.
- **`FStageSseParser`** (`Private/AI/StageSseParser`): a pure splitter for H3's streamed replies. It handles LF, CRLF and CR line ends, comments, multi-line data, an event closed without a blank line, and UTF-8 cut anywhere. Over-long lines are capped.
- **`StageCraft.Assistant.Spike [Model=…|all] [Runs=2] [Url=…] [NoFallbacks]`** (development only):
  - It sends the v2 request shape: the stage_command tool, one system block (prompt and knowledge digest) with `cache_control`, and a question.
  - It logs time to the first byte, block and text; the chunks; cache write and read tokens; and the cost.
  - It appends a row per request to `Saved/Profiling/AssistantSpike.csv`.
- **Against the mock:** the parser read the usage (cache write 1800, then cache read 1800), the deltas and the stop reason.
- **Not done:** the paid measurement per model. There is no `ANTHROPIC_API_KEY` on this PC, and it spends money. That run is Gevor's (see todo.md).

**H1 design (ADR 0030 §14)**
- **Index** (`Public/AI/StageKnowledge.h`, `Private/AI/StageKnowledge.cpp`): `FStageKnowledgeEntry` / `FStageKnowledgeIndex`.
  - `StageKnowledge::Build` (pure) covers commands (`StageKeyBindings`), panels, anchored controls, 16 fixture attributes (models from the catalog with ranges, e.g. "Beam 280 4 to 60°") and help articles.
  - BM25 statistics are computed once.
  - `ParseArticle` (title, keywords/panel/control header, body; 16 K cap).
  - `BuildDigest`: names only, byte-stable, for the cached system prompt.
  - `Answer` puts the user's current chords into command answers.
- **Subsystem** (`UStageKnowledgeSubsystem`, a game-instance subsystem):
  - The index is built lazily: 175 entries in about 7 ms.
  - It is marked stale by the item catalog and panel catalog delegates.
  - `GatherStaticSources` builds without a game instance (tests).
- **Search** (`Public/AI/StageKnowledgeSearch.h`):
  - BM25 (k1 1.2, b 0.75), names weighted 3 to the body's 1.
  - English: stop words, then a light stem.
  - Armenian: question and emphasis marks removed; endings stripped until none is left; `-ության/-ությամբ` folded into `-ություն`; lone Armenian letters dropped. An Armenian prompt is also read through `StageAIArmenian`.
  - One hit per id, in the asker's language when there is a translation. Nothing is answered below a score of 2.
  - `IsHowToQuestion` (EN/HY) and `Route` (Command, Knowledge, KnowledgeOnly).
- **Assistant** (`UStageAICommandSubsystem`):
  - `InterpretLocally` asks the knowledge for prompts the grammar does not understand and for "help" with a topic.
  - An action asked as a how-to question ("how do I store cue 5") is answered from the knowledge, or with "I have no help on that yet, so I changed nothing", and **never runs**.
  - How-to questions the knowledge answers are answered offline even with the online model on.
  - Replies carry `FStageAIReference` (panel, control, source title), a new USTRUCT in `StageAITypes.h`.
- **Anchors** (`Public/UI/StageUIAnchors.h`):
  - `FStageUIAnchor` and the `IStageAnchoredPanel` UINTERFACE (`FindAnchorWidget`).
  - `FStageAnchorWidgets`, refilled when a panel builds.
  - `GetTables`/`Find`/`Get`, plus `IsShown` (every container visible), `BringIntoView` (scroll boxes) and `Pulse` (three opacity blinks over 1.5 s, compact controls swell 12 %, a core ticker only while a pulse runs, restored after).
  - Static tables, from which the build code takes the labels and tooltips: Playback 10, Cues 11, Effects 10, Audio 16, Patch 13, Groups 5, Palettes 3 (68 anchors).
- **Workspace** (`UStageWorkspaceSubsystem`):
  - `ShowControl` opens the panel, looks for a shown control for up to 0.5 s, scrolls and pulses it, and logs "SHOW ME: …". A hidden control (TAP with no effect selected) is not pulsed; that is logged.
  - `FindPanelWidget`, and `LivePanelWidgets` (weak) filled in `CreatePanelContent`.
- **Chat** (`UStageAIAssistantWidget`):
  - SHOW ME or OPEN PANEL and "From: <title>" under knowledge replies. `BuildMessage` now takes the message index.
  - The welcome line mentions how-to questions.
- **Layouts** (`StageWorkspaceShell.cpp`): the assistant is docked on the right.
  - Default: the right column is now Inspector/Render/Environment/BOM (0.6) over AI Assistant (0.4).
  - Lighting Desk: Groups/Palettes (0.32), AI Assistant (0.36), Inspector/Outliner (0.32).
  - Saved user layouts are unchanged.
- **Help articles** (`Content/StageCraft/Help/en|hy/*.md`), 18 in each language: tracking, cue-timing, playbacks, masters, programmer, groups, palettes, patch, dmx-in, dmx-out, effects, audio, flash-safety, undo, layouts, assistant, why-dark, scenes.
  - The facts were checked against the code: fan modes, Ctrl+S, layout names, and that faders and masters are live and not undoable.
  - Staged as UFS (`DefaultGame.ini`).
- **Dev console** (`#if !UE_BUILD_SHIPPING`): `StageCraft.Assistant.Search <question>`, `.Knowledge`, `.ShowMe <PanelTag> [Control]`, `.CheckAnchors`, `.Spike`.

**Files changed** (under `Source/ModularSceneBuilder` unless noted)
- New:
  - `Public/AI/StageKnowledge.h`, `Public/AI/StageKnowledgeSearch.h`, `Public/AI/StageKnowledgeSubsystem.h`, `Public/UI/StageUIAnchors.h`;
  - `Private/AI/StageKnowledge.cpp`, `Private/AI/StageKnowledgeSearch.cpp`, `Private/AI/StageKnowledgeSubsystem.cpp`, `Private/AI/StageKnowledgeConsoleCommands.cpp`, `Private/AI/StageSseParser.h`, `Private/AI/StageSseParser.cpp`, `Private/AI/StageClaudeSpikeCommands.cpp`, `Private/UI/StageUIAnchors.cpp`;
  - `Private/AI/Tests/StageKnowledgeTests.cpp`;
  - `Content/StageCraft/Help/en/*.md` and `Content/StageCraft/Help/hy/*.md` (36 files);
  - `Scripts/Assistant/MockSseServer.ps1`.
- Changed:
  - `Public/AI/StageAITypes.h`, `Public/AI/StageAICommandSubsystem.h`, `Private/AI/StageAICommandSubsystem.cpp`;
  - `Public/UI/StageAIAssistantWidget.h`, `Private/UI/StageAIAssistantWidget.cpp`;
  - the anchored panels: `Public/UI/` and `Private/UI/` `StagePlaybackPanel`, `StageCuePanel`, `StageEffectsPanel`, `StageAudioPanel`, `StagePatchPanel`, `StageGroupsPanel`, `StagePalettesPanel`;
  - `Public/Workspace/StageWorkspaceSubsystem.h`, `Private/Workspace/StageWorkspaceSubsystem.cpp`, `Private/Workspace/StageWorkspaceShell.cpp`;
  - `Config/DefaultGame.ini`, `Docs/ADR/0030-…` (status, §13, §14), `Docs/CURRENT_SYSTEMS.md`, `tasks/todo.md`, `STATE.md` (#71 commit, #72).
- No content assets, maps or Blueprints changed.

**Verification**
- **Tests:** 209/209 with no renderer and 209/209 with the offscreen renderer (`RunAutomation.ps1 -ExpectedCount 209`); user data unchanged. The five new tests:
  - `StageCraft.Assistant.Sse`: every split point of a stream gives the same five events; CR, CRLF, comments, unnamed and data-less events, Finish, the over-long line cap.
  - `StageCraft.Assistant.Knowledge`: 36 articles read with no errors, each in both languages; their anchors are real; 68 anchors with label, tooltip and a unique id; one entry per command (34), anchor and attribute (16). The digest has names, no console command and no article text, and is the same when built twice. Article parsing refuses bad input. A rebound Blackout (Ctrl+F9) is answered with Ctrl+F9.
  - `StageCraft.Assistant.Search`: 93 questions (65 EN, 28 HY) find their answer in the top three. The first run found 91; the two Armenian misses were fixed in the stemmer, not in the test. Armenian questions get the Armenian article; nonsense gets nothing; ranking is stable.
  - `StageCraft.Assistant.Routing`: stems; 22 how-to cases; routes (an action asked how never runs, stage questions keep their answers, bare "help" keeps the list).
  - `StageCraft.Assistant.HowTo` (world): "how do I store cue 5" (EN and HY) never reaches the executor; "store cue 5" runs once; "how many fixtures?" is answered live.
- **Builds:** Editor, Game Development and Game Shipping with 0 warnings and 0 errors.
- **Shipping scan (UTF-16):**
  - absent: `StageCraft.Assistant.Spike/Search/CheckAnchors/ShowMe/Knowledge`, `AssistantSpike.csv`, `StageCraft.Music.Master`;
  - present: "SHOW ME", "OPEN PANEL", "Open the panel and point at the control.", "I have no help on that yet".
- **Scripted run H1a** (Lighting Desk, mock server):
  - knowledge 175 entries and 0 article problems; the anchor check twice, 68 of 68 live;
  - "where is the blackout button", "how do I store a cue", "ինչպե՞ս պատչել սարքերը" (the Armenian patch article) and "what is zoom" (catalog ranges) answered from the knowledge; "how many fixtures" answered by the grammar; no cue was stored;
  - the H0 mock results above;
  - user data restored.
- **Scripted run H1b:** the docked assistant shows "TAP is in the Effects panel…", SHOW ME and "From: Effects › TAP".
  - A Slate window capture 0.25 s after SHOW ME shows BLACKOUT mid-blink. The engine's screenshot does not show render opacity, so it cannot be used for this.
  - SHOW ME on TAP opened the Effects panel. TAP was hidden (no effect selected), which led to the `IsShown` check.
  - Screenshots: `%TEMP%\StageCraftRuns\H1a`, `H1b`.
- **Benchmark:** not run. Nothing in the show tick or the effect runner changed. Anchors are recorded once when a panel builds; the index is built once on the first question.
- **Awaiting Gevor:**
  - the paid H0 run;
  - by mouse: how-to questions in both languages, SHOW ME and OPEN PANEL, floating and re-docking the assistant;
  - a packaged build answering from the help articles (UFS staging not yet exercised).

**Commit:** `6185a1f`.

**Known issues / follow-ups**
- **The pulse is a dimmed blink, not ADR §7's Slate overlay.** It is visible but modest on the dark theme. An overlay outline could follow if Gevor wants it stronger.
- **Anchors cover the seven Lighting Desk panels.** Inspector, Environment, Preferences, the viewport toolbar and the status bar are not anchored yet. The knowledge still names them through panels, commands and articles.
- **Answers to how-to questions are English text inside an Armenian frame** when the hit is a control or command: labels and tooltips are English UI. Armenian articles answer in Armenian.
- **93 questions, not the ADR's ~120.** The corpus grows with H2–H4.
- **ADR 0030 decisions still open (Gevor):** the default online model (after the paid H0 run) and the key policy for a shipped product.
- **Next:** H2 (`StageShowDiagnostics`, `StageFixtureExplain`, Problems list).

## #73 — Phase H2: show diagnostics, Problems panel, "why is fixture N dark?" (2026-11-06)

**Housekeeping first.** #72 said "uncommitted"; it now carries `6185a1f` (Gevor committed H0/H1).

**What & why**
- Gevor's brief (2026-11-06) asks for Phase H2, aligned with ADR 0030, in auto mode:
  - a live diagnostic scanner (`StageShowDiagnostics`) for unpatched fixtures, DMX address conflicts, missing cue targets, muted or disconnected audio and broken signal chains;
  - a Problems list whose rows jump to the fixture, cue or patch concerned;
  - `StageFixtureExplain`: "why is fixture X dark / not responding?" traced along patch, DMX, cue values, programmer, Grand Master and Blackout, dimmer, effects and the audio flash limit / KILL, in English and Armenian;
  - 0-warning builds, all tests, todo.md and STATE.md.
- **Where the brief and ADR 0030 / Rules.md differ:**
  - "Constantly checks" is push, not poll (Rules.md: subsystems never poll). The checks re-run once, on the frame after any show, patch, programmer, palette, DMX or audio change, and only while the Problems panel (or anyone) listens. Whether the music is heard is not pushed; REFRESH and the assistant re-run on demand.
  - ADR §5 put the Problems list in the Patch and Cues panels. One Problems panel covering every place replaces that, as the brief asks.
  - "Explain" is pure functions plus one world subsystem that gathers the facts (the ADR §5 shape), not a subsystem per feature.

**Design (ADR 0030 §15)**
- **Mix explanation** (`Public/Show/StageShowMix.h`, `Private/Show/StageShowMix.cpp`): `StageShowMix::ExplainAttribute` mirrors `ResolveAttribute` and reports the winning layer (home, cue list, playback with `FPlaybackLayer::Index`, programmer, highlight), whether an effect changed the value and whether the masters scaled it.
  - `UShowControlSubsystem::BuildLayers` now builds the layers for both `ResolveFixture` (writing) and `ExplainAttribute` (explaining), so the explanation cannot drift from the output.
  - `UShowControlSubsystem::FindSettingCue` / `StageCueTracking::FindSettingCue`: the cue a tracked value comes from.
- **Diagnostics** (`Public/Show/StageShowDiagnostics.h`, `Private/Show/StageShowDiagnostics.cpp`, pure): `Run(FStageShowSnapshot)` returns `FStageDiagnostic`s worst first, then in code order. 18 codes:
  - PatchConflict (overlap or past channel 512), Unpatched (only while DMX goes in or out), DMXInOff, DMXInFailed, DMXInNoSignal, DMXOutFailed, UniverseMuted, OutputPausedUnderExternal, EffectsHeldUnderExternal;
  - PaletteReferenceMissing, CueNamesMissingFixture (one finding per cue list, listing the cues);
  - BlackoutOn, GrandMasterLow (at or below 20 %);
  - AudioSourceFailed, AudioNoSource, AudioNoSignal, AudioKilled (the last three only while effects follow the music);
  - ProgrammerHolds.
  - `Describe(Diagnostic, Language)`: one sentence in English or Armenian that says what to do.
  - Each finding names an `EStageDiagnosticPlace`; `StageUIAnchors::GetPlaceTarget` maps it to a panel anchor, so the show code stays UI-free.
- **Why is it dark** (`Public/Show/StageFixtureExplain.h`, `Private/Show/StageFixtureExplain.cpp`, pure): `Explain(FStageFixtureFacts, Language)`.
  - The first dark cause along the chain (`EStageDarkCause`): DMX IN off, a silent universe or no address (under External); Blackout; the Grand Master; the dimmer's layer (tracked-from cue, playback name and fader, programmer, home); an effect; the colour mix; the iris.
  - Separately, what keeps a real fixture from following (`EStageOutputCause`): DMX OUT off, failed or paused; unpatched; muted universe; conflict.
  - The text gives the verdict, then one line per step: control, intensity, effect (with the flash limit and KILL), masters, colour, iris, real fixture.
- **`UStageShowDiagnosticsSubsystem`** (world subsystem, Game and PIE; `Public/Subsystems/StageShowDiagnosticsSubsystem.h`, `Private/Subsystems/StageShowDiagnosticsSubsystem.cpp`): gathers the snapshot and one fixture's facts.
  - It binds the show, patch, library, programmer, DMX network and audio delegates.
  - A one-shot FTSTicker coalesces a frame's changes into one run, and `OnProblemsChanged` is broadcast only when the findings differ.
  - Everything is unbound and the ticker removed in Deinitialize. It never changes the show.
- **Problems panel** (`Public/UI/StageProblemsPanel.h`, `Private/UI/StageProblemsPanel.cpp`, `StageCraft.Panel.Problems`, `DA_Panel_Problems` from `Scripts/Content/create_problems_content.py`): a summary, REFRESH, and one row per finding (ERROR / WARNING / INFO).
  - A click selects the fixtures through `RequestSelectItems` and calls `ShowControl` on the place's anchor.
  - Anchors ProblemList and Refresh; the knowledge index now has 70 anchors on eight panels.
  - A tab in the Lighting Desk layout next to Audio; Window › Problems...
- **Assistant:** intents `ExplainFixture` (`FixtureId`; unset means the selected fixture) and `ListProblems`. Both are questions and never run anything.
  - Grammar (`ParseDiagnostics`, before the fixture controls): "why is fixture 12 dark", "why is light 3 not responding/off", "explain fixture 7", "why is it dark", "any problems", "what's wrong", "check the show", "run diagnostics". A why-question about no fixture in particular still goes to the help article, and "how do I fix ..." does too.
  - Armenian lexicon: ինչու, բացատր, մութ, չի/չեն, վառվ, աշխատ, արձագանք, խնդիր, սխալ, ստուգ.
  - Online tool: `explain_fixture {fixture_id}` (a whole number of at least 1) and `list_problems`.
  - Reply: an explanation comes with SHOW ME at the cause's control; a list shows 6 findings with OPEN PANEL on Problems.
- **Help:** `why-dark` (EN/HY) now mentions asking the assistant and the Problems panel; new `problems` article (EN/HY).
- **Dev commands** (`Private/Show/StageDiagnosticsConsoleCommands.cpp`, `#if !UE_BUILD_SHIPPING`): `StageCraft.Problems.List [hy]`, `StageCraft.Problems.GoTo <row>`, `StageCraft.Show.Explain <id> [hy]`.

**Files changed**
- New:
  - `Public/Show/StageShowDiagnostics.h`, `Private/Show/StageShowDiagnostics.cpp`;
  - `Public/Show/StageFixtureExplain.h`, `Private/Show/StageFixtureExplain.cpp`;
  - `Public/Subsystems/StageShowDiagnosticsSubsystem.h`, `Private/Subsystems/StageShowDiagnosticsSubsystem.cpp`;
  - `Public/UI/StageProblemsPanel.h`, `Private/UI/StageProblemsPanel.cpp`;
  - `Private/Show/StageDiagnosticsConsoleCommands.cpp`, `Private/Show/Tests/StageDiagnosticsTests.cpp`;
  - `Content/StageCraft/UI/Panels/DA_Panel_Problems.uasset`, `Scripts/Content/create_problems_content.py`;
  - `Content/StageCraft/Help/en/problems.md`, `Content/StageCraft/Help/hy/problems.md`.
- Changed:
  - `Public/Show/StageShowMix.h`, `Private/Show/StageShowMix.cpp`, `Public/Show/StageCueTracking.h`, `Private/Show/StageCueTracking.cpp`;
  - `Public/Subsystems/ShowControlSubsystem.h`, `Private/Subsystems/ShowControlSubsystem.cpp`;
  - `Public/UI/StageUIAnchors.h`, `Private/UI/StageUIAnchors.cpp`;
  - `Public/AI/StageAITypes.h`, `Private/AI/StageAITypes.cpp`, `Private/AI/StageAICommandParser.cpp`, `Private/AI/StageAICommandSchema.cpp`, `Private/AI/StageAIArmenian.cpp`, `Public/AI/StageAICommandSubsystem.h`, `Private/AI/StageAICommandSubsystem.cpp`, `Private/AI/Tests/StageKnowledgeTests.cpp`;
  - `Public/Workspace/StageWorkspaceTypes.h`, `Private/Workspace/StageWorkspaceTypes.cpp`, `Private/Workspace/SStageWorkspaceMenuBar.cpp`, `Private/Workspace/StageWorkspaceShell.cpp`;
  - `Content/StageCraft/Help/en/why-dark.md`, `Content/StageCraft/Help/hy/why-dark.md`;
  - `Docs/ADR/0030-...` (status, §15), `Docs/CURRENT_SYSTEMS.md`, `tasks/todo.md`, `STATE.md` (#72 commit, #73).
- No maps or Blueprints changed; one new data asset.

**Verification**
- **Builds:** Editor, Game Development and Game Shipping, 0 warnings.
- **Tests:** 212/212 with no renderer and 212/212 with the offscreen renderer (`RunAutomation.ps1 -ExpectedCount 212`); user data unchanged (108 files). New tests:
  - `StageCraft.Show.Explain`:
    - Ten layer cases agree with `ResolveAttribute`: HTP playback index, LTP latest, programmer, an effect, the highlight hiding an effect, the masters on intensity only, Blackout over the highlight.
    - 19 cause cases: each dark and output cause, its place and its wording, with Armenian text for each.
    - On a built show: tracked from cue 1 into cue 2, programmer, GM 0, Blackout, playback 1 by HTP, highlight, an unknown ID.
  - `StageCraft.Show.Diagnostics`:
    - A clean show finds nothing.
    - Each of the 18 faults fires with its severity, worst first, with a sentence in both languages, the same on every run, and a place that is a real anchor.
    - Subjects are sorted; no Unpatched without DMX; findings per playback; the GM threshold; no audio finding under External.
    - On a built world: three changes in one frame give one broadcast; no change gives none; a change that leaves the list as it was gives none; new findings give one.
  - `StageCraft.Assistant.Diagnostics`:
    - 13 English and Armenian questions parse to the right intent and fixture.
    - A why-question about no particular fixture, and "how do I fix", stay with the help; "turn fixture 3 off" still sets the fixture.
    - The tool's fixture_id validation.
    - In a world: Blackout explained, with SHOW ME on BLACKOUT; the Armenian answer; "which fixture?" when nothing is selected; the problem list and "found no problems"; nothing ever executed.
  - Changed: `StageCraft.Assistant.Knowledge` (eight anchored panels) and `StageCraft.Assistant.Search` (96 questions, three of them about the Problems panel).
- **Shipping UTF-16 scan:**
  - absent: `StageCraft.Problems.List`, `StageCraft.Problems.GoTo`, `StageCraft.Show.Explain`, `StageCraft.Assistant.Spike`;
  - present: PROBLEMS, "No problems found.", explain_fixture, list_problems and the dark-cause sentences.
- **Scripted run H2** (Lighting Desk layout, user data restored):
  - Setup: 3 fixtures added, cue 1 stored, one fixture deleted, fixture 1 held at 0 and patched at 1.001, fixture 2 at 1.003, Blackout on, GM at 10 %.
  - `Problems.List`: 5 findings (conflict between fixtures 1 and 2; cue 1 names fixture 5; Blackout; GM 10 %; the programmer holds 1).
  - `Show.Explain 1`: "dark: BLACKOUT is on", intensity "0 % held in the programmer". `Show.Explain 2 hy`: the same in Armenian.
  - The Problems tab shows the five rows; the docked assistant answered "why is fixture 2 dark?" and "any problems?".
  - GO TO on row 1 selected both fixtures and opened the Patch panel (floating) with them marked red.
- **Implemented, awaiting manual verification (Gevor):** click rows of Window › Problems; ask the questions in both languages and click SHOW ME.
- **Benchmark not run:** the show tick is unchanged. `ResolveFixture` builds the same layers through `BuildLayers`, and nothing is added per frame.

**Commit:** `597704b`.

**Known issues / follow-ups**
- The music's signal (AudioNoSignal) is not pushed. The list catches it on the next change, on REFRESH, or when the assistant is asked.
- Cell fixtures: the explanation follows the master dimmer, not each cell's dimmer.
- When an audio effect drives the dimmer, the flash limiter is reported as "rises are limited to N flashes a second". Whether it is holding at this instant is not read out.
- Problems rows are in English (the UI language); the assistant answers in the asker's language.
- Layouts saved before this change do not get the Problems tab; Window › Problems opens it.
- Still open from #72: the paid H0 run, the H1 mouse checks, and a packaged build reading the help articles.

## #74 — Phase H3: tool catalogue, proposal cards, plans as one undo step with rollback (2026-11-06)

**Housekeeping first.** #73 said "uncommitted"; it now carries `597704b` (Gevor committed H2).

**What & why**
- Gevor's brief (2026-11-06) asks for Phase H3 in auto mode:
  - a bounded, safe tool catalogue for the assistant (patching, fixture parameter overrides, cue modifications, DMX routing);
  - every plan or fix shown as a proposal card for review, never run silently;
  - an approved plan applied as one transaction (one Ctrl+Z);
  - 0-warning builds, all tests, benchmarks, todo.md and STATE.md.
- **Scope against ADR 0030 and todo.md:**
  - todo.md's H3 line also listed the bounded online read/act loop (read tools), append-only history replay, the cached prefix in the live backend and the API key in the credential vault. The brief does not ask for them, and they are a separate piece of work. They are now **H3b** in todo.md. Nothing in them was started.
  - The brief's "DMX channel routing" is `patch_fixtures`, which sets universe and address. Per-channel output routing does not exist in StageCraft, and none was invented.
  - ADR §6 shows a card for every plan that is not playback, selected-fixture controls or the camera. As built, **a single change typed to the local grammar still runs at once**, like pressing its button: that is today's behaviour, which 200+ tests and Gevor's offline-first decision rely on. Anyone who wants every change reviewed turns on **ASK FIRST**. Online plans and plans of several steps always show a card.

**Design (ADR 0030 §16)**
- **Tool catalogue** (`Public/AI/StageAIToolCatalogue.h`, `Private/AI/StageAIToolCatalogue.cpp`, pure):
  - one `FStageAITool` per `EStageAIIntent`, in enum order (static_assert), each with:
    - the wire name;
    - `EStageAIToolEffect` (none, fixtures, playback, cues, patch, stage, environment, files, history, marketplace);
    - `bUndoable` (the history records it);
    - `bPlannable` (it may be one step of several);
  - functions:
    - `GetTool`, `FindTool`;
    - `CanApplyWithoutAsking` (cue playback; fixture controls with the selection as target);
    - `CheckPlan` (1–32 changes; undo, redo, export and the marketplace only alone);
    - `HasStepsNotUndone`;
    - `DescribeStep` (one line per step, English and Armenian);
    - `GetNeverExposed` (ADR §2's list).
  - `StageAICommandSchema` lost its own intent-name table and reads the catalogue, so there is one table.
- **New tools** (`StageAITypes`, `StageAICommandSchema`, `StageAICommandRunner`, `StageAICommandParser`, `StageAIArmenian`):
  - `PatchFixtures {FixtureIds | selection, Universe, Address}`:
    - packed one after another by `UStageDMXPatchSubsystem::PlanConsecutive` and applied through `RequestPatchFixtures` (one step, all or nothing);
    - fixtures already patched there count as done, not as a refusal;
    - the online tool gained `fixture_ids`, `universe` and `address` (strict: whole numbers, at most 256 IDs, universe 1–63999, address 1–512).
  - `UpdateCue {CueNumber, Label?, Fade?}` through `RequestUpdateCue`; whatever the request leaves out keeps its value.
  - `DeleteCue {CueNumber}` through `RequestDeleteCue`.
  - **Grammar** (English and Armenian):
    - "patch fixture 3 to universe 2 address 101", "patch fixtures 1 to 4 at 2.001", "1-4", "1, 4 and 9", "patch the selected fixtures to universe 2";
    - "delete cue 3", "rename cue 2 to Finale", "set cue 4 fade to 2.5 seconds", "change the fade of cue 4 to 3";
    - «պատչիր 1-4 սարքերը 2 յունիվերսում», «ջնջիր կյու 3».
    - "delete a cue" without a number is still explained. "export the patch sheet", "make cue 5" and "cue 3" keep their meaning.
    - The Armenian rewrite now reads a digit-dash-digit as a range.
  - **Confirmations:** "apply" / «կիրառ…» count as yes.
- **History groups** (`UStageEditHistorySubsystem::BeginGroup / EndGroup / AbortGroup / IsGroupOpen / GetGroupCount`; `UStageEditHistoryComponent::BeginGroup / EndGroup / AbortGroup / IsGroupOpen`):
  - While a group is open, `Record` collects every step from any request. Undo and redo are refused, and `RequestHistoryStep` reports "The assistant is still applying its changes".
  - **End:** the steps are recorded as one batch named after the group, or as the single step itself, under its own name.
  - **Abort:** reverts the steps through the live item editor, newest first, all or nothing, and records nothing. If the rules refuse the revert, the steps are recorded as one step instead.
  - Component group calls end the edit transaction first. A group still open at EndPlay is recorded; a scene load (`ClearHistory`) drops it.
- **Plans** (`FStageAIPlan`, `UStageAICommandSubsystem`):
  - **Every action becomes a plan.** `HandlePlan`:
    - `CheckPlan`;
    - one non-plannable step (undo, redo, export, marketplace) → `RunAction` as before;
    - `NeedsApproval` → `ProposePlan`; otherwise `RunPlan`.
  - **`NeedsApproval`:** ASK FIRST, or (from the online model or several steps) and not every step `CanApplyWithoutAsking`.
  - **The proposal card** is an assistant line with `FStageAIMessage::ProposalId`. One proposal is open at a time.
    - `ApplyProposal(Host)` / `CancelProposal()`, or a typed yes / no (`AnswerProposal`, before the purchase offer).
    - Any other prompt drops it unapplied; ClearConversation drops it.
  - **`RunPlan`:**
    - opens the group through the host, named "Assistant: <first step> (+N more)" (English, as the Edit menu shows it);
    - runs the steps in order (`AdvancePlan`);
    - a Pending step waits for `FinishPendingAction`, which carries on the plan, and a step that finishes inside its own Execute call is caught (`EarlyOutcome`);
    - the first refused step ends the plan (`FinishPlan`): rollback through `EndPlan(false)`, and a reply naming the step, the reason, what was undone, and that cue playback stays;
    - success keeps the group and lists each step's result.
  - **`FStageAIActionHost`** (Execute, BeginPlan, EndPlan, bAskBeforeChanges) replaces the bare executor; the old `SubmitPrompt(Prompt, Executor)` overload wraps it.
  - **Online:**
    - `ParseResponseBody` keeps every `stage_command` call in `Commands`, in order;
    - one invalid call, or `max_tokens` while writing calls, fails the whole answer;
    - `disable_parallel_tool_use` is no longer sent;
    - the tool description and the system prompt explain plans and the new tools;
    - `Respond` answers questions among several calls and plans the changes.
  - **Bug fixed:** `HandleBackendFinished` called `SetState(Idle)` before `Respond`, which cleared the asking player's executor. Every online action was therefore answered "There is no player to make that change for". It now sets the state without clearing. The online path had never run with a key, so nobody saw it.
- **Controller** (`AModularPlayerController`):
  - `RequestAssistantProposal(bApply)` (BlueprintCallable);
  - `BeginAssistantPlan` / `EndAssistantPlan` (the host's group calls);
  - `Is/SetAssistantAskingBeforeChanges` (saved at once);
  - the host is built in one place (`ModularPlayerControllerAssistant::MakeHost`, weak bindings);
  - `RequestAssistantPlanForTesting` (non-Shipping).
- **Per-PC setting:** `UStageCraftUserSettings::bAssistantAsksBeforeChanges` (off).
- **UI** (`UStageAIAssistantWidget`):
  - an ASK FIRST toggle in the header (accent when on, with a tooltip explaining both modes);
  - the open card is captioned "PROPOSED CHANGES" and has APPLY (primary) and CANCEL, both disabled while busy;
  - an answered card reads "PROPOSAL (ANSWERED)";
  - a stale card's buttons do nothing (`AnswerProposal` checks the id).
- **Dev commands** (`#if !UE_BUILD_SHIPPING`, `StageAIConsoleCommands.cpp`):
  - `StageCraft.Assistant.Plan <request> | <request> ...` reads each part with the local grammar and proposes them as the online model would. The local grammar makes one change per prompt; several are H4.
  - `StageCraft.Assistant.Apply`, `StageCraft.Assistant.Cancel`.
  - Test hooks: `UStageAICommandSubsystem::SubmitPlanForTesting`, `SetBackendForTesting`, and `UStageAITestBackend` (Private/AI/Tests, a scripted interpreter that answers inside `Interpret`).

**Files changed**
- New:
  - `Source/ModularSceneBuilder/Public/AI/StageAIToolCatalogue.h`, `Private/AI/StageAIToolCatalogue.cpp`;
  - `Private/AI/Tests/StageAIPlanTests.cpp`, `Private/AI/Tests/StageAITestBackend.h`.
- Changed (under `Source/ModularSceneBuilder`):
  - `Public/AI/StageAITypes.h`, `Private/AI/StageAITypes.cpp`;
  - `Public/AI/StageAICommandSchema.h`, `Private/AI/StageAICommandSchema.cpp`;
  - `Public/AI/StageAICommandParser.h`, `Private/AI/StageAICommandParser.cpp`, `Private/AI/StageAIArmenian.cpp`;
  - `Private/AI/StageAICommandRunner.cpp`, `Public/AI/StageAICommandSubsystem.h`, `Private/AI/StageAICommandSubsystem.cpp`, `Private/AI/StageAIConsoleCommands.cpp`;
  - `Public/History/StageEditHistorySubsystem.h`, `Private/History/StageEditHistorySubsystem.cpp`, `Public/History/StageEditHistoryComponent.h`, `Private/History/StageEditHistoryComponent.cpp`;
  - `Public/Player/ModularPlayerController.h`, `Private/Player/ModularPlayerController.cpp`;
  - `Public/Settings/StageCraftUserSettings.h`, `Private/Settings/StageCraftUserSettings.cpp`;
  - `Public/UI/StageAIAssistantWidget.h`, `Private/UI/StageAIAssistantWidget.cpp`;
  - `Private/AI/Tests/StageAITests.cpp` (several calls are allowed now), `Private/AI/Tests/StageAIEditTests.cpp` ("delete cue 3" is DeleteCue now).
- Docs: `Docs/ADR/0030-...` (status line, §16), `Docs/CURRENT_SYSTEMS.md`, `tasks/todo.md`, `STATE.md` (#73 commit, #74).
- No maps, Blueprints, content assets or input actions changed.

**Verification**
- **Builds:** Editor, Game Development and Game Shipping, 0 warnings.
- **Tests:** 217/217 with no renderer and 217/217 with the offscreen renderer (`RunAutomation.ps1 -ExpectedCount 217`); user data unchanged (108 files).
  - The first run flagged `GameUserSettings.ini` as changed: the engine wrote the new `bAssistantAsksBeforeChanges=False` key once, a known pattern. Every run after it is clean.
  - New tests:
    - `StageCraft.Assistant.ToolCatalogue`:
      - every intent's entry, unique names and lookup by name, the schema's names equal to the catalogue's;
      - questions change nothing, actions are changes, and only changes are undoable or plannable;
      - no tool matches a never-exposed operation, and every change has English and Armenian lines;
      - the online tool definition carries the new fields;
      - the auto-apply rules, `CheckPlan` (empty, 33, 32, undo with a store, undo alone, a question) and `HasStepsNotUndone`.
    - `StageCraft.Assistant.PlanGrammar`:
      - the patch phrasings (one fixture, a range at U.AAA, a dashed range, a list, the selection);
      - validation refusing a 1000-fixture range and address 600;
      - the export still an export, and the cue edit phrasings;
      - store, "make cue 5" and "cue 3" unchanged, and "set cue 4" not an update;
      - Armenian delete and patch, and the new confirmations.
    - `StageCraft.History.Group`:
      - three steps become one named step: undo refused while open, then Ctrl+Z and redo take all three;
      - a group of one keeps its name, and an empty group records nothing;
      - abort reverts and records nothing; abort with nothing open; abort of an empty group;
      - a refused rollback is kept as one step;
      - `ClearHistory` drops an open group.
    - `StageCraft.Assistant.Plan`, against the real history subsystem with a scripted player:
      - a typed store runs at once as one step;
      - ASK FIRST shows a card, and "no" cancels it;
      - APPLY runs, and APPLY twice is refused;
      - a question drops the card;
      - a 3-step plan is one step named "Assistant: Store cue 8 (+2 more)", and one undo reverts it;
      - a refused third of four steps rolls back two, records nothing and says so;
      - GO plus controls of the selected fixtures apply without a card, and undo inside a plan is refused;
      - a pending step holds the plan (group open, other prompts refused) and then finishes as one step;
      - a step that finishes inside its own call works;
      - a refused pending step rolls back;
      - an Armenian card and its cancellation.
    - `StageCraft.Assistant.OnlinePlan`:
      - two tool calls parse in order; a field like "wallet", max_tokens, or a non-number ID fails the whole answer; no parallel limit on the wire;
      - with the scripted backend: an online GO runs for the player (the bug above); an online delete waits for APPLY; two calls make one card and one step; a question among changes is answered and the changes are carded; undo with a store is refused.
- **Shipping UTF-16 scan:**
  - absent: `StageCraft.Assistant.Plan`, `.Apply`, `.Cancel`, `.Ask`;
  - present: ASK FIRST, APPLY, patch_fixtures, update_cue, delete_cue, "Assistant: {0} (+{1} more)", and the undo-refused sentence.
- **Scripted run H3** (Lighting Desk layout, `RunGameScripted`, user data restored), through the real controller:
  - 4 moving heads added and cue 1 "Opening" stored (each a step of its own, run at once).
  - `Assistant.Plan` with four steps (store cue 2 "Finale", cue 1 fade 4 s, patch fixtures 1-4 from 2.001, rain) showed the card with APPLY / CANCEL in the docked assistant.
  - `Assistant.Apply` ran all four:
    - the fixtures were patched 2.001 to 2.055 (18-channel heads);
    - the history gained one step, "Assistant: Store cue 2 "Finale" (+3 more)";
    - list cues: Q1 Opening (4 s), Q2 Finale.
  - One `Edit.Undo` reverted the whole plan: cues back to Q1 Opening (2 s), and the history back to 2 steps.
  - The plan "store cue 3 | delete cue 9 | go" answered "yes" stopped at step 2 ("There is no cue 9"). Cue 3 was rolled back and no step was recorded.
  - An Armenian plan's card was cancelled («Լավ, ոչինչ չփոխեցի։»).
- **Benchmark A/B** against `597704b` (worktree `E:/SC_Baseline`, workspace copied, imports set aside and restored): no regression.
  - No show-path code changed (the only per-frame-reachable change is one `IsSet` check in `UStageEditHistorySubsystem::Record`, which runs per recorded edit, not per frame).
  - E0 (no effects), runs back to back: show tick at 200 fixtures crossfading 4.63 ms (H3) against 4.71 ms (H2), and at 500 fixtures 10.19 ms against 10.40 ms. Idle frames 11.7 / 11.9 ms and 16.9 / 16.5 ms.
  - E1 (circle and dimmer sine on every fixture), alternated H3, H2, H2, H3, after copying the main tree's `GameUserSettings.ini` into the baseline. The first E1 pairs ran on default settings in the fresh worktree and differed by up to 14 %; that was the settings, not the code.
    - 500 idle: 11.89 / 11.86 ms against 11.59 / 11.68 ms.
    - 500 crossfade: 13.51 / 14.44 ms against 13.87 / 13.68 ms.
    - 200 crossfade: 5.68 / 5.62 ms against 5.54 / 5.39 ms.
    - All within 4 % and inside the run-to-run spread.
  - Base E0 r1 timed out: the first launch of a fresh worktree compiles shaders. The worktree was removed afterwards.

- **Implemented, awaiting manual verification (Gevor):**
  - with ASK FIRST on, ask "store cue 5", press APPLY, then Ctrl+Z;
  - `StageCraft.Assistant.Plan store cue 2 | patch fixtures 1 to 4 at 2.001`, then APPLY, then one Ctrl+Z;
  - turn ASK FIRST off again (it is saved per PC).

**Commit:** `5f62474`.

**Known issues / follow-ups**
- **H3b (not started):** the bounded online read loop (read tools, 8 turns / 24 calls), append-only history replay with thinking blocks, the cached prefix in the live backend, the key in the credential vault, and cancelling a running plan from the panel.
- **Not undone:** cue playback a plan started (GO, BACK, go to cue), and a selection change. The card and the reply say so.
- **Active cue after undo:** undoing or rolling back a cue store restores the cue list, but the "cue on stage" stays the removed number ("Cue 3 is on stage" with only cue 1). This predates H3: an ordinary Ctrl+Z of a store does the same. To fix in the cue history step.
- **Edits during a waiting step:** while a plan waits for fixtures to stream, the player's own edits are collected into the plan's step. Undo is refused during it, so nothing is lost.
- **Remaining tools:** apply_palette, start/stop_effect, select, record_group and the offline multi-step grammar are H4.
- **Proposal card:** an online plan that is entirely cue playback or controls of the selected fixtures runs without one, by design (ADR §6).
- **Still open from #72 / #73:** the paid H0 run, the H1 and H2 mouse checks, and a packaged build reading the help articles.

## #75 — Phase H4: offline multi-step plans, conversation memory, palette / group / effect tools (2026-11-06)

**Housekeeping first.** #74 said "uncommitted"; it now carries `5f62474` (Gevor committed H3).

**What & why**
- Gevor's brief (2026-11-06) asks for Phase H4 in auto mode:
  - the offline grammar reads compound prompts in English and Armenian into one proposal card ("add 4 pars and store cue 1");
  - short-term conversation memory for follow-ups ("now set their colour to blue");
  - the remaining tools for palettes, groups and effect settings, inside H3's one-undo-step plans;
  - 0-warning Editor / Game / Shipping builds, 217+ tests with no regression, todo.md and STATE.md.
- **Scope:**
  - Done: ADR 0030 §6's offline items, and Phase 26's multi-step and memory lines.
  - Still open from Phase 26: BOM answers and export completion in chat.
  - Still open from ADR §6: H3b (the online read loop, replay, the vault key).

**Design (ADR 0030 §17)**
- **Multi-step** (`StageAICommandParser::ParsePlan`, pure, public):
  - **Splitting:** `SplitClauses` cuts the raw prompt at the words and / then (Armenian և, ու, հետո, ապա, իսկ) and after a word ending in , ; . ! or ։, and at line breaks. Never inside "" “” «». "2.001" is not a full stop.
  - **Reading:** each part goes through `Parse` (Armenian through `StageAIArmenian`), and every step speaks the whole prompt's language.
  - **Joining:**
    - A part that is no plannable change on its own joins its neighbour, so "delete the spots and the washes" stays one delete.
    - A SetFixture without its own target joins the SetFixture before it, so "set the spots to red and zoom wide" is one step.
  - **Continuing:** a later fixture-tool step without a target, after a step that acted on fixtures (a fixture tool or an add), gets `Target = Recent`.
  - **Result:** fewer than two steps, or more than 48 parts, means an empty result, and `Parse` reads the prompt as before.
  - **In the subsystem:** `UStageAICommandSubsystem::InterpretLocally` tries `ParsePlan` first (never for how-to questions) and hands the steps on as `MoreCommands`. H3's `Respond` / `HandlePlan` then validate, check, propose or run them, as one undo step with rollback.
- **Targets** (`EStageAITarget` gains `Fixtures`, `Group`, `Recent`; `FStageAICommand` gains `Group`, `Palette`, `PaletteKind`, `Effect`, `RateHz`, `EffectSize`; `FixtureIds` is reused):
  - **IDs and groups:** the parser's `ReadFixtureRefs` reads them:
    - "fixture 3", "fixtures 1 to 4", "lights 1, 2 and 5";
    - IDs before the word, as Armenian puts them («1-4 սարքերը»);
    - "group 2", "group front wash", and Armenian "2 group".
    - A range end that carries a unit is a value, not an ID ("fixture 3 to 50%").
  - **SetFixture** reads these first, so no control takes their numbers.
  - **Recent** is what "them / they / those / it" means; "this / these / the selected" stay the selection.
  - **`ValidateTarget`** (schema):
    - IDs must be 1..200 and at least 1; a group must be named.
    - Fixtures, Group and Recent belong to the fixture tools only (catalogue `bTargetsFixtures`), and are refused on delete / move / replace.
  - **Runner:** `FindTargets` resolves Fixtures and Group:
    - through `UShowControlSubsystem::FindFixture`;
    - a group through `UStageShowLibrarySubsystem` and `ResolveName`.
- **Memory** (subsystem, private `FConversationMemory`: `FixtureIds`, `SelectionAfter`, `LastChange`):
  - **What is remembered:** `FStageAIActionOutcome::AffectedFixtureIds`.
    - The runner fills it for add, set, select, patch, palettes, groups and effects.
    - For an add, it is the placed fixtures that were not there before `RequestBuildPlan`.
  - **When:**
    - `RememberStep` runs after each succeeded plan step.
    - `ResolveRecent` runs just before each step, so step 2 of a plan sees step 1's fixtures.
  - **"Them" falls back to the selection** when nothing is remembered, or when the player's selected fixtures differ from those right after the last change.
    - The selection comes from the new `FStageAIActionHost::SelectedFixtures`, bound in `ModularPlayerControllerAssistant::MakeHost`.
  - **"Again":**
    - `EStageAIIntent::Repeat` is local-only (catalogue `bLocalOnly`). It is left out of the online enum, and the schema refuses it from the model.
    - `ResolveRepeat` swaps it for `LastChange` before validation; only a fixture tool takes a new target.
  - **Lifetime:**
    - A rolled-back plan restores the memory it found (`FRunningPlan::MemoryBefore`).
    - `ClearConversation` and `Deinitialize` empty it.
    - It is never saved.
- **Nine new tools** (catalogue entries; schema fields `group`, `palette`, `palette_kind`, `effect`, `rate_hz`, `effect_size`; runner; grammar EN/HY):

  | Tool | Effect | Undoable | Request(s) |
  |---|---|---|---|
  | `select_fixtures` | Selection (new) | no | `RequestSelectItems` |
  | `apply_palette` | Fixtures | yes | `RequestApplyPalette` |
  | `record_palette` | Library (new) | yes | `RequestRecordPalette` (Global) |
  | `delete_palette` | Library | yes | `RequestDeletePalette` |
  | `record_group` | Library | yes | `RequestRecordGroup` |
  | `delete_group` | Library | yes | `RequestDeleteGroup` |
  | `start_effect` | Fixtures | yes | `RequestStartEffect` (a quick effect by name, rate and size) |
  | `update_effect` | Fixtures | yes | `RequestUpdateEffect` on the programmer's running effects |
  | `stop_effect` | Fixtures | yes | `RequestStopEffect` (by name, or STOP ALL) |

  - Selection tools select their target first: a selection is tool state, not an undo step.
  - Pools are named through `StageAICommandParser::ResolveName`: the whole name, else the one name holding every word, else the place in the pool. Several matches are listed back.
  - "Chase" means the Dimmer Chase (first of several, for effects only).
  - An effect size is in degrees for position effects; for the others it is the wave's height above its bottom (0..1).
  - Without the word "effect", only the quick effects' own words name one, so "stop the strobe" stays the strobe control and "add a circle truss" stays a shape.
- **Applies without asking:**
  - now also a selection, and palettes and effects on the selection or "them";
  - `HasStepsNotUndone` ignores selections.
- **Help** gained the new commands in English and Armenian (54 Armenian examples, all parsed by `StageCraft.AI.Armenian`).

**Files changed**
- Public: `AI/StageAITypes.h`, `AI/StageAIToolCatalogue.h`, `AI/StageAICommandParser.h`, `AI/StageAICommandSubsystem.h`.
- Private:
  - `AI/StageAITypes.cpp`, `AI/StageAIToolCatalogue.cpp`, `AI/StageAICommandSchema.cpp`, `AI/StageAICommandParser.cpp`;
  - `AI/StageAIArmenian.cpp`, `AI/StageAICommandRunner.cpp`, `AI/StageAICommandSubsystem.cpp`;
  - `Player/ModularPlayerController.cpp`.
- Tests:
  - new: `AI/Tests/StageAIMultiStepTests.cpp`;
  - changed: `AI/Tests/StageAIPlanTests.cpp`, `StageAITests.cpp`, `StageAIFixtureTests.cpp`, `StageAIArmenianTests.cpp`.
- Docs:
  - `Docs/ADR/0030` (status line, §17), `Docs/CURRENT_SYSTEMS.md` (new row, 221 tests);
  - `tasks/todo.md` (H4 a–e, the manual item, follow-ups; Phase 26 lines);
  - STATE.md (#74 hash, #75).
- No content assets, maps or input actions.

**Verification**
- **Builds:** Editor, Game Development and Game Shipping, all 0 warnings.
- **Shipping UTF-16 scan:**
  - absent: `StageCraft.Assistant.Plan`, `.Ask`, `.Apply`;
  - present: the new tool names and replies.
- **Tests:** 221/221 without a renderer and 221/221 with the offscreen renderer. User data unchanged (116 files).
  - **New:**
    - `StageCraft.Assistant.MultiStep`: splitting, joining, continuing, quotes, questions, undo, Armenian, the clause cap.
    - `StageCraft.Assistant.ToolGrammar`: selection, ID and group targets, them, palettes, groups, effects, again, Armenian, `ResolveName`.
    - `StageCraft.Assistant.ToolSchema`: what applies without asking, `repeat` local-only, the new fields and refusals, card lines.
    - `StageCraft.Assistant.Memory`:
      - them before any memory;
      - "add 4 pars and make them blue" as one card, one undo step, blue on the added IDs;
      - them across prompts, and a new selection winning;
      - again on a group, and again as it was;
      - an Armenian card cancelled;
      - again refused for a cue;
      - a rolled-back plan restoring memory;
      - Clear forgetting.
  - **Three earlier assertions updated to the intended behaviour:**
    - "make them green" is now `Recent`, which resolves to the selection when nothing is remembered;
    - the online enum leaves out the local-only `repeat`;
    - the Armenian help has 54 examples.
  - **One bug found by the tests and fixed:** "do it again …" starts with "do", which the grammar counts as the start of a question; Repeat now treats only "?" as a question.
- **Scripted run H4** (`-game`, without user imports, user data restored; log and shots in `C:\Users\gevor\AppData\Local\Temp\StageCraftRuns\H4`):
  - "add 4 fixtures and make them blue": a card (screenshot shows APPLY / CANCEL), yes, both steps as one undo step "Assistant: Add 4 fixtures (+1 more)", blue on the added beams.
  - "select fixtures 1 to 2 and record a group called Front": a card, then applied.
  - "record a colour palette called Ocean", then "make them red" (on the same two fixtures).
  - "apply palette ocean to group front", "start a circle effect on group front at 0.5 hz", "effect speed 1 hz".
  - "make fixtures 3 to 4 green", then "do it again on group front" (green on Front).
  - "stop all effects", undone by Ctrl+Z.
  - "delete palette ocean", "delete group front".
  - «ընտրիր 1-4 սարքերը և դարձրու դրանք կարմիր»: two steps applied at once (selection and them).
  - No refusals in the transcript.
- **Benchmark:** not run. H4 changes no show tick, mix or render path; the parser and memory run once per prompt.
- **Awaiting manual verification (Gevor):** see todo.md, "H4 manual verification".

**Commit:** `76d464a`.

**Known issues / follow-ups**
- Not offered:
  - adding fixtures to or removing them from an existing group ("remove the spots from group 2" is deliberately not read as deleting the group);
  - renaming palettes and groups;
  - per-fixture palettes;
  - effect settings other than rate and size;
  - "make it faster" without a number.
- "Select the spots and washes" finds nothing: a name query needs every word in one item's name (as for deletes).
- FindLabel takes every word after "called" up to fade / with / in / and, so "create a group called Front from the spots" names it "Front from the spots"; say the target first.
- From #74, still open: the "cue on stage" goes stale after undoing a store; H3b.

## #76 — Phase I: stage people (audience, band, crew), realistic props and set, no UI sounds, clean starter stages (2026-11-06)

**Housekeeping first:** #75 now carries `76d464a`, since Gevor committed H4.

**What & why**
- Gevor's brief (2026-11-06, auto mode) asks for Phase I:
  - realistic stage elements with instancing and LODs;
  - a scalable animated crowd (VAT / GPU instancing, 120+ FPS aim);
  - animated band members and crew;
  - a zero-regression benchmark gate;
  - 0 warnings on every target.
- The same brief asks for two UI overrides:
  - no extra windows or panels when a default stage is picked;
  - no UI action sounds.
- **Art source:** the roadmap's I0 asked Gevor to choose one (MetaHuman, Fab or commissioned), and the project has no character art. The brief asked for "visual proxies", so Phase I builds procedural stand-ins on a contract real art can fill later (ADR 0031 §3.4).
- **Design:** `Docs/ADR/0031-stage-people-and-realism.md` (new, awaiting Gevor's approval).

**UI overrides**
- **No interface sounds.** Removed:
  - `UStageEditorAudioFeedbackComponent` (selection, placement, snap and refusal cues; from #23) and its controller subobject;
  - the cue table (`UStageAudioDeveloperSettings`, its `DefaultGame.ini` section and the `/Game/StageCraft/Audio` cook entry);
  - `PlayCue` and the cue loading in `UStageAudioSubsystem`;
  - the `StageCraft.Sound.*` tags and `StageAudioMath::ComputeCueGain`;
  - the Effects volume (user setting, Preferences slider, Audio menu slider);
  - the dev commands `StageCraft.Audio.Effects` and `.Play`;
  - the four `SFX_*` assets and `make_feedback_wavs.ps1`.

  Master volume and Mute stay; they now govern only the music the show follows. A leftover `EffectsVolume` line in a user's ini is ignored.
- **Starter stages open on their own.** New `UStageWorkspaceSubsystem::PresentStarterStage`, called by `AModularPlayerController::HandleSceneOperationFinished` on every successful OpenTemplate (welcome card, File > New from Template, assistant, console). It:
  - clears the pending welcome timer;
  - closes the welcome screen;
  - closes every panel in a floating window.

  Docked panels stay, and nothing opens.
  - **Cause found in a scripted run:** Gevor's restored layout reopens 7 floating panel windows (Cues, Effects, Groups, Palettes, Patch, Playback, Sketch) behind the welcome screen, so they appeared as a stage was picked.
  - Also, the console route left the welcome screen open.

**Phase I (ADR 0031)**
- **Figure** (`People/StageFigureRig`, pure):
  - an 11-bone rigid humanoid, 175 cm, built in code in 3 LODs (348 / 168 / 72 triangles);
  - UV1 holds the bone and the body region;
  - 10 looping clips (crowd idle, bounce, cheer and clap; drummer seated; guitarist; vocalist; keyboardist; desk operator; stagehand), solved by forward kinematics;
  - the bounce keeps the feet planted and the clap aims the hands together;
  - baked into a 22 x 320 RGBA32F bone animation texture (rotation and translation per bone per frame, hemisphere-consistent).
- **`UStageFigureAssetSubsystem`** (an engine subsystem, so the export and editor worlds have it too):
  - builds the runtime static mesh (LOD screen sizes 1 / 0.16 / 0.05, extended bounds, simple box collision used as complex) and the texture once;
  - hands out one dynamic material instance per figure material;
  - 8 custom data floats per instance.
- **`M_StageFigure`** (content, `Scripts/Content/create_people_content.py`): world position offset from two blended texture frames; the normal turned in the vertex shader and passed as a world normal; colour by region from the shirt RGB, skin tone and trouser shade.
- **Audience area** (`AStageAudienceActor`, `UStageAudienceData`, `People/StageAudienceMath`):
  - width, depth, density and mood are SaveGame values plus a seed; they are sanitized and counted on restore, and are undoable shape parameters (`IsShapeParameter`). People is read-only.
  - **Fill:** a deterministic jittered grid (nobody overlaps), clips from the mood mix, random phase, speed, height, turn and clothes.
  - **Drawing:** one HISM with no shadows, distance fields or ray tracing, culled at 300 m.
  - **Picking:** a 2 cm floor box answers only the stage item channel, so clicking the crowd selects it while placement, camera pivots and fixtures beyond are untouched.
- **Props** (`AStagePropActor`, `UStagePropData`, `FStageFigurePlacement`):
  - parts through the new `UStageSpanGeometryComponent::BuildParts` (one instanced batch per mesh and material);
  - figures in one ISM that answers only the stage item channel;
  - the overlay is drawn on the parts only.
- **Shared helper:** `People/StageFigureActors` (configure the component, show the people, make the custom data).
- **Catalog** (13 items, `create_people_content.py`; engine cubes and cylinders, 12 `M_Prop*` materials):
  - `DA_Audience_Standing`;
  - band: drummer with kit, guitarist, bassist, vocalist, keyboardist;
  - crew: FOH lighting position, FOH sound position, monitor position, stagehand with a road case;
  - `DA_Backline_AmpStack` and `DA_Set_LEDBackdrop` (8 x 4.5 m);
  - `DA_Span_Barricade` (span, whole 1 m sections).

  New categories `People` (`.Audience`, `.Band`, `.Crew`), `Prop.Backline` and `Stage.Set`. New tags `Param_Audience_*` and `FeatureGroup_Audience`.
- **Concert starter stage:** its build script now places a dancing crowd (16 x 10 m, 320 people), the band, two amp stacks, the LED wall, the FOH desks and crew. `build_starter_stages.ps1` reran, now `-WithoutUserImports`.
  - Concert has 26 items.
  - All four templates were regenerated, so the three unchanged ones moved from scene format v5 to v11 with the current fixture fields.
- **Build plans** gained `Items` (any catalog item at a transform, placed last, rolled back with the rest), which `StageCraft.Edit.PlaceAt` uses.
- **New dev commands** (`#if !UE_BUILD_SHIPPING`):
  - `StageCraft.Edit.PlaceAt <Item> <x> <y> [z] [yaw]` (metres; one undo step through `RequestBuildPlan`);
  - `StageCraft.View.Camera <x> <y> <z> <pitch> <yaw>`;
  - `StageCraft.People.Status` (areas, people, figures, and the figure material's compile errors in editor builds);
  - `StageCraft.Bench.Fixtures ... Crowd=N` (an audience of N people under the bench fixtures; CSV column Crowd).

  `RunFixtureBench.ps1` gained `-Crowd` and `-Camera`.
- **Latent link bug fixed:** `FStageSceneExporter::GetTempPath` collided with the Windows `GetTempPath` macro (`GetTempPathW`). A fresh worktree of `76d464a` failed to link; the main tree's unity grouping happened to hide it. Renamed to `GetExportTempPath`.

**Files changed**
- New, `Public/` and `Private/`:
  - `People/` (StageFigureRig, StageFigureAssetSubsystem, StageAudienceMath, StageFigureActors);
  - `Actors/StageAudienceActor`, `Actors/StagePropActor`;
  - `Data/StageAudienceData`, `Data/StagePropData`;
  - `Private/People/StagePeopleConsoleCommands.cpp`;
  - `Private/People/Tests/StagePeopleTests.cpp`.
- Changed:
  - `Audio/StageAudioSubsystem.h/.cpp`, `Audio/StageAudioTypes.h`, `Audio/StageAudioConsoleCommands.cpp`;
  - `Settings/StageCraftUserSettings.h/.cpp`, `UI/StagePreferencesPanel.cpp`, `Workspace/SStageWorkspaceMenuBar.cpp`;
  - `Player/ModularPlayerController.h/.cpp`, `Workspace/StageWorkspaceSubsystem.h/.cpp`;
  - `Components/StageSpanGeometryComponent.h/.cpp`;
  - `Data/StageItemTypes.h/.cpp`, `Data/StageParameterTypes.h/.cpp`;
  - `Sketch/StageSketchTypes.h`, `Sketch/StageSketchBuilder.h/.cpp`;
  - `Placement/StagePlacementConsoleCommands.cpp`, `Viewport/StageViewportConsoleCommands.cpp`, `Diagnostics/StageFixtureBenchmark.cpp`;
  - `Bridge/StageSceneExporter.h/.cpp`, `Bridge/StageSceneExportJob.cpp`, `Bridge/Tests/StageBridgeLoopbackTests.cpp`;
  - `Placement/Tests/StagePlacementTests.cpp`;
  - `ModularSceneBuilder.Build.cs` (comment only).
- Deleted:
  - `Audio/StageEditorAudioFeedbackComponent.h/.cpp`, `Audio/StageAudioDeveloperSettings.h`, `Audio/StageAudioTypes.cpp`;
  - `Content/StageCraft/Audio/Feedback/SFX_*.uasset` (4);
  - `Scripts/Content/make_feedback_wavs.ps1`.
- Content:
  - `Content/StageCraft/People/M_StageFigure` and 12 `M_Prop*`;
  - `Content/StageCraft/Items/DA_Audience_Standing`, `DA_Band_*` (5), `DA_Crew_*` (4), `DA_Backline_AmpStack`, `DA_Set_LEDBackdrop`, `DA_Span_Barricade`;
  - `Content/StageCraft/Templates/*.json` (4).
- Config: `DefaultGame.ini` (cue section and audio cook entry removed).
- Scripts:
  - new `Scripts/Content/create_people_content.py`;
  - changed `build_starter_stages.ps1`, `create_placement_audio_content.py`, `Scripts/Bench/RunFixtureBench.ps1`.
- Docs: `Docs/ADR/0031` (new), `Docs/CURRENT_SYSTEMS.md` (Phase I row, controller components, 226 tests), `tasks/todo.md`, STATE.md (#75 hash, #76).
- No maps or input actions.

**Verification**
- **Builds:** Editor, Game Development and Game Shipping, all 0 warnings.
- **Shipping UTF-16 scan:**
  - absent: `StageCraft.Edit.PlaceAt`, `StageCraft.View.Camera`, `StageCraft.People.Status`, `StageCraft.Bench.Fixtures`, `StageCraft.Audio.Effects`, `StageCraft.Audio.Play`;
  - present: the audience actor's text namespace and moods.
- **Tests:** 226/226 without a renderer and 226/226 with the offscreen renderer. User data unchanged.
  - **New:**
    - `StageCraft.People.FigureRig`: rest pose, LOD validity and winding, height, loops, smooth frames, feet planted, cheer hands above the head, drummer seated with the feet on the pedals, bounce, clap, texture layout and hemispheres.
    - `StageCraft.People.AudienceMath`: counts, caps, invalid input, mood shares, determinism, bounds, facing, no overlap.
    - `StageCraft.People.FigureAssets`: mesh LODs and screen sizes, bounds, texture, shared instances, custom data.
    - `StageCraft.People.Items`: area defaults, parameters, clamps, refusals, undoable tags, save and restore of the same crowd, 4 counted repairs, prop parts and figures, collision channels, missing material.
    - `StageCraft.People.NoUISounds`.
  - The every-catalog-item glTF export test (`StageCraft.Bridge.ExportCompleteness`) now covers the 13 new items, people included (rest pose).
  - `StageCraft.Audio.Math` and `.UserSettings` lost their cue and effects checks.
- **Scripted runs** (`-game`, user data restored; `C:\Users\gevor\AppData\Local\Temp\StageCraftRuns\I0a` to `I1f`):
  - **I0a** found the 7 floating windows and the welcome screen left open.
  - **I1** logged "welcome closed, 7 floating panel(s) closed".
  - **I1b and I1c** found `M_StageFigure` failing to compile:
    - first, the Linear Color sampler had an sRGB default texture, so it now uses `BaseFlattenLinearColor`;
    - then, the Transform nodes had no input, because the pin is `""` and not `"Input"`.
    - `StageCraft.People.Status` printed the errors.
  - **I1d:** crowd, band and crew rendered and animated (screenshots).
  - **I1e:** the regenerated Concert from a normal start: welcome closed, 7 floating windows closed, 26 items, 320 people, 9 figures, material without errors.
  - **I1f:** the LED wall's glow was reduced after it blew out at night exposure.
- **Benchmark, no-regression gate** (`RunFixtureBench -Effects 1`, A/B against a `76d464a` worktree with Gevor's workspace copied in and his model imports set aside):

  | | 200 fixtures idle | 200 crossfade | 500 idle | 500 crossfade |
  |---|---|---|---|---|
  | Show tick, baseline | 4.45 ms | 4.83 ms | 11.37 ms | 13.46 ms |
  | Show tick, now | 4.18 ms | 4.95 ms | 10.72 ms | 12.74 ms |
  | Frame mean, baseline | 13.40 ms | 13.47 ms | 24.86 ms | 26.80 ms |
  | Frame mean, now | 12.42 ms | 13.07 ms | 23.55 ms | 25.16 ms |
  | GPU, baseline | 8.52 ms | 8.69 ms | 11.05 ms | 11.79 ms |
  | GPU, now | 8.33 ms | 8.58 ms | 10.92 ms | 11.56 ms |

  No regression, within run noise.
- **Crowd cost** (200 fixtures, Effects 1, camera at -40 0 12 looking at the crowd), idle GPU and frame:

  | People | GPU | Frame |
  |---|---|---|
  | 0 | 8.19 ms | 12.68 ms |
  | 1,000 | 8.13 ms | 12.59 ms |
  | 5,000 | 8.34 ms | 12.57 ms |
  | 17,888 | 9.23 ms | 12.56 ms |

  (A request for 20,000 was capped by the item's 80 m width.) The crowd costs no CPU (game thread and show tick flat). The GPU cost is about +1 ms at 18k people on this PC.
- **Awaiting manual verification (Gevor):** see todo.md, "Phase I manual verification".

**Commit:** `8f1a5ed`.

**Known issues / follow-ups**
- Not done:
  - crowd energy and band tempo from the music (ADR 0029 features);
  - audience polygon outlines;
  - capacity in the reports and the bill of materials;
  - a placement ghost for prop and audience items (decks have none either);
  - real character art (MetaHuman, Fab or commissioned) through the ADR 0031 §3.4 contract;
  - I3 (haze, starter stages v2) and I4 (packaged validation per template).
- The starter-stage camera now frames the whole Concert stage including the FOH desks 21 m out, so the opening view is wider than before.
- The hover and selection overlay shows on prop parts only, never on the people.
- The figures are stylised box people: convincing at crowd distance, clearly proxies close up.

## #77 — Phase J: real-money marketplace on StageCraft Cloud, secure checkout and payouts, one-click purchase, Vision Inspector (photo → BOM), support and bug reports, admin backend (2026-11-07)

**Housekeeping first:** #76 now carries `8f1a5ed`, since Gevor committed Phase I.

**What & why**
- Gevor's brief (2026-11-07, auto mode) asks for Phase J:
  - a real-money marketplace (15 % platform, 85 % seller) on AWS / Kubernetes;
  - hosted checkout settled only by server webhooks; card data never reaches StageCraft;
  - a ledger, orders, refunds, disputes and seller balances;
  - in-app browsing, buying and publishing with one-click purchase and instant import;
  - an AI image-to-BOM inspector;
  - an in-app support center and bug report;
  - admin endpoints;
  - 0 warnings on every target, tests, todo.md and STATE.md.
- Architecture, contract and open decisions: **ADR 0032** (new). Its §12 records the as-built details.
- **Decisions only Gevor can make (ADR 0032 §2, D1–D8).**
  - Stripe cannot open a platform account in Armenia. Global Payouts (Armenia, AMD) can be sent only from a US or UK platform (docs.stripe.com/global-payouts/recipient-countries). Selling therefore needs a US or UK entity.
  - Lemon Squeezy and Paddle have no multi-seller payouts.
  - Also open: tax, who pays the card fee (assumed: the platform, out of its 15 %), the hold period (assumed 14 days), the identity provider, AWS, and the refund policy.
- **Nothing moves real money until those decisions are made and the service is deployed with live keys.**

**StageCraft Cloud service (`Services/StageCraftCloud`, new)**
- Built by a subagent from the ADR contract and reviewed by me.
- **Stack:** Node 22 and TypeScript, run with type stripping. The only runtime dependency is `pg`. Hand-written HTTP; Stripe over plain REST.
- **Money.**
  - The ledger is double-entry and balanced per currency; balances are sums of journal lines only. Amounts are bigint minor units. The split is `seller = floor(P * 8500 / 10000)`.
  - Refunds and disputes are apportioned with caps.
  - Settlement happens only through Stripe-signed webhooks: HMAC, 300 s tolerance, event-id idempotency, one database transaction. An amount mismatch puts the order `OnHold`.
  - Orders use a pure state machine; the client's TransactionId is the idempotency key.
  - Payouts after the hold period go by Stripe transfer (US, UK, EEA, CA, CH) or as manual batches (Armenia until a Global Payouts adapter exists).
- **Listings.** The server re-reads `.stage` packages with its own port of `StagePackage::Read`. The author comes from the account, and an admin approves before a listing is shown. Entitlement-checked downloads come from a short-lived address (S3 SigV4 or local).
- **Accounts.** RFC 8628 device sign-in through OIDC, with PKCE and an RS256 check against the JWKS. Session tokens are stored only as a SHA-256 hash. The admin role is a database row.
- **Support tickets.** Redacted, with an idempotent `clientReportId`.
- **Admin.** Summary, orders, refund, payouts (run and mark paid), moderation and support, each mutation with an audit log.
- **Deployment.** Rate limits, security headers, Dockerfile, Kubernetes manifests (Deployment, Ingress for AWS Load Balancer Controller, HPA, PDB, NetworkPolicy, ExternalSecret, migration Job), `docs/RUNBOOK.md`.
- Production refuses to start with dev settings.

**Client (UE)**
- **`UStageCloudSubsystem`.**
  - `BaseUrl` in config; empty means offline, the default.
  - Device sign-in.
  - The session lives in the credential vault, bound to its server address.
  - One send path: `/v1/` paths only, a 401 ends the session, downloads carry no session.
  - A test transport for tests.
  - Dev-only `StageCraft.Cloud.Status / SignIn / SignOut / LogBrowserUrls`.
- **`StageCloudJson`.** The pure wire format. Every answer is validated and capped; unknown error codes deny; only safe addresses are opened.
- **`UStageCloudMarketplaceBackend`.** Uses the cloud while configured, otherwise the community folder.
- **`UStageCloudCommerceBackend`.**
  - Real-money listings: order, Stripe hosted checkout in the browser, long polls, approve only when the server reports paid.
  - Already owned on the server means approved, with no charge.
  - Cancel answers declined, once.
  - Credits stay local.
- **Commerce backend contract.** `GetSettlementTimeoutSeconds` (checkout lifetime instead of 30 s, clamped to 1 s..1 h) and `CancelPurchase`.
- **New requests.** `UStageEconomySubsystem::RequestCancelListingPurchase`, `AModularPlayerController::RequestMarketplaceCancelPayment`.
- **Config.** `DefaultGame.ini` uses both cloud backends. With an empty `BaseUrl` they behave exactly like the local ones.
- **Marketplace panel.**
  - Account bar: SIGN IN, the code while signing in, SIGN OUT.
  - Money prices ("$15.00").
  - One-click BUY opens the checkout and turns into PAYING IN BROWSER · CANCEL; the stage opens when paid, and a stage with unsaved changes still asks.
  - New result `SignInRequired`.
- **Vision Inspector** (`StageVisionBom`, `UStageVisionBomSubsystem`, `UStageClaudeVisionBomBackend`, `UStageVisionInspectorPanel`).
  - A photo is resized to 1568 px and read by Claude with the strict `stage_inventory` tool: 24 kinds, counts, confidence, sizes and coarse positions.
  - The answer is validated (case-sensitive names, caps, unknown kinds dropped) and matched to the catalog by category and keyword, required where the category alone would be wrong.
  - Real-world specs: truss runs in 3 / 2 / 1 / 0.5 m sections, LED walls in 500 mm panels, decks in 2 x 1 m.
  - Weight and power totals; CSV with formula-injection protection.
  - **Build in Scene** goes through the sketch generator (deck, truss) plus matched items laid out by region, as one undo step via `RequestBuildFromVisionBom`.
  - Lines under 50 % confidence are marked CHECK.
- **Fix:** `UStageSketchSubsystem::RequestPlanAssets` now also streams a plan's `Items`.
- **Support** (`StageSupportReport`, `UStageSupportSubsystem`, `UStageSupportPanel`).
  - Redaction: API, Stripe and webhook keys, Bearer and x-api-key values, the cloud session, e-mail addresses and the Windows user name in paths.
  - Opt-in log tail (256 KiB) and window screenshot (<= 4 MiB, halved until it fits).
  - Sent to the cloud, or saved to `Saved/StageCraft/Support/<time>_<id>/`; a failed send is saved too.
- **Panels:** tags `StageCraft.Panel.VisionInspector` and `.Support`, menu entries (Edit › Draw, Community), `DA_Panel_*` from `Scripts/Content/create_phase_j_content.py`.

**Files changed**
- **New (service):** the whole of `Services/StageCraftCloud/`:
  - `package.json` and lock, `tsconfig.json`, `.env.example`, `Dockerfile`, `.dockerignore`, `README.md`;
  - `docs/RUNBOOK.md`, `deploy/k8s/*`, `migrations/0001_init.sql`;
  - `scripts/test-pg.ts`, `scripts/make-dev-package.ts`;
  - `src/**` and `test/**` (12 tests, helpers, one C++-written fixture package).
- **New (client):**
  - `Public|Private/Cloud/StageCloudTypes.h`, `StageCloudJson`, `StageCloudSubsystem`, `StageCloudMarketplaceBackend`, `StageCloudCommerceBackend`, `Private/Cloud/StageCloudConsoleCommands.cpp`, `Private/Cloud/Tests/StageCloudTests.cpp`;
  - `Public|Private/Vision/StageVisionBom`, `StageVisionBomBackend`, `StageVisionBomSubsystem`, `Private/Vision/Tests/StageTestVisionBomBackend.h/.cpp`, `StageVisionBomTests.cpp`;
  - `Public|Private/Support/StageSupportReport`, `StageSupportSubsystem`, `Private/Support/Tests/StageSupportReportTests.cpp`;
  - `Public|Private/UI/StageVisionInspectorPanel`, `StageSupportPanel`.
- **New (other):**
  - `Content/StageCraft/UI/Panels/DA_Panel_VisionInspector.uasset`, `DA_Panel_Support.uasset`;
  - `Scripts/Content/create_phase_j_content.py`, `Scripts/Tests/RunCloudEndToEnd.ps1`;
  - `Docs/ADR/0032-real-money-marketplace-cloud-and-stage-inspector.md`.
- **Changed:**
  - `Public/Economy/StageCommerceBackend.h`;
  - `Public|Private` `StageEconomySubsystem`;
  - `Public/Marketplace/StageMarketplaceTypes.h` (`SignInRequired`), `Private/Marketplace/StageMarketplaceSubsystem.cpp`;
  - `Public|Private` `ModularPlayerController`;
  - `Public|Private/UI/StageMarketplaceWidget`;
  - `Private/Sketch/StageSketchSubsystem.cpp`;
  - `Public|Private/Workspace/StageWorkspaceTypes`, `Private/Workspace/StageWorkspaceShell.cpp`, `SStageWorkspaceMenuBar.cpp`;
  - `Config/DefaultGame.ini`, `.gitignore`, `tasks/todo.md`, STATE.md.

**Verification**
- **Server:**
  - `npm test` 71/71 (I re-ran it). `npm run test:pg` against `postgres:16-alpine` in Docker also passes.
  - Line coverage: 100 % for money, ledger, orders, webhook signature and processing, package reader, redaction, query and support; 96.9 % overall.
  - `docker build` succeeds and the container runs healthy with a read-only root filesystem.
  - `kubectl kustomize` parses the manifests. There is no cluster for schema validation.
- **Client tests:** `RunAutomation.ps1 -ExpectedCount 240 -NullRHI` gives 240/240, user data unchanged.
  - New: StageCraft.Vision.Request / Parse / Spec / Match / Layout / Flow, StageCraft.Support.Validate / Redact / Format / Submit, StageCraft.Cloud.Json / SignIn / Marketplace / Checkout.
  - Bugs the tests caught and I fixed: `FString ==` is case-insensitive, so wire names now match case-sensitively; `AsObject` on a non-object logged errors.
- **End to end:** `Scripts/Tests/RunCloudEndToEnd.ps1` passed 13/13. It runs the real game against the real dev server:
  - the seller publishes, the admin approves;
  - the game signs in through the device flow;
  - one-click purchase of a $15.00 listing;
  - the fake checkout pays and sends a signed webhook;
  - the game sees it paid, downloads and imports ("Added to your scenes");
  - the server books 1500 gross, 1275 seller, 225 platform;
  - no session token in the log; sign-out.
- **Scripted `-game` screenshot J10:** the Marketplace account bar, Vision Inspector and Support panel render as designed (window capture only).
- **Builds and Shipping scan:** Editor, Game Development and Game Shipping build with 0 warnings. The Shipping exe (UTF-16 scan) does not contain `StageCraft.Cloud.Status`, `.SignIn` or `.LogBrowserUrls`, nor the older dev commands. It does contain `StageCloudSubsystem`, `StageCloudCommerceBackend`, `StageVisionInspectorPanel`, `StageSupportPanel`, `stage_inventory` and `/v1/orders`.
- **Not verified:**
  - real Stripe test mode, AWS and an identity provider (D1–D8);
  - a paid Claude reading of a real stage photo (needs Gevor's key; the parser is tested with recorded answers);
  - clicking through the panels by mouse;
  - Shipping packaging.

**Commit:** uncommitted (working tree).

**Known issues / follow-ups**
- **A scripted `-game` run stalls when a panel window is created mid-run.**
  - Any `StageCraft.Workspace.OpenPanel` or `Workspace.Reset` step stops the core ticker, so later Delay steps never run. The game keeps responding and drawing.
  - It reproduces identically on a fresh build of `8f1a5ed` (Phase I), so it is not caused by Phase J. The likely cause is the floating or new window creation on this multi-monitor setup (the main window sits at X 3840).
  - Workaround used: put panels into the saved layout before the run.
  - Needs Gevor to check whether opening a panel from the Window menu by hand also freezes. Not investigated further.
- **Follow-ups** (todo.md):
  - Global Payouts adapter;
  - card fees in the ledger;
  - admin web UI;
  - credits for money;
  - signed packages;
  - ratings;
  - seller onboarding and sales in the app;
  - merging a bought rig into the open scene;
  - drag-and-drop of photos.
- **Manual checks for Gevor** are in todo.md (Phase J manual verification).
