// Copyright 2025-2026 NGG. All Rights Reserved.
// worldpartition.js — unrealngg-mcp
//
// World Partition: reading a partitioned world, authoring Data Layers, moving
// actors between them, and streaming regions in and out of the editor. Plus
// Level Instances, to the extent Python can reach them.
//
// Runs through POST /editor/exec_python, like sequencer.js and pcg.js.
//
// What the 5.8 Python API actually offers (probed against a live editor before
// any of this was written):
//
//   * DataLayerEditorSubsystem — the full editor surface: create/delete
//     instances, add/remove actors, visibility, editor-loaded state, initial
//     runtime state, parenting. This is the solid part.
//   * WorldPartitionBlueprintLibrary — actor descs (the streaming manifest),
//     world bounds, load/unload/pin by actor, and the DataLayerManager.
//   * A Data Layer is TWO objects: a DataLayerAsset (content, reusable across
//     levels) and a DataLayerInstance (this level's use of it). Creating one
//     means creating the asset, then instancing it into the current world —
//     which is why ue5_wp_create_data_layer does both and reports both.
//   * LevelInstanceSubsystem is NOT exposed to Python in 5.8, so a Level
//     Instance cannot be built from a selection of actors here. What does work
//     is spawning an ALevelInstance actor pointed at an existing level asset,
//     which covers the "place this level inside that one" half.

import * as ue5 from "./ue5client.js";

const RESULT_BEGIN = "__NGG_WP_RESULT_BEGIN__";
const RESULT_END   = "__NGG_WP_RESULT_END__";

const PY_PRELUDE = `
import unreal, json, base64, traceback

def ngg_result(obj):
    print("${RESULT_BEGIN}" + json.dumps(obj) + "${RESULT_END}")

def ngg_error(msg):
    ngg_result({"ok": False, "error": msg})

def s(v):
    return None if v is None else str(v)

def editor_world():
    ues = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
    return ues.get_editor_world() if ues else None

def data_layer_subsystem():
    return unreal.get_editor_subsystem(unreal.DataLayerEditorSubsystem)

def actor_descs():
    """The streaming manifest. Empty on a non-partitioned world."""
    try:
        return unreal.WorldPartitionBlueprintLibrary.get_actor_descs() or []
    except Exception:
        return []

def is_partitioned():
    """UWorld exposes no get_world_partition() to Python in 5.8, so infer it:
    only a partitioned world has an actor-desc container."""
    return len(actor_descs()) > 0

def find_layer(name):
    """Match a DataLayerInstance by short name, full name, or asset name."""
    dls = data_layer_subsystem()
    if dls is None:
        return None
    for inst in dls.get_all_data_layers():
        candidates = [str(inst.get_data_layer_short_name()),
                      str(inst.get_data_layer_full_name()),
                      str(inst.get_name())]
        asset = inst.get_asset() if hasattr(inst, "get_asset") else None
        if asset is not None:
            candidates.append(str(asset.get_name()))
            candidates.append(str(asset.get_path_name()))
        if name in candidates:
            return inst
    return None

def layer_names():
    dls = data_layer_subsystem()
    if dls is None:
        return []
    return [str(i.get_data_layer_short_name()) for i in dls.get_all_data_layers()]

def actors_by_label(labels):
    """Resolve actor labels to actors. Returns (actors, missing_labels)."""
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    wanted = list(labels)
    found, by_label = [], {}
    for a in eas.get_all_level_actors():
        by_label[str(a.get_actor_label())] = a
    missing = []
    for label in wanted:
        a = by_label.get(label)
        if a is None:
            missing.append(label)
        else:
            found.append(a)
    return found, missing

def describe_layer(inst):
    asset = inst.get_asset() if hasattr(inst, "get_asset") else None
    return {
        "short_name": s(inst.get_data_layer_short_name()),
        "full_name": s(inst.get_data_layer_full_name()),
        "asset": s(asset.get_path_name()) if asset is not None else None,
        "type": "runtime" if inst.is_runtime() else "editor",
        "is_visible": bool(inst.is_visible()),
        "is_initially_visible": bool(inst.is_initially_visible()),
        "initial_runtime_state": s(inst.get_initial_runtime_state()),
        "is_client_only": bool(inst.is_client_only()),
        "is_server_only": bool(inst.is_server_only()),
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
// Read
// ---------------------------------------------------------------------------

/**
 * Describe the currently open world: whether it is partitioned, its bounds, its
 * Data Layers, and a summary of the streaming manifest.
 *
 * Call this first — every other tool here needs to know which layers exist and
 * whether the level is partitioned at all.
 *
 * @param {object}  [opts]
 * @param {boolean} [opts.include_actors=false] - list every actor desc (can be long)
 * @param {string}  [opts.class_filter]         - only descs whose class contains this
 */
export async function wpRead({ include_actors, class_filter } = {}) {
  return runPy(`
