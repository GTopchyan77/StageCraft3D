// Copyright 2025-2026 NGG. All Rights Reserved.
// substrate.js — unrealngg-mcp
//
// Substrate materials. ue5_create_material builds the legacy BaseColor /
// Metallic / Roughness graph; on a project with Substrate enabled that graph is
// auto-converted at compile time, which works but throws away everything
// Substrate is for — layering, coverage, per-slab control.
//
// Runs through POST /editor/exec_python, like rendering.js / interchange.js.
//
// Probed against a live UE 5.8.1 editor before this was written:
//
//   * Substrate output goes to MP_FRONT_MATERIAL, not BASE_COLOR. Wiring a
//     Substrate BSDF into the legacy outputs silently does nothing.
//   * Pin names are not consistent between Substrate nodes: the Slab BSDF has
//     'Diffuse Albedo' and 'Emissive Color' (with spaces), while the Unlit BSDF
//     has 'EmissiveColor' (without). Friendly names here are resolved against
//     what the engine actually reports for each node, so neither spelling has to
//     be memorised and a rename in a later engine version does not break this.
//   * The Slab BSDF has no BaseColor or Metallic pin at all — it takes Diffuse
//     Albedo and F0. SubstrateMetalnessToDiffuseAlbedoF0 is the bridge from the
//     familiar base colour / metallic / specular triple, so this module inserts
//     it whenever those are given for a slab-style shading model.

import * as ue5 from "./ue5client.js";

const RESULT_BEGIN = "__NGG_SUBSTRATE_RESULT_BEGIN__";
const RESULT_END   = "__NGG_SUBSTRATE_RESULT_END__";

/** Shading models this tool can build, and the node class behind each. */
export const SHADING_MODELS = [
  "slab", "clearcoat", "unlit", "toon", "hair", "eye", "water",
];

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

MEL = unreal.MaterialEditingLibrary

def substrate_enabled():
    try:
        return int(unreal.SystemLibrary.get_console_variable_int_value("r.Substrate")) != 0
    except Exception:
        return False

def input_names(node):
    return [str(n) for n in MEL.get_material_expression_input_names(node)]

def _key(text):
    return str(text).lower().replace(" ", "").replace("_", "")

def find_pin(node, friendly):
    """Match a friendly pin name against what this node actually exposes.

    Substrate spells the same concept differently per node ('Emissive Color' vs
    'EmissiveColor'), so matching ignores case, spaces and underscores.
    """
    want = _key(friendly)
    for name in input_names(node):
        if _key(name) == want:
            return name, None
    return None, ("'" + str(friendly) + "' is not an input of " + type(node).__name__ +
                  ". Its pins are: " + ", ".join(input_names(node)))

# class name, whether base colour has to go through the metalness converter, and
# the friendly names this node takes directly.
SHADING = {
    "slab": {
        "class": "SubstrateSlabBSDF", "converter": True,
        "direct": ["roughness", "emissive", "normal", "anisotropy"],
    },
    "clearcoat": {
        "class": "SubstrateSimpleClearCoatBSDF", "converter": True,
        "direct": ["roughness", "emissive", "normal",
                   "clear coat coverage", "clear coat roughness"],
    },
    "unlit": {
        "class": "SubstrateUnlitBSDF", "converter": False,
        "direct": ["emissive", "normal"],
    },
    "toon": {
        "class": "SubstrateToonBSDF", "converter": False,
        "direct": ["base color", "metallic", "specular", "roughness", "emissive", "normal"],
    },
    "hair": {
        "class": "SubstrateHairBSDF", "converter": False,
        "direct": ["base color", "specular", "roughness", "emissive"],
    },
    "eye": {
        "class": "SubstrateEyeBSDF", "converter": False,
        "direct": ["diffuse color", "roughness", "emissive"],
    },
    "water": {
        "class": "SubstrateSingleLayerWaterBSDF", "converter": False,
        "direct": ["base color", "metallic", "specular", "roughness", "emissive", "normal"],
    },
}

# The friendly name a caller passes -> the pin it means on a direct-input node.
DIRECT_ALIASES = {
    "base_color": ["base color", "diffuse color"],
    "metallic": ["metallic"],
    "specular": ["specular"],
    "roughness": ["roughness"],
    "emissive_color": ["emissive color", "emissive"],
    "normal": ["normal"],
}

