// Copyright 2025-2026 NGG. All Rights Reserved.
// metasound.js — unrealngg-mcp
//
// MetaSound authoring: build a Source or Patch graph and save it as an asset,
// read back what the API exposes, and retune an existing graph's inputs.
//
// Runs through POST /editor/exec_python, like sequencer.js and worldpartition.js.
//
// Everything below was established by probing a live UE 5.8.1 editor first —
// the MetaSound builder API has several non-obvious edges:
//
//   * A node is identified by a MetasoundFrontendClassName struct of
//     {namespace, name, variant}. The variant is NOT decoration: audio-rate
//     nodes use "Audio" (StandardNodes::AudioVariant), trigger/flow nodes use
//     an empty variant. Getting it wrong returns FAILED with no explanation,
//     so `variant` is optional here and both are tried.
//   * There is NO way to enumerate the node registry from Python. Nothing can
//     list "all available nodes", which is why add-node errors below spell out
//     exactly what was tried and where the real names come from.
//   * There is likewise no way to enumerate the nodes already IN a graph — the
//     builder exposes graph inputs/outputs but not its node set. That is why a
//     whole graph is built in one call (the same shape as ue5_bp_add_logic)
//     rather than node-by-node across calls, and why ue5_metasound_read is
//     explicit about what it cannot see.
//   * create_source_builder returns a 5-tuple: the builder plus the handles for
//     On Play, On Finished, and the audio output pins.
//   * Builders live in the subsystem, not the asset. The asset only exists once
//     MetaSoundEditorSubsystem.build_to_asset runs.

import * as ue5 from "./ue5client.js";

const RESULT_BEGIN = "__NGG_MS_RESULT_BEGIN__";
const RESULT_END   = "__NGG_MS_RESULT_END__";

const PY_PRELUDE = `
import unreal, json, base64, traceback

def ngg_result(obj):
    print("${RESULT_BEGIN}" + json.dumps(obj) + "${RESULT_END}")

def ngg_error(msg):
    ngg_result({"ok": False, "error": msg})

def s(v):
    return None if v is None else str(v)

def ok_result(r):
    """MetaSoundBuilderResult.SUCCEEDED is 0; compare on the enum, not a string."""
    try:
        return r == unreal.MetaSoundBuilderResult.SUCCEEDED
    except Exception:
        return "SUCCEEDED" in str(r)

def builder_subsystem():
    return unreal.get_engine_subsystem(unreal.MetaSoundBuilderSubsystem)

def editor_subsystem():
    return unreal.get_editor_subsystem(unreal.MetaSoundEditorSubsystem)

def class_name(namespace, name, variant):
    cn = unreal.MetasoundFrontendClassName()
    cn.set_editor_property("namespace", namespace)
    cn.set_editor_property("name", name)
    cn.set_editor_property("variant", variant)
    return cn

# Variants actually used by the shipped node set: audio-rate nodes carry
# "Audio", trigger/flow/maths nodes carry none. Callers may pin one explicitly.
VARIANTS_TO_TRY = ["Audio", ""]

def add_node(builder, spec_class, explicit_variant, major_version):
    \"\"\"Add a node, trying the known variants when one wasn't pinned.

    Returns (node_handle, used_variant, tried_list) or (None, None, tried_list).
    \"\"\"
    if "." in spec_class:
        namespace, short = spec_class.rsplit(".", 1)
    else:
        namespace, short = "UE", spec_class

    variants = [explicit_variant] if explicit_variant is not None else VARIANTS_TO_TRY
    tried = []
    for variant in variants:
        cn = class_name(namespace, short, variant)
        node, res = builder.add_node_by_class_name(cn, major_version)
        tried.append("%s/%s/%s" % (namespace, short, variant if variant else "<none>"))
        if ok_result(res):
            return node, variant, tried
    return None, None, tried

def node_input_names(builder, node):
    handles, _ = builder.find_node_inputs(node)
    return [str(builder.get_node_input_data(h)[0]) for h in handles]

def node_output_names(builder, node):
    handles, _ = builder.find_node_outputs(node)
    return [str(builder.get_node_output_data(h)[0]) for h in handles]

def literal_for(value):
    \"\"\"Wrap a JSON value as a MetaSound literal of the matching type.\"\"\"
    subs = builder_subsystem()
    if isinstance(value, bool):
        return subs.create_bool_meta_sound_literal(value)[0]
    if isinstance(value, int):
        return subs.create_int_meta_sound_literal(value)[0]
    if isinstance(value, float):
        return subs.create_float_meta_sound_literal(value)[0]
    if isinstance(value, str):
        return subs.create_string_meta_sound_literal(value)[0]
    return None
`;

