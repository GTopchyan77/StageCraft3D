// Copyright 2025-2026 NGG. All Rights Reserved.
// rendering.js — unrealngg-mcp
//
// Nanite, Lumen and the project's renderer settings — the three knobs that
// decide how a UE 5.8 project actually looks, none of which had a tool before.
//
// Runs through POST /editor/exec_python, like sequencer.js / rigging.js.
//
// Probed against a live UE 5.8.1 editor before this was written. Three findings
// shaped the API:
//
//   * PostProcessSettings is a *struct*. Reading it from a volume hands back a
//     copy, so every mutation has to be written back with set_editor_property
//     or it evaporates silently. That is the single easiest way to "configure
//     Lumen" and have nothing happen.
//   * Every post-process property is inert until its `override_<name>` bool is
//     set. Setting `lumen_reflection_quality` alone changes nothing at all.
//     This module sets the flag for you and reports which ones it set.
//   * There is no `unreal.RendererSettings` binding, so project-wide renderer
//     settings cannot be reached through the usual CDO route. They are read and
//     written as ini text instead, which is also why they need an editor
//     restart — see ue5_render_project_settings.
//
// One 5.8 rename to know about: FMeshNaniteSettings::bPreserveArea became the
// three-way enum `shape_preservation` (none / preserve_area / voxelize). The old
// name still resolves for reads but warns, and writing a bool to it throws.

import * as ue5 from "./ue5client.js";

const RESULT_BEGIN = "__NGG_RENDER_RESULT_BEGIN__";
const RESULT_END   = "__NGG_RENDER_RESULT_END__";

/** Section of DefaultEngine.ini that holds project-wide renderer settings. */
export const RENDERER_SETTINGS_SECTION = "/Script/Engine.RendererSettings";

/**
 * Console variables worth reporting for a "why does my level look like this"
 * question. Read-only here; the ini is the place to change them for good.
 */
export const REPORTED_CVARS = [
  "r.DynamicGlobalIlluminationMethod",
  "r.ReflectionMethod",
  "r.Lumen.HardwareRayTracing",
  "r.Lumen.TranslucencyReflections.FrontLayer.EnableForProject",
  "r.Shadow.Virtual.Enable",
  "r.Nanite",
  "r.Nanite.ProjectEnabled",
  "r.Substrate",
  "r.GenerateMeshDistanceFields",
  "r.AllowStaticLighting",
  "r.AntiAliasingMethod",
  "r.DefaultFeature.AutoExposure",
];

