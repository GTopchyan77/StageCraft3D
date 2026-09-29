// Copyright 2025-2026 NGG. All Rights Reserved.
// rigging.js — unrealngg-mcp
//
// IK Rigs, IK Retargeters and Control Rig blueprints — the authoring layer
// underneath the existing ue5_anim_* AnimGraph tools.
//
// Runs through POST /editor/exec_python, like sequencer.js / worldpartition.js.
//
// Probed against a live UE 5.8.1 editor before this was written. Two findings
// shaped the API:
//
//   * IKRigController.add_solver(type) CANNOT be called from Python: the
//     TSubclassOf parameter fails to convert, whether given the Python type,
//     .static_class() (which does not exist on these bindings) or load_class().
//     apply_auto_fbik() does work and produces a Full Body IK solver, so that
//     is the supported path here — and ue5_ikrig_create says so plainly rather
//     than pretending other solvers are available.
//   * A goal cannot be connected to a solver before a solver exists;
//     connect_goal_to_solver returns False rather than raising. Order matters:
//     mesh -> solver -> goals -> chains.

import * as ue5 from "./ue5client.js";

const RESULT_BEGIN = "__NGG_RIG_RESULT_BEGIN__";
const RESULT_END   = "__NGG_RIG_RESULT_END__";

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

def load_skeletal_mesh(path):
    obj = unreal.load_asset(path)
    if obj is None:
        return None, "skeletal mesh not found: " + path
    if not isinstance(obj, unreal.SkeletalMesh):
        return None, path + " is a " + type(obj).__name__ + ", not a SkeletalMesh"
    return obj, None

# Note: bone names cannot be listed from Python. Skeleton.bone_tree returns
# opaque BoneNode structs with nothing readable on them, and there is no
# get_bone_names(). That is why a bad bone name below can only be reported as
# "the engine rejected it" rather than "did you mean ...".

