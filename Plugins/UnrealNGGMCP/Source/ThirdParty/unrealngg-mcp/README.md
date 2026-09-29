# unrealngg-mcp

A Node.js MCP (Model Context Protocol) server that bridges Claude Code to the
`UnrealNGGMCP` UE5 Editor plugin. Claude Code spawns this as a stdio child
process; every MCP tool call is translated into an HTTP request against the
plugin's local bridge (default `http://localhost:6776`).

```
Claude Code ──stdio──►  unrealngg-mcp (this package)
                               │
                        HTTP :6776
                               │
                        UnrealNGGMCP plugin (UE5 editor)
```

---

## Install

Nothing to do manually: `.mcp.json` launches `bootstrap.js`, which runs
`npm install` itself on first use (or whenever `package-lock.json` changes)
before handing off to `index.js`. The UE5 editor plugin also pre-installs
these dependencies at editor startup (`FNodeDepsInstaller`); both share the
`node_modules/.ngg-deps-stamp` freshness stamp so they never fight.

Manual fallback (e.g. no network at first run):

```bash
cd Plugins/UnrealNGGMCP/Source/ThirdParty/unrealngg-mcp
npm install          # pulls @modelcontextprotocol/sdk and zod
```

The MCP server is wired into Claude Code via `.mcp.json` at the project root;
see the plugin README for that configuration. Running `node index.js` directly
is only useful for debugging — Claude Code manages the lifecycle normally.

---

## Bundled Claude skills & agents

The server installs any Claude Code **skills** and **agents** placed under
`assets/` into the project's `.claude/` directory on startup — the same
"ships with the plugin, applies on any PC" model as `UE5_NGG_RULES.md` (which is
sent as the MCP `instructions`). This is pure local-filesystem work, independent
of the editor bridge.

Drop content under `assets/skills/<name>/SKILL.md` and `assets/agents/<name>.md`
(see `assets/README.md`); the tree is copied verbatim to
`<project>/.claude/skills/...` and `.../agents/...`. Bump `version` in
`assets/manifest.json` whenever you change content so unedited installs refresh.

What ships (v1.1):

| Kind | Name | Purpose |
|---|---|---|
| skill | `ue5-niagara` | Niagara VFX — systems/emitters/modules, C++ spawning, pooling, debugging |
| skill | `ue5-umg-widgets` | UMG/Slate widgets — layout, anchors, C++ binding, reference-matching (+ `references/`) |
| agent | `ue5-senior-dev` | Expert UE5 gameplay/systems engineer (C++ + Blueprint + architecture) |
| agent | `ue5-build-engineer` | UBT/UHT/UAT build, link, packaging, and module/plugin diagnosis |

**Install behaviour** (see `setup-assets.js`):

- **Destination** — `<project>/.claude/skills` and `<project>/.claude/agents`,
  resolved from the `.uproject` (`NGG_UE_PROJECT_PATH` or upward `cwd` walk).
- **Version-tracked, never clobbers edits** — a manifest at
  `.claude/.ngg-assets.json` records the hash of every file the server wrote.
  On later runs it installs missing files, refreshes files the user hasn't
  touched when the bundled `assets/manifest.json` `version` bumps, and leaves
  any user-modified file alone.
- **Picked up on next restart** — Claude Code loads skills/agents at session
  start, so freshly installed files appear after the next Claude Code restart,
  not mid-session.
- **Force a refresh** with the `ue5_setup_skills` tool (reports installed /
  updated / unchanged / preserved counts).
- **Disable** with `NGG_SKIP_SKILL_INSTALL=1`.

To change what ships: edit the files under `assets/skills` / `assets/agents`
and bump `version` in `assets/manifest.json` so unedited installs refresh.

---

## Path auto-discovery

At startup the server resolves:

