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
