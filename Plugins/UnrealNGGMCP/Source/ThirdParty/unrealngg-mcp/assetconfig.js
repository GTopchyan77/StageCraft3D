// Copyright 2025-2026 NGG. All Rights Reserved.
// assetconfig.js — unrealngg-mcp
//
// The per-asset setup that used to be reachable only through the generic
// ue5_set_asset_property, where you had to already know the exact UPROPERTY
// name and enum spelling: static mesh collision, physical materials, foliage
// types and texture compression.
//
// Runs through POST /editor/exec_python, like rendering.js / rigging.js.
//
// Probed against a live UE 5.8.1 editor before this was written. What it found:
//
//   * Several of these properties are absent from dir() on the Python class but
//     are still readable and writable through get/set_editor_property —
//     FoliageType's `density` and `radius`, for instance. Enumerating the
//     bindings is therefore not a reliable way to know what is available, which
//     is why the field lists here are explicit.
//   * Enum spellings are inconsistent across the engine (TC_NORMALMAP,
//     TEXTUREGROUP_CHARACTER, TMGS_NO_MIPMAPS, CTF_USE_SIMPLE_AS_COMPLEX), so
//     every enum argument accepts the short form too and a bad value answers
//     with the real list rather than a converter error.
//   * EPhysicalSurface only ever contains the surface types a project has
//     declared in DefaultEngine.ini. On a project that declared none, the only
//     valid value is 'default' — so a rejected surface_type says which ones the
//     project actually has instead of implying the name was misspelled.

import * as ue5 from "./ue5client.js";

const RESULT_BEGIN = "__NGG_ASSETCFG_RESULT_BEGIN__";
const RESULT_END   = "__NGG_ASSETCFG_RESULT_END__";

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

def mesh_subsystem():
    return unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)

def load_typed(path, cls, label):
    obj = unreal.load_asset(path)
    if obj is None:
        return None, label + " not found: " + path
    if not isinstance(obj, cls):
        return None, path + " is a " + type(obj).__name__ + ", not a " + label
    return obj, None

# --- enums -----------------------------------------------------------------
#
# UE spells its enum entries with a per-enum prefix (TC_, TEXTUREGROUP_, TMGS_,
# CTF_, ...). Callers should not have to remember which. Both the full name and
# the prefix-less short form resolve, case- and underscore-insensitively.

ENUM_PREFIXES = ("TC_", "TEXTUREGROUP_", "TMGS_", "TA_", "TF_", "CTF_",
                 "SURFACE_TYPE_", "TLCA_", "TMLO_")

def enum_names(enum_type):
    return sorted(n for n in dir(enum_type) if n.isupper())

def _norm(text):
    return text.upper().replace("_", "").replace(" ", "")

def resolve_enum(enum_type, value, label):
    """Return (entry, None) or (None, error). None input means 'not given'."""
    if value is None:
        return None, None
    names = enum_names(enum_type)
    want = _norm(str(value))
    for n in names:
        if _norm(n) == want:
            return getattr(enum_type, n), None
    for n in names:
        for pre in ENUM_PREFIXES:
            if n.startswith(pre) and _norm(n[len(pre):]) == want:
                return getattr(enum_type, n), None
    return None, (label + " '" + str(value) + "' is not valid. Options: " + ", ".join(names))

def interval(cls, pair, cast):
    iv = cls()
    iv.set_editor_property("min", cast(pair[0]))
    iv.set_editor_property("max", cast(pair[1]))
    return iv

def interval_out(iv):
    if iv is None:
        return None
    try:
        return [iv.get_editor_property("min"), iv.get_editor_property("max")]
    except Exception:
        return s(iv)

COLLISION_SHAPES = {
    "box": unreal.ScriptCollisionShapeType.BOX,
    "sphere": unreal.ScriptCollisionShapeType.SPHERE,
    "capsule": unreal.ScriptCollisionShapeType.CAPSULE,
    "ndop10_x": unreal.ScriptCollisionShapeType.NDOP10_X,
    "ndop10_y": unreal.ScriptCollisionShapeType.NDOP10_Y,
    "ndop10_z": unreal.ScriptCollisionShapeType.NDOP10_Z,
    "ndop18": unreal.ScriptCollisionShapeType.NDOP18,
    "ndop26": unreal.ScriptCollisionShapeType.NDOP26,
}