def describe_rig(ctrl):
    goals = []
    for g in ctrl.get_all_goals():
        name = s(g.get_editor_property("goal_name")) if hasattr(g, "get_editor_property") else s(g)
        goals.append({"name": name, "bone": s(ctrl.get_bone_for_goal(name)) if name else None})
    chains = []
    for c in ctrl.get_retarget_chains():
        cname = s(c.get_editor_property("chain_name"))
        chains.append({
            "name": cname,
            "start_bone": s(ctrl.get_retarget_chain_start_bone(cname)),
            "end_bone": s(ctrl.get_retarget_chain_end_bone(cname)),
            "goal": s(ctrl.get_retarget_chain_goal(cname)),
        })
    return {
        "solver_count": ctrl.get_num_solvers(),
        "retarget_root": s(ctrl.get_retarget_root()),
        "goals": goals,
        "retarget_chains": chains,
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
// IK Rig
// ---------------------------------------------------------------------------

/**
 * Create an IK Rig for a skeletal mesh: solver, IK goals, and the retarget
 * chains a retargeter later maps between characters.
 *
 * Order is enforced internally (mesh -> solver -> goals -> chains) because a
 * goal cannot attach to a solver that does not exist yet, and the API reports
 * that as a quiet `False`.
 *
 * @param {object}   opts
 * @param {string}   opts.asset_path
 * @param {string}   opts.skeletal_mesh
 * @param {string}   [opts.retarget_root]  - usually "pelvis" / "root"
 * @param {boolean}  [opts.auto_fbik=true] - add a Full Body IK solver
 * @param {Array}    [opts.goals]          - [{name, bone}]
 * @param {Array}    [opts.chains]         - [{name, start_bone, end_bone, goal?}]
 */
export async function ikRigCreate({
  asset_path, skeletal_mesh, retarget_root, auto_fbik, goals, chains,
} = {}) {
  return runPy(`
${pyArgs({
  asset_path, skeletal_mesh, retarget_root,
  auto_fbik: auto_fbik !== false, goals: goals || [], chains: chains || [],
})}
if not args.get("asset_path"):
    return ngg_error("asset_path is required")
if not args.get("skeletal_mesh"):
    return ngg_error("skeletal_mesh is required")

lib = unreal.EditorAssetLibrary
if lib.does_asset_exist(args["asset_path"]):
    return ngg_error("an asset already exists at " + args["asset_path"])

mesh, err = load_skeletal_mesh(args["skeletal_mesh"])
if err:
    return ngg_error(err)

name, folder = split_path(args["asset_path"])
rig = asset_tools().create_asset(name, folder, unreal.IKRigDefinition, unreal.IKRigDefinitionFactory())
if rig is None:
    return ngg_error("could not create an IKRigDefinition at " + args["asset_path"])

ctrl = unreal.IKRigController.get_controller(rig)
if ctrl is None:
    lib.delete_asset(args["asset_path"])
    return ngg_error("could not get an IKRigController for the new asset (it was removed again)")

if not ctrl.set_skeletal_mesh(mesh):
    lib.delete_asset(args["asset_path"])
    return ngg_error("the mesh " + args["skeletal_mesh"] + " was rejected by the rig "
                     "(the asset was removed again)")

warnings = []

# Solver first: goals attach to it.
if args["auto_fbik"]:
    if not ctrl.apply_auto_fbik():
        warnings.append("apply_auto_fbik() returned false — no solver was added")

for g in args["goals"]:
    gname, gbone = g.get("name"), g.get("bone")
    if not gname or not gbone:
        return ngg_error("every goal needs 'name' and 'bone'")
    added = ctrl.add_new_goal(gname, gbone)
    if not added or str(added) == "None":
        return ngg_error("could not add goal '" + str(gname) + "' on bone '" + str(gbone) +
                         "' — check the bone exists in " + args["skeletal_mesh"])
    if ctrl.get_num_solvers() > 0:
        if not ctrl.connect_goal_to_solver(gname, 0):
            warnings.append("goal '" + str(gname) + "' was created but not connected to the solver")
    else:
        warnings.append("goal '" + str(gname) + "' has no solver to connect to (auto_fbik was off)")

for c in args["chains"]:
    cname, start, end = c.get("name"), c.get("start_bone"), c.get("end_bone")
    if not cname or not start or not end:
        return ngg_error("every chain needs 'name', 'start_bone' and 'end_bone'")
    made = ctrl.add_retarget_chain(cname, start, end, c.get("goal", ""))
    if not made or str(made) == "None":
        return ngg_error("could not add retarget chain '" + str(cname) + "' (" + str(start) +
                         " -> " + str(end) + ") — check both bones exist")

if args.get("retarget_root"):
    if not ctrl.set_retarget_root(args["retarget_root"]):
        warnings.append("retarget root '" + str(args["retarget_root"]) + "' was rejected — "
                        "check the bone name")

lib.save_asset(args["asset_path"], only_if_is_dirty=False)

result = {"ok": True, "asset": args["asset_path"], "skeletal_mesh": args["skeletal_mesh"]}
result.update(describe_rig(ctrl))
if warnings:
    result["warnings"] = warnings
ngg_result(result)
`);
}

/**
 * Read an IK Rig: its mesh, solver count, goals and retarget chains.
 *
 * @param {object} opts
 * @param {string} opts.asset_path
 */
export async function ikRigRead({ asset_path } = {}) {
  return runPy(`
${pyArgs({ asset_path })}
if not args.get("asset_path"):
    return ngg_error("asset_path is required")

rig = unreal.load_asset(args["asset_path"])
if rig is None:
    return ngg_error("asset not found: " + args["asset_path"])
if not isinstance(rig, unreal.IKRigDefinition):
    return ngg_error(args["asset_path"] + " is a " + type(rig).__name__ + ", not an IKRigDefinition")

ctrl = unreal.IKRigController.get_controller(rig)
mesh = ctrl.get_skeletal_mesh()

result = {
    "ok": True,
    "asset": args["asset_path"],
    "skeletal_mesh": s(mesh.get_path_name()) if mesh else None,
}
result.update(describe_rig(ctrl))
ngg_result(result)
`);
}

// ---------------------------------------------------------------------------
// IK Retargeter
// ---------------------------------------------------------------------------

/**
 * Create an IK Retargeter that maps animation from one IK Rig onto another.
 *
 * @param {object}  opts
 * @param {string}  opts.asset_path
 * @param {string}  opts.source_rig
 * @param {string}  opts.target_rig
 * @param {string}  [opts.auto_map="exact"] - "exact", "fuzzy" or "none"
 * @param {string}  [opts.source_preview_mesh]
 * @param {string}  [opts.target_preview_mesh]
 */
export async function ikRetargeterCreate({
  asset_path, source_rig, target_rig, auto_map, source_preview_mesh, target_preview_mesh,
} = {}) {
  return runPy(`
${pyArgs({
  asset_path, source_rig, target_rig,
  auto_map: (auto_map || "exact").toLowerCase(),
  source_preview_mesh, target_preview_mesh,
})}
if not args.get("asset_path"):
    return ngg_error("asset_path is required")
if not args.get("source_rig") or not args.get("target_rig"):
    return ngg_error("both source_rig and target_rig are required (IK Rig asset paths)")
if args["auto_map"] not in ("exact", "fuzzy", "none"):
    return ngg_error("auto_map must be 'exact', 'fuzzy' or 'none'")

lib = unreal.EditorAssetLibrary
if lib.does_asset_exist(args["asset_path"]):
    return ngg_error("an asset already exists at " + args["asset_path"])

def load_rig(path, label):
    obj = unreal.load_asset(path)
    if obj is None:
        return None, label + " IK Rig not found: " + path
    if not isinstance(obj, unreal.IKRigDefinition):
        return None, label + " path is a " + type(obj).__name__ + ", not an IKRigDefinition: " + path
    return obj, None

src, err = load_rig(args["source_rig"], "source")
if err:
    return ngg_error(err)
tgt, err = load_rig(args["target_rig"], "target")
if err:
    return ngg_error(err)

name, folder = split_path(args["asset_path"])
rt = asset_tools().create_asset(name, folder, unreal.IKRetargeter, unreal.IKRetargetFactory())
if rt is None:
    return ngg_error("could not create an IKRetargeter at " + args["asset_path"])

rc = unreal.IKRetargeterController.get_controller(rt)
if rc is None:
    lib.delete_asset(args["asset_path"])
    return ngg_error("could not get an IKRetargeterController (the asset was removed again)")

rc.set_ik_rig(unreal.RetargetSourceOrTarget.SOURCE, src)
rc.set_ik_rig(unreal.RetargetSourceOrTarget.TARGET, tgt)

for key, which in (("source_preview_mesh", unreal.RetargetSourceOrTarget.SOURCE),
                   ("target_preview_mesh", unreal.RetargetSourceOrTarget.TARGET)):
    if args.get(key):
        mesh, merr = load_skeletal_mesh(args[key])
        if merr:
            return ngg_error(merr)
        rc.set_preview_mesh(which, mesh)

# The op stack is what actually performs the retarget; without it the asset is
# inert, so create the standard set before mapping chains onto it.
rc.add_default_ops()

if args["auto_map"] != "none":
    mode = (unreal.AutoMapChainType.EXACT if args["auto_map"] == "exact"
            else unreal.AutoMapChainType.FUZZY)
    rc.auto_map_chains(mode, True)

# Read the mapping back per target chain. get_all_chain_settings() returns an
# empty array in 5.8 — chain settings moved into the retarget op stack — so the
# target rig's own chain list is the source of truth, queried one at a time.
mapped = []
tgt_ctrl = unreal.IKRigController.get_controller(tgt)
for c in tgt_ctrl.get_retarget_chains():
    cname = s(c.get_editor_property("chain_name"))
    if not cname:
        continue
    try:
        mapped.append({"target_chain": cname, "source_chain": s(rc.get_source_chain(cname))})
    except Exception:
        mapped.append({"target_chain": cname, "source_chain": None})

lib.save_asset(args["asset_path"], only_if_is_dirty=False)

ngg_result({
    "ok": True,
    "asset": args["asset_path"],
    "source_rig": args["source_rig"],
    "target_rig": args["target_rig"],
    "auto_map": args["auto_map"],
    "op_count": rc.get_num_retarget_ops(),
    "chain_mappings": mapped,
})
`);
}

/**
 * Map one source chain onto a target chain on an existing retargeter, for the
 * pairs auto-mapping got wrong.
 *
 * @param {object} opts
 * @param {string} opts.asset_path
 * @param {Array}  opts.mappings - [{target_chain, source_chain}]
 * @param {boolean}[opts.save=true]
 */
export async function ikRetargeterSetChainMapping({ asset_path, mappings, save } = {}) {
  return runPy(`
${pyArgs({ asset_path, mappings: mappings || [], save: save !== false })}
if not args.get("asset_path"):
    return ngg_error("asset_path is required")
if not args["mappings"]:
    return ngg_error("mappings[] is empty — nothing to do")

rt = unreal.load_asset(args["asset_path"])
if rt is None:
    return ngg_error("asset not found: " + args["asset_path"])
if not isinstance(rt, unreal.IKRetargeter):
    return ngg_error(args["asset_path"] + " is a " + type(rt).__name__ + ", not an IKRetargeter")

rc = unreal.IKRetargeterController.get_controller(rt)

# Same reason as in ikRetargeterCreate: chain settings live in the op stack, so
# the list of valid target chains comes from the target rig itself.
known = []
tgt_rig = rc.get_ik_rig(unreal.RetargetSourceOrTarget.TARGET)
if tgt_rig is not None:
    tgt_ctrl = unreal.IKRigController.get_controller(tgt_rig)
    for c in tgt_ctrl.get_retarget_chains():
        cname = s(c.get_editor_property("chain_name"))
        if cname:
            known.append(cname)

applied = []
for m in args["mappings"]:
    tgt, srcname = m.get("target_chain"), m.get("source_chain")
    if not tgt or srcname is None:
        return ngg_error("every mapping needs 'target_chain' and 'source_chain'")
    if not rc.set_source_chain(srcname, tgt):
        return ngg_error("could not map source '" + str(srcname) + "' onto target chain '" +
                         str(tgt) + "'. Target chains on this retargeter: " +
                         (", ".join(known) or "(none)"))
    applied.append({"target_chain": tgt, "source_chain": srcname})

result = {"ok": True, "asset": args["asset_path"], "applied": applied}
if args["save"]:
    result["saved"] = unreal.EditorAssetLibrary.save_asset(args["asset_path"], only_if_is_dirty=False)
ngg_result(result)
`);
}

// ---------------------------------------------------------------------------
// Control Rig
// ---------------------------------------------------------------------------

/**
 * Create a Control Rig blueprint for a skeletal mesh, with its hierarchy
 * imported from the mesh's skeleton.
 *
 * @param {object}  opts
 * @param {string}  opts.skeletal_mesh
 * @param {string}  [opts.asset_path]   - where to put it; defaults beside the mesh
 * @param {boolean} [opts.modular=false]
 */
export async function controlRigCreate({ skeletal_mesh, asset_path, modular } = {}) {
  return runPy(`
${pyArgs({ skeletal_mesh, asset_path, modular: !!modular })}
if not args.get("skeletal_mesh"):
    return ngg_error("skeletal_mesh is required")

mesh, err = load_skeletal_mesh(args["skeletal_mesh"])
if err:
    return ngg_error(err)

lib = unreal.EditorAssetLibrary
if args.get("asset_path") and lib.does_asset_exist(args["asset_path"]):
    return ngg_error("an asset already exists at " + args["asset_path"])

# This factory call both creates the blueprint and imports the skeleton
# hierarchy into it — doing those separately leaves an empty rig.
bp = unreal.ControlRigBlueprintFactory.create_control_rig_from_skeletal_mesh_or_skeleton(
    mesh, args["modular"])
if bp is None:
    return ngg_error("create_control_rig_from_skeletal_mesh_or_skeleton returned nothing for " +
                     args["skeletal_mesh"])

created_path = str(bp.get_path_name()).split(".")[0]

# The factory decides the location itself; move it if the caller asked for one.
moved = None
if args.get("asset_path") and args["asset_path"] != created_path:
    moved = lib.rename_asset(created_path, args["asset_path"])
    if not moved:
        return ngg_error("the Control Rig was created at " + created_path +
                         " but could not be moved to " + args["asset_path"])
    created_path = args["asset_path"]

lib.save_asset(created_path, only_if_is_dirty=False)

hierarchy_count = None
try:
    hierarchy = bp.get_hierarchy()
    hierarchy_count = len(hierarchy.get_all_keys(True)) if hierarchy else None
except Exception:
    pass

ngg_result({
    "ok": True,
    "asset": created_path,
    "skeletal_mesh": args["skeletal_mesh"],
    "modular": args["modular"],
    "hierarchy_element_count": hierarchy_count,
    "moved": bool(moved),
})
`);
}
