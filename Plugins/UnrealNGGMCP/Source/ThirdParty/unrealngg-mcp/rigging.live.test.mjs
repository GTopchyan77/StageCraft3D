#!/usr/bin/env node
// Copyright 2025-2026 NGG. All Rights Reserved.
// rigging.live.test.mjs — end-to-end exercise of the IK Rig / IK Retargeter /
// Control Rig tools against a RUNNING UE5 editor.
//
// Builds two IK Rigs on the project's mannequin, retargets between them, reads
// everything back, creates a Control Rig, then deletes it all.
//
// Run:  node rigging.live.test.mjs
//
// Needs a skeletal mesh; it uses the Third Person template mannequin and skips
// with a clear message if the project does not have one.

import * as rig from "./rigging.js";
import * as ue5 from "./ue5client.js";

const ROOT = "/Game/__NGGRigTest__";
const RIG_A = `${ROOT}/IK_SourceRig`;
const RIG_B = `${ROOT}/IK_TargetRig`;
const RTG = `${ROOT}/RTG_Test`;
const CR = `${ROOT}/CR_Test`;

let pass = 0, fail = 0;

function check(name, cond, detail) {
  if (cond) { pass++; console.log(`  PASS  ${name}`); }
  else { fail++; console.log(`  FAIL  ${name}${detail ? "\n        " + detail : ""}`); }
}

async function step(name, fn) {
  try {
    const r = await fn();
    if (r && r.ok === false) { fail++; console.log(`  FAIL  ${name}\n        ${r.error}`); return null; }
    pass++; console.log(`  PASS  ${name}`);
    return r?.data ?? r;
  } catch (e) {
    fail++; console.log(`  FAIL  ${name}\n        ${e.message}`);
    return null;
  }
}

async function py(body) {
  const r = await ue5.execPython(`import unreal\n${body}\n`, "execute_file");
  return (r.log ?? []).map(e => e.output ?? "").join("");
}

// Find a mannequin-style skeletal mesh. Bone names cannot be read from Python
// (Skeleton.bone_tree yields opaque structs), so probe by actually building a
// throwaway rig and seeing whether the standard bones are accepted.
const meshProbe = await py(`
ar = unreal.AssetRegistryHelpers.get_asset_registry()
f = unreal.ARFilter(class_paths=[unreal.TopLevelAssetPath('/Script/Engine','SkeletalMesh')],
                    package_paths=['/Game'], recursive_paths=True)
tools = unreal.AssetToolsHelpers.get_asset_tools()
lib = unreal.EditorAssetLibrary
found = ''
for a in ar.get_assets(f):
    path = str(a.package_name)
    m = unreal.load_asset(path)
    if not m or not m.skeleton:
        continue
    probe = tools.create_asset('IK_BoneProbe', '/Game/__NGGRigProbeTmp__',
                               unreal.IKRigDefinition, unreal.IKRigDefinitionFactory())
    okmesh = False
    if probe:
        c = unreal.IKRigController.get_controller(probe)
        if c and c.set_skeletal_mesh(m):
            g = c.add_new_goal('BoneProbe', 'hand_l')
            okmesh = bool(g) and str(g) != 'None'
    if lib.does_directory_exist('/Game/__NGGRigProbeTmp__'):
        for p in lib.list_assets('/Game/__NGGRigProbeTmp__', recursive=True):
            lib.delete_asset(p.split('.')[0])
        lib.delete_directory('/Game/__NGGRigProbeTmp__')
    if okmesh:
        found = path; break
print('MESH=' + found)
`);
const MESH = (meshProbe.match(/MESH=(\S+)/) || [])[1];
if (!MESH) {
  console.log("SKIP: no skeletal mesh in /Game accepts the standard mannequin bone 'hand_l'");
  process.exit(0);
}
console.log(`\nusing mesh: ${MESH}`);

