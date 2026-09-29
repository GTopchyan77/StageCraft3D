# Conversion vault template (seed)

This folder is a **template** for a Blueprint→C++ conversion vault. It ships generic methodology so the
`bp-extract` pipeline (and the Obsidian-based tracking around it) can be bootstrapped in any UE project.
It is a *copy* of the generic docs — adapt them per project.

## What's here

| File | Reusable as-is? | Notes |
|---|---|---|
| `BP_TO_CPP_PLAYBOOK.md` | Mostly | Canonical methodology. The "Project adaptations" section + the `GameAnimationSample` / `SandboxCharacter` / `GameAnim` references are **examples** — adapt them to your project. |
| `Conversion Guide.md` | Mostly | Quick-start loop + tool index. Update the example invocation paths/token if yours differ. |
| `Home.md`, `Conversion Tracker.md` | Yes | Dataview dashboards (need the Obsidian Dataview plugin); fully generic. |
| `_Templates/Asset Conversion Note.md` | Yes | Templater template for per-asset notes (needs the Templater plugin). |
| `_Templates/System Design Note.md` | Yes | Templater template for per-system design notes. |
| `Capabilities.md`, `Commands (RU).md` | Reference | Menu of what the pipeline can do + a Russian command cheatsheet — trim project specifics. |
| `.obsidian/` | Yes | Vault config (community-plugins/core-plugins/templates) so the vault opens with Dataview + Templater + the `_Templates` folder set. No plugin binaries and no `workspace.json` — Obsidian installs the community plugins on first open. |
| `BlueprintConvert.json.example` | Yes | Sample `bp-extract` config; copy to your **project root** as `BlueprintConvert.json` and edit. |

## Bootstrapping a new project

1. Copy the **contents** of this folder (including the hidden `.obsidian/`) into your project's vault
   root: `YourProject/BlueprintConvertInfo/`  (omit this `README.md` and `BlueprintConvert.json.example`).
2. Copy `BlueprintConvert.json.example` → `YourProject/BlueprintConvert.json` and set at least `mirror_token`
   (a short C++ identifier prefix, e.g. `GameAnim`). Module / API / source auto-derive from the editor's
   `/project_info`, or pin them in the same file. (Skip this entirely to accept the auto-derived names.)
3. Open `YourProject/BlueprintConvertInfo/` as an Obsidian vault. The bundled `.obsidian/` already enables
   Dataview + Templater; on first open Obsidian will prompt to install/trust those community plugins. (This
   step is optional — the pipeline itself doesn't need Obsidian; it's just how the tracker is read/edited.)
4. Author one note per convertible Blueprint under `Assets/<System>/` (use the `_Templates/` note), then run
   the pipeline from the project root:
   `py Plugins\UnrealNGGMCP\Tools\bp-extract --from-vault` → `order` → `brief` → `codegen`.

The **live** vault (your `Assets/**` notes, `Systems/**` design notes, and the generated `_extracted/` /
`_briefs/` / `Conversion Order.md`) lives in your project — never back in the plugin.

> **Maintenance:** these files are template copies. When the methodology improves in a live project, re-sync
> the changed files back into this `vault-template/` so new projects inherit the improvement.
