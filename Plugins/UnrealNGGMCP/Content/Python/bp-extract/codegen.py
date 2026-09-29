# Copyright 2025-2026 NGG. All Rights Reserved.
"""Generate C++ scaffolding (UENUM / USTRUCT / UINTERFACE + empty typed parents)
from extracted records, into Source/GameAnimationSample/<System>/.

This is the playbook's Phase 2 (type pipeline) + Phase 3 (empty parents) automated:
the bulkiest mechanical work, collapsed into reviewable text emitted in one pass.
Output is scaffolding to review + build — not guaranteed zero-touch (engine includes
for exotic field types and non-trivial parents are flagged in the manifest).

Read-only over JSON; writes .h/.cpp files. No editor involved.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

import typemap
import config

# Module-level defaults. config.configure() overwrites API / DEFAULT_SOURCE / DEFAULT_OUT / PARENT_MAP
# from the resolved per-project config before any emit runs, so these are only fallbacks.
API = "GAME_API"
DEFAULT_OUT: Path | None = None
DEFAULT_SOURCE: Path | None = None

GEN_KINDS = ("Enum", "Struct", "Interface")


def build_parent_map(class_group: str) -> dict:
    """parent_class path -> (C++ base, include, is_actor, extra UCLASS specifiers).

    Only the component entries carry a project-specific ClassGroup; the rest are engine-generic.
    """
    comp = f"ClassGroup=({class_group}), meta=(BlueprintSpawnableComponent)"
    return {
        "/Script/Engine.ActorComponent": ("UActorComponent", "Components/ActorComponent.h", False, comp),
        "/Script/Engine.SceneComponent": ("USceneComponent", "Components/SceneComponent.h", False, comp),
        "/Script/Engine.Actor": ("AActor", "GameFramework/Actor.h", True, ""),
        "/Script/Engine.Pawn": ("APawn", "GameFramework/Pawn.h", True, ""),
        "/Script/Engine.Character": ("ACharacter", "GameFramework/Character.h", True, ""),
        "/Script/Engine.GameModeBase": ("AGameModeBase", "GameFramework/GameModeBase.h", True, ""),
        "/Script/Engine.GameMode": ("AGameModeBase", "GameFramework/GameModeBase.h", True, ""),
        "/Script/Engine.PlayerController": ("APlayerController", "GameFramework/PlayerController.h", True, ""),
    }


# Overwritten by config.configure(); "Game" is a neutral fallback for un-configured use.
PARENT_MAP = build_parent_map("Game")

_FIELD_GUID = re.compile(r"_\d+_[0-9A-Fa-f]{16,}$")


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


def build_index(records: list[dict]) -> dict:
    """path -> {kind, system, mirror, header_rel} for every generatable type."""
    index: dict[str, dict] = {}
    for r in records:
        a = r.get("asset", {})
        path = a.get("object_path")
        kind = a.get("asset_kind")
        system = a.get("system") or "Misc"
        if not path:
            continue
        mirror = None
        if kind == "Enum":
            mirror = typemap.enum_mirror(path)
        elif kind == "Struct":
            mirror = typemap.struct_mirror(path)
        elif kind == "Interface":
            mirror = typemap.interface_mirror(path)[0]  # U-class name
        index[path] = {"kind": kind, "system": system, "mirror": mirror,
                       "header_rel": f"{system}/{mirror}.h" if mirror else None}
    return index


def _field_name(field: dict) -> str:
    # raw_name is the FProperty identifier ("StartTime_3_GUID"); the authored `name`
    # is a display label that may contain spaces. Prefer raw_name, strip the GUID suffix.
    raw = field.get("raw_name") or field.get("name") or "Field"
    n = _FIELD_GUID.sub("", raw)
    n = re.sub(r"[^0-9A-Za-z_]", "", n)
    return n or "Field"


def _includes_for(refs: list[str], index: dict, self_path: str) -> list[str]:
    out = []
    for ref in refs:
        pkg = typemap.package_path(ref)
        if pkg == self_path:
            continue
        info = index.get(pkg)
        if info and info.get("header_rel"):
            out.append(info["header_rel"])
    return sorted(set(out))


# --- emitters: return (relative_path, content) ----------------------------

def emit_enum(rec: dict) -> tuple[str, str]:
    a = rec["asset"]
    mirror = typemap.enum_mirror(a["object_path"])
    system = a.get("system") or "Misc"
    entries = rec.get("enum_entries") or []

    lines, used = [], {}
    has_zero = any(int(e.get("value", -1)) == 0 for e in entries)
    if not has_zero:
        lines.append("\tNone = 0 UMETA(Hidden),")
    for e in entries:
        ident = typemap.clean_name(e.get("display") or e.get("name") or "Entry")
        # de-dup identifiers
        if ident in used:
            used[ident] += 1
            ident = f"{ident}_{used[ident]}"
        else:
            used[ident] = 0
        disp = (e.get("display") or ident).replace('"', "'")
        lines.append(f'\t{ident} = {int(e.get("value", 0))} UMETA(DisplayName="{disp}"),')

    body = "\n".join(lines)
    content = f"""#pragma once