${pyArgs({ include_actors: !!include_actors, class_filter: class_filter || "" })}
world = editor_world()
if world is None:
    return ngg_error("no editor world is open")

descs = actor_descs()
partitioned = len(descs) > 0

by_class = {}
spatially_loaded = 0
grids = {}
for d in descs:
    cls = str(d.class_) if d.class_ else "?"
    by_class[cls] = by_class.get(cls, 0) + 1
    if bool(d.is_spatially_loaded):
        spatially_loaded += 1
    g = str(d.runtime_grid) if d.runtime_grid else "(default)"
    grids[g] = grids.get(g, 0) + 1

result = {
    "ok": True,
    "world": s(world.get_name()),
    "world_path": s(world.get_path_name()),
    "is_partitioned": partitioned,
    "actor_desc_count": len(descs),
    "spatially_loaded_count": spatially_loaded,
    "actors_by_class": by_class,
    "runtime_grids": grids,
}

try:
    b = unreal.WorldPartitionBlueprintLibrary.get_editor_world_bounds()
    result["editor_world_bounds"] = {
        "min": [b.min.x, b.min.y, b.min.z],
        "max": [b.max.x, b.max.y, b.max.z],
    }
except Exception:
    pass

dls = data_layer_subsystem()
result["data_layers"] = [describe_layer(i) for i in dls.get_all_data_layers()] if dls else []

if args["include_actors"]:
    listed = []
    for d in descs:
        cls = str(d.class_) if d.class_ else ""
        if args["class_filter"] and args["class_filter"].lower() not in cls.lower():
            continue
        listed.append({
            "label": s(d.label),
            "name": s(d.name),
            "class": cls,
            "is_spatially_loaded": bool(d.is_spatially_loaded),
            "runtime_grid": s(d.runtime_grid),
            "data_layer_assets": [s(x) for x in (d.data_layer_assets or [])],
            "guid": s(d.guid),
        })
    result["actors"] = listed

if not partitioned:
    result["note"] = ("This world is not partitioned — it has no actor-desc container. "
                      "Data Layer tools need a World Partition map; create one with "
                      "ue5_create_level partitioned=true.")

ngg_result(result)
`);
}

// ---------------------------------------------------------------------------
// Data Layers
// ---------------------------------------------------------------------------

/**
 * Create a Data Layer: the DataLayerAsset plus an instance of it in the current
 * world. Both are reported, because they are separate objects and the asset
 * outlives the level that uses it.
 *
 * @param {object}  opts
 * @param {string}  opts.asset_path   - where to put the DataLayerAsset
 * @param {string}  [opts.type]       - "runtime" (default) or "editor"
 * @param {string}  [opts.parent]     - short name of an existing layer to nest under
 * @param {string}  [opts.initial_runtime_state] - unloaded | loaded | activated
 * @param {boolean} [opts.is_private] - a level-private layer, not a shared asset
 * @param {boolean} [opts.save=true]
 */
export async function wpCreateDataLayer({
  asset_path, type, parent, initial_runtime_state, is_private, save,
} = {}) {
  return runPy(`
${pyArgs({
  asset_path, type: (type || "runtime").toLowerCase(), parent,
  initial_runtime_state: (initial_runtime_state || "").toLowerCase(),
  is_private: !!is_private, save: save !== false,
})}
if not args.get("asset_path"):
    return ngg_error("asset_path is required")

world = editor_world()
if world is None:
    return ngg_error("no editor world is open")
if not is_partitioned():
    return ngg_error("this world is not partitioned — Data Layers need a World Partition map. "
                     "Create one with ue5_create_level partitioned=true.")

dls = data_layer_subsystem()
if dls is None:
    return ngg_error("DataLayerEditorSubsystem unavailable")

if args["type"] not in ("runtime", "editor"):
    return ngg_error("type must be 'runtime' or 'editor'")

