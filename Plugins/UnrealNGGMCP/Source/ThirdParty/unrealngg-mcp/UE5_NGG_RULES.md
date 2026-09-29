# UE5 NGG — MCP Workflow Rules

These rules ship with the `ue5-ngg` plugin. The MCP sidecar (`index.js`) loads
this file at startup and splits it (see `rules.js`):

- everything up to rule 6 — the rules that apply no matter what you are doing —
  is sent as the server's `instructions`, injected into the model's system
  context on every connection;
- the subsystem chapters (rules 6-14, the cheat sheet, the Blueprint schema
  reference) are served as MCP **resources** under `ue5-ngg://rules/…` and read
  on demand, so they cost nothing until the work actually touches them.

Either way any project using this plugin gets the rules automatically, on any
PC, without per-project CLAUDE.md edits or other setup.

To add, change, or remove a rule: edit this file, then have the user run
`/mcp` (or restart their MCP client) so the sidecar reloads.

**Section headings are load-bearing.** `rules.js` slices this document at the
headings listed in its `SECTION_SPECS`. Renaming or reordering one of those
headings makes the sidecar fall back to sending the whole file (nothing is
lost, but the handshake gets big again) — update `SECTION_SPECS` to match, and
`rules.test.js` will confirm the split still works.

---

## What this server does

This MCP server is the bridge to a running Unreal Engine 5 editor. It exposes
tools for inspecting and editing assets, Blueprints, components, levels,
materials, Niagara systems, UMG widgets, and meshes. The editor's asset
registry is the source of truth for any project state question.

---

## Rules whenever this server is connected

### 1. MCP-first for editor work

For asset / Blueprint / material / component inspection or mutation, ALWAYS
use the `ue5_*` tools (`ue5_list_assets`, `ue5_get_asset`,
`ue5_bp_read_graph`, `ue5_create_blueprint`, `ue5_create_blueprint_struct`,
`ue5_create_blueprint_enum`, `ue5_create_blueprint_interface`,
`ue5_create_anim_blueprint`, `ue5_add_component_to_blueprint`,
`ue5_remove_component_from_blueprint`, `ue5_bp_add_logic`,
`ue5_bp_create_variable`, `ue5_bp_create_function`, `ue5_bp_create_macro`,
`ue5_create_material_instance`, etc.).

Do NOT use shell `find` / `ls` / `grep` over `Content/`, and do NOT read
`.uasset` files from disk — those are opaque binaries and won't tell you
parameters, dependencies, or the editor's view of an asset. Filesystem reads
of `Content/` are only acceptable when (a) the editor is unreachable AND (b)
the user is specifically asking about disk state.

Source code under `Source/` and `Plugins/**/Source/` is normal text and uses
normal file tools — this rule is about content, not C++.

**Editor lifecycle is also MCP-first.** Building, killing, launching, saving,
and probing the editor go through these tools — never raw shell commands:

| Operation | Use this | Do NOT use |
|---|---|---|
| Compile the project | `ue5_build` (target=editor or game) | `Build.bat`, `dotnet UnrealBuildTool.dll`, `cmd /c …Build.bat`, manual MSBuild calls |
| Stop the editor | `ue5_kill_editor` | `taskkill /IM UnrealEditor.exe`, `Stop-Process -Name UnrealEditor`, `Get-Process … | kill` |
| Start the editor | `ue5_launch_editor` | `Start-Process *.uproject`, double-click prompts, `cmd /c start`, raw `UnrealEditor.exe` invocation |
| Save dirty assets | `ue5_save_all` | filesystem writes, asking the user to click Save |
| Verify the editor is alive | `ue5_health_check` | port probes via `curl`, `Test-NetConnection`, `netstat` parsing |

The MCP versions handle Windows-specific concerns the plain shell calls miss:
`ue5_kill_editor` clears the Live Coding mutex (a `taskkill` leaves it stuck);
`ue5_launch_editor` goes through `UnrealVersionSelector` so the right engine
binds to the `.uproject`; `ue5_build` blocks correctly on Live Coding and
returns a structured pass/fail with the trailing log lines. Shelling out
loses all of that.

The standard rebuild sequence after a C++ change is:
`ue5_save_all` → `ue5_kill_editor` → `ue5_build` → `ue5_launch_editor` →
`ue5_health_check` (poll until the bridge answers). Don't substitute pieces
of this with shell commands.

### 2. Health-check first if unsure

If a session has just started, the editor was just rebuilt, or any tool
returned `Bridge not reachable`, call `ue5_health_check` before proceeding.
If it fails, the editor isn't running — ask the user to launch the editor
rather than falling back to filesystem inspection.

### 3. Color / visual swaps: parameterize, don't proliferate

For any runtime color or visual swap, use **one** parameterized parent
material plus a Dynamic Material Instance at runtime — NOT N color-variant
MaterialInstance assets.

**The default `/Engine/BasicShapes/*` materials have NO parameters.**
Calling `SetVectorParameterValue` on a DMI built from one of those is a
silent no-op — the box won't actually change color. You must create or
assign a parent material that exposes a parameter first.

Canonical recipe (full end-to-end):

```
1. ue5_create_material with use_parameters:true
   → creates a UMaterial with VectorParameter "BaseColor" and
     ScalarParameter "BrightnessMultiplier" wired to the BaseColor pin.
   Pass blueprint_path + component_name in the same call to also assign
   it as override material slot 0 on the component (one round-trip).

2. In the BP's BeginPlay event:
   - call_function CreateDynamicMaterialInstance on the component, store
     return in a MaterialInstanceDynamic variable.

3. To swap color (per-frame, on event, etc.):
   - call_function SetVectorParameterValue on the DMI, with:
       ParameterName: "BaseColor"   ← MUST match the parent material's parameter name
       Value:         (R=1.0, G=0.0, B=0.0, A=1.0)
```

**Parameter names are exact-match.** The parent material created by
`ue5_create_material` exposes `BaseColor` (not `Color`, not `Base Color`).
If you create your own parent material via the editor, use whatever name
you defined there — but stick to one canonical name per project.

Authoring `MI_Red` + `MI_Blue` (or similar variant instances) just to switch
between `SetMaterial` calls is wasteful and unidiomatic. Variant MIs are only
justified when variants differ in more than a single parameter (different
textures, shading models, etc.).

### 4. Save after mutating

Most write tools mention `ue5_save_all` as the next step — call it before
declaring a task done so the `.uasset` is on disk and survives editor
restarts.

### 5. Close the asset before mutating it through MCP

Before running any MCP tool that modifies an existing asset (sockets, blueprint
defaults, components, properties, graph nodes, niagara params, anim BP wiring,
mesh re-bakes over an existing path, etc.), the asset must **not be open in
the editor**. If it is open, ask the user to close that tab first — or run
`ue5_save_all` and have them close it — before issuing the MCP call.

**Why:** the open asset editor (StaticMesh editor, Persona, BlueprintEditor,
WidgetBlueprint editor, etc.) holds a working copy in memory and a UI
representation that does **not** auto-refresh when the underlying UObject is
mutated from outside. Two concrete failure modes:

- **Stale UI:** an MCP-added socket / variable / component won't appear in
  the open editor panel until the user closes and re-opens the asset.
- **Clobbered edits:** if the user has unsaved manual edits in the open
  editor and MCP mutates the same UObject, whichever side saves last wins —
  silently losing the other side's work.

Memory corruption is **not** a risk: every MCP handler dispatches to the Game
Thread via `AsyncTask`, so it can't race with editor input on the same array.
This rule is purely about UI staleness and unsaved-edit conflicts.

**How to apply:**
1. Before mutating, ask the user "is `<asset_path>` currently open in the
   editor? If yes, please close it (saving any unsaved changes first)."
2. Run the MCP mutation only after confirmation.
3. Tell the user they can re-open the asset to see the result.

Skip this rule for asset *creation* tools (`ue5_create_*`) where the asset
doesn't yet exist, and for handlers that don't touch saved `.uasset` state
(`ue5_list_*`, `ue5_get_*`, health checks, mesh handles in
`/mesh/create`+`/mesh/append_primitive`+`/mesh/bake_static` first-time bakes).

### 6. Custom events vs override events

