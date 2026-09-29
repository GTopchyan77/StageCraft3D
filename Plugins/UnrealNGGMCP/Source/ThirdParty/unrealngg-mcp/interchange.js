// Copyright 2025-2026 NGG. All Rights Reserved.
// interchange.js — unrealngg-mcp
//
// Importing through Interchange, UE's current import framework, rather than the
// legacy UAssetImportTask path that ue5_import_asset uses.
//
// The difference that matters: Interchange runs a *pipeline* over the file, and
// the pipeline is where every real import decision lives — combine meshes or
// not, build Nanite, which skeleton to bind to, whether to create materials,
// LOD handling, import offsets. UAssetImportTask exposes almost none of that
// from script, so ue5_import_asset can only drop a file in a folder.
//
// Runs through POST /editor/exec_python, like rendering.js / assetconfig.js.
//
// Probed against a live UE 5.8.1 editor before this was written:
//
//   * The pipeline is a tree of nested settings objects (mesh_pipeline,
//     material_pipeline, common_meshes_properties, ...) holding well over a
//     hundred properties between them. Rather than mirror that into a tool
//     schema — which would cost more handshake tokens than the rest of this
//     server's rigging and audio tools put together — settings are addressed by
//     dotted path, and ue5_interchange_inspect lists the real names.
//   * ImportAssetParameters.override_pipelines is an array of soft object
//     paths, so a pipeline configured in memory has to be referenced by its
//     transient path. Passing the object itself does not work.
//   * is_automated must be set, or an import from a headless request pops a
//     modal dialog and the bridge request hangs until somebody clicks it.

import * as ue5 from "./ue5client.js";

const RESULT_BEGIN = "__NGG_ICH_RESULT_BEGIN__";
const RESULT_END   = "__NGG_ICH_RESULT_END__";

/**
 * Friendly names for the decisions almost every import makes, mapped onto their
 * real pipeline property paths. Anything not here is still reachable through
 * `pipeline_settings`.
 */
export const IMPORT_SHORTCUTS = {
  import_static_meshes:   "mesh_pipeline.import_static_meshes",
  import_skeletal_meshes: "mesh_pipeline.import_skeletal_meshes",
  import_morph_targets:   "mesh_pipeline.import_morph_targets",
  // 5.8 renamed bImportCollision -> Collision and replaced bCombineStaticMeshes
  // with the CombineStaticMeshesBehavior enum. Reading either old name throws.
  import_collision:       "mesh_pipeline.collision",
  combine_static_meshes_behavior: "mesh_pipeline.combine_static_meshes_behavior",
  combine_skeletal_meshes_behavior: "mesh_pipeline.combine_skeletal_meshes_behavior",
  build_nanite:           "mesh_pipeline.build_nanite",
  create_physics_asset:   "mesh_pipeline.create_physics_asset",
  import_lods:            "common_meshes_properties.import_lods",
  import_sockets:         "common_meshes_properties.import_sockets",
  recompute_normals:      "common_meshes_properties.recompute_normals",
  recompute_tangents:     "common_meshes_properties.recompute_tangents",
  import_animations:      "animation_pipeline.import_animations",
  import_materials:       "material_pipeline.import_materials",
  create_new_materials:   "material_pipeline.create_new_materials",
  reuse_existing_materials: "material_pipeline.reuse_existing_materials",
  parent_material:        "material_pipeline.parent_material",
  skeleton:               "common_skeletal_meshes_and_animations_properties.skeleton",
  import_offset_translation:   "import_offset_translation",
  import_offset_rotation:      "import_offset_rotation",
  import_offset_uniform_scale: "import_offset_uniform_scale",
};