const PY_PRELUDE = `
import unreal, json, base64, traceback, os

def ngg_result(obj):
    print("${RESULT_BEGIN}" + json.dumps(obj) + "${RESULT_END}")

def ngg_error(msg):
    ngg_result({"ok": False, "error": msg})

def s(v):
    return None if v is None else str(v)

def editor_world():
    return unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()

def actor_subsystem():
    return unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

def mesh_subsystem():
    return unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)

def load_static_mesh(path):
    obj = unreal.load_asset(path)
    if obj is None:
        return None, "static mesh not found: " + path
    if not isinstance(obj, unreal.StaticMesh):
        return None, path + " is a " + type(obj).__name__ + ", not a StaticMesh"
    return obj, None

def config_path(file_name):
    base = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_config_dir())
    return os.path.join(base, file_name)

def read_ini_section(path, section):
    """Return {key: value} for one ini section, or None if the file is absent."""
    if not os.path.isfile(path):
        return None
    with open(path, "r", encoding="utf-8-sig", errors="replace") as f:
        lines = f.read().replace("\\r\\n", "\\n").split("\\n")
    header = "[" + section + "]"
    found = {}
    inside = False
    for line in lines:
        t = line.strip()
        if t.startswith("[") and t.endswith("]"):
            inside = (t == header)
            continue
        if not inside or not t or t.startswith(";") or "=" not in t:
            continue
        k, v = t.split("=", 1)
        found[k.strip()] = v.strip()
    return found

def write_ini_section(path, section, changes):
    """Upsert keys in one ini section, leaving the rest of the file untouched.

    A value of None removes the key. Returns the list of actions taken so the
    caller can report what really happened rather than assuming.

    Line endings: the file is read and rewritten in text mode, so on Windows it
    comes back CRLF regardless of what it was. That matches how the editor
    itself writes ini files, and this plugin is Win64-only.
    """
    if os.path.isfile(path):
        with open(path, "r", encoding="utf-8-sig", errors="replace") as f:
            text = f.read().replace("\\r\\n", "\\n")
        lines = text.split("\\n")
    else:
        lines = []

    header = "[" + section + "]"
    start = -1
    for i, line in enumerate(lines):
        if line.strip() == header:
            start = i
            break

    if start < 0:
        # Nothing to remove from a section that does not exist yet.
        if all(v is None for v in changes.values()):
            return [{"key": k, "action": "absent"} for k in changes]
        while lines and lines[-1].strip() == "":
            lines.pop()
        if lines:
            lines.append("")
        lines.append(header)
        start = len(lines) - 1
        end = len(lines)
    else:
        end = len(lines)
        for j in range(start + 1, len(lines)):
            t = lines[j].strip()
            if t.startswith("[") and t.endswith("]"):
                end = j
                break

    actions = []
    for key, value in changes.items():
        hit = -1
        for j in range(start + 1, end):
            t = lines[j].strip()
            if not t or t.startswith(";") or "=" not in t:
                continue
            if t.split("=", 1)[0].strip() == key:
                hit = j
                break
        if value is None:
            if hit < 0:
                actions.append({"key": key, "action": "absent"})
            else:
                del lines[hit]
                end -= 1
                actions.append({"key": key, "action": "removed"})
        elif hit >= 0:
            old = lines[hit].strip().split("=", 1)[1].strip()
            lines[hit] = key + "=" + value
            actions.append({"key": key, "action": "changed" if old != value else "unchanged",
                            "from": old, "to": value})
        else:
            lines.insert(end, key + "=" + value)
            end += 1
            actions.append({"key": key, "action": "added", "to": value})

    with open(path, "w", encoding="utf-8") as f:
        f.write("\\n".join(lines))
    return actions

def ini_value(v):
    if isinstance(v, bool):
        return "True" if v else "False"
    return str(v)

# --- post process ----------------------------------------------------------
#
# Two things bite here and both are handled centrally:
#   1. get_editor_property("settings") returns a COPY of the struct.
#   2. a property does nothing until override_<name> is True.

def pp_set(settings, name, value):
    """Set one PostProcessSettings property and its override flag."""
    settings.set_editor_property(name, value)
    flag = "override_" + name
    try:
        settings.set_editor_property(flag, True)
        return flag
    except Exception:
        return None

def pp_snapshot(settings, names):
    out = {}
    for n in names:
        try:
            value = settings.get_editor_property(n)
        except Exception:
            continue
        overridden = None
        try:
            overridden = bool(settings.get_editor_property("override_" + n))
        except Exception:
            pass
        out[n] = {"value": s(value), "overridden": overridden}
    return out

GI_METHODS = {
    "lumen": unreal.DynamicGlobalIlluminationMethod.LUMEN,
    "screen_space": unreal.DynamicGlobalIlluminationMethod.SCREEN_SPACE,
    "none": unreal.DynamicGlobalIlluminationMethod.NONE,
    "plugin": unreal.DynamicGlobalIlluminationMethod.PLUGIN,
}

REFLECTION_METHODS = {
    "lumen": unreal.ReflectionMethod.LUMEN,
    "screen_space": unreal.ReflectionMethod.SCREEN_SPACE,
    "none": unreal.ReflectionMethod.NONE,
}

RAY_LIGHTING_MODES = {
    "default": unreal.LumenRayLightingModeOverride.DEFAULT,
    "surface_cache": unreal.LumenRayLightingModeOverride.SURFACE_CACHE,
    "hit_lighting": unreal.LumenRayLightingModeOverride.HIT_LIGHTING,
    "hit_lighting_for_reflections": unreal.LumenRayLightingModeOverride.HIT_LIGHTING_FOR_REFLECTIONS,
}

# Reported by ue5_render_read; also the set a caller is most likely to tune.
LUMEN_REPORTED = [
    "dynamic_global_illumination_method",
    "reflection_method",
    "lumen_scene_lighting_quality",
    "lumen_scene_detail",
    "lumen_scene_view_distance",
    "lumen_final_gather_quality",
    "lumen_reflection_quality",
    "lumen_max_reflection_bounces",
    "lumen_max_trace_distance",
    "lumen_surface_cache_resolution",
    "lumen_ray_lighting_mode",
]

FALLBACK_TARGETS = {
    "auto": unreal.NaniteFallbackTarget.AUTO,
    "percent_triangles": unreal.NaniteFallbackTarget.PERCENT_TRIANGLES,
    "relative_error": unreal.NaniteFallbackTarget.RELATIVE_ERROR,
}

# 5.8 replaced the bool bPreserveArea with a three-way enum. The old name still
# resolves, but reading it emits a deprecation warning, so use the new one.
SHAPE_PRESERVATION = {
    "none": unreal.NaniteShapePreservation.NONE,
    "preserve_area": unreal.NaniteShapePreservation.PRESERVE_AREA,
    "voxelize": unreal.NaniteShapePreservation.VOXELIZE,
}

def nanite_summary(mesh, settings=None):
    ns = settings if settings is not None else mesh_subsystem().get_nanite_settings(mesh)
    out = {
        "enabled": bool(ns.get_editor_property("enabled")),
        "position_precision": int(ns.get_editor_property("position_precision")),
        "normal_precision": int(ns.get_editor_property("normal_precision")),
        "tangent_precision": int(ns.get_editor_property("tangent_precision")),
        "keep_percent_triangles": float(ns.get_editor_property("keep_percent_triangles")),
        "trim_relative_error": float(ns.get_editor_property("trim_relative_error")),
        "fallback_target": s(ns.get_editor_property("fallback_target")),
        "fallback_percent_triangles": float(ns.get_editor_property("fallback_percent_triangles")),
        "fallback_relative_error": float(ns.get_editor_property("fallback_relative_error")),
        "explicit_tangents": bool(ns.get_editor_property("explicit_tangents")),
        "shape_preservation": s(ns.get_editor_property("shape_preservation")),
        "max_edge_length_factor": float(ns.get_editor_property("max_edge_length_factor")),
    }
    try:
        out["num_lods"] = mesh.get_num_lods()
        out["num_triangles_lod0"] = mesh.get_num_triangles(0)
        out["num_nanite_triangles"] = mesh.get_num_nanite_triangles()
    except Exception:
        pass
    return out
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

/**
 * Reject anything that is not a plausible console-variable / ini key before it
 * reaches a file write. These land verbatim in DefaultEngine.ini and are echoed
 * into a console command, so an unchecked value is an injection.
 */
export const CVAR_KEY_RE = /^[A-Za-z][A-Za-z0-9_.]{0,127}$/;

/** @param {string} key */
export function isValidCvarKey(key) {
  return typeof key === "string" && CVAR_KEY_RE.test(key);
}

/**
 * ini values are one line each, so a newline in a value would forge a second
 * setting. Values also feed `execute_console_command`.
 * @param {*} value
 */
export function isValidCvarValue(value) {
  if (value === null) return true;
  if (typeof value === "number" || typeof value === "boolean") return true;
  return typeof value === "string" && !/[\r\n\[\]]/.test(value);
}

// ---------------------------------------------------------------------------
// Nanite
// ---------------------------------------------------------------------------

/**
 * Turn Nanite on (or off) for static meshes and tune how it simplifies them.
 *
 * @param {object}   opts
 * @param {string[]} opts.static_meshes
 * @param {boolean}  [opts.enabled]
 * @param {number}   [opts.position_precision]
 * @param {number}   [opts.normal_precision]
 * @param {number}   [opts.tangent_precision]
 * @param {number}   [opts.keep_percent_triangles]
 * @param {number}   [opts.trim_relative_error]
 * @param {string}   [opts.fallback_target] - auto | percent_triangles | relative_error
 * @param {number}   [opts.fallback_percent_triangles]
 * @param {number}   [opts.fallback_relative_error]
 * @param {boolean}  [opts.explicit_tangents]
 * @param {string}   [opts.shape_preservation] - none | preserve_area | voxelize
 * @param {number}   [opts.max_edge_length_factor]
 * @param {boolean}  [opts.save=true]
 */
export async function naniteConfigure({
  static_meshes, enabled, position_precision, normal_precision, tangent_precision,
  keep_percent_triangles, trim_relative_error, fallback_target,
  fallback_percent_triangles, fallback_relative_error, explicit_tangents,
  shape_preservation, max_edge_length_factor, save,
} = {}) {
  return runPy(`
