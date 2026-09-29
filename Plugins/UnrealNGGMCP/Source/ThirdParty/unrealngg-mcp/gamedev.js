// Copyright 2025-2026 NGG. All Rights Reserved.
// gamedev.js — unrealngg-mcp
// Game-development workflow endpoints implemented without new C++ plugin code:
// thin Python scripts shipped to the editor via POST /editor/exec_python (same
// pattern as pcg.js), plus Node-side filesystem access for log/ini work.
//
// Endpoints shipped here:
//   Viewport & feedback loop
//     gd_set_viewport_camera
//     gd_viewport_screenshot
//     gd_get_log            (Node-side: reads <Project>/Saved/Logs)
//   Play-in-editor
//     gd_pie_start
//     gd_pie_stop
//     gd_pie_status
//   DataTables
//     gd_datatable_create
//     gd_datatable_read
//     gd_datatable_set_rows
//   Material node graphs
//     gd_material_add_expression
//     gd_material_connect
//   Sequencer / cinematics
//     gd_sequencer_create
//     gd_sequencer_bind_actor
//     gd_sequencer_add_camera
//   Audio
//     gd_create_sound_cue
//   Gameplay tags
//     gd_add_gameplay_tags  (Node-side ini edit + editor-side reload attempt)
//   Level actors
//     gd_add_component_to_actor

import fs from "node:fs";
import path from "node:path";
import * as ue5 from "./ue5client.js";

// ---------------------------------------------------------------------------
// Python helpers (same sentinel pattern as pcg.js, distinct markers)
// ---------------------------------------------------------------------------

const RESULT_BEGIN = "__NGG_GD_RESULT_BEGIN__";
const RESULT_END   = "__NGG_GD_RESULT_END__";

const PY_PRELUDE = `
import unreal, json, base64, traceback
def ngg_result(obj):
    print("${RESULT_BEGIN}" + json.dumps(obj) + "${RESULT_END}")
def ngg_error(msg):
    ngg_result({"ok": False, "error": msg})
`;

// base64-decode payloads inside Python so paths/labels with quotes,
// backslashes, or newlines can't corrupt or inject into the script.
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
    "try:\n    _ngg_main()\nexcept Exception as _e:\n    ngg_error(str(_e) + '\\n' + traceback.format_exc())\n";

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
// Viewport camera + screenshot
// ---------------------------------------------------------------------------

/**
 * Point the level-editor viewport camera. Either give explicit location/
 * rotation, or `focus_actor` (label) to frame an actor pilot-style.
 */
export async function setViewportCamera({ location, rotation, focus_actor, distance } = {}) {
  return runPy(`
${pyArgs({ location, rotation, focus_actor, distance })}
# UE5 moved viewport camera control to UnrealEditorSubsystem; older builds
# expose it on EditorLevelLibrary. Support both.
ues = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
def set_cam(v, r):
    if ues and hasattr(ues, "set_level_viewport_camera_info"):
        ues.set_level_viewport_camera_info(v, r)
    else:
        unreal.EditorLevelLibrary.set_level_viewport_camera_info(v, r)
eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
if args.get("focus_actor"):
    target = None
    for a in eas.get_all_level_actors():
        if a.get_actor_label() == args["focus_actor"]:
            target = a
            break
    if target is None:
        return ngg_error("actor not found: " + args["focus_actor"])
    origin, extent = target.get_actor_bounds(False)
    dist = float(args.get("distance") or max(extent.length() * 3.0, 500.0))
    # Frame from up-back at a 30-degree pitch so the subject reads clearly.
    import math
    pitch_rad = math.radians(30.0)
    cam_loc = unreal.Vector(
        origin.x - dist * math.cos(pitch_rad),
        origin.y,
        origin.z + dist * math.sin(pitch_rad))
    look = unreal.MathLibrary.find_look_at_rotation(cam_loc, origin)
    set_cam(cam_loc, look)
    ngg_result({"ok": True, "framed": args["focus_actor"],
                "camera_location": [cam_loc.x, cam_loc.y, cam_loc.z],
                "camera_rotation": [look.pitch, look.yaw, look.roll]})
else:
    loc = args.get("location") or [0, 0, 300]
    rot = args.get("rotation") or [0, 0, 0]
    v = unreal.Vector(float(loc[0]), float(loc[1]), float(loc[2]))
    r = unreal.Rotator()
    r.pitch = float(rot[0]); r.yaw = float(rot[1]); r.roll = float(rot[2])
    set_cam(v, r)
    ngg_result({"ok": True, "camera_location": loc, "camera_rotation": rot})
`);
}

/**
 * Capture the current editor camera view to a PNG and return its absolute
 * path (agents can then Read it). Implemented with a transient SceneCapture2D
 * + render target rather than viewport screenshots: SceneCapture renders on
 * demand, so it works even when the editor window is backgrounded and its
 * viewport is render-throttled (where HighResShot can stall for minutes).
 */
