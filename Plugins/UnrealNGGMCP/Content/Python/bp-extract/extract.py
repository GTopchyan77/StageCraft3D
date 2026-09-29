# Copyright 2025-2026 NGG. All Rights Reserved.
"""Kind-aware extraction of a single Blueprint/asset into a normalized record.

Pulls together three read routes (assets/get, bp/list_variables, bp/read_graph)
and reshapes their output into the schema documented in README.md. Everything is
best-effort: a route that fails or a field the bridge does not expose is recorded
in ``warnings`` instead of aborting the extraction.
"""

from __future__ import annotations

from bridge import Bridge, BridgeError
import paths

# Pin categories that carry execution flow rather than data — excluded from
# reconstructed function signatures.
EXEC_PIN = "exec"

# UObject _class values that have no event graph / member variables.
DATA_KINDS = {
    "UserDefinedStruct": "Struct",
    "UserDefinedEnum": "Enum",
    "CurveFloat": "Curve",
    "CurveVector": "Curve",
    "CurveLinearColor": "Curve",
    "BlueprintGeneratedClass": "Blueprint",
    "Blueprint": "Blueprint",
    "AnimBlueprint": "AnimBlueprint",
    "WidgetBlueprint": "WidgetBlueprint",
}


def _kind_from_class(asset_class: str) -> str:
    return DATA_KINDS.get(asset_class, asset_class or "Unknown")


def _classify_graph(graph: dict) -> str:
    """Return 'function', 'macro', or 'event' from a graph's node classes."""
    classes = {n.get("class", "") for n in graph.get("nodes", [])}
    if "K2Node_FunctionEntry" in classes:
        return "function"
    if "K2Node_Tunnel" in classes:
        return "macro"
    return "event"


def _data_pins(node: dict, direction: str) -> list[dict]:
    """Data pins (non-exec) of a node in the requested direction."""
    out = []
    for pin in node.get("pins", []):
        if pin.get("direction") != direction:
            continue
        if pin.get("type") == EXEC_PIN:
            continue
        name = pin.get("name", "")
        if name in ("self",):  # implicit context pin, not a real parameter
            continue
        entry = {"name": name, "type": pin.get("type", "")}
        if pin.get("type_object"):
            entry["type_object"] = pin["type_object"]
        if pin.get("container"):
            entry["container"] = pin["container"]
        out.append(entry)
    return out


def _function_signature(graph: dict) -> tuple[list[dict], list[dict]]:
    """Reconstruct (inputs, outputs) from a function graph's entry/result nodes."""
    inputs: list[dict] = []
    outputs: list[dict] = []
    for node in graph.get("nodes", []):
        cls = node.get("class", "")
        if cls == "K2Node_FunctionEntry":
            inputs.extend(_data_pins(node, "output"))
        elif cls == "K2Node_FunctionResult":
            outputs.extend(_data_pins(node, "input"))
    return inputs, outputs


def _build_edges(graph: dict) -> list[dict]:
    """Flatten per-pin links into directed edges (emitted from output pins only)."""
    edges = []
    for node in graph.get("nodes", []):
        src_id = node.get("node_id", "")
        for pin in node.get("pins", []):
            if pin.get("direction") != "output":
                continue
            for link in pin.get("connected_to", []):
                edges.append({
                    "from": {"node": src_id, "pin": pin.get("name", "")},
                    "to": {"node": link.get("node", ""), "pin": link.get("pin", "")},
                })
    return edges


def _find_field(obj: dict, *candidates: str):
    """Case-insensitive lookup of the first matching key in an assets/get dump."""
    lowered = {k.lower(): v for k, v in obj.items()}
    for c in candidates:
        if c.lower() in lowered:
            return lowered[c.lower()]
    return None