def collision_summary(mesh):
    sub = mesh_subsystem()
    body = mesh.get_editor_property("body_setup")
    phys = body.get_editor_property("phys_material") if body else None
    return {
        "simple_collision_count": sub.get_simple_collision_count(mesh),
        "convex_collision_count": sub.get_convex_collision_count(mesh),
        "collision_complexity": s(sub.get_collision_complexity(mesh)),
        "physical_material": s(phys.get_path_name()) if phys else None,
        "double_sided_geometry": bool(body.get_editor_property("double_sided_geometry")) if body else None,
    }
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
// Collision
// ---------------------------------------------------------------------------

/**
 * Set up collision on static meshes: simple primitives, convex decomposition,
 * the trace flag, and the physical material.
 *
 * @param {object}   opts
 * @param {string[]} opts.static_meshes
 * @param {boolean}  [opts.remove_existing]  - clear collision before adding
 * @param {string}   [opts.add_shape]        - box | sphere | capsule | ndop10_x | ndop10_y | ndop10_z | ndop18 | ndop26
 * @param {number}   [opts.shape_count=1]
 * @param {number}   [opts.convex_hulls]     - run convex decomposition with this many hulls
 * @param {number}   [opts.convex_max_verts=16]
 * @param {number}   [opts.convex_precision=100000]
 * @param {string}   [opts.trace_flag]       - use_default | use_simple_and_complex | use_simple_as_complex | use_complex_as_simple
 * @param {string}   [opts.physical_material]
 * @param {boolean}  [opts.double_sided_geometry]
 * @param {boolean}  [opts.save=true]
 */
export async function collisionConfigure({
  static_meshes, remove_existing, add_shape, shape_count, convex_hulls,
  convex_max_verts, convex_precision, trace_flag, physical_material,
  double_sided_geometry, save,
} = {}) {
  return runPy(`
${pyArgs({
  static_meshes: static_meshes || [],
  remove_existing: !!remove_existing,
  add_shape: add_shape ? String(add_shape).toLowerCase() : undefined,
  shape_count: shape_count == null ? 1 : shape_count,
  convex_hulls,
  convex_max_verts: convex_max_verts == null ? 16 : convex_max_verts,
  convex_precision: convex_precision == null ? 100000 : convex_precision,
  trace_flag, physical_material, double_sided_geometry,
  save: save !== false,
})}
if not args["static_meshes"]:
    return ngg_error("static_meshes[] is empty — nothing to configure")

if args.get("add_shape") is not None and args["add_shape"] not in COLLISION_SHAPES:
    return ngg_error("add_shape must be one of: " + ", ".join(sorted(COLLISION_SHAPES)))

if (not args["remove_existing"] and args.get("add_shape") is None
        and args.get("convex_hulls") is None and args.get("trace_flag") is None
        and args.get("physical_material") is None
        and args.get("double_sided_geometry") is None):
    return ngg_error("nothing to change — pass remove_existing, add_shape, convex_hulls, "
                     "trace_flag, physical_material or double_sided_geometry")

flag, err = resolve_enum(unreal.CollisionTraceFlag, args.get("trace_flag"), "trace_flag")
if err:
    return ngg_error(err)

phys = None
if args.get("physical_material"):
    phys, err = load_typed(args["physical_material"], unreal.PhysicalMaterial, "PhysicalMaterial")
    if err:
        return ngg_error(err)

sub = mesh_subsystem()
lib = unreal.EditorAssetLibrary
results = []

for path in args["static_meshes"]:
    mesh, err = load_typed(path, unreal.StaticMesh, "StaticMesh")
    if err:
        return ngg_error(err)

    before = collision_summary(mesh)

    # Order matters: clearing after adding would throw the new shapes away.
    if args["remove_existing"]:
        sub.remove_collisions(mesh)

    added = None
    if args.get("add_shape") is not None:
        count = int(args["shape_count"])
        if count < 1:
            return ngg_error("shape_count must be at least 1")
        for _ in range(count):
            added = sub.add_simple_collisions(mesh, COLLISION_SHAPES[args["add_shape"]])
        if added is None or int(added) < 0:
            return ngg_error("add_simple_collisions refused shape '" + args["add_shape"] +
                             "' on " + path)

    decomposed = None
    if args.get("convex_hulls") is not None:
        decomposed = sub.set_convex_decomposition_collisions(
            mesh, int(args["convex_hulls"]), int(args["convex_max_verts"]),
            int(args["convex_precision"]))
        if not decomposed:
            return ngg_error("convex decomposition failed on " + path +
                             " (hulls=" + str(args["convex_hulls"]) + ")")

    body = mesh.get_editor_property("body_setup")
    if body is None and (flag is not None or phys is not None
                         or args.get("double_sided_geometry") is not None):
        return ngg_error(path + " has no BodySetup, so trace_flag / physical_material / "
                         "double_sided_geometry have nothing to write to. Add collision first "
                         "(add_shape or convex_hulls).")
    if flag is not None:
        body.set_editor_property("collision_trace_flag", flag)
    if phys is not None:
        body.set_editor_property("phys_material", phys)
    if args.get("double_sided_geometry") is not None:
        body.set_editor_property("double_sided_geometry", bool(args["double_sided_geometry"]))

    saved = None
    if args["save"]:
        saved = lib.save_asset(path, only_if_is_dirty=False)

    results.append({
        "static_mesh": path,
        "before": before,
        "after": collision_summary(mesh),
        "convex_decomposed": decomposed,
        "saved": saved,
    })

ngg_result({"ok": True, "meshes": results})
`);
}