STATES = {
    "unloaded":  unreal.DataLayerRuntimeState.UNLOADED,
    "loaded":    unreal.DataLayerRuntimeState.LOADED,
    "activated": unreal.DataLayerRuntimeState.ACTIVATED,
}
if args["initial_runtime_state"] and args["initial_runtime_state"] not in STATES:
    return ngg_error("initial_runtime_state must be one of: " + ", ".join(sorted(STATES.keys())))

path = args["asset_path"]
lib = unreal.EditorAssetLibrary
if lib.does_asset_exist(path):
    return ngg_error("an asset already exists at " + path)

name = path.rsplit("/", 1)[-1]
folder = path.rsplit("/", 1)[0]
tools = unreal.AssetToolsHelpers.get_asset_tools()
asset = tools.create_asset(name, folder, unreal.DataLayerAsset, unreal.DataLayerFactory())
if asset is None:
    return ngg_error("could not create a DataLayerAsset at " + path)

# The asset carries the editor/runtime distinction; the instance carries this
# level's state for it.
asset.set_editor_property("data_layer_type",
    unreal.DataLayerType.RUNTIME if args["type"] == "runtime" else unreal.DataLayerType.EDITOR)

params = unreal.DataLayerCreationParameters()
params.set_editor_property("data_layer_asset", asset)
params.set_editor_property("is_private", args["is_private"])

instance = dls.create_data_layer_instance(params)
if instance is None:
    # Roll the asset back — leaving a DataLayerAsset with no instance behind
    # would look like success in the content browser but do nothing in the level.
    lib.delete_asset(path)
    return ngg_error("create_data_layer_instance failed; the DataLayerAsset was removed again")

if args.get("parent"):
    parent_inst = find_layer(args["parent"])
    if parent_inst is None:
        return ngg_error("parent layer not found: " + args["parent"] +
                         ". Existing layers: " + (", ".join(layer_names()) or "(none)"))
    if not dls.set_parent_data_layer(instance, parent_inst):
        return ngg_error("the editor refused to nest this layer under " + args["parent"])

if args["initial_runtime_state"]:
    dls.set_data_layer_initial_runtime_state(instance, STATES[args["initial_runtime_state"]])

result = {"ok": True, "asset": path, "instance": describe_layer(instance)}
if args["save"]:
    result["saved_asset"] = lib.save_asset(path, only_if_is_dirty=False)
    result["saved_level"] = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level()

ngg_result(result)
`);
}

/**
 * Add or remove actors (by label) on a Data Layer.
 *
 * @param {object}   opts
 * @param {string}   opts.layer      - short name of the Data Layer
 * @param {string[]} opts.actors     - actor labels
 * @param {string}   [opts.mode]     - "add" (default) or "remove"
 * @param {boolean}  [opts.save=true]
 */
export async function wpSetActorDataLayers({ layer, actors, mode, save } = {}) {
  return runPy(`
${pyArgs({ layer, actors: actors || [], mode: (mode || "add").toLowerCase(), save: save !== false })}
if not args.get("layer"):
    return ngg_error("layer is required")
if not args["actors"]:
    return ngg_error("actors[] is empty — nothing to do")
if args["mode"] not in ("add", "remove"):
    return ngg_error("mode must be 'add' or 'remove'")

dls = data_layer_subsystem()
if dls is None:
    return ngg_error("DataLayerEditorSubsystem unavailable")

inst = find_layer(args["layer"])
if inst is None:
    return ngg_error("data layer not found: " + args["layer"] +
                     ". Existing layers: " + (", ".join(layer_names()) or "(none)"))

found, missing = actors_by_label(args["actors"])
if missing:
    return ngg_error("actor label(s) not found in the level: " + ", ".join(missing))

ok = (dls.add_actors_to_data_layer(found, inst) if args["mode"] == "add"
      else dls.remove_actors_from_data_layer(found, inst))
if not ok:
    return ngg_error("the editor refused to " + args["mode"] + " these actors "
                     "(an actor may not be valid for data layers — e.g. it is not spatially loaded)")

members = [s(a.get_actor_label()) for a in dls.get_actors_from_data_layer(inst)]
result = {
    "ok": True,
    "layer": s(inst.get_data_layer_short_name()),
    "mode": args["mode"],
    "affected": args["actors"],
    "layer_actors_now": members,
}
if args["save"]:
    result["saved_level"] = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level()

