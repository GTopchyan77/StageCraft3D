// Copyright 2025-2026 NGG. All Rights Reserved.
// pcg.js — unrealngg-mcp
// PCG (Procedural Content Generation) endpoints. Implemented as thin Python
// scripts shipped to the UE editor via POST /editor/exec_python. No new C++
// plugin code required — every method invoked here is already reflected into
// the `unreal` Python module (UPCGGraph, UPCGComponent, UPCGSubsystem, every
// UPCGSettings subclass, UPCGGraphFactory, etc.).
//
// Endpoints shipped here:
//   Graph authoring
//     pcg_list_node_types
//     pcg_create_graph
//     pcg_add_node
//     pcg_connect_pins
//     pcg_save_graph
//   Introspection
//     pcg_read_graph
//     pcg_list_settings_properties
//   Node mutation
//     pcg_set_node_settings_property
//     pcg_set_node_position
//     pcg_remove_node
//     pcg_disconnect_pins
//     pcg_rename_node
//   PCGComponent runtime
//     pcg_add_component_to_actor
//     pcg_set_component_property
//     pcg_generate_component
//     pcg_cleanup_component
//   Graph properties + parameters
//     pcg_set_graph_property
//     pcg_add_graph_parameter
//     pcg_list_graph_parameters
//     pcg_remove_graph_parameter
//   Graph instance assets
//     pcg_create_graph_instance
//     pcg_set_graph_instance_parameter
//   Editor utilities
//     pcg_open_in_editor
//     pcg_regenerate_all

import * as ue5 from "./ue5client.js";

// ---------------------------------------------------------------------------
// Python helpers
// ---------------------------------------------------------------------------

// Sentinel the Python scripts print around a JSON-encoded result so Node can
// recover structured output from the `log` array exec_python returns.
const RESULT_BEGIN = "__NGG_PCG_RESULT_BEGIN__";
const RESULT_END   = "__NGG_PCG_RESULT_END__";

// Prelude injected at the top of every script. Provides a `ngg_result(obj)`
// function that prints the sentinel-wrapped JSON, plus robust error capture.
const PY_PRELUDE = `
import unreal, json, base64, traceback
def ngg_result(obj):
    print("${RESULT_BEGIN}" + json.dumps(obj) + "${RESULT_END}")
def ngg_error(msg):
    ngg_result({"ok": False, "error": msg})
`;

// Emit the Python that decodes a JS payload into `args` (or any var name)
// without any quoting hazards. JSON.stringify does NOT escape single quotes,
// newlines, or backslashes for a Python triple-quoted literal, so asset
// names/paths containing those could corrupt the script or inject Python.
// base64's alphabet ([A-Za-z0-9+/=]) is safe inside a normal Python string.
function pyArgs(payload, varName = "args") {
  // Accept either an already-JSON string or a plain object.
  const jsonStr = typeof payload === "string" ? payload : JSON.stringify(payload);
  const b64 = Buffer.from(jsonStr, "utf8").toString("base64");
  return `${varName} = json.loads(base64.b64decode("${b64}").decode("utf-8"))`;
}

/**
 * Run a Python script inside the editor and recover the sentinel-wrapped
 * JSON result. The body is wrapped in a function so `return ngg_error(...)`
 * works as an early-exit pattern. Returns { ok, data | error, raw }.
 */
async function runPy(body) {
  const indented = body.split("\n").map((l) => "    " + l).join("\n");
  const script =
    PY_PRELUDE +
    "def _ngg_main():\n" + indented + "\n" +
    "try:\n    _ngg_main()\nexcept Exception as _e:\n    ngg_error(str(_e) + '\\n' + traceback.format_exc())\n";

  let resp;
  try {
    resp = await ue5.execPython(script, "execute_file");
  } catch (err) {
    return { ok: false, error: `bridge: ${err.message}` };
  }

  for (const entry of resp.log ?? []) {
    const out = entry.output ?? "";
    // Prefer the LAST begin sentinel and the first end sentinel AFTER it, so
    // echoed user data containing a sentinel-like string (or a double-print)
    // can't capture garbage. Identical to before for the single-result case.
    const start = out.lastIndexOf(RESULT_BEGIN);
    const end = start >= 0 ? out.indexOf(RESULT_END, start + RESULT_BEGIN.length) : -1;
    if (start >= 0 && end > start) {
      const json = out.slice(start + RESULT_BEGIN.length, end);
      try {
        const parsed = JSON.parse(json);
        if (parsed && parsed.ok === false) return { ok: false, error: parsed.error, raw: resp };
        return { ok: true, data: parsed, raw: resp };
      } catch (err) {
        return { ok: false, error: `bad result json: ${err.message}`, raw: resp };
      }
    }
  }

  // No sentinel — surface the log so the caller can diagnose.
  const logStr = (resp.log ?? []).map((e) => `${e.type}: ${e.output}`).join("\n");
  return { ok: false, error: `no result sentinel; log:\n${logStr}`, raw: resp };
}

// Wrap a Python-produced value/error in the MCP `content` envelope shape.
function mcpResponse(result) {
  if (result.ok) {
    return { content: [{ type: "text", text: JSON.stringify(result.data, null, 2) }] };
  }
  return { content: [{ type: "text", text: result.error }], isError: true };
}

// ---------------------------------------------------------------------------
// Curated list of PCG settings classes. Used as the default node-type
// palette for pcg_list_node_types. Runtime existence is verified before
// returning so the list gracefully degrades across engine versions.
// Grouped roughly by category in the Claude.ai PCG picker UI mental model.
// ---------------------------------------------------------------------------
const CURATED_NODE_TYPES = [
  // Samplers
  ["Samplers", "PCGSurfaceSamplerSettings"],
  ["Samplers", "PCGSplineSamplerSettings"],
  ["Samplers", "PCGVolumeSamplerSettings"],
  ["Samplers", "PCGTextureSamplerSettings"],

  // Point operations
  ["Points", "PCGSelectPointsSettings"],
  ["Points", "PCGTransformPointsSettings"],
  ["Points", "PCGSplitPointsSettings"],
  ["Points", "PCGCopyPointsSettings"],
  ["Points", "PCGDensityFilterSettings"],
  ["Points", "PCGDensityNoiseSettings"],
  ["Points", "PCGNormalToDensitySettings"],
  ["Points", "PCGSelfPruningSettings"],

  // Spawners
  ["Spawners", "PCGStaticMeshSpawnerSettings"],
  ["Spawners", "PCGSkinnedMeshSpawnerSettings"],
  ["Spawners", "PCGSpawnActorSettings"],
  ["Spawners", "PCGSpawnSplineSettings"],
  ["Spawners", "PCGSpawnSplineMeshSettings"],
  ["Spawners", "PCGAddComponentSettings"],

  // Spatial ops
  ["Spatial", "PCGSpatialNoiseSettings"],
  ["Spatial", "PCGUnionSettings"],
  ["Spatial", "PCGIntersectionSettings"],
  ["Spatial", "PCGDifferenceSettings"],
  ["Spatial", "PCGProjectionSettings"],

  // Splines
  ["Splines", "PCGCreateSplineSettings"],
  ["Splines", "PCGReverseSplineSettings"],
  ["Splines", "PCGSplineIntersectionSettings"],
  ["Splines", "PCGSplitSplinesSettings"],
  ["Splines", "PCGSubdivideSplineSettings"],

  // Polygons
  ["Polygons", "PCGCreatePolygon2DSettings"],
  ["Polygons", "PCGOffsetPolygon2DSettings"],
  ["Polygons", "PCGPolygon2DOperationSettings"],
  ["Polygons", "PCGClipPathsSettings"],

  // Control flow
  ["ControlFlow", "PCGBranchSettings"],
  ["ControlFlow", "PCGBooleanSelectSettings"],
  ["ControlFlow", "PCGMultiSelectSettings"],
  ["ControlFlow", "PCGSwitchSettings"],
  ["ControlFlow", "PCGQualityBranchSettings"],
  ["ControlFlow", "PCGQualitySelectSettings"],
  ["ControlFlow", "PCGWaitSettings"],

  // Metadata / attributes
  ["Metadata", "PCGAttributeCastSettings"],
  ["Metadata", "PCGAttributeRemapSettings"],
  ["Metadata", "PCGExtractAttributeSettings"],
  ["Metadata", "PCGHashAttributeSettings"],
  ["Metadata", "PCGMetadataBitwiseSettings"],
  ["Metadata", "PCGMetadataBooleanSettings"],
  ["Metadata", "PCGMetadataCompareSettings"],
  ["Metadata", "PCGMetadataMathsSettings"],
  ["Metadata", "PCGMetadataMakeVectorSettings"],
  ["Metadata", "PCGMetadataMakeRotatorSettings"],
  ["Metadata", "PCGMetadataMakeTransformSettings"],
  ["Metadata", "PCGMetadataPartitionSettings"],
  ["Metadata", "PCGFilterByAttributeSettings"],
  ["Metadata", "PCGFilterByTagSettings"],
  ["Metadata", "PCGFilterByTypeSettings"],

  // IO
  ["IO",   "PCGLoadDataTableSettings"],
  ["IO",   "PCGExportSelectedAttributesSettings"],
  ["IO",   "PCGGetAssetListSettings"],
  ["IO",   "PCGLoadAssetSettings"],
  ["IO",   "PCGSaveAssetSettings"],

  // Typed getters
  ["Getters", "PCGGetLandscapeSettings"],
  ["Getters", "PCGGetSplineSettings"],
  ["Getters", "PCGGetVolumeSettings"],
  ["Getters", "PCGGetPrimitiveSettings"],
  ["Getters", "PCGGetPCGComponentSettings"],
  ["Getters", "PCGGetVirtualTextureSettings"],

  // World
  ["World", "PCGWorldQuerySettings"],
  ["World", "PCGWorldRayHitSettings"],
  ["World", "PCGWorldRaycastElementSettings"],
  ["World", "PCGWaitLandscapeReadySettings"],

  // Grammar
  ["Grammar", "PCGSelectGrammarSettings"],
  ["Grammar", "PCGSubdivideSegmentSettings"],
  ["Grammar", "PCGSubdivideSplineSettings"],
  ["Grammar", "PCGDuplicateCrossSectionsSettings"],
  ["Grammar", "PCGPrintGrammarSettings"],

  // Blueprint-authored node templates
  ["Blueprint", "PCGBlueprintPointProcessorSimpleElement"],
  ["Blueprint", "PCGBlueprintPointProcessorElement"],
  ["Blueprint", "PCGBlueprintBaseElement"],

  // Subgraph / user params
  ["Subgraph", "PCGSubgraphSettings"],
  ["Parameters", "PCGUserParameterGetSettings"],
];