const PY_PRELUDE = `
import unreal, json, base64, traceback, os

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

def manager():
    return unreal.InterchangeManager.get_interchange_manager_scripted()

# Where a pipeline asset gets copied when per-import settings are layered on top
# of it. Created and removed inside the same call.
SCRATCH_DIR = "/Game/__NGGInterchangeTmp__"

# Names present on every UObject binding, plus the pipeline plumbing that is not
# a setting. Filtered out so the reported property list is the settings only.
_NOISE = set(dir(unreal.Object)) | {
    "scripted_execute_export_pipeline", "scripted_execute_pipeline",
    "scripted_execute_post_broadcast_pipeline", "scripted_execute_post_factory_pipeline",
    "scripted_execute_post_import_pipeline", "scripted_get_pipeline_display_name",
    "scripted_set_reimport_source_index", "does_property_states_exist",
    "find_or_add_property_states", "get_support_asset_classes", "is_reimport_context",
    "support_reimport", "pipeline_display_name",
}

def settable_props(obj):
    """Property names that can actually be read and written.

    dir() also lists properties UE has deprecated, and reading one of those
    raises rather than returning a value — so a name in dir() is not evidence
    that it is usable. 5.8 deprecated three of them on the generic pipeline
    (import_collision, combine_static_meshes, auto_detect_mesh_type), which is
    exactly the kind of thing a caller would otherwise learn from a confusing
    error. Probing filters them out of every list this module reports.
    """
    out = []
    for n in dir(obj):
        if n.startswith("_") or n in _NOISE:
            continue
        try:
            obj.get_editor_property(n)
        except Exception:
            continue
        out.append(n)
    return sorted(out)

def deprecation_note(obj, name):
    """Return UE's own deprecation message for a property, or None."""
    try:
        obj.get_editor_property(name)
        return None
    except Exception as e:
        text = str(e)
        return text if "deprecated" in text else None

def enum_names(enum_type):
    return sorted(n for n in dir(enum_type) if n.isupper())

def coerce(current, value, where):
    """Cast an incoming JSON value to whatever the property already holds."""
    if isinstance(current, bool):
        return bool(value), None
    if isinstance(current, int) and not isinstance(current, bool):
        return int(value), None
    if isinstance(current, float):
        return float(value), None
    if isinstance(current, str):
        return str(value), None
    # Enums report as their own type; match by name, case/underscore-insensitive.
    if isinstance(current, unreal.EnumBase):
        names = enum_names(type(current))
        want = str(value).upper().replace(" ", "_")
        for n in names:
            if n == want or n.replace("_", "") == want.replace("_", ""):
                return getattr(type(current), n), None
        return None, (where + ": '" + str(value) + "' is not a valid value. Options: " +
                      ", ".join(names))
    if isinstance(current, unreal.Vector):
        if not isinstance(value, list) or len(value) != 3:
            return None, where + ": expected [x, y, z]"
        return unreal.Vector(float(value[0]), float(value[1]), float(value[2])), None
    if isinstance(current, unreal.Rotator):
        if not isinstance(value, list) or len(value) != 3:
            return None, where + ": expected [pitch, yaw, roll]"
        return unreal.Rotator(float(value[0]), float(value[1]), float(value[2])), None
    # Object-valued properties (skeleton, parent_material, physics_asset, ...)
    # take a content path.
    if current is None or isinstance(current, unreal.Object):
        if value is None:
            return None, None
        loaded = unreal.load_asset(str(value))
        if loaded is None:
            return None, where + ": no asset found at '" + str(value) + "'"
        return loaded, None
    return value, None

def apply_setting(pipeline, dotted, value):
    """Set one dotted pipeline property. Returns (applied_value_str, error)."""
    parts = str(dotted).split(".")
    obj = pipeline
    for i, part in enumerate(parts[:-1]):
        try:
            nxt = obj.get_editor_property(part)
        except Exception:
            return None, ("'" + ".".join(parts[:i + 1]) + "' is not a property of " +
                          type(obj).__name__ + ". Available here: " + ", ".join(settable_props(obj)))
        if nxt is None:
            return None, ("'" + ".".join(parts[:i + 1]) + "' is empty, so '" + dotted +
                          "' cannot be reached")
        obj = nxt

    leaf = parts[-1]
    try:
        current = obj.get_editor_property(leaf)
    except Exception:
        # The name may exist but be deprecated, in which case UE's own message
        # names the replacement — far more useful than "no such property".
        note = deprecation_note(obj, leaf)
        if note:
            return None, "'" + dotted + "' is deprecated in this engine version. " + note
        return None, ("'" + dotted + "' is not a property of " + type(obj).__name__ +
                      ". Available here: " + ", ".join(settable_props(obj)))

    cast, err = coerce(current, value, dotted)
    if err:
        return None, err
    try:
        obj.set_editor_property(leaf, cast)
    except Exception as e:
        return None, "'" + dotted + "' was rejected: " + str(e)
    return s(obj.get_editor_property(leaf)), None

def describe_pipeline(pipeline):
    """Group the settable property names by the sub-object that owns them.

    Root-level properties keep no prefix; everything reached through a nested
    settings object is addressed as 'group.property'.
    """
    groups = {}
    root = []
    for name in settable_props(pipeline):
        try:
            child = pipeline.get_editor_property(name)
        except Exception:
            root.append(name)
            continue
        if isinstance(child, unreal.Object) and settable_props(child):
            groups[name] = settable_props(child)
        else:
            root.append(name)
    groups["(root)"] = root
    return groups
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
 * Merge the friendly shortcut arguments into the dotted-path settings map.
 * Explicit `pipeline_settings` entries win, so a caller can always override.
 *
 * @param {object} shortcuts
 * @param {object} settings
 * @returns {object}
 */
export function mergeSettings(shortcuts = {}, settings = {}) {
  const merged = {};
  for (const [name, path] of Object.entries(IMPORT_SHORTCUTS)) {
    if (shortcuts[name] !== undefined && shortcuts[name] !== null) merged[path] = shortcuts[name];
  }
  return { ...merged, ...settings };
}

// ---------------------------------------------------------------------------
// Inspect
// ---------------------------------------------------------------------------

/**
 * Report whether Interchange can import a file, which translator handles it,
 * and the pipeline property names available for `pipeline_settings`.
 *
 * @param {object}  opts
 * @param {string}  [opts.source_file] - absolute path of the file to test
 * @param {boolean} [opts.list_pipeline_properties=true]
 */
export async function interchangeInspect({ source_file, list_pipeline_properties } = {}) {
  return runPy(`
