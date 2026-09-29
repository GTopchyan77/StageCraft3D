#!/usr/bin/env node
// Copyright 2025-2026 NGG. All Rights Reserved.
// assetconfig.live.test.mjs — end-to-end exercise of the collision / physical
// material / foliage type / texture tools against a RUNNING UE5 editor.
//
// Works on duplicates in a scratch folder so the project's own assets are never
// modified, and removes the folder afterwards.
//
// Run:  node assetconfig.live.test.mjs

import * as cfg from "./assetconfig.js";
import * as ue5 from "./ue5client.js";

const ROOT = "/Game/__NGGAssetCfgTest__";
const MESH = `${ROOT}/SM_CollisionProbe`;
const TEX = `${ROOT}/T_Probe`;
const PHYS = `${ROOT}/PM_Test`;
const FOLIAGE = `${ROOT}/FT_Test`;

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

// --- setup ------------------------------------------------------------------

const setup = await py(`
ar = unreal.AssetRegistryHelpers.get_asset_registry()
lib = unreal.EditorAssetLibrary

def first(cls_name):
    f = unreal.ARFilter(class_paths=[unreal.TopLevelAssetPath('/Script/Engine', cls_name)],
                        package_paths=['/Game'], recursive_paths=True)
    for a in ar.get_assets(f):
        p = str(a.package_name)
        if not p.startswith('${ROOT}'):
            return p
    return ''

sm = first('StaticMesh')
tx = first('Texture2D')
print('SM=' + sm)
print('TX=' + tx)
if sm:
    lib.duplicate_asset(sm, '${MESH}')
if tx:
    lib.duplicate_asset(tx, '${TEX}')
print('DUPSM=%s' % lib.does_asset_exist('${MESH}'))
print('DUPTX=%s' % lib.does_asset_exist('${TEX}'))
`);

if (!/DUPSM=True/.test(setup)) {
  console.log("SKIP: no static mesh in /Game to duplicate");
  console.log(setup);
  process.exit(0);
}
const HAS_TEXTURE = /DUPTX=True/.test(setup);
console.log(`\nscratch mesh:    ${MESH}`);
console.log(`scratch texture: ${HAS_TEXTURE ? TEX : "(none — texture checks skipped)"}`);

// --- physical material ------------------------------------------------------

console.log("\n=== physical material ===");
const pm = await step("create a physical material", () => cfg.physicalMaterialCreate({
  asset_path: PHYS, friction: 0.35, restitution: 0.2, density: 2.4,
  friction_combine_mode: "max",
}));
check("it reports itself as created", pm?.created === true, String(pm?.created));
check("friction stuck", Math.abs(pm?.friction - 0.35) < 1e-5, String(pm?.friction));
check("density stuck", Math.abs(pm?.density - 2.4) < 1e-5, String(pm?.density));
check("combine mode applied", /MAX/.test(String(pm?.friction_combine_mode)), String(pm?.friction_combine_mode));
check("its override flag was set too — the value is inert without it",
  pm?.override_friction_combine_mode === true, String(pm?.override_friction_combine_mode));

const pmDupe = await cfg.physicalMaterialCreate({ asset_path: PHYS, friction: 0.9 });
check("creating over an existing asset is refused",
  pmDupe.ok === false && /already exists/.test(pmDupe.error), pmDupe.error);

const pmUpdate = await step("update in place with overwrite", () => cfg.physicalMaterialCreate({
  asset_path: PHYS, friction: 0.9, overwrite: true,
}));
check("update reports created:false", pmUpdate?.created === false, String(pmUpdate?.created));
check("the new friction took", Math.abs(pmUpdate?.friction - 0.9) < 1e-5, String(pmUpdate?.friction));

const pmBadSurface = await cfg.physicalMaterialCreate({
  asset_path: `${ROOT}/PM_Bad`, surface_type: "quicksand",
});
check("an unknown surface type explains where surface types come from",
  pmBadSurface.ok === false && /DefaultEngine\.ini/.test(pmBadSurface.error), pmBadSurface.error);

const pmBadCombine = await cfg.physicalMaterialCreate({
  asset_path: `${ROOT}/PM_Bad2`, friction_combine_mode: "sideways",
});
check("an unknown combine mode lists the real options",
  pmBadCombine.ok === false && /AVERAGE.*MAX.*MIN.*MULTIPLY/.test(pmBadCombine.error), pmBadCombine.error);

// --- collision --------------------------------------------------------------

console.log("\n=== collision ===");
const col = await step("replace collision with boxes and a physical material", () => cfg.collisionConfigure({
  static_meshes: [MESH],
  remove_existing: true,
  add_shape: "box",
  shape_count: 2,
  trace_flag: "use_simple_as_complex",
  physical_material: PHYS,
}));
check("two simple shapes are present", col?.meshes?.[0]?.after?.simple_collision_count === 2,
  String(col?.meshes?.[0]?.after?.simple_collision_count));