// ---------------------------------------------------------------------------
// Endpoint: pcg_list_node_types
// ---------------------------------------------------------------------------

export async function pcgListNodeTypes({ category } = {}) {
  const entries = category
    ? CURATED_NODE_TYPES.filter(([cat]) => cat.toLowerCase() === category.toLowerCase())
    : CURATED_NODE_TYPES;
  const pyList = entries.map(([cat, name]) => ({ category: cat, name }));

  const result = await runPy(`
${pyArgs(pyList, "entries")}
out = []
for e in entries:
    class_name = e["name"]
    cls = unreal.load_class(None, "/Script/PCG." + class_name)
    if cls is None:
        # Some classes live in the PCGCompute module or elsewhere — try a few.
        for mod in ("PCGCompute", "PCGEditor"):
            cls = unreal.load_class(None, "/Script/" + mod + "." + class_name)
            if cls is not None:
                break
    if cls is None:
        continue
    out.append({"name": class_name, "category": e["category"]})
ngg_result({"ok": True, "classes": out, "count": len(out)})
`);
  return mcpResponse(result);
}

// ---------------------------------------------------------------------------
// Endpoint: pcg_create_graph
// ---------------------------------------------------------------------------

export async function pcgCreateGraph({ asset_path, standalone = false, overwrite = false }) {
  const payload = JSON.stringify({ path: asset_path, standalone, overwrite });
  const result = await runPy(`
${pyArgs(payload)}
full = args["path"]
if "/" not in full:
    return ngg_error("asset_path must be a content-browser path like /Game/PCG/MyGraph")
dir_path, name = full.rsplit("/", 1)

lib = unreal.EditorAssetLibrary
tools = unreal.AssetToolsHelpers.get_asset_tools()

# Overwrite REUSES the existing graph in place (clears its non-IO nodes) rather
# than deleting it. Deleting goes through ObjectTools::ForceDeleteObjects, which
# fires an ensure — and a crash report in unattended/headless mode — whenever the
# graph is still referenced by a live PCGComponent. Reusing avoids that entirely,
# and any component already bound to this graph keeps working against the rebuild.
graph = None
reused = False
if lib.does_asset_exist(full):
    if not args.get("overwrite"):
        return ngg_error("asset already exists: " + full + " (pass overwrite=True to replace)")
    existing = lib.load_asset(full)
    if isinstance(existing, unreal.PCGGraph):
        for n in list(existing.get_editor_property("nodes")):
            try:
                existing.remove_node(n)
            except Exception:
                pass
        graph = existing
        reused = True
    else:
        # A non-PCGGraph asset sits here. Do NOT force-delete (can crash on
        # referenced assets); make the caller choose another path.
        return ngg_error("a non-PCGGraph asset already exists at " + full + " — choose a different path or delete it manually")

if graph is None:
    factory = unreal.PCGGraphFactory()
    try:
        factory.b_skip_template_selection = True
    except Exception:
        pass
    graph = tools.create_asset(name, dir_path, unreal.PCGGraph, factory)
    if graph is None:
        return ngg_error("create_asset returned None for " + full)

if args.get("standalone"):
    try:
        graph.set_editor_property("b_is_standalone_graph", True)
    except Exception:
        pass
lib.save_asset(full)
ngg_result({"ok": True, "path": full, "standalone": bool(args.get("standalone")), "reused": reused})
`);
  return mcpResponse(result);
}

// ---------------------------------------------------------------------------
// Endpoint: pcg_add_node
// ---------------------------------------------------------------------------

export async function pcgAddNode({ graph_path, settings_class, node_title }) {
  const payload = JSON.stringify({ graph: graph_path, cls: settings_class, title: node_title ?? null });
  const result = await runPy(`
${pyArgs(payload)}
lib = unreal.EditorAssetLibrary
graph = lib.load_asset(args["graph"])
if not isinstance(graph, unreal.PCGGraph):
    return ngg_error("not a PCGGraph asset: " + args["graph"])

class_name = args["cls"]
cls = None
for mod in ("PCG", "PCGCompute", "PCGEditor"):
    cls = unreal.load_class(None, "/Script/" + mod + "." + class_name)
    if cls is not None:
        break
if cls is None:
    return ngg_error("unknown PCG settings class: " + class_name)

result = graph.add_node_of_type(cls)
if isinstance(result, tuple):
    node, _settings = result[0], result[1]
else:
    node = result
if node is None:
    return ngg_error("add_node_of_type returned None for " + class_name)

if args.get("title"):
    try:
        node.set_editor_property("node_title", unreal.Text(args["title"]))
    except Exception:
        pass

# Mutate in memory and mark dirty — do NOT save_asset() here. Saving (and the
# editor's content-validation pass) on every single node add is the dominant
# cost when authoring a graph (N nodes + M edges = N+M full SavePackage runs).
# Persistence is deferred to the explicit pcg_save_graph endpoint (or editor
# Save All); the in-memory graph object is what later add_node/connect_pins
# calls load and keep editing, so edits accumulate correctly between calls.
try:
    graph.mark_package_dirty()
except Exception:
    pass

# Return node identity so callers can reference it later.
node_info = {
    "ok": True,
    "graph": args["graph"],
    "settings_class": class_name,
    "node_name": str(node.get_name()),
    "node_title": str(node.get_editor_property("node_title")) if hasattr(node, "get_editor_property") else "",
}
ngg_result(node_info)
`);
  return mcpResponse(result);
}

// ---------------------------------------------------------------------------
// Endpoint: pcg_connect_pins
// ---------------------------------------------------------------------------

export async function pcgConnectPins({ graph_path, from_node, from_pin, to_node, to_pin }) {
  const payload = JSON.stringify({
    graph: graph_path,
    from_node, from_pin,
    to_node, to_pin,
  });
  const result = await runPy(`
${pyArgs(payload)}
lib = unreal.EditorAssetLibrary
graph = lib.load_asset(args["graph"])
if not isinstance(graph, unreal.PCGGraph):
    return ngg_error("not a PCGGraph asset: " + args["graph"])

def resolve(name):
    if name in ("__input__", "InputNode", "Input"):
        return graph.get_input_node()
    if name in ("__output__", "OutputNode", "Output"):
        return graph.get_output_node()
    for n in graph.get_editor_property("nodes"):
        if str(n.get_name()) == name:
            return n
    return None

frm = resolve(args["from_node"])
to  = resolve(args["to_node"])
if frm is None:
    return ngg_error("from_node not found: " + args["from_node"])
if to is None:
    return ngg_error("to_node not found: " + args["to_node"])

edge = graph.add_edge(frm, unreal.Name(args["from_pin"]), to, unreal.Name(args["to_pin"]))
if edge is None:
    return ngg_error("add_edge returned None — pin labels may not match")

# Mark dirty rather than save per-edge (see pcg_add_node). pcg_save_graph
# flushes the whole graph to disk once authoring is complete.
try:
    graph.mark_package_dirty()
except Exception:
    pass
ngg_result({
    "ok": True,
    "graph": args["graph"],
    "from": {"node": str(frm.get_name()), "pin": args["from_pin"]},
    "to":   {"node": str(to.get_name()),  "pin": args["to_pin"]},
})
`);
  return mcpResponse(result);
}