// ---------------------------------------------------------------------------
// Physical material
// ---------------------------------------------------------------------------

/**
 * Create or update a PhysicalMaterial — friction, restitution, density and the
 * surface type that drives footstep sounds and decals.
 *
 * @param {object}  opts
 * @param {string}  opts.asset_path
 * @param {number}  [opts.friction]
 * @param {number}  [opts.static_friction]
 * @param {number}  [opts.restitution]
 * @param {number}  [opts.density]
 * @param {string}  [opts.friction_combine_mode]     - average | min | multiply | max
 * @param {string}  [opts.restitution_combine_mode]
 * @param {string}  [opts.surface_type]
 * @param {boolean} [opts.overwrite=false]           - update an existing asset
 */
export async function physicalMaterialCreate({
  asset_path, friction, static_friction, restitution, density,
  friction_combine_mode, restitution_combine_mode, surface_type, overwrite,
} = {}) {
  return runPy(`
${pyArgs({
  asset_path, friction, static_friction, restitution, density,
  friction_combine_mode, restitution_combine_mode, surface_type,
  overwrite: !!overwrite,
})}
if not args.get("asset_path"):
    return ngg_error("asset_path is required")

lib = unreal.EditorAssetLibrary
exists = lib.does_asset_exist(args["asset_path"])
if exists and not args["overwrite"]:
    return ngg_error("an asset already exists at " + args["asset_path"] +
                     " — pass overwrite:true to change it in place")

surface, err = resolve_enum(unreal.PhysicalSurface, args.get("surface_type"), "surface_type")
if err:
    return ngg_error(err + ". Surface types come from [/Script/Engine.PhysicsSettings] in "
                     "DefaultEngine.ini — a project that declares none has only 'default'.")

combine = {}
for key in ("friction_combine_mode", "restitution_combine_mode"):
    value, err = resolve_enum(unreal.FrictionCombineMode, args.get(key), key)
    if err:
        return ngg_error(err)
    combine[key] = value

if exists:
    mat, err = load_typed(args["asset_path"], unreal.PhysicalMaterial, "PhysicalMaterial")
    if err:
        return ngg_error(err)
    created = False
else:
    name, folder = split_path(args["asset_path"])
    mat = asset_tools().create_asset(name, folder, unreal.PhysicalMaterial,
                                     unreal.PhysicalMaterialFactoryNew())
    if mat is None:
        return ngg_error("could not create a PhysicalMaterial at " + args["asset_path"])
    created = True

applied = []
for key in ("friction", "static_friction", "restitution", "density"):
    if args.get(key) is not None:
        mat.set_editor_property(key, float(args[key]))
        applied.append(key)

# The combine modes are ignored unless their override flag is on — same trap as
# post-process settings, and just as quiet.
if combine["friction_combine_mode"] is not None:
    mat.set_editor_property("friction_combine_mode", combine["friction_combine_mode"])
    mat.set_editor_property("override_friction_combine_mode", True)
    applied.append("friction_combine_mode")
if combine["restitution_combine_mode"] is not None:
    mat.set_editor_property("restitution_combine_mode", combine["restitution_combine_mode"])
    mat.set_editor_property("override_restitution_combine_mode", True)
    applied.append("restitution_combine_mode")
if surface is not None:
    mat.set_editor_property("surface_type", surface)
    applied.append("surface_type")

saved = lib.save_asset(args["asset_path"], only_if_is_dirty=False)

ngg_result({
    "ok": True,
    "asset": args["asset_path"],
    "created": created,
    "applied": applied,
    "friction": mat.get_editor_property("friction"),
    "static_friction": mat.get_editor_property("static_friction"),
    "restitution": mat.get_editor_property("restitution"),
    "density": mat.get_editor_property("density"),
    "surface_type": s(mat.get_editor_property("surface_type")),
    "friction_combine_mode": s(mat.get_editor_property("friction_combine_mode")),
    "override_friction_combine_mode": bool(mat.get_editor_property("override_friction_combine_mode")),
    "saved": saved,
})
`);
}

