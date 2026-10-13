# UnrealNGGMCP — Tutorial & Prompt Cookbook

> **Build Unreal Engine 5 content by talking to an AI.**
> UnrealNGGMCP turns Claude into an actual collaborator inside the UE5 Editor — it spawns Blueprints, wires Animation Graphs, generates PCG networks, builds Niagara systems, creates materials, places actors, writes ability sets, and lints your project, all from natural-language prompts.

![Hero placeholder — drop a 20s GIF: prompt "build me a third-person shooter character" → camera pans across spawned Blueprint, Animation BP, Input Mapping Context, GAS attributes](media/hero.gif)

---

## Why this exists

Most UE5 work is *clicking the same 6 panels in the same order*. You know what you want to build. You just don't want to spend 40 minutes opening the Skeleton, finding the bone, right-clicking, picking the IK node, naming it, plugging pins.

UnrealNGGMCP lets you describe the result and skip the clicking. Claude understands the Editor — it can read your assets, modify Blueprints, build PCG graphs, configure Niagara emitters, and verify its own work via the Asset Registry. It also explains *what* it's doing so you learn the UE5 idioms instead of being locked into a black box.

**Three things make this different from a "chat plugin":**

1. **It edits real assets, not chat replies.** Every prompt produces actual `.uasset` mutations the Editor sees instantly. No copy-pasting code.
2. **It understands UE5 semantics.** It uses the Asset Registry, BP graph schema, PCG node types, and GAS conventions — not generic file edits.
3. **It survives editor restarts.** A long-lived sidecar daemon keeps the conversation alive across recompiles, restarts, and crashes. Pick up exactly where you left off.

---

## Table of Contents