${pyArgs({ source_file, list_pipeline_properties: list_pipeline_properties !== false })}
result = {"ok": True}

if args.get("source_file"):
    path = args["source_file"]
    if not os.path.isfile(path):
        return ngg_error("no file at " + path + " (the path is read by the editor process, "
                         "so it must be reachable from the machine the editor runs on)")
    im = manager()
    sd = im.create_source_data(path)
    can = bool(im.can_translate_source_data(sd))
    translator = im.get_translator_for_source_data(sd) if can else None
    result["source_file"] = path
    result["can_import"] = can
    result["translator"] = type(translator).__name__ if translator is not None else None
    if not can:
        result["hint"] = ("No Interchange translator claims this file. Formats depend on which "
                          "Interchange plugins are enabled — FBX, glTF, USD and common image "
                          "formats are the usual set. ue5_import_asset still uses the legacy "
                          "importer and may accept it.")

if args["list_pipeline_properties"]:
    pipeline = unreal.InterchangeGenericAssetsPipeline()
    result["pipeline_properties"] = describe_pipeline(pipeline)
    result["pipeline_note"] = ("Address these from ue5_interchange_import as "
                               "'group.property', e.g. 'mesh_pipeline.build_nanite'. "
                               "Root-level names need no prefix.")

ngg_result(result)
`);
}

// ---------------------------------------------------------------------------
// Import
// ---------------------------------------------------------------------------

/**
 * Import a source file through Interchange, with pipeline settings applied.
 *
 * @param {object}  opts
 * @param {string}  opts.source_file       - absolute path on the editor's machine
 * @param {string}  opts.destination_path  - content folder, e.g. '/Game/Imported'
 * @param {string}  [opts.asset_name]
 * @param {string}  [opts.pipeline_asset]  - a saved pipeline to use as the base
 * @param {object}  [opts.pipeline_settings] - {"mesh_pipeline.build_nanite": true}
 * @param {boolean} [opts.replace_existing=true]
 * @param {boolean} [opts.import_static_meshes]
 * @param {boolean} [opts.import_skeletal_meshes]
 * @param {boolean} [opts.import_animations]
 * @param {boolean} [opts.import_materials]
 * @param {boolean} [opts.import_lods]
 * @param {boolean} [opts.import_sockets]
 * @param {boolean} [opts.import_collision]
 * @param {boolean} [opts.import_morph_targets]
 * @param {boolean} [opts.combine_static_meshes]
 * @param {boolean} [opts.build_nanite]
 * @param {boolean} [opts.create_physics_asset]
 * @param {boolean} [opts.create_new_materials]
 * @param {boolean} [opts.reuse_existing_materials]
 * @param {boolean} [opts.recompute_normals]
 * @param {boolean} [opts.recompute_tangents]
 * @param {string}  [opts.skeleton]        - existing skeleton to bind to
 * @param {string}  [opts.parent_material]
 * @param {number[]}[opts.import_offset_translation]
 * @param {number[]}[opts.import_offset_rotation]
 * @param {number}  [opts.import_offset_uniform_scale]
 */
export async function interchangeImport(opts = {}) {
  const {
    source_file, destination_path, asset_name, pipeline_asset,
    pipeline_settings, replace_existing,
  } = opts;

  const settings = mergeSettings(opts, pipeline_settings || {});

  // Naming the result is genuinely awkward in 5.8. Verified against the live
  // editor: ImportAssetParameters.destination_name does not rename a plain asset
  // import, and the pipeline's own asset_name is honoured for meshes but ignored
  // for textures (all three of asset_name, use_source_name_for_asset:false and
  // texture_pipeline.asset_name left the file name in place). So the setting is
  // applied for the cases that respect it, and anything still misnamed is
  // renamed afterwards — reported, not silently.
  if (asset_name && settings.asset_name === undefined) settings.asset_name = asset_name;

  return runPy(`
