# Copyright 2025-2026 NGG. All Rights Reserved.
"""Project configuration resolution for bp-extract.

All project-specific settings (module name, mirror token, source/vault paths) resolve here so the
tool is reusable across UE projects. Per-setting precedence:

    CLI flag  >  env var  >  BlueprintConvert.json (project root)  >  bridge /project_info  >  built-in fallback

The bridge is consulted **lazily** and only when a ``Bridge`` is passed AND a value is still
unresolved — the offline modes (order / codegen / brief) pass ``bridge=None`` and fall back to the
``.uproject`` stem, so they never require a live editor.

``configure(cfg)`` pushes the resolved values into the module-global constants that the pure
functions in ``typemap`` / ``codegen`` read, so those functions stay untouched.

Stdlib only.
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass
from pathlib import Path

# Default leading asset-name prefixes stripped when forming a C++ identifier (overridable per project).
DEFAULT_NAME_PREFIXES = ("E_", "S_", "BPI_", "BP_", "AC_", "ABP_", "GM_", "PC_", "CR_", "CHT_", "Curve_")
CONFIG_FILENAME = "BlueprintConvert.json"


@dataclass(frozen=True)
class Config:
    project_root: Path
    module_name: str
    api_macro: str
    source_dir: Path
    mirror_token: str
    name_prefixes: tuple[str, ...]
    class_group: str
    vault_dir: Path
    out_dir: Path
    briefs_dir: Path
    ini_path: Path
    bridge_url: str | None


def find_uproject(start: Path | None = None) -> Path | None:
    """Walk up from ``start`` (default cwd) for the nearest ``*.uproject`` (up to ~12 levels)."""
    here = (start or Path.cwd()).resolve()
    for d in [here, *here.parents][:12]:
        hits = sorted(d.glob("*.uproject"))
        if hits:
            return hits[0]
    return None


def load_config_file(project_root: Path, explicit: str | None = None) -> dict:
    """Read the optional ``BlueprintConvert.json`` (``{}`` if absent / invalid)."""
    path = Path(explicit).resolve() if explicit else project_root / CONFIG_FILENAME
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def _arg(args, name: str):
    return getattr(args, name, None)


def _first(*vals):
    for v in vals:
        if v not in (None, ""):
            return v
    return None


def _discover_project_root(args) -> Path:
    pr = _arg(args, "project_root") or os.environ.get("BPX_PROJECT_ROOT")
    if pr:
        return Path(pr).resolve()
    cfg = _arg(args, "config")
    if cfg:
        return Path(cfg).resolve().parent
    up = find_uproject(Path.cwd()) or find_uproject(Path(__file__).parent)
    if up:
        return up.parent
    return Path.cwd().resolve()


def resolve(args, *, bridge=None) -> Config:
    project_root = _discover_project_root(args)
    filecfg = load_config_file(project_root, _arg(args, "config"))

    # Optional, lazy bridge auto-derivation (only when a Bridge is supplied and reachable).
    info: dict = {}
    if bridge is not None:
        try:
            info = bridge.project_info() or {}
        except Exception:
            info = {}

    def stem_module() -> str:
        up = find_uproject(project_root) or find_uproject(Path.cwd())
        return up.stem if up else project_root.name

    bridge_module = info.get("project_name") or None
    if not bridge_module and info.get("editor_target"):
        et = str(info["editor_target"])
        bridge_module = et[:-6] if et.endswith("Editor") else et

    module_name = _first(
        _arg(args, "module"), os.environ.get("BPX_MODULE"),
        filecfg.get("module_name"), bridge_module, stem_module(),
    ) or "Game"

    api_macro = _first(
        _arg(args, "api_macro"), os.environ.get("BPX_API_MACRO"), filecfg.get("api_macro"),
    ) or f"{module_name.upper()}_API"

    src = _first(_arg(args, "source"), os.environ.get("BPX_SOURCE"), filecfg.get("source_dir"))
    source_dir = Path(src).resolve() if src else project_root / "Source" / module_name

    mirror_token = _first(
        _arg(args, "token"), os.environ.get("BPX_TOKEN"), filecfg.get("mirror_token"),
    ) or module_name

    class_group = _first(_arg(args, "class_group"), filecfg.get("class_group")) or mirror_token

    prefixes_arg = _arg(args, "prefixes")
    if prefixes_arg:
        name_prefixes = tuple(p.strip() for p in prefixes_arg.split(",") if p.strip())
    elif isinstance(filecfg.get("name_prefixes"), list):
        name_prefixes = tuple(str(p) for p in filecfg["name_prefixes"])
    else:
        name_prefixes = DEFAULT_NAME_PREFIXES

    vlt = _first(_arg(args, "vault"), os.environ.get("BPX_VAULT"), filecfg.get("vault_dir"))
    vault_dir = Path(vlt).resolve() if vlt else project_root / "BlueprintConvertInfo"

    out = _arg(args, "out")
    out_dir = Path(out).resolve() if out else vault_dir / "_extracted"
    briefs = _arg(args, "briefs")
    briefs_dir = Path(briefs).resolve() if briefs else vault_dir / "_briefs"
    ini = _arg(args, "ini")
    ini_path = Path(ini).resolve() if ini else project_root / "Config" / "DefaultEngine.ini"
    bridge_url = _first(_arg(args, "bridge_url"), os.environ.get("NGG_BRIDGE_URL"))

    return Config(
        project_root=project_root, module_name=module_name, api_macro=api_macro,
        source_dir=source_dir, mirror_token=mirror_token, name_prefixes=name_prefixes,
        class_group=class_group, vault_dir=vault_dir, out_dir=out_dir, briefs_dir=briefs_dir,
        ini_path=ini_path, bridge_url=bridge_url,
    )


def configure(cfg: Config) -> None:
    """Push resolved settings into the module globals the pure functions read."""
    import typemap
    import codegen
    typemap.TOKEN = cfg.mirror_token
    typemap._NAME_PREFIXES = tuple(cfg.name_prefixes)
    codegen.API = cfg.api_macro
    codegen.DEFAULT_SOURCE = cfg.source_dir
    codegen.DEFAULT_OUT = cfg.out_dir
    codegen.PARENT_MAP = codegen.build_parent_map(cfg.class_group)


def add_common_args(parser) -> None:
    """Register the project-config flags shared by every entry point.

    Only the new config flags live here; the per-entry path flags (--out / --source / --vault /
    --briefs / --bridge-url / --md) stay on their own parsers. resolve() reads all of them by name.
    """
    g = parser.add_argument_group("project config")
    g.add_argument("--config", default=None,
                   help=f"Path to {CONFIG_FILENAME} (default: <project root>/{CONFIG_FILENAME}).")
    g.add_argument("--project-root", dest="project_root", default=None,
                   help="Override project root (default: walk up from cwd for *.uproject).")
    g.add_argument("--module", default=None, help="C++ module name (default: project name).")
    g.add_argument("--api-macro", dest="api_macro", default=None,
                   help="UPROPERTY API macro (default: <MODULE>_API).")
    g.add_argument("--token", default=None, help="Mirror type token, e.g. GameAnim (default: module name).")
    g.add_argument("--class-group", dest="class_group", default=None,
                   help="UCLASS ClassGroup for components (default: token).")
    g.add_argument("--prefixes", default=None, help="Comma list of asset-name prefixes to strip.")
    g.add_argument("--print-config", dest="print_config", action="store_true",
                   help="Print the resolved configuration and exit.")


def maybe_print_config(cfg: Config, args) -> bool:
    """If --print-config was passed, print the resolved config and return True (caller should exit)."""
    if not _arg(args, "print_config"):
        return False
    print("Resolved bp-extract config:")
    for field in cfg.__dataclass_fields__:  # type: ignore[attr-defined]
        print(f"  {field:14} = {getattr(cfg, field)}")
    return True