function pyArgs(payload, varName = "args") {
  const jsonStr = typeof payload === "string" ? payload : JSON.stringify(payload);
  const b64 = Buffer.from(jsonStr, "utf8").toString("base64");
  return `${varName} = json.loads(base64.b64decode("${b64}").decode("utf-8"))`;
}

async function runPy(body) {
  const indented = body.split("\n").map((l) => "    " + l).join("\n");
  const script =
    PY_PRELUDE +
    "def _ngg_main():\n" + indented + "\n" +
    "try:\n    _ngg_main()\nexcept Exception as _e:\n" +
    "    ngg_error(str(_e) + '\\n' + traceback.format_exc())\n";

  let resp;
  try {
    resp = await ue5.execPython(script, "execute_file");
  } catch (err) {
    return { ok: false, error: `bridge: ${err.message}` };
  }

  for (const entry of resp.log ?? []) {
    const out = entry.output ?? "";
    const start = out.lastIndexOf(RESULT_BEGIN);
    const end = start >= 0 ? out.indexOf(RESULT_END, start + RESULT_BEGIN.length) : -1;
    if (start >= 0 && end > start) {
      try {
        const parsed = JSON.parse(out.slice(start + RESULT_BEGIN.length, end));
        if (parsed && parsed.ok === false) return { ok: false, error: parsed.error, raw: resp };
        return { ok: true, data: parsed, raw: resp };
      } catch (err) {
        return { ok: false, error: `bad result json: ${err.message}`, raw: resp };
      }
    }
  }

  const logStr = (resp.log ?? []).map((e) => `${e.type}: ${e.output}`).join("\n");
  return { ok: false, error: `no result sentinel; log:\n${logStr}`, raw: resp };
}

export function mcpResponse(result) {
  if (result.ok) {
    return { content: [{ type: "text", text: JSON.stringify(result.data, null, 2) }] };
  }
  return { content: [{ type: "text", text: result.error }], isError: true };
}

// ---------------------------------------------------------------------------
// Create
// ---------------------------------------------------------------------------

/**
 * Build a MetaSound Source or Patch and save it as an asset.
 *
 * The whole graph goes in one call because the builder API cannot enumerate the
 * nodes of an existing graph — there is no way to find a node again in a later
 * call. Nodes are addressed by caller-chosen `id`s within this request.
 *
 * @param {object}  opts
 * @param {string}  opts.asset_path
 * @param {string}  [opts.type="source"]      - "source" or "patch"
 * @param {string}  [opts.output_format="mono"] - "mono" or "stereo" (source only)
 * @param {boolean} [opts.one_shot=true]      - source stops itself (On Finished)
 * @param {Array}   [opts.nodes]              - [{id, class, variant?, version?, inputs?}]
 * @param {Array}   [opts.connections]        - [{from, to}], see below
 * @param {Array}   [opts.graph_inputs]       - [{name, type, default}]
 * @param {string}  [opts.author="NGG"]
 */