check("the trace flag took",
  /SIMPLE_AS_COMPLEX/.test(String(col?.meshes?.[0]?.after?.collision_complexity)),
  String(col?.meshes?.[0]?.after?.collision_complexity));
check("the physical material is bound",
  String(col?.meshes?.[0]?.after?.physical_material || "").includes("PM_Test"),
  String(col?.meshes?.[0]?.after?.physical_material));
check("before/after are both reported",
  col?.meshes?.[0]?.before && col?.meshes?.[0]?.after, JSON.stringify(col?.meshes?.[0]?.before));

const conv = await step("convex decomposition", () => cfg.collisionConfigure({
  static_meshes: [MESH], remove_existing: true, convex_hulls: 4, convex_max_verts: 12,
}));
check("decomposition reported success", conv?.meshes?.[0]?.convex_decomposed === true,
  String(conv?.meshes?.[0]?.convex_decomposed));
check("hulls landed on the mesh", conv?.meshes?.[0]?.after?.convex_collision_count >= 1,
  String(conv?.meshes?.[0]?.after?.convex_collision_count));

console.log("\n=== collision validation ===");
const colEmpty = await cfg.collisionConfigure({ static_meshes: [] });
check("empty mesh list is refused", colEmpty.ok === false && /empty/.test(colEmpty.error), colEmpty.error);

const colNothing = await cfg.collisionConfigure({ static_meshes: [MESH] });
check("a call that changes nothing is refused",
  colNothing.ok === false && /nothing to change/.test(colNothing.error), colNothing.error);

const colBadShape = await cfg.collisionConfigure({ static_meshes: [MESH], add_shape: "pyramid" });
check("an unknown shape lists the real ones",
  colBadShape.ok === false && /box.*capsule.*sphere|box.*ndop/.test(colBadShape.error), colBadShape.error);

const colBadFlag = await cfg.collisionConfigure({ static_meshes: [MESH], trace_flag: "use_magic" });
check("an unknown trace flag lists the real ones",
  colBadFlag.ok === false && /CTF_USE_DEFAULT/.test(colBadFlag.error), colBadFlag.error);

const colBadPhys = await cfg.collisionConfigure({
  static_meshes: [MESH], physical_material: `${ROOT}/NoSuchPM`,
});
check("a missing physical material is rejected",
  colBadPhys.ok === false && /not found/.test(colBadPhys.error), colBadPhys.error);

const colWrongPhys = await cfg.collisionConfigure({
  static_meshes: [MESH], physical_material: MESH,
});
check("passing a mesh where a physical material belongs says so",
  colWrongPhys.ok === false && /not a PhysicalMaterial/.test(colWrongPhys.error), colWrongPhys.error);

// --- foliage ----------------------------------------------------------------

console.log("\n=== foliage type ===");
const fol = await step("create a foliage type", () => cfg.foliageTypeCreate({
  asset_path: FOLIAGE,
  static_mesh: MESH,
  density: 250,
  radius: 120,
  scaling: "uniform",
  scale_x: [0.8, 1.4],
  ground_slope_angle: [0, 35],
  cull_distance: [2000, 6000],
  align_to_normal: false,
  random_yaw: true,
}));
check("it was created", fol?.created === true, String(fol?.created));
check("the mesh is bound", String(fol?.static_mesh || "").includes("SM_CollisionProbe"), String(fol?.static_mesh));
check("density stuck — a property dir() does not even list",
  Math.abs(fol?.density - 250) < 1e-5, String(fol?.density));
check("radius stuck", Math.abs(fol?.radius - 120) < 1e-5, String(fol?.radius));
check("the float interval round-tripped",
  Math.abs(fol?.scale_x?.[0] - 0.8) < 1e-5 && Math.abs(fol?.scale_x?.[1] - 1.4) < 1e-5,
  JSON.stringify(fol?.scale_x));
check("the int interval round-tripped",
  fol?.cull_distance?.[0] === 2000 && fol?.cull_distance?.[1] === 6000,
  JSON.stringify(fol?.cull_distance));
check("ground slope took", Math.abs(fol?.ground_slope_angle?.[1] - 35) < 1e-5,
  JSON.stringify(fol?.ground_slope_angle));
check("align_to_normal:false is honoured, not treated as absent",
  fol?.align_to_normal === false, String(fol?.align_to_normal));

console.log("\n=== foliage validation ===");
const folNoMesh = await cfg.foliageTypeCreate({ asset_path: `${ROOT}/FT_Bad` });
check("a foliage type with no mesh is refused",
  folNoMesh.ok === false && /scatters nothing/.test(folNoMesh.error), folNoMesh.error);