// ---------------------------------------------------------------------------
// Endpoint: pcg_save_graph
// ---------------------------------------------------------------------------

export async function pcgSaveGraph({ graph_path }) {
  const payload = JSON.stringify({ graph: graph_path });
  const result = await runPy(`
${pyArgs(payload)}
lib = unreal.EditorAssetLibrary
asset = lib.load_asset(args["graph"])
if asset is None:
    return ngg_error("asset not found: " + args["graph"])
ok = lib.save_asset(args["graph"])
ngg_result({"ok": bool(ok), "path": args["graph"]})
`);
  return mcpResponse(result);
}

// ---------------------------------------------------------------------------
// Shared Python helper snippets, inlined into endpoints that need them.
//
// PY_RESOLVE_NODE — produces a function `resolve(name)` that returns the
//   PCGNode matching `name`, supporting '__input__' / '__output__' aliases
//   or a literal node name from get_name().
// PY_NODE_TO_DICT — produces a function `node_to_dict(node)` that returns a
//   JSON-friendly dict of a node's identity, position, settings class, and
//   pin labels. Tolerates engine-version differences (some accessors moved
//   between UE5.4 and 5.7).
// ---------------------------------------------------------------------------

const PY_RESOLVE_NODE = `
def resolve(name):
    if name in ("__input__", "InputNode", "Input"):
        return graph.get_input_node()
    if name in ("__output__", "OutputNode", "Output"):
        return graph.get_output_node()
    try:
        nodes = graph.get_editor_property("nodes")
    except Exception:
        nodes = []
    for n in nodes:
        if str(n.get_name()) == name:
            return n
    return None
`;

const PY_NODE_TO_DICT = `
def node_to_dict(n):
    if n is None:
        return None
    info = {"name": str(n.get_name())}
    try:
        info["title"] = str(n.get_editor_property("node_title"))
    except Exception:
        info["title"] = ""
    try:
        s = n.get_settings()
        info["settings_class"] = s.get_class().get_name() if s else ""
    except Exception:
        info["settings_class"] = ""
    try:
        pos = n.get_node_position()
        info["position"] = [int(pos[0]), int(pos[1])]
    except Exception:
        try:
            info["position"] = [int(n.get_editor_property("position_x")), int(n.get_editor_property("position_y"))]
        except Exception:
            info["position"] = [0, 0]
    info["input_pins"]  = [str(p.get_editor_property("properties").label) for p in (n.get_editor_property("input_pins")  or [])]
    info["output_pins"] = [str(p.get_editor_property("properties").label) for p in (n.get_editor_property("output_pins") or [])]
    return info
`;

// ---------------------------------------------------------------------------
// Endpoint: pcg_read_graph — dump full graph state (nodes + edges + I/O).
// ---------------------------------------------------------------------------

export async function pcgReadGraph({ graph_path }) {
  const payload = JSON.stringify({ graph: graph_path });
  const result = await runPy(`
${pyArgs(payload)}
lib = unreal.EditorAssetLibrary
graph = lib.load_asset(args["graph"])
if not isinstance(graph, unreal.PCGGraph):
    return ngg_error("not a PCGGraph asset: " + args["graph"])

${PY_NODE_TO_DICT}

nodes = []
nodes.append({**node_to_dict(graph.get_input_node()),  "role": "input"})
for n in (graph.get_editor_property("nodes") or []):
    nodes.append({**node_to_dict(n), "role": "node"})
nodes.append({**node_to_dict(graph.get_output_node()), "role": "output"})

edges = []
seen = set()
input_node  = graph.get_input_node()
output_node = graph.get_output_node()
all_nodes = [input_node, output_node] + list(graph.get_editor_property("nodes") or [])

# FPCGEdge's InputPin/OutputPin TWeakObjectPtr fields are not BlueprintReadable
# in 5.5/5.7, so we can't dereference an edge to its pins directly. Instead,
# edges are stored on the endpoint pins (output pin's edges + input pin's edges
# share the SAME FPCGEdge object identity for inner connections). We index edge
# → pin membership and pair the two pins each edge belongs to.
pin_index = []  # list of (edge_obj, owner_node_name, pin_label, is_output)
unmatched_inputs = []  # input-pin entries that may need a synthetic source
for n in all_nodes:
    if n is None:
        continue
    for is_out, pins in ((False, n.get_editor_property("input_pins")),
                         (True,  n.get_editor_property("output_pins"))):
        for pin in (pins or []):
            try:
                label = str(pin.get_editor_property("properties").label)
            except Exception:
                label = ""
            for e in (pin.get_editor_property("edges") or []):
                pin_index.append((e, str(n.get_name()), label, is_out))

# Pair by Python object identity: each edge appears once on the source pin
# (is_output=True) and once on the destination pin (is_output=False).
by_edge = {}
for e, owner, label, is_out in pin_index:
    by_edge.setdefault(id(e), []).append((owner, label, is_out))

input_node_name  = input_node.get_name()  if input_node  else None
output_node_name = output_node.get_name() if output_node else None

for _eid, entries in by_edge.items():
    src = next((x for x in entries if x[2]), None)
    dst = next((x for x in entries if not x[2]), None)
    # Connections from the graph input node are not always stored on its
    # output-pin's edges array in 5.5/5.7. When we see a destination pin with
    # no matching source, attribute it to the graph input node — that's the
    # only edge endpoint that could be unmatched in a well-formed graph.
    if src is None and dst is not None and input_node_name is not None:
        src = (input_node_name, dst[1], True)
    # Similarly, connections to the graph output node may only be stored on
    # the output node's input pin. Synthesize the destination if missing.
    if dst is None and src is not None and output_node_name is not None:
        dst = (output_node_name, src[1], False)
    if src is None or dst is None:
        continue
    key = (src[0], src[1], dst[0], dst[1])
    if key in seen:
        continue
    seen.add(key)
    edges.append({"from_node": key[0], "from_pin": key[1],
                  "to_node":   key[2], "to_pin":   key[3]})

ngg_result({"ok": True, "graph": args["graph"], "nodes": nodes, "edges": edges})
`);
  return mcpResponse(result);
}

// ---------------------------------------------------------------------------
// Endpoint: pcg_list_settings_properties — list editable UPROPERTYs on a
// UPCGSettings subclass, so callers know what they can mutate with
// pcg_set_node_settings_property.
// ---------------------------------------------------------------------------

export async function pcgListSettingsProperties({ settings_class }) {
  const payload = JSON.stringify({ cls: settings_class });
  const result = await runPy(`
${pyArgs(payload)}
class_name = args["cls"]
cls = None
for mod in ("PCG", "PCGCompute", "PCGEditor"):
    cls = unreal.load_class(None, "/Script/" + mod + "." + class_name)
    if cls is not None:
        break
if cls is None:
    return ngg_error("unknown PCG settings class: " + class_name)

# Instantiate a transient CDO-like instance and walk its property names.
try:
    cdo = unreal.get_default_object(cls)
except Exception:
    cdo = None

import warnings

props = []
deprecated_skipped = 0
if cdo is not None:
    # The reflected unreal.* binding exposes editor-property names directly.
    for name in sorted(dir(cdo)):
        if name.startswith("_"):
            continue
        # Read each property while capturing Python warnings. The unreal binding
        # raises a DeprecationWarning when a renamed/legacy alias is read (e.g.
        # 'params'->'sampler_params', 'use_seed', 'mesh_selector_instance'). Skip
        # those so we (a) never list a deprecated alias and (b) don't spam the
        # output log — callers only see the current ("last way") API.
        try:
            with warnings.catch_warnings(record=True) as caught:
                warnings.simplefilter("always")
                val = cdo.get_editor_property(name)
            if any(issubclass(w.category, DeprecationWarning) for w in caught):
                deprecated_skipped += 1
                continue
        except Exception:
            continue
        # Skip methods — we only want exposed UPROPERTYs that get_editor_property reads.
        type_str = type(val).__name__
        try:
            sval = str(val)
            if len(sval) > 120:
                sval = sval[:117] + "..."
        except Exception:
            sval = ""
        props.append({"name": name, "type": type_str, "default": sval})

ngg_result({"ok": True, "settings_class": class_name, "properties": props, "count": len(props), "deprecated_skipped": deprecated_skipped})
`);
  return mcpResponse(result);
}