console.log("\n=== ik rig ===");
const a = await step("create the source IK Rig", () => rig.ikRigCreate({
  asset_path: RIG_A,
  skeletal_mesh: MESH,
  retarget_root: "pelvis",
  goals: [{ name: "Hand_L", bone: "hand_l" }, { name: "Hand_R", bone: "hand_r" }],
  chains: [
    { name: "LeftArm", start_bone: "upperarm_l", end_bone: "hand_l", goal: "Hand_L" },
    { name: "RightArm", start_bone: "upperarm_r", end_bone: "hand_r", goal: "Hand_R" },
  ],
}));
check("a solver was added", a && a.solver_count >= 1, `solver_count=${a?.solver_count}`);
// apply_auto_fbik() creates its own limb goals (LeftHandIK, RightFootIK, ...),
// so the rig ends up with more goals than were requested. Assert ours are there
// rather than pinning a total that the engine controls.
check("the requested goals are present",
  a && ["Hand_L", "Hand_R"].every(n => a.goals?.some(g => g.name === n)),
  JSON.stringify(a?.goals?.map(g => g.name)));
check("auto-fbik contributed its own limb goals",
  a && a.goals?.some(g => /IK$/.test(String(g.name))),
  JSON.stringify(a?.goals?.map(g => g.name)));
check("goal is bound to its bone", a?.goals?.some(g => g.bone === "hand_l"), JSON.stringify(a?.goals));
check("both chains exist", a && a.retarget_chains?.length === 2, JSON.stringify(a?.retarget_chains?.map(c => c.name)));
check("chain records its bones",
  a?.retarget_chains?.some(c => c.start_bone === "upperarm_l" && c.end_bone === "hand_l"),
  JSON.stringify(a?.retarget_chains?.[0]));
check("retarget root applied", a && /pelvis/.test(String(a.retarget_root)), String(a?.retarget_root));
check("no warnings on the happy path", !a?.warnings, JSON.stringify(a?.warnings));

await step("create the target IK Rig", () => rig.ikRigCreate({
  asset_path: RIG_B,
  skeletal_mesh: MESH,
  retarget_root: "pelvis",
  chains: [
    { name: "LeftArm", start_bone: "upperarm_l", end_bone: "hand_l" },
    { name: "RightArm", start_bone: "upperarm_r", end_bone: "hand_r" },
  ],
}));

console.log("\n=== ik rig validation ===");
const dupe = await rig.ikRigCreate({ asset_path: RIG_A, skeletal_mesh: MESH });
check("creating over an existing asset is refused",
  dupe.ok === false && /already exists/.test(dupe.error), dupe.error);

const noMesh = await rig.ikRigCreate({ asset_path: `${ROOT}/IK_Bad`, skeletal_mesh: `${ROOT}/NoSuchMesh` });
check("missing skeletal mesh is rejected", noMesh.ok === false && /not found/.test(noMesh.error), noMesh.error);

const wrongType = await rig.ikRigCreate({ asset_path: `${ROOT}/IK_Bad2`, skeletal_mesh: RIG_A });
check("a non-mesh path says what it actually is",
  wrongType.ok === false && /not a SkeletalMesh/.test(wrongType.error), wrongType.error);

const badBone = await rig.ikRigCreate({
  asset_path: `${ROOT}/IK_Bad3`, skeletal_mesh: MESH,
  goals: [{ name: "Ghost", bone: "no_such_bone" }],
});
check("a goal on a nonexistent bone is rejected",
  badBone.ok === false && /could not add goal/.test(badBone.error), badBone.error);

const badChain = await rig.ikRigCreate({
  asset_path: `${ROOT}/IK_Bad4`, skeletal_mesh: MESH,
  chains: [{ name: "Ghost", start_bone: "no_such_bone", end_bone: "hand_l" }],
});
check("a chain on a nonexistent bone is rejected",
  badChain.ok === false && /could not add retarget chain/.test(badChain.error), badChain.error);

console.log("\n=== ik rig read ===");
const read = await step("read the rig back", () => rig.ikRigRead({ asset_path: RIG_A }));
check("read reports the mesh", read && /SKM|SK_/.test(String(read.skeletal_mesh)), String(read?.skeletal_mesh));
check("read reports goals and chains",
  read?.goals?.length >= 2 && read?.retarget_chains?.length === 2,
  `${read?.goals?.length} goals, ${read?.retarget_chains?.length} chains`);