${pyArgs({
  static_meshes: static_meshes || [],
  enabled, position_precision, normal_precision, tangent_precision,
  keep_percent_triangles, trim_relative_error,
  fallback_target: fallback_target ? String(fallback_target).toLowerCase() : undefined,
  fallback_percent_triangles, fallback_relative_error, explicit_tangents,
  shape_preservation: shape_preservation ? String(shape_preservation).toLowerCase() : undefined,
  max_edge_length_factor, save: save !== false,
})}
if not args["static_meshes"]:
    return ngg_error("static_meshes[] is empty — nothing to configure")

if args.get("fallback_target") is not None and args["fallback_target"] not in FALLBACK_TARGETS:
    return ngg_error("fallback_target must be one of: " + ", ".join(sorted(FALLBACK_TARGETS)))
if args.get("shape_preservation") is not None and args["shape_preservation"] not in SHAPE_PRESERVATION:
    return ngg_error("shape_preservation must be one of: " + ", ".join(sorted(SHAPE_PRESERVATION)))

# name in args -> name on FMeshNaniteSettings, for the plainly-typed fields.
# The two enums are handled separately below.
FIELDS = [
    ("enabled", "enabled", bool),
    ("position_precision", "position_precision", int),
    ("normal_precision", "normal_precision", int),
    ("tangent_precision", "tangent_precision", int),
    ("keep_percent_triangles", "keep_percent_triangles", float),
    ("trim_relative_error", "trim_relative_error", float),
    ("fallback_percent_triangles", "fallback_percent_triangles", float),
    ("fallback_relative_error", "fallback_relative_error", float),
    ("explicit_tangents", "explicit_tangents", bool),
    ("max_edge_length_factor", "max_edge_length_factor", float),
]

