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
| UE install dir | 1. env var; 2. Windows registry `HKCU\Software\Epic Games\Unreal Engine\Builds`; 3. scan every drive for `UE_<version>` / `Program Files/Epic Games/UE_<version>` | `NGG_UE_INSTALL_DIR` |
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
| `NGG_BRIDGE_TOKEN` | *(ini fallback)* | Bearer token sent in `Authorization` header. Must match `AuthToken` in the project's `Config/DefaultEngine.ini`. When unset, the client reads `[UnrealNGGMCP] AuthToken` from that ini automatically, so standalone runs (live tests, `node index.js`) need no env setup. |
| `NGG_UE_PROJECT_PATH` | auto | Absolute path to the `.uproject` |
| `NGG_UE_INSTALL_DIR` | auto | Root of the Unreal Engine install |
| `NGG_MCP_TRANSPORT` | `stdio` | Set to `ws` for WebSocket transport |
| `NGG_MCP_WS_PORT` | `6777` | WebSocket port (only if `NGG_MCP_TRANSPORT=ws`) |
| `MESHY_API_KEY` | *(empty)* | Required for `ue5_meshy_generate`. Falls back to `.meshy.json` at project root. |

Env vars are read **per request**, not at import — tests can override them
dynamically. See `ue5client.js::bridgeUrl()` / `timeoutMs()` / `authToken()`.

---

## Tools

171 MCP tools total, grouped below (major tools listed; run `/mcp` in Claude
Code for the complete live catalog with argument schemas).

### Editor lifecycle

| Tool | Purpose |
|---|---|
| `ue5_health_check` | Confirm the bridge is reachable |
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
| `ue5_create_level` | Create and open a new empty level |
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

### Audio

| Tool | Purpose |
|---|---|
| `ue5_create_sound_cue` | Create a SoundCue from a SoundWave (WavePlayer wired; volume/pitch/loop) |
| `ue5_create_sound_attenuation` | SoundAttenuation asset with distance falloff / shape / spatialization |
| `ue5_spawn_ambient_sound` | Spawn an AmbientSound actor with sound + volume + attenuation |
| `ue5_play_sound_preview` | Play a sound through the editor's audio device (audible to the user) |

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

### ADL (optional project-specific module)

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

124 tests via Node's built-in `node --test`. Covers:

- **`helpers.test.js`** — `jsonPreprocess`, `env()`
- **`paths.test.js`** — `findUProject`, `readEngineAssociation`, `discoverEngineDir` (env branch + registry branch via dependency injection), `discoverBuildTargets`, `projectNameFrom`
- **`ue5client.test.js`** — every wrapper in `ue5client.js` (53 functions): path, method, body shape, optional-field elision, auth header forwarding, timeout handling, error propagation — all against a local mock HTTP server on an ephemeral port
- **`pcg.unit.test.js`** — PCG payload construction and `runPy` sentinel parsing (including malformed-JSON and missing-sentinel error paths)
- **`meshy.test.js`** — `resolveApiKey` env / file / missing / malformed / empty-key

No UE editor required. Runs in ~900 ms.

### Live (`npm run test:live`)

26 tests that hit a real running editor + plugin. Exercises:

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

**401 Unauthorized** — the token the client resolved (from `NGG_BRIDGE_TOKEN`, or the `Config/DefaultEngine.ini` fallback) doesn't match the `AuthToken` the running editor loaded at startup. Usually a stale env var, or the ini was edited after the editor started — unset the env var or restart the editor.

**Live test fails with "fetch failed"** — the editor crashed. Check `Saved/Logs/<YourProject>.log` for the most recent `Assertion failed` line. Most recurring crashes touch `TypedElementRegistry` in the selection subsystem; see the plugin README's *Selection safety* section.

**Tests pass but Claude Code doesn't see the tools** — the MCP server exited mid-init, usually because `.uproject` wasn't found or `NGG_UE_INSTALL_DIR` couldn't be resolved. Run `node index.js` once in a shell from the project root to see the startup error.