export async function viewportScreenshot({ filename, resolution_x, resolution_y } = {}) {
  const name = (filename || `ngg_shot_${Date.now()}`).replace(/[^\w.-]/g, "_");
  const result = await runPy(`
${pyArgs({ name, rx: resolution_x || 1920, ry: resolution_y || 1080 })}
import os
ues = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
loc, rot = ues.get_level_viewport_camera_info()
world = ues.get_editor_world()
cap = eas.spawn_actor_from_class(unreal.SceneCapture2D, loc, rot)
try:
    comp = cap.get_editor_property("capture_component2d")
    rt = unreal.RenderingLibrary.create_render_target2d(
        world, int(args["rx"]), int(args["ry"]),
        unreal.TextureRenderTargetFormat.RTF_RGBA8,
        unreal.LinearColor(0.0, 0.0, 0.0, 1.0), False)
    comp.set_editor_property("texture_target", rt)
    comp.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
    comp.capture_scene()
    out_dir = os.path.join(unreal.SystemLibrary.get_project_saved_directory(), "Screenshots", "NGG")
    fname = args["name"] + ".png"
    target = os.path.join(out_dir, fname)
    if os.path.exists(target):
        os.remove(target)
    unreal.RenderingLibrary.export_render_target(world, rt, out_dir, fname)
finally:
    eas.destroy_actor(cap)
ngg_result({"ok": True, "screenshot": target,
            "camera_location": [loc.x, loc.y, loc.z],
            "camera_rotation": [rot.pitch, rot.yaw, rot.roll]})
`);
  if (!result.ok) return result;

  // export_render_target flushes through the render thread; give the file a
  // few seconds to appear, then report its size.
  const target = result.data.screenshot;
  const deadline = Date.now() + 10000;
  while (Date.now() < deadline) {
    if (fs.existsSync(target) && fs.statSync(target).size > 0) {
      await new Promise((r) => setTimeout(r, 200));
      return {
        ok: true,
        data: { ...result.data, size_bytes: fs.statSync(target).size },
      };
    }
    await new Promise((r) => setTimeout(r, 200));
  }
  return { ok: false, error: `render-target export did not write ${target} within 10s` };
}

// ---------------------------------------------------------------------------
// Output log (Node-side — no editor round-trip)
// ---------------------------------------------------------------------------

/**
 * Read the tail of the project's live editor log with optional filters.
 * @param {object} opts
 *   - lines:     max lines returned after filtering (default 100)
 *   - severity:  "error" | "warning" — keeps Error/Warning lines only
 *   - category:  e.g. "LogBlueprint" — substring match on the log category
 *   - contains:  free-text substring filter
 */
export async function getLog({ lines = 100, severity, category, contains } = {}, projectSavedDir) {
  let logFile;
  if (projectSavedDir) {
    logFile = null; // resolved below from explicit dir
  }
  // Resolve the log path from the editor when available, else walk up from cwd.
  let saved = projectSavedDir;
  if (!saved) {
    try {
      const info = await ue5.getProjectInfo();
      saved = path.join(path.dirname(info.uproject_path), "Saved");
    } catch {
      return { ok: false, error: "bridge unreachable and no saved dir supplied — cannot locate the log file" };
    }
  }
  const logsDir = path.join(saved, "Logs");
  let candidates;
  try {
    candidates = fs.readdirSync(logsDir)
      .filter((f) => f.endsWith(".log"))
      .map((f) => ({ f, m: fs.statSync(path.join(logsDir, f)).mtimeMs }))
      .sort((a, b) => b.m - a.m);
  } catch (err) {
    return { ok: false, error: `cannot read ${logsDir}: ${err.message}` };
  }
  if (!candidates.length) return { ok: false, error: `no .log files in ${logsDir}` };
  logFile = path.join(logsDir, candidates[0].f);

  let text;
  try {
    text = fs.readFileSync(logFile, "utf8");
  } catch (err) {
    return { ok: false, error: `cannot read ${logFile}: ${err.message}` };
  }

  let all = text.split(/\r?\n/);
  if (severity === "error")   all = all.filter((l) => /:\s*Error[: ]/i.test(l) || /\bError:/.test(l));
  if (severity === "warning") all = all.filter((l) => /:\s*(Warning|Error)[: ]/i.test(l) || /\b(Warning|Error):/.test(l));
  if (category) all = all.filter((l) => l.includes(category));
  if (contains) all = all.filter((l) => l.toLowerCase().includes(contains.toLowerCase()));

  const tail = all.slice(-Math.max(1, lines));
  return {
    ok: true,
    data: {
      log_file: logFile,
      matched_lines: all.length,
      returned_lines: tail.length,
      lines: tail,
    },
  };
}

// ---------------------------------------------------------------------------
// Play-in-editor
// ---------------------------------------------------------------------------

export async function pieStart({ simulate } = {}) {
  return runPy(`
${pyArgs({ simulate: !!simulate })}
les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
if les.is_in_play_in_editor():
    return ngg_error("a PIE session is already running — call ue5_pie_stop first")
if args["simulate"]:
    les.editor_play_simulate()
    mode = "simulate"
elif hasattr(les, "editor_request_begin_play"):
    les.editor_request_begin_play()
    mode = "play"
else:
    # Engine version without EditorRequestBeginPlay — Simulate is the
    # closest supported start mode.
    les.editor_play_simulate()
    mode = "simulate (fallback: this engine version cannot start normal Play from script)"
ngg_result({"ok": True, "mode": mode})
`);
}