ENUM_FIELDS = [
    ("fallback_target", "fallback_target", FALLBACK_TARGETS),
    ("shape_preservation", "shape_preservation", SHAPE_PRESERVATION),
]

requested = [a for a, _, _ in FIELDS if args.get(a) is not None]
requested += [a for a, _, _ in ENUM_FIELDS if args.get(a) is not None]
if not requested:
    return ngg_error("no settings given — pass at least one, e.g. enabled:true")

sub = mesh_subsystem()
lib = unreal.EditorAssetLibrary
results = []

for path in args["static_meshes"]:
    mesh, err = load_static_mesh(path)
    if err:
        return ngg_error(err)

    settings = sub.get_nanite_settings(mesh)
    before = nanite_summary(mesh, settings)

    for arg_name, prop, cast in FIELDS:
        if args.get(arg_name) is None:
            continue
        settings.set_editor_property(prop, cast(args[arg_name]))
    for arg_name, prop, table in ENUM_FIELDS:
        if args.get(arg_name) is None:
            continue
        settings.set_editor_property(prop, table[args[arg_name]])

    # apply_changes rebuilds the mesh; without it the asset keeps the old build
    # and the new settings only show up on the next unrelated rebuild.
    sub.set_nanite_settings(mesh, settings, apply_changes=True)

    saved = None
    if args["save"]:
        saved = lib.save_asset(path, only_if_is_dirty=False)

    results.append({
        "static_mesh": path,
        "before": before,
        "after": nanite_summary(mesh),
        "saved": saved,
    })