def color_of(triple, fallback=(0.0, 0.0, 0.0)):
    if triple is None:
        triple = fallback
    vals = list(triple) + [1.0] * (4 - len(triple))
    return unreal.LinearColor(float(vals[0]), float(vals[1]), float(vals[2]), float(vals[3]))
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
 * Build a native Substrate material: a BSDF node wired into the material's
 * FrontMaterial output, fed by constants or by parameters a material instance
 * can drive.
 *
 * @param {object}   opts
 * @param {string}   opts.asset_path
 * @param {string}   [opts.shading="slab"] - slab | clearcoat | unlit | toon | hair | eye | water
 * @param {number[]} [opts.base_color]     - [r, g, b] linear
 * @param {number}   [opts.metallic]
 * @param {number}   [opts.specular]
 * @param {number}   [opts.roughness]
 * @param {number[]} [opts.emissive_color]
 * @param {string}   [opts.base_color_texture] - texture asset driving base colour
 * @param {string}   [opts.normal_texture]
 * @param {boolean}  [opts.use_parameters] - expose named parameters for instances
 * @param {string}   [opts.blend_mode]     - opaque | masked | translucent | additive
 * @param {boolean}  [opts.two_sided]
 * @param {boolean}  [opts.overwrite]
 */
export async function substrateMaterialCreate({
  asset_path, shading, base_color, metallic, specular, roughness, emissive_color,
  base_color_texture, normal_texture, use_parameters, blend_mode, two_sided, overwrite,
} = {}) {
  return runPy(`
${pyArgs({
  asset_path,
  shading: (shading || "slab").toLowerCase(),
  base_color, metallic, specular, roughness, emissive_color,
  base_color_texture, normal_texture,
  use_parameters: !!use_parameters,
  blend_mode: blend_mode ? String(blend_mode).toLowerCase() : undefined,
  two_sided, overwrite: !!overwrite,
})}
if not args.get("asset_path"):
    return ngg_error("asset_path is required")
if args["shading"] not in SHADING:
    return ngg_error("shading must be one of: " + ", ".join(sorted(SHADING)))

if not substrate_enabled():
    return ngg_error("Substrate is off in this project (r.Substrate = 0), so a FrontMaterial "
                     "graph would compile to nothing. Turn it on with "
                     "ue5_render_project_settings {\\"r.Substrate\\": 1} and restart the editor, "
                     "or use ue5_create_material for the legacy material model.")

lib = unreal.EditorAssetLibrary
if lib.does_asset_exist(args["asset_path"]) and not args["overwrite"]:
    return ngg_error("an asset already exists at " + args["asset_path"] +
                     " — pass overwrite:true to replace it")

spec = SHADING[args["shading"]]
node_class = getattr(unreal, "MaterialExpression" + spec["class"])

BLEND_MODES = {"opaque": unreal.BlendMode.BLEND_OPAQUE,
               "masked": unreal.BlendMode.BLEND_MASKED,
               "translucent": unreal.BlendMode.BLEND_TRANSLUCENT,
               "additive": unreal.BlendMode.BLEND_ADDITIVE,
               "modulate": unreal.BlendMode.BLEND_MODULATE,
               "alpha_composite": unreal.BlendMode.BLEND_ALPHA_COMPOSITE,
               "alpha_holdout": unreal.BlendMode.BLEND_ALPHA_HOLDOUT}
if args.get("blend_mode") and args["blend_mode"] not in BLEND_MODES:
    return ngg_error("blend_mode must be one of: " + ", ".join(sorted(BLEND_MODES)))

# Everything that can be checked without touching the project is checked here,
# before an asset exists. Rolling a material back is not reliable — a freshly
# created Material often refuses to delete — so the honest fix is to not create
# one that is going to fail.
for key, friendly in (("metallic", "metallic"), ("specular", "specular")):
    if args.get(key) is None or spec["converter"]:
        continue
    if friendly not in spec["direct"]:
        return ngg_error("the '" + args["shading"] + "' shading model has no " + friendly +
                         " input. It takes: " + ", ".join(spec["direct"]))
for key, friendly in (("roughness", "roughness"), ("emissive_color", "emissive"),
                      ("normal_texture", "normal")):
    if args.get(key) is None:
        continue
    if friendly not in spec["direct"]:
        return ngg_error("the '" + args["shading"] + "' shading model has no " + friendly +
                         " input. It takes: " + ", ".join(spec["direct"]))

# Load textures before creating anything, for the same reason.
preloaded = {}
for key in ("base_color_texture", "normal_texture"):
    if not args.get(key):
        continue
    tex = unreal.load_asset(args[key])
    if tex is None:
        return ngg_error("texture not found: " + args[key])
    if not isinstance(tex, unreal.Texture):
        return ngg_error(args[key] + " is a " + type(tex).__name__ + ", not a Texture")
    preloaded[key] = tex

name, folder = split_path(args["asset_path"])
if lib.does_asset_exist(args["asset_path"]):
    lib.delete_asset(args["asset_path"])
mat = asset_tools().create_asset(name, folder, unreal.Material, unreal.MaterialFactoryNew())
if mat is None:
    return ngg_error("could not create a Material at " + args["asset_path"])

def fail(msg):
    # Saving first makes the delete far more likely to succeed; a brand new,
    # never-saved Material is often still referenced and refuses to go.
    lib.save_asset(args["asset_path"], only_if_is_dirty=False)
    if lib.delete_asset(args["asset_path"]):
        return ngg_error(msg + " (the half-built material was removed again)")
    return ngg_error(msg + " — and the half-built material at " + args["asset_path"] +
                     " could NOT be removed, so delete it yourself")

bsdf = MEL.create_material_expression(mat, node_class, -400, 0)

# --- value sources ---------------------------------------------------------
made = []
row = [0]

def next_y():
    row[0] += 180
    return row[0] - 500

def scalar(value, param_name):
    if args["use_parameters"]:
        n = MEL.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -900, next_y())
        n.set_editor_property("parameter_name", param_name)
        n.set_editor_property("default_value", float(value))
    else:
        n = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant, -900, next_y())
        n.set_editor_property("r", float(value))
    made.append({"node": type(n).__name__, "for": param_name})
    return n

def vector(value, param_name):
    if args["use_parameters"]:
        n = MEL.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -900, next_y())
        n.set_editor_property("parameter_name", param_name)
        n.set_editor_property("default_value", color_of(value))
    else:
        n = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector, -900, next_y())
        n.set_editor_property("constant", color_of(value))
    made.append({"node": type(n).__name__, "for": param_name})
    return n

def texture(key, param_name):
    tex = preloaded[key]
    cls = (unreal.MaterialExpressionTextureSampleParameter2D if args["use_parameters"]
           else unreal.MaterialExpressionTextureSample)
    n = MEL.create_material_expression(mat, cls, -1200, next_y())
    if args["use_parameters"]:
        n.set_editor_property("parameter_name", param_name)
    n.set_editor_property("texture", tex)
    made.append({"node": type(n).__name__, "for": param_name})
    return n, None

def wire(source, target, friendly, source_output=""):
    pin, err = find_pin(target, friendly)
    if err:
        return err
    if not MEL.connect_material_expressions(source, source_output, target, pin):
        return ("the engine refused to connect " + type(source).__name__ + " -> " +
                type(target).__name__ + "." + pin)
    return None

connections = []

# --- base colour / metallic / specular -------------------------------------
base_node = None
if args.get("base_color_texture"):
    base_node, err = texture("base_color_texture", "BaseColorTexture")
    if err:
        return fail(err)
elif args.get("base_color") is not None:
    base_node = vector(args["base_color"], "BaseColor")

if spec["converter"]:
    if base_node is not None or args.get("metallic") is not None or args.get("specular") is not None:
        conv = MEL.create_material_expression(
            mat, unreal.MaterialExpressionSubstrateMetalnessToDiffuseAlbedoF0, -650, 0)
        made.append({"node": type(conv).__name__, "for": "base_color -> Diffuse Albedo / F0"})
        if base_node is not None:
            err = wire(base_node, conv, "BaseColor")
            if err:
                return fail(err)
            connections.append("BaseColor -> converter")
        if args.get("metallic") is not None:
            err = wire(scalar(args["metallic"], "Metallic"), conv, "Metallic")
            if err:
                return fail(err)
            connections.append("Metallic -> converter")
        if args.get("specular") is not None:
            err = wire(scalar(args["specular"], "Specular"), conv, "Specular")
            if err:
                return fail(err)
            connections.append("Specular -> converter")
        # The slab takes Diffuse Albedo and F0; this is what the converter is for.
        for out_name, pin in (("DiffuseAlbedo", "Diffuse Albedo"), ("F0", "F0")):
            err = wire(conv, bsdf, pin, out_name)
            if err:
                return fail(err)
            connections.append("converter." + out_name + " -> " + pin)
else:
    if base_node is not None:
        target = "diffuse color" if args["shading"] == "eye" else "base color"
        err = wire(base_node, bsdf, target)
        if err:
            return fail(err)
        connections.append("BaseColor -> " + target)
    for key, friendly in (("metallic", "metallic"), ("specular", "specular")):
        if args.get(key) is None:
            continue
        err = wire(scalar(args[key], key.capitalize()), bsdf, friendly)
        if err:
            return fail(err)
        connections.append(key + " -> " + friendly)

# --- roughness / emissive / normal -----------------------------------------
if args.get("roughness") is not None:
    err = wire(scalar(args["roughness"], "Roughness"), bsdf, "roughness")
    if err:
        return fail(err)
    connections.append("roughness")

if args.get("emissive_color") is not None:
    # 'Emissive Color' on a slab, 'EmissiveColor' on the unlit BSDF — find_pin
    # normalises spaces, so one lookup covers both spellings.
    err = wire(vector(args["emissive_color"], "EmissiveColor"), bsdf, "emissive color")
    if err:
        return fail(err)
    connections.append("emissive")

if args.get("normal_texture"):
    normal_node, err = texture("normal_texture", "NormalTexture")
    if err:
        return fail(err)
    err = wire(normal_node, bsdf, "normal")
    if err:
        return fail(err)
    connections.append("normal")

# --- output ----------------------------------------------------------------
# Substrate output is FrontMaterial. Wiring into BASE_COLOR compiles to nothing.
if not MEL.connect_material_property(bsdf, "", unreal.MaterialProperty.MP_FRONT_MATERIAL):
    return fail("could not connect the " + spec["class"] + " to the material's FrontMaterial output")

if args.get("blend_mode"):
    mat.set_editor_property("blend_mode", BLEND_MODES[args["blend_mode"]])
if args.get("two_sided") is not None:
    mat.set_editor_property("two_sided", bool(args["two_sided"]))

MEL.recompile_material(mat)
saved = lib.save_asset(args["asset_path"], only_if_is_dirty=False)

ngg_result({
    "ok": True,
    "asset": args["asset_path"],
    "shading": args["shading"],
    "root_node": spec["class"],
    "uses_parameters": args["use_parameters"],
    "nodes_created": made,
    "connections": connections,
    "bsdf_pins": input_names(bsdf),
    "blend_mode": s(mat.get_editor_property("blend_mode")),
    "two_sided": bool(mat.get_editor_property("two_sided")),
    "scalar_parameters": [str(n) for n in MEL.get_scalar_parameter_names(mat)],
    "vector_parameters": [str(n) for n in MEL.get_vector_parameter_names(mat)],
    "texture_parameters": [str(n) for n in MEL.get_texture_parameter_names(mat)],
    "saved": saved,
})
`);
}