// ---------------------------------------------------------------------------
// Endpoint: pcg_set_node_settings_property — set a UPROPERTY on a node's
// settings object. Accepts a raw value or an asset content path (auto-loaded
// when the property is an object reference, e.g. a StaticMesh for a spawner).
// ---------------------------------------------------------------------------

export async function pcgSetNodeSettingsProperty({ graph_path, node, property, value, asset_value }) {
  const payload = JSON.stringify({
    graph: graph_path, node, property,
    value: value === undefined ? null : value,
    asset_value: asset_value ?? null,
  });
  const result = await runPy(`
${pyArgs(payload)}
lib = unreal.EditorAssetLibrary
graph = lib.load_asset(args["graph"])
if not isinstance(graph, unreal.PCGGraph):
    return ngg_error("not a PCGGraph asset: " + args["graph"])

${PY_RESOLVE_NODE}

node = resolve(args["node"])
if node is None:
    return ngg_error("node not found: " + args["node"])

settings = node.get_settings()
if settings is None:
    return ngg_error("node has no settings: " + args["node"])

prop_name = args["property"]
asset_path = args.get("asset_value")
raw_value = args.get("value")

# --- Virtual helper: assign a StaticMesh to a Static Mesh Spawner ----------
# The mesh lives inside the read-only, instanced weighted mesh selector, so a
# plain set_editor_property can't reach it. Accept property 'mesh' (or
# 'static_mesh') with asset_value = a StaticMesh path; optional value = integer
# weight (default 1). Pass value="replace" to clear existing entries first;
# otherwise entries append so repeated calls build a weighted variety set.
is_sms = settings.get_class().get_name() == "PCGStaticMeshSpawnerSettings"
if is_sms and prop_name in ("mesh", "static_mesh", "meshes"):
    if not asset_path:
        return ngg_error("mesh assignment needs asset_value (a StaticMesh content path)")
    mesh = lib.load_asset(asset_path)
    if mesh is None:
        return ngg_error("asset_value path failed to load: " + asset_path)
    sel = settings.get_editor_property("mesh_selector_parameters")
    if sel is None:
        try:
            settings.set_editor_property("mesh_selector_type", unreal.PCGMeshSelectorWeighted.static_class())
        except Exception:
            pass
        sel = settings.get_editor_property("mesh_selector_parameters")
    if sel is None:
        return ngg_error("could not access mesh_selector_parameters on the spawner")
    entry = unreal.PCGMeshSelectorWeightedEntry()
    desc = entry.get_editor_property("descriptor")
    desc.set_editor_property("static_mesh", mesh)
    entry.set_editor_property("descriptor", desc)
    weight = 1
    try:
        if raw_value is not None and str(raw_value).lstrip("-").isdigit():
            weight = int(raw_value)
    except Exception:
        weight = 1
    entry.set_editor_property("weight", weight)
    entries = list(sel.get_editor_property("mesh_entries") or [])
    if str(raw_value).lower() == "replace":
        entries = []
    entries.append(entry)
    sel.set_editor_property("mesh_entries", entries)
    lib.save_asset(args["graph"])
    ngg_result({"ok": True, "graph": args["graph"], "node": args["node"],
                "property": prop_name, "mesh": asset_path, "mesh_entry_count": len(entries)})
    return

# --- Resolve the value to set ----------------------------------------------
new_value = None
if asset_path:
    new_value = lib.load_asset(asset_path)
    if new_value is None:
        return ngg_error("asset_value path failed to load: " + asset_path)
else:
    # The MCP layer may deliver an array/object 'value' as a JSON string
    # (e.g. "[2,3,4]"); parse it back so struct coercion below can run.
    if isinstance(raw_value, str):
        s = raw_value.strip()
        if s[:1] in "[{":
            try:
                raw_value = json.loads(s)
            except Exception:
                pass
    new_value = raw_value
    # set_editor_property won't accept a raw JSON list/dict for struct
    # properties — coerce into the Vector / Rotator the property expects.
    if isinstance(raw_value, (list, dict)):
        try:
            cur = settings.get_editor_property(prop_name)
        except Exception:
            cur = None
        if isinstance(cur, unreal.Vector):
            if isinstance(raw_value, dict):
                new_value = unreal.Vector(float(raw_value.get("x", 0.0)), float(raw_value.get("y", 0.0)), float(raw_value.get("z", 0.0)))
            else:
                v = [float(x) for x in raw_value] + [0.0, 0.0, 0.0]
                new_value = unreal.Vector(v[0], v[1], v[2])
        elif isinstance(cur, unreal.Rotator):
            r = unreal.Rotator()
            if isinstance(raw_value, dict):
                r.roll = float(raw_value.get("roll", 0.0)); r.pitch = float(raw_value.get("pitch", 0.0)); r.yaw = float(raw_value.get("yaw", 0.0))
            else:
                v = [float(x) for x in raw_value] + [0.0, 0.0, 0.0]
                r.pitch = v[0]; r.yaw = v[1]; r.roll = v[2]
            new_value = r

try:
    settings.set_editor_property(prop_name, new_value)
except Exception as e:
    return ngg_error("set_editor_property failed for " + prop_name + ": " + str(e))

lib.save_asset(args["graph"])

# Echo the post-set value for confirmation.
try:
    confirmed = str(settings.get_editor_property(prop_name))
    if len(confirmed) > 200:
        confirmed = confirmed[:197] + "..."
except Exception:
    confirmed = ""

ngg_result({
    "ok": True, "graph": args["graph"], "node": args["node"],
    "property": prop_name, "value": confirmed,
})
`);
  return mcpResponse(result);
}

// ---------------------------------------------------------------------------
// Endpoint: pcg_set_node_transform_ranges — convenience setter for the six
// Vector/Rotator range properties on a transform-points style node
// (scale_min/max, offset_min/max as Vector; rotation_min/max as Rotator) in a
// single call. Each arg is optional; only provided ones are set. Values may be
// arrays ([x,y,z] / [pitch,yaw,roll]), objects ({x,y,z} / {pitch,yaw,roll}),
// or JSON strings of either — same coercion as pcg_set_node_settings_property.
// ---------------------------------------------------------------------------

export async function pcgSetNodeTransformRanges({
  graph_path, node,
  scale_min, scale_max, offset_min, offset_max, rotation_min, rotation_max,
}) {
  // Only forward keys the caller actually supplied so we don't clobber unset
  // ranges with nulls. The Python side iterates this map.
  const ranges = {};
  if (scale_min    !== undefined) ranges.scale_min    = scale_min;
  if (scale_max    !== undefined) ranges.scale_max    = scale_max;
  if (offset_min   !== undefined) ranges.offset_min   = offset_min;
  if (offset_max   !== undefined) ranges.offset_max   = offset_max;
  if (rotation_min !== undefined) ranges.rotation_min = rotation_min;
  if (rotation_max !== undefined) ranges.rotation_max = rotation_max;

  const payload = JSON.stringify({ graph: graph_path, node, ranges });
  const result = await runPy(`
${pyArgs(payload)}
lib = unreal.EditorAssetLibrary
graph = lib.load_asset(args["graph"])
if not isinstance(graph, unreal.PCGGraph):
    return ngg_error("not a PCGGraph asset: " + args["graph"])

${PY_RESOLVE_NODE}

node = resolve(args["node"])
if node is None:
    return ngg_error("node not found: " + args["node"])

settings = node.get_settings()
if settings is None:
    return ngg_error("node has no settings: " + args["node"])

ranges = args.get("ranges") or {}
if not ranges:
    return ngg_error("no transform ranges supplied (set at least one of scale_min/scale_max/offset_min/offset_max/rotation_min/rotation_max)")

def coerce(prop_name, raw_value):
    # Mirror pcg_set_node_settings_property: tolerate JSON-string payloads and
    # coerce list/dict into the Vector / Rotator the property expects.
    if isinstance(raw_value, str):
        s = raw_value.strip()
        if s[:1] in "[{":
            try:
                raw_value = json.loads(s)
            except Exception:
                pass
    if isinstance(raw_value, (list, dict)):
        try:
            cur = settings.get_editor_property(prop_name)
        except Exception:
            cur = None
        if isinstance(cur, unreal.Vector):
            if isinstance(raw_value, dict):
                return unreal.Vector(float(raw_value.get("x", 0.0)), float(raw_value.get("y", 0.0)), float(raw_value.get("z", 0.0)))
            v = [float(x) for x in raw_value] + [0.0, 0.0, 0.0]
            return unreal.Vector(v[0], v[1], v[2])
        if isinstance(cur, unreal.Rotator):
            r = unreal.Rotator()
            if isinstance(raw_value, dict):
                r.roll = float(raw_value.get("roll", 0.0)); r.pitch = float(raw_value.get("pitch", 0.0)); r.yaw = float(raw_value.get("yaw", 0.0))
            else:
                v = [float(x) for x in raw_value] + [0.0, 0.0, 0.0]
                r.pitch = v[0]; r.yaw = v[1]; r.roll = v[2]
            return r
    return raw_value

applied = {}
for prop_name in ("scale_min", "scale_max", "offset_min", "offset_max", "rotation_min", "rotation_max"):
    if prop_name not in ranges:
        continue
    try:
        settings.set_editor_property(prop_name, coerce(prop_name, ranges[prop_name]))
    except Exception as e:
        return ngg_error("set_editor_property failed for " + prop_name + ": " + str(e))
    try:
        applied[prop_name] = str(settings.get_editor_property(prop_name))
    except Exception:
        applied[prop_name] = ""

lib.save_asset(args["graph"])
ngg_result({"ok": True, "graph": args["graph"], "node": args["node"], "applied": applied})
`);
  return mcpResponse(result);
}

