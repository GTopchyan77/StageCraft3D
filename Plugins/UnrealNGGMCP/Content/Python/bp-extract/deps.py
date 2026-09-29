# Copyright 2025-2026 NGG. All Rights Reserved.
"""Derive dependency edges from a normalized extracted record.

A dependency is any reference from one asset to a class/struct/enum/asset:
  - parent class
  - member variable types (struct:/enum: paths, type_object, default_object)
  - SCS component classes
  - graph pins (type_object, default_object) and node targets (call/cast/variable/macro)
  - implemented interfaces

References are classified engine (/Script, /Engine) vs project (/Game). Only
project references that resolve to another asset matter for conversion ordering.
"""

from __future__ import annotations

import re

ENGINE_PREFIXES = ("/Script/", "/Engine/")
PROJECT_PREFIX = "/Game/"

# struct:/enum:/object:... tags inside a list_variables type string, incl. inside array<>/map<>.
_TYPE_TAG = re.compile(
    r"(?:struct|enum|object|class|interface|softobject|softclass):(/[^,>]+)"
)


def classify(ref: str) -> str:
    if not ref:
        return "other"
    if ref.startswith(PROJECT_PREFIX):
        return "project"
    if ref.startswith(ENGINE_PREFIXES):
        return "engine"
    return "other"


def package_path(ref: str) -> str:
    """Object path -> owning package path (drops a trailing ``.SubObject``).

    ``/Game/Blueprints/Foo.Foo_C`` -> ``/Game/Blueprints/Foo``
    A package path never contains a dot, so splitting on the first dot is safe.
    """
    return ref.split(".", 1)[0]


def _refs_from_variable(v: dict) -> list[str]:
    refs: list[str] = []
    if v.get("type_object"):
        refs.append(v["type_object"])
    if v.get("default_object"):
        refs.append(v["default_object"])
    refs.extend(_TYPE_TAG.findall(v.get("type") or ""))
    return refs


def _refs_from_graph(graph: dict) -> list[str]:
    refs: list[str] = []
    for node in graph.get("nodes", []):
        target = node.get("target") or {}
        if target.get("class"):
            refs.append(target["class"])
        if target.get("graph"):
            refs.append(target["graph"])
        for pin in node.get("pins", []):
            if pin.get("type_object"):
                refs.append(pin["type_object"])
            if pin.get("default_object"):
                refs.append(pin["default_object"])
    return refs


def asset_dependencies(record: dict) -> dict:
    """Return engine/project dependency sets for a record, plus a per-graph breakdown.

    ``project`` and the per-graph project sets are package paths (self excluded).
    """
    self_pkg = package_path(record.get("asset", {}).get("object_path", ""))

    def project_pkgs(refs) -> set[str]:
        out = set()
        for r in refs:
            if classify(r) == "project":
                pkg = package_path(r)
                if pkg and pkg != self_pkg:
                    out.add(pkg)
        return out

    engine: set[str] = set()
    project: set[str] = set()

    # Top-level references (parent, interfaces, variables, components).
    top_refs: list[str] = []
    parent = record.get("asset", {}).get("parent_class")
    if parent:
        top_refs.append(parent)
    top_refs.extend(record.get("interfaces", []) or [])
    for v in record.get("variables", []):
        top_refs.extend(_refs_from_variable(v))
    for c in record.get("components", []):
        if c.get("class"):
            top_refs.append(c["class"])

    for r in top_refs:
        kind = classify(r)
        if kind == "engine":
            engine.add(r)
        elif kind == "project":
            pkg = package_path(r)
            if pkg and pkg != self_pkg:
                project.add(pkg)

    # Graph-level references, tracked per graph so we can flag engine-only functions.
    by_graph: dict[str, dict] = {}
    for kind_key in ("functions", "event_graphs", "macros"):
        for item in record.get(kind_key, []):
            graph = item.get("graph") if kind_key == "functions" else item
            name = item.get("name", "?")
            refs = _refs_from_graph(graph or {})
            for r in refs:
                c = classify(r)
                if c == "engine":
                    engine.add(r)
            gp = project_pkgs(refs)
            project |= gp
            by_graph[name] = {
                "kind": "function" if kind_key == "functions" else kind_key[:-1],
                "engine_only": len(gp) == 0,
                "project": sorted(gp),
            }

    return {
        "engine": sorted(engine),
        "project": sorted(project),
        "by_graph": by_graph,
    }
