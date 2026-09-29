# bp-extract — Blueprint introspection extractor

A read-only Python tool that pulls every Blueprint's **variables, functions,
types, components, parent/interfaces and graph topology** out of the running
UE5 editor (via the UnrealNGGMCP HTTP bridge) and writes one normalized JSON
file per asset. It is **step 1** of the Blueprint→C++ port: the JSON it produces
is the input a later code-generation phase consumes. It never mutates or saves
any asset — only GET routes are issued.

This tool ships **inside the UnrealNGGMCP plugin** (`Plugins/UnrealNGGMCP/Content/Python/bp-extract/`) so it
travels with the plugin to any UE project. It is project-agnostic — all project-specific values resolve
through `config.py` (see Configuration).

## Prerequisites

- **Python ≥3.10** (stdlib only — nothing to `pip install`; uses PEP-604 `X | None` unions).
- The **UE5 editor open** with the UnrealNGGMCP plugin loaded (only for `--from-vault` / `--asset`
  extraction — `order` / `codegen` / `brief` are offline). Confirm in the Output Log:
  `LogNGGBridge: ... HTTP bridge listening on http://localhost:6776`.

## Configuration

Every project-specific setting resolves with precedence
**CLI flag > env var > `BlueprintConvert.json` (project root) > bridge `/project_info` > built-in fallback**.
A new project needs no config at all if it accepts the auto-derived names; pin overrides in an optional
`<project root>/BlueprintConvert.json` (see `vault-template/BlueprintConvert.json.example`).

| Setting | Flag / env / file key | Default |
|---|---|---|
| Project root | `--project-root` / `$BPX_PROJECT_ROOT` | walk up from cwd for `*.uproject` |
| Module name | `--module` / `$BPX_MODULE` / `module_name` | bridge `project_name`, else `.uproject` stem |
| API macro | `--api-macro` / `$BPX_API_MACRO` / `api_macro` | `<MODULE>_API` |
| Source dir | `--source` / `$BPX_SOURCE` / `source_dir` | `<root>/Source/<module>` |
| Mirror token | `--token` / `$BPX_TOKEN` / `mirror_token` | module name (this project pins `GameAnim`) |
| Class group | `--class-group` / `class_group` | mirror token |
| Name prefixes | `--prefixes` / `name_prefixes` | `E_ S_ BPI_ BP_ AC_ ABP_ GM_ PC_ CR_ CHT_ Curve_` |
| Vault dir | `--vault` / `$BPX_VAULT` / `vault_dir` | `<root>/BlueprintConvertInfo` |
| Bridge URL | `--bridge-url` / `$NGG_BRIDGE_URL` | `http://localhost:6776` |
| Auth token | `$NGG_BRIDGE_TOKEN`, else `[UnrealNGGMCP] AuthToken` in `Config/DefaultEngine.ini` | none (auth off) |

Run `py Plugins\UnrealNGGMCP\Tools\bp-extract <mode> --print-config` to see the resolved values.

## Tests

Offline smoke tests (pure functions, no editor / no bridge):

```powershell
py Plugins\UnrealNGGMCP\Tools\bp-extract\test_bp_extract.py
```

## Usage

Run it as a **directory script** (the folder name has a hyphen, so `python -m`
does not apply). From the repo root:

```powershell
# Pilot: one Components-system Blueprint
py Plugins\UnrealNGGMCP\Tools\bp-extract --from-vault --system Components --limit 1

# A specific asset by content path or /Game object path
py Plugins\UnrealNGGMCP\Tools\bp-extract --asset Content/Blueprints/Characters/SandboxCharacter_CMC.uasset
py Plugins\UnrealNGGMCP\Tools\bp-extract --asset /Game/Blueprints/Characters/SandboxCharacter_CMC

# See which assets would be processed, without touching the bridge
py Plugins\UnrealNGGMCP\Tools\bp-extract --from-vault --dry-run

# After extracting, compute the dependency-ordered conversion worklist (no editor needed)
py Plugins\UnrealNGGMCP\Tools\bp-extract order
```

### Options

- `--asset PATH` — a content path (`Content/...uasset`) or `/Game/...` object path. Repeatable.
- `--from-vault` — drive the list from `bp_path` frontmatter under `BlueprintConvertInfo/Assets/**`.
- `--system NAME` / `--status NAME` — filter `--from-vault` by frontmatter fields.
- `--limit N` — process at most N assets (pilot guard).
- `--out DIR` — output directory (default `<vault>/_extracted`).
- `--dry-run` — print resolved targets and exit.
- Plus the shared config flags (`--config`, `--module`, `--token`, `--source`, `--print-config`, …) — see Configuration.

## Output

