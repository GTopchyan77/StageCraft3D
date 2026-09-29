# Copyright 2025-2026 NGG. All Rights Reserved.
"""CLI entry point for the Blueprint introspection extractor.

Run it as a directory script (the folder name contains a hyphen, so ``-m`` does
not apply):

    py Plugins\\UnrealNGGMCP\\Tools\\bp-extract --from-vault --system Components --limit 1
    py Plugins\\UnrealNGGMCP\\Tools\\bp-extract --asset Content/Blueprints/Characters/SandboxCharacter_CMC.uasset
    py Plugins\\UnrealNGGMCP\\Tools\\bp-extract --from-vault --dry-run

Read-only: only GET routes are issued, so no asset is ever modified or saved.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from bridge import Bridge, BridgeError
import config
import extract
import paths


def _parse_frontmatter(md_path: Path) -> dict:
    """Return the YAML-ish frontmatter of a vault note as a flat dict of strings."""
    try:
        text = md_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return {}
    lines = text.splitlines()
    if not lines or lines[0].strip() != "---":
        return {}
    data: dict[str, str] = {}
    for line in lines[1:]:
        if line.strip() == "---":
            break
        if ":" in line and not line.lstrip().startswith("#"):
            key, _, value = line.partition(":")
            data[key.strip()] = value.strip().strip("'\"")
    return data


def _targets_from_vault(vault: Path, system: str | None, status: str | None) -> list[dict]:
    targets: list[dict] = []
    assets_dir = vault / "Assets"
    for md in sorted(assets_dir.rglob("*.md")):
        fm = _parse_frontmatter(md)
        bp_path = fm.get("bp_path")
        if not bp_path:
            continue
        if system and fm.get("system", "").lower() != system.lower():
            continue
        if status and fm.get("status", "").lower() != status.lower():
            continue
        targets.append({
            "bp_path": bp_path,
            "object_path": paths.to_object_path(bp_path),
            "asset_kind": fm.get("asset_kind"),
            "system": fm.get("system"),
        })
    return targets


def _targets_from_args(values: list[str]) -> list[dict]:
    targets = []
    for v in values:
        is_disk = "content/" in v.lower().replace("\\", "/")
        targets.append({
            "bp_path": v if is_disk else None,
            "object_path": paths.to_object_path(v),
            "asset_kind": None,
            "system": None,
        })
    return targets


def main(argv: list[str] | None = None) -> int:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8")
        except (AttributeError, ValueError):
            pass

    raw = sys.argv[1:] if argv is None else argv
    if raw and raw[0] == "order":
        import order
        return order.main(raw[1:])
    if raw and raw[0] == "codegen":
        import codegen
        return codegen.main(raw[1:])
    if raw and raw[0] == "brief":
        import brief
        return brief.main(raw[1:])

    parser = argparse.ArgumentParser(
        prog="bp-extract",
        description="Extract Blueprint variables/functions/types to JSON via the UnrealNGGMCP bridge.",
    )
    parser.add_argument("--asset", action="append", default=[], metavar="PATH",
                        help="A content path or /Game object path. Repeatable.")
    parser.add_argument("--from-vault", action="store_true",
                        help="Drive the asset list from bp_path frontmatter in the Obsidian vault.")
    parser.add_argument("--vault", type=Path, default=None,
                        help="Vault directory (default: <project root>/BlueprintConvertInfo).")
    parser.add_argument("--system", default=None, help="Filter --from-vault by 'system' frontmatter.")
    parser.add_argument("--status", default=None, help="Filter --from-vault by 'status' frontmatter.")
    parser.add_argument("--limit", type=int, default=None, help="Process at most N assets.")
    parser.add_argument("--out", type=Path, default=None,
                        help="Output directory (default: <vault>/_extracted).")
    parser.add_argument("--bridge-url", dest="bridge_url", default=None, help="Override $NGG_BRIDGE_URL.")
    parser.add_argument("--dry-run", action="store_true", help="List resolved targets and exit.")
    config.add_common_args(parser)
    args = parser.parse_args(argv)

    cfg = config.resolve(args)
    config.configure(cfg)
    if config.maybe_print_config(cfg, args):
        return 0
    vault = args.vault or cfg.vault_dir
    out_dir = args.out or cfg.out_dir

    if args.asset:
        targets = _targets_from_args(args.asset)
    elif args.from_vault:
        targets = _targets_from_vault(vault, args.system, args.status)
    else:
        parser.error("provide --asset PATH ... or --from-vault")

    if args.limit is not None:
        targets = targets[: args.limit]

    if not targets:
        print("No matching assets.", file=sys.stderr)
        return 1

    if args.dry_run:
        print(f"{len(targets)} target(s):")
        for t in targets:
            print(f"  {t['object_path']}  [{t.get('system') or '-'}/{t.get('asset_kind') or '-'}]")
        return 0

    bridge = Bridge(base_url=args.bridge_url, ini_path=cfg.ini_path)

    try:
        health = bridge.health()
    except BridgeError as err:
        print(f"Bridge health check failed: {err}", file=sys.stderr)
        return 2
    print(f"Bridge OK: {health.get('project', '?')} on port {health.get('port', '?')} "
          f"({bridge.base_url})")

    written = 0
    for t in targets:
        try:
            record = extract.extract_asset(
                bridge,
                t["object_path"],
                bp_path=t.get("bp_path"),
                asset_kind=t.get("asset_kind"),
                system=t.get("system"),
            )
        except BridgeError as err:
            print(f"  SKIP {t['object_path']}: {err}", file=sys.stderr)
            continue

        system_dir = out_dir / (record["asset"].get("system") or "Unknown")
        system_dir.mkdir(parents=True, exist_ok=True)
        out_file = system_dir / f"{record['asset']['name']}.json"
        out_file.write_text(json.dumps(record, indent=2, ensure_ascii=False), encoding="utf-8")
        written += 1

        flag = " (partial)" if record["partial"] else ""
        warn = f" | {len(record['warnings'])} warning(s)" if record["warnings"] else ""
        print(f"  OK  {record['asset']['name']}: "
              f"{len(record['variables'])} vars, "
              f"{len(record['functions'])} funcs, "
              f"{len(record['event_graphs'])} event graph(s), "
              f"{len(record['components'])} component(s){flag}{warn}")

    print(f"\nWrote {written}/{len(targets)} record(s) to {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