Use `type="custom_event"` for self-defined Blueprint events (`MoveUp`,
`MyHelper`, etc.). Use `type="event"` only for actual parent-class overrides
(`ReceiveBeginPlay`, `ReceiveTick`, etc.).

The bridge auto-aliases common short forms — `BeginPlay` → `ReceiveBeginPlay`,
`Tick` → `ReceiveTick`, `EndPlay`/`ActorBeginOverlap`/`Hit`/`AnyDamage` etc. —
so passing the short form is fine.

Passing a non-override name that doesn't auto-alias is refused at the bridge
— historically it created Blueprints that crashed the editor in
`FixOverriddenEventSignature` on the next compile. Use `custom_event` for
self-defined names.

Similarly, common Actor function display names auto-alias to their internal
C++ names: `GetActorLocation` → `K2_GetActorLocation`, `SetActorLocation` →
`K2_SetActorLocation`, `AddActorLocalOffset` → `K2_AddActorLocalOffset`,
`AddActorWorldOffset` → `K2_AddActorWorldOffset`,
`AddActorLocalRotation` → `K2_AddActorLocalRotation`,
`AddActorWorldRotation` → `K2_AddActorWorldRotation`,
`DestroyActor` → `K2_DestroyActor`, `AttachToActor`/
`AttachToComponent`/`TeleportTo` → `K2_*`, `GetWorld` → `K2_GetWorld`. For
SceneComponent: `GetWorldLocation` → `K2_GetComponentLocation` etc. You can
pass either form — the bridge resolves and binds the correct UFunction.

### 7. Always use `_DoubleDouble` math operators, never `_FloatFloat`

UE5.0+ uses `double` (64-bit) as the default scalar. Every arithmetic
operator on `KismetMathLibrary` was renamed accordingly.

```
PREFER:  Multiply_DoubleDouble  Add_DoubleDouble  Less_DoubleDouble
AVOID:   Multiply_FloatFloat    Add_FloatFloat    Less_FloatFloat
```

Full rename table: `Add`, `Subtract`, `Multiply`, `Divide`, `EqualEqual`,
`NotEqual`, `Less`, `Greater`, `LessEqual`, `GreaterEqual`, `Percent`,
`MultiplyMultiply` — all `_FloatFloat` → `_DoubleDouble`. Abs/FMin/FMax/
FClamp/Lerp/Sin/Cos/VSize/etc. did **not** change names.

**The tool auto-corrects `_FloatFloat` → `_DoubleDouble` and `EventTick` →
`ReceiveTick` (and similar `Event*` → `Receive*` override names) before
sending to the bridge — so these mistakes no longer cause a 400 error or
a retry. Still prefer the correct names to keep the spec readable.**

### 8. Use `UE5_BP_FUNCTION_NAMES.md` for function name lookups

When a `ue5_bp_add_logic` call fails with "function not found", consult
`Plugins/UnrealNGGMCP/Source/ThirdParty/unrealngg-mcp/UE5_BP_FUNCTION_NAMES.md` before guessing. That file
maps every common Blueprint display name to its internal UFunction name (K2_
prefix, renames, deprecated aliases), flags which functions are Pure (no exec
pin), and includes a quick-diagnosis checklist.

### 9. The tool descriptions and this rules file are the CANONICAL contract

Do NOT grep the `Plugins/UnrealNGGMCP/.../*.cpp` plugin source or the
`Plugins/UnrealNGGMCP/Source/ThirdParty/unrealngg-mcp/*.js` sidecar source to figure out how a `ue5_*` tool
works. Every `ue5_*` tool's `description` field plus the schema reference
below is the authoritative spec. If something looks underspecified, ask the
user — don't burn time reading 2000-line C++ files. The plugin source can
change between versions; its surface contract cannot.

The only legitimate reason to read plugin source is when the user explicitly
asks you to fix a bug **in the plugin itself**, not when you're using its
tools to do editor work.

### 10. Split big Blueprint logic into small single-purpose functions

The Event Graph should orchestrate, not implement. Long node chains in
`EventGraph` are unreadable, untestable, and impossible to reuse. When
authoring non-trivial logic, break it into named function graphs and have
the Event Graph call them.