One JSON per asset at `BlueprintConvertInfo/_extracted/<system>/<AssetName>.json`:

```jsonc
{
  "asset":      { "object_path", "bp_path", "name", "class", "parent_class", "asset_kind", "system" },
  "interfaces": [ ... ],
  "variables":  [ { "name", "type", "type_object", "default", "category", "container", "flags": { ... } } ],
  "components": [ { "name", "class" } ],
  "functions":  [ { "name", "inputs": [{ "name", "type", "type_object" }], "outputs": [...], "graph": { "nodes", "edges" } } ],
  "event_graphs": [ { "name", "nodes", "edges" } ],
  "macros":     [ { "name", "nodes", "edges" } ],
  "source":     { "bridge_url", "routes_used" },
  "partial":    false,
  "warnings":   [ ]
}
```

Type strings follow the bridge's format: `bool`, `int32`, `FString`,
`enum:<path>`, `struct:<path>`, `object:<path>`, `array<T>`, `map<K,V>`, …

### Notes & known limits

- **Data-only kinds** (`Struct`, `Enum`, `Curve`, …) have no event graph or
  member variables; for these the tool records the `/assets/get` dump and marks
  `partial: true`. Field-level struct/enum detail is only as deep as the bridge
  exposes.
- **`parent_class` / `interfaces`** are best-effort: the bridge's read routes do
  not always surface them, in which case a note is added to `warnings`.
- Failures on any single route are recorded in `warnings`; extraction never
  aborts mid-asset.

Graph nodes additionally carry, when the enhanced bridge is loaded: pin `type_object`
(full path of the concrete class/struct/enum behind an object/struct/enum pin) and node
`target` (`{ kind: function|variable|cast|macro, class, name }`) — the data the ordering
step needs to see cross-Blueprint dependencies.

## Ordering the conversion

`py Plugins\UnrealNGGMCP\Tools\bp-extract order` reads the extracted JSON (no editor needed), builds the project
dependency graph, topo-sorts it into tiers, detects cycles, and writes:

- `BlueprintConvertInfo/Conversion Order.md` — the human worklist (dependencies first; per-BP
  functions split engine-only-first).
- `BlueprintConvertInfo/_extracted/_order.json` — the machine form.

Conversion methodology and how to read the order: `BlueprintConvertInfo/Conversion Guide.md` and the
canonical `BlueprintConvertInfo/BP_TO_CPP_PLAYBOOK.md`.

## Codegen + briefs

```powershell
# Per-BP pre-flight brief (blockers, function order, don't-port flags) -> _briefs/<System>/
py Plugins\UnrealNGGMCP\Tools\bp-extract brief SandboxCharacter_CMC
py Plugins\UnrealNGGMCP\Tools\bp-extract brief                      # all assets

# Generate C++ scaffolding (UENUM/USTRUCT/UINTERFACE + empty parents) -> Source/<Module>/
py Plugins\UnrealNGGMCP\Tools\bp-extract codegen --system Data       # tier-0 data types first
py Plugins\UnrealNGGMCP\Tools\bp-extract codegen --kinds Enum,Struct # filter by kind; --dry-run to preview
```

Codegen output is reviewable scaffolding (prefix `E<token>*`/`F<token>*`/`U|I|A<token>*`, where `<token>` is
the configured `mirror_token` — `GameAnim` for this project), not guaranteed zero-touch — exotic field
includes and non-trivial parents are flagged in the manifest.
Requires the enriched bridge (`enum_entries` / `struct_fields` / `parent_class`); re-extract first.

## Files

- `bridge.py` — HTTP client for the read routes (`/health`, `/assets/get`, `/bp/list_variables`, `/bp/read_graph`).
- `paths.py` — `Content/...uasset` ↔ `/Game/...` object-path mapping.
- `extract.py` — kind-aware reshaping into the normalized record.
- `deps.py` — dependency extraction from a record (engine vs project; per-function engine-only flag).
- `order.py` — dependency graph, topo-sort into tiers, cycle detection, worklist output.
- `typemap.py` — BP type / struct-field → C++ type mapping, with project-mirror prefixing.
- `codegen.py` — emit UENUM/USTRUCT/UINTERFACE + empty typed parents.
- `brief.py` — per-BP conversion briefs (pre-flight matrix).
- `config.py` — per-project settings resolution (`Config`, `find_uproject`, `resolve`, `configure`) + shared CLI flags.
- `__main__.py` — CLI (`extract` default mode; `order` / `codegen` / `brief` subcommands).
- `test_bp_extract.py` — offline stdlib `unittest` smoke (de-hardcoding, paths, deps, order, config precedence).
- `vault-template/` — generic methodology + dashboards + templates to seed a new project's `BlueprintConvertInfo/` vault.