ngg_result({"ok": True, "applied": requested, "meshes": results})
`);
}

// ---------------------------------------------------------------------------
// Lumen / post process
// ---------------------------------------------------------------------------

/**
 * Configure Lumen global illumination and reflections on a post-process volume
 * in the open level, creating an unbounded one if there is nothing to write to.
 *
 * @param {object}  opts
 * @param {string}  [opts.volume_label]      - target a specific volume by label
 * @param {boolean} [opts.create_if_missing=true]
 * @param {string}  [opts.global_illumination] - lumen | screen_space | none | plugin
 * @param {string}  [opts.reflections]         - lumen | screen_space | none
 * @param {string}  [opts.ray_lighting_mode]   - default | surface_cache | hit_lighting | hit_lighting_for_reflections
 * @param {number}  [opts.lumen_scene_lighting_quality]
 * @param {number}  [opts.lumen_scene_detail]
 * @param {number}  [opts.lumen_scene_view_distance]
 * @param {number}  [opts.lumen_final_gather_quality]
 * @param {number}  [opts.lumen_reflection_quality]
 * @param {number}  [opts.lumen_max_reflection_bounces]
 * @param {number}  [opts.lumen_max_trace_distance]
 * @param {object}  [opts.advanced] - any other PostProcessSettings property
 * @param {boolean} [opts.save=true]
 */
export async function lumenConfigure({
  volume_label, create_if_missing, global_illumination, reflections, ray_lighting_mode,
  lumen_scene_lighting_quality, lumen_scene_detail, lumen_scene_view_distance,
  lumen_final_gather_quality, lumen_reflection_quality, lumen_max_reflection_bounces,
  lumen_max_trace_distance, advanced, save,
} = {}) {
  return runPy(`
${pyArgs({
  volume_label,
  create_if_missing: create_if_missing !== false,
  global_illumination: global_illumination ? String(global_illumination).toLowerCase() : undefined,
  reflections: reflections ? String(reflections).toLowerCase() : undefined,
  ray_lighting_mode: ray_lighting_mode ? String(ray_lighting_mode).toLowerCase() : undefined,
  numbers: {
    lumen_scene_lighting_quality, lumen_scene_detail, lumen_scene_view_distance,
    lumen_final_gather_quality, lumen_reflection_quality,
    lumen_max_reflection_bounces, lumen_max_trace_distance,
  },
  advanced: advanced || {},
  save: save !== false,
})}
if args.get("global_illumination") is not None and args["global_illumination"] not in GI_METHODS:
    return ngg_error("global_illumination must be one of: " + ", ".join(sorted(GI_METHODS)))
if args.get("reflections") is not None and args["reflections"] not in REFLECTION_METHODS:
    return ngg_error("reflections must be one of: " + ", ".join(sorted(REFLECTION_METHODS)))
if args.get("ray_lighting_mode") is not None and args["ray_lighting_mode"] not in RAY_LIGHTING_MODES:
    return ngg_error("ray_lighting_mode must be one of: " + ", ".join(sorted(RAY_LIGHTING_MODES)))

wants = [k for k, v in args["numbers"].items() if v is not None]
if (args.get("global_illumination") is None and args.get("reflections") is None
        and args.get("ray_lighting_mode") is None and not wants and not args["advanced"]):
    return ngg_error("nothing to change — pass global_illumination, reflections, "
                     "ray_lighting_mode, a lumen_* number, or advanced{}")

world = editor_world()
if world is None:
    return ngg_error("no level is open in the editor")

eas = actor_subsystem()
volumes = [a for a in eas.get_all_level_actors() if isinstance(a, unreal.PostProcessVolume)]

