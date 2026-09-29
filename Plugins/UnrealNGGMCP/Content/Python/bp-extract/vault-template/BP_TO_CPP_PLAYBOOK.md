# Blueprint → C++ Conversion Playbook (UE 5.x)

A reusable methodology for converting Blueprint-only UE projects to a
C++/Blueprint hybrid, distilled from converting Epic's
`GameAnimationSample` Sandbox core. Optimised for the **Reparent** pattern
(BP stays as a thin data/override wrapper, C++ becomes the parent) and
**additive migration** (C++ logic added alongside BP, BP retired in
follow-up steps). Assumes the `UnrealNGGMCP` plugin is installed so MCP
tools (`ue5_*`) are available.

## Project adaptations (READ FIRST — overrides for GameAnimationSample)

This playbook is the **canonical methodology** for this project, with two standing overrides
and an automation layer that replaces several manual phases.

**Override 1 — Duplicate, don't reparent the original.** Everywhere this playbook reparents
the *original* BP, this project instead **duplicates first**: `BP_Foo → BP_Foo_CPP` (via
`POST /assets/duplicate`), reparents the **copy** to the C++ parent, does all additive migration
on the copy, and swaps external references (levels, other BPs, GameMode default pawn) to the copy
in Phase 6. **Originals are never edited.**

**Override 2 — Native bridge routes, not Python.** `PythonScriptPlugin` is not enabled here, so
substitute native routes for the playbook's `/editor/exec_python` steps:
- Struct fields / enum values → the enriched `GET /assets/get` (`struct_fields` / `enum_entries`).
- `.t3d` dump for call/cast/member refs is **unnecessary** — `GET /bp/read_graph` now emits node
  `target` (`{kind, class, name}`) and pin `type_object`, and `GET /bp/list_variables` emits `parent_class`.
  `read_graph`'s `target.kind` also resolves `cast` / `variable` / `macro` / `input_action` / `custom_event` /
  `event`, plus the two AnimGraph node kinds the `.t3d` was previously needed for: `property_access` (bound
  path) and `composite` (the collapsed subgraph is inlined under a `composite` field, recursively). See Phase 3.11.
- Duplicate → `POST /assets/duplicate`; default/CDO lifts → `POST /assets/set_property`.

**Override 3 — Duplicate vs. create-new is dictated by how much you lift into C++.** The choice is
**not** a free per-asset preference; it follows whether the C++ parent stays a *scaffold* or is *fully
fleshed*:
- **Scaffold port** (logic stays in the BP; the C++ parent is an *empty* typed base — zero UPROPERTY,
  zero functions): **duplicate** (`POST /assets/duplicate` → `ue5_reparent_blueprint`). The copy keeps
  the original's variables, functions, event graph, and SCS components + defaults, and nothing in the
  empty parent collides. Use for components/characters whose logic you are **not** porting yet
  (e.g. `AC_TraversalLogic`, and `SandboxCharacter_CMC` while only the type seam was needed).
- **Full logic port** (variables, functions, **and** components are lifted into the C++ parent): the
  `*_CPP` asset MUST be an **empty child** (`ue5_create_blueprint` with `parent_class` = the C++ class) —
  clean graph, zero variables, zero SCS components. The **original BP is the read-only reference**: read
  its graphs (`ue5_bp_read_graph`) and port them into C++ part by part. The empty child carries only what
  *can't* live in C++ — BP-only component instances and per-instance asset defaults.

> ⚠ **Never keep a content duplicate once the C++ parent mirrors its members.** A duplicate's BP
> variables / functions / SCS components collide *by name* with the now-native C++ members, and the
> editor **hard-crashes inside `RerunConstructionScripts`** the moment that BP is compiled
> (observed on `SandboxCharacter_CMC_CPP` after its parent gained native `SpringArm`/`Camera`/… components
> and matching vars/functions). The bridge has **no** delete-variable / delete-function / delete-SCS-node
> route, so a duplicate cannot be retro-emptied — you must **delete it and recreate it as an empty child**
> (`ue5_delete_asset` → `ue5_create_blueprint`) before/while fleshing out the C++ parent. Practical rule:
> start a full port as an empty child from the beginning; only ever use a duplicate when the parent will
> stay an empty scaffold.

**Delete an orphaned duplicate outright — don't recreate an empty child — when nothing references it.**
If the C++ owner already holds the C++ component/type *directly* (e.g. the character declares
`UPROPERTY() TObjectPtr<UMyComponent>` and `CreateDefaultSubobject`s it) and **no** Blueprint references
the `*_CPP` duplicate, there's no need for a BP child at all: just `ue5_delete_asset` the duplicate. Do it
**before** the C++ parent gains members of the same names (else the editor crashes compiling the stale
duplicate on next load). Real example: `AC_TraversalLogic_CPP` was deleted in AC_TraversalLogic Stage 2 —
the character holds a `UGameAnimTraversalLogic` component directly and `SandboxCharacter_CMC_CPP` referenced
only `AC_FoleyEvents`/`AC_SmartObjectAnimation`, never the duplicate.

**Override 4 — Convert Blueprint Function Libraries early (right after data types).** A `BFL_*` /
function-library Blueprint is a leaf utility called from many places, so port it to a C++
`UBlueprintFunctionLibrary` **before** the components/characters/ABP that call it. Two reasons:
- **Clean caller repoint.** Once the library is C++, callers either resolve directly or are repointed
  with `+FunctionRedirects` (more reliable than enum/struct redirects, which fail on Switch/Break nodes).
- **Avoids a Blueprint load-order trap.** A BP function library that calls *plugin* functions (e.g.
  `BFL_HelpfulFunctions` → the `DrawDebugLibrary` plugin) can be **unresolved at editor startup** because
  the content compiles-on-load before the plugin module's script package registers — leaving the BP (and
  everything that calls it, e.g. the ABP) erroring until a manual recompile, on every launch. A C++ library
  links the plugin module directly, so the dependency is always available. Porting the library is the
  durable fix; chasing it with module load-order tweaks (game-module `Build.cs` dep) does **not** fix the
  content compile-on-load ordering. Treat function libraries as tier-0 alongside enums/structs.

**Override 5 — Seam at the type boundary; do NOT unify BP↔C++ types.** Replacing the BP `S_*` / `E_*`
types with their `FGameAnim*` / `EGameAnim*` mirrors project-wide is a **confirmed dead end** — `[CoreRedirects]`
fail in both shapes: `+EnumRedirects` make `K2Node_SwitchEnum` / `CastByteToEnum` / break-struct nodes go to
"bad enum" (null), and on a plain enum variable with no Switch/Break they **silently degrade it to a bare
`byte`** (the C++ enum is never adopted); `+StructRedirects` don't take on a variable's pin either.
(Two project-wide enum attempts + an isolated single-enum pilot on `E_MovementDirectionBias`, all rolled
back — see the project memory `bp-type-migration-coreredirects`.) So the **canonical architecture is the
seam** — keep BP types in BP content, use the C++ mirrors in C++, and bridge at the boundary:
- **struct in/out**: a CustomThunk reflection filler (`FillPropertiesForAnimation` fills the BP
  `S_CharacterPropertiesForAnimation` from C++ state; the Phase 3.8 helpers read BP UDS/enum variables into C++),
- **calls across the boundary**: `BlueprintNativeEvent` / `BlueprintImplementableEvent` seams,
- **data accessors on a foreign BP**: a small `UINTERFACE` (Phase 3.7).
These seams already drive the live game (the ABP animates through `FillPropertiesForAnimation`; the CMC
component seams work). Convert each system **whole** with C++ types internally and seam its edges — do not
wait on, or attempt, a type unification. Consequence for the readiness gate (Phase 3.4): an `in-progress`
shared type is **not a blocker** — it is a seam boundary, i.e. **GO-with-seams**, not BLOCKED.

**Codegen build requirement.** Codegen emits headers into per-system subfolders and includes them
with the system prefix (`#include "Components/Foo.h"`, `"Data/EGameAnimGait.h"`). For that to resolve,
`GameAnimationSample.Build.cs` puts the module root on the include path: `PublicIncludePaths.Add(ModuleDirectory)`.
Enums compiled before this was needed (no `.cpp`; their `.generated.h` is auto-found in the UHT dir) —
the first emitted `.cpp` is what surfaces it.