${pyArgs({
  source_file, destination_path, asset_name, pipeline_asset,
  settings, replace_existing: replace_existing !== false,
})}
if not args.get("source_file"):
    return ngg_error("source_file is required (an absolute path on the machine running the editor)")
if not args.get("destination_path"):
    return ngg_error("destination_path is required, e.g. '/Game/Imported'")
if not str(args["destination_path"]).startswith("/"):
    return ngg_error("destination_path must be a content path starting with '/', "
                     "e.g. '/Game/Imported' — not a filesystem path")

if not os.path.isfile(args["source_file"]):
    return ngg_error("no file at " + args["source_file"] + " (the editor process reads this "
                     "path, so it must exist on the editor's machine)")

im = manager()
sd = im.create_source_data(args["source_file"])
if not im.can_translate_source_data(sd):
    return ngg_error("no Interchange translator handles " + args["source_file"] +
                     ". Run ue5_interchange_inspect on it to see what the editor thinks, "
                     "or use ue5_import_asset for the legacy importer.")

# Build a pipeline only when the caller actually asked for settings; otherwise
# leave override_pipelines empty so the project's own default stack applies.
pipeline = None
scratch_pipeline = None
applied = {}
lib = unreal.EditorAssetLibrary

if args.get("pipeline_asset"):
    loaded = unreal.load_asset(args["pipeline_asset"])
    if loaded is None:
        return ngg_error("pipeline asset not found: " + args["pipeline_asset"])
    if not isinstance(loaded, unreal.InterchangePipelineBase):
        return ngg_error(args["pipeline_asset"] + " is a " + type(loaded).__name__ +
                         ", not an Interchange pipeline")
    if args["settings"]:
        # Settings on top of a saved pipeline apply to a throwaway copy, so this
        # import cannot silently rewrite the asset every later import shares.
        scratch_pipeline = SCRATCH_DIR + "/" + loaded.get_name() + "_NGGOverride"
        if lib.does_asset_exist(scratch_pipeline):
            lib.delete_asset(scratch_pipeline)
        pipeline = lib.duplicate_asset(args["pipeline_asset"], scratch_pipeline)
        if pipeline is None:
            return ngg_error("could not copy " + args["pipeline_asset"] +
                             " to apply pipeline_settings on top of it")
    else:
        pipeline = loaded
elif args["settings"]:
    pipeline = unreal.InterchangeGenericAssetsPipeline()

def drop_scratch():
    if scratch_pipeline and lib.does_asset_exist(scratch_pipeline):
        lib.delete_asset(scratch_pipeline)
    if lib.does_directory_exist(SCRATCH_DIR) and not lib.list_assets(SCRATCH_DIR, recursive=True):
        lib.delete_directory(SCRATCH_DIR)

for dotted, value in args["settings"].items():
    got, err = apply_setting(pipeline, dotted, value)
    if err:
        drop_scratch()
        return ngg_error(err)
    applied[dotted] = got

params = unreal.ImportAssetParameters()
# Without is_automated the import opens a modal dialog and this request never
# returns — the bridge would sit waiting for somebody at the editor to click it.
params.set_editor_property("is_automated", True)
params.set_editor_property("replace_existing", bool(args["replace_existing"]))
if pipeline is not None:
    params.set_editor_property("override_pipelines",
                               [unreal.SoftObjectPath(pipeline.get_path_name())])