created = False
target = None
if args.get("volume_label"):
    for v in volumes:
        if str(v.get_actor_label()) == args["volume_label"]:
            target = v
            break
    if target is None:
        return ngg_error("no PostProcessVolume labelled '" + args["volume_label"] + "' in this level. "
                         "Present: " + (", ".join(sorted(str(v.get_actor_label()) for v in volumes)) or "(none)"))
else:
    # Prefer an unbounded volume: it is the one that affects the whole level.
    for v in volumes:
        if bool(v.get_editor_property("unbound")):
            target = v
            break
    if target is None:
        if not args["create_if_missing"]:
            return ngg_error("this level has no unbounded PostProcessVolume and create_if_missing "
                             "is false. Bounded volumes present: " +
                             (", ".join(sorted(str(v.get_actor_label()) for v in volumes)) or "(none)"))
        target = eas.spawn_actor_from_class(unreal.PostProcessVolume, unreal.Vector(0.0, 0.0, 0.0))
        if target is None:
            return ngg_error("could not spawn a PostProcessVolume in this level")
        target.set_actor_label("NGG_GlobalPostProcess")
        target.set_editor_property("unbound", True)
        created = True

settings = target.get_editor_property("settings")
flags = []

if args.get("global_illumination") is not None:
    f = pp_set(settings, "dynamic_global_illumination_method", GI_METHODS[args["global_illumination"]])
    if f: flags.append(f)
if args.get("reflections") is not None:
    f = pp_set(settings, "reflection_method", REFLECTION_METHODS[args["reflections"]])
    if f: flags.append(f)
if args.get("ray_lighting_mode") is not None:
    f = pp_set(settings, "lumen_ray_lighting_mode", RAY_LIGHTING_MODES[args["ray_lighting_mode"]])
    if f: flags.append(f)

for name in wants:
    value = args["numbers"][name]
    if name == "lumen_max_reflection_bounces":
        value = int(value)
    else:
        value = float(value)
    try:
        f = pp_set(settings, name, value)
    except Exception as e:
        return ngg_error("could not set " + name + ": " + str(e))
    if f: flags.append(f)

for name, value in args["advanced"].items():
    try:
        f = pp_set(settings, name, value)
    except Exception as e:
        return ngg_error("advanced property '" + str(name) + "' was rejected: " + str(e) +
                         ". Names are PostProcessSettings properties in snake_case, "
                         "e.g. 'lumen_surface_cache_resolution'.")
    if f: flags.append(f)

# The struct came back as a copy; without this write-back nothing above sticks.
target.set_editor_property("settings", settings)

saved = None
if args["save"]:
    saved = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level()

ngg_result({
    "ok": True,
    "volume": str(target.get_actor_label()),
    "created_volume": created,
    "unbound": bool(target.get_editor_property("unbound")),
    "overrides_set": flags,
    "settings": pp_snapshot(target.get_editor_property("settings"), LUMEN_REPORTED),
    "saved": saved,
})
`);
}

// ---------------------------------------------------------------------------
// Reading what is actually in effect
// ---------------------------------------------------------------------------

/**
 * Report the rendering state of the open level: post-process volumes and what
 * they override, the live console variables, the project's renderer ini, and
 * optionally the Nanite state of named static meshes.
 *
 * @param {object}   opts
 * @param {string[]} [opts.static_meshes]
 */
export async function renderRead({ static_meshes } = {}) {
  return runPy(`
${pyArgs({ static_meshes: static_meshes || [], cvars: REPORTED_CVARS, section: RENDERER_SETTINGS_SECTION })}
world = editor_world()
if world is None:
    return ngg_error("no level is open in the editor")

volumes = []
for a in actor_subsystem().get_all_level_actors():
    if not isinstance(a, unreal.PostProcessVolume):
        continue
    ps = a.get_editor_property("settings")
    volumes.append({
        "label": str(a.get_actor_label()),
        "unbound": bool(a.get_editor_property("unbound")),
        "enabled": bool(a.get_editor_property("enabled")),
        "priority": float(a.get_editor_property("priority")),
        "blend_radius": float(a.get_editor_property("blend_radius")),
        "blend_weight": float(a.get_editor_property("blend_weight")),
        "overridden": {k: v["value"] for k, v in pp_snapshot(ps, LUMEN_REPORTED).items()
                       if v["overridden"]},
    })