**Automation (`Plugins/UnrealNGGMCP/Content/Python/bp-extract/`, Python ≥3.10) — what replaces the manual phases:**
(Invoke from the project root: `py Plugins\UnrealNGGMCP\Tools\bp-extract <mode>`.)
| Playbook phase | Automated by |
|---|---|
| Phase 1 — Inventory | `py Plugins\UnrealNGGMCP\Tools\bp-extract --from-vault` → one JSON per asset in `_extracted/` |
| Phase 3.4 — Readiness gate (dep closure + statuses) | [[Conversion Order]] (deps + "Unresolved external") + each dep's `Assets/**.md` `status:` frontmatter — classify done / in-progress / external, write a GO/BLOCKED verdict **before** coding |
| Phase 3.5 — Pre-flight dependency scan | `py Plugins\UnrealNGGMCP\Tools\bp-extract order` ([[Conversion Order]]) + `... brief <name>` (`_briefs/`) — dependency graph, tiers, cycles, per-function engine-only/blocker matrix |
| Phase 2 + 3 — Type pipeline + empty parents | `py Plugins\UnrealNGGMCP\Tools\bp-extract codegen` — UENUM/USTRUCT/UINTERFACE + empty typed parents into `Source/<Module>/<System>/` |

The mirror prefixes / module name / paths are **per-project config** (`config.py`: CLI > env > `BlueprintConvert.json`
> bridge `/project_info` > fallback). This project pins `mirror_token`/`class_group` = `GameAnim` via a root
`BlueprintConvert.json` (module/API/source auto-derive). Offline smoke: `py …\bp-extract\test_bp_extract.py`.

Type/prefix scheme used by codegen: `EGameAnim*` / `FGameAnim*` / `U`·`I`·`AGameAnim*`, module
macro `GAMEANIMATIONSAMPLE_API`. Quick-start: [[Conversion Guide]].

## TL;DR — the migration arc

1. Scaffold a primary game module (empty), regen `.sln`, build.
2. **Type pipeline first**: UENUM → USTRUCT → UINTERFACE in C++ (prefixed
   to avoid Engine collisions). Don't migrate logic yet.