| What | How it's found | Override |
|---|---|---|
| `.uproject` | Walks upward from `cwd` looking for `*.uproject` | `NGG_UE_PROJECT_PATH` |
| UE install dir | 1. env var; 2. `HKCU\Software\Epic Games\Unreal Engine\Builds` (custom source builds); 3. `HKLM\SOFTWARE\EpicGames\Unreal Engine\<ver>\InstalledDirectory` (launcher installs); 4. `%PROGRAMDATA%\Epic\UnrealEngineLauncher\LauncherInstalled.dat`; 5. scan every drive for `UE_<version>` / `Program Files/Epic Games/UE_<version>` | `NGG_UE_INSTALL_DIR` |
| Build targets | Parses `Source/*.Target.cs` (distinguishes `Editor`, `Server`, `Client`, `Game`) | (not overridable) |

If nothing is found, `ue5_launch_editor` and `ue5_build` throw with a clear
message. All other tools work because they only need the bridge URL.

Resolution lives in `paths.js` (pure helpers, no side effects) so tests can
import it without booting the server. See `paths.test.js`.

---

## Environment variables

| Variable | Default | Purpose |
|---|---|---|
| `NGG_BRIDGE_URL` | `http://localhost:6776` | UE5 plugin HTTP bridge URL |
| `NGG_BRIDGE_TIMEOUT_MS` | `15000` | Per-request timeout (ms). Live tests bump to 60000. |
| `NGG_BRIDGE_TOKEN` | *(ini fallback)* | Bearer token sent in `Authorization` header. Must match the `AuthToken` the editor loaded. When unset, the client reads `[UnrealNGGMCP] AuthToken` from `Config/UserEngine.ini`, then `Config/DefaultEngine.ini` — so standalone runs (live tests, `node index.js`) need no env setup. See [Setting the bridge token](#setting-the-bridge-token). |
| `NGG_UE_PROJECT_PATH` | auto | Absolute path to the `.uproject` |
| `NGG_UE_INSTALL_DIR` | auto | Root of the Unreal Engine install |
| `NGG_MCP_TRANSPORT` | `stdio` | Set to `ws` for WebSocket transport |
| `NGG_MCP_WS_PORT` | `6777` | WebSocket port (only if `NGG_MCP_TRANSPORT=ws`) |
| `NGG_TOOLSETS` | *(all)* | Allowlist of tool groups to register, e.g. `core,blueprint,umg,level`. See [Toolsets](#toolsets). |
| `NGG_TOOLSETS_EXCLUDE` | *(none)* | Denylist of tool groups, e.g. `pcg,gas`. Ignored when `NGG_TOOLSETS` is set. |
| `MESHY_API_KEY` | *(empty)* | Required for `ue5_meshy_generate`. Falls back to `.meshy.json` at project root. |

Env vars are read **per request**, not at import — tests can override them
dynamically. See `ue5client.js::bridgeUrl()` / `timeoutMs()` / `authToken()`.

---

## Toolsets

The full catalog is 208 tools. Their schemas plus the rules core cost roughly
**60k tokens of context on every session**, before any work happens. A project
that never authors PCG graphs or GAS abilities can drop those groups:

```jsonc
// .mcp.json
"env": {
  "NGG_TOOLSETS_EXCLUDE": "pcg,gas,profiling"   // or an allowlist:
  // "NGG_TOOLSETS": "core,blueprint,umg,level"
}
```

Measured on this project:

| Configuration | Tools | Handshake | Saving |
|---|---|---|---|
| default (all) | 208 | ~60.7k tokens | — |
| `NGG_TOOLSETS_EXCLUDE=pcg` | 183 | ~56.5k | 7% |
| `…=interchange,landscape,rendering,assetconfig` | 195 | ~55.3k | 9% |
| `…=pcg,gas,profiling,exercises,youtube,meshy` | 161 | ~49.8k | 18% |
| `NGG_TOOLSETS=core,blueprint,umg,level` | 72 | ~21.0k | 65% |

`core` is always registered and cannot be excluded — it holds health check,
project info, save, the editor lifecycle, generic asset read/write and
`ue5_batch`, which any session needs to function or to diagnose itself.

Available sets: `core`, `blueprint`, `umg`, `level`, `landscape`, `material`,
`anim`, `rigging`, `niagara`, `mesh`, `meshy`, `pcg`, `gas`, `ai`, `profiling`,
`rendering`, `assetconfig`, `interchange`, `sequencer`, `worldpartition`,
`audio`, `datatable`, `input`, `gameplaytags`, `exercises`, `youtube`. Unknown
names are reported on stderr and ignored.

### Why this isn't auto-detected

The plugin's `.uplugin` declares PCG, GameplayAbilities, Niagara,
GeometryScripting, StateTree, EnhancedInput and PythonScriptPlugin as
dependencies, and UE auto-enables a plugin's dependencies. So "is PCG enabled
in this project?" is always *yes* whenever this server can run at all — reading
the `.uproject` would tell us nothing. (The example project's `.uproject`
doesn't even list PCG for that reason; see `Docs/FAB_TRC_FIXES.md`.) What the
engine cannot know is whether *you* intend to author PCG graphs, so that stays
an explicit choice.

---

## Setting the bridge token

**Normally you do not have to.** On startup the plugin checks for a configured
`[UnrealNGGMCP] AuthToken`; if there is none it generates one and writes it to
`<Project>/Config/UserEngine.ini`, which this sidecar reads. A fresh clone works
with no setup step. If the write fails (read-only file, missing directory) the
plugin logs the reason and starts without auth rather than using a token the
sidecar could never learn — see below for what that costs.

Without a token the bridge still serves every ordinary route, but `exec_python`
and the lifecycle routes (`shutdown`, `build_and_run`, `kill_and_restart`) stay
disabled — and since every `pcg_*` tool is implemented on top of `exec_python`,
**26 tools go dark**.

To rotate the token, or to set one by hand, run this from this directory:

```bash
npm run set-token          # generate a fresh 64-hex token
npm run set-token -- --print   # show the current one
```

That writes `[UnrealNGGMCP] AuthToken=…` to `<Project>/Config/UserEngine.ini`
and prints a confirmation. **Restart the editor afterwards** — the plugin reads
the token once at startup.

### Why `UserEngine.ini`

`UserEngine.ini` is GConfig's `GameDirUser` layer — the last entry in
`GConfigLayers` (`Engine/Source/Runtime/Core/Public/Misc/ConfigHierarchy.h`),
so it overrides `Config/DefaultEngine.ini` and the editor never rewrites it.
It is gitignored (`Config/User*.ini`), which is the point: `DefaultEngine.ini`
is tracked, so a token placed there is published in the repository history.

The sidecar reads both layers in the same order, so an existing install that
still keeps its token in `DefaultEngine.ini` keeps working — move it to
`UserEngine.ini` when convenient.

---

## Tools

208 MCP tools total, grouped below (major tools listed; run `/mcp` in Claude
Code for the complete live catalog with argument schemas).

### Editor lifecycle

| Tool | Purpose |
|---|---|
| `ue5_health_check` | Confirm the bridge is reachable; also reports engine/plugin/protocol versions, whether auth is configured, and warns on a plugin↔sidecar version skew |
| `ue5_launch_editor` | Shell-execute the `.uproject` (via UnrealVersionSelector) |
| `ue5_kill_editor` | `taskkill /F /IM UnrealEditor.exe` — use before `ue5_build` |
| `ue5_build` | Run `Build.bat <target>Editor Win64 Development <uproject>`; auto-detects target name |
| `ue5_save_all` | Save all dirty packages |
| `ue5_setup_skills` | Install/refresh the bundled Claude skills & agents into `.claude/` (runs on startup; call to force a refresh) |
| `ue5_project_info` | Ground-truth project/engine paths + build target names from the running editor |

### Content browser / assets

| Tool | Purpose |
|---|---|
| `ue5_list_assets` | List assets at a content path |
| `ue5_get_asset` | Get all UPROPERTYs of an asset as JSON |
| `ue5_set_asset_property` | Set one UPROPERTY via reflection |
| `ue5_delete_asset` | Force-delete an asset |
| `ue5_import_asset` | Import an external file (wav, fbx, png, etc.) into the content browser |
| `ue5_create_data_asset` | Create a `UDataAsset` subclass with optional initial properties |
| `ue5_list_gameplay_tags` | List every registered GameplayTag |
| `ue5_add_gameplay_tags` | Register new tags in `DefaultGameplayTags.ini` (+ live refresh when supported) |
| `ue5_duplicate_asset` | Duplicate any asset to a new path (duplicate-and-tweak workflow) |
| `ue5_set_asset_map_entries` | Replace all entries of a TMap property (class refs LoadObject-resolved) |
| `ue5_reimport_asset` | Reimport from the original source file; refuses assets without one (modal-dialog guard) |

### Blueprints

| Tool | Purpose |
|---|---|
| `ue5_create_blueprint` | Create a BP subclass of any UClass |
| `ue5_reparent_blueprint` | Change a Blueprint's parent class |
| `ue5_set_blueprint_defaults` | Write CDO properties |
| `ue5_add_component_to_blueprint` | Append a component to the SCS; pass `attach_parent` to nest a scene component under another (e.g. Camera under SpringArm) |
| `ue5_set_component_defaults` | Write properties on a named component |

### Blueprint graph authoring

| Tool | Purpose |
|---|---|
| `ue5_bp_add_node` | Add one K2 node by class path |
| `ue5_bp_add_logic` | Add a whole `{nodes, connections}` subgraph in one call |
| `ue5_bp_connect_pins` | Wire two named pins |
| `ue5_bp_delete_node` | Remove a node by id/guid |
| `ue5_bp_read_graph` | Dump a graph as JSON (same shape `add_logic` accepts) |
| `ue5_bp_compile` | Compile, optionally save on success |
| `ue5_bp_lint` | Diagnose stale refs / orphaned pins; optional auto-fix |
| `ue5_bp_lint_project` | Sweep lint across every BP under a content path |
| `ue5_bp_add_interface` | Add a Blueprint Interface to a BP's implemented list |
| `ue5_bp_implement_interface_function` | Create the override graph for an interface function with a return value |
| `ue5_bp_refresh_all_nodes` | Reconstruct all nodes after reparent/struct/signature changes |

### UMG widgets

| Tool | Purpose |
|---|---|
| `ue5_create_widget_blueprint` | Create a WBP with an initial widget tree |
| `ue5_add_widget_to_blueprint` | Append widgets to an existing tree |
| `ue5_style_widgets` | Apply visual styling (font, color, anchors, size) to named widgets |
| `ue5_set_widget_properties` | Set arbitrary editor properties on a named widget (brush textures, enums, colors) |
| `ue5_widget_bind_event` | Bind a widget delegate (Button OnClicked, ...) to a new graph event node |

### Levels / actors

| Tool | Purpose |
|---|---|
| `ue5_open_level` | Open an existing level |
| `ue5_create_level` | Create and open a new empty level; `partitioned:true` makes a World Partition map |
| `ue5_set_world_settings` | Set GameMode / PlayerController / DefaultPawn |
| `ue5_set_level_environment` | Configure ExponentialHeightFog, DirectionalLight, SkyLight |
| `ue5_spawn_actor` | Spawn an actor at a world location (optionally with a static mesh) |
| `ue5_update_actor` | Move / rotate / relabel / set component props |
| `ue5_list_actors` | Enumerate actors in the current level |
| `ue5_delete_actor` | Remove an actor by label |

### Input

| Tool | Purpose |
|---|---|
| `ue5_configure_imc` | Clear an `InputMappingContext` and write fresh action → key mappings with modifiers |

### Materials

| Tool | Purpose |
|---|---|
| `ue5_create_material_instance` | Create a `UMaterialInstanceConstant` from a parent with scalar/vector overrides; optionally apply to a BP's mesh component |
| `ue5_material_add_expression` | Add a node to a Material's expression graph (any `MaterialExpression*` class) |
| `ue5_material_connect` | Wire expression nodes together or into a material output (BaseColor, Emissive, ...) |

### Animation

| Tool | Purpose |
|---|---|
| `ue5_configure_anim_blueprint` | Wire an AnimBP state machine with states + transitions |

### Rigging — IK Rig, IK Retargeter, Control Rig

| Tool | Purpose |
|---|---|
| `ue5_ikrig_create` | IK Rig for a skeletal mesh: FBIK solver, IK goals on bones, retarget chains |
| `ue5_ikrig_read` | Mesh, solver count, retarget root, goals with bones, chains with start/end bones |
| `ue5_ikretargeter_create` | IK Retargeter mapping one rig's animation onto another, with the standard op stack |
| `ue5_ikretargeter_set_chain_mapping` | Fix the chain pairs auto-mapping got wrong |
| `ue5_controlrig_create` | Control Rig blueprint for a mesh, with the skeleton hierarchy imported |

Two API limits worth knowing, both the engine's:

- **Only Full Body IK.** `IKRigController.add_solver` cannot receive a solver
  class through UE 5.8's Python bindings (the `TSubclassOf` parameter fails to
  convert every way it can be passed), so `apply_auto_fbik()` is the supported
  path. Other solvers must be added in the IK Rig editor.
- **auto-FBIK brings its own goals** — `LeftHandIK`, `RightHandIK`,
  `LeftFootIK`, `RightFootIK` — so a created rig reports more goals than were
  requested. Yours are added alongside them.

Bone names cannot be listed from Python either (`Skeleton.bone_tree` yields
opaque structs), which is why a wrong bone can only be reported as rejected
rather than with a suggestion.

Live coverage: `npm run test:live:rigging` (34 assertions; needs a skeletal mesh
with mannequin bone names, skips cleanly otherwise; removes `/Game/__NGGRigTest__`).

### Niagara VFX

| Tool | Purpose |
|---|---|
| `ue5_create_niagara_system` | Create a blank `UNiagaraSystem` or clone from a template |
| `ue5_configure_niagara_system` | Add a sprite-burst emitter and set user-facing parameters |
| `ue5_set_niagara_emitter_params` | Set Rapid Iteration Parameters directly (bypasses `User.*` store) |

### Mesh composition (dynamic mesh handles)

| Tool | Purpose |
|---|---|
| `ue5_mesh_create` | Allocate a new dynamic-mesh handle |
| `ue5_mesh_append_primitive` | Append a Box / Sphere / Cylinder / etc. at a transform |
| `ue5_mesh_boolean` | Union / Subtract / Intersect two handles |
| `ue5_mesh_transform` | Translate / rotate / scale a mesh |
| `ue5_mesh_deform` | Twist / taper / bend along an axis |
| `ue5_mesh_remesh` | Retopologize with target edge length |
| `ue5_mesh_bake_static` | Bake a handle into a `UStaticMesh` asset |
| `ue5_mesh_delete_handle` | Free a handle |

### PCG (Procedural Content Generation)

| Tool | Purpose |
|---|---|
| `pcg_list_node_types` | List available `UPCGSettings` subclasses (optionally filter by category) |
| `pcg_create_graph` | Create a new `UPCGGraph` asset |
| `pcg_add_node` | Add a node by settings-class short name |
| `pcg_connect_pins` | Wire two pins (supports `__input__` / `__output__` aliases) |
| `pcg_save_graph` | Save a PCG graph to disk |

### Viewport & feedback loop

| Tool | Purpose |
|---|---|
| `ue5_set_viewport_camera` | Point the editor camera (explicit pose, or auto-frame a named actor) |
| `ue5_viewport_screenshot` | Capture the camera view to PNG via SceneCapture (works with a backgrounded editor) — Read it to *see* your edits |
| `ue5_get_log` | Tail the live editor log with severity/category/text filters |

### Play-In-Editor

| Tool | Purpose |
|---|---|
| `ue5_pie_start` | Start a PIE session (play or simulate) |
| `ue5_pie_stop` | End the PIE session |
| `ue5_pie_status` | Is PIE currently running? |

### DataTables

| Tool | Purpose |
|---|---|
| `ue5_datatable_create` | Create a DataTable bound to a row struct, optionally filling rows |
| `ue5_datatable_read` | Read struct, row names, and all row data as JSON |
| `ue5_datatable_set_rows` | Replace all rows from JSON |

### Sequencer

| Tool | Purpose |
|---|---|
| `ue5_sequencer_create` | Create a Level Sequence with frame rate + duration |
| `ue5_sequencer_bind_actor` | Bind a level actor and add transform keyframes |
| `ue5_sequencer_add_camera` | Spawn a CineCamera, bind it, add a camera-cut track |
| `ue5_sequencer_read` | Read the whole structure: bindings, tracks, sections, channels, keys — the counterpart to `ue5_bp_read_graph` |
| `ue5_sequencer_add_spawnable` | Add a binding the sequence spawns itself, from a class or an asset |
| `ue5_sequencer_add_track` | Add a track (transform, visibility, audio, animation, cameracut, event, property tracks…) |
| `ue5_sequencer_add_section` | Cut an extra section on a track, bounded in seconds |
| `ue5_sequencer_set_keys` | Key channels **by name** (`Location.X`, `Rotation.Z`), times in seconds |
| `ue5_sequencer_set_playback_range` | Retime the sequence and/or change its display rate |

**Channels are addressed by name, not index.** Call `ue5_sequencer_read` (or
read the `channels` returned by `ue5_sequencer_add_track`) to see what a section
exposes. Indices are not a stable contract — an off-by-one silently keys the
wrong property, which is exactly what the older `ue5_sequencer_bind_actor`
helper risks with its hard-coded "channels 0..2 = Location".

Times are given in **seconds** everywhere and converted using the sequence's own
display rate; the resulting frame numbers are echoed back in every result.

Live coverage: `npm run test:live:sequencer` (34 assertions, needs a running
editor; creates and removes `/Game/__NGGSeqTest__`).

### World Partition, Data Layers, Level Instances

| Tool | Purpose |
|---|---|
| `ue5_wp_read` | Is this world partitioned? Bounds, Data Layers, streaming manifest (counts by class, runtime grids) |
| `ue5_wp_create_data_layer` | Create a DataLayerAsset **and** instance it into the current world; runtime or editor type, optional nesting |
| `ue5_wp_set_actor_data_layers` | Add/remove level actors on a layer, by label; returns the layer's membership afterwards |
| `ue5_wp_set_data_layer_state` | Editor visibility, loaded-in-editor, and the runtime state the layer starts in |
| `ue5_wp_delete_data_layer` | Remove the layer instance from this level (the shared asset is kept) |
| `ue5_wp_load_region` | Load / unload / pin / unpin the actors inside a box, to edit a region without opening the whole map |
| `ue5_level_instance_create` | Place an existing level inside the current one as a Level Instance |

**A Data Layer is two objects**: a `DataLayerAsset` (shared content, reusable
across levels) and a `DataLayerInstance` (this level's use of it).
`ue5_wp_create_data_layer` makes both and reports both; deleting removes only
the instance.

Data Layers require a **World Partition** map. Make one with
`ue5_create_level` and `partitioned: true`. `ue5_wp_read` says plainly when the
open level is not partitioned, and the layer tools refuse with the same advice
rather than failing obscurely.

**Level Instances are half-covered.** UE 5.8 does not expose
`LevelInstanceSubsystem` to Python, so collapsing a selection of actors *into* a
new level instance is not reachable from here. Placing an existing level inside
another one is.

Live coverage: `npm run test:live:worldpartition` (42 assertions; creates a
scratch partitioned map, returns the editor to the level it started on, and
removes `/Game/__NGGWPTest__`).

### Audio

| Tool | Purpose |
|---|---|
| `ue5_metasound_create` | Build a MetaSound **Source or Patch** graph and save it as an asset — nodes, connections and exposed parameters in one call |
| `ue5_metasound_read` | Type, output format, and the graph inputs/outputs of an existing MetaSound |
| `ue5_metasound_set_graph_input_defaults` | Retune an existing MetaSound's exposed parameters without rebuilding it |
| `ue5_create_sound_cue` | Create a SoundCue from a SoundWave (WavePlayer wired; volume/pitch/loop) — the legacy path; prefer MetaSounds for new work |
| `ue5_create_sound_attenuation` | SoundAttenuation asset with distance falloff / shape / spatialization |
| `ue5_spawn_ambient_sound` | Spawn an AmbientSound actor with sound + volume + attenuation |
| `ue5_play_sound_preview` | Play a sound through the editor's audio device (audible to the user) |

**A MetaSound graph is built in one call.** UE 5.8's MetaSound scripting API
cannot find a node again in a later call — there is no node enumeration — so
`ue5_metasound_create` takes the whole graph at once and addresses nodes by ids
you choose, the same shape as `ue5_bp_add_logic`.

**Node classes are `namespace/name/variant`.** The variant is load-bearing:
audio-rate nodes (`Sine`, `Saw`, `Noise`) use `"Audio"`, while trigger and maths
nodes use none — passing the wrong one returns a bare failure with no
explanation. Give just the name and the tool tries both. Names are the node's
title in the MetaSound editor; there is no registry listing exposed to
scripting, so they must be exact, and the error says so when one misses.

For the same reason `ue5_metasound_read` reports the interface (graph inputs and
outputs) but not the node network — open the asset in the MetaSound editor for
that. This is an API limit, not a shortcut.

Live coverage: `npm run test:live:metasound` (28 assertions; creates and removes
`/Game/__NGGMSTest__`).

### Gameplay Ability System

| Tool | Purpose |
|---|---|
| `ue5_gas_setup_actor` | Add an AbilitySystemComponent to an actor BP (idempotent) |
| `ue5_gas_create_attribute_set` / `ue5_gas_add_attribute` / `ue5_gas_list_attributes` | AttributeSet BPs: create with attributes, append later, list |
| `ue5_gas_create_ability` / `ue5_gas_create_effect` | GameplayAbility / GameplayEffect BPs (policies, tags, modifiers, stacking) |
| `ue5_gas_grant_on_beginplay` | Wire BeginPlay → GiveAbility / ApplyGameplayEffectToSelf into the event graph |
| `ue5_gas_configure_asc` / `ue5_gas_read_setup` | Replication mode; read the full GAS setup |

### Level-actor components

| Tool | Purpose |
|---|---|
| `ue5_add_component_to_actor` | Add a component to a level actor *instance* (SubobjectDataSubsystem) |

### ADL exercises (extension routes — provided by the host project, not the plugin)

These tools call `/adl/exercise/*` routes that only exist when the host project
registers them through the bridge's extension API. Without them the tools
return 404 — hide the group with `NGG_TOOLSETS_EXCLUDE=exercises`.

| Tool | Purpose |
|---|---|
| `ue5_create_exercise` | Create/overwrite a `UAdlExerciseDefinition` with phases + steps |
| `ue5_add_phase_to_exercise` | Append one phase |
| `ue5_add_step_to_exercise` | Append one step to a phase |
| `ue5_get_exercise` | Fetch an exercise by `ExerciseID` |

### Misc

| Tool | Purpose |
|---|---|
| `ue5_batch` | Execute multiple bridge operations in a single HTTP round-trip |
| `ue5_meshy_generate` | Generate a 3D mesh via Meshy.ai v2 (preview + optional refine), download, import into `/Game/...` |

---

## Test suite

Two tiers:

### Unit (`npm test`)

333 tests via Node's built-in `node --test`. Covers:

- **`helpers.test.js`** — `jsonPreprocess`, `env()`
- **`paths.test.js`** — `findUProject`, `readEngineAssociation`, `discoverEngineDir` (env, HKCU, HKLM and launcher-manifest branches via dependency injection, plus the shell-injection guard on `EngineAssociation`), `discoverBuildTargets`, `projectNameFrom`
- **`ue5client.test.js`** — every wrapper in `ue5client.js`: path, method, body shape, optional-field elision, auth header forwarding, timeout handling, error propagation — all against a local mock HTTP server on an ephemeral port
- **`pcg.unit.test.js`** — PCG payload construction and `runPy` sentinel parsing (including malformed-JSON and missing-sentinel error paths)
- **`gamedev.test.js` / `newtools.test.js`** — payload construction for the gamedev and block-C tool families (sequencer, DataTables, GAS, audio, rendering, …)
- **`meshy.test.js`** — `resolveApiKey` env / file / missing / malformed / empty-key
- **`setup-assets.test.js`** — bundled skill/agent install payloads
- **`index.tools.test.js`** — tool-registry structure: unique names, allowed prefixes, and a check that every "N tools" claim in the docs matches the real registry count
- **`set-token.test.js`** — token generation and `Config/UserEngine.ini` writing
- **`rules.test.js`** — the rules-file split served over MCP resources
- **`toolsets.test.js`** — every tool belongs to exactly one toolset; allow/deny-list resolution

No UE editor required. Runs in under a second.

### Live (`npm run test:live`)

22 checks that hit a real running editor + plugin (plus per-subsystem suites —
`npm run test:live:<suite>` for `endpoints`, `sequencer`, `worldpartition`,
`metasound`, `rigging`, `rendering`, `assetconfig`, `interchange`, `substrate`,
`landscape`). The main suite exercises:

- Bridge health
- Content-browser listing
- Data-asset CRUD (create → set_property → get → save)
- Blueprint create + compile + read-graph
- Actor spawn / delete (plus defensive `clear selection` to avoid viewport crash)
- `batch(save_all + list_assets)` round-trip
- PCG list / create / add_node / save
- Cleanup via `execPython → EditorAssetLibrary.delete_asset`

Requires the editor to be open with `UnrealNGGMCP` enabled and the bridge
reachable. Uses scratch assets under `/Game/__NGGTest__/` and cleans up after
itself. Re-runnable — pre-cleanup step evicts stale scratch assets if a
previous run crashed.

---

## Repository layout

```
Plugins/UnrealNGGMCP/Source/ThirdParty/unrealngg-mcp/
├── index.js              MCP server entry (registers all tools with the SDK)
├── setup-assets.js       First-run installer: copies assets/ skills+agents into <project>/.claude
├── setup-assets.test.js  Unit suite for the installer (temp-dir based)
├── assets/               Bundled Claude skills + agents (manifest.json, skills/, agents/)
├── ue5client.js          HTTP wrapper around every UE5 plugin endpoint
├── pcg.js                PCG tools (drive UE via POST /editor/exec_python)
├── meshy.js              Meshy.ai v2 text-to-3D client
├── paths.js              .uproject / UE-install / build-target discovery
├── helpers.js            jsonPreprocess, env()
├── helpers.test.js       ┐
├── paths.test.js         │
├── ue5client.test.js     ├─ unit suite (node --test)
├── pcg.unit.test.js      │
├── meshy.test.js         ┘
├── live.test.mjs         live integration suite (requires UE running)
├── pcg.test.mjs          live PCG smoke test (standalone; complements live.test.mjs)
└── README.meshy.md       Meshy-specific usage notes
```

---

## Transports

- **stdio** (default): Claude Code spawns `node index.js` and talks over the child's stdin/stdout. No ports.
- **WebSocket**: set `NGG_MCP_TRANSPORT=ws` + `NGG_MCP_WS_PORT=<port>` to start a WebSocket server instead. Requires `@modelcontextprotocol/sdk >= 1.1.0`.

---

## Troubleshooting

**Bridge unreachable** — the editor is closed, the plugin is disabled, the port is blocked, or `NGG_BRIDGE_URL` points somewhere else. Open the Output Log and search for `LogNGGBridge`.

**Timeouts on first request** — a cold editor with unloaded modules can take 20–40 s to respond to the first asset-creation request. Bump `NGG_BRIDGE_TIMEOUT_MS=60000`.

**401 Unauthorized** — the token the client resolved (from `NGG_BRIDGE_TOKEN`, or the `Config/UserEngine.ini` → `Config/DefaultEngine.ini` fallback) doesn't match the `AuthToken` the running editor loaded at startup. Usually a stale env var, or the ini was edited after the editor started — unset the env var or restart the editor. `npm run set-token -- --print` shows what the client will send.

**`exec_python` or any `pcg_*` tool returns 403** — no token is configured, so the bridge keeps those routes disabled. Run `npm run set-token` and restart the editor. See [Setting the bridge token](#setting-the-bridge-token).

**Live test fails with "fetch failed"** — the editor crashed. Check `Saved/Logs/<YourProject>.log` for the most recent `Assertion failed` line. Most recurring crashes touch `TypedElementRegistry` in the selection subsystem; see the plugin README's *Selection safety* section.

**Tests pass but Claude Code doesn't see the tools** — the MCP server exited mid-init, usually because `.uproject` wasn't found or `NGG_UE_INSTALL_DIR` couldn't be resolved. Run `node index.js` once in a shell from the project root to see the startup error.