const folDupe = await cfg.foliageTypeCreate({ asset_path: FOLIAGE, static_mesh: MESH });
check("creating over an existing asset is refused",
  folDupe.ok === false && /already exists/.test(folDupe.error), folDupe.error);

const folBadInterval = await cfg.foliageTypeCreate({
  asset_path: `${ROOT}/FT_Bad2`, static_mesh: MESH, scale_x: [1],
});
check("a one-element interval is refused",
  folBadInterval.ok === false && /\[min, max\] pair/.test(folBadInterval.error), folBadInterval.error);

const folBadScaling = await cfg.foliageTypeCreate({
  asset_path: `${ROOT}/FT_Bad3`, static_mesh: MESH, scaling: "wobbly",
});
check("an unknown scaling mode lists the real ones",
  folBadScaling.ok === false && /FREE.*LOCK_XY.*UNIFORM/.test(folBadScaling.error), folBadScaling.error);

const folUpdate = await step("update the foliage type in place", () => cfg.foliageTypeCreate({
  asset_path: FOLIAGE, static_mesh: MESH, density: 400, overwrite: true,
}));
check("update kept the earlier radius", Math.abs(folUpdate?.radius - 120) < 1e-5, String(folUpdate?.radius));
check("update changed the density", Math.abs(folUpdate?.density - 400) < 1e-5, String(folUpdate?.density));

// --- textures ---------------------------------------------------------------

if (HAS_TEXTURE) {
  console.log("\n=== texture ===");
  const tex = await step("set texture up as a normal map", () => cfg.textureConfigure({
    textures: [TEX],
    compression_settings: "normalmap",
    srgb: false,
    lod_group: "character_normal_map",
    mip_gen_settings: "simple_average",
    max_texture_size: 1024,
    address_x: "clamp",
  }));
  check("short enum name resolved to TC_NORMALMAP",
    /TC_NORMALMAP/.test(String(tex?.textures?.[0]?.after?.compression_settings)),
    String(tex?.textures?.[0]?.after?.compression_settings));
  check("srgb:false is honoured, not treated as absent",
    tex?.textures?.[0]?.after?.srgb === false, String(tex?.textures?.[0]?.after?.srgb));
  check("lod group resolved past its TEXTUREGROUP_ prefix",
    /CHARACTER_NORMAL_MAP/.test(String(tex?.textures?.[0]?.after?.lod_group)),
    String(tex?.textures?.[0]?.after?.lod_group));
  check("mip gen resolved past its TMGS_ prefix",
    /SIMPLE_AVERAGE/.test(String(tex?.textures?.[0]?.after?.mip_gen_settings)),
    String(tex?.textures?.[0]?.after?.mip_gen_settings));
  check("address mode resolved past its TA_ prefix",
    /TA_CLAMP/.test(String(tex?.textures?.[0]?.after?.address_x)),
    String(tex?.textures?.[0]?.after?.address_x));
  check("max size took", tex?.textures?.[0]?.after?.max_texture_size === 1024,
    String(tex?.textures?.[0]?.after?.max_texture_size));
  check("before differs from after, so the change is real",
    JSON.stringify(tex?.textures?.[0]?.before) !== JSON.stringify(tex?.textures?.[0]?.after),
    JSON.stringify(tex?.textures?.[0]?.before));

  const texFull = await step("the full enum name also works", () => cfg.textureConfigure({
    textures: [TEX], compression_settings: "TC_MASKS",
  }));
  check("TC_MASKS applied", /TC_MASKS/.test(String(texFull?.textures?.[0]?.after?.compression_settings)),
    String(texFull?.textures?.[0]?.after?.compression_settings));

  console.log("\n=== texture validation ===");
  const texEmpty = await cfg.textureConfigure({ textures: [] });
  check("empty texture list is refused", texEmpty.ok === false && /empty/.test(texEmpty.error), texEmpty.error);

  const texNothing = await cfg.textureConfigure({ textures: [TEX] });
  check("no settings is refused", texNothing.ok === false && /no settings given/.test(texNothing.error), texNothing.error);

  const texBadEnum = await cfg.textureConfigure({ textures: [TEX], compression_settings: "jpeg" });
  check("an unknown compression setting lists the real ones",
    texBadEnum.ok === false && /TC_NORMALMAP/.test(texBadEnum.error), texBadEnum.error);

  const texWrongType = await cfg.textureConfigure({ textures: [MESH], srgb: true });
  check("a non-texture path says what it actually is",
    texWrongType.ok === false && /not a Texture/.test(texWrongType.error), texWrongType.error);
}

// --- teardown ---------------------------------------------------------------

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
check("scratch assets removed", /DIR_EXISTS=False/.test(cleanup), cleanup.trim().slice(-300));

console.log(`\n━━━ assetconfig: ${pass} passed, ${fail} failed ━━━`);
process.exit(fail ? 1 : 0);