3. **Empty typed C++ parents** for each BP being converted (no UPROPERTY
   that shadows BP variables — that's the #1 source of broken graphs).
4. **Reparent** each BP to its C++ parent.
5. **Migrate one BP function/event at a time**, additively:
   - Add the C++ equivalent (different name when conflict-prone).
   - BP keeps the original until callers move over.
6. **Lift data** from BP variables to C++ UPROPERTYs via BP CDO override.
7. **Delete dead BP code** once all callers have switched.
8. **Update external callers** (other BPs, input bindings, etc.) to use
   the C++ API directly. Then delete remaining BP wrappers.

Every step ends with: build C++, compile BP, run `ue5_bp_lint`, commit.

## Phase 0 — Setup

- Primary game module under `Source/<Project>/` with a `<Project>.Target.cs`
  and `<Project>Editor.Target.cs`. **UE 5.7**: use
  `BuildSettingsVersion.V6` + `EngineIncludeOrderVersion.Unreal5_7`.
  `Latest` resolves past 5.7 and conflicts with the installed editor
  (`UndefinedIdentifierWarningLevel` etc.).
- Regen `.sln` via UBT directly (`GenerateProjectFiles.bat` is missing on
  installed engine builds):

  ```powershell
  & "C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe" `
      -projectfiles -project="<absolute>.uproject" -game -rocket -progress
  ```

- Build editor target (`Development Editor / Win64`). Open editor. Confirm
  `ue5_health_check` returns `{ status: "ok" }`.
- **Disable AutoSave for the bridge workflow.** `ue5_kill_editor` is a hard process kill (= unclean
  shutdown), so with AutoSave on, UE shows a modal **"Restore Packages"** dialog on the next launch that
  blocks the bridge. Add `Config/DefaultEditorPerProjectUserSettings.ini` with
  `[/Script/UnrealEd.EditorLoadingSavingSettings]` / `bAutoSaveEnable=False` (this file is per-user /
  gitignored). We save explicitly (`ue5_save_all`, `ue5_bp_compile save=true`) and commit to git, so the
  AutoSave safety net is redundant. Takes effect on the next launch; clear any stale `Saved/Autosaves/`
  once if a dialog still appears.

## Phase 0.5 — Speed budget & editor lifecycle

Read this before deciding how to slice the work — the biggest wall-clock
cost on this pipeline is **editor restarts**, not C++ compilation.

| Action | Wall-clock cost |
|---|---|
| C++ incremental build (1 file change) | ~3-6 s |
| `ue5_kill_editor` | ~1-2 s |
| `ue5_launch_editor` + readiness poll | ~30-90 s |
| **Full restart cycle** | **~45-100 s** |
| `ue5_bp_compile` (single BP) | ~1-3 s |
| `ue5_bp_lint` (single BP) | ~1-2 s |

The full cycle costs more than the code change you're trying to verify.
The optimisation rule that follows:

> **Batch C++ changes, run one editor cycle, commit the series.**

### When Live Coding works (avoid full cycle)

Live Coding handles **bodies of existing UFUNCTIONs / methods** without
header edits. Iterating on a staged function body — e.g. adding three
sequential trace blocks inside an existing function — is the canonical
Live Coding case. The editor recompiles in-process, no restart.

### When Live Coding fails (full cycle required)

- New `UCLASS`, `UFUNCTION`, `UPROPERTY`, `USTRUCT`, `UENUM`, `UINTERFACE`.
- New `#include` that pulls a new module dependency.
- `Build.cs` edit.
- `meta=(...)` reflection metadata changes on existing decls.

For these, kill+build+launch is mandatory.

### Batched workflow (default for linear stages)

```
1.  ue5_kill_editor                            (1×)
2.  Edit C++ × N           (all stages in one editor-off window)
3.  ue5_build target=editor                    (1×, incremental)
4.  ue5_launch_editor + poll readiness         (1×)
5.  ue5_bp_compile blueprint=<target> save=true  (1×, final verify)
6.  ue5_bp_lint blueprint=<target>             (1×, sanity)
7.  Commit-series: N atomic commits using `git add -p` to slice hunks
```

### Sequential workflow (only when stages depend on each other at runtime)

Per-stage editor cycle. Reserve for: cross-class dep changes, fixing a
regression discovered via PIE, or risky struct-shape changes where you
need BP-side verification between stages.

### Anti-pattern

`commit → editor cycle → commit → editor cycle`. Spends ~90 s per cycle
to confirm something a C++ build success already implies. The BP
compile check belongs at the **end** of a stage series, not between
intermediate stages.

## Phase 1 — Inventory

For each BP in scope:

```
ue5_get_asset            path=/Game/<...>         # parent class
ue5_bp_list_variables    blueprint=/Game/<...>    # vars + components
ue5_bp_read_graph        blueprint=/Game/<...>    # all graphs
```

For deeper schema (struct fields, enum values, K2Node references that
`bp_read_graph` omits), export the asset as `.t3d`:

```python
# via /editor/exec_python (ue5_batch)
asset = unreal.EditorAssetLibrary.load_asset('/Game/.../Foo')
task = unreal.AssetExportTask()
task.set_editor_property('object', asset)
task.set_editor_property('filename', 'F:/.../Intermediate/StructDump/Foo.t3d')
task.set_editor_property('replace_identical', True)
task.set_editor_property('automated', True)
task.set_editor_property('prompt', False)
unreal.Exporter.run_asset_export_task(task)
```

`.t3d` is a UE text dump — `FunctionReference`, `VariableReference`,
`SubsystemClass`, `DelegateReference`, struct field GUIDs, default values
all live there.

## Phase 2 — Type pipeline (USTRUCT, UENUM, UINTERFACE)

**Do this before any logic migration.** Logic migrations need parameter/
return types, and BP types need C++ mirrors.

### UENUM

```cpp
UENUM(BlueprintType)
enum class EGameAnimGait : uint8        // BlueprintType requires uint8
{
    Walk   = 0 UMETA(DisplayName="Walk"),
    Run    = 1 UMETA(DisplayName="Run"),
    Sprint = 2 UMETA(DisplayName="Sprint"),
};
```

- Always prefix (`EGameAnim*`, project-specific). Engine has `EMovementMode`,
  plugins have collisions; unprefixed names cause UHT collision errors.
- If a `BlueprintType` enum doesn't have a `0` entry, UHT errors. Add a
  hidden zero: `None = 0 UMETA(Hidden)`.
- Match BP `NewEnumeratorN` indices in the numeric values so byte
  serialisation stays compatible if assets round-trip.

### USTRUCT

```cpp
USTRUCT(BlueprintType)
struct GAMEANIMATIONSAMPLE_API FGameAnimPlayerInputState
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Input")
    bool WantsToSprint = false;
    // ...
};
```

- Prefix `FGameAnim*` / similar. Mover/PoseSearch/etc. have many `F...`
  collisions.
- BP-side struct (`S_*`) stays in place — C++ struct is used by **new**
  C++ code only. K2 nodes that broke a `S_*` struct don't auto-migrate.

### UINTERFACE

```cpp
UINTERFACE(MinimalAPI, Blueprintable)         // Blueprintable = BP can implement
class UGameAnimSampleCharacterPawnInterface : public UInterface
{
    GENERATED_BODY()
};

class GAMEANIMATIONSAMPLE_API IGameAnimSampleCharacterPawnInterface
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintImplementableEvent, BlueprintCallable, Category="Getters")
    FGameAnimCharacterPropertiesForAnimation Get_PropertiesForAnimation();
};
```

- `Blueprintable` is required; `meta=(CannotImplementInterfaceInBlueprint=...)`
  is the wrong syntax and breaks `BlueprintImplementableEvent` members.
- Both old BPI and new UINTERFACE can be implemented on the same class
  in parallel during migration.
- Match BP `bThreadSafe` with `meta=(BlueprintThreadSafe)` where applicable.

## Phase 3 — Empty typed parents + reparent

The critical lesson: **never declare a C++ UPROPERTY whose name matches
a BP variable on the BP being reparented**. The reparent silently deletes
the BP variable as a duplicate; K2 nodes referencing it lose their
`MemberName` metadata and become unrecoverable stale nodes. Auto-fix
can't repair them (`fixable: false`); removing them destroys execution
flow.

So Phase 3 looks like:

```cpp
UCLASS(ClassGroup=(MyGame), Blueprintable, BlueprintType, meta=(BlueprintSpawnableComponent))
class MYGAME_API UMyComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UMyComponent() { PrimaryComponentTick.bCanEverTick = false; }
};
```

Zero UPROPERTY, zero logic. Just a typed parent so reparent doesn't
collide with anything. Then:

```
ue5_reparent_blueprint blueprint=/Game/.../AC_X parent=/Script/MyGame.UMyComponent
ue5_bp_compile         blueprint=/Game/.../AC_X save_on_success=true
ue5_bp_lint            blueprint=/Game/.../AC_X
```

Lint should be clean (or only false-positive `stale_variable_ref` on
`K2Node_BreakStruct`/`MakeStruct`/`SetFieldsInStruct` — verify by reading
the actual node class in the `.t3d`; the lint tool mis-classifies these).

## Phase 3.4 — Readiness gate (vault-first dependency closure)

**Run this FIRST — before Phase 3.5, before duplicating, before one line of C++.** The vault has
already computed most of what you need; use it instead of drilling the graph reactively. Almost every
"surprise" blocker (a dependency rooted in an unrelated toolkit, a type that only *looks* converted)
is visible here in ~2 minutes. Skipping this gate is how AC_TraversalLogic got picked as a target and
LevelBlock_Traversable got recommended as a "leaf" — both wrong, both visible in the vault up-front.

### Step 1 — pull the full transitive closure from the vault (don't drill reactively)
- `Conversion Order.md` (`_extracted/_order.json`) already lists, **per function**, every project
  dependency, plus a top-level **"Unresolved / external project references"** section. Read your
  target's entry end-to-end.
- For each project-BP dependency, open its `Assets/**/<Dep>.md` and read the `status:` frontmatter.
- **Recurse** one level for any dependency that is itself a project BP — its *parent chain* counts
  (e.g. `LevelBlock_Traversable`'s parent is the `LevelBlock` prototyping-toolkit actor, which drags
  in `LevelButton` + `S_GridMaterialParams` + grid materials).

### Step 2 — classify every dependency
| Dep state | Meaning | Action |
|---|---|---|
| `status: done` | C++ type/class exists **and** is the one actually in use | Green — call it directly |
| `status: in-progress` | **C++ mirror exists but the BP still uses the BP type** — a seam *boundary*, not a green light | **GO-with-seams** (Override 5): keep the BP type in BP, use the mirror in C++, bridge at the edge. NOT a blocker — type unification is abandoned. |
| Missing from the inventory / in "Unresolved external" | Engine type, or a **toolkit / 3rd-party-rooted** BP (level prototyping, editor utility) | **Do not port the class.** Seam the one method you need via a UInterface (Phase 3.7) |
| Plugin module (Chooser, Mover, NavigationSystem…) | Available, just needs a `Build.cs` dep | Note the module; not a blocker |

> ⚠ **`in-progress` is THE trap.** A generated `FGameAnim*`/`EGameAnim*` mirror while the BP `S_*`/`E_*`
> is still live means the two are **different reflection types** (the "struct dichotomy", Phase 3.8).
> Anything that shares those types across the C++/BP boundary — the component's own member vars, its
> callers, interface I/O, Chooser I/O — is a **seam point**. **"Mirror exists" ≠ "same type".** A target
> whose shared types are all `in-progress` is a **seam job** (Override 5: convert it whole with C++ types
> internally, bridge the BP boundary with a reflection filler / BlueprintNativeEvent / UINTERFACE) — NOT a
> unification job. Don't try to retype the BP content; that path is a dead end.

### Step 3 — write the verdict before coding
- **GO** — every dep is `done` or pure-engine. Proceed to Phase 3.5.
- **GO with seams** — only external/toolkit roots block; unblock each with a UInterface (Phase 3.7),
  then proceed.
- **BLOCKED** — a shared type is `in-progress`, or a needed project dependency is unported. **Do not
  start.** Convert the blocker first (topological order) or pick a different target. STT_FindRandomLocation
  was a GO (engine-only deps); STT_SetCharacterInputState was BLOCKED (BP interface + BP struct).

### Worked verdict — AC_TraversalLogic = GO-with-seams (revised 2026-06, post Override 5)
Closure from the vault: every data type it touches (`S_TraversalCheck*`, `S_TraversalChooser*`,
`S_CharacterPropertiesForTraversal`, `E_Gait` / `E_MovementMode` / `E_TraversalActionType`) is
`in-progress`; `LevelBlock_Traversable` is in **Unresolved external** (rooted in the `LevelBlock`
prototyping toolkit); `TryTraversalAction` also needs Chooser eval + the `BPI_SandboxCharacter_*` interfaces.
*Originally* called BLOCKED-on-type-unification. **Revised under Override 5: GO-with-seams** — the
`in-progress` types are seam boundaries, not a wall. Port the component **whole** with the C++ mirror types
internally; bridge the edges: an `IGameAnimTraversable` UINTERFACE for `GetLedgeTransforms` (do NOT port the
LevelBlock toolkit chain — Phase 3.7), a C++ Chooser-eval call for the montage selection, and
BlueprintNativeEvent / reflection seams for the `BPI_SandboxCharacter_*` calls. It is a **large** seam job
(`TryTraversalAction` is ~15k JSON lines), not a quick one — but tractable, and no longer gated on a type
migration.

## Phase 3.5 — Pre-flight dependency scan

**Run only after the readiness gate (Phase 3.4) returns GO or GO-with-seams.** The gate decided
*whether* to start and *which* boundaries need seams; this deeper scan reads the target function's
graph 2 levels deep to set the *per-function* seam strategy and migration order. Catching a blocker
here still saves a revert cycle.

```
Step 1: ue5_bp_read_graph blueprint=<BP> graph=<targetFunction>
        If JSON > ~5000 lines, delegate the structural map to an Explore
        subagent. Ask it to report:
          - K2Node_DynamicCast target classes
          - K2Node_CallFunction function names + target classes
          - K2Node_VariableGet/Set member-name + owning-class references

Step 2: For each Cast → find the target class. If it's a BP, recursively
        read its called methods. Note which of its methods/properties the
        original function actually touches.

Step 3: For each K2Node_CallFunction → write down function-name +
        target-class. If the target is a sibling BP function on the
        same blueprint, mark the migration order (the call site moves
        last).

Step 4: Reduce to a matrix: [target function → required blockers].
        Decide for each blocker:
          - Port to C++ first (full reparent)?
          - Bridge via UInterface contract?
          - Leave as BlueprintImplementableEvent stub (BP authoritative)?
          - Replace with a free-function helper?
        Check for cross-class cycles (A→B and B→A) — these need extra
        thought, sometimes a third party object holding shared state.
```

**Subagent mapping discipline.** When you delegate a big graph map to an Explore subagent, give it
**specific node IDs / line ranges** and require it to **trace each input pin to its source** (follow
`connected_to`) and mark **UNRESOLVED** rather than infer "by analogy". Vague "summarize this graph"
prompts produced *wrong* maps this session — a debug-draw branch was mislabeled as the room-check entry,
and ActionType was placed in the wrong phase. Re-verify any constant/offset the subagent reports as
inferred by reading the exact node before porting it.

Real example (from the GameAnimationSample conversion): IA_Jump handler
called BP `Try Traversal Action` → which cast hit-actor to
`LevelBlock_Traversable` (BP) → which called its
`FindLedgeClosestToActor` / `GetLedgeTransforms` BP methods → which
read its `Ledges` / `OppositeLedges` / `MinLedgeWidth` BP variables.
Discovering this *mid-port* cost a revert (commit `e8d75bb`) and a full
re-plan into the UInterface approach. **A pre-flight scan would have
surfaced the cascade before the first commit.**

### When the right call is "don't port"

Not every BP function should move to C++. Pre-flight sometimes surfaces
conditions that flip the answer to *leave it in BP*:

| Signal | Interpretation |
|---|---|
| Function is debug-only (visualisation, telemetry, log spew) | BP authoring is fine; no runtime data flow benefits from a C++ port |
| Requires a plugin module not otherwise in `Build.cs` (especially `Experimental`) | Adding a module dep changes the project's surface area for a debug-only payoff — usually not worth it |
| Has no C++ caller and no foreseeable C++ caller | Parallel `*Native` would never run; the port is dead code |
| ~100+ typed-API calls with no shared structure (just a long chain of `DrawX(...)`, `LogY(...)`) | Mechanical port with low code-readability gain |
| BP version already works and is not on any list of suspect/buggy behaviour | Don't fix what isn't broken |

**Two or more signals together = skip and document.** Add a one-line entry
to your project's "intentionally not ported" list with the reason,
so a future pass doesn't re-litigate the decision.

Real example (this repo): `SandboxCharacter_Mover.DebugDraws` — a ~6k-JSON-
line debug-only graph that depends on the experimental `DrawDebugLibrary`
plugin (not in `GameAnimationSample.Build.cs`, not in `.uproject`), with
no C++ caller. All five signals above. Intentionally left in BP; the
BP-side version is the authoritative debug visualiser.

The single most common waste case in BP→C++ migrations is mechanically
porting a debug function. Treat the BP→C++ scope as "what changes runtime
behaviour" — debug overlays sit outside that.

## Phase 3.6 — Per-function staging decision

Decide up front: one commit, or staged across several? Wrong choice costs
editor cycles (over-staged) or a hard-to-bisect commit (under-staged).

| Scenario | Decision |
|---|---|
| Self-contained, <50 BP nodes, no cross-class deps | **Single commit, full body** |
| 50-150 nodes, linear trace/struct mutations, no deps | **Single commit, full body** |
| 150+ nodes OR 2+ cross-class deps | **Staged**, but cap at 4-6 commits |
| Depends on an unported BP function | **Port the dependency first**, then this — topological order |
| Depends on a BP-only `UserDefinedStruct` / `Enum` | **Mirror types first** (Phase 2), then port logic |

Anti-example from the same conversion: `TryTraversalAction` (241 nodes)
was split into 8 stages, of which 6 were linear additive trace blocks
with no cross-class deps. Three commits would have sufficed (skeleton,
helpers + interface, full body). The other 5 commits were pure
ceremony — 5 extra editor cycles ≈ 5-8 minutes lost.

### Staging a whole-component port under the seam model (Override 5)

A large, interdependent component (shared struct-typed members across all its functions) can't be
half-migrated — port it **whole** into one C++ class, seamed at the BP boundary, staged across sub-commits
(each its own build):
1. **Skeleton + typed members** — C++ parent gains the member UPROPERTYs (mirror types) + function
   skeletons; delete/empty the BP duplicate (Override 3).
2. **Trace + interface seam** — entry logic + the `IGameAnim*` call to the unported BP dependency.
3. **Validation / geometry** — pure-C++ checks (e.g. clearance sweeps).
4. **Data seams** — Chooser eval, BP-struct conversion (Phase 3.8), interface calls for state.
5. **Side-effecting tail** — warp targets, montage play, etc.
6. **Repoint the caller** off its reflection seams onto the C++ component + populate its inputs.

Until the later sub-stages land, return a **safe placeholder** (e.g. report the op failed so callers fall
back) so the half-ported component is harmless. The component isn't live until step 6, so end-to-end PIE
verification happens once at the end. Real example: AC_TraversalLogic's 6 sub-stages.

## Phase 3.7 — UInterface as escape hatch for deep BP hierarchies

When the BP whose data you need is itself a child of another BP (e.g.
`LevelBlock_Traversable` extends `LevelBlock` extends `AActor`), a full
port forces porting **both** levels: ~500 lines including
material/transform/naming infrastructure on the parent BP that has
nothing to do with what you actually need.

Use a UInterface contract instead (~80 lines):

```cpp
UINTERFACE(Blueprintable, BlueprintType, MinimalAPI)
class UMyDataProvider : public UInterface { GENERATED_BODY() };

class MYGAME_API IMyDataProvider
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintImplementableEvent, BlueprintCallable, Category="Data")
    TArray<USomeComponent*> GetData() const;
};
```

On the BP side: Class Settings → Implemented Interfaces → Add → wire each
event to return the existing BP variable. From C++:

```cpp
if (HitActor->Implements<UMyDataProvider>())
{
    const TArray<USomeComponent*> Data =
        IMyDataProvider::Execute_GetData(HitActor);
    // …
}
```

No reparent, no touching the parent BP, no porting the materials/
transforms hierarchy.

**⚠ Gotcha — name the interface method differently from any existing BP function.** If the target BP
already has a function whose name matches your UINTERFACE method, implementing the interface on it errors
with *"Found more than one function with the same name"*. Rename the C++ interface method so it differs,
and have the BP impl forward to its existing function. Real example: the obstacle BP `LevelBlock_Traversable`
already had a `GetLedgeTransforms`, so the interface method was named **`QueryLedgeTransforms`** — its BP
impl just forwards to the BP's own `GetLedgeTransforms`.

**Applicable when**:
- BP exposes data or simple accessor methods to C++.
- The parent-class behaviour (e.g. randomisation, naming, materials)
  isn't what you're migrating.

**Not applicable when**:
- C++ needs to OVERRIDE a BP method (use `BlueprintNativeEvent` and
  full reparent for that).
- Method bodies on the BP do non-trivial logic you'd want in C++ — at
  that point you're hiding work behind the interface, port the body
  directly instead.

## Phase 3.8 — Reading BP UserDefinedStruct from C++ via reflection

When a BP variable's type is a `UserDefinedStruct` (e.g. `S_PlayerInputState`,
`S_MoverCustomInputs`) and the C++ mirror struct (e.g. `FGameAnimPlayerInputState`)
is a separate USTRUCT, **direct casts between them don't work** — they're
different UE types with different reflection identities. This is the
"struct dichotomy" blocker: C++ Native functions can't read the BP
variable as the C++ type without an explicit conversion layer.