// ---------------------------------------------------------------------------
// Endpoint: pcg_set_node_position — move a node in the editor canvas.
// ---------------------------------------------------------------------------

export async function pcgSetNodePosition({ graph_path, node, x, y }) {
  const payload = JSON.stringify({ graph: graph_path, node, x, y });
  const result = await runPy(`
${pyArgs(payload)}
lib = unreal.EditorAssetLibrary
graph = lib.load_asset(args["graph"])
if not isinstance(graph, unreal.PCGGraph):
    return ngg_error("not a PCGGraph asset: " + args["graph"])

${PY_RESOLVE_NODE}

n = resolve(args["node"])
if n is None:
    return ngg_error("node not found: " + args["node"])

x = int(args["x"])
y = int(args["y"])

if hasattr(n, "set_node_position"):
    try:
        n.set_node_position(x, y)
    except Exception as e:
        return ngg_error("set_node_position failed: " + str(e))
else:
    # Last-resort path for older engine versions.
    try:
        n.set_editor_property("position_x", x)
        n.set_editor_property("position_y", y)
    except Exception as e:
        return ngg_error("no set_node_position and editor-property fallback failed: " + str(e))

lib.save_asset(args["graph"])
ngg_result({"ok": True, "node": args["node"], "x": x, "y": y})
`);
  return mcpResponse(result);
}

// ---------------------------------------------------------------------------
// Endpoint: pcg_remove_node — delete a node from a PCG graph.
// ---------------------------------------------------------------------------

export async function pcgRemoveNode({ graph_path, node }) {
  const payload = JSON.stringify({ graph: graph_path, node });
  const result = await runPy(`
${pyArgs(payload)}
lib = unreal.EditorAssetLibrary
graph = lib.load_asset(args["graph"])
if not isinstance(graph, unreal.PCGGraph):
    return ngg_error("not a PCGGraph asset: " + args["graph"])

${PY_RESOLVE_NODE}

n = resolve(args["node"])
if n is None:
    return ngg_error("node not found: " + args["node"])
if args["node"] in ("__input__", "__output__", "InputNode", "OutputNode", "Input", "Output"):
    return ngg_error("cannot remove the graph input/output node")

graph.remove_node(n)
lib.save_asset(args["graph"])
ngg_result({"ok": True, "graph": args["graph"], "removed": args["node"]})
`);
  return mcpResponse(result);
}

// ---------------------------------------------------------------------------
// Endpoint: pcg_disconnect_pins — remove a directed edge from a PCG graph.
// ---------------------------------------------------------------------------

export async function pcgDisconnectPins({ graph_path, from_node, from_pin, to_node, to_pin }) {
  const payload = JSON.stringify({
    graph: graph_path,
    from_node, from_pin, to_node, to_pin,
  });
  const result = await runPy(`
${pyArgs(payload)}
lib = unreal.EditorAssetLibrary
graph = lib.load_asset(args["graph"])
if not isinstance(graph, unreal.PCGGraph):
    return ngg_error("not a PCGGraph asset: " + args["graph"])

${PY_RESOLVE_NODE}

frm = resolve(args["from_node"])
to  = resolve(args["to_node"])
if frm is None:
    return ngg_error("from_node not found: " + args["from_node"])
if to is None:
    return ngg_error("to_node not found: " + args["to_node"])

ok = graph.remove_edge(frm, unreal.Name(args["from_pin"]), to, unreal.Name(args["to_pin"]))
lib.save_asset(args["graph"])
ngg_result({"ok": bool(ok), "graph": args["graph"]})
`);
  return mcpResponse(result);
}

// ---------------------------------------------------------------------------
// Endpoint: pcg_rename_node — change a node's display title.
// ---------------------------------------------------------------------------

export async function pcgRenameNode({ graph_path, node, new_title }) {
  const payload = JSON.stringify({ graph: graph_path, node, title: new_title });
  const result = await runPy(`
${pyArgs(payload)}
lib = unreal.EditorAssetLibrary
graph = lib.load_asset(args["graph"])
if not isinstance(graph, unreal.PCGGraph):
    return ngg_error("not a PCGGraph asset: " + args["graph"])

${PY_RESOLVE_NODE}

n = resolve(args["node"])
if n is None:
    return ngg_error("node not found: " + args["node"])

try:
    n.set_editor_property("node_title", unreal.Name(args["title"]))
except Exception as e:
    return ngg_error("set node_title failed: " + str(e))

lib.save_asset(args["graph"])
ngg_result({"ok": True, "node": args["node"], "title": args["title"]})
`);
  return mcpResponse(result);
}

// ---------------------------------------------------------------------------
// Endpoint: pcg_add_component_to_actor — attach a UPCGComponent to a level
// actor and bind a graph to it. The actor is found by label (display name)
// in the current edited world.
// ---------------------------------------------------------------------------

export async function pcgAddComponentToActor({ actor_label, graph_path }) {
  const payload = JSON.stringify({ actor: actor_label, graph: graph_path });
  const result = await runPy(`
${pyArgs(payload)}

# Find the actor by label in the current edited world.
eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
actors = eas.get_all_level_actors()
target = None
for a in actors:
    try:
        if a.get_actor_label() == args["actor"]:
            target = a
            break
    except Exception:
        continue
if target is None:
    return ngg_error("actor not found by label: " + args["actor"])

# Load the graph asset.
lib = unreal.EditorAssetLibrary
graph = lib.load_asset(args["graph"])
if graph is None:
    return ngg_error("graph asset not found: " + args["graph"])

# Add or reuse UPCGComponent on the actor.
pcg_cls = unreal.PCGComponent
existing = target.get_component_by_class(pcg_cls)
comp = existing
created = False
if comp is None:
    # add_component_by_class is Blueprint-only and not exposed on level actors.
    # Use new_object + add_instance_component, which is sufficient to make the
    # component reachable via get_component_by_class and to call set_graph /
    # generate / cleanup. Some UE 5.x Python builds don't expose
    # register_component on UPCGComponent — try it via reflection, but don't
    # treat a missing binding as a hard failure: the previous calls already
    # made the component live for the editor.
    comp = unreal.new_object(pcg_cls, outer=target)
    if comp is None:
        return ngg_error("new_object(PCGComponent) returned None")
    try:
        target.add_instance_component(comp)
    except Exception:
        pass
    created = True
    for m in ("register_component", "k2_register_component"):
        if hasattr(comp, m):
            try:
                getattr(comp, m)()
                break
            except Exception:
                continue
    # If register_component is unavailable, the component still functions for
    # property edits and graph binding; full registration happens on the next
    # editor reload or world tick.

comp.set_graph(graph)

# Verify the binding actually stuck. A freshly created (and not yet fully
# registered) component, or a graph whose reference was nulled by an earlier
# force-delete, can silently leave graph_instance.graph == None — which makes
# every later generate() produce nothing with no error. Read it back and, if
# empty, re-bind directly on the graph instance, then report what we ended up
# with so callers can detect a failed bind instead of debugging blind.
def _bound_graph():
    try:
        gi = comp.get_editor_property("graph_instance")
        return gi.get_editor_property("graph") if gi is not None else None
    except Exception:
        return None

bound = _bound_graph()
if bound is None:
    gi = None
    try:
        gi = comp.get_editor_property("graph_instance")
    except Exception:
        gi = None
    if gi is not None:
        try:
            gi.set_graph(graph)
        except Exception:
            pass
    try:
        comp.set_graph(graph)
    except Exception:
        pass
    bound = _bound_graph()

# Mark the component and its owner dirty so the binding is serialized with the
# level on the next save (instance components added from Python don't always
# dirty the package on their own).
for obj in (comp, target):
    try:
        obj.modify()
    except Exception:
        pass

bound_name = str(bound.get_name()) if bound is not None else None
ngg_result({
    "ok": bound_name is not None,
    "actor": args["actor"],
    "graph": args["graph"],
    "bound_graph": bound_name,
    "component_name": str(comp.get_name()),
    "reused_existing": existing is not None,
    "created": created,
    "warning": None if bound_name is not None else "graph binding is empty after set_graph — generation will produce nothing",
})
`);
  return mcpResponse(result);
}

