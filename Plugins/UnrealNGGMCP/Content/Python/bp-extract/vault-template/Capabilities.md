---
type: doc
tags: [capabilities, conversion]
---

# Capabilities — what can be done & how to ask

A menu of the BP→C++ conversion system: what each step does, the phrase that triggers it, and the
limits. The *method* is [[BP_TO_CPP_PLAYBOOK]]; the short loop is [[Conversion Guide]]; the live
worklist is [[Conversion Order]].

## What you can ask for

| Ask (example phrase) | What happens | Tool |
|---|---|---|
| "extract `<System>`" / "extract everything" | Pull each asset's vars, functions, components, parent, graph topology → JSON in `_extracted/` | `bp-extract --from-vault` |
| "compute the conversion order" | Dependency graph → tiers + cycles + per-function engine-only split → [[Conversion Order]] | `bp-extract order` |
| "brief `<Name>`" / "brief the Components" | Per-BP pre-flight matrix: blockers, function order, don't-port flags → `_briefs/` | `bp-extract brief` |
| "codegen the Data types" / "codegen `<System>`" | Generate `UENUM`/`USTRUCT`/`UINTERFACE` + empty typed parents → `Source/GameAnimationSample/<System>/` | `bp-extract codegen` |
| "convert `<Name>` to C++" | Full per-asset cycle (brief → codegen → duplicate → reparent → migrate → build → verify → update tracker) | playbook |
| "convert tier 0" / "convert all enums" | Same, batched over a tier/kind in dependency order | playbook |
| "continue the conversion" | Take the next unconverted asset from [[Conversion Order]] | playbook |

### Conversion modifiers

- **"scaffold only"** — create the C++ type/parent + make the `*_CPP` child, but don't port logic yet.
- **"full type pipeline"** — generate *all* types (every system's enums/structs/interfaces) together and build to green, before any logic migration (the playbook's "type pipeline first").
- **"create new" vs "duplicate"** — how the `*_CPP` child is made (per-asset, playbook Override 3):
  *duplicate* (default; keeps the original's vars/functions/graph **and SCS components + defaults**) vs
  *create new* (empty child of the C++ class — only for trivial types with nothing to preserve).
- **"don't touch the original"** is always implied — conversion works on a `BP_Foo_CPP` copy/child; originals stay intact.

## Prerequisites

- **UE editor open** with the UnrealNGGMCP plugin (bridge `:6776` healthy) for anything that reads the
  editor (extract) or mutates assets (codegen-build, duplicate, reparent). `order` / `brief` are
  offline (they read `_extracted/`). If the bridge is down, you'll be asked to launch the editor.
- Convert **bottom-up**: enums/structs first, gameplay BPs last. Naming an asset with open dependencies
  triggers a warning + a suggestion to do its dependencies first.

## What's already done

- Bridge introspection v2 (node `target`, pin `type_object`, `parent_class`, struct/enum fields, `/assets/duplicate`).
- `Content/Python/bp-extract/` pipeline (extract / order / brief / codegen) — committed, offline-tested.
- 46 assets extracted (Data, Characters, Components); [[Conversion Order]] generated (6 tiers, 1 cycle).
- 10 C++ enum mirrors generated under `Source/GameAnimationSample/Data/` and compiling.
- Methodology canonized ([[BP_TO_CPP_PLAYBOOK]], [[Conversion Guide]]).

## Limits (be realistic)

- **Codegen is scaffolding, not zero-touch.** Object-pointer and cross-system fields need forward-decls
  or `Build.cs` module deps; the full type pipeline must be generated together to compile.
- **Graph logic is not auto-transpiled.** Function bodies are migrated by hand/Claude per the brief —
  the tooling automates inventory, ordering, scaffolding, and the duplicate+reparent mechanics, not the
  node-by-node C++ logic.
- **Editor restarts dominate wall-clock** (~45–100 s). Batch C++ edits, build once per batch.
- **Some functions shouldn't be ported** (debug-only, plugin-dep, no C++ caller) — the brief flags these.

## Links
- Method: [[BP_TO_CPP_PLAYBOOK]] · Quick start: [[Conversion Guide]] · Order: [[Conversion Order]]
- Tracker: [[Conversion Tracker]] · Tool reference: `Content/Python/bp-extract/README.md`