**Threshold heuristic.** If a single coherent logical step needs more than
~6–8 exec-bearing nodes (counting `call_function`, `branch`, `cast`,
`sequence`, `variable_set`; pure data nodes like `variable_get`, math, or
`GetActorLocation` don't count), extract it into its own function. Below
the threshold, inline it.

**Workflow — 3 steps per extracted function:**

1. `ue5_bp_create_function` with typed `inputs` / `outputs`, plus
   `is_pure: true` for data-only helpers (no side effects, no exec pins).
   Returns a `graph_name`.
2. `ue5_bp_add_logic` with `graph: <graph_name>` to populate the body.
   This is what the new function actually does.
3. Call it from the Event Graph (or another function) with
   `call_function` + `self_context: true` — the function name resolves
   against the BP's own graphs.

**One function = one verb.** Good names: `ValidateInput`, `SpawnBomb`,
`ApplyDamage`, `ComputeArc`, `RefreshHUD`, `IsPhaseActive` (pure).
Bad names: `DoStuff`, `HandleBeginPlay`, `Update`, `Process`. If you
can't pick a verb without saying "and", split further.

**Don't pre-decompose.** Trivial handlers ("read var → call one
function → done") stay inline. Splitting those just adds noise. The
rule fires when logic gets non-trivial, not as a default for every
event.

**Why this matters in BP specifically.** Function graphs are the BP unit
of reuse and inspection — the Functions panel in the editor lets the
team see the call surface, and `ue5_bp_read_graph` returns each function
graph separately. A 40-node Event Graph hides intent; five 8-node
functions plus a 6-node orchestration Event Graph reveal it.

### 11. Blueprint code style — readable, structured, maintainable

A Blueprint that compiles is not done. A Blueprint that another developer can
open and understand within seconds is done. Every graph this server authors is
held to that bar — readability is a correctness requirement, not a stylistic
preference. **This rule binds every `ue5_bp_add_logic` (and BT / function /
macro graph) call unconditionally. Producing a tangled or unlabeled graph is a
bug, not a shortfall — fix the layout before declaring work done.**

**Layout — left-to-right execution flow.** `ue5_bp_add_logic` accepts a
`position: { x, y }` object per node — use it. Do NOT leave nodes piled at
(0,0); that's the auto-layout fallback and produces an unreadable wall.

Reasonable starting grid:
- Triggers (events, custom events, component events) on the far left at `x = 0`.
- Each subsequent exec node advances `x` by ~300–400. A typical 5-node exec
  chain ends around `x = 1500`.
- Main exec row sits at `y = 0`. Pure data nodes (`variable_get`, math,
  `GetActorLocation`, etc.) feeding a consumer go slightly above-and-left at
  `y = -160`. Parallel / secondary exec branches go at `y = +200`.
- Nodes that share a logical row stay aligned on `y`. Vertical alignment is
  the single biggest readability win.

The Event Graph should read like a horizontal timeline of cause → effect.

**Wire hygiene.** Short, non-crossing wires only. When two exec wires would
cross, insert a `knot` (reroute) node and route one of them through it.
Wires spanning more than ~700 px horizontally signal that the layout is wrong
— either reposition the consumer or extract a function.

**Naming.**
- Variables and functions describe purpose, not type. `WheelVelocity`,
  `CurrentTargetActor`, `IsGrounded` — never `Var1`, `MyFloat`, `BoolCheck`,
  `Temp`, `Result2`.
- Booleans use ONE consistent prefix: `Is*`, `Has*`, or `Can*`. `IsAlive`,
  `HasAmmo`, `CanThrow`. Not `Alive`, `Ammo`, `Throw`, `BFlag`.
- Functions are verbs (`SpawnBomb`, `RefreshHUD`, `ComputeArc`); pure helpers
  are noun-phrase predicates (`IsPhaseActive`, `GetNearestEnemy`).
- Component variable names match what the design refers to: `BombMesh`,
  `MuzzleArrow`, `HealthBar` — not `StaticMesh1`, `Arrow`, `ProgressBar_0`.

**Comments group sections, never narrate nodes.** Use `K2Node_Comment` boxes
to label *regions* of the graph — "Input handling", "Aim calculation",
"Apply damage", "Spawn FX". Never comment what an individual node does — the
node's name already does that. Comments are visual segmentation, not running
commentary. A small graph with no comments is fine; a graph with a comment on
every node is wrong.

**Avoid Event Tick for any logic that has a better trigger.** Tick is the
wrong tool for state machines, periodic checks, AI decisions, animation
polling, UI refreshes, or anything event-driven. Use instead:
- Events / delegates for one-shot reactions (`OnHit`, `OnPhaseChanged`,
  GAS attribute change delegates, multicast delegates on components).
- `SetTimerByEvent` / `SetTimerByFunctionName` for periodic work.
- Latent tasks (`MoveComponentTo`, ability tasks, Timeline) for long-running
  smooth operations.

The only legitimate Event Tick uses are per-frame interpolation that genuinely
needs every frame (smooth camera follow, projectile arc preview drawing).
Even those should disable themselves when not needed
(`SetActorTickEnabled(false)`).

**Separation of concerns.** Input, physics, UI updates, AI decisions, and
gameplay state changes each get their own function or component. A function
that handles a button press AND mutates HP AND plays VFX is doing three
things; split it. This complements Rule 10 — Rule 10 says *split when long*,
this rule says *split when responsibilities differ even if short*.

**Modularity.** Anything reusable — even speculatively — goes in a function
(`is_pure: true` for read-only data helpers). The Functions panel becomes the
Blueprint's API surface; `ue5_bp_read_graph` returns each function graph
separately, so reviewers and tools see the call surface immediately.

**When the graph stays complex after all of the above, escape to C++.**
Blueprints excel at high-level orchestration, prototyping, and
designer-tunable parameters. They're a poor fit for dense per-frame math,
complex state machines, anything that needs unit testing, or anything
performance-critical. If a single function still has 20+ nodes after
decomposition, propose a C++ implementation rather than authoring it as a
Blueprint. The Blueprint can stay as a thin wrapper that calls into the C++.

---

### 12. AI logic — Behavior Trees and Blackboards

UE5 AI lives across two assets: a **Blackboard** (typed shared key/value
store) and a **Behavior Tree** (decision graph). The bridge covers
both assets *and* the graph wiring inside the BT.

**Standard authoring order:**

1. `ue5_bt_create_blackboard` — `BB_Foo` at e.g. `/Game/AI/BB_Enemy`.
2. `ue5_bt_add_blackboard_keys` — populate it. Supported types:
   `Bool`, `Int`, `Float`, `Vector`, `Rotator`, `Object`, `Class`,
   `Enum`, `Name`, `String`. Pass `base_class` for `Object`/`Class`
   (e.g. `"Pawn"`, `"/Script/Engine.Actor"`, or a BP path); pass
   `enum_path` for `Enum` (UEnum asset path).
3. `ue5_bt_create_tree` — `BT_Foo` at e.g. `/Game/AI/BT_Enemy`. Pass
   `blackboard_path` to link the BB; that is the same field the editor
   exposes as the BT root's "Blackboard Asset" picker.
4. `ue5_bt_add_logic` — wire the tree (composites, tasks, decorators,
   services, parent/child links) in **one call**. See schema below.
5. (Optional) `ue5_bt_read_tree` to round-trip the graph back as JSON
   for inspection or further edits.

**Custom task / decorator / service classes — use the existing BP tool.**
BP-derived AI nodes are just normal Blueprint subclasses:

```jsonc
ue5_create_blueprint {
  asset_path:  "/Game/AI/BTT_AttackTarget",
  parent_class: "BTTask_BlueprintBase"   // or BTDecorator_BlueprintBase / BTService_BlueprintBase
}
```

UE picks these up automatically in the BT editor's "Add Task" /
"Add Decorator" / "Add Service" submenus — no registration step.
Reference them in `ue5_bt_add_logic` by their content path:
`task_class: "/Game/AI/BTT_AttackTarget"`.

#### `ue5_bt_add_logic` schema

```jsonc
{
  "behavior_tree": "/Game/AI/BT_Patrol",
  "clear": false,                       // wipe non-Root nodes first; default false → 409 if tree non-empty
  "nodes": [
    { "id": "sel",   "type": "composite", "composite": "Selector" },
    { "id": "seq",   "type": "composite", "composite": "Sequence" },
    { "id": "wait",  "type": "task", "task_class": "BTTask_Wait",
      "properties": { "WaitTime": 2.0 } },
    { "id": "move",  "type": "task", "task_class": "BTTask_MoveTo",
      "blackboard_key": "PatrolPoint",
      "properties": { "AcceptableRadius": 50.0 } },
    { "id": "atk",   "type": "task", "task_class": "/Game/AI/BTT_Attack" }
  ],
  "decorators": [
    { "parent": "sel", "decorator_class": "BTDecorator_Blackboard",
      "blackboard_key": "IsAlerted" }
  ],
  "services": [
    { "parent": "sel", "service_class": "BTService_DefaultFocus",
      "blackboard_key": "TargetActor",
      "properties": { "Interval": 0.5 } }
  ],
  "connections": [
    { "from": "root", "to": "seq" },
    { "from": "seq",  "to": "sel" },
    { "from": "sel",  "to": "atk"  },
    { "from": "sel",  "to": "move" },
    { "from": "sel",  "to": "wait" }
  ]
}
```

**Schema rules — read once, internalize:**

- **`"root"` is reserved.** It refers to the BT's auto-created
  `UBehaviorTreeGraphNode_Root`. Don't declare it in `nodes[]`. Always
  start `connections[]` with one `{ from: "root", to: "<your-top-node>" }`.
- **Composite aliases:** `Selector`, `Sequence`, `SimpleParallel`
  (case-insensitive). These map to `UBTComposite_Selector` /
  `UBTComposite_Sequence` / `UBTComposite_SimpleParallel`.
- **Task / decorator / service class resolution:** short names
  (`BTTask_MoveTo`, `BTDecorator_Blackboard`, `BTService_DefaultFocus`)
  resolve against `/Script/AIModule`. Use full paths
  (`/Script/Module.Class` or `/Game/.../BP_X`) when needed. BP classes
  auto-append `_C` if missing.
- **Sibling order = order in `connections[]`.** The first connection
  out of a parent makes that child the leftmost / first-to-execute.
  Selectors fall through left-to-right; sequences run left-to-right.
- **`blackboard_key` is shorthand for `BlackboardKey.SelectedKeyName`.**
  It only applies to `*_BlackboardBase` derivatives (MoveTo, Wait
  variants, RotateToFaceBBEntry, Blackboard decorator,
  DefaultFocus service, …). Silently ignored on nodes without that
  field.
- **`properties` is a UPROPERTY dict** — `{ name: value }` pairs
  applied via reflection. Use it for `WaitTime`, `AcceptableRadius`,
  `Interval`, and any other tunable. Numbers, strings, and booleans
  work directly; complex types follow the same rules as the rest of
  the bridge's reflection helpers.
- **`FValueOrBBKey_*` types (UE5.4+)** — built-in tasks like
  `BTTask_Wait`, `BTTask_MoveTo`, and `BTService_DefaultFocus` wrap
  their tunables in tagged-union structs that accept either a literal
  value or a Blackboard key reference. The bridge handles both:
    - `"WaitTime": 1.5`   → literal value (writes the struct's
      `DefaultValue` field).
    - `"WaitTime": "@PatrolSpeed"` → bind to a Blackboard key (writes
      the struct's `Key` field). The `@` prefix is the only signal —
      use it any time a property's underlying type is
      `FValueOrBBKey_*` and you want to read from the Blackboard at
      runtime instead of using a fixed value.
  `ue5_bt_read_tree` round-trips these by emitting either the literal
  scalar or the `"@<keyname>"` form, never the wrapper struct itself.
- **Decorators and services are sub-nodes** — they belong to a
  `parent`, not to `nodes[]`. They never appear in `connections[]`.
- **Tasks are leaves** — they have an input pin only. A `connection`
  TO a task is fine; a `connection` FROM a task is rejected with a
  warning (you'll see "has no output pin" in the response).
- **Pre-built tasks worth knowing** (in `/Script/AIModule`):
  `BTTask_Wait`, `BTTask_MoveTo`, `BTTask_RunBehaviorDynamic`,
  `BTTask_RotateToFaceBBEntry`, `BTTask_FinishWithResult`,
  `BTTask_PlaySound`, `BTTask_PlayAnimation`, `BTTask_RunEQSQuery`.
- **Pre-built decorators:** `BTDecorator_Blackboard`,
  `BTDecorator_Cooldown`, `BTDecorator_Loop`, `BTDecorator_TimeLimit`,
  `BTDecorator_ConditionalLoop`, `BTDecorator_ForceSuccess`,
  `BTDecorator_IsAtLocation`, `BTDecorator_ReachedMoveGoal`.
- **Pre-built services:** `BTService_DefaultFocus`,
  `BTService_RunEQSQuery`.

**Idempotency.** All Phase 1 + 2 tools are safe to re-run:
- `ue5_bt_create_tree` / `ue5_bt_create_blackboard` return
  `already_existed: true` if the asset exists. `_create_tree` will
  refresh the blackboard link if `blackboard_path` is supplied.
- `ue5_bt_add_blackboard_keys` skips name-collisions and reports them.
- `ue5_bt_add_logic` refuses by default if the BT already has authored
  nodes (returns 409). Pass `clear: true` to wipe and re-author.

**Compile / save.** `compile: true` (default) calls
`UBehaviorTreeGraph::UpdateAsset` to mirror the EdGraph into the
runtime tree. `save: true` (default) writes the package. Skip both
during iteration only if you plan to chain more edits.

**Running the tree at game-time** is a Blueprint/C++ concern, not an
asset-authoring one — typically `RunBehaviorTree(BT)` on an
`AAIController` after `UseBlackboard(BB, ...)`. The bridge stops at
asset-level work.

---

### 13. Gameplay Ability System (GAS) — single player, multiplayer, and AI

GAS is UE5's framework for abilities (active skills), gameplay effects (stat
modifiers, buffs/debuffs), and attribute sets (HP, Stamina, etc.). It works
identically across all three modes; what changes is the **replication mode**
on the ASC and the **net execution policy** on each ability.

---

#### 11.1 Project setup (do this before authoring any GAS assets)

**Build.cs** — add to `PublicDependencyModuleNames`:
```csharp
"GameplayAbilities", "GameplayTags", "GameplayTasks"
```
`GameplayTasks` is required transitively; omitting it causes linker errors on
ASC methods. `GameplayTags` is needed for `FGameplayTag` / `FGameplayTagContainer`.

**`.uplugin`** — if your plugin uses GAS, declare the runtime dependency:
```json
{ "Name": "GameplayAbilities", "Enabled": true }
```
Without this, the plugin DLL fails to load at runtime with `GetLastError=126`
even though it compiled fine. The dependency is needed so the plugin loader
finds `UnrealEditor-GameplayAbilities.dll` (in the engine plugins tree, not
`Binaries/Win64/`).

---

#### 11.2 Core class relationships

```
Actor (owner)
 ├── UAbilitySystemComponent (ASC)
 │    ├── TArray<UAttributeSet*>          — attribute data (Health, Mana, …)
 │    ├── TArray<FActiveGameplayEffect>   — applied/ticking effects
 │    ├── TArray<FGameplayAbilitySpec>    — granted abilities
 │    └── FGameplayTagCountContainer      — active gameplay tags
 └── implements IAbilitySystemInterface
      └── GetAbilitySystemComponent() → returns ASC pointer
```

Every GAS actor must implement `IAbilitySystemInterface`. GAS internals (effect
application, targeting, ability queries) use that interface to find the ASC
without knowing the concrete class. Forgetting this interface means GAS
internally can't locate the ASC and silently skips operations.

**Where to put the ASC:**

| Actor type | ASC lives on | Reason |
|---|---|---|
| Player (can respawn) | `APlayerState` | ASC persists across pawn respawn; abilities/attributes survive death |
| AI / NPC | The `ACharacter` itself | No respawn; simpler; one owner/avatar |
| Non-character (turret, item) | The actor itself | Owner == Avatar, initialization trivial |

When ASC owner ≠ avatar (PlayerState case), both must call
`InitAbilityActorInfo` — see §11.3.

---

#### 11.3 Runtime setup workflow (C++ — authoritative order)

The MCP tools handle **asset authoring**. At runtime these steps must happen
in this exact order:

```
1. Constructor:  CreateDefaultSubobject<UAbilitySystemComponent>(...)
                 ASC->SetIsReplicated(true)
                 ASC->SetReplicationMode(...)
                 CreateDefaultSubobject<UMyAttributeSet>(...)   ← auto-registered

2. Server:  PossessedBy(Controller)
              → InitAbilityActorInfo(OwnerActor, AvatarActor)
              → GiveStartupAbilities()     ← HasAuthority() guard
              → ApplyStartupEffects()      ← HasAuthority() guard

3. Client:  OnRep_PlayerState()            ← NOT BeginPlay — PS replicates late
              → InitAbilityActorInfo(OwnerActor, AvatarActor)
              (abilities/effects replicate from server; do NOT grant on client)
```

**`InitAbilityActorInfo` is the critical call.** Call it before any
`GiveAbility` or `ApplyGameplayEffect`. If the ASC and the pawn are on the
same actor, `InitAbilityActorInfo(this, this)` in `BeginPlay` is enough. If
the ASC is on PlayerState, you must call it in both `PossessedBy` (server)
and `OnRep_PlayerState` (client).

**Grant abilities — server only:**
```cpp
if (!HasAuthority()) return;
FGameplayAbilitySpec Spec(AbilityClass, 1 /*level*/);
AbilitySystemComponent->GiveAbility(Spec);
```
`GiveAbility` returns an `FGameplayAbilitySpecHandle`. Cache it if you need
to remove or force-activate by handle later. Granted specs replicate to the
owning client automatically in `Full`/`Mixed` mode.

**Apply startup effects — server only:**
```cpp
FGameplayEffectContextHandle Ctx = ASC->MakeEffectContext();
Ctx.AddSourceObject(this);
FGameplayEffectSpecHandle Spec = ASC->MakeOutgoingSpec(EffectClass, 1, Ctx);
ASC->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());
```
Startup effects are typically `Infinite` duration — used to initialize base
attribute values (MaxHealth, BaseSpeed, etc.).

---

#### 11.4 Replication modes (set on ASC in constructor)

| Mode | What replicates to simulated proxies | Use for |
|---|---|---|
| `Full` | Every GE replicated to all clients | Singleplayer / listen-server co-op |
| `Mixed` | GEs to owner only; tags+attributes to all | **Player characters on dedicated server** |
| `Minimal` | Tags and attributes only; no GE details | **AI pawns, minions, non-player actors** |

`Mixed` and `Minimal` require the actor's `Owner` to be the `PlayerController`.
PlayerState sets its own owner to the controller automatically — this is
usually correct without extra code.

---

#### 11.5 Net execution policies (set on UGameplayAbility CDO)

| Policy | Where ability runs | Use for |
|---|---|---|
| `LocalPredicted` | Client immediately + server confirms | Player input abilities — attack, dash, throw (feels instant) |
| `LocalOnly` | Owning client only, never server | Cosmetic only — camera shake, client VFX. **No game state mutation.** |
| `ServerInitiated` | Server starts it, owning client mirrors | Server-triggered abilities the client should also run |
| `ServerOnly` | Server only, results replicate | AI abilities; trusted-server-only logic |

**LocalPredicted flow:**
1. Client calls `TryActivateAbility` → runs locally with a prediction key.
2. GAS sends an RPC to the server with that key.
3. Server runs `CanActivateAbility` independently. If it passes, confirms the
   key; if it fails, sends a rejection and the client rolls back.
4. Target data (hit location, target actor) travels from client to server via
   `AbilityTask_WaitTargetData` or `ServerSetReplicatedTargetData`. Never
   assume the server already knows what the client aimed at.

---

#### 11.6 Instancing policies

| Policy | Instance | State | Use for |
|---|---|---|---|
| `NonInstanced` | Never — CDO reused | No per-execution state | Stateless, synchronous, high-frequency passives |
| `InstancedPerActor` | Once per grant | Persists between activations | **Default for everything** — stateful, safe to use delegates/tasks |
| `InstancedPerExecution` | New per activation | Fully isolated | Abilities that stack simultaneously on the same actor (rare) |

Start with `InstancedPerActor`. Only use `NonInstanced` when you have a
measured performance reason — it cannot store UProperties between activations,
cannot bind delegates, and cannot use latent `AbilityTask`s.

---

#### 11.7 Gameplay Effects — full reference

**Duration types:**

| Type | Modifies | Persists until | Can grant tags |
|---|---|---|---|
| `Instant` | **Base value** permanently | Removed immediately after apply | No |
| `HasDuration` | **Current value** temporarily | Duration expires or manual removal | Yes |
| `Infinite` | **Current value** indefinitely | `RemoveActiveGameplayEffect` called | Yes |

`Instant` changes the base — use it for damage and permanent stat changes.
`HasDuration`/`Infinite` change the current modifier layer — they revert
when removed.

**Modifier operations:**

| Operation | Formula | Use for |
|---|---|---|
| `Add` | `Attr += Magnitude` | Flat bonus (+50 health) |
| `Multiply` | `Attr *= Magnitude` | Percentage modifier (1.2 = +20%) |
| `Override` | `Attr = Magnitude` | Force-set a value |

**Multiply stacking gotcha:** Multiple `Multiply` modifiers on the same
attribute are **summed** before multiplying — `Base * (Mod1 + Mod2)`, not
`Base * Mod1 * Mod2`. For chained multiplication use a
`GameplayEffectExecutionCalculation` (GEExecCalc).

**Period (tick):** Setting `Period > 0` on `HasDuration`/`Infinite` causes
the GE to apply an Instant-equivalent modification every `Period` seconds —
this is how DoT and HoT work.

**Stacking modes:**

| Mode | Behavior |
|---|---|
| `None` | Each application is independent |
| `AggregateBySource` | Stack count grouped per source actor |
| `AggregateByTarget` | Stack count grouped on target (typical for DoT) |

With stacking set also configure: `StackLimitCount`, `StackDurationRefreshPolicy`,
`StackPeriodResetPolicy`.

**Granted tags:** `HasDuration`/`Infinite` GEs can grant tags to the target
ASC while active. Tags vanish when the GE is removed. Standard pattern:
Stun GE grants `Status.Stunned`; abilities with `ActivationBlockedTags =
[Status.Stunned]` refuse to activate while the tag is present.

**Immunity:** Add `GrantedApplicationImmunityTags` to an active GE — any
incoming GE carrying those tags is blocked entirely.

**Modifier attribute format for `ue5_gas_create_effect`:**
```
"attribute": "AttributeSetClassName.PropertyName"
e.g. "AS_Character.Health"
```
The attribute set Blueprint must be loaded in the editor at call time for
resolution to succeed. If resolution fails, the modifier is created with an
unbound ref (warning returned) and must be corrected in the editor.

---

#### 11.8 Attribute set best practices

**Declaration pattern (C++):**
```cpp
// ATTRIBUTE_ACCESSORS macro generates Get/Set/Init accessors + FGameplayAttribute() getter
#define ATTRIBUTE_ACCESSORS(ClassName, PropertyName) \
    GAMEPLAYATTRIBUTE_PROPERTY_GETTER(ClassName, PropertyName) \
    GAMEPLAYATTRIBUTE_VALUE_GETTER(PropertyName) \
    GAMEPLAYATTRIBUTE_VALUE_SETTER(PropertyName) \
    GAMEPLAYATTRIBUTE_VALUE_INITTER(PropertyName)

UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_Health)
FGameplayAttributeData Health;
ATTRIBUTE_ACCESSORS(UMyAttributeSet, Health)
```

**Replication — use `REPNOTIFY_Always`:**
```cpp
DOREPLIFETIME_CONDITION_NOTIFY(UMyAttributeSet, Health, COND_None, REPNOTIFY_Always);
```
`REPNOTIFY_Always` is required. Without it, the `OnRep` won't fire when the
server value equals the client's predicted value — a silent desync.

**`OnRep_*` — one line only:**
```cpp
void UMyAttributeSet::OnRep_Health(const FGameplayAttributeData& OldHealth)
{
    GAMEPLAYATTRIBUTE_REPNOTIFY(UMyAttributeSet, Health, OldHealth);
}
```
No gameplay logic in OnRep. OnRep is for GAS internals; UI updates should
bind to `GetGameplayAttributeValueChangeDelegate` instead.

**`PreAttributeChange` — clamping only:**
```cpp
void UMyAttributeSet::PreAttributeChange(const FGameplayAttribute& Attr, float& NewValue)
{
    Super::PreAttributeChange(Attr, NewValue);
    if (Attr == GetHealthAttribute())
        NewValue = FMath::Clamp(NewValue, 0.f, GetMaxHealth());
}
```
Fires for any attribute change from any source. **Only clamp here.** No
gameplay events, no delegate broadcasts. GE context is not available here.

**`PostGameplayEffectExecute` — gameplay consequences:**
```cpp
void UMyAttributeSet::PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data)
{
    Super::PostGameplayEffectExecute(Data);
    // Only fires after Instant GE apply. Has full GE context.
    if (Data.EvaluatedData.Attribute == GetDamageAttribute())
    {
        const float Dmg = GetDamage();
        SetDamage(0.f);                               // clear meta attribute
        SetHealth(FMath::Max(0.f, GetHealth() - Dmg)); // apply to real attribute
        if (GetHealth() <= 0.f) { /* broadcast death */ }
    }
}
```
**Meta attribute pattern:** `Damage` is a transient sink — the GE writes to
it, `PostGameplayEffectExecute` reads it, zeroes it, then commits to `Health`.
This lets `GEExecCalc` contribute multiple damage sources before commitment.

---

#### 11.9 Gameplay Tags — rules

**Registration is required before use.** Three methods:
1. **Project Settings → GameplayTags** in editor (saves to `DefaultGameplayTags.ini`).
2. **INI directly:**
   ```ini
   ; Config/DefaultGameplayTags.ini
   [/Script/GameplayTags.GameplayTagsSettings]
   +GameplayTagList=(Tag="Ability.ThrowBomb",DevComment="")
   +GameplayTagList=(Tag="Status.Stunned",DevComment="")
   ```
3. **C++ NativeGameplayTags (UE5.1+):**
   `UE_DEFINE_GAMEPLAY_TAG(TAG_Ability_ThrowBomb, "Ability.ThrowBomb")`
   in a .cpp file — registered at startup, no INI needed.

Unregistered tags return invalid handles; tag-based blocking/requirements
are silently ignored and `ensure` fires in dev builds. Tags written by
`ue5_gas_create_ability` (ability_tags, block_ability_tags, etc.) will not
round-trip in `ue5_gas_read_setup` until they are registered in the project.

**Tag roles in GAS:**

| Tag location | Purpose |
|---|---|
| `Ability.Tags` (asset tags) | The ability's own identity — used for blocking, cancellation, immunity |
| `ActivationRequiredTags` | Tags that **must** be present on ASC for ability to activate |
| `ActivationBlockedTags` | Tags that **must not** be present on ASC for ability to activate |
| `CancelAbilitiesWithTag` | Cancel all active abilities with these tags on activation |
| `BlockAbilitiesWithTag` | Block activation of abilities with these tags while active |
| `GE.GrantedTags` | Tags added to target ASC while GE is active |
| `GE.ApplicationRequiredTags` | Target must have all these tags for GE to apply |
| `GE.GrantedApplicationImmunityTags` | Block incoming GEs carrying these tags |

---

#### 11.10 Ability activation flow

```
TryActivateAbility(Handle)
  → CanActivateAbility():
      • In granted list?
      • Net policy allows on this machine?
      • CheckCost() — ASC can pay the cost GE?
      • CheckCooldown() — no active cooldown tag?
      • ActivationRequiredTags present?
      • ActivationBlockedTags absent?
      • Not blocked by another active ability?
  → ActivateAbility()  [your override]
      CommitAbility()        ← apply cost GE + cooldown GE; call exactly once
      [ability logic — play montage, apply damage GE, await AbilityTasks…]
      EndAbility(…)          ← MUST be called on EVERY code path
```

**`EndAbility` is mandatory on every exit path**, including:
- Normal completion
- Early-out (`if (!Target) { EndAbility(…); return; }`)
- Latent task failure callbacks
- `CancelAbility` override

Forgetting `EndAbility` leaves the ability permanently "active". With
`InstancedPerActor` it can never be activated again (blocks itself). With
`BlockAbilitiesWithTag` it permanently blocks the tagged abilities.

```cpp
void UMyAbility::ActivateAbility(...)
{
    if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
    {
        EndAbility(Handle, ActorInfo, ActivationInfo, true /*replicate*/, true /*cancelled*/);
        return;
    }
    // ... do work ...
    EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
}
```

---

#### 11.11 Multiplayer authority rules

| Action | Who calls it | Notes |
|---|---|---|
| `GiveAbility` | **Server only** | Specs replicate to owning client automatically |
| `RemoveAbility` | **Server only** | |
| `ApplyGameplayEffectSpecToSelf/Target` | **Server** for authoritative; owning client for predicted GEs inside `LocalPredicted` | |
| `TryActivateAbility` | Owning client (`LocalPredicted`) or server (`ServerOnly`/`ServerInitiated`) | GAS translates client call to RPC internally |
| `InitAbilityActorInfo` | **Both** server and owning client (at different times — §11.3) | |
| Read `GetGameplayAttributeValue` | Anyone — safe from any machine | |
| `SetAttributeBaseValue` | **Server only** — bypasses prediction; use GEs for client-visible changes | |

Use `ActorInfo->IsNetAuthority()` rather than `GetOwningActorFromActorInfo()->HasAuthority()`
inside ability code — it's the canonical GAS pattern and handles prediction edge cases.

---

#### 11.12 Asset authoring with MCP tools

```
ue5_gas_setup_actor          → add ASC to an actor Blueprint; set replication mode
ue5_gas_create_attribute_set → create AS_Foo with FGameplayAttributeData variables
ue5_gas_create_effect        → create GE_Foo (Instant / HasDuration / Infinite)
ue5_gas_create_ability       → create GA_Foo with net policy, instancing, tags, cost, cooldown
ue5_gas_configure_asc        → change replication mode on an existing ASC
ue5_gas_read_setup           → inspect GAS configuration of any Blueprint
```

**Recommended authoring order:**
```
1. ue5_gas_create_attribute_set   (AS_*)
2. ue5_gas_create_effect          (GE_Cost_*, GE_Cooldown_*, GE_Damage_*, etc.)
3. ue5_gas_create_ability         (GA_*) — reference the GE paths in cost_effect / cooldown_effect
4. ue5_gas_setup_actor            on the actor Blueprint
5. ue5_gas_read_setup             to verify
```

The MCP tools handle asset authoring only. Runtime wiring (`GiveAbility`,
`ApplyGameplayEffectSpecToSelf`, `InitAbilityActorInfo`) is C++/Blueprint work
done in the game code, not by these tools.

---

#### 11.13 Common GAS pitfalls

1. **`InitAbilityActorInfo` not called on client** — abilities activate on
   server but client sees no changes. Fix: call it in `OnRep_PlayerState`,
   not `BeginPlay` — PlayerState replicates after BeginPlay fires.

2. **Missing `REPNOTIFY_Always`** — `OnRep` doesn't fire when the server value
   equals the client's predicted value. Silent desync.

3. **`EndAbility` never called** — ability works once, then
   `TryActivateAbility` silently does nothing. Check every code path.

4. **Gameplay logic in `OnRep_*`** — runs on clients, causes desync. OnRep
   calls `GAMEPLAYATTRIBUTE_REPNOTIFY` only. Gameplay consequences go in
   `PostGameplayEffectExecute` (server) or `GetGameplayAttributeValueChangeDelegate`
   (for UI on clients).

5. **`GiveAbility` called on client** — server never recognizes the ability;
   `TryActivateAbility` fails. `GiveAbility` is server-only.

6. **Tag not registered in `DefaultGameplayTags.ini`** — blocking/requirements
   silently ignored, `ensure` fires in dev builds.

7. **`GiveAbility` before `InitAbilityActorInfo`** — abilities granted but
   never found; `TryActivateAbility` always fails. Correct order: init → give.

8. **UI polling attributes in Tick** — use
   `ASC->GetGameplayAttributeValueChangeDelegate(Attr).AddUObject(...)` instead.

9. **Multiple attribute sets of the same class on one ASC** — crash on
   attribute lookup. One instance per subclass per ASC.

10. **`LocalPredicted` cooldown not blocking on client** — ensure the cooldown
    GE grants a tag (e.g. `Cooldown.Ability.ThrowBomb`) and that same tag is in
    `ActivationBlockedTags` on the ability. Both sides must use the identical tag.

---

### 14. Performance profiling — Unreal Insights via MCP

For "what's slow?" / "which function eats the most CPU?" / "is collision
expensive?" questions, use the `ue5_profile_*` tools — do NOT ask the user
to run UnrealFrontend, click Trace, open Insights manually, or read raw
`.utrace` files yourself. The MCP captures the trace, runs `UnrealInsights.exe`
headless, and returns a structured top-N ranking that you can reason over.

**Capture / analyze loop:**

```
1. ue5_profile_trace_start                       (defaults: cpu,gpu,frame,bookmark,log)
2. (do whatever activity needs profiling — PIE, sequence, repro steps)
3. ue5_profile_trace_stop                        (returns the .utrace path)
4. ue5_profile_analyze utrace=<path> mode=...    (returns top-N by TotalInclusiveTime)
```

**Modes** (each is a wildcard preset on `-timers=`):

| Mode | Targets |
|---|---|
| `top_functions` | No filter — full top-N (default) |
| `collision` | `*Collision*,*Sweep*,*Overlap*,*LineTrace*,*PrimitiveComponent*,*BodyInstance*` |
| `physics` | `*Physics*,*Chaos*,*PxScene*,*FBodyInstance*,*TickPhysics*,*Solver*` |
| `rendering` | `*RenderThread*,*BasePass*,*Shadow*,*PostProcess*,*Slate*,*RHIThread*` |
| `gameplay` | `*Tick*,*Actor*,*ProcessEvent*,*Blueprint*,*Behavior*,*AI*` |
| `loading` | `*Load*,*Stream*,*Package*,*AsyncLoad*,*Cook*` |
| `ui` | `*Slate*,*UMG*,*Widget*,*Paint*` |

`mode=raw` plus `timers_filter="*Foo*"` lets you write any custom wildcard.

**Region-based slicing** (preferred for repeatable comparisons): wrap the
activity in `ue5_profile_region(name="X", state="begin")` /
`state="end"`, then pass `region="X"` to `ue5_profile_analyze`. Regions
work better than start/stop time math because Insights handles the
clock alignment for you.

**Bookmarks** (`ue5_profile_bookmark`) mark *moments* — useful for
navigating Insights' Timing Insights track. Use them to mark hard-to-find
events: `BombSpawned`, `BombResolved`, `PhaseTransition`. Don't confuse
with regions, which are spans.

**Channels — when to deviate from the default:**

- Default `cpu,gpu,frame,bookmark,log` covers 90% of perf questions.
- Add `memalloc` only when investigating memory growth — file size grows
  *fast* (hundreds of MB/min in busy scenes).
- Add `loadtime` for asset/level load investigations.
- Add `task` for task-graph / job-system parallelism questions.
- Add `net` only when chasing a replication-bandwidth issue.

**Don't:**

- Don't ask the user to run UnrealFrontend or open Insights manually if
  this MCP is connected — `ue5_profile_*` does it.
- Don't run `Trace.Start <args>` via `ue5_bp_*` exec hacks — `ue5_profile_trace_start`
  uses the new (5.4+) `Trace.File` command which `Trace.Start` is deprecated
  in favor of.
- Don't leave a trace running across long idle periods — file size grows
  even when nothing's happening on `bookmark`/`log`.
- Don't profile in Editor build for shipping-relevant numbers. Editor
  has overhead. For real numbers profile a Test or Shipping build with
  the trace launched via `-trace=cpu,gpu,frame,bookmark,log` at process
  startup. (For exploratory in-editor profiling, in-editor Trace.File
  via this MCP is fine.)

**Verified against UE 5.7 source:** the underlying console commands are
`Trace.File [Path] [ChannelSet]`, `Trace.Stop`, `Trace.Bookmark [Name]`,
`Trace.RegionBegin/End [Name]` — registered in
`Engine/Source/Runtime/Core/Private/ProfilingDebugging/TraceAuxiliary.cpp`.
Headless analysis CLI is `UnrealInsights.exe -OpenTraceFile=<path>
-AutoQuit -NoUI -ExecOnAnalysisCompleteCmd="<task>" -log` — verified
against `Engine/Source/Developer/TraceInsights/Private/Insights/Tests/FunctionalTests/ExportCommandsTests.cpp`.

---

## Asset creation tools (cheat sheet)

| Asset | Tool | Convention |
|---|---|---|
| Blueprint class (Actor / Pawn / etc.) | `ue5_create_blueprint` | `BP_Foo` |
| Blueprint Struct | `ue5_create_blueprint_struct` | `S_Foo` |
| Blueprint Enum | `ue5_create_blueprint_enum` | `E_Foo` |
| Blueprint Interface | `ue5_create_blueprint_interface` | `BPI_Foo` |
| Animation Blueprint | `ue5_create_anim_blueprint` (then `ue5_configure_anim_blueprint`) | `ABP_Foo` |
| Widget Blueprint | `ue5_create_widget_blueprint` | `WBP_Foo` |
| Niagara System | `ue5_create_niagara_system` | `NS_Foo` |
| Material Instance | `ue5_create_material_instance` | `MI_Foo` |
| Data Asset | `ue5_create_data_asset` | `DA_Foo` |
| Level | `ue5_create_level` | `L_Foo` |
| Behavior Tree | `ue5_bt_create_tree` | `BT_Foo` |
| Blackboard | `ue5_bt_create_blackboard` (then `ue5_bt_add_blackboard_keys`) | `BB_Foo` |
| Attribute Set | `ue5_gas_create_attribute_set` | `AS_Foo` |
| Gameplay Ability | `ue5_gas_create_ability` | `GA_Foo` |
| Gameplay Effect | `ue5_gas_create_effect` | `GE_Foo` |
| DataTable | `ue5_datatable_create` (rows via `ue5_datatable_set_rows`) | `DT_Foo` |
| Level Sequence | `ue5_sequencer_create` (then `ue5_sequencer_bind_actor` / `_add_camera`) | `LS_Foo` |
| Sound Cue | `ue5_create_sound_cue` (import the wave first with `ue5_import_asset`) | `SC_Foo` |
| Sound Attenuation | `ue5_create_sound_attenuation` (assign via `ue5_spawn_ambient_sound` or component props) | `ATT_Foo` |
| Copy of any asset | `ue5_duplicate_asset` | keep the source prefix |

Within an existing Blueprint:

| What | Tool |
|---|---|
| Member variable | `ue5_bp_create_variable` (or rely on `ue5_bp_add_logic` auto-create) |
| Inspect existing variables | `ue5_bp_list_variables` |
| Custom function (with typed inputs/outputs) | `ue5_bp_create_function` |
| Macro graph | `ue5_bp_create_macro` |
| SCS Component | `ue5_add_component_to_blueprint` |
| Event-graph nodes + connections | `ue5_bp_add_logic` |

`ue5_bp_create_function` returns a `graph_name` you can pass as the `graph` field of `ue5_bp_add_logic` to populate the function body. Same applies to `ue5_bp_create_macro` (use the macro instance node `K2Node_MacroInstance` to *invoke* it from another graph — see the `macro` node type in the schema below).

---

## Verify your work — the feedback loop

The server gives you eyes and hands for verification. After meaningful
mutations, USE them instead of assuming success:

1. **See it**: `ue5_set_viewport_camera` (use `focus_actor` to auto-frame),
   then `ue5_viewport_screenshot`, then Read the returned PNG. Works even
   when the editor window is in the background (SceneCapture-based). This is
   how you check levels, materials, meshes, lighting, and VFX visually.
2. **Play it**: `ue5_pie_start` → observe (`ue5_get_log`, screenshots) →
   `ue5_pie_stop`. Check `ue5_pie_status` first; never start PIE while a
   session is running.
3. **Check the log**: `ue5_get_log` with `severity: "error"` (or `"warning"`)
   after compiles, spawns, and generation — cheap and catches silent failures.

Interface workflow: `ue5_create_blueprint_interface` → `ue5_bp_add_interface`
on the implementing BP → for functions WITH return values also call
`ue5_bp_implement_interface_function` (never `ue5_bp_create_function` — that
creates a colliding standalone function). After reparenting or struct edits,
run `ue5_bp_refresh_all_nodes`.

Widget events: use `ue5_widget_bind_event` (it flags the widget as a variable,
compiles, and adds the bound-event node), then wire the handler body with
`ue5_bp_add_logic` connecting from the returned node id. GAS startup: use
`ue5_gas_grant_on_beginplay` to wire GiveAbility / ApplyGameplayEffectToSelf —
don't hand-build those nodes.

`ue5_reimport_asset` only works on assets imported from a source file; it
refuses in-editor-created assets (that would open a modal dialog and freeze a
headless editor).

**Never call `ue5_create_blueprint` (or other create tools) on an asset path
that already exists** — check with `ue5_get_asset`/`ue5_list_assets` first, or
delete/duplicate instead. Creating over an existing package can deadlock the
editor's game thread in `FlushAsyncLoading`.

---

## Blueprint authoring schema reference

`ue5_bp_add_logic` takes a `nodes[]` and `connections[]` spec. This is the
full contract for each node `type` and connection format — read this once,
don't grep.

### Node types

| `type` | Required fields | Optional fields | What it produces |
|---|---|---|---|
| `event` | `event` (or `event_name`) — name of a function on the parent class (e.g. `ReceiveBeginPlay`, `ReceiveTick`, `ReceiveActorBeginOverlap`) | `override` (default true) | `K2Node_Event` overriding the parent function. **Refuses** if the function does not exist on the parent class — use `custom_event` instead. |
| `custom_event` | `event_name` (or `name`) — any identifier (`MoveUp`, `OnFoo`, etc.) | — | `K2Node_CustomEvent`, callable via `call_function` with the same name and `self_context: true`. |
| `component_event` | `component` (SCS variable name on this BP) + `delegate` (or `event`/`event_name`) — name of a multicast delegate on that component class | — | `K2Node_ComponentBoundEvent` (e.g. binding to `OnComponentHit` of `BoxCollision`). |
| `call_function` | `function` (function name) | `class` (full `/Script/Pkg.ClassName` path; required UNLESS `self_context: true`); `self_context` (bool, default false — set true for functions on the BP itself or its parent class); `defaults` (see below) | `K2Node_CallFunction`. Function lookup uses `FindFunctionByName` on the resolved class. |
| `variable_get` | `variable` (or `name`) — name of a variable on the BP, or a component name (components are SCS variables) | `self_context` (default true) | `K2Node_VariableGet`. If the variable doesn't exist, `bp_add_logic` will auto-create it when type can be inferred from a connection (call_function param, branch condition, sibling get/set). For explicit creation, use `ue5_bp_create_variable` first. |
| `variable_set` | `variable` (or `name`) | `self_context` | `K2Node_VariableSet`. Same auto-create behavior as `variable_get`. |
| `branch` | — | — | `K2Node_IfThenElse`. Pin aliases: `condition`, `true`, `false`. |
| `sequence` | — | `outputs` (int, default 2) | `K2Node_ExecutionSequence`. Output pins: `Then_0`, `Then_1`, … |
| `cast` | `target_class` (full path or short name) | — | `K2Node_DynamicCast`. |
| `self` | — | — | `K2Node_Self`. |
| `knot` | — | — | `K2Node_Knot`. Reroute node. |
| `macro` | `macro` (asset path of the macro graph) | — | `K2Node_MacroInstance`. |

### `defaults` field

Object of `{ pin_name: value }` applied after the node's pins are allocated.
Values can be strings, numbers, or booleans. **Asset reference pins** (object
/ class / soft-object pins) accept content paths starting with `/Game/`,
`/Engine/`, or `/Script/` — the bridge calls `LoadObject` on the path and
binds it as `DefaultObject`. Non-asset values become `DefaultValue` strings
(numeric pins accept `"3.0"`, `"100"`, etc.; vectors/rotators take their
text form `"X=0,Y=0,Z=200"` for vectors but **rotators reject** the
abbreviated `"P=0,Y=0,R=0"` form during compile — leave rotator pins at the
default 0 unless you need a non-zero rotation, and even then prefer wiring
in a `Make Rotator` node rather than setting the default).

### Connection format

Strings of `node_id.pin_name`, paired as `{ "from": ..., "to": ... }` in
`connections[]`. Pin lookup is case-insensitive and accepts these aliases:

| alias | resolves to |
|---|---|
| `execute`, `exec`, `in` | `PN_Execute` (input flow) |
| `then`, `out`, `next` | `PN_Then` (output flow) |
| `true` | `PN_Then` (on `branch`) |
| `false` | `PN_Else` (on `branch`) |
| `condition` | `PN_Condition` |
| `self`, `target` | `PN_Self` |
| `return`, `returnvalue`, `result`, `value`, `output` | `PN_ReturnValue` on call_function; the variable's named pin on `variable_get`/`variable_set` |

**Aliases do NOT cover every "exec input"**. Functions tagged
`ExpandEnumAsExecs` (notably `MoveComponentTo`, latent action functions)
expose multiple exec input pins like `Move` / `Stop` / `Return`. Wire to the
literal pin name — `Move`, not `execute`. If a connection fails with `"pin
not found"`, the pin name in the error is the exact case-correct name; use
that.

### Component setup workflow (the 3-step idiom)

1. `ue5_create_blueprint` — creates the BP at `/Game/.../BP_X`, parent class `Actor` (or whatever).
2. `ue5_add_component_to_blueprint` with **initial `properties`** — adds an SCS component AND sets defaults on the SCS template at the same time. For a `UStaticMeshComponent`, set `properties: [{name:"StaticMesh", value:"/Engine/BasicShapes/Cube.Cube"}]`. The bridge knows to call `SetStaticMesh()` for `StaticMesh` properties on `UStaticMeshComponent` (and `SetSkeletalMeshAsset()` for `USkeletalMeshComponent`); other properties go through normal `ImportText`.
3. `ue5_bp_add_logic` — wires the event graph using `variable_get` referencing the component name. The component IS the variable.

`ue5_set_component_defaults` is for **changing component defaults after the
component already exists** — and it requires the BP to have been compiled at
least once first (so the SCS node has produced a generated-class subobject).
For initial setup, do everything in `ue5_add_component_to_blueprint`'s
`properties` array — fewer round-trips and no compile cycle needed.

### Pin-name patterns the alias table doesn't cover

The alias table earlier handles the common cases. The patterns below are
the ones that catch people out — examples are illustrative, the
**pattern** is what's load-bearing:

| Pattern | Actual pin name |
|---|---|
| **`variable_get` / `variable_set` value pin** is named after the variable itself. | `variable: "BoxMesh"` → output pin `BoxMesh`. NOT `return`. |
| **`K2Node_CallFunction` self/target input** is `self` (alias `target` resolves to it). | Connect to `<node>.self` or `<node>.target`. |
| **`K2Node_CallFunction` return value** is `ReturnValue` (aliases `return`, `result`). | |
| **`ExpandEnumAsExecs` latent functions** (anything with multiple input exec pins) — input pins are named after the enum values. | `MoveComponentTo` has `MoveAction` enum so its input execs are `Move`, `Stop`, `Return`. `execute` won't match. |
| **Latent / async completion output** is `then`, not `Completed`. | The editor UI sometimes labels the connector "Completed" but the underlying pin name is just `then`. |

If `connect_pins` reports `pin not found`, the error now says which side
failed. Call `ue5_bp_read_graph` once for that node and read its real pin
names rather than guessing again.

### Compile warnings are the authoritative deprecation source

UE ships deprecated function wrappers for backward compatibility. The
bridge will accept any of them; `ue5_bp_compile` returns warning messages
naming the canonical replacement (e.g. *"Use X instead"*). Read those
warnings and swap to the named replacement on the next call.

Don't try to memorize specific deprecated → current pairs in this file:
the list churns between engine versions, and the warning text in the
compile result is always the authoritative spec for the engine you're
running against.

### Calling a custom event from the same `ue5_bp_add_logic` batch

When you create a `custom_event` and a `call_function` that targets it in
the **same** batch, the bridge resolves call_function names against the
BP's graphs first (so freshly-added custom events are visible) and falls
back to the parent `GeneratedClass` only when no in-graph match is found.

**Order matters**: place the `custom_event` node spec earlier in `nodes[]`
than the `call_function` that targets it. The bridge processes
`nodes[]` in array order; an earlier custom_event is already in the graph
by the time a later call_function looks it up.

```jsonc
"nodes": [
  { "id": "ce_up",    "type": "custom_event", "event_name": "DoMoveUp" },
  // ... other nodes that wire into ce_up.then ...
  { "id": "call_up",  "type": "call_function", "function": "DoMoveUp",
    "self_context": true }   // resolves against ce_up, not GeneratedClass
]
```

(If you can't reorder for some reason, `K2_SetTimer` with a string
`FunctionName` works as a string-based escape hatch — but it's ugly and
adds a 0.01s timer hop. Reordering is cleaner.)

### Common pitfalls (don't repeat these)

- **Pure functions have no exec pins — keep the exec chain over them, not through them.**
  `variable_get`, `GetActorLocation`, math nodes, and any function flagged
  `const` / "pure" in Blueprint are data-only nodes: they expose no `execute`
  input and no `then` output. When building an exec chain that *reads* a pure
  node's output, the exec wire must connect directly between the surrounding
  exec-bearing nodes (e.g. `BeginPlay.then → SetActorLocation.execute`) and
  the pure node feeds only the **data** pin (e.g.
  `GetActorLocation.ReturnValue → SetActorLocation.NewLocation`). Never try to
  route `then` through a pure node — the pin doesn't exist and the batch will
  silently drop or mis-route that connection, leaving a broken exec chain that
  requires a follow-up `ue5_bp_connect_pins` call to repair.
- **Variables: use `ue5_bp_create_variable` for explicit declaration**, or
  rely on `ue5_bp_add_logic`'s auto-create (which infers type from a
  connection). If you reference a `variable_get`/`variable_set` whose type
  can't be inferred from any connection in the same batch (e.g. it's only
  read by a `variable_get` with no further connections), pre-declare it with
  `ue5_bp_create_variable`. Components added via `ue5_add_component_to_blueprint`
  are also addressable as variables — use them when the data is a component.