export async function pieStop() {
  return runPy(`
les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
if not les.is_in_play_in_editor():
    return ngg_error("no PIE session is running")
les.editor_request_end_play()
ngg_result({"ok": True, "stopped": True})
`);
}

export async function pieStatus() {
  return runPy(`
les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
ngg_result({"ok": True, "in_pie": bool(les.is_in_play_in_editor())})
`);
}

// ---------------------------------------------------------------------------
// DataTables
// ---------------------------------------------------------------------------

/**
 * Create a DataTable asset bound to a row struct. Optionally fill it from
 * JSON rows in the same call.
 */
export async function datatableCreate({ asset_path, row_struct, rows, overwrite } = {}) {
  return runPy(`
${pyArgs({ asset_path, row_struct, rows: rows || null, overwrite: !!overwrite })}
lib = unreal.EditorAssetLibrary
p = args["asset_path"]
if lib.does_asset_exist(p):
    if not args["overwrite"]:
        return ngg_error("asset already exists: " + p + " (pass overwrite=true to replace)")
    lib.delete_asset(p)
struct = unreal.load_object(None, args["row_struct"])
if struct is None:
    struct = unreal.load_object(None, "/Script/Engine." + args["row_struct"])
if struct is None:
    return ngg_error("row struct not found: " + args["row_struct"] +
                     " (use the full path, e.g. /Game/Data/S_ItemRow.S_ItemRow or a /Script path)")
factory = unreal.DataTableFactory()
factory.struct = struct
tools = unreal.AssetToolsHelpers.get_asset_tools()
name = p.rsplit("/", 1)[-1]
folder = p.rsplit("/", 1)[0]
dt = tools.create_asset(name, folder, unreal.DataTable, factory)
if dt is None:
    return ngg_error("create_asset failed for " + p)
filled = 0
if args["rows"]:
    # UE's own JSON round-trip format: [{"Name": "RowA", ...fields}]
    ok = unreal.DataTableFunctionLibrary.fill_data_table_from_json_string(dt, json.dumps(args["rows"]))
    if not ok:
        return ngg_error("asset created but fill_data_table_from_json_string failed — check field names against the row struct")
    filled = len(unreal.DataTableFunctionLibrary.get_data_table_row_names(dt))
unreal.EditorAssetLibrary.save_asset(p, only_if_is_dirty=False)
ngg_result({"ok": True, "asset_path": p, "row_struct": struct.get_path_name(), "rows": filled})
`);
}

/** Read a DataTable's rows back as JSON. */
export async function datatableRead({ asset_path } = {}) {
  return runPy(`
${pyArgs({ asset_path })}
dt = unreal.load_object(None, args["asset_path"])
if dt is None or not isinstance(dt, unreal.DataTable):
    return ngg_error("DataTable not found: " + args["asset_path"])
js = unreal.DataTableFunctionLibrary.export_data_table_to_json_string(dt)
rows = json.loads(js) if js else []
names = [str(n) for n in unreal.DataTableFunctionLibrary.get_data_table_row_names(dt)]
struct = dt.get_editor_property("row_struct")
ngg_result({"ok": True, "asset_path": args["asset_path"],
            "row_struct": struct.get_path_name() if struct else None,
            "row_names": names, "rows": rows})
`);
}

/** Replace a DataTable's rows from JSON (UE row-JSON format). */
export async function datatableSetRows({ asset_path, rows } = {}) {
  return runPy(`
${pyArgs({ asset_path, rows })}
dt = unreal.load_object(None, args["asset_path"])
if dt is None or not isinstance(dt, unreal.DataTable):
    return ngg_error("DataTable not found: " + args["asset_path"])
ok = unreal.DataTableFunctionLibrary.fill_data_table_from_json_string(dt, json.dumps(args["rows"]))
if not ok:
    return ngg_error("fill_data_table_from_json_string failed — check field names against the row struct")
unreal.EditorAssetLibrary.save_asset(args["asset_path"], only_if_is_dirty=False)
names = [str(n) for n in unreal.DataTableFunctionLibrary.get_data_table_row_names(dt)]
ngg_result({"ok": True, "asset_path": args["asset_path"], "row_names": names})
`);
}

// ---------------------------------------------------------------------------
// Material node graphs (MaterialEditingLibrary)
// ---------------------------------------------------------------------------

/**
 * Add a material expression node to a material. Returns a node_id (the
 * expression's object name) usable in gd_material_connect.
 */