export async function metasoundCreate({
  asset_path, type, output_format, one_shot, nodes, connections, graph_inputs, author,
} = {}) {
  return runPy(`
${pyArgs({
  asset_path,
  type: (type || "source").toLowerCase(),
  output_format: (output_format || "mono").toLowerCase(),
  one_shot: one_shot !== false,
  nodes: nodes || [],
  connections: connections || [],
  graph_inputs: graph_inputs || [],
  author: author || "NGG",
})}
if not args.get("asset_path"):
    return ngg_error("asset_path is required")
if args["type"] not in ("source", "patch"):
    return ngg_error("type must be 'source' or 'patch'")
if args["output_format"] not in ("mono", "stereo"):
    return ngg_error("output_format must be 'mono' or 'stereo'")

lib = unreal.EditorAssetLibrary
if lib.does_asset_exist(args["asset_path"]):
    return ngg_error("an asset already exists at " + args["asset_path"] +
                     " (delete it first, or pick another path)")

subs = builder_subsystem()
ed = editor_subsystem()
if subs is None or ed is None:
    return ngg_error("MetaSound builder/editor subsystem unavailable")

asset_name = args["asset_path"].rsplit("/", 1)[-1]
package_path = args["asset_path"].rsplit("/", 1)[0]
builder_name = "NGG_" + asset_name

audio_outs = []
on_play_output = None
on_finished_input = None

if args["type"] == "source":
    fmt = (unreal.MetaSoundOutputAudioFormat.STEREO if args["output_format"] == "stereo"
           else unreal.MetaSoundOutputAudioFormat.MONO)
    created = subs.create_source_builder(builder_name, fmt, args["one_shot"])
    builder, on_play_output, on_finished_input, audio_outs, res = created
else:
    builder, res = subs.create_patch_builder(builder_name)

if builder is None or not ok_result(res):
    return ngg_error("could not create a %s builder (%s)" % (args["type"], s(res)))

# --- graph inputs (the parameters the sound exposes) ----------------------
declared_inputs = []
for gi in args["graph_inputs"]:
    name = gi.get("name")
    dtype = gi.get("type")
    if not name or not dtype:
        return ngg_error("every graph_input needs 'name' and 'type' (e.g. type 'Float', 'Bool', 'Trigger')")
    lit = literal_for(gi.get("default", 0.0))
    if lit is None:
        return ngg_error("unsupported default for graph input '" + str(name) + "'")
    handle, r = builder.add_graph_input_node(name, dtype, lit, False)
    if not ok_result(r):
        return ngg_error("could not add graph input '" + str(name) + "' of type '" + str(dtype) +
                         "'. Check the MetaSound data type name (Float, Bool, Int32, String, Trigger, Audio).")
    declared_inputs.append({"name": name, "type": dtype})

# --- nodes ----------------------------------------------------------------
handles = {}
report = []
for spec in args["nodes"]:
    nid = spec.get("id")
    cls = spec.get("class")
    if not nid or not cls:
        return ngg_error("every node needs 'id' and 'class' (e.g. {\\"id\\":\\"osc\\",\\"class\\":\\"Sine\\"})")
    if nid in handles:
        return ngg_error("duplicate node id: " + str(nid))

    node, used_variant, tried = add_node(builder, cls, spec.get("variant"), int(spec.get("version", 1)))
    if node is None:
        return ngg_error(
            "node class not found: '" + str(cls) + "'. Tried " + ", ".join(tried) + ". "
            "A MetaSound node is namespace/name/variant — audio-rate nodes use variant 'Audio', "
            "trigger and maths nodes use none. Names are the node's title in the MetaSound editor "
            "(e.g. 'Sine', 'Saw', 'Noise', 'Stereo Delay', 'Trigger Repeat'). "
            "There is no registry listing exposed to scripting, so the name must be exact.")

    handles[nid] = node
    entry = {
        "id": nid, "class": cls, "variant": used_variant,
        "inputs": node_input_names(builder, node),
        "outputs": node_output_names(builder, node),
    }

    # Per-node input defaults.
    for in_name, value in (spec.get("inputs") or {}).items():
        h, r = builder.find_node_input_by_name(node, in_name)
        if not ok_result(r):
            return ngg_error("node '" + str(nid) + "' has no input '" + str(in_name) +
                             "'. It exposes: " + ", ".join(entry["inputs"]))
        lit = literal_for(value)
        if lit is None:
            return ngg_error("unsupported value for " + str(nid) + "." + str(in_name))
        r2 = builder.set_node_input_default(h, lit)
        if not ok_result(r2):
            return ngg_error("could not set " + str(nid) + "." + str(in_name) + " (" + s(r2) + ")")
    report.append(entry)

# --- connections ----------------------------------------------------------
# "nodeId.PinName" on both sides; the right-hand side also accepts the special
# targets "audio_out" (the source's audio output) and "graph_output:Name".
def split_ref(ref):
    if "." not in ref:
        return None, None
    nid, pin = ref.split(".", 1)
    return nid, pin

made = []
for conn in args["connections"]:
    src = conn.get("from")
    dst = conn.get("to")
    if not src or not dst:
        return ngg_error("every connection needs 'from' and 'to'")

    snid, spin = split_ref(src)
    if snid is None or snid not in handles:
        return ngg_error("connection 'from' must be \\"nodeId.OutputName\\" of a node in this call; got '" +
                         str(src) + "'. Known nodes: " + (", ".join(handles.keys()) or "(none)"))
    out_h, r = builder.find_node_output_by_name(handles[snid], spin)
    if not ok_result(r):
        return ngg_error("node '" + snid + "' has no output '" + str(spin) + "'. It exposes: " +
                         ", ".join(node_output_names(builder, handles[snid])))

    if dst == "audio_out":
        if not audio_outs:
            return ngg_error("'audio_out' is only available on a source (type='source')")
        r2 = builder.connect_nodes(out_h, audio_outs[0])
        if ok_result(r2) and len(audio_outs) > 1 and args["output_format"] == "stereo":
            # Mono source feeding a stereo output: drive both channels rather
            # than silently leaving the right one unconnected.
            builder.connect_nodes(out_h, audio_outs[1])
    elif dst.startswith("graph_output:"):
        r2 = builder.connect_node_output_to_graph_output(dst.split(":", 1)[1], out_h)
    else:
        dnid, dpin = split_ref(dst)
        if dnid is None or dnid not in handles:
            return ngg_error("connection 'to' must be \\"nodeId.InputName\\", 'audio_out' or "
                             "'graph_output:Name'; got '" + str(dst) + "'")
        in_h, r3 = builder.find_node_input_by_name(handles[dnid], dpin)
        if not ok_result(r3):
            return ngg_error("node '" + dnid + "' has no input '" + str(dpin) + "'. It exposes: " +
                             ", ".join(node_input_names(builder, handles[dnid])))
        r2 = builder.connect_nodes(out_h, in_h)

    if not ok_result(r2):
        return ngg_error("could not connect " + str(src) + " -> " + str(dst) + " (" + s(r2) + "). "
                         "MetaSound connections are type-checked: an Audio output cannot drive a Float input.")
    made.append({"from": src, "to": dst})

# --- build ----------------------------------------------------------------
built, br = ed.build_to_asset(builder, args["author"], asset_name, package_path)
if not ok_result(br) or built is None:
    return ngg_error("build_to_asset failed (" + s(br) + ") — no asset was written")

ngg_result({
    "ok": True,
    "asset": s(built.get_path_name()),
    "type": args["type"],
    "output_format": args["output_format"] if args["type"] == "source" else None,
    "one_shot": args["one_shot"] if args["type"] == "source" else None,
    "graph_inputs": declared_inputs,
    "nodes": report,
    "connections": made,
})
`);
}