// ---------------------------------------------------------------------------
// Endpoint: pcg_set_component_property — set a UPROPERTY on the PCGComponent
// of a level actor. Use this for seed, generation_trigger, is_component_partitioned,
// activated, regenerate_in_editor, etc.
// ---------------------------------------------------------------------------

export async function pcgSetComponentProperty({ actor_label, property, value }) {
  const payload = JSON.stringify({
    actor: actor_label, property,
    value: value === undefined ? null : value,
  });
  const result = await runPy(`
${pyArgs(payload)}
eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
target = None
for a in eas.get_all_level_actors():
    try:
        if a.get_actor_label() == args["actor"]:
            target = a
            break
    except Exception:
        continue
if target is None:
    return ngg_error("actor not found by label: " + args["actor"])

comp = target.get_component_by_class(unreal.PCGComponent)
if comp is None:
    return ngg_error("actor has no PCGComponent: " + args["actor"])

prop = args["property"]
val  = args["value"]

# Convert string enums like "GenerateOnLoad" to unreal.PCGComponentGenerationTrigger.GENERATE_ON_LOAD
# when the property looks like a known enum field.
def coerce(p, v):
    if not isinstance(v, str):
        return v
    if p == "generation_trigger":
        m = {
            "GenerateOnLoad":   unreal.PCGComponentGenerationTrigger.GENERATE_ON_LOAD,
            "GenerateOnDemand": unreal.PCGComponentGenerationTrigger.GENERATE_ON_DEMAND,
            "GenerateAtRuntime":getattr(unreal.PCGComponentGenerationTrigger, "GENERATE_AT_RUNTIME", None),
        }
        if v in m and m[v] is not None:
            return m[v]
    if p == "input_type":
        m = {
            "Actor":     getattr(unreal.PCGComponentInput, "ACTOR", None),
            "Landscape": getattr(unreal.PCGComponentInput, "LANDSCAPE", None),
            "Other":     getattr(unreal.PCGComponentInput, "OTHER", None),
        }
        if v in m and m[v] is not None:
            return m[v]
    # The MCP layer delivers every 'value' as a string, so coerce it to the type
    # the property actually expects — otherwise bools/ints/floats fail to nativize
    # (e.g. 'regenerate_in_editor' = "true" -> True).
    s = v.strip()
    if s[:1] in "[{":
        try:
            return json.loads(s)
        except Exception:
            pass
    try:
        cur = comp.get_editor_property(p)
    except Exception:
        cur = None
    if isinstance(cur, bool):
        if s.lower() in ("true", "1", "yes", "on"):  return True
        if s.lower() in ("false", "0", "no", "off"): return False
    elif isinstance(cur, int):  # bool already handled above
        try: return int(s)
        except Exception: pass
    elif isinstance(cur, float):
        try: return float(s)
        except Exception: pass
    return v

try:
    comp.set_editor_property(prop, coerce(prop, val))
except Exception as e:
    return ngg_error("set_editor_property failed for " + prop + ": " + str(e))

# Echo current value.
try:
    confirmed = str(comp.get_editor_property(prop))
except Exception:
    confirmed = ""

ngg_result({"ok": True, "actor": args["actor"], "property": prop, "value": confirmed})
`);
  return mcpResponse(result);
}

// ---------------------------------------------------------------------------
// Endpoint: pcg_generate_component — call generate() on the PCGComponent of
// a level actor.
// ---------------------------------------------------------------------------

export async function pcgGenerateComponent({ actor_label, force = true }) {
  const payload = JSON.stringify({ actor: actor_label, force: Boolean(force) });
  const result = await runPy(`
${pyArgs(payload)}
eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
target = None
for a in eas.get_all_level_actors():
    try:
        if a.get_actor_label() == args["actor"]:
            target = a
            break
    except Exception:
        continue
if target is None:
    return ngg_error("actor not found by label: " + args["actor"])

comp = target.get_component_by_class(unreal.PCGComponent)
if comp is None:
    return ngg_error("actor has no PCGComponent: " + args["actor"])

# A component with no bound graph generates nothing but reports no error — the
# single biggest source of silent "generate did nothing" confusion. Surface it.
bound_name = None
try:
    gi = comp.get_editor_property("graph_instance")
    g = gi.get_editor_property("graph") if gi is not None else None
    bound_name = str(g.get_name()) if g is not None else None
except Exception:
    bound_name = None

comp.generate(bool(args["force"]))

# Report how many instanced-mesh instances now exist on the owner so callers
# get immediate feedback (generation is async, so a 0 here right after the
# call may just mean "not finished yet" — but a persistent 0 with an empty
# bound_graph is the real failure mode).
instance_count = 0
try:
    for c in target.get_components_by_class(unreal.InstancedStaticMeshComponent):
        instance_count += c.get_instance_count()
except Exception:
    pass

ngg_result({
    "ok": True,
    "actor": args["actor"],
    "forced": bool(args["force"]),
    "bound_graph": bound_name,
    "instance_count": instance_count,
    "note": "PCG editor generation is asynchronous: instance_count is sampled immediately and may read 0 (or a stale value) until generation finishes on a later tick. Re-query instance counts in a separate call to see the final result. A persistent 0 with a non-null bound_graph usually means a scripted spline edit did not dirty the component — call pcg_cleanup_component then generate again, or edit the spline in the viewport.",
    "warning": None if bound_name is not None else "no graph bound to this PCGComponent — generation will produce nothing (call pcg_add_component_to_actor)",
})
`);
  return mcpResponse(result);
}

// ---------------------------------------------------------------------------
// Endpoint: pcg_cleanup_component — call cleanup() on the PCGComponent.
// ---------------------------------------------------------------------------

export async function pcgCleanupComponent({ actor_label, remove_components = true }) {
  const payload = JSON.stringify({ actor: actor_label, rm: Boolean(remove_components) });
  const result = await runPy(`
${pyArgs(payload)}
eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
target = None
for a in eas.get_all_level_actors():
    try:
        if a.get_actor_label() == args["actor"]:
            target = a
            break
    except Exception:
        continue
if target is None:
    return ngg_error("actor not found by label: " + args["actor"])

comp = target.get_component_by_class(unreal.PCGComponent)
if comp is None:
    return ngg_error("actor has no PCGComponent: " + args["actor"])

comp.cleanup(bool(args["rm"]))
ngg_result({"ok": True, "actor": args["actor"], "removed_components": bool(args["rm"])})
`);
  return mcpResponse(result);
}

// ---------------------------------------------------------------------------
// Endpoint: pcg_set_graph_property — set a top-level property on a UPCGGraph
// (description, category, is_editor_only, ignore_landscape_tracking, etc.).
// ---------------------------------------------------------------------------

export async function pcgSetGraphProperty({ graph_path, property, value }) {
  const payload = JSON.stringify({
    graph: graph_path, property,
    value: value === undefined ? null : value,
  });
  const result = await runPy(`
${pyArgs(payload)}
lib = unreal.EditorAssetLibrary
graph = lib.load_asset(args["graph"])
if not isinstance(graph, unreal.PCGGraph):
    return ngg_error("not a PCGGraph asset: " + args["graph"])

prop = args["property"]
val  = args["value"]

# Auto-wrap strings as Text for Text-typed properties.
if prop in ("description", "category") and isinstance(val, str):
    val = unreal.Text(val)

try:
    graph.set_editor_property(prop, val)
except Exception as e:
    return ngg_error("set_editor_property failed for " + prop + ": " + str(e))

lib.save_asset(args["graph"])

try:
    confirmed = str(graph.get_editor_property(prop))
except Exception:
    confirmed = ""

ngg_result({"ok": True, "graph": args["graph"], "property": prop, "value": confirmed})
`);
  return mcpResponse(result);
}