export async function materialAddExpression({ material_path, expression_class, node_x, node_y, properties } = {}) {
  return runPy(`
${pyArgs({ material_path, expression_class, node_x: node_x || 0, node_y: node_y || 0, properties: properties || {} })}
mat = unreal.load_object(None, args["material_path"])
if mat is None or not isinstance(mat, unreal.Material):
    return ngg_error("Material not found: " + args["material_path"])
cls_name = args["expression_class"]
if not cls_name.startswith("MaterialExpression"):
    cls_name = "MaterialExpression" + cls_name
cls = getattr(unreal, cls_name, None)
if cls is None:
    return ngg_error("unknown expression class: " + cls_name)
expr = unreal.MaterialEditingLibrary.create_material_expression(mat, cls, args["node_x"], args["node_y"])
if expr is None:
    return ngg_error("create_material_expression failed for " + cls_name)
applied = []
for k, v in args["properties"].items():
    try:
        cur = expr.get_editor_property(k)
        if isinstance(cur, unreal.LinearColor) and isinstance(v, list):
            v = unreal.LinearColor(*[float(x) for x in v])
        elif isinstance(cur, unreal.Vector) and isinstance(v, list):
            v = unreal.Vector(*[float(x) for x in v])
        elif isinstance(cur, (unreal.Texture, unreal.Object)) and isinstance(v, str):
            v = unreal.load_object(None, v)
        expr.set_editor_property(k, v)
        applied.append(k)
    except Exception as e:
        return ngg_error("set property '" + k + "' failed: " + str(e))
unreal.MaterialEditingLibrary.recompile_material(mat)
ngg_result({"ok": True, "material": args["material_path"], "node_id": expr.get_name(),
            "class": cls_name, "applied_properties": applied})
`);
}

/**
 * Connect expressions to each other or to a material output property.
 * to_node "OUTPUT" targets the material's property (BaseColor, Roughness, ...).
 */
export async function materialConnect({ material_path, from_node, from_output, to_node, to_input, material_property } = {}) {
  return runPy(`
${pyArgs({ material_path, from_node, from_output: from_output || "", to_node: to_node || "", to_input: to_input || "", material_property: material_property || "" })}
mat = unreal.load_object(None, args["material_path"])
if mat is None or not isinstance(mat, unreal.Material):
    return ngg_error("Material not found: " + args["material_path"])
def find_expr(name):
    for e in unreal.MaterialEditingLibrary.get_material_expressions(mat) \
             if hasattr(unreal.MaterialEditingLibrary, "get_material_expressions") else []:
        if e.get_name() == name:
            return e
    # Fallback: resolve as subobject of the material
    return unreal.find_object(mat, name)
src = find_expr(args["from_node"])
if src is None:
    return ngg_error("from_node not found: " + args["from_node"])
if args["material_property"]:
    prop = getattr(unreal.MaterialProperty, "MP_" + args["material_property"].upper(), None)
    if prop is None:
        # Try exact enum name, e.g. MP_BASE_COLOR
        prop = getattr(unreal.MaterialProperty, args["material_property"], None)
    if prop is None:
        return ngg_error("unknown material property: " + args["material_property"] +
                         " (try BASE_COLOR, METALLIC, ROUGHNESS, EMISSIVE_COLOR, NORMAL, OPACITY)")
    ok = unreal.MaterialEditingLibrary.connect_material_property(src, args["from_output"], prop)
    if not ok:
        return ngg_error("connect_material_property failed")
else:
    dst = find_expr(args["to_node"])
    if dst is None:
        return ngg_error("to_node not found: " + args["to_node"])
    ok = unreal.MaterialEditingLibrary.connect_material_expressions(src, args["from_output"], dst, args["to_input"])
    if not ok:
        return ngg_error("connect_material_expressions failed")
unreal.MaterialEditingLibrary.recompile_material(mat)
unreal.EditorAssetLibrary.save_asset(args["material_path"], only_if_is_dirty=False)
ngg_result({"ok": True, "material": args["material_path"], "connected": True})
`);
}

// ---------------------------------------------------------------------------
// Sequencer
// ---------------------------------------------------------------------------

/** Create a Level Sequence asset. */
export async function sequencerCreate({ asset_path, frame_rate, length_seconds, overwrite } = {}) {
  return runPy(`
${pyArgs({ asset_path, frame_rate: frame_rate || 30, length_seconds: length_seconds || 5.0, overwrite: !!overwrite })}
lib = unreal.EditorAssetLibrary
p = args["asset_path"]
if lib.does_asset_exist(p):
    if not args["overwrite"]:
        return ngg_error("asset already exists: " + p + " (pass overwrite=true to replace)")
    lib.delete_asset(p)
tools = unreal.AssetToolsHelpers.get_asset_tools()
name = p.rsplit("/", 1)[-1]
folder = p.rsplit("/", 1)[0]
seq = tools.create_asset(name, folder, unreal.LevelSequence, unreal.LevelSequenceFactoryNew())
if seq is None:
    return ngg_error("create_asset failed for " + p)
fps = int(args["frame_rate"])
seq.set_display_rate(unreal.FrameRate(fps, 1))
end_frame = int(round(args["length_seconds"] * fps))
seq.set_playback_start(0)
seq.set_playback_end(end_frame)
lib.save_asset(p, only_if_is_dirty=False)
ngg_result({"ok": True, "asset_path": p, "frame_rate": fps,
            "playback_start": 0, "playback_end": end_frame})
`);
}

/**
 * Bind a level actor into a sequence (possessable) and optionally add a
 * transform track with location keyframes: [{time, location, rotation?}].
 */