// ---------------------------------------------------------------------------
// Read
// ---------------------------------------------------------------------------

/**
 * Report what the MetaSound API exposes about an existing asset: its type,
 * output format, declared interfaces, and graph inputs/outputs.
 *
 * Deliberately honest about its limit — UE 5.8's builder API has no way to
 * enumerate the nodes inside a graph, so this cannot show them. Open the asset
 * in the MetaSound editor to see the node graph.
 *
 * @param {object} opts
 * @param {string} opts.asset_path
 */
export async function metasoundRead({ asset_path } = {}) {
  return runPy(`
${pyArgs({ asset_path })}
if not args.get("asset_path"):
    return ngg_error("asset_path is required")

asset = unreal.load_asset(args["asset_path"])
if asset is None:
    return ngg_error("asset not found: " + args["asset_path"])

kind = type(asset).__name__
if kind not in ("MetaSoundSource", "MetaSoundPatch"):
    return ngg_error(args["asset_path"] + " is a " + kind + ", not a MetaSound")

ed = editor_subsystem()
builder, r = ed.find_or_begin_building(asset)
if builder is None or not ok_result(r):
    return ngg_error("could not open a builder for " + args["asset_path"] + " (" + s(r) + ")")

gi, _ = builder.get_graph_input_names()
go, _ = builder.get_graph_output_names()

result = {
    "ok": True,
    "asset": args["asset_path"],
    "type": "source" if kind == "MetaSoundSource" else "patch",
    "graph_inputs": [s(x) for x in gi],
    "graph_outputs": [s(x) for x in go],
    "is_preset": bool(builder.is_preset()),
    "note": ("UE 5.8's MetaSound scripting API cannot enumerate the nodes inside a graph — "
             "only its inputs and outputs. Open the asset in the MetaSound editor to inspect nodes."),
}
if kind == "MetaSoundSource":
    try:
        result["output_format"] = s(asset.get_editor_property("output_format"))
    except Exception:
        pass

ngg_result(result)
`);
}