def extract_asset(
    bridge: Bridge,
    object_path: str,
    *,
    bp_path: str | None = None,
    asset_kind: str | None = None,
    system: str | None = None,
) -> dict:
    """Extract one asset into the normalized record. Never raises on missing data."""
    warnings: list[str] = []
    routes_used: list[str] = []
    partial = False

    record: dict = {
        "asset": {
            "object_path": object_path,
            "bp_path": bp_path,
            "name": paths.asset_name(object_path),
            "class": None,
            "parent_class": None,
            "asset_kind": asset_kind,
            "system": system,
        },
        "interfaces": [],
        "variables": [],
        "components": [],
        "functions": [],
        "event_graphs": [],
        "macros": [],
        "source": {"bridge_url": bridge.base_url, "routes_used": routes_used},
        "partial": False,
        "warnings": warnings,
    }

    # ---- assets/get: class, parent, interfaces ----------------------------
    asset_class = None
    try:
        info = bridge.assets_get(object_path)
        routes_used.append("/assets/get")
        asset_class = info.get("_class")
        record["asset"]["class"] = asset_class
        if asset_kind is None:
            record["asset"]["asset_kind"] = _kind_from_class(asset_class or "")

        parent = _find_field(info, "ParentClass", "ParentClassPath", "NativeParentClass")
        if parent:
            record["asset"]["parent_class"] = parent if isinstance(parent, str) else str(parent)

        interfaces = _find_field(info, "ImplementedInterfaces", "Interfaces")
        if isinstance(interfaces, list):
            record["interfaces"] = interfaces

        # Data-type introspection for codegen (enriched bridge).
        if isinstance(info.get("enum_entries"), list):
            record["enum_entries"] = info["enum_entries"]
        if isinstance(info.get("struct_fields"), list):
            record["struct_fields"] = info["struct_fields"]
    except BridgeError as err:
        warnings.append(f"assets/get failed: {err}")

    effective_kind = record["asset"]["asset_kind"] or _kind_from_class(asset_class or "")
    is_blueprintish = effective_kind in ("Blueprint", "AnimBlueprint", "Component", "WidgetBlueprint")

    # ---- bp/list_variables: members + components --------------------------
    try:
        vars_resp = bridge.bp_list_variables(object_path)
        routes_used.append("/bp/list_variables")
        # Authoritative parent class (the enriched bridge returns it here).
        if vars_resp.get("parent_class"):
            record["asset"]["parent_class"] = vars_resp["parent_class"]
        for v in vars_resp.get("variables", []):
            record["variables"].append({
                "name": v.get("name"),
                "type": v.get("type"),
                "type_object": v.get("type_object"),
                "default": v.get("default_value"),
                "default_object": v.get("default_object"),
                "category": v.get("category"),
                "container": v.get("container"),
                "flags": {
                    "instance_editable": v.get("instance_editable"),
                    "blueprint_read_only": v.get("blueprint_read_only"),
                    "expose_on_spawn": v.get("expose_on_spawn"),
                    "private": v.get("private"),
                },
            })
        record["components"] = vars_resp.get("components", [])
        record["inherited_count"] = vars_resp.get("inherited_count", 0)
    except BridgeError as err:
        if is_blueprintish:
            warnings.append(f"bp/list_variables failed: {err}")
        partial = True

    # ---- bp/read_graph: functions, event graphs, macros ------------------
    try:
        graph_resp = bridge.bp_read_graph(object_path)
        routes_used.append("/bp/read_graph")
        graphs = graph_resp.get("graphs", {})
        for name, graph in graphs.items():
            kind = _classify_graph(graph)
            edges = _build_edges(graph)
            payload = {"name": name, "nodes": graph.get("nodes", []), "edges": edges}
            if kind == "function":
                inputs, outputs = _function_signature(graph)
                record["functions"].append({
                    "name": name,
                    "inputs": inputs,
                    "outputs": outputs,
                    "graph": payload,
                })
            elif kind == "macro":
                record["macros"].append(payload)
            else:
                record["event_graphs"].append(payload)
    except BridgeError as err:
        if is_blueprintish:
            warnings.append(f"bp/read_graph failed: {err}")
        partial = True

    if is_blueprintish and not record["asset"]["parent_class"]:
        warnings.append("parent_class unavailable")

    if not is_blueprintish:
        partial = True
        if effective_kind in ("Struct", "Enum") and (record.get("struct_fields") or record.get("enum_entries")):
            pass  # data captured via enriched assets_get — not a gap
        else:
            warnings.append(
                f"data-only kind '{effective_kind}': field-level detail is limited to /assets/get"
            )

    record["partial"] = partial
    return record