#include "CoreMinimal.h"
#include "{mirror}.generated.h"

UENUM(BlueprintType)
enum class {mirror} : uint8
{{
{body}
}};
"""
    return f"{system}/{mirror}.h", content


def emit_struct(rec: dict, index: dict) -> tuple[str, str]:
    a = rec["asset"]
    mirror = typemap.struct_mirror(a["object_path"])
    system = a.get("system") or "Misc"
    self_path = typemap.package_path(a["object_path"])
    fields = rec.get("struct_fields") or []

    refs = [f["type_object"] for f in fields if f.get("type_object")]
    includes = _includes_for(refs, index, self_path)
    inc_block = "".join(f'#include "{h}"\n' for h in includes)

    props, field_types = [], []
    for f in fields:
        cpp = typemap.map_struct_field(f, index)
        field_types.append(cpp)
        name = _field_name(f)
        props.append("\tUPROPERTY(EditAnywhere, BlueprintReadWrite)")
        props.append(f"\t{cpp} {name};\n")
    body = "\n".join(props) if props else "\t// (no fields)"

    # Forward-declare engine object/class pointer types (CoreMinimal won't have them).
    included_names = {h.rsplit("/", 1)[-1][:-2] for h in includes}
    fwd = _forward_decls(field_types, included_names)
    fwd_block = ("\n" + "\n".join(f"class {c};" for c in fwd) + "\n") if fwd else ""

    content = f"""#pragma once

#include "CoreMinimal.h"
{inc_block}#include "{mirror}.generated.h"
{fwd_block}
USTRUCT(BlueprintType)
struct {API} {mirror}
{{
\tGENERATED_BODY()

{body}
}};
"""
    return f"{system}/{mirror}.h", content


_CLASS_TOKEN = re.compile(r"\b([UA][A-Z]\w+)\b")


def _forward_decls(type_strings: list[str], included_names: set[str]) -> list[str]:
    """Engine UObject/AActor class names used as pointers — forward-declare them.

    Project mirrors are pulled in via includes (skip those); value types (F*, E*,
    scalars, containers) don't match the U/A class token.
    """
    found = set()
    for t in type_strings:
        for name in _CLASS_TOKEN.findall(t):
            if name in included_names:
                continue
            found.add(name)
    return sorted(found)


def _sig(fn: dict, index: dict) -> str:
    inputs = fn.get("inputs") or []
    outputs = fn.get("outputs") or []
    params = [f"{typemap.map_var_type(i.get('type', ''), i.get('type_object'), index)} {typemap.clean_name(i.get('name') or 'In')}"
              for i in inputs]
    if len(outputs) == 1:
        ret = typemap.map_var_type(outputs[0].get("type", ""), outputs[0].get("type_object"), index)
    else:
        ret = "void"
        for o in outputs:
            t = typemap.map_var_type(o.get("type", ""), o.get("type_object"), index)
            params.append(f"{t}& {typemap.clean_name(o.get('name') or 'Out')}")
    return f"{ret} {typemap.clean_name(fn.get('name') or 'Func')}({', '.join(params)})"


def emit_interface(rec: dict, index: dict) -> tuple[str, str]:
    a = rec["asset"]
    u_name, i_name = typemap.interface_mirror(a["object_path"])
    system = a.get("system") or "Misc"

    methods = []
    for fn in rec.get("functions") or []:
        methods.append("\tUFUNCTION(BlueprintImplementableEvent, BlueprintCallable)")
        methods.append(f"\t{_sig(fn, index)};\n")
    body = "\n".join(methods) if methods else "\t// (no functions)"

    content = f"""#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "{u_name}.generated.h"