Three strategies to get out:

| Strategy | When | Cost |
|---|---|---|
| **Thin-wrap BP forwarders** | Few call sites, one-off | BP graph rewrite per function |
| **Project-wide S_ → F_ refactor** | Final migration step | Touches everything; not for incremental work |
| **Reflection-based read from BP UDS** | Incremental ports, no BP graph edits | One helper, scales linearly |

The reflection approach is the right default for *parallel-port* migrations
where BP stays the runtime authority for the data and C++ Native reads
without writing.

### The pattern

For each BP variable type you need to read, write a typed reflection helper
on the host class. Keep them `private` — they're implementation details of
the parallel-port migration, not part of the API surface.

**Reading a bool field from a BP UDS variable:**

```cpp
bool AHostClass::ReadBPInputStateBoolFlag(const FString& FieldPrefix) const
{
    const FStructProperty* StateProp = FindFProperty<FStructProperty>(
        GetClass(), TEXT("CharacterInputState"));
    if (!StateProp || !StateProp->Struct) return false;

    const void* StateData = StateProp->ContainerPtrToValuePtr<const void>(this);

    for (TFieldIterator<FBoolProperty> It(StateProp->Struct); It; ++It)
    {
        if (It->GetName().StartsWith(FieldPrefix))
        {
            return It->GetPropertyValue(
                It->ContainerPtrToValuePtr<const void>(StateData));
        }
    }
    return false;
}
```

**Why prefix-match, not exact-match:** BP UDS auto-appends `_<Index>_<GUID>`
to every field name on save — `WantsToSprint_1_840C190D...`,
`WantsToCrouch_9_47D0791...`. Hard-coding the GUID locks the helper to a
specific BP-save snapshot; prefix-match keeps working when the BP
re-serializes after an unrelated edit.

**Reading an enum/byte class variable (not inside a UDS):**

```cpp
uint8 AHostClass::ReadBPEnumByteProperty(const FName& PropertyName) const
{
    if (const FByteProperty* ByteProp =
            FindFProperty<FByteProperty>(GetClass(), PropertyName))
    {
        return ByteProp->GetPropertyValue_InContainer(this);
    }
    if (const FEnumProperty* EnumProp =
            FindFProperty<FEnumProperty>(GetClass(), PropertyName))
    {
        const void* Data = EnumProp->ContainerPtrToValuePtr<const void>(this);
        return static_cast<uint8>(
            EnumProp->GetUnderlyingProperty()->GetUnsignedIntPropertyValue(Data));
    }
    return 0;
}
```

