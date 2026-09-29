// Copyright 2025-2026 NGG. All Rights Reserved.
// landscape.js — unrealngg-mcp
//
// Landscape inventory and material/paint-layer setup.
//
// The honest headline first: **a landscape cannot be created from script in UE
// 5.8.** This was checked rather than assumed —
//
//   * there is no ULandscapeEditorSubsystem, and ULandscapeInfo,
//     ULandscapeEditorObject and FLandscapeImportHelper are not exposed to
//     Python at all;
//   * ALandscape::Import and ULandscapeEditorObject::ImportLandscapeData are
//     plain C++, not UFUNCTIONs, so nothing reaches them from script;
//   * spawning unreal.Landscape does not produce a landscape — the engine gives
//     back a LandscapePlaceholder actor, which is the same thing you get from
//     dragging the class into a level and which has none of the landscape API.
//
// So the first step stays manual: Landscape mode -> New Landscape (or Import
// from a heightmap). Everything after that — assigning the landscape material,
// creating the LandscapeLayerInfoObject assets its paint layers need, and
// wiring them into the target layer map — is tedious by hand and is what these
// tools do.
//
// Runs through POST /editor/exec_python, like rendering.js / substrate.js.

import * as ue5 from "./ue5client.js";

const RESULT_BEGIN = "__NGG_LANDSCAPE_RESULT_BEGIN__";
const RESULT_END   = "__NGG_LANDSCAPE_RESULT_END__";

/** Message used whenever the open level has no landscape to work on. */
export const NO_LANDSCAPE_MESSAGE =
  "This level has no landscape. UE 5.8 cannot create one from script — " +
  "ALandscape::Import is not exposed to Python and spawning the class yields a " +
  "LandscapePlaceholder actor, not a landscape. Create it in the editor " +
  "(Landscape mode -> New Landscape, or Import from a heightmap), then these " +
  "tools can set its material and paint layers.";

const PY_PRELUDE = `
import unreal, json, base64, traceback

def ngg_result(obj):
    print("${RESULT_BEGIN}" + json.dumps(obj) + "${RESULT_END}")

def ngg_error(msg):
    ngg_result({"ok": False, "error": msg})

def s(v):
    return None if v is None else str(v)

def asset_tools():
    return unreal.AssetToolsHelpers.get_asset_tools()

def split_path(path):
    return path.rsplit("/", 1)[-1], path.rsplit("/", 1)[0]

def editor_world():
    return unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()

def actor_subsystem():
    return unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

def landscapes():
    """Every landscape in the open level.

    Streaming proxies are separate actors on a World Partition map, so the
    result can hold many entries for what an author thinks of as one landscape;
    the parent ALandscape is what owns the material and the target layers.
    """
    out = []
    for a in actor_subsystem().get_all_level_actors():
        if isinstance(a, unreal.LandscapeProxy):
            out.append(a)
    return out

def is_parent(actor):
    return isinstance(actor, unreal.Landscape)

def describe(actor):
    entry = {
        "label": str(actor.get_actor_label()),
        "class": type(actor).__name__,
        "is_parent_landscape": is_parent(actor),
        "location": list(actor.get_actor_location().to_tuple()),
        "scale": list(actor.get_actor_scale3d().to_tuple()),
    }
    try:
        mat = actor.get_editor_property("landscape_material")
        entry["landscape_material"] = s(mat.get_path_name()) if mat else None
    except Exception:
        pass
    try:
        hole = actor.get_editor_property("landscape_hole_material")
        entry["landscape_hole_material"] = s(hole.get_path_name()) if hole else None
    except Exception:
        pass
    try:
        target = actor.get_editor_property("target_layers")
        layers = []
        for name, settings in target.items():
            info = settings.get_editor_property("layer_info_obj")
            layers.append({
                "name": str(name),
                "layer_info": s(info.get_path_name()) if info else None,
            })
        entry["target_layers"] = sorted(layers, key=lambda l: l["name"])
    except Exception as e:
        entry["target_layers_error"] = str(e)
    if is_parent(actor):
        try:
            entry["target_layer_names"] = [str(n) for n in actor.get_target_layer_names(True)]
        except Exception:
            pass
        try:
            entry["edit_layers"] = [str(l.get_editor_property("name"))
                                    for l in actor.get_edit_layers_bp()]
        except Exception:
            pass
    return entry
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
// Read
// ---------------------------------------------------------------------------

/**
 * Report the landscapes in the open level: their material, paint layers, edit
 * layers, and which actor is the parent as opposed to a streaming proxy.
 *
 * @param {object}  opts
 * @param {boolean} [opts.include_proxies=false] - list World Partition streaming proxies too
 */
export async function landscapeRead({ include_proxies } = {}) {
  return runPy(`
${pyArgs({ include_proxies: !!include_proxies })}
world = editor_world()
if world is None:
    return ngg_error("no level is open in the editor")

found = landscapes()
parents = [a for a in found if is_parent(a)]
proxies = [a for a in found if not is_parent(a)]

result = {
    "ok": True,
    "level": str(world.get_name()),
    "landscape_count": len(parents),
    "streaming_proxy_count": len(proxies),
    "landscapes": [describe(a) for a in parents],
}
if args["include_proxies"]:
    result["streaming_proxies"] = [describe(a) for a in proxies]
if not parents:
    result["note"] = ${JSON.stringify(NO_LANDSCAPE_MESSAGE)}