ngg_result(result)
`);
}

/**
 * Change a Data Layer's state: editor visibility, whether it is loaded in the
 * editor, and the runtime state it starts in.
 *
 * @param {object}  opts
 * @param {string}  opts.layer
 * @param {boolean} [opts.visible]
 * @param {boolean} [opts.loaded_in_editor]
 * @param {string}  [opts.initial_runtime_state] - unloaded | loaded | activated
 * @param {boolean} [opts.save=true]
 */
export async function wpSetDataLayerState({
  layer, visible, loaded_in_editor, initial_runtime_state, save,
} = {}) {
  return runPy(`
${pyArgs({
  layer, visible, loaded_in_editor,
  initial_runtime_state: (initial_runtime_state || "").toLowerCase(),
  save: save !== false,
})}
if not args.get("layer"):
    return ngg_error("layer is required")
if (args.get("visible") is None and args.get("loaded_in_editor") is None
        and not args["initial_runtime_state"]):
    return ngg_error("nothing to change — pass visible, loaded_in_editor or initial_runtime_state")

dls = data_layer_subsystem()
inst = find_layer(args["layer"])
if inst is None:
    return ngg_error("data layer not found: " + args["layer"] +
                     ". Existing layers: " + (", ".join(layer_names()) or "(none)"))

STATES = {
    "unloaded":  unreal.DataLayerRuntimeState.UNLOADED,
    "loaded":    unreal.DataLayerRuntimeState.LOADED,
    "activated": unreal.DataLayerRuntimeState.ACTIVATED,
}
if args["initial_runtime_state"] and args["initial_runtime_state"] not in STATES:
    return ngg_error("initial_runtime_state must be one of: " + ", ".join(sorted(STATES.keys())))

if args.get("visible") is not None:
    dls.set_data_layer_visibility(inst, bool(args["visible"]))
if args.get("loaded_in_editor") is not None:
    # is_from_user_change=True so the editor treats it as an explicit choice and
    # persists it, rather than as transient streaming bookkeeping.
    dls.set_data_layer_is_loaded_in_editor(inst, bool(args["loaded_in_editor"]), True)
if args["initial_runtime_state"]:
    dls.set_data_layer_initial_runtime_state(inst, STATES[args["initial_runtime_state"]])

result = {"ok": True, "layer": describe_layer(inst)}
if args["save"]:
    result["saved_level"] = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level()

ngg_result(result)
`);
}

/**
 * Delete a Data Layer instance from this level. The DataLayerAsset is left
 * alone — it is shared content, and other levels may instance it.
 *
 * @param {object}  opts
 * @param {string}  opts.layer
 * @param {boolean} [opts.save=true]
 */
export async function wpDeleteDataLayer({ layer, save } = {}) {
  return runPy(`
${pyArgs({ layer, save: save !== false })}
if not args.get("layer"):
    return ngg_error("layer is required")

dls = data_layer_subsystem()
inst = find_layer(args["layer"])
if inst is None:
    return ngg_error("data layer not found: " + args["layer"] +
                     ". Existing layers: " + (", ".join(layer_names()) or "(none)"))

removed = describe_layer(inst)
dls.delete_data_layer(inst)

still_there = find_layer(args["layer"]) is not None
if still_there:
    return ngg_error("delete_data_layer did not remove " + args["layer"])

result = {"ok": True, "deleted": removed, "layers_now": layer_names(),
          "note": "the DataLayerAsset itself was kept — it is shared content"}
if args["save"]:
    result["saved_level"] = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level()

ngg_result(result)
`);
}

// ---------------------------------------------------------------------------
// Streaming regions
// ---------------------------------------------------------------------------

/**
 * Load or unload the actors of a partitioned world inside a box, so a region
 * can be edited without opening the whole map.
 *
 * @param {object}   opts
 * @param {number[]} opts.min   - [x, y, z]
 * @param {number[]} opts.max   - [x, y, z]
 * @param {string}   [opts.mode] - "load" (default), "unload", "pin" or "unpin"
 */
export async function wpLoadRegion({ min, max, mode } = {}) {
  return runPy(`
${pyArgs({ min, max, mode: (mode || "load").toLowerCase() })}
if not args.get("min") or not args.get("max"):
    return ngg_error("min and max are required, each [x, y, z]")
if len(args["min"]) != 3 or len(args["max"]) != 3:
    return ngg_error("min and max must each have exactly 3 numbers")