1. [Quick Start (5 min)](#quick-start-5-min)
2. [The two ways to use it](#the-two-ways-to-use-it)
3. [Video / GIF storyboards](#video--gif-storyboards) — recording scripts for marketplace assets
4. [Prompt Cookbook](#prompt-cookbook) — copy-paste prompts by feature area
5. [Feature matrix](#feature-matrix)
6. [Setup details](#setup-details) — for engineers integrating MCP
7. [Troubleshooting](#troubleshooting)
8. [Marketplace asset checklist](#marketplace-asset-checklist)

---

## Quick Start (5 min)

### 1. Install the plugin

Drop `UnrealNGGMCP/` into your project's `Plugins/` folder (or install from the Epic Marketplace once published). Restart the Editor. The HTTP bridge auto-starts at `http://localhost:6776` and you'll see this in the Output Log:

```
LogNGGBridge: UnrealNGGMCP: HTTP bridge listening on http://localhost:6776
```

### 2. Open the Chat tab

`Window → Claude Chat` (or use the toolbar button). The first time you open it, it spawns a long-lived Node daemon ("sidecar") that hosts your Claude Code session. The session survives editor restarts and rebuilds.

### 3. Send your first prompt

In the chat box, type:

> List every Blueprint in `/Game/Characters` and tell me which ones have a `Health` variable.

Claude inspects the Asset Registry, reads each Blueprint's variable list, and answers — no scripts, no Python, no Blueprint code. **You did not touch the editor.**

### 4. Now make something

> Create a Blueprint called `BP_HealthPickup` parented to `Actor`. Give it a `StaticMeshComponent` (use the engine sphere mesh), a `SphereCollision` of radius 50, a `Float` variable `HealRate = 25`, and a `BeginOverlap` event that calls a custom function `ApplyHeal`.

Open `Content Browser → Characters/` and the asset is already there, compiled, ready to drop into the level.

That's the whole loop. **Read assets. Mutate assets. Iterate.**

---

## The two ways to use it

| Surface | When to use | How it talks to UE5 |
|---|---|---|
| **In-Editor Chat tab** | Day-to-day work, level design, "build me X", iterating inside the editor. No setup beyond enabling the plugin. | Slate widget → WebSocket → sidecar daemon → headless `claude` CLI → MCP server → HTTP bridge → UE5 Editor |
| **External Claude Code CLI** | Pair-programming with code + assets, multi-project workflows, CI integration, when you want Claude in your terminal alongside `git` | Claude Code → MCP server (`unrealngg-mcp`) → HTTP bridge → UE5 Editor |

Both surfaces share the same MCP tool set. Pick whichever fits your workflow — most users live in the Chat tab.

---

## Video / GIF storyboards

Eight short recordings to film for the Marketplace listing carousel. Each is 15-30 seconds. Save as `Plugins/UnrealNGGMCP/Docs/media/<slug>.gif`. Recording tips at the [bottom of this section](#recording-tips).

### Storyboard 1 — "Spawn a Blueprint character in one prompt"

**Length:** 25 seconds
**Slug:** `gif-01-spawn-character.gif`

| Frame | Duration | Show |
|---|---|---|
| Cold open | 2s | Empty `/Game/Characters` folder in Content Browser. |
| Prompt | 4s | Camera over chat panel; type `Create a Character BP called BP_Hero with a SkeletalMesh using SK_Mannequin, a SpringArm + Camera, and CharacterMovement with MaxWalkSpeed=600.` |
| Generation | 12s | Cut to Content Browser as the asset appears, then to the Blueprint Editor opening to show the component tree filled in. |
| Result | 5s | Drag the BP into the level. Hit Play. The character runs around. |
| Hold | 2s | Pause on "25 seconds. No clicking." |

**Pull-quote overlay:** *"Type the character. Get the character."*

---

### Storyboard 2 — "PCG forest from a sentence"

**Length:** 30 seconds
**Slug:** `gif-02-pcg-forest.gif`

| Frame | Duration | Show |
|---|---|---|
| Cold open | 2s | Empty Landscape level with one volume. |
| Prompt | 5s | `Build a PCG graph called PCG_Forest that scatters SM_Tree_Pine on the volume, density 1 per 500cm², jittered, with a 200cm exclusion radius around the player start.` |
| Graph build | 12s | Cut to PCG editor — nodes appearing, connecting one by one (Surface Sampler → Density Filter → Transform Points → Static Mesh Spawner). |
| Generation | 6s | Back to viewport — trees populating. |
| Tweak | 5s | One more prompt: `make the density half and add SM_Tree_Birch mixed in 30%`. Trees update live. |

**Pull-quote:** *"Real PCG networks, not blackbox magic."*

---

### Storyboard 3 — "Animation graph wiring"

**Length:** 25 seconds
**Slug:** `gif-03-anim-graph.gif`

| Frame | Duration | Show |
|---|---|---|
| Cold open | 2s | Skeletal mesh in the viewport with default A-pose. |
| Prompt | 5s | `Create an Anim BP for SK_Mannequin. Add a state machine: Idle/Walk/Run blendspace based on Speed, a layered upper-body blend on slot "UpperBody", and a Two-Bone IK on the right hand to socket "weapon_grip".` |
| Graph fill | 14s | Cut between Anim Graph (nodes appearing) and State Machine (states + transitions appearing with their rules). |
| Result | 4s | Preview window: character walks, runs, plays an upper-body shoot animation on top. |

**Pull-quote:** *"From skeleton to playable in 25 seconds."*

---

### Storyboard 4 — "Gameplay Ability Set from scratch"

**Length:** 30 seconds
**Slug:** `gif-04-gas-setup.gif`

| Frame | Duration | Show |
|---|---|---|
| Cold open | 2s | Empty `/Game/GAS/` folder. |
| Prompt | 6s | `Set up a basic GAS pawn: AttributeSet with Health/MaxHealth/Mana/MaxMana, a GameplayEffect "GE_Damage_Instant" that reduces Health by Magnitude, and a GameplayAbility "GA_Fireball" that costs 20 Mana and applies GE_Damage_Instant to the target.` |
| Asset spam | 14s | Folder fills with the AttributeSet, GE, GA, all parented correctly. Cut to the GA Blueprint showing the cost/effect wired up. |
| Test | 8s | In-level test: cast Fireball → mana drops → target health drops → visible bar update. |

**Pull-quote:** *"Set up GAS without reading 4 tutorials."*

---

### Storyboard 5 — "Niagara from description"

**Length:** 20 seconds
**Slug:** `gif-05-niagara.gif`

| Frame | Duration | Show |
|---|---|---|
| Cold open | 2s | Dark scene, single point light. |
| Prompt | 5s | `Make a Niagara system "NS_Embers" — 300 GPU particles, upward drift with turbulence, orange→red color over life, additive blending, lifespan 1.5-3s.` |
| Build | 8s | Cut to Niagara editor — emitter spawning, modules being added. |
| Result | 5s | Drop in scene. Embers float up. |

**Pull-quote:** *"Describe the look. Get the system."*

---

### Storyboard 6 — "Refactor by selection"

**Length:** 25 seconds
**Slug:** `gif-06-selection-refactor.gif`

| Frame | Duration | Show |
|---|---|---|
| Cold open | 3s | Blueprint Editor open with a messy event graph. User clicks 5 nodes and hits the `+ Selection` button in chat. |
| Prompt | 5s | `Pull these nodes into a function called CalculateDamage, with inputs BaseDamage and Multiplier, return Float.` |
| Refactor | 12s | Watch nodes disappear from the event graph, the new function appear in the My Blueprint panel, get opened, and show the extracted logic. Original call site now has a function call node. |
| Compile | 5s | Click Compile. Green checkmark. |

**Pull-quote:** *"Refactor Blueprints like code."*

---

### Storyboard 7 — "Lint your project"

**Length:** 20 seconds
**Slug:** `gif-07-lint.gif`

| Frame | Duration | Show |
|---|---|---|
| Cold open | 2s | Content Browser with ~30 Blueprints in `/Game/`. |
| Prompt | 4s | `Lint every Blueprint under /Game/Characters and tell me the top 5 issues.` |
| Scan | 8s | Progress dots in chat → output appears with line-by-line warnings (unused variables, missing Super calls, Tick on cheap actors). |
| Fix | 6s | Follow-up prompt: `fix the unused variables.` Watch BPs save themselves. |

**Pull-quote:** *"Codebase hygiene for Blueprints."*

---

### Storyboard 8 — "Level dressing with one paragraph"

**Length:** 30 seconds
**Slug:** `gif-08-level-dressing.gif`

| Frame | Duration | Show |
|---|---|---|
| Cold open | 3s | Empty level with one PlayerStart. |
| Prompt | 7s | `Create a courtyard scene: 10x10 stone tiles for the floor, four walls with arched windows, a directional light tilted for golden-hour, exponential height fog at density 0.5, and a post-process volume with a slight teal tint.` |
| Build | 16s | Time-lapse: tiles snap into place, walls extrude, light rotates, fog rolls in, PP volume tints. |
| Reveal | 4s | Final beauty shot. |

**Pull-quote:** *"Block out a scene in the time it takes to write a paragraph."*

---

### Recording tips

- **Resolution:** 1920×1080 minimum. Marketplace upscales but downscales look bad.
- **Frame rate:** 30 fps for GIFs, 60 fps for any video that ends up on YouTube.
- **Capture tool:** ShareX (free), ScreenToGif (free, Windows), or OBS for longer clips.
- **Trim aggressively** — the moment a prompt finishes typing, *cut*. Don't film the model thinking.
- **Show the prompt fully** before cutting. Viewers need to read what was typed.
- **Speed up generation time** by 1.5×–2× in post. The result is what sells, not the wait.
- **Always end on the working asset.** No "blackness." No editor close.
- **No audio in GIFs.** Save voiceover for the hero video only.

---

## Prompt Cookbook

Copy-paste, edit the bracketed bits, send.

### Blueprints

> **Create a Blueprint class**
> Create a Blueprint called `BP_<Name>` parented to `<Actor|Character|Pawn|GameMode>` in `/Game/Blueprints/`.

> **Add components**
> To `BP_<Name>`, add a `StaticMeshComponent` named `Mesh` using `/Engine/BasicShapes/Cube`, a `SphereCollision` of radius 100 named `Trigger`, and a `PointLight` offset Z=200 with Intensity 5000.

> **Create variables in bulk**
> On `BP_<Name>`, add these variables: `Health (Float) = 100`, `MaxHealth (Float) = 100`, `bIsAlive (Boolean) = true`, `DamageType (Class<UDamageType>)`, `Inventory (Array of Object)`.

> **Wire an overlap event**
> On `BP_HealthPickup`, add a `BeginOverlap` event on `Trigger`. Cast the OtherActor to `BP_Hero`, call `ApplyHeal(HealRate)` on it, then call `DestroyActor` on self.

> **Extract a function**
> Pull the selected nodes into a function called `<Name>` with inputs `<Type Name, …>` and outputs `<Type Name, …>`.

> **Refactor selection**
> Replace the selected `Branch + SetTimer + Delay` chain with a single `Set Timer by Function Name` using the same delay.

> **Lint**
> Lint `BP_<Name>` and tell me what's wrong. / Lint every Blueprint under `/Game/Characters` and list the top issues.

### Animation

> **Create an Anim BP**
> Create an Anim BP for `SK_Mannequin` saved as `/Game/Animation/ABP_Hero`. Add a state machine "Locomotion" with `Idle/Walk/Run` driven by `Speed` (float), and add a layered bone blend for the `upperbody` slot.

> **Aim offset**
> Add an Aim Offset Blend Space pose using `AO_Pistol`, driven by `AimPitch` and `AimYaw`, blended additively over the state machine.

> **IK**
> Add a Two-Bone IK on `hand_r` targeting socket `weapon_grip` with alpha driven by `IKAlpha`. Add Hand IK retargeting between `hand_l` and `weapon_left_grip`.

> **Modify bone**
> Add a `ModifyBone` on `head` driving rotation to look at the player camera, clamped ±60°.

### PCG

> **Scatter from a prompt**
> Create a PCG graph `PCG_Rocks` that surface-samples the parent actor's static mesh, filters to slopes under 30°, then spawns `SM_Rock_01-04` with random rotation and scale 0.5-1.5.

> **Parameterize**
> Add graph parameters: `Density (float, default 1.0)`, `SeedOffset (int32, default 0)`, `MeshTag (Name, default "Rock")`. Drive Surface Sampler density from `Density` and Transform Points scale from `1 + (Density × 0.1)`.

> **Instance and tweak**
> Create a PCG graph instance of `PCG_Rocks` at the selected actor and override `Density` to 0.3.

### Materials

> **Master material**
> Create a master material `M_SciFi_Trim` with vector parameters `BaseColor`, `EmissiveColor`, scalar parameters `EmissiveIntensity`, `Roughness`, `Metallic`, and a `TexCoord × Tiling` driving 3 sampler params: `BaseColorMap`, `NormalMap`, `RoughnessMap`.

> **Post-process material**
> Create a post-process material `PPM_Vignette` that darkens the screen edges with a configurable radius and intensity. Spawn a `PostProcessVolume` covering the level using it.

> **Instance**
> Create a material instance `MI_SciFi_Trim_Red` of `M_SciFi_Trim` with `BaseColor = (0.8, 0.05, 0.05, 1)`, `EmissiveIntensity = 4`.

### Niagara

> **Effect from description**
> Create a Niagara system `NS_<Name>` — `<N>` GPU particles, `<motion description>`, color `<gradient description>`, lifespan `<min-max>`, `<additive|translucent|opaque>`. Save under `/Game/VFX/`.

> **Tweak params**
> On `NS_Embers`, set `SpawnRate = 200`, `InitialVelocity Z = (50, 80)`, `Color over Life = (Orange → DeepRed)`.

### UMG

> **Create a HUD**
> Create a widget BP `WBP_HUD` with a vertical stack of: ProgressBar "HealthBar" bound to `Health/MaxHealth`, ProgressBar "ManaBar" bound to `Mana/MaxMana`, TextBlock "Score" bound to `Score`.

> **Restyle**
> Restyle every TextBlock in `WBP_HUD` to use font `Roboto Bold`, size 18, color white with a 2px black drop shadow.

### Gameplay Ability System

> **Bootstrap**
> Set up GAS on `BP_Hero`: add `AbilitySystemComponent`, create `UAttributeSet_Hero` with `Health`, `MaxHealth`, `Mana`, `MaxMana`, `Stamina`, `MaxStamina`. Default values 100/100/100/100/100/100.

> **Effect**
> Create `GE_Damage` (Instant) reducing `Health` by Magnitude, with `Damage` set caller magnitude. Create `GE_Burn` (Duration 5s, period 1s) ticking 5 damage per tick.

> **Ability**
> Create `GA_Fireball`: cost 20 Mana via `GE_Cost_Mana20`, cooldown 3s via `GE_CD_Fireball`, activation tags `Ability.Fireball`, applies `GE_Damage` to target with magnitude 50.

### Levels & Actors

> **Spawn**
> Spawn 5 `BP_Enemy` actors in a 1000cm circle around the PlayerStart, facing the center.

> **Update**
> Set every `BP_Lamp` in the level to `Intensity = 12000`, `LightColor = warm white (1, 0.85, 0.6)`.

> **Block out a scene**
> Build a corridor: 10 stone wall pieces tiled along X with 200cm spacing, ceiling at Z=400, floor at Z=0, two point lights every 800cm at Z=350.

### Behavior Trees

> **AI tree**
> Create a Behavior Tree `BT_Guard` with blackboard `BB_Guard` (keys: `TargetActor (Actor)`, `PatrolPoint (Vector)`, `LastKnownLocation (Vector)`). Tree: Selector → [Sequence: HasTarget? → MoveTo Target → Attack] → [Sequence: PatrolPoint set → MoveTo PatrolPoint → Wait 2s].

### Procedural meshes

> **Generate**
> Use Meshy to generate a `weathered medieval crate` mesh, bake it to a static mesh at `/Game/Props/SM_Crate_Weathered`, add a socket `Top` at the top center.

> **Boolean**
> Take `SM_Wall` and subtract a 100×200×400 box at offset (0, 0, 100) to cut a doorway. Save as `SM_Wall_Door`.

### Profiling

> **Trace and analyze**
> Start an Insights trace named `combat-perf`, play the level for 30 seconds, stop the trace, then analyze it and list the top 5 expensive ticks.

> **Find spikes**
> In the most recent trace, find any frames over 33ms and tell me what was running.

### Project hygiene

> **Inventory**
> List every asset in `/Game/Characters/` with type, size, and last modified date.

> **Find references**
> What references `M_Glass_Old`? If nothing does, delete it.

> **Lint everything**
> Run `bp_lint_project` and group findings by severity.

---

## Feature matrix

| Area | Tools | Status |
|---|---|---|
| **Blueprints** | Create class / struct / enum / interface, reparent, add/remove components, create variables / functions / macros, add/connect/delete nodes, set defaults, list variables, read graph, compile, lint per-asset & project-wide | ✅ Full |
| **Animation** | Create Anim BP, configure state machines, add Aim Offset / Sequence Player / Two-Bone IK / Hand IK / Look-At / Modify Bone / Copy Bone / Layered Bone Blend, skeleton virtual bones | ✅ Full |
| **PCG** | Create graphs & instances, add/remove/rename/position nodes, connect pins, set settings properties, manage graph parameters, attach components to actors, generate / regenerate / cleanup | ✅ Full |
| **GAS** | Configure ASC, create Attribute Sets / Gameplay Effects / Gameplay Abilities, set up actors, read setup | ✅ Full |
| **UMG** | Create widget BPs, add/remove/rename/reparent widgets, get widget tree, style widgets, compile | ✅ Full |
| **Materials** | Master materials, material instances, post-process materials, spawn PP volumes | ✅ Full |
| **Niagara** | Create system, configure system, set emitter parameters | ✅ Full |
| **Behavior Trees** | Create blackboard & tree, add blackboard keys, add logic, read tree | ✅ Full |
| **Mesh editing** | Create / append primitives / boolean / deform / remesh / transform / bake / sockets / Meshy AI generation | ✅ Full |
| **Levels** | Create / open / set environment / world settings / spawn / update / delete actors | ✅ Full |
| **Input** | Configure Input Mapping Contexts | ✅ Full |
| **Profiling** | Start / stop / list / status traces, region bookmarks, analyze | ✅ Full |
| **Asset registry** | List / get / set property / import / delete assets, list gameplay tags | ✅ Full |
| **Editor lifecycle** | Health check, launch, kill, build, save all | ✅ Full |

---

## Setup details

The full setup reference lives in [`Plugins/UnrealNGGMCP/README.md`](../README.md). This is the 30-second version for engineers.

### Architecture at a glance

```
┌─────────────────────────┐        ┌─────────────────────────┐
│  In-Editor Chat tab     │        │   External Claude Code  │
│  (Slate widget)         │        │   CLI / Web / API       │
└────────────┬────────────┘        └────────────┬────────────┘
             │ WebSocket :{port}                │ stdio MCP
             ▼                                  ▼
┌─────────────────────────┐        ┌─────────────────────────┐
│  ngg-sidecar (Node)     │        │  unrealngg-mcp (Node)   │
│  Long-lived daemon      │        │  MCP server             │
│  hosting `claude` CLI   │        └────────────┬────────────┘
└────────────┬────────────┘                     │
             │ MCP over stdio                   │
             └──────────────────┬───────────────┘
                                │ HTTP :6776
                                ▼
                  ┌─────────────────────────────┐
                  │  UnrealNGGMCP (UE5 plugin)  │
                  │  Editor HTTP bridge         │
                  └─────────────────────────────┘
```

### Required external dependencies

| Component | Minimum version | Where |
|---|---|---|
| Unreal Engine | 5.4 LTS | host project |
| Node.js | 18.0.0 | machine PATH |
| Claude Code | any current | machine PATH (only required for the Chat tab) |
| OS | Windows 10/11 64-bit | host machine |

### Per-project port isolation

Each project's bridge and sidecar pick a project-unique port derived from the `.uproject` path hash (`[20000, 29999]` range). Running two projects side-by-side does **not** cause port collisions or log-file collisions (each sidecar writes to `sidecar-<port>.log`).

### Permissions model

Every tool call goes through Claude Code's permission system. The Chat tab surfaces approvals in-pane so you can:

- **Approve once** — runs this tool with these args.
- **Approve always for this tool** — auto-approve future invocations.
- **Deny** — block the call.
- **Bypass mode** — toggle for trusted edit sessions.

Read-only tools (asset listing, lint, graph reading, health checks) auto-approve.

### Adding custom HTTP routes

The plugin exposes a `RegisterRoute()` extension API for adding project-specific endpoints. See [`Plugins/UnrealNGGMCP/README.md#adding-custom-routes-extension-api`](../README.md#adding-custom-routes-extension-api).

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| Chat tab says "Sidecar did not start" | Node.js not on PATH, or `index.js` missing | `node --version` should print 18+. Verify `Plugins/UnrealNGGMCP/Source/ThirdParty/ngg-sidecar/index.js` exists. Set `NGG_NODE_PATH` env var to an explicit path if needed. |
| "Sidecar still not alive after 4s — retrying spawn once" | Stale sidecar holding port/log file | The retry usually clears it. If it persists, kill any stray `node.exe` processes in Task Manager and reopen the Chat tab. Each project now uses its own log file (`sidecar-<port>.log`), so cross-project collisions are no longer a cause. |
| MCP tools fail with `ECONNREFUSED` | HTTP bridge isn't listening | Check Output Log for `LogNGGBridge: ... listening on http://localhost:6776`. If absent, the plugin failed to load — check for compile errors. |
| Claude says a tool doesn't exist | MCP server isn't connected, or schema mismatch | Run `/mcp` in Claude Code to reload servers. From the Editor Chat tab: hit *Reconnect / Restart Sidecar*. |
| Permission prompts hang forever | No UE Editor connected to approve | The sidecar denies the call after the prompt is dispatched but no client picks it up. Open the Chat tab so prompts have somewhere to land. |
| Two projects open, only one's chat works | Pre-1.4 cross-project log collision | Update to the latest plugin — fixed by per-project log isolation. Kill the orphaned `node.exe` once after upgrading. |

Logs:

- **Windows:** `%LOCALAPPDATA%\NGG\sidecar-<port>.log` and the UE Output Log channels `LogNGGBridge`, `LogNGGChat`.
- **macOS:** `~/Library/Logs/NGG/sidecar-<port>.log`
- **Linux:** `~/.local/state/NGG/sidecar-<port>.log`

---

## Marketplace asset checklist

When publishing on the Epic Games Store / Fab, you need:

### Required

- [ ] **Featured image** — 1920×1080. Hero shot of the Chat tab open in the editor with a beautiful scene Claude built behind it.
- [ ] **Gallery images** (5-25, 1920×1080) — one per major feature: BP creation, PCG, Animation Graph, GAS, Niagara, UMG, Materials, BehaviorTree.
- [ ] **Thumbnail** — 284×284. Plugin logo + "AI Assistant" tagline.
- [ ] **Featured video** (optional but strongly recommended) — 1-2 min walkthrough using Storyboard 1 + 2 + 4 + 6 strung together with voice-over.
- [ ] **Long description** — first 3 sentences are everything. Lead with "Type prompts. Get UE5 assets." Don't open with "this is a plugin that exposes…"
- [ ] **Short description** — single sentence under 150 chars. "AI co-developer for UE5 — generates Blueprints, animation graphs, PCG networks, materials, and full GAS setups from natural-language prompts."
- [ ] **Tags** — `AI`, `Blueprint`, `Productivity`, `PCG`, `Animation`, `Procedural`, `Tools`, `Editor`.

### Recommended additions

- [ ] **GIF storyboards 1-8 from this doc**, rendered and embedded in the long description.
- [ ] **Pinned demo project** the buyer can download to see live examples.
- [ ] **Public documentation link** — point `DocsURL` in `UnrealNGGMCP.uplugin` to a hosted version of this Tutorial.md.
- [ ] **Support channel** — set `SupportURL` in the `.uplugin`. A Discord or GitHub Issues link.
- [ ] **Comparison table** vs. "ChatGPT for Unreal" type tools — emphasize: real asset mutation, asset-registry awareness, persistent conversation, PCG/GAS/Animation depth.

### Pre-submit smoke test (do these in a fresh clone of a vanilla UE5 project)

- [ ] Plugin loads on PostEngineInit without errors.
- [ ] Chat tab opens, sidecar spawns, "Ready" status appears within 10 seconds.
- [ ] Run Storyboards 1, 4, 7 end-to-end against the fresh project — every prompt must complete cleanly.
- [ ] Close and reopen the Editor — Chat history is preserved (sidecar persistence working).
- [ ] Open a second project simultaneously — second Chat tab works independently (per-project isolation working).
- [ ] Lint the demo project — `bp_lint_project` returns clean.

---

## Next steps

- **Day 1:** Run Storyboards 1, 2, 4 yourself, save the GIFs to `Docs/media/`, link them into this doc.
- **Day 2:** Record the hero video using Storyboards 1+2+4+6 with voice-over.
- **Day 3:** Build the demo project (one level, one character, one PCG scatter, one ability) and zip it.
- **Day 4:** Fill out the Epic Marketplace submission form, attach gallery + featured assets, copy the long description from this doc's [Why this exists](#why-this-exists) section.
- **Day 5:** Submit. While in review, set up the Discord + GitHub Issues.

Good luck shipping. Questions on the doc itself → edit this file and PR.