export async function sequencerBindActor({ sequence_path, actor_label, transform_keys } = {}) {
  return runPy(`
${pyArgs({ sequence_path, actor_label, keys: transform_keys || [] })}
seq = unreal.load_object(None, args["sequence_path"])
if seq is None or not isinstance(seq, unreal.LevelSequence):
    return ngg_error("LevelSequence not found: " + args["sequence_path"])
eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
target = None
for a in eas.get_all_level_actors():
    if a.get_actor_label() == args["actor_label"]:
        target = a
        break
if target is None:
    return ngg_error("actor not found: " + args["actor_label"])
binding = seq.add_possessable(target)
fps = seq.get_display_rate().numerator / max(1, seq.get_display_rate().denominator)
keyed = 0
if args["keys"]:
    track = binding.add_track(unreal.MovieScene3DTransformTrack)
    section = track.add_section()
    section.set_start_frame_bounded(True)
    section.set_end_frame_bounded(True)
    section.set_range(seq.get_playback_start(), seq.get_playback_end())
    channels = section.get_all_channels()
    # Channels 0..2 = Location XYZ, 3..5 = Rotation, 6..8 = Scale
    for k in args["keys"]:
        frame = unreal.FrameNumber(int(round(float(k["time"]) * fps)))
        loc = k.get("location")
        rot = k.get("rotation")
        if loc:
            for i in range(3):
                channels[i].add_key(frame, float(loc[i]))
        if rot:
            for i in range(3):
                channels[3 + i].add_key(frame, float(rot[i]))
        keyed += 1
unreal.EditorAssetLibrary.save_asset(args["sequence_path"], only_if_is_dirty=False)
ngg_result({"ok": True, "sequence": args["sequence_path"], "bound_actor": args["actor_label"],
            "binding_id": str(binding.get_id()), "keyframes": keyed})
`);
}

/**
 * Spawn a CineCameraActor, bind it, and add a camera-cut track covering the
 * whole sequence. Optional look_at aims the camera at an actor.
 */
export async function sequencerAddCamera({ sequence_path, camera_label, location, look_at_actor } = {}) {
  return runPy(`
${pyArgs({ sequence_path, camera_label: camera_label || "NGG_CineCamera", location: location || [500, 0, 300], look_at_actor: look_at_actor || "" })}
seq = unreal.load_object(None, args["sequence_path"])
if seq is None or not isinstance(seq, unreal.LevelSequence):
    return ngg_error("LevelSequence not found: " + args["sequence_path"])
eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
loc = unreal.Vector(*[float(x) for x in args["location"]])
rot = unreal.Rotator(0.0, 0.0, 0.0)
if args["look_at_actor"]:
    target = None
    for a in eas.get_all_level_actors():
        if a.get_actor_label() == args["look_at_actor"]:
            target = a
            break
    if target is None:
        return ngg_error("look_at_actor not found: " + args["look_at_actor"])
    rot = unreal.MathLibrary.find_look_at_rotation(loc, target.get_actor_location())
cam = eas.spawn_actor_from_class(unreal.CineCameraActor, loc, rot)
if cam is None:
    return ngg_error("failed to spawn CineCameraActor")
cam.set_actor_label(args["camera_label"])
binding = seq.add_possessable(cam)
cut_track = seq.add_track(unreal.MovieSceneCameraCutTrack)
cut = cut_track.add_section()
cut.set_range(seq.get_playback_start(), seq.get_playback_end())
bid = unreal.MovieSceneObjectBindingID()
bid.set_editor_property("guid", binding.get_id())
cut.set_editor_property("camera_binding_id", bid)
unreal.EditorAssetLibrary.save_asset(args["sequence_path"], only_if_is_dirty=False)
ngg_result({"ok": True, "sequence": args["sequence_path"], "camera": args["camera_label"],
            "binding_id": str(binding.get_id())})
`);
}

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------

/**
 * Create a SoundCue from an imported SoundWave (wires a WavePlayer via the
 * factory's initial_sound_wave). Optional volume/pitch/looping.
 */
export async function createSoundCue({ asset_path, sound_wave, volume, pitch, looping, overwrite } = {}) {
  return runPy(`
${pyArgs({ asset_path, sound_wave, volume, pitch, looping, overwrite: !!overwrite })}
lib = unreal.EditorAssetLibrary
p = args["asset_path"]
if lib.does_asset_exist(p):
    if not args["overwrite"]:
        return ngg_error("asset already exists: " + p + " (pass overwrite=true to replace)")
    lib.delete_asset(p)
wave = unreal.load_object(None, args["sound_wave"])
if wave is None or not isinstance(wave, unreal.SoundWave):
    return ngg_error("SoundWave not found: " + args["sound_wave"])
tools = unreal.AssetToolsHelpers.get_asset_tools()
name = p.rsplit("/", 1)[-1]
folder = p.rsplit("/", 1)[0]
cue = tools.create_asset(name, folder, unreal.SoundCue, unreal.SoundCueFactoryNew())
if cue is None:
    return ngg_error("create_asset failed for " + p)
# The factory's InitialSoundWave is protected from scripting (UE 5.7), so
# build the WavePlayer node directly and set it as the cue's root node.
node = unreal.new_object(unreal.SoundNodeWavePlayer, outer=cue)
try:
    node.set_editor_property("sound_wave_asset_ptr", wave)
except Exception:
    node.set_editor_property("sound_wave", wave)
if args.get("looping"):
    node.set_editor_property("looping", True)
cue.set_editor_property("first_node", node)
if args.get("volume") is not None:
    cue.set_editor_property("volume_multiplier", float(args["volume"]))
if args.get("pitch") is not None:
    cue.set_editor_property("pitch_multiplier", float(args["pitch"]))
lib.save_asset(p, only_if_is_dirty=False)
ngg_result({"ok": True, "asset_path": p, "sound_wave": args["sound_wave"]})
`);
}