UINTERFACE(Blueprintable, MinimalAPI)
class {u_name} : public UInterface
{{
\tGENERATED_BODY()
}};

class {API} {i_name}
{{
\tGENERATED_BODY()
public:
{body}
}};
"""
    return f"{system}/{u_name}.h", content


def emit_parent(rec: dict, index: dict) -> tuple[str, str, str] | None:
    a = rec["asset"]
    parent = a.get("parent_class")
    # parent_class is a full native path (e.g. /Script/Engine.ActorComponent) where the
    # class name is after the dot, so look up the whole path (not the package path).
    mapping = PARENT_MAP.get(parent or "")
    if not mapping:
        return None  # exotic/plugin/project parent — flagged in manifest, hand-write
    base, base_inc, actor, specifiers = mapping
    mirror = typemap.class_mirror(a["object_path"], actor)
    system = a.get("system") or "Misc"
    uclass = (f"UCLASS(Blueprintable, BlueprintType, {specifiers})" if specifiers
              else "UCLASS(Blueprintable, BlueprintType)")

    header = f"""#pragma once

#include "CoreMinimal.h"
#include "{base_inc}"
#include "{mirror}.generated.h"

// Empty typed parent (playbook Phase 3): ZERO UPROPERTY shadowing BP variables.
{uclass}
class {API} {mirror} : public {base}
{{
\tGENERATED_BODY()
public:
\t{mirror}();
}};
"""
    cpp = f"""#include "{system}/{mirror}.h"

{mirror}::{mirror}()
{{
}}
"""
    return f"{system}/{mirror}.h", header, cpp


def main(argv: list[str] | None = None) -> int:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8")
        except (AttributeError, ValueError):
            pass

    parser = argparse.ArgumentParser(
        prog="bp-extract codegen",
        description="Generate C++ UENUM/USTRUCT/UINTERFACE + empty parents from extracted JSON.")
    parser.add_argument("--out", type=Path, default=None, help="Extracted-JSON dir.")
    parser.add_argument("--source", type=Path, default=None, help="C++ module source dir.")
    parser.add_argument("--system", default=None, help="Only this system.")
    parser.add_argument("--kinds", default="Enum,Struct,Interface,Parent",
                        help="Comma list: Enum,Struct,Interface,Parent.")
    parser.add_argument("--dry-run", action="store_true", help="List files without writing.")
    config.add_common_args(parser)
    args = parser.parse_args(argv)

    cfg = config.resolve(args)
    config.configure(cfg)
    if config.maybe_print_config(cfg, args):
        return 0
    out_dir = args.out or cfg.out_dir
    source_dir = args.source or cfg.source_dir

    records = _load_records(out_dir)
    if not records:
        print(f"No records under {out_dir}. Run extraction first.", file=sys.stderr)
        return 1

    index = build_index(records)
    want = {k.strip() for k in args.kinds.split(",")}
    written, flagged = [], []

    for r in records:
        a = r.get("asset", {})
        if args.system and (a.get("system") or "") != args.system:
            continue
        kind = a.get("asset_kind")

        out_files: list[tuple[str, str]] = []
        if kind == "Enum" and "Enum" in want and r.get("enum_entries"):
            out_files.append(emit_enum(r))
        elif kind == "Struct" and "Struct" in want and r.get("struct_fields") is not None:
            out_files.append(emit_struct(r, index))
        elif kind == "Interface" and "Interface" in want:
            out_files.append(emit_interface(r, index))
        elif "Parent" in want and kind in ("Blueprint", "Component", "AnimBlueprint"):
            res = emit_parent(r, index)
            if res is None:
                flagged.append(f"{a.get('name')}: parent '{a.get('parent_class')}' not in PARENT_MAP — hand-write")
                continue
            rel, header, cpp = res
            out_files.append((rel, header))
            out_files.append((rel[:-2] + ".cpp", cpp))
        else:
            continue

        for rel, content in out_files:
            dest = source_dir / rel
            if args.dry_run:
                print(f"  would write {dest}")
            else:
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_text(content, encoding="utf-8")
            written.append(rel)

    print(f"\n{'Would generate' if args.dry_run else 'Generated'} {len(written)} file(s) under {source_dir}")
    for rel in written:
        print(f"  {rel}")
    if flagged:
        print(f"\nFlagged ({len(flagged)}) — review/hand-write:")
        for f in flagged:
            print(f"  ! {f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
