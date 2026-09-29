# Copyright 2025-2026 NGG. All Rights Reserved.
"""Map BP types and struct fields to C++ types, with project-mirror prefixing.

Two inputs are mapped:
  - list_variables ``type`` strings: ``bool``, ``int32``, ``FString``,
    ``enum:/path``, ``struct:/path``, ``object:Name``, ``array<T>``, ``map<K,V>`` …
  - struct_fields ``cpp_type`` (FProperty::GetCPPType, already exact C++ for engine
    types) + ``type_object`` (referenced asset path).

Project (/Game) types are remapped to generated C++ mirrors; engine types pass through.
"""

from __future__ import annotations

import re

PROJECT_PREFIX = "/Game/"
TOKEN = "GameAnim"  # E/F<TOKEN><Name>, U/I/A<TOKEN><Name>

# Leading asset-name prefixes stripped when forming a C++ identifier.
_NAME_PREFIXES = ("E_", "S_", "BPI_", "BP_", "AC_", "ABP_", "GM_", "PC_", "CR_", "CHT_", "Curve_")

_SCALARS = {
    "bool": "bool", "int32": "int32", "int64": "int64", "int": "int32",
    "float": "float", "double": "double", "byte": "uint8",
    "FString": "FString", "FName": "FName", "FText": "FText",
}


def package_path(ref: str) -> str:
    return ref.split(".", 1)[0]


def is_project(ref: str) -> bool:
    return bool(ref) and ref.startswith(PROJECT_PREFIX)


def _last_seg(ref: str) -> str:
    return package_path(ref).rstrip("/").rsplit("/", 1)[-1]


def clean_name(asset_name: str) -> str:
    n = asset_name
    for p in _NAME_PREFIXES:
        if n.startswith(p):
            n = n[len(p):]
            break
    parts = [s for s in re.split(r"[^0-9A-Za-z]", n) if s]
    core = "".join(parts)  # drop separators -> CamelCase-ish
    if core and core[0].isdigit():
        core = "_" + core
    return core or "Unnamed"


def enum_mirror(ref: str) -> str:
    return f"E{TOKEN}{clean_name(_last_seg(ref))}"


def struct_mirror(ref: str) -> str:
    return f"F{TOKEN}{clean_name(_last_seg(ref))}"


def interface_mirror(ref: str) -> tuple[str, str]:
    core = clean_name(_last_seg(ref))
    return f"U{TOKEN}{core}Interface", f"I{TOKEN}{core}Interface"


def class_mirror(ref: str, actor: bool) -> str:
    return f"{'A' if actor else 'U'}{TOKEN}{clean_name(_last_seg(ref))}"


# --- variable type strings -------------------------------------------------

def _typename(ref: str) -> str:
    """Type/class name from an object path.

    For ``/Script/Module.Class`` the name is *after* the dot (engine native types);
    for asset paths it is the package's last segment, with any ``_C`` suffix dropped.
    """
    if ref.startswith("/Script/"):
        return ref.rsplit(".", 1)[-1] if "." in ref else ref.rstrip("/").rsplit("/", 1)[-1]
    seg = package_path(ref).rstrip("/").rsplit("/", 1)[-1]
    return seg[:-2] if seg.endswith("_C") else seg


def map_var_type(type_str: str, type_object: str | None, index: dict) -> str:
    """Map a list_variables type string to a C++ type, remapping project refs."""
    if not type_str:
        return "int32"

    m = re.match(r"^(array|set)<(.+)>$", type_str)
    if m:
        inner = map_var_type(m.group(2).strip(), type_object, index)
        return f"T{'Array' if m.group(1) == 'array' else 'Set'}<{inner}>"
    m = re.match(r"^map<(.+),\s*(.+)>$", type_str)
    if m:
        k = map_var_type(m.group(1).strip(), None, index)
        v = map_var_type(m.group(2).strip(), None, index)
        return f"TMap<{k}, {v}>"

    # read_graph pins report only the category ("struct"/"enum"/"byte") and carry the
    # concrete type in type_object — resolve those before the scalar/tag handling.
    if type_object and type_str in ("struct", "enum", "byte"):
        if type_str == "struct":
            return struct_mirror(type_object) if is_project(type_object) else "F" + _typename(type_object)
        return enum_mirror(type_object) if is_project(type_object) else _typename(type_object)

    if type_str in _SCALARS:
        return _SCALARS[type_str]

    tag, _, rest = type_str.partition(":")
    if tag == "enum":
        return enum_mirror(rest) if is_project(rest) else _typename(rest)  # engine UENUM name has E
    if tag == "struct":
        return struct_mirror(rest) if is_project(rest) else "F" + _typename(rest)
    if tag in ("object", "softobject"):
        ref = type_object or rest
        core = class_mirror(ref, _is_actor(ref, index)) if is_project(ref) else "U" + _typename(ref)
        return f"TObjectPtr<{core}>" if tag == "object" else f"TSoftObjectPtr<{core}>"
    if tag in ("class", "softclass"):
        ref = type_object or rest
        core = class_mirror(ref, _is_actor(ref, index)) if is_project(ref) else "U" + _typename(ref)
        return f"TSubclassOf<{core}>" if tag == "class" else f"TSoftClassPtr<{core}>"
    if tag == "interface":
        ref = type_object or rest
        return interface_mirror(ref)[1] if is_project(ref) else "I" + _typename(ref)

    # Fallback: a bare class name (object var without a path) — assume UObject ptr.
    return f"TObjectPtr<U{clean_name(type_str)}>"


def _is_actor(ref: str, index: dict) -> bool:
    info = index.get(package_path(ref))
    return bool(info and str(info.get("kind", "")).lower() in ("blueprint", "character", "pawn", "actor"))


# --- struct fields (cpp_type is already exact for engine types) ------------

_WRAP = re.compile(r"^(TArray|TSet|TObjectPtr|TSubclassOf|TSoftObjectPtr|TSoftClassPtr)<(.+)>$")


def map_struct_field(field: dict, index: dict) -> str:
    cpp = (field.get("cpp_type") or "").strip()
    ref = field.get("type_object")

    if ref and is_project(ref):
        info = index.get(package_path(ref))
        kind = (info or {}).get("kind", "")
        if kind == "Enum":
            mirror = enum_mirror(ref)
        elif kind == "Struct":
            mirror = struct_mirror(ref)
        else:
            mirror = class_mirror(ref, _is_actor(ref, index))
        # Preserve a single container/pointer wrapper from the exact cpp_type.
        w = _WRAP.match(cpp)
        if w:
            return f"{w.group(1)}<{mirror}>"
        return mirror

    return cpp or "int32"