ngg_result(result)
`);
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

/**
 * Assign a landscape's material and set up its paint layers, creating the
 * LandscapeLayerInfoObject assets each named layer needs.
 *
 * A paint layer in the material is inert until a LayerInfo asset is bound to
 * it — that binding is what the Landscape mode UI's "+" button next to a layer
 * does, and it is per landscape.
 *
 * @param {object}   opts
 * @param {string}   [opts.landscape_label]   - which landscape; defaults to the only one
 * @param {string}   [opts.material]
 * @param {string}   [opts.hole_material]
 * @param {Array}    [opts.layers]            - [{name, layer_info_path?, hardness?, debug_color?, phys_material?}]
 * @param {string}   [opts.layer_info_folder] - where new LayerInfo assets go
 * @param {boolean}  [opts.save=true]
 */
export async function landscapeSetup({
  landscape_label, material, hole_material, layers, layer_info_folder, save,
} = {}) {
  return runPy(`
${pyArgs({
  landscape_label, material, hole_material,
  layers: layers || [], layer_info_folder, save: save !== false,
})}
if (not args.get("material") and not args.get("hole_material") and not args["layers"]):
    return ngg_error("nothing to do — pass material, hole_material or layers[]")

world = editor_world()
if world is None:
    return ngg_error("no level is open in the editor")

found = [a for a in landscapes() if is_parent(a)]
if not found:
    return ngg_error(${JSON.stringify(NO_LANDSCAPE_MESSAGE)})

if args.get("landscape_label"):
    target = None
    for a in found:
        if str(a.get_actor_label()) == args["landscape_label"]:
            target = a
            break
    if target is None:
        return ngg_error("no landscape labelled '" + args["landscape_label"] + "' in this level. "
                         "Present: " + ", ".join(sorted(str(a.get_actor_label()) for a in found)))
elif len(found) > 1:
    return ngg_error("this level has " + str(len(found)) + " landscapes (" +
                     ", ".join(sorted(str(a.get_actor_label()) for a in found)) +
                     ") — pass landscape_label to say which one")
else:
    target = found[0]

lib = unreal.EditorAssetLibrary

def load_material(path, label):
    obj = unreal.load_asset(path)
    if obj is None:
        return None, label + " not found: " + path
    if not isinstance(obj, unreal.MaterialInterface):
        return None, path + " is a " + type(obj).__name__ + ", not a Material or Material Instance"
    return obj, None

mat = hole = None
if args.get("material"):
    mat, err = load_material(args["material"], "material")
    if err:
        return ngg_error(err)
if args.get("hole_material"):
    hole, err = load_material(args["hole_material"], "hole_material")
    if err:
        return ngg_error(err)

# Validate every layer before creating any asset, so a typo in the third entry
# does not leave the first two behind.
default_folder = args.get("layer_info_folder")
if not default_folder:
    level_path = str(world.get_path_name()).split(".")[0]
    default_folder = level_path.rsplit("/", 1)[0] + "/LayerInfo"

for spec in args["layers"]:
    if not spec.get("name"):
        return ngg_error("every entry in layers[] needs a 'name' — the paint layer name used in "
                         "the landscape material")
    if spec.get("phys_material"):
        pm = unreal.load_asset(spec["phys_material"])
        if pm is None or not isinstance(pm, unreal.PhysicalMaterial):
            return ngg_error("phys_material for layer '" + str(spec["name"]) +
                             "' is not a PhysicalMaterial: " + str(spec["phys_material"]))
    color = spec.get("debug_color")
    if color is not None and (not isinstance(color, list) or len(color) < 3):
        return ngg_error("debug_color for layer '" + str(spec["name"]) + "' must be [r, g, b]")

if mat is not None:
    target.set_editor_property("landscape_material", mat)
if hole is not None:
    target.set_editor_property("landscape_hole_material", hole)

target_layers = target.get_editor_property("target_layers")
applied = []

for spec in args["layers"]:
    name = str(spec["name"])
    path = spec.get("layer_info_path") or (default_folder + "/LI_" + name)

    created = False
    info = unreal.load_asset(path)
    if info is None:
        asset_name, folder = split_path(path)
        info = asset_tools().create_asset(asset_name, folder, unreal.LandscapeLayerInfoObject, None)
        if info is None:
            return ngg_error("could not create a LandscapeLayerInfoObject at " + path)
        created = True
    elif not isinstance(info, unreal.LandscapeLayerInfoObject):
        return ngg_error(path + " is a " + type(info).__name__ + ", not a LandscapeLayerInfoObject")

    # LayerName is what ties the asset to the paint layer in the material; an
    # asset with the wrong name binds but never paints.
    info.set_editor_property("layer_name", name)
    if spec.get("hardness") is not None:
        info.set_editor_property("hardness", float(spec["hardness"]))
    if spec.get("phys_material"):
        info.set_editor_property("phys_material", unreal.load_asset(spec["phys_material"]))
    if spec.get("debug_color") is not None:
        c = list(spec["debug_color"]) + [1.0] * (4 - len(spec["debug_color"]))
        info.set_editor_property("layer_usage_debug_color",
                                 unreal.LinearColor(float(c[0]), float(c[1]), float(c[2]), float(c[3])))
    lib.save_asset(path, only_if_is_dirty=False)

    settings = unreal.LandscapeTargetLayerSettings()
    settings.set_editor_property("layer_info_obj", info)
    target_layers[name] = settings

    applied.append({"layer": name, "layer_info": path, "layer_info_created": created})

if args["layers"]:
    # target_layers came back as a copy of the map, like every struct/container
    # property in the Python bindings — without writing it back nothing sticks.
    target.set_editor_property("target_layers", target_layers)

saved = None
if args["save"]:
    saved = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level()

ngg_result({
    "ok": True,
    "landscape": str(target.get_actor_label()),
    "layers_applied": applied,
    "saved": saved,
    "state": describe(target),
})
`);
}