**Why both FByteProperty and FEnumProperty:** BP-side enums are emitted as
one or the other depending on whether the variable was authored as a typed
UENUM-property variable (`FEnumProperty`) or as a legacy
byte-with-enum-meta variable (`FByteProperty`). The helper handles both
so callers don't have to think about it.

### Where this pattern fits

- C++ Native function needs to **read** a BP UDS or BP enum: use the helpers.
- C++ Native function needs to **write** to the BP variable: writes are
  similar (`SetPropertyValue_InContainer` / `SetUnsignedIntPropertyValue`),
  but consider whether the BP-side will also write to the variable —
  parallel writes from both sides race.
- The data is high-frequency (per-tick): the reflection lookups are not
  trivially cheap. Cache the `FProperty*` pointers on `InitializeComponent`
  if you're hitting a frame budget; reuse the cached pointer per call.
- Want to write generic templated helpers: possible
  (`template<typename TEnum>`), but UE reflection makes it awkward and the
  per-type concrete helpers are more readable.

Real example (this repo): `AGameAnimSampleCMCCharacter::ReadBPInputStateBoolFlag`
+ `ReadBPEnumByteProperty` together unblock `CanSprintNative`,
`GetDesiredGaitNative`, and (once ported) the `Calculate*` family +
`UpdateMovement_PreCMC` / `UpdateRotation_PreCMC`. BP still owns the
input-state writes (via the still-authored BP custom events); C++ Native
only reads.

### Full-struct converter (read a whole BP UDS into the C++ mirror in one call)