const readWrong = await rig.ikRigRead({ asset_path: MESH });
check("reading a non-IK-Rig says what it is",
  readWrong.ok === false && /not an IKRigDefinition/.test(readWrong.error), readWrong.error);

console.log("\n=== retargeter ===");
const rt = await step("create a retargeter with exact auto-mapping", () => rig.ikRetargeterCreate({
  asset_path: RTG, source_rig: RIG_A, target_rig: RIG_B, auto_map: "exact",
}));
check("the default op stack was created", rt && rt.op_count > 0, `op_count=${rt?.op_count}`);
check("chains were auto-mapped", rt && rt.chain_mappings?.length >= 1, JSON.stringify(rt?.chain_mappings));

const badMap = await rig.ikRetargeterCreate({
  asset_path: `${ROOT}/RTG_Bad`, source_rig: RIG_A, target_rig: RIG_B, auto_map: "sideways",
});
check("unknown auto_map is rejected", badMap.ok === false && /exact.*fuzzy.*none/.test(badMap.error), badMap.error);

const missingRig = await rig.ikRetargeterCreate({
  asset_path: `${ROOT}/RTG_Bad2`, source_rig: `${ROOT}/NoSuchRig`, target_rig: RIG_B,
});
check("missing source rig is rejected", missingRig.ok === false && /source IK Rig not found/.test(missingRig.error), missingRig.error);

const rigIsMesh = await rig.ikRetargeterCreate({
  asset_path: `${ROOT}/RTG_Bad3`, source_rig: MESH, target_rig: RIG_B,
});
check("passing a mesh where a rig belongs says so",
  rigIsMesh.ok === false && /not an IKRigDefinition/.test(rigIsMesh.error), rigIsMesh.error);

console.log("\n=== chain mapping ===");
const remap = await step("remap a chain by hand", () => rig.ikRetargeterSetChainMapping({
  asset_path: RTG, mappings: [{ target_chain: "LeftArm", source_chain: "RightArm" }],
}));
check("mapping applied", remap?.applied?.[0]?.source_chain === "RightArm", JSON.stringify(remap?.applied));

const badRemap = await rig.ikRetargeterSetChainMapping({
  asset_path: RTG, mappings: [{ target_chain: "NoSuchChain", source_chain: "LeftArm" }],
});
check("unknown target chain lists the real ones",
  badRemap.ok === false && /Target chains on this retargeter/.test(badRemap.error), badRemap.error);

const emptyRemap = await rig.ikRetargeterSetChainMapping({ asset_path: RTG, mappings: [] });
check("empty mappings rejected", emptyRemap.ok === false && /empty/.test(emptyRemap.error), emptyRemap.error);

console.log("\n=== control rig ===");
const cr = await step("create a Control Rig from the mesh", () => rig.controlRigCreate({
  skeletal_mesh: MESH, asset_path: CR,
}));
check("control rig landed at the requested path", cr && cr.asset === CR, String(cr?.asset));
check("hierarchy was imported from the skeleton",
  cr && (cr.hierarchy_element_count === null || cr.hierarchy_element_count > 0),
  String(cr?.hierarchy_element_count));

const crBadMesh = await rig.controlRigCreate({ skeletal_mesh: `${ROOT}/NoSuchMesh` });
check("control rig on a missing mesh is rejected",
  crBadMesh.ok === false && /not found/.test(crBadMesh.error), crBadMesh.error);

console.log("\n=== teardown ===");
const cleanup = await py(`
lib = unreal.EditorAssetLibrary
if lib.does_directory_exist('${ROOT}'):
    for p in lib.list_assets('${ROOT}', recursive=True):
        try:
            lib.delete_asset(p.split('.')[0])
        except Exception:
            pass
    lib.delete_directory('${ROOT}')
print('DIR_EXISTS=%s' % lib.does_directory_exist('${ROOT}'))
`);
check("scratch assets removed", /DIR_EXISTS=False/.test(cleanup), cleanup.trim().slice(-200));

console.log(`\n━━━ rigging: ${pass} passed, ${fail} failed ━━━`);
process.exit(fail ? 1 : 0);