// ---------------------------------------------------------------------------
// Retune
// ---------------------------------------------------------------------------

/**
 * Change the default value of a graph input on an existing MetaSound — the
 * knobs the sound exposes, without rebuilding the graph.
 *
 * @param {object} opts
 * @param {string} opts.asset_path
 * @param {object} opts.inputs      - { InputName: value, ... }
 * @param {boolean}[opts.save=true]
 */
export async function metasoundSetGraphInputDefaults({ asset_path, inputs, save } = {}) {
  return runPy(`
${pyArgs({ asset_path, inputs: inputs || {}, save: save !== false })}
if not args.get("asset_path"):
    return ngg_error("asset_path is required")
if not args["inputs"]:
    return ngg_error("inputs is empty — nothing to change")

asset = unreal.load_asset(args["asset_path"])
if asset is None:
    return ngg_error("asset not found: " + args["asset_path"])

ed = editor_subsystem()
builder, r = ed.find_or_begin_building(asset)
if builder is None or not ok_result(r):
    return ngg_error("could not open a builder for " + args["asset_path"] + " (" + s(r) + ")")

available, _ = builder.get_graph_input_names()
available = [str(x) for x in available]

applied = []
for name, value in args["inputs"].items():
    if name not in available:
        return ngg_error("no graph input named '" + str(name) + "'. This MetaSound exposes: " +
                         (", ".join(available) or "(none)"))
    lit = literal_for(value)
    if lit is None:
        return ngg_error("unsupported value for input '" + str(name) + "'")
    res = builder.set_graph_input_default(name, lit)
    if not ok_result(res):
        return ngg_error("could not set '" + str(name) + "' (" + s(res) + ") — the value's type "
                         "must match the input's declared MetaSound type")
    applied.append({"name": name, "value": value})

builder.build_and_overwrite_meta_sound(asset, False)

result = {"ok": True, "asset": args["asset_path"], "applied": applied, "graph_inputs": available}
if args["save"]:
    result["saved"] = unreal.EditorAssetLibrary.save_asset(args["asset_path"], only_if_is_dirty=False)

ngg_result(result)
`);
}