When you need the *entire* BP UserDefinedStruct as the C++ mirror at once (e.g. an interface impl that must
return the mirror after running BP logic), use a single `CustomThunk` converter instead of N field-reads.
It takes the BP struct as a `CustomStructureParam` wildcard (C++ can't name the BP type) and returns the
mirror, copying fields matched by `GetAuthoredName()` per type:

```cpp
// .h, static on a UBlueprintFunctionLibrary:
UFUNCTION(BlueprintCallable, CustomThunk, meta=(CustomStructureParam="In"))
static FGameAnimMyStruct MakeMyStructFromBP(const int32& In);
DECLARE_FUNCTION(execMakeMyStructFromBP)
{
    Stack.StepCompiledIn<FStructProperty>(nullptr);
    const void* Data = Stack.MostRecentPropertyAddress;
    const FStructProperty* P = CastField<FStructProperty>(Stack.MostRecentProperty);
    P_FINISH;
    P_NATIVE_BEGIN;
    *static_cast<FGameAnimMyStruct*>(RESULT_PARAM) = MakeMyStructFromBP_Impl(P ? P->Struct : nullptr, Data);
    P_NATIVE_END;
}
```

`_Impl` finds each field by `GetAuthoredName()` (BP UDS names carry a `_<idx>_<GUID>` suffix) and copies
per type — `FBoolProperty` / `FDoubleProperty`, `FStructProperty` for FVector via `CopyScriptStruct`,
`FByteProperty`/`FEnumProperty` for enums (read the underlying byte → `static_cast` to the mirror enum).
This is the inverse of the "fill a BP struct from C++" CustomThunk. Real example:
`UGameAnimHelpfulFunctions::MakeTraversalResultFromBP` — the BP traversable obstacle runs its existing
`S_`-typed ledge logic, then calls this to return the C++ `FGameAnimTraversalCheckResult`.

## Phase 3.9 — Implementing a C++ interface ON a Blueprint via the bridge

To make a BP implement a C++ UINTERFACE (the Phase 3.7 seam) from the bridge:

1. `POST /bp/add_interface { blueprint, interface }` — adds the interface to the BP's implemented list.
   For interface functions with **no return value** this also auto-creates the impl graph.
2. For interface functions **with a return value**, the impl graph is NOT auto-created — call
   `POST /bp/implement_interface_function { blueprint, function }` (it binds as the interface override, not
   a colliding standalone function).

Both are custom routes → direct PowerShell POST to `http://localhost:6776/...` (see the MCP tool reference;
they are not `ue5_*` tools and not in the `ue5_batch` allowlist).

**Wiring the impl graph** with `ue5_bp_create_variable` + `ue5_bp_add_logic`: reference the pre-existing
FunctionEntry / FunctionResult nodes by their **GUID** in `connections`. To forward to an existing BP
function that takes an **in/out (by-ref) struct**, create a temp member var of that BP struct type and wire
its Get to the by-ref pin (and to the converter), then the converter output → FunctionResult.ReturnValue.
Real example: `LevelBlock_Traversable.QueryLedgeTransforms` = entry → existing `GetLedgeTransforms`(by-ref
`IGT_TempResult`) → `MakeTraversalResultFromBP(IGT_TempResult)` → return.

## Phase 3.10 — Recipe: porting AI StateTree tasks (STT_*)

A BP StateTree task (parent `UStateTreeTaskBlueprintBase`) ports to a `UCLASS(Blueprintable)` C++ subclass:
put the logic in an `EnterState(FStateTreeExecutionContext&, const FStateTreeTransitionResult&)` override
returning `EStateTreeRunStatus`, and bind Context/Input by the property **Category string** —
`Category="Context"` auto-binds the schema context object (e.g. the AIController), `Category="Input"` an
asset input. The StateTree compiler maps the category to the binding usage, so this works for a C++ subclass
exactly like the BP variables did. In UE 5.7 the `ReceiveEnterState`-with-return-value event is
**deprecated** — use the void `EnterState` + a `Finish Task` node; default run status is `Running`. Build.cs:
`StateTreeModule` (public) + whatever engine modules the body calls (e.g. `AIModule`, `NavigationSystem`).
The StateTree assets that reference the task must be repointed to the C++ class **manually** (no bridge
route edits StateTree nodes). Real examples: `UGameAnimClearFocusTask` / `UGameAnimFindRandomLocationTask`.

## Phase 3.11 — Recipe: porting an Animation Blueprint (UAnimInstance)

An AnimBP (parent `UAnimInstance`) ports to a `UCLASS(Blueprintable, BlueprintType)` C++ subclass that holds
the member variables **and** the logic functions; the **AnimGraph stays in the Blueprint**. Reparent the
**original** ABP (not a duplicate — the skeletal mesh's `AnimClass` points at it; same reason as the
camera-director / foley ports). This is a large, multi-session port — stage it: (1) scaffold + variable mirror
+ reparent, (2..n) port pure getters then the update spine function-by-function, AnimGraph untouched throughout.
Real example: `SandboxCharacter_CMC_ABP` → `UGameAnimSandboxCharacterCMCAnimInstance`.

### Stage 1 — variable mirror + reparent
Split every member variable BEFORE mirroring (decide per var):
- **Category A — engine/CoreUObject types** (FTransform/FVector/float/bool/int, plus plugin structs whose
  UScriptStruct is shared: `FTransformTrajectory`[Engine], `FPoseSearchTrajectoryData`/`_WorldCollisionResults`
  [PoseSearch], `FFootPlacement*Settings`[AnimationWarpingRuntime], `TObjectPtr<UMoverComponent/...>`): a
  same-named, same-typed `UPROPERTY(BlueprintReadWrite, Category="<exact BP category string>")` **is** the same
  property after reparent → AnimGraph rebinds, UE auto-drops the BP duplicate, compile errors:0.
- **Category B — BP UserDefinedStruct / BP enum** (the `S_*` / `E_*` types): the C++ mirror is a DIFFERENT
  reflection type → AnimGraph pins won't accept it. **Keep BP-side**; read from C++ by reflection-on-self
  (Phase 3.8). Convert per-cluster later only if you do AnimGraph pin surgery.
- **Invalid C++ identifiers** (`MM Search Cost`, `NotifyTransition_Re-Transition`): keep BP-side; write by
  reflection-on-self (authored name).
- **Complex-default plugin structs** (curve/Stops CDO defaults): mirror-able but defaults are painful → defer
  until the owning function is ported.

> ⚠ **THE REPARENT-RENAME-`_0` TRAP.** Reparent **drops** most duplicate Category-A vars (good) but **renames a
> few to `Name_0`** instead — observed for floats written via `K2Node_VariableSet` (e.g. `Speed2D`,
> `AccelerationAmount`). The renamed var keeps its setter nodes pointed at `_0`, so deleting the `_0` leftover via
> the bridge leaves a `disconnected_exec_input` and a runtime regression (the value is never written). **Fix: do
> NOT mirror those vars** — exclude collision-rename-prone Category-A floats from the C++ scaffold (keep them
> BP-side with their original names: no collision, no rename, references intact) and read them from C++ by
> reflection-on-self. Recover from a bad delete: `git checkout` the ABP `.uasset` (kill the editor first — the
> file is locked), trim the C++ member set, redo the reparent.

Verify with `ue5_bp_list_variables` (`inherited_count` = mirrored Category-A count; the remaining list stays BP-side).

### Stage 2..n — port functions (pure getters first, then the update spine)
Function-port loop: add the C++ method (exact BP name + matching signature + an **engine return type** so callers
rebind — a Category-B/BP-enum return would not) → build → `ue5_bp_delete_function` the BP function → `ue5_bp_compile`
→ callers rebind to the inherited C++ method (errors:0). Batch several C++ adds per editor cycle.

- **`meta=(BlueprintThreadSafe)` is mandatory** on every ported anim getter/function — they run on the worker
  thread (`BlueprintThreadSafeUpdateAnimation`); a plain UFUNCTION errors *"non-thread-safe function called from
  thread-safe graph"*. Pure getters → `UFUNCTION(BlueprintPure, meta=(BlueprintThreadSafe))`.
- **Purity is decided by the CALLER's call node, not the body.** A BP function can be marked **pure** yet contain
  an internal exec/branch graph (`K2Node_IfThenElse` + multiple `FunctionResult`). "Pure" only governs the call
  site (no exec pins, re-evaluated each read). Inspect the caller: no exec pins on its call node → port as
  `BlueprintPure` (reimplement the internal branches as plain C++ `if`/`switch`); exec-wired caller →
  `BlueprintCallable`. AnimGraph **pin-bound** getters always rebind cleanly to BlueprintPure (binding is by name;
  exec is irrelevant). Real example: `CalculateRelativeAccelerationAmount` (pure, internal Branch) — its caller
  `Get_LeanAmount` had no exec pins → BlueprintPure.
- **Reflection-on-self helpers** (private methods on the AnimInstance) read the Category-B vars kept BP-side:
  `ReadSelfEnumByte(authored)` (state enums via FByteProperty/FEnumProperty), `ReadSelfReal(authored)` (deferred
  floats e.g. the `_0`-trap vars), `FindSelfStructProp(authored, OutData)` + `GameAnimStructReflect::*`
  (CharacterProperties fields, incl. nested two-level reads like `CharacterProperties.InputState.WantsToAim`).
- **Inherited UAnimInstance fns** are callable directly and are thread-safe: `IsSlotActive`, `GetCurveValue`,
  `Blueprint_GetSlotMontageLocalWeight`.
- **Enum compares**: a `K2Node_Select`/`SwitchEnum` keyed on a BP enum maps option pin `NewEnumeratorN` to byte
  value `N` — verify with `get_asset` `enum_entries` (display name ≠ byte value), then `switch` on the raw byte
  from `ReadSelfEnumByte`. SwitchEnum fallthrough (unwired case) returns the node's default — pick the dominant
  explicit value and document it.

### AnimGraph read_graph blind spots — now covered by the bridge
The `.t3d` workaround (Phase 1) is no longer needed for these two AnimGraph node kinds — `read_graph` exposes them:
- **`K2Node_PropertyAccess`** (fast-path AnimGraph property reads) emits its bound path:
  `target:{kind:"property_access", text_path, path[]}` (resolved by reflection on the node's private Path/TextPath).
- **`K2Node_Composite`** (collapsed subgraphs) recurses: the node carries a sibling `composite:{nodes:[...]}` field
  inlining the subgraph (and nested composites).
A Composite-heavy function is easiest to decode by loading the persisted `read_graph` JSON and walking
`composite.nodes` in a throwaway Python script, rather than regex over the escaped output.

## Phase 4 — Migrate logic (additive, per function)

For each BP function or event, the recipe is the same:

1. Read the BP graph (`ue5_bp_read_graph`) and the `.t3d` to recover
   `FunctionReference`s and member references that the JSON omits.
2. Translate node-by-node into C++. Use `UKismetSystemLibrary` /
   `UKismetMathLibrary` / `UGameplayStatics` to match BP behaviour
   exactly when crossing channels (`TraceTypeQuery1` etc.).
3. **Pick a non-colliding name** (`CycleToNextPawn` vs BP `CyclePawn`)
   unless you intend to use `BlueprintNativeEvent` for override semantics.
4. Add the C++ function as `UFUNCTION(BlueprintCallable, Category=...)`.
   Existing BP keeps working; new BP/C++ code can call the new method.
5. Build C++, launch editor, verify the new symbol is reachable
   (`unreal.load_class(None, '/Script/MyGame.UMyComponent')` etc.).
6. Commit (Source/ only — no .uasset diff yet).

### Common translations

| BP construct | C++ equivalent |
|---|---|
| Custom event | `UFUNCTION(BlueprintCallable)` or private `UFUNCTION()` |
| Event Tick on Actor/Component | `TickActor` / `TickComponent` override |
| BP-implemented event on parent | `UFUNCTION(BlueprintImplementableEvent)` |
| BP override of parent function | `UFUNCTION(BlueprintNativeEvent)` + `_Implementation` |
| `Get/SetConsoleVariableIntValue` | `UKismetSystemLibrary::GetConsoleVariableIntValue` / `ExecuteConsoleCommand` |
| Engine subsystem event hook | `GEngine->GetEngineSubsystem<U...Subsystem>()->Delegate.AddDynamic(...)` |
| Multicast delegate variable | `DECLARE_DYNAMIC_MULTICAST_DELEGATE_*Param` + `UPROPERTY(BlueprintAssignable)` |
| `ForEachLoop` macro | `for (int32 i = 0; i < N; ++i)` |
| `Cast<>` to BP class | `Cast<UMyCppParent>(...)` — works when caller has C++ class ref |

### Cross-class calls before full migration

When C++ needs to call a BP-defined function (the BP version still has
the implementation), use reflection:

```cpp
if (UFunction* Fn = FindFunction(FName(TEXT("ResetAllPlayers"))))
{
    ProcessEvent(Fn, /*Params=*/nullptr);
}
```

Replace with a direct call once the BP function is migrated.

### Subscribing to DataDrivenCVar changes

```cpp
// .h
UFUNCTION()
void HandleDataDrivenCVarChanged(FString CVarName);

// .cpp
#include "DataDrivenCVars/DataDrivenCVars.h"
#include "Engine/Engine.h"

void AMyGameMode::BeginPlay()
{
    Super::BeginPlay();
    if (auto* Sub = GEngine ? GEngine->GetEngineSubsystem<UDataDrivenCVarEngineSubsystem>() : nullptr)
    {
        Sub->OnDataDrivenCVarDelegate.AddDynamic(this, &AMyGameMode::HandleDataDrivenCVarChanged);
    }
}
```

Dynamic-delegate-bound UFUNCTIONs must match the delegate signature
**exactly** (e.g. `FString CVarName`, not `const FString&`) or
`AddDynamic` silently fails / runtime crashes.

### Migrating EnhancedInput bindings

```cpp
UPROPERTY(EditDefaultsOnly, Category="Input")
TObjectPtr<UInputAction> NextPawnAction;

virtual void SetupInputComponent() override;

void APC::SetupInputComponent()
{
    Super::SetupInputComponent();
    if (auto* EIC = Cast<UEnhancedInputComponent>(InputComponent))
    {
        if (NextPawnAction)
        {
            EIC->BindAction(NextPawnAction, ETriggerEvent::Triggered, this, &APC::OnNextPawnTriggered);
        }
    }
}
```

The BP CDO sets each `UInputAction*` UPROPERTY default to the actual IA
asset via `editor_property` (set via Python or in the BP class defaults).

### Lint false-positive baseline

`ue5_bp_lint` over-reports `stale_variable_ref` on struct nodes — every
`K2Node_BreakStruct` / `MakeStruct` / `SetFieldsInStruct` that operates
on a `UserDefinedStruct` field looks like a broken variable reference
to the linter, but compiles and runs fine. On a busy graph this is the
**majority of the output** — and it drowns out the rare real error.

Measured on the GameAnimationSample sandbox BPs:

| Blueprint | Known struct-node FPs |
|---|---|
| `AC_TraversalLogic` | 52 |
| `SandboxCharacter_Mover` | 44 |
| `SandboxCharacter_CMC` | 11 |
| `SandboxCharacter_CMC_ABP` | 15 |
| `LevelBlock_Traversable.GetLedgeTransforms` | 6 |

Keep a per-BP baseline (e.g. `Plans/lint-baseline.md`) listing the
known-FP node GUIDs by graph:

```
### AC_TraversalLogic (52 known FPs)
EventGraph:
  - 985DFFC0-...  K2Node_SetFieldsInStruct  S_PlayerInputState
  - 31D61AC1-...  K2Node_BreakStruct        S_PlayerInputState
  ...
TryTraversalAction:
  - AC724F50-...  K2Node_SetFieldsInStruct  S_TraversalCheckResult
  ...
```

`ue5_bp_compile` returning `success: true, errors: 0, warnings: 0`
combined with lint output that **only** contains baselined FPs = clean.
A new lint error that isn't in the baseline is the real signal.

Re-sync the baseline when the graph changes meaningfully (new nodes
mean new GUIDs; node deletion also).

`disconnected_exec_input` on a `K2Node_VariableSet` is usually a leftover
orphan from older BP authoring. Verify it isn't load-bearing by reading
the surrounding nodes, then ignore — `auto_fix` would delete it
including any live execution.

## Phase 5 — Lift defaults (BP variable → C++ UPROPERTY)

When a BP variable holds data the C++ side wants to own:

1. Declare the C++ UPROPERTY with a **different** name (avoid the
   reparent-collision foot-gun if the BP variable still exists):

   ```cpp
   UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Classes")
   TArray<TSubclassOf<APawn>> PawnClassesNative;
   ```

2. Copy the BP CDO override of the old variable into the new C++ UPROPERTY:

   ```python
   bp_class = unreal.EditorAssetLibrary.load_blueprint_class('/Game/.../GM_X')
   cdo = unreal.get_default_object(bp_class)
   cdo.set_editor_property('PawnClassesNative', list(cdo.get_editor_property('PawnClasses')))
   unreal.EditorAssetLibrary.save_loaded_asset(bp_asset, only_if_is_dirty=False)
   ```

3. Both arrays carry the same data — BP graphs that still read the old
   variable keep working; C++ reads from the new one.

4. After all readers migrate, delete the BP variable.

## Phase 6 — Cleanup / dead-code removal

Work in this order (least → most risky):

1. **Delete BP event-graph nodes** that have a C++ equivalent already
   broadcasting/dispatching. Avoids the double-subscription footgun
   (e.g. both BP and C++ running `BeginPlay` subscribers).
2. **Delete BP function graphs** with no callers:

   ```python
   unreal.BlueprintEditorLibrary.remove_function_graph(bp, 'FunctionName')
   unreal.BlueprintEditorLibrary.compile_blueprint(bp)
   unreal.EditorAssetLibrary.save_loaded_asset(bp, only_if_is_dirty=False)
   ```

3. **Empty the EventGraph** entirely (BP still compiles without one):

   ```python
   eg = unreal.BlueprintEditorLibrary.find_event_graph(bp)
   unreal.BlueprintEditorLibrary.remove_graph(bp, eg)
   ```

4. **Thin-wrap BP functions** when external callers still reference them
   by BP function name: replace the body with a single `CallFunction`
   forwarding to the C++ method. `ue5_bp_create_function` then
   `ue5_bp_add_logic` with `call_function` + `self_context: true`
   connected to the auto-generated `K2Node_FunctionEntry`.
5. **Update external callers** (other BPs that call into the migrated BP)
   to call the C++ method directly. Then delete the thin BP wrapper.
6. **Delete BP variables** once nothing reads them. Verify with a
   cross-BP scan (Reference Viewer, or Python iterating BPs and walking
   K2Node_VariableGet/Set with matching MemberGuid).

## Pitfalls / gotchas

- **Reparent name collision**. Repeating because it's the #1 cause of
  broken graphs: don't declare C++ UPROPERTY with a name that shadows an
  existing BP variable on the BP being reparented. The BP variable is
  deleted, K2 nodes referencing it become permanently stale.
- **Nested-struct component overrides are lost on reparent**. When the original BP overrode a *member inside a
  component's struct property* (e.g. `CharacterMovement.NavAgentProps.bCanCrouch=True`), reparenting the
  duplicate/child onto a C++ class that owns the component natively **drops that nested override** while scalar
  component deltas (`JumpZVelocity`, etc.) survive — the symptom is a silent capability regression (PIE logged
  *"crouching is disabled on this character"*). Lift such defaults in the C++ **constructor**
  (`GetCharacterMovement()->GetNavAgentPropertiesRef().bCanCrouch = true;`). When auditing what to lift,
  `get_component_defaults` does NOT list native (non-SCS) components in the no-`component=` sweep — query the
  native component explicitly (`?component=CharMoveComp`, CDO-subobject fallback) and **inspect its nested
  structs, not just scalar deltas**. (UE 5.7: `CrouchedHalfHeight` direct member access is deprecated C4996 —
  use `SetCrouchedHalfHeight()`.)
- **Lint false positives** on struct nodes: `stale_variable_ref` with
  `Variable 'None' on class '<null>'` is often a `K2Node_BreakStruct` /
  `MakeStruct` / `SetFieldsInStruct` mis-classified by the linter.
  Verify by reading the actual node class in the `.t3d` — these nodes
  legitimately have no `MemberName` (they have `StructType` instead) and
  work fine at runtime. `ue5_bp_compile` will report 0 errors.
- **Dynamic delegate signature mismatch**. `AddDynamic` requires the
  bound UFUNCTION to match the delegate's parameter signature exactly,
  including value vs const-ref. Mismatch silently fails or crashes.
- **Live Coding is fragile** for new UFUNCTIONs / UPROPERTYs. Kill the
  editor (`ue5_kill_editor`) before running `ue5_build`, then relaunch.
- **`auto_fix=true`** on `ue5_bp_lint` does NOT fix `stale_variable_ref`
  (always `fixable: false`); auto-removing the node destroys execution
  flow. Address the root cause manually.
- **`save=false`** on `ue5_bp_delete_node` works in editor memory only.
  Closing the editor reverts. If the auto-mode classifier blocks the
  destructive op, the partial state hasn't been persisted yet.
- **UE 5.7 build settings**: use `BuildSettingsVersion.V6` +
  `EngineIncludeOrderVersion.Unreal5_7` — `Latest` causes UBT to refuse
  builds against the installed editor.
- **Cross-BP refs** are not auto-rewritten by anything. Renames and
  function deletions must be paired with manual caller updates (or a
  Python K2Node_CallFunction walker rewriting `MemberName`).
- **`replace_variable_references`** has single-BP scope. It can't redirect
  K2Node_VariableGet/Set in other BPs.
- **Premature input-action migration**. Don't port an `IA_X` handler to
  C++ before its downstream BP function chain is in C++. Engine-standard
  `ACharacter::Jump` etc. does not cover project-specific smart logic
  (Jump-or-Traverse, Crouch-while-aim filters, etc.). Wire the input
  handler last, after the BP functions it dispatches to are native.
- **`.uasset` save churn**. The editor re-serialises BP assets when a
  parent C++ class recompiles — cooked metadata / dependency hashes
  change even with no logic edit. Don't commit those `.uasset` diffs
  alongside source changes; let them accumulate and land in one
  end-of-session sweep commit. Otherwise the history looks like each
  C++ change touched every BP.
- **Interface `BlueprintImplementableEvent` const qualifier**. UE
  generates `Execute_<Method>` based on the exact signature; an
  unmatched `const` qualifier between interface declaration and BP
  implementation means the dispatcher silently doesn't generate. Pick
  one (typically `const` on data accessors) and keep it consistent.

## MCP tool reference

| Task | Tool |
|---|---|
| Asset inspection | `ue5_get_asset`, `ue5_list_assets`, `ue5_bp_list_variables` |
| Graph inspection | `ue5_bp_read_graph` |
| Full schema dump | `unreal.AssetExportTask` → `.t3d` via `/editor/exec_python` |
| Reparent | `ue5_reparent_blueprint` |
| New BP function | `ue5_bp_create_function` (empty graph) + `ue5_bp_add_logic` (body) |
| New BP variable | `ue5_bp_create_variable` |
| Node manipulation | `ue5_bp_add_logic` (create + wire), `ue5_bp_delete_node` (remove) |
| Whole-graph removal | `BlueprintEditorLibrary.remove_function_graph` / `remove_graph` (Python) |
| CDO defaults | Python: `get_default_object` + `set_editor_property` + `save_loaded_asset` |
| Compile / save / lint | `ue5_bp_compile`, `ue5_save_all`, `ue5_bp_lint` (single), `ue5_bp_lint_project` (sweep) |
| C++ build | `ue5_kill_editor` → `ue5_build target=editor` → `ue5_launch_editor` |
| Verify symbol | `unreal.load_class(None, '/Script/MyGame.UMyClass')` |

For multi-call sequences (e.g. dumping `.t3d` for 10 assets, or batch
property sets), use `ue5_batch` to avoid the per-call MCP round-trip overhead — **but `ue5_batch` accepts
only an allowlisted set of paths**; an unlisted/custom route returns `Unknown batch path` (hit on
`/bp/add_interface` this session).

### Custom / non-allowlisted bridge routes

Routes not exposed as a `ue5_*` MCP tool and not in the `ue5_batch` allowlist (e.g. `/bp/add_interface`,
`/bp/implement_interface_function`) are reached by POSTing **directly** to the bridge HTTP server:

```powershell
Invoke-RestMethod -Uri "http://localhost:6776/bp/add_interface" -Method Post `
    -Body (@{ blueprint="/Game/..."; interface="/Script/MyGame.MyInterface" } | ConvertTo-Json) `
    -ContentType "application/json"
```

Add the `[UnrealNGGMCP] AuthToken` value as a header if one is configured. The bridge runs the handler on
the game thread and returns when it completes.

### MCP batching patterns

Three places batching makes a noticeable difference:

**Inventory phase** — instead of 3 calls per BP × N BPs, one batch:

```
ue5_batch [
  { path: "/asset/get",        body: {asset_path: "/Game/...AC_X"} },
  { path: "/bp/list_variables",body: {blueprint: "/Game/...AC_X"} },
  { path: "/bp/read_graph",    body: {blueprint: "/Game/...AC_X"} },
  // … repeated per BP
]
```

**Batch `.t3d` export** via `/editor/exec_python`:

```python
assets_to_dump = [
    '/Game/Blueprints/AC_TraversalLogic',
    '/Game/Blueprints/SandboxCharacter_Mover',
    '/Game/Levels/LevelPrototyping/LevelBlock_Traversable',
]
out_dir = 'F:/<project>/Intermediate/StructDump'
for asset_path in assets_to_dump:
    asset = unreal.EditorAssetLibrary.load_asset(asset_path)
    if not asset:
        continue
    task = unreal.AssetExportTask()
    task.set_editor_property('object', asset)
    task.set_editor_property('filename', f"{out_dir}/{asset.get_name()}.t3d")
    task.set_editor_property('replace_identical', True)
    task.set_editor_property('automated', True)
    task.set_editor_property('prompt', False)
    unreal.Exporter.run_asset_export_task(task)
```

**Bulk CDO inspection** — `load_blueprint_class` + `get_default_object`
in a single Python loop. Useful when comparing N BPs' CDO overrides for
parity checks before/after a reparent.

### Context7 for plugin API surface

When a port crosses into a plugin module (Mover, Chooser, EnhancedInput,
PoseSearch, MotionWarping, GameplayCameras, AnimationLocomotionLibrary),
the API surface shifts between UE versions. Look it up rather than
guessing:

```
mcp__plugin_context7_context7__resolve-library-id
  libraryName="Unreal Engine"
  query="<specific API or class>"

mcp__plugin_context7_context7__query-docs
  libraryId="<resolved id, e.g. /websites/dev_epicgames_en-us_unreal-engine>"
  query="<deeper question>"
```

Worked examples from real ports:

| Need | Context7 query |
|---|---|
| Mover-aware montage playback | `UPlayMoverMontageCallbackProxy CreateProxyObjectForPlayMoverMontage` |
| Chooser table eval from C++ | `UChooserFunctionLibrary EvaluateChooser UE 5.7` |
| Enhanced-Input handler signatures | `UEnhancedInputComponent BindAction ETriggerEvent` |
| Sync BP to native parent after C++ change | `FKismetEditorUtilities ConformBlueprintFlagsAndComponents` |
| Walk class chain to find C++ ancestor | `UClass FindNativeParent` |

## Per-commit workflow

Two modes: **sequential** for risky / cross-class / runtime-dependent
changes, **batched** for linear additive stages on a single function.

### Sequential mode (one editor cycle per commit)

For every migration step:

1. `ue5_kill_editor`
2. Edit C++ (`.h` / `.cpp` / `.Build.cs`)
3. `ue5_build target=editor` — must say `Result: Succeeded`
4. `ue5_launch_editor`
5. (if BP-side changes) Use MCP/Python tools to modify the BP
6. `ue5_bp_compile blueprint=<...> save=true`
7. `ue5_bp_lint blueprint=<...>` — clean modulo the baselined FPs
8. `git add <specific paths>` (never `git add -A` — `.claude/` and
   transient state shouldn't land in commits)
9. `git commit` with a descriptive message naming the BP function /
   variable / event being migrated and the C++ symbol that replaced it.

Use this when: the change crosses class boundaries, depends on another
in-flight migration, or has a non-trivial runtime invariant you want
to confirm in PIE between commits.

### Batched mode (one editor cycle per N commits)

For a series of linear additive stages on one function (e.g. adding
sequential trace blocks):

1. `ue5_kill_editor`                            (1×)
2. Edit C++ × N                                 (no editor in the loop)
3. `ue5_build target=editor`                    (1×, incremental)
4. `ue5_launch_editor` + readiness poll         (1×)
5. `ue5_bp_compile blueprint=<...> save=true`   (1×, final verify)
6. `ue5_bp_lint blueprint=<...>`                (1×)
7. Commit-series — N atomic commits, using `git add -p <file>` to
   stage only the hunks that belong to each stage.

Use this when: the stages are linear (each one adds more, none changes
earlier semantics), no cross-class deps in flight, and the C++ symbol
surface stays stable across the series (Live Coding would work for the
intermediate bodies even if you don't use it).

**Anti-pattern**: editor cycle per commit when each commit only added
a block to an existing function body. Costs ~90 s per cycle to verify
something the C++ build already implied. The BP compile check belongs
at the *end* of the series, not between intermediate stages.

## Decision shortcuts

| Scenario | Right answer |
|---|---|
| Migrating a function that's called from multiple BPs by name | Thin-wrap the BP, migrate body to C++, leave the BP function as a 1-node forwarder |
| BP variable has data, C++ wants to read it | Lift via BP CDO override on a new C++ UPROPERTY with a different name |
| BP override of a parent UFUNCTION exists | Declare the C++ version as `BlueprintNativeEvent` + `_Implementation` so deleting the BP override falls through to C++ |
| BP event needs to fire from C++ | `UFUNCTION(BlueprintImplementableEvent)` on the C++ class; BP implements the event |
| BP function with no callers (post-migration) | `BlueprintEditorLibrary.remove_function_graph` |
| Whole `EventGraph` is dead | `BlueprintEditorLibrary.remove_graph` on the result of `find_event_graph` |
| BP has been "fixed" but compile is clean while lint screams | Verify it's a struct-node lint false-positive before reverting anything |

## Anti-patterns to avoid

- Migrating logic before the type pipeline is done. You'll write
  signatures that need to change later when types arrive.
- Big-bang commits that touch all six BPs at once. Keep migrations
  atomic (one function or one variable per commit) — easier to bisect,
  easier to revert, easier to review.
- Trying to handle name conflicts with `meta=(...)` hacks. Either rename
  (one side) or use `BlueprintNativeEvent` (intentional override).
- Letting `.claude/` or other local-state directories slip into commits
  via `git add -A`. Stage explicit paths.
- Auto-fixing lint without reading what the lint is actually flagging.
  Struct nodes look broken to the linter but are fine; auto-fix would
  delete legitimate execution paths.
- Restarting the editor per commit when the C++ build already verified
  the change. Compile success on incremental stages is a sufficient
  proof of structural validity; defer BP compile to the end of the
  stage series.
- Committing a skeleton + 1-line stub as its own commit. If it doesn't
  do measurable work yet, fold it into the next substantive stage —
  history shouldn't have placeholder commits.
- Writing the C++ stub for a BP function before reading its downstream
  graph. This is how IA_Jump was reverted in the GameAnimationSample
  port — the BP body did Jump-or-Traverse and the C++ replaced it with
  unconditional `Jump()`. Pre-flight always.

## Reference: real conversion from this repo

| Commit | Step |
|---|---|
| Reparent six BPs to empty C++ parents | Phase 3 |
| Add C++ USTRUCT/UENUM mirrors of `S_*` / `E_*` BP types | Phase 2 |
| Add C++ UINTERFACE mirrors of `BPI_*` | Phase 2 |
| Add `PoseSearch` to `Build.cs`, add `Get_PoseHistory` | Phase 2 |
| `UPreMovementTickComponent::TickComponent` + delegate | Phase 4 (additive) |
| `ApplyVisualOverrideClass` helper on visual-override component | Phase 4 (additive helper) |
| `TeleportForward` on PlayerController | Phase 4 (additive) |
| GM_Sandbox data lift + `GetDefaultPawnClassForController_Implementation` | Phase 5 |
| `CycleToNextPawn` / `CycleToNextVisualOverride` | Phase 4 |
| `BeginPlay` subscribes to `OnDataDrivenCVarDelegate` | Phase 4 |
| `ResetAllPlayersNative` + remove BP `ResetAllPlayers` | Phase 4 + 6 |
| Remove BP `GetDefaultPawnClassForController` BP override | Phase 6 |
| Thin-wrap BP `CyclePawn` / `CycleVisualOverride` | Phase 6 |
| `SetupInputComponent` + remove PC_Sandbox `EventGraph` | Phase 4 + 6 |

Result: from "Blueprint-only sample" → C++/BP hybrid with all sandbox
gameplay routed through native code in ~17 atomic commits over one
session. Each commit had a green build and clean lint.