if args["mode"] not in ("load", "unload", "pin", "unpin"):
    return ngg_error("mode must be load, unload, pin or unpin")
if not is_partitioned():
    return ngg_error("this world is not partitioned — there are no streaming regions to act on")

lo = [float(x) for x in args["min"]]
hi = [float(x) for x in args["max"]]
for i in range(3):
    if hi[i] <= lo[i]:
        return ngg_error("max must be greater than min on every axis (axis %d: %g <= %g)" % (i, hi[i], lo[i]))

box = unreal.Box()
box.set_editor_property("min", unreal.Vector(lo[0], lo[1], lo[2]))
box.set_editor_property("max", unreal.Vector(hi[0], hi[1], hi[2]))
box.set_editor_property("is_valid", True)

descs = unreal.WorldPartitionBlueprintLibrary.get_intersecting_actor_descs(box) or []
guids = [d.guid for d in descs]

wpbl = unreal.WorldPartitionBlueprintLibrary
if args["mode"] == "load":
    wpbl.load_actors(guids)
elif args["mode"] == "unload":
    wpbl.unload_actors(guids)
elif args["mode"] == "pin":
    wpbl.pin_actors(guids)
else:
    wpbl.unpin_actors(guids)

ngg_result({
    "ok": True,
    "mode": args["mode"],
    "region": {"min": lo, "max": hi},
    "actors_matched": len(descs),
    "labels": [s(d.label) for d in descs][:50],
})
`);
}

// ---------------------------------------------------------------------------
// Level Instances
// ---------------------------------------------------------------------------

/**
 * Place an existing level inside the current one as a Level Instance.
 *
 * Note the direction: LevelInstanceSubsystem is not exposed to Python in 5.8,
 * so the reverse operation — collapsing a selection of actors into a new level
 * instance — cannot be done from here. This spawns an ALevelInstance actor and
 * points it at a level asset, which is the half that is reachable.
 *
 * @param {object}   opts
 * @param {string}   opts.level_asset  - the level to instance, e.g. "/Game/Maps/L_Room"
 * @param {number[]} [opts.location]   - [x, y, z], defaults to the origin
 * @param {number[]} [opts.rotation]   - [pitch, yaw, roll]
 * @param {string}   [opts.actor_label]
 * @param {boolean}  [opts.save=true]
 */
export async function levelInstanceCreate({
  level_asset, location, rotation, actor_label, save,
} = {}) {
  return runPy(`
${pyArgs({
  level_asset, location: location || [0, 0, 0], rotation: rotation || [0, 0, 0],
  actor_label, save: save !== false,
})}
if not args.get("level_asset"):
    return ngg_error("level_asset is required")

lib = unreal.EditorAssetLibrary
if not lib.does_asset_exist(args["level_asset"]):
    return ngg_error("level asset not found: " + args["level_asset"])

loc = args["location"]
rot = args["rotation"]
if len(loc) != 3 or len(rot) != 3:
    return ngg_error("location and rotation must each have exactly 3 numbers")

eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
actor = eas.spawn_actor_from_class(
    unreal.LevelInstance,
    unreal.Vector(float(loc[0]), float(loc[1]), float(loc[2])),
    unreal.Rotator(float(rot[0]), float(rot[1]), float(rot[2])))
if actor is None:
    return ngg_error("could not spawn a LevelInstance actor")

try:
    # world_asset is a SoftObjectProperty on UWorld: it wants the loaded world
    # object, not a path or a TopLevelAssetPath (which fails with
    # "Cannot nativize 'TopLevelAssetPath' as 'Object'").
    target_world = unreal.load_asset(args["level_asset"])
    if target_world is None:
        raise Exception("load_asset returned nothing")
    actor.set_editor_property("world_asset", target_world)
except Exception as _e:
    # A LevelInstance with no world asset is an empty shell that looks placed
    # but shows nothing — remove it rather than leave that behind.
    eas.destroy_actor(actor)
    return ngg_error("could not point the LevelInstance at " + args["level_asset"] + ": " + str(_e))

if args.get("actor_label"):
    actor.set_actor_label(args["actor_label"])

result = {
    "ok": True,
    "actor_label": s(actor.get_actor_label()),
    "level_asset": args["level_asset"],
    "location": loc,
    "rotation": rot,
}
if args["save"]:
    result["saved_level"] = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level()

ngg_result(result)
`);
}