try:
    imported = im.import_asset(args["destination_path"], sd, params)
finally:
    drop_scratch()

objects = []
for obj in (imported or []):
    if obj is None:
        continue
    objects.append({
        "asset": str(obj.get_path_name()).split(".")[0],
        "class": type(obj).__name__,
    })

if not objects:
    return ngg_error("Interchange ran but produced no assets from " + args["source_file"] +
                     ". The usual causes are a pipeline that switched everything off "
                     "(import_static_meshes / import_materials / import_animations) or a "
                     "source file with nothing of that kind in it.")

# See the note in the JS wrapper: the pipeline's asset_name is honoured for some
# asset types and ignored for others, so finish the job here when it was ignored.
warnings = []
wanted = args.get("asset_name")
if wanted:
    misnamed = [o for o in objects if o["asset"].rsplit("/", 1)[-1] != wanted]
    if not misnamed:
        pass
    elif len(objects) > 1:
        warnings.append("asset_name was not applied: the import produced " + str(len(objects)) +
                        " assets and renaming one of them would be arbitrary. Names: " +
                        ", ".join(o["asset"].rsplit("/", 1)[-1] for o in objects))
    else:
        old = objects[0]["asset"]
        new = args["destination_path"].rstrip("/") + "/" + wanted
        if lib.rename_asset(old, new):
            objects[0]["asset"] = new
            objects[0]["renamed_after_import"] = True
        else:
            warnings.append("the asset imported as " + old + " but could not be renamed to " + new)

result = {
    "ok": True,
    "source_file": args["source_file"],
    "destination_path": args["destination_path"],
    "pipeline": ("asset: " + args["pipeline_asset"]) if args.get("pipeline_asset")
                else ("configured in memory" if pipeline is not None else "project default stack"),
    "settings_applied": applied,
    "imported": objects,
}
if warnings:
    result["warnings"] = warnings
ngg_result(result)
`);
}

// ---------------------------------------------------------------------------
// Reusable pipeline assets
// ---------------------------------------------------------------------------

/**
 * Save a configured Interchange pipeline as an asset, so the same import
 * settings can be reused (and version-controlled) instead of repeated per call.
 *
 * @param {object}  opts
 * @param {string}  opts.asset_path
 * @param {object}  [opts.pipeline_settings]
 * @param {boolean} [opts.overwrite=false]
 */
export async function interchangePipelineCreate(opts = {}) {
  const { asset_path, pipeline_settings, overwrite } = opts;
  const settings = mergeSettings(opts, pipeline_settings || {});

  return runPy(`
${pyArgs({ asset_path, settings, overwrite: !!overwrite })}
if not args.get("asset_path"):
    return ngg_error("asset_path is required")
if not args["settings"]:
    return ngg_error("no settings given — a pipeline asset with nothing set is the project "
                     "default, which ue5_interchange_import already uses when you pass none")

lib = unreal.EditorAssetLibrary
exists = lib.does_asset_exist(args["asset_path"])
if exists and not args["overwrite"]:
    return ngg_error("an asset already exists at " + args["asset_path"] +
                     " — pass overwrite:true to change it in place")

if exists:
    pipeline = unreal.load_asset(args["asset_path"])
    if not isinstance(pipeline, unreal.InterchangePipelineBase):
        return ngg_error(args["asset_path"] + " is a " + type(pipeline).__name__ +
                         ", not an Interchange pipeline")
    created = False
else:
    name, folder = split_path(args["asset_path"])
    pipeline = asset_tools().create_asset(name, folder, unreal.InterchangeGenericAssetsPipeline, None)
    if pipeline is None:
        return ngg_error("could not create an Interchange pipeline at " + args["asset_path"])
    created = True

applied = {}
for dotted, value in args["settings"].items():
    got, err = apply_setting(pipeline, dotted, value)
    if err:
        if created:
            lib.delete_asset(args["asset_path"])
            return ngg_error(err + " (the new pipeline asset was removed again)")
        return ngg_error(err)
    applied[dotted] = got

saved = lib.save_asset(args["asset_path"], only_if_is_dirty=False)

ngg_result({
    "ok": True,
    "asset": args["asset_path"],
    "created": created,
    "settings_applied": applied,
    "saved": saved,
})
`);
}