// ---------------------------------------------------------------------------
// Read
// ---------------------------------------------------------------------------

/**
 * Report whether Substrate is on, and what a material's graph is made of —
 * which Substrate nodes it contains, their pins, and the parameters it exposes.
 *
 * @param {object} opts
 * @param {string} [opts.asset_path] - omit to report only the project state
 */
export async function substrateRead({ asset_path } = {}) {
  return runPy(`
${pyArgs({ asset_path })}
result = {"ok": True, "substrate_enabled": substrate_enabled()}
if not result["substrate_enabled"]:
    result["note"] = ("r.Substrate is 0: materials in this project use the legacy shading "
                      "model and any FrontMaterial graph is ignored.")

if args.get("asset_path"):
    mat = unreal.load_asset(args["asset_path"])
    if mat is None:
        return ngg_error("asset not found: " + args["asset_path"])
    if not isinstance(mat, unreal.Material):
        return ngg_error(args["asset_path"] + " is a " + type(mat).__name__ +
                         ", not a Material (material instances have no graph of their own)")

    nodes = []
    substrate_nodes = 0
    for e in MEL.get_material_expressions(mat):
        cls = type(e).__name__
        is_substrate = cls.startswith("MaterialExpressionSubstrate")
        if is_substrate:
            substrate_nodes += 1
        entry = {"class": cls, "substrate": is_substrate}
        try:
            entry["inputs"] = input_names(e)
        except Exception:
            pass
        nodes.append(entry)

    result["asset"] = args["asset_path"]
    result["is_substrate_graph"] = substrate_nodes > 0
    result["substrate_node_count"] = substrate_nodes
    result["nodes"] = nodes
    result["blend_mode"] = s(mat.get_editor_property("blend_mode"))
    result["two_sided"] = bool(mat.get_editor_property("two_sided"))
    result["scalar_parameters"] = [str(n) for n in MEL.get_scalar_parameter_names(mat)]
    result["vector_parameters"] = [str(n) for n in MEL.get_vector_parameter_names(mat)]
    result["texture_parameters"] = [str(n) for n in MEL.get_texture_parameter_names(mat)]
    if substrate_nodes == 0:
        result["note_material"] = ("No Substrate nodes in this graph. With Substrate on, a legacy "
                                   "BaseColor/Metallic/Roughness material still renders — the "
                                   "engine converts it — but none of the layering features apply.")

ngg_result(result)
`);
}