// ---------------------------------------------------------------------------
// Graph user parameters — UPCGGraph stores an FInstancedPropertyBag under
// 'user_parameters'. We expose add/list/remove. The bag's Python surface
// varies across UE versions, so we try several method names with graceful
// fallback to set_editor_property.
// ---------------------------------------------------------------------------

const PY_USER_PARAMS_BAG = `
def get_bag(graph):
    for attr in ("user_parameters", "graph_user_parameters", "instanced_property_bag"):
        try:
            bag = graph.get_editor_property(attr)
            if bag is not None:
                return bag, attr
        except Exception:
            continue
    return None, None
`;

// Map of friendly type names → unreal.PropertyBagPropertyType (5.4+).
// Used by pcg_add_graph_parameter so callers don't need to know enum casing.
const PY_PARAM_TYPE_MAP = `
def param_type(name):
    if not hasattr(unreal, "PropertyBagPropertyType"):
        return None
    pt = unreal.PropertyBagPropertyType
    m = {
        "bool":     getattr(pt, "BOOL",   None),
        "int":      getattr(pt, "INT32",  None) or getattr(pt, "INT", None),
        "int32":    getattr(pt, "INT32",  None),
        "int64":    getattr(pt, "INT64",  None),
        "float":    getattr(pt, "FLOAT",  None),
        "double":   getattr(pt, "DOUBLE", None),
        "name":     getattr(pt, "NAME",   None),
        "string":   getattr(pt, "STRING", None),
        "text":     getattr(pt, "TEXT",   None),
        "vector":   getattr(pt, "STRUCT", None),
        "rotator":  getattr(pt, "STRUCT", None),
        "transform":getattr(pt, "STRUCT", None),
        "object":   getattr(pt, "OBJECT", None),
        "soft_object": getattr(pt, "SOFT_OBJECT", None),
    }
    return m.get(name.lower())
`;

// ---------------------------------------------------------------------------
// Endpoint: pcg_add_graph_parameter — add a user parameter to a PCG graph.
// ---------------------------------------------------------------------------

export async function pcgAddGraphParameter({ graph_path, name, type }) {
  const payload = JSON.stringify({ graph: graph_path, name, type });
  const result = await runPy(`
${pyArgs(payload)}
lib = unreal.EditorAssetLibrary
graph = lib.load_asset(args["graph"])
if not isinstance(graph, unreal.PCGGraph):
    return ngg_error("not a PCGGraph asset: " + args["graph"])

${PY_USER_PARAMS_BAG}
${PY_PARAM_TYPE_MAP}

bag, attr = get_bag(graph)
if bag is None:
    return ngg_error("graph has no user_parameters bag (UE version too old?)")

ptype = param_type(args["type"])
if ptype is None:
    return ngg_error("unknown parameter type: " + args["type"] + " (try bool/int/float/name/string/object)")

added = False
for method in ("add_property", "add_property_by_name"):
    if hasattr(bag, method):
        try:
            getattr(bag, method)(unreal.Name(args["name"]), ptype)
            added = True
            break
        except Exception as e:
            return ngg_error(method + " failed: " + str(e))
if not added:
    # UE 5.5 / 5.7 do NOT expose FInstancedPropertyBag::AddProperty to Python:
    # the only struct methods available are assign / copy / import_text /
    # export_text. New user parameters cannot be created from Python in this
    # build — they must be added via the PCG graph editor UI (Details panel →
    # User Parameters → +). Return a clearly-typed error so callers can guide
    # the user.
    return ngg_error(
        "user-parameter creation is not supported by this UE build's Python "
        "API (FInstancedPropertyBag lacks add_property bindings). Add the "
        "parameter via the PCG editor UI (Details panel → User Parameters → +), "
        "then use pcg_set_graph_instance_parameter to override its value."
    )

# Push the mutated bag back onto the graph if the attribute is read-write.
try:
    graph.set_editor_property(attr, bag)
except Exception:
    pass

lib.save_asset(args["graph"])
ngg_result({"ok": True, "graph": args["graph"], "name": args["name"], "type": args["type"]})
`);
  return mcpResponse(result);
}

// ---------------------------------------------------------------------------
// Endpoint: pcg_list_graph_parameters — list the user parameters on a graph.
// ---------------------------------------------------------------------------

export async function pcgListGraphParameters({ graph_path }) {
  const payload = JSON.stringify({ graph: graph_path });
  const result = await runPy(`
${pyArgs(payload)}
lib = unreal.EditorAssetLibrary
graph = lib.load_asset(args["graph"])
if not isinstance(graph, unreal.PCGGraph):
    return ngg_error("not a PCGGraph asset: " + args["graph"])

${PY_USER_PARAMS_BAG}
bag, attr = get_bag(graph)
if bag is None:
    ngg_result({"ok": True, "graph": args["graph"], "parameters": [], "note": "no user_parameters bag"})
    return

params = []

# Preferred path (newer engines): typed descriptor accessors.
desc_list = None
for getter in ("get_property_descs", "property_descs", "property_descriptors", "get_value_property_descs"):
    if hasattr(bag, getter):
        try:
            attr_val = getattr(bag, getter)
            desc_list = attr_val() if callable(attr_val) else attr_val
            if desc_list is not None:
                break
        except Exception:
            continue

if desc_list:
    for d in desc_list:
        try:
            params.append({
                "name": str(d.get_editor_property("name")),
                "type": str(d.get_editor_property("value_type")),
            })
        except Exception:
            try:
                params.append({"name": str(d), "type": ""})
            except Exception:
                continue

# Fallback: parse export_text, which serializes the bag including each user
# parameter's name and type. The format looks like:
#   (PropertyDescs=((Name="Density",ValueType=Float,...),(Name="IsActive",ValueType=Bool,...)),...)
if not params and hasattr(bag, "export_text"):
    import re
    try:
        text = bag.export_text()
    except Exception:
        text = ""
    for m in re.finditer(r'Name\s*=\s*"([^"]+)"[^,)]*(?:,\s*ValueType\s*=\s*([A-Za-z0-9_]+))?', text or ""):
        params.append({"name": m.group(1), "type": m.group(2) or ""})

ngg_result({"ok": True, "graph": args["graph"], "parameters": params, "count": len(params)})
`);
  return mcpResponse(result);
}

// ---------------------------------------------------------------------------
// Endpoint: pcg_remove_graph_parameter — remove a user parameter by name.
// ---------------------------------------------------------------------------

export async function pcgRemoveGraphParameter({ graph_path, name }) {
  const payload = JSON.stringify({ graph: graph_path, name });
  const result = await runPy(`
${pyArgs(payload)}
lib = unreal.EditorAssetLibrary
graph = lib.load_asset(args["graph"])
if not isinstance(graph, unreal.PCGGraph):
    return ngg_error("not a PCGGraph asset: " + args["graph"])

${PY_USER_PARAMS_BAG}
bag, attr = get_bag(graph)
if bag is None:
    return ngg_error("graph has no user_parameters bag")

removed = False
for method in ("remove_property_by_name", "remove_property"):
    if hasattr(bag, method):
        try:
            getattr(bag, method)(unreal.Name(args["name"]))
            removed = True
            break
        except Exception as e:
            return ngg_error(method + " failed: " + str(e))
if not removed:
    return ngg_error(
        "user-parameter removal is not supported by this UE build's Python "
        "API (FInstancedPropertyBag lacks remove_property bindings). Remove "
        "the parameter via the PCG editor UI (Details panel → User Parameters)."
    )

try:
    graph.set_editor_property(attr, bag)
except Exception:
    pass

lib.save_asset(args["graph"])
ngg_result({"ok": True, "graph": args["graph"], "removed": args["name"]})
`);
  return mcpResponse(result);
}

// ---------------------------------------------------------------------------
// Endpoint: pcg_create_graph_instance — create a UPCGGraphInstance asset
// that references a base UPCGGraph and can override its user parameters.
// ---------------------------------------------------------------------------

