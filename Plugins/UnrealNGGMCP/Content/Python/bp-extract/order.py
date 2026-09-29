# Copyright 2025-2026 NGG. All Rights Reserved.
"""Compute a dependency-ordered conversion worklist from extracted records.

Reads ``_extracted/**/*.json`` (skipping ``_*`` files), builds the project
dependency graph restricted to in-inventory assets, topo-sorts it into tiers,
detects cycles (Tarjan SCC), and writes:
  - ``BlueprintConvertInfo/Conversion Order.md``  — human worklist
  - ``BlueprintConvertInfo/_extracted/_order.json`` — machine form

Read-only over JSON; the editor/bridge is not involved.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import config
import deps as depmod

# Tie-break order within a dependency tier (data types first).
KIND_RANK = {
    "Enum": 0, "Struct": 1, "Curve": 2, "Interface": 3,
    "DataAsset": 4, "Component": 5, "AnimBlueprint": 6, "Blueprint": 7,
}


def _load_records(out_dir: Path) -> list[dict]:
    records = []
    for f in sorted(out_dir.rglob("*.json")):
        if f.name.startswith("_"):
            continue
        try:
            records.append(json.loads(f.read_text(encoding="utf-8")))
        except (OSError, ValueError):
            continue
    return records


def _tarjan_scc(nodes: list[str], edges: dict[str, set]) -> list[list[str]]:
    """Strongly-connected components, emitted dependencies-first (leaf SCCs first)."""
    index: dict[str, int] = {}
    low: dict[str, int] = {}
    on_stack: set[str] = set()
    stack: list[str] = []
    sccs: list[list[str]] = []
    counter = [0]
    sys.setrecursionlimit(max(10000, len(nodes) * 4 + 1000))

    def strongconnect(v: str) -> None:
        index[v] = low[v] = counter[0]
        counter[0] += 1
        stack.append(v)
        on_stack.add(v)
        for w in edges.get(v, ()):
            if w not in index:
                strongconnect(w)
                low[v] = min(low[v], low[w])
            elif w in on_stack:
                low[v] = min(low[v], index[w])
        if low[v] == index[v]:
            comp = []
            while True:
                w = stack.pop()
                on_stack.discard(w)
                comp.append(w)
                if w == v:
                    break
            sccs.append(comp)

    for v in nodes:
        if v not in index:
            strongconnect(v)
    return sccs


def build_order(records: list[dict]) -> dict:
    inventory = {r["asset"]["object_path"]: r for r in records if r.get("asset", {}).get("object_path")}
    by_name: dict[str, str] = {}
    for path in inventory:
        by_name.setdefault(path.rsplit("/", 1)[-1], path)

    def resolve(pkg: str) -> str | None:
        if pkg in inventory:
            return pkg
        return by_name.get(pkg.rsplit("/", 1)[-1])

    edges: dict[str, set] = {p: set() for p in inventory}
    external: dict[str, set] = {p: set() for p in inventory}
    dep_cache: dict[str, dict] = {}

    for path, rec in inventory.items():
        d = depmod.asset_dependencies(rec)
        dep_cache[path] = d
        for pkg in d["project"]:
            target = resolve(pkg)
            if target and target != path:
                edges[path].add(target)
            elif not target:
                external[path].add(pkg)

    nodes = list(inventory)
    sccs = _tarjan_scc(nodes, edges)
    comp_of = {n: i for i, comp in enumerate(sccs) for n in comp}

    # Condensation dependencies + tier (longest path) per component.
    comp_deps: dict[int, set] = {i: set() for i in range(len(sccs))}
    for n in nodes:
        ci = comp_of[n]
        for dnode in edges[n]:
            cj = comp_of.get(dnode)
            if cj is not None and cj != ci:
                comp_deps[ci].add(cj)

    tier: dict[int, int] = {}

    def comp_tier(ci: int) -> int:
        if ci in tier:
            return tier[ci]
        tier[ci] = 0  # guard (condensation is acyclic, so this is only a placeholder)
        tier[ci] = (1 + max((comp_tier(c) for c in comp_deps[ci]), default=-1))
        return tier[ci]

    for ci in range(len(sccs)):
        comp_tier(ci)

    def sort_key(path: str):
        rec = inventory[path]
        kind = rec["asset"].get("asset_kind") or ""
        return (tier[comp_of[path]], KIND_RANK.get(kind, 99), rec["asset"].get("name") or path)

    ordered = sorted(inventory, key=sort_key)
    cycles = [sorted(comp) for comp in sccs if len(comp) > 1]

    return {
        "inventory": inventory,
        "ordered": ordered,
        "tier": {p: tier[comp_of[p]] for p in inventory},
        "edges": {p: sorted(e) for p, e in edges.items()},
        "external": {p: sorted(e) for p, e in external.items() if e},
        "cycles": cycles,
        "dep_cache": dep_cache,
    }


def _function_split(dep: dict) -> tuple[list[str], list[tuple[str, list[str]]]]:
    engine_only, project_dependent = [], []
    for name, g in dep.get("by_graph", {}).items():
        if g.get("kind") != "function":
            continue
        if g.get("engine_only"):
            engine_only.append(name)
        else:
            project_dependent.append((name, g.get("project", [])))
    return sorted(engine_only), sorted(project_dependent)


def _name(path: str) -> str:
    return path.rsplit("/", 1)[-1]


def render_markdown(result: dict) -> str:
    inv = result["inventory"]
    ordered = result["ordered"]
    tier = result["tier"]
    cycles = result["cycles"]
    external = result["external"]

    lines = [
        "---",
        "type: generated",
        "tags: [conversion-order, generated]",
        "---",
        "",
        "# Conversion Order (generated)",
        "",
        "> Generated by `py Plugins\\UnrealNGGMCP\\Tools\\bp-extract order` from `_extracted/`. Do not edit by hand —",
        "> re-run after re-extracting. Methodology: [[Conversion Guide]].",
        "",
        f"- Assets: **{len(inv)}** | Tiers: **{(max(tier.values()) + 1) if tier else 0}** | "
        f"Cycles: **{len(cycles)}** | Assets with unresolved refs: **{len(external)}**",
        "",
    ]

    if cycles:
        lines += ["## Cycles", "",
                  "Each cluster has circular dependencies — break the seam with an interface, a "
                  "forward declaration, or a soft reference, or convert the cluster together "
                  "(see [[Conversion Guide]]).", ""]
        for i, comp in enumerate(cycles, 1):
            lines.append(f"- **Cluster {i}:** " + ", ".join(f"[[{_name(p)}]]" for p in comp))
        lines.append("")

    lines += ["## Worklist (dependencies first)", ""]
    current_tier = None
    for path in ordered:
        t = tier[path]
        if t != current_tier:
            current_tier = t
            label = "no project dependencies" if t == 0 else f"depends on tier ≤ {t - 1}"
            lines += ["", f"### Tier {t} — {label}", ""]
        rec = inv[path]
        kind = rec["asset"].get("asset_kind") or "?"
        system = rec["asset"].get("system") or "?"
        ndeps = len(result["edges"].get(path, []))
        suffix = f" — {ndeps} project dep(s)" if ndeps else ""
        lines.append(f"- [ ] [[{_name(path)}]] — {kind} ({system}){suffix}")
        eo, pd = _function_split(result["dep_cache"].get(path, {}))
        if eo:
            lines.append(f"    - engine-only functions (port first): {', '.join(eo)}")
        for fname, needs in pd:
            need_links = ", ".join(f"[[{_name(dep_pkg)}]]" for dep_pkg in needs) or "project types"
            lines.append(f"    - `{fname}` needs: {need_links}")

    if external:
        lines += ["", "## Unresolved / external project references",
                  "", "_Project (`/Game`) refs not found in the inventory — verify manually._", ""]
        for path in sorted(external):
            refs = ", ".join(external[path])
            lines.append(f"- [[{_name(path)}]] → {refs}")

    lines.append("")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8")
        except (AttributeError, ValueError):
            pass

    parser = argparse.ArgumentParser(
        prog="bp-extract order",
        description="Compute a dependency-ordered BP->C++ conversion worklist from extracted JSON.",
    )
    parser.add_argument("--out", type=Path, default=None,
                        help="Extracted-JSON directory (default: <vault>/_extracted).")
    parser.add_argument("--md", type=Path, default=None,
                        help="Markdown worklist output path (default: <vault>/Conversion Order.md).")
    config.add_common_args(parser)
    args = parser.parse_args(argv)

    cfg = config.resolve(args)
    config.configure(cfg)
    if config.maybe_print_config(cfg, args):
        return 0
    out_dir = args.out or cfg.out_dir
    md_path = args.md or (cfg.vault_dir / "Conversion Order.md")

    records = _load_records(out_dir)
    if not records:
        print(f"No extracted records under {out_dir}. Run extraction first.", file=sys.stderr)
        return 1

    result = build_order(records)

    md_path.write_text(render_markdown(result), encoding="utf-8")

    machine = {
        "assets": {
            path: {
                "tier": result["tier"][path],
                "kind": result["inventory"][path]["asset"].get("asset_kind"),
                "system": result["inventory"][path]["asset"].get("system"),
                "deps": result["edges"].get(path, []),
                "external": result["external"].get(path, []),
                "dependencies_detail": result["dep_cache"].get(path, {}),
            }
            for path in result["ordered"]
        },
        "order": [_name(p) for p in result["ordered"]],
        "cycles": [[_name(p) for p in c] for c in result["cycles"]],
    }
    (out_dir / "_order.json").write_text(
        json.dumps(machine, indent=2, ensure_ascii=False), encoding="utf-8")

    print(f"Ordered {len(records)} asset(s) into {max(result['tier'].values()) + 1} tier(s); "
          f"{len(result['cycles'])} cycle cluster(s).")
    print(f"Wrote {md_path}")
    print(f"Wrote {out_dir / '_order.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