// ---------------------------------------------------------------------------
// Foliage
// ---------------------------------------------------------------------------

/**
 * Create a FoliageType asset for a static mesh: the settings the foliage brush
 * and procedural foliage both read.
 *
 * @param {object}   opts
 * @param {string}   opts.asset_path
 * @param {string}   opts.static_mesh
 * @param {number}   [opts.density]
 * @param {number}   [opts.radius]
 * @param {string}   [opts.scaling]              - uniform | free | lock_xy | lock_xz | lock_yz
 * @param {number[]} [opts.scale_x]              - [min, max]
 * @param {number[]} [opts.scale_y]
 * @param {number[]} [opts.scale_z]
 * @param {boolean}  [opts.align_to_normal]
 * @param {boolean}  [opts.random_yaw]
 * @param {number}   [opts.random_pitch_angle]
 * @param {number[]} [opts.ground_slope_angle]   - [min, max] degrees
 * @param {number[]} [opts.z_offset]             - [min, max]
 * @param {number[]} [opts.cull_distance]        - [min, max] integers
 * @param {boolean}  [opts.collision_with_world]
 * @param {boolean}  [opts.cast_shadow]
 * @param {boolean}  [opts.overwrite=false]
 */
export async function foliageTypeCreate({
  asset_path, static_mesh, density, radius, scaling, scale_x, scale_y, scale_z,
  align_to_normal, random_yaw, random_pitch_angle, ground_slope_angle, z_offset,
  cull_distance, collision_with_world, cast_shadow, overwrite,
} = {}) {
  return runPy(`
${pyArgs({
  asset_path, static_mesh, density, radius, scaling, scale_x, scale_y, scale_z,
  align_to_normal, random_yaw, random_pitch_angle, ground_slope_angle, z_offset,
  cull_distance, collision_with_world, cast_shadow, overwrite: !!overwrite,
})}
if not args.get("asset_path"):
    return ngg_error("asset_path is required")
if not args.get("static_mesh"):
    return ngg_error("static_mesh is required — a FoliageType with no mesh scatters nothing")

lib = unreal.EditorAssetLibrary
exists = lib.does_asset_exist(args["asset_path"])
if exists and not args["overwrite"]:
    return ngg_error("an asset already exists at " + args["asset_path"] +
                     " — pass overwrite:true to change it in place")

mesh, err = load_typed(args["static_mesh"], unreal.StaticMesh, "StaticMesh")
if err:
    return ngg_error(err)

scaling, err = resolve_enum(unreal.FoliageScaling, args.get("scaling"), "scaling")
if err:
    return ngg_error(err)

for key in ("scale_x", "scale_y", "scale_z", "ground_slope_angle", "z_offset", "cull_distance"):
    v = args.get(key)
    if v is not None and (not isinstance(v, list) or len(v) != 2):
        return ngg_error(key + " must be a [min, max] pair")

if exists:
    ft, err = load_typed(args["asset_path"], unreal.FoliageType, "FoliageType")
    if err:
        return ngg_error(err)
    created = False
else:
    name, folder = split_path(args["asset_path"])
    ft = asset_tools().create_asset(name, folder, unreal.FoliageType_InstancedStaticMesh,
                                    unreal.FoliageType_InstancedStaticMeshFactory())
    if ft is None:
        return ngg_error("could not create a FoliageType at " + args["asset_path"])
    created = True

ft.set_editor_property("mesh", mesh)
applied = ["mesh"]

for key, cast in (("density", float), ("radius", float), ("random_pitch_angle", float)):
    if args.get(key) is not None:
        ft.set_editor_property(key, cast(args[key]))
        applied.append(key)

for key in ("align_to_normal", "random_yaw", "collision_with_world", "cast_shadow"):
    if args.get(key) is not None:
        ft.set_editor_property(key, bool(args[key]))
        applied.append(key)

for key in ("scale_x", "scale_y", "scale_z", "ground_slope_angle", "z_offset"):
    if args.get(key) is not None:
        ft.set_editor_property(key, interval(unreal.FloatInterval, args[key], float))
        applied.append(key)

if args.get("cull_distance") is not None:
    ft.set_editor_property("cull_distance", interval(unreal.Int32Interval, args["cull_distance"], int))
    applied.append("cull_distance")

if scaling is not None:
    ft.set_editor_property("scaling", scaling)
    applied.append("scaling")

saved = lib.save_asset(args["asset_path"], only_if_is_dirty=False)

ngg_result({
    "ok": True,
    "asset": args["asset_path"],
    "created": created,
    "applied": applied,
    "static_mesh": s(ft.get_editor_property("mesh").get_path_name()) if ft.get_editor_property("mesh") else None,
    "density": ft.get_editor_property("density"),
    "radius": ft.get_editor_property("radius"),
    "scaling": s(ft.get_editor_property("scaling")),
    "scale_x": interval_out(ft.get_editor_property("scale_x")),
    "ground_slope_angle": interval_out(ft.get_editor_property("ground_slope_angle")),
    "cull_distance": interval_out(ft.get_editor_property("cull_distance")),
    "align_to_normal": bool(ft.get_editor_property("align_to_normal")),
    "random_yaw": bool(ft.get_editor_property("random_yaw")),
    "saved": saved,
})
`);
}

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------