cvars = {}
for name in args["cvars"]:
    try:
        cvars[name] = unreal.SystemLibrary.get_console_variable_int_value(name)
    except Exception as e:
        cvars[name] = "unreadable: " + str(e)

ini_path = config_path("DefaultEngine.ini")
project = read_ini_section(ini_path, args["section"])

meshes = []
for path in args["static_meshes"]:
    mesh, err = load_static_mesh(path)
    if err:
        return ngg_error(err)
    entry = {"static_mesh": path}
    entry.update(nanite_summary(mesh))
    meshes.append(entry)

result = {
    "ok": True,
    "level": str(world.get_name()),
    "post_process_volumes": volumes,
    "console_variables": cvars,
    "project_renderer_settings": project if project is not None else {},
    "project_ini": ini_path,
}
if meshes:
    result["static_meshes"] = meshes
if project is None:
    result["note"] = "DefaultEngine.ini not found at " + ini_path
ngg_result(result)
`);
}

/**
 * Read/modify `[/Script/Engine.RendererSettings]` in the project's
 * DefaultEngine.ini — Nanite, Lumen, virtual shadow maps, Substrate and the
 * rest of the project-wide renderer switches.
 *
 * These are ini text, not an exposed settings object: UE 5.8 has no
 * `unreal.RendererSettings` binding, so there is no CDO to set and save. That
 * also means the running editor does not pick them up — hence `apply_live`,
 * which pushes the same key as a console variable so the viewport changes now,
 * while the ini makes it survive a restart.
 *
 * @param {object}  opts
 * @param {object}  opts.settings      - {key: value}; a null value removes the key
 * @param {boolean} [opts.apply_live=true]
 */
export async function renderProjectSettings({ settings, apply_live } = {}) {
  const entries = settings && typeof settings === "object" ? Object.entries(settings) : [];
  if (entries.length === 0) {
    return { ok: false, error: "settings{} is empty — nothing to change. Use ue5_render_read to see the current values." };
  }
  for (const [key, value] of entries) {
    if (!isValidCvarKey(key)) {
      return {
        ok: false,
        error: `'${key}' is not a valid renderer setting name. Expected something like 'r.DynamicGlobalIlluminationMethod' — letters, digits, dots and underscores only.`,
      };
    }
    if (!isValidCvarValue(value)) {
      return { ok: false, error: `the value for '${key}' contains a line break or bracket, which would corrupt the ini file` };
    }
  }

  return runPy(`
${pyArgs({ settings, apply_live: apply_live !== false, section: RENDERER_SETTINGS_SECTION })}
ini_path = config_path("DefaultEngine.ini")

changes = {}
for key, value in args["settings"].items():
    changes[key] = None if value is None else ini_value(value)

actions = write_ini_section(ini_path, args["section"], changes)

applied_live = []
if args["apply_live"]:
    world = editor_world()
    for key, value in args["settings"].items():
        if value is None:
            continue
        try:
            unreal.SystemLibrary.execute_console_command(world, key + " " + ini_value(value))
            applied_live.append({"cvar": key, "now": unreal.SystemLibrary.get_console_variable_int_value(key)})
        except Exception as e:
            applied_live.append({"cvar": key, "error": str(e)})

ngg_result({
    "ok": True,
    "ini": ini_path,
    "section": args["section"],
    "actions": actions,
    "applied_live": applied_live,
    "section_now": read_ini_section(ini_path, args["section"]),
    "note": "The running editor keeps its own copy of DefaultEngine.ini; the file change "
            "takes effect on the next editor start. applied_live pushes the same value as a "
            "console variable so the viewport updates immediately, but some renderer settings "
            "are read-only at runtime and will only report their old value.",
})
`);
}