- **`Sin`/`Cos` on `KismetMathLibrary` take RADIANS.** For a sine wave with
  oscillation period `T` seconds, multiply elapsed-seconds by `2π / T` before
  feeding into `Sin`. Period 4s → `Multiply_DoubleDouble.B = 1.5708`. Period
  2s → `Multiply_DoubleDouble.B = 3.1416`. If you actually want degrees, use
  `DegSin` / `DegCos` instead. The classic mistake is multiplying by `90.0`
  expecting "90 degrees per second" and getting a ~0.07-second cycle in
  radian space.
- **Float math operators: see Rule 7.** Always write `_DoubleDouble` — never
  `_FloatFloat`. This must be correct on the first attempt, not fixed after
  a bridge error.
- **Component name in `variable_get` is case-sensitive on the wire** — match
  what `ue5_add_component_to_blueprint` returned (`Mesh`, `Box`, etc.). The
  bridge does case-insensitive lookup but consistent capitalization makes
  graph reads cleaner.
- **Don't set rotator pin defaults via the `defaults` object** — the
  `(P=0,Y=0,R=0)` and `X=0,Y=0,Z=0` forms behave differently and the rotator
  one trips a "Invalid value for an FRotator" compile error. Either omit
  (defaults to zero rotator) or wire a `Make Rotator` node.
- **`ReceiveBeginPlay` and `ReceiveTick` exist on every Actor by default**
  as empty stub event nodes after BP creation. `ue5_bp_add_logic`
  auto-deduplicates these when you add another `event` of the same name, so
  a fresh `ReceiveBeginPlay` request reuses the existing stub.
- **Don't author variant assets to vary one value at runtime** — see
  Rule 3 above. Anything that swaps a single material parameter (color,
  emissive intensity, scalar effect strength, etc.) belongs in a Dynamic
  Material Instance, not in N alternate `MI_*` files swapped with
  `SetMaterial`.