/**
 * Set import/compression settings on textures — the pass that turns a freshly
 * imported normal map or mask into something the renderer treats correctly.
 *
 * @param {object}   opts
 * @param {string[]} opts.textures
 * @param {string}   [opts.compression_settings] - default | normalmap | masks | grayscale | hdr | bc7 | ...
 * @param {string}   [opts.lod_group]            - world | character | ui | ...
 * @param {boolean}  [opts.srgb]
 * @param {number}   [opts.max_texture_size]
 * @param {string}   [opts.mip_gen_settings]     - from_texture_group | no_mipmaps | simple_average | ...
 * @param {number}   [opts.lod_bias]
 * @param {boolean}  [opts.never_stream]
 * @param {boolean}  [opts.virtual_texture_streaming]
 * @param {string}   [opts.filter]               - default | nearest | bilinear | trilinear
 * @param {string}   [opts.address_x]            - wrap | clamp | mirror
 * @param {string}   [opts.address_y]
 * @param {boolean}  [opts.compression_no_alpha]
 * @param {boolean}  [opts.flip_green_channel]
 * @param {boolean}  [opts.save=true]
 */
export async function textureConfigure({
  textures, compression_settings, lod_group, srgb, max_texture_size,
  mip_gen_settings, lod_bias, never_stream, virtual_texture_streaming,
  filter, address_x, address_y, compression_no_alpha, flip_green_channel, save,
} = {}) {
  return runPy(`
${pyArgs({
  textures: textures || [],
  compression_settings, lod_group, srgb, max_texture_size, mip_gen_settings,
  lod_bias, never_stream, virtual_texture_streaming, filter, address_x, address_y,
  compression_no_alpha, flip_green_channel, save: save !== false,
})}
if not args["textures"]:
    return ngg_error("textures[] is empty — nothing to configure")

ENUMS = [
    ("compression_settings", unreal.TextureCompressionSettings),
    ("lod_group", unreal.TextureGroup),
    ("mip_gen_settings", unreal.TextureMipGenSettings),
    ("filter", unreal.TextureFilter),
    ("address_x", unreal.TextureAddress),
    ("address_y", unreal.TextureAddress),
]
BOOLS = ["srgb", "never_stream", "virtual_texture_streaming",
         "compression_no_alpha", "flip_green_channel"]
INTS = ["max_texture_size", "lod_bias"]

resolved = {}
for key, enum_type in ENUMS:
    value, err = resolve_enum(enum_type, args.get(key), key)
    if err:
        return ngg_error(err)
    resolved[key] = value

requested = ([k for k, _ in ENUMS if args.get(k) is not None]
             + [k for k in BOOLS if args.get(k) is not None]
             + [k for k in INTS if args.get(k) is not None])
if not requested:
    return ngg_error("no settings given — pass at least one, e.g. compression_settings:'normalmap'")

lib = unreal.EditorAssetLibrary
results = []

for path in args["textures"]:
    tex, err = load_typed(path, unreal.Texture, "Texture")
    if err:
        return ngg_error(err)

    def snapshot():
        out = {}
        for key, _ in ENUMS:
            try:
                out[key] = s(tex.get_editor_property(key))
            except Exception:
                pass
        for key in BOOLS + INTS:
            try:
                out[key] = tex.get_editor_property(key)
            except Exception:
                pass
        return out

    before = snapshot()

    for key, _ in ENUMS:
        if resolved[key] is not None:
            tex.set_editor_property(key, resolved[key])
    for key in BOOLS:
        if args.get(key) is not None:
            tex.set_editor_property(key, bool(args[key]))
    for key in INTS:
        if args.get(key) is not None:
            tex.set_editor_property(key, int(args[key]))

    saved = None
    if args["save"]:
        saved = lib.save_asset(path, only_if_is_dirty=False)

    results.append({"texture": path, "before": before, "after": snapshot(), "saved": saved})

ngg_result({"ok": True, "applied": requested, "textures": results})
`);
}