export async function pcgCreateGraphInstance({ asset_path, base_graph, overwrite = false }) {
  const payload = JSON.stringify({ path: asset_path, base: base_graph, overwrite });
  const result = await runPy(`
${pyArgs(payload)}
full = args["path"]
if "/" not in full:
    return ngg_error("asset_path must be a content-browser path like /Game/PCG/MyGraphInst")
dir_path, name = full.rsplit("/", 1)

lib = unreal.EditorAssetLibrary
tools = unreal.AssetToolsHelpers.get_asset_tools()

base = lib.load_asset(args["base"])
if not isinstance(base, unreal.PCGGraph):
    return ngg_error("base_graph is not a PCGGraph asset: " + args["base"])

# Overwrite REUSES an existing PCGGraphInstance in place (just rebinds the base
# graph) rather than deleting it — deleting referenced assets goes through
# ForceDeleteObjects, which ensures/crashes in unattended mode. See pcg_create_graph.
inst = None
reused = False
if lib.does_asset_exist(full):
    if not args.get("overwrite"):
        return ngg_error("asset already exists: " + full + " (pass overwrite=True to replace)")
    existing = lib.load_asset(full)
    if isinstance(existing, unreal.PCGGraphInstance):
        inst = existing
        reused = True
    else:
        return ngg_error("a non-PCGGraphInstance asset already exists at " + full + " — choose a different path or delete it manually")

if inst is None:
    # Some engine versions ship a dedicated factory; fall back to plain create_asset.
    factory = None
    try:
        factory = unreal.PCGGraphInstanceFactory()
    except Exception:
        pass
    if factory is not None:
        inst = tools.create_asset(name, dir_path, unreal.PCGGraphInstance, factory)
    else:
        inst = tools.create_asset(name, dir_path, unreal.PCGGraphInstance, unreal.PCGGraphFactory())
    if inst is None:
        return ngg_error("create_asset returned None for PCGGraphInstance")

# Bind the base graph. The 'graph' property is documented as read-only in
# Python, but set_editor_property on UPROPERTY-marked fields normally works
# regardless of the Python-side flag.
try:
    inst.set_editor_property("graph", base)
except Exception as e:
    return ngg_error("could not bind base graph: " + str(e))

lib.save_asset(full)
ngg_result({"ok": True, "path": full, "base_graph": args["base"], "reused": reused})
`);
  return mcpResponse(result);
}

// ---------------------------------------------------------------------------
// Endpoint: pcg_set_graph_instance_parameter — set an override value for a
// user parameter on a UPCGGraphInstance.
// ---------------------------------------------------------------------------

export async function pcgSetGraphInstanceParameter({ instance_path, name, value, asset_value }) {
  const payload = JSON.stringify({
    inst: instance_path, name,
    value: value === undefined ? null : value,
    asset_value: asset_value ?? null,
  });
  const result = await runPy(`
${pyArgs(payload)}
lib = unreal.EditorAssetLibrary
inst = lib.load_asset(args["inst"])
if not isinstance(inst, unreal.PCGGraphInstance):
    return ngg_error("not a PCGGraphInstance asset: " + args["inst"])

# Resolve the override bag (PCGOverrideInstancedPropertyBag).
bag = None
for attr in ("parameters_overrides", "parameters_override"):
    try:
        bag = inst.get_editor_property(attr)
        if bag is not None:
            break
    except Exception:
        continue
if bag is None:
    return ngg_error("graph instance has no parameters_overrides bag")

# The override bag itself is a wrapper; the actual FInstancedPropertyBag lives
# under a 'parameters' or 'value' field on it. Try a few names.
inner = bag
for attr in ("parameters", "value", "property_bag"):
    try:
        candidate = bag.get_editor_property(attr)
        if candidate is not None:
            inner = candidate
            break
    except Exception:
        continue

new_value = args.get("value")
if args.get("asset_value"):
    loaded = lib.load_asset(args["asset_value"])
    if loaded is None:
        return ngg_error("asset_value failed to load: " + args["asset_value"])
    new_value = loaded

ok = False
for method in ("set_value_serialized_string",):
    pass

# Try typed setters in order. Each returns False if the type doesn't match.
typed_setters = (
    "set_value_bool", "set_value_int32", "set_value_int64",
    "set_value_float", "set_value_double", "set_value_name",
    "set_value_string", "set_value_text", "set_value_object",
)
for setter in typed_setters:
    if hasattr(inner, setter):
        try:
            r = getattr(inner, setter)(unreal.Name(args["name"]), new_value)
            # Only an explicit True counts as applied. A None return is
            # ambiguous (often a type mismatch / no-op) and was previously
            # masking failures as success, so don't treat it as applied.
            # TODO: read back the value to positively confirm the write when
            # the bag exposes a typed getter for this parameter.
            if r is True:
                ok = True
                break
        except Exception:
            continue

# Fallback: mark override flag and use generic set_editor_property. Unlike the
# typed setters, set_editor_property returns nothing and silently no-ops when
# the property name doesn't exist on the bag, so a clean call is NOT proof the
# value was applied. Read the value back and only claim success if it actually
# matches what we asked for; otherwise report the write as unverified.
confirmed = False
verified = False  # whether we were able to read the value back at all
if not ok:
    try:
        inner.set_editor_property(args["name"], new_value)
    except Exception as e:
        return ngg_error("could not set parameter override: " + str(e))
    # Read-back confirmation.
    try:
        read_back = inner.get_editor_property(args["name"])
        verified = True
        confirmed = (read_back == new_value)
    except Exception:
        # Property isn't readable this way — can't confirm. Leave verified False.
        verified = False
    ok = confirmed

# Mark the override as active so the instance actually applies it.
for marker in ("override_parameter", "set_parameter_override_active"):
    if hasattr(bag, marker):
        try:
            getattr(bag, marker)(unreal.Name(args["name"]), True)
        except Exception:
            pass

lib.save_asset(args["inst"])
if ok:
    ngg_result({"ok": True, "instance": args["inst"], "name": args["name"]})
else:
    # Generic fallback ran without raising, but the value could not be confirmed
    # applied (read-back differed or the property was unreadable). Signal this so
    # the JS side and caller don't mistake a silent no-op for a real write.
    note = ("set_editor_property did not change the value (read-back differs) — "
            "the parameter may not exist on this bag or expects a different type"
            if verified else
            "set_editor_property ran but the value could not be read back to confirm it was applied")
    ngg_result({
        "ok": False,
        "confirmed": False,
        "instance": args["inst"],
        "name": args["name"],
        "warning": "unverified: change not confirmed applied",
        "note": note,
    })
`);
  return mcpResponse(result);
}

// ---------------------------------------------------------------------------
// Endpoint: pcg_open_in_editor — open a PCG graph in the editor window.
// ---------------------------------------------------------------------------

export async function pcgOpenInEditor({ graph_path }) {
  const payload = JSON.stringify({ graph: graph_path });
  const result = await runPy(`
${pyArgs(payload)}
lib = unreal.EditorAssetLibrary
asset = lib.load_asset(args["graph"])
if asset is None:
    return ngg_error("asset not found: " + args["graph"])

eus = unreal.get_editor_subsystem(unreal.AssetEditorSubsystem)
ok = eus.open_editor_for_assets([asset])
ngg_result({"ok": bool(ok), "graph": args["graph"]})
`);
  return mcpResponse(result);
}

// ---------------------------------------------------------------------------
// Endpoint: pcg_regenerate_all — regenerate every PCGComponent in the level.
// Optionally filter by graph asset path so only components bound to that
// graph are refreshed.
// ---------------------------------------------------------------------------

export async function pcgRegenerateAll({ graph_filter, force = true } = {}) {
  const payload = JSON.stringify({ filter: graph_filter ?? null, force: Boolean(force) });
  const result = await runPy(`
${pyArgs(payload)}
lib = unreal.EditorAssetLibrary
filt = args.get("filter")

filter_asset = None
if filt:
    filter_asset = lib.load_asset(filt)
    if filter_asset is None:
        return ngg_error("graph_filter asset not found: " + filt)

eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
regenerated = []
for a in eas.get_all_level_actors():
    comp = a.get_component_by_class(unreal.PCGComponent)
    if comp is None:
        continue
    if filter_asset is not None:
        try:
            inst = comp.get_editor_property("graph_instance")
            bound_graph = inst.get_editor_property("graph") if inst is not None else None
        except Exception:
            bound_graph = None
        if bound_graph is None or bound_graph.get_path_name() != filter_asset.get_path_name():
            continue
    try:
        comp.cleanup(True)
    except Exception:
        pass
    try:
        comp.generate(bool(args["force"]))
        regenerated.append(a.get_actor_label())
    except Exception:
        continue

ngg_result({"ok": True, "count": len(regenerated), "actors": regenerated})
`);
  return mcpResponse(result);
}