// ---------------------------------------------------------------------------
// Reimport guard
// ---------------------------------------------------------------------------

/**
 * Check whether an asset actually has a reimportable source file before the
 * C++ route hands it to FReimportManager. Reimporting an asset with no import
 * source (e.g. a Blueprint) opens a modal file-picker on the game thread,
 * freezing the whole editor for a headless agent — this guard turns that into
 * a structured error instead.
 */
export async function reimportSourceCheck({ asset_path } = {}) {
  return runPy(`
${pyArgs({ asset_path })}
asset = unreal.load_object(None, args["asset_path"])
if asset is None:
    return ngg_error("asset not found: " + args["asset_path"])
aid = None
try:
    aid = asset.get_editor_property("asset_import_data")
except Exception:
    aid = None
if aid is None:
    return ngg_error("asset has no import source (was created in-editor, not imported) — reimport would open a modal file dialog. Nothing to reimport.")
try:
    src = aid.get_first_filename()
except Exception:
    src = ""
if not src:
    return ngg_error("asset has import data but no source filename — reimport would open a modal file dialog.")
import os
ngg_result({"ok": True, "source_file": src, "source_exists": os.path.exists(src)})
`);
}

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------

/**
 * Create a SoundAttenuation asset with the common distance-falloff settings.
 */
export async function createSoundAttenuation({ asset_path, falloff_distance, inner_radius, shape, spatialize, overwrite } = {}) {
  return runPy(`
${pyArgs({ asset_path, falloff_distance, inner_radius, shape: shape || "", spatialize, overwrite: !!overwrite })}
lib = unreal.EditorAssetLibrary
p = args["asset_path"]
if lib.does_asset_exist(p):
    if not args["overwrite"]:
        return ngg_error("asset already exists: " + p + " (pass overwrite=true to replace)")
    lib.delete_asset(p)
tools = unreal.AssetToolsHelpers.get_asset_tools()
name = p.rsplit("/", 1)[-1]
folder = p.rsplit("/", 1)[0]
atten = tools.create_asset(name, folder, unreal.SoundAttenuation, unreal.SoundAttenuationFactory())
if atten is None:
    return ngg_error("create_asset failed for " + p)
settings = atten.get_editor_property("attenuation")
if args.get("falloff_distance") is not None:
    settings.set_editor_property("falloff_distance", float(args["falloff_distance"]))
if args.get("inner_radius") is not None:
    settings.set_editor_property("attenuation_shape_extents",
        unreal.Vector(float(args["inner_radius"]), 0.0, 0.0))
if args["shape"]:
    shape = getattr(unreal.AttenuationShape, args["shape"].upper(), None)
    if shape is None:
        return ngg_error("unknown shape: " + args["shape"] + " (SPHERE, CAPSULE, BOX, CONE)")
    settings.set_editor_property("attenuation_shape", shape)
if args.get("spatialize") is not None:
    settings.set_editor_property("spatialize", bool(args["spatialize"]))
atten.set_editor_property("attenuation", settings)
lib.save_asset(p, only_if_is_dirty=False)
ngg_result({"ok": True, "asset_path": p})
`);
}

/**
 * Spawn an AmbientSound actor playing a sound asset, with optional volume /
 * pitch / attenuation.
 */
export async function spawnAmbientSound({ sound, location, actor_label, volume, pitch, attenuation, auto_activate } = {}) {
  return runPy(`
${pyArgs({ sound, location: location || [0, 0, 100], actor_label: actor_label || "", volume, pitch, attenuation: attenuation || "", auto_activate })}
snd = unreal.load_object(None, args["sound"])
if snd is None or not isinstance(snd, unreal.SoundBase):
    return ngg_error("sound asset not found (SoundWave/SoundCue): " + args["sound"])
eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
loc = unreal.Vector(*[float(x) for x in args["location"]])
actor = eas.spawn_actor_from_class(unreal.AmbientSound, loc, unreal.Rotator(0, 0, 0))
if actor is None:
    return ngg_error("failed to spawn AmbientSound")
if args["actor_label"]:
    actor.set_actor_label(args["actor_label"])
comp = actor.get_editor_property("audio_component")
comp.set_editor_property("sound", snd)
if args.get("volume") is not None:
    comp.set_editor_property("volume_multiplier", float(args["volume"]))
if args.get("pitch") is not None:
    comp.set_editor_property("pitch_multiplier", float(args["pitch"]))
if args["attenuation"]:
    att = unreal.load_object(None, args["attenuation"])
    if att is None:
        return ngg_error("attenuation asset not found: " + args["attenuation"])
    comp.set_editor_property("attenuation_settings", att)
if args.get("auto_activate") is not None:
    comp.set_editor_property("auto_activate", bool(args["auto_activate"]))
ngg_result({"ok": True, "actor": actor.get_actor_label(), "sound": args["sound"]})
`);
}

/**
 * Play a sound once through the editor's audio device so the user can hear it.
 */
export async function playSoundPreview({ sound, volume } = {}) {
  return runPy(`
${pyArgs({ sound, volume })}
snd = unreal.load_object(None, args["sound"])
if snd is None or not isinstance(snd, unreal.SoundBase):
    return ngg_error("sound asset not found (SoundWave/SoundCue): " + args["sound"])
world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
vol = float(args.get("volume") or 1.0)
unreal.GameplayStatics.play_sound_at_location(
    world, snd, unreal.Vector(0.0, 0.0, 0.0), unreal.Rotator(0.0, 0.0, 0.0), vol)
ngg_result({"ok": True, "played": args["sound"],
            "note": "audible on the editor machine's speakers only"})
`);
}

// ---------------------------------------------------------------------------
// UMG widgets
// ---------------------------------------------------------------------------

// WidgetBlueprint.widget_tree isn't scripting-exposed in 5.7, but the tree
// and its widgets are name-addressable subobjects of the Blueprint asset.
const PY_FIND_WIDGET = `
def find_widget(wbp, name):
    tree = unreal.find_object(wbp, "WidgetTree")
    if tree is None:
        return None
    w = unreal.find_object(tree, name)
    if w is not None and isinstance(w, unreal.Widget):
        return w
    return None
`;

/**
 * Set arbitrary editor properties on a named widget inside a Widget
 * Blueprint's tree. Handles color arrays, enum names, object paths, and
 * UImage brush textures (key: brush_texture). Compile the WBP afterwards.
 */
export async function setWidgetProperties({ widget_blueprint, widget_name, properties } = {}) {
  return runPy(`
${pyArgs({ widget_blueprint, widget_name, properties: properties || {} })}
${PY_FIND_WIDGET}
wbp = unreal.load_object(None, args["widget_blueprint"])
if wbp is None:
    return ngg_error("Widget Blueprint not found: " + args["widget_blueprint"])
w = find_widget(wbp, args["widget_name"])
if w is None:
    return ngg_error("widget not found in tree: " + args["widget_name"])
applied = []
for k, v in args["properties"].items():
    try:
        if k == "brush_texture":
            tex = unreal.load_object(None, v)
            if tex is None:
                return ngg_error("texture not found: " + str(v))
            brush = w.get_editor_property("brush")
            brush.set_editor_property("resource_object", tex)
            w.set_editor_property("brush", brush)
            applied.append(k)
            continue
        cur = w.get_editor_property(k)
        if isinstance(cur, unreal.LinearColor) and isinstance(v, list):
            v = unreal.LinearColor(*[float(x) for x in v])
        elif isinstance(cur, unreal.SlateColor) and isinstance(v, list):
            v = unreal.SlateColor(unreal.LinearColor(*[float(x) for x in v]))
        elif isinstance(cur, unreal.Object) and isinstance(v, str):
            v = unreal.load_object(None, v)
        elif hasattr(cur, "__class__") and cur.__class__.__module__ == "unreal" \
             and isinstance(v, str) and hasattr(type(cur), v.upper()):
            v = getattr(type(cur), v.upper())
        w.set_editor_property(k, v)
        applied.append(k)
    except Exception as e:
        return ngg_error("set property '" + k + "' failed: " + str(e))
wbp.modify()
ngg_result({"ok": True, "widget": args["widget_name"], "applied_properties": applied,
            "note": "call ue5_compile_widget_blueprint to apply"})
`);
}

// ---------------------------------------------------------------------------
// GAS helpers
// ---------------------------------------------------------------------------

/**
 * List the GameplayAttributeData attributes of an AttributeSet Blueprint by
 * filtering its member variables (via the /bp/list_variables route — BP
 * variables aren't reachable through Python reflection in 5.7).
 */
export async function gasListAttributes({ asset_path } = {}) {
  let vars;
  try {
    vars = await ue5.bpListVariables({ blueprint: asset_path });
  } catch (err) {
    return { ok: false, error: err.message };
  }
  const list = vars?.variables ?? vars ?? [];
  const attrs = (Array.isArray(list) ? list : []).filter((v) => {
    const t = JSON.stringify(v.type ?? v.var_type ?? "");
    return /GameplayAttributeData/i.test(t);
  });
  return {
    ok: true,
    data: {
      asset_path,
      attributes: attrs.map((v) => ({ name: v.name ?? v.var_name, type: v.type ?? v.var_type })),
      total_variables: Array.isArray(list) ? list.length : 0,
    },
  };
}

/**
 * Append gameplay tags to Config/DefaultGameplayTags.ini. Tags load at editor
 * startup; we also try an in-editor refresh so newly added tags are usable
 * immediately when the engine version supports it.
 * @param {Array<{tag: string, comment?: string}>} tags
 */
export async function addGameplayTags({ tags } = {}, projectDirOverride) {
  if (!Array.isArray(tags) || !tags.length) {
    return { ok: false, error: "tags array is required, e.g. [{tag: 'Ability.Dash', comment: '...'}]" };
  }
  for (const t of tags) {
    if (!t.tag || !/^[A-Za-z0-9_.]+$/.test(t.tag)) {
      return { ok: false, error: `invalid tag name: '${t.tag ?? ""}' (letters, digits, underscore, dot)` };
    }
  }

  let projectDir = projectDirOverride;
  if (!projectDir) {
    try {
      const info = await ue5.getProjectInfo();
      projectDir = path.dirname(info.uproject_path);
    } catch {
      return { ok: false, error: "bridge unreachable — cannot locate the project Config directory" };
    }
  }
  const iniPath = path.join(projectDir, "Config", "DefaultGameplayTags.ini");

  let text = "";
  try {
    text = fs.readFileSync(iniPath, "utf8");
  } catch {
    text = "[/Script/GameplayTags.GameplayTagsSettings]\n";
  }
  if (!text.includes("[/Script/GameplayTags.GameplayTagsSettings]")) {
    text += "\n[/Script/GameplayTags.GameplayTagsSettings]\n";
  }

  const added = [];
  const skipped = [];
  for (const { tag, comment } of tags) {
    if (text.includes(`Tag="${tag}"`)) {
      skipped.push(tag);
      continue;
    }
    const devComment = (comment ?? "").replace(/"/g, "'");
    const line = `+GameplayTagList=(Tag="${tag}",DevComment="${devComment}")`;
    // Insert right after the settings section header so lines group together.
    text = text.replace(
      "[/Script/GameplayTags.GameplayTagsSettings]",
      `[/Script/GameplayTags.GameplayTagsSettings]\n${line}`
    );
    added.push(tag);
  }
  fs.writeFileSync(iniPath, text, "utf8");

  // Best-effort live refresh; harmless if the API is absent in this version.
  let refreshed = false;
  if (added.length) {
    const r = await runPy(`
try:
    unreal.GameplayTagsManager.get().editor_refresh_gameplay_tag_tree()
    ngg_result({"ok": True, "refreshed": True})
except Exception:
    ngg_result({"ok": True, "refreshed": False})
`);
    refreshed = !!(r.ok && r.data && r.data.refreshed);
  }

  return {
    ok: true,
    data: {
      ini: iniPath,
      added,
      skipped_existing: skipped,
      live_refresh: refreshed,
      note: refreshed
        ? "tags are usable immediately"
        : "tags are written to the ini; restart the editor (ue5_kill_editor + ue5_launch_editor) if they don't appear in ue5_list_gameplay_tags",
    },
  };
}

// ---------------------------------------------------------------------------
// Level-actor components
// ---------------------------------------------------------------------------

/**
 * Add a component to a *level actor instance* (not a Blueprint class) via
 * AActor::AddComponentByClass, with optional initial properties.
 */
export async function addComponentToActor({ actor_label, component_class, component_label, properties } = {}) {
  return runPy(`
${pyArgs({ actor_label, component_class, component_label: component_label || "", properties: properties || {} })}
eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
target = None
for a in eas.get_all_level_actors():
    if a.get_actor_label() == args["actor_label"]:
        target = a
        break
if target is None:
    return ngg_error("actor not found: " + args["actor_label"])
cls_name = args["component_class"]
cls = getattr(unreal, cls_name, None)
if cls is None and cls_name.endswith("Component"):
    cls = getattr(unreal, cls_name[:-len("Component")], None)
if cls is None:
    loaded = unreal.load_object(None, cls_name)
    cls = loaded if isinstance(loaded, type) else None
if cls is None:
    return ngg_error("component class not found: " + cls_name)
# AActor.AddComponentByClass isn't scripting-exposed in UE 5.7 — the supported
# path for instance components is the SubobjectDataSubsystem.
sds = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
handles = sds.k2_gather_subobject_data_for_instance(target)
if not handles:
    return ngg_error("could not gather subobject data for actor " + args["actor_label"])
params = unreal.AddNewSubobjectParams(parent_handle=handles[0], new_class=cls)
new_handle, fail_reason = sds.add_new_subobject(params)
fail_text = str(fail_reason)
if fail_text and fail_text != "None":
    return ngg_error("add_new_subobject failed: " + fail_text)
comps = list(target.get_components_by_class(cls))
if not comps:
    return ngg_error("component was added but not found on the actor")
comp = comps[-1]
if args["component_label"]:
    try:
        sds.rename_subobject(new_handle, unreal.Text(args["component_label"]))
    except Exception:
        pass
applied = []
for k, v in args["properties"].items():
    try:
        cur = comp.get_editor_property(k)
        if isinstance(cur, unreal.Object) and isinstance(v, str):
            v = unreal.load_object(None, v)
        comp.set_editor_property(k, v)
        applied.append(k)
    except Exception as e:
        return ngg_error("set property '" + k + "' failed: " + str(e))
target.modify()
ngg_result({"ok": True, "actor": args["actor_label"], "component": comp.get_name(),
            "class": comp.get_class().get_name(),
            "applied_properties": applied})
`);
}
