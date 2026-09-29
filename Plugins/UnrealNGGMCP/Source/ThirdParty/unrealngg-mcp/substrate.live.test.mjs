#!/usr/bin/env node
// Copyright 2025-2026 NGG. All Rights Reserved.
// substrate.live.test.mjs — end-to-end exercise of the Substrate material tools
// against a RUNNING UE5 editor.
//
// Skips with a clear message if the project has Substrate turned off, since
// there is nothing meaningful to build in that case.
//
// Run:  node substrate.live.test.mjs

import * as sub from "./substrate.js";
import * as ue5 from "./ue5client.js";

const ROOT = "/Game/__NGGSubstrateTest__";

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

// --- project state ----------------------------------------------------------

const state = await step("read the project's Substrate state", () => sub.substrateRead({}));
if (state?.substrate_enabled !== true) {
  console.log("SKIP: r.Substrate is 0 in this project — nothing to build");
  process.exit(0);
}
console.log("\nSubstrate is enabled");

// A texture to drive base colour, if the project has one.
const texProbe = await py(`
ar = unreal.AssetRegistryHelpers.get_asset_registry()
f = unreal.ARFilter(class_paths=[unreal.TopLevelAssetPath('/Script/Engine','Texture2D')],
                    package_paths=['/Game'], recursive_paths=True)
a = ar.get_assets(f)
print('TEX=' + (str(a[0].package_name) if len(a) else ''))
`);
const TEX = (texProbe.match(/TEX=(\S+)/) || [])[1];

// --- slab -------------------------------------------------------------------

console.log("\n=== slab ===");
const slab = await step("build a slab material from base colour / metallic / roughness",
  () => sub.substrateMaterialCreate({
    asset_path: `${ROOT}/M_Slab`,
    base_color: [0.8, 0.2, 0.15],
    metallic: 0.3,
    specular: 0.6,
    roughness: 0.45,
    emissive_color: [0.1, 0.0, 0.0],
  }));
check("the root node is the slab BSDF", slab?.root_node === "SubstrateSlabBSDF", String(slab?.root_node));
check("the metalness converter was inserted",
  slab?.nodes_created?.some(n => n.node === "MaterialExpressionSubstrateMetalnessToDiffuseAlbedoF0"),
  JSON.stringify(slab?.nodes_created?.map(n => n.node)));
check("the converter feeds Diffuse Albedo and F0, which is what a slab actually has",
  slab?.connections?.includes("converter.DiffuseAlbedo -> Diffuse Albedo") &&
  slab?.connections?.includes("converter.F0 -> F0"),
  JSON.stringify(slab?.connections));
check("the slab has no BaseColor pin at all — confirming the converter is not optional",
  slab?.bsdf_pins && !slab.bsdf_pins.includes("BaseColor") && slab.bsdf_pins.includes("Diffuse Albedo"),
  JSON.stringify(slab?.bsdf_pins));
check("emissive resolved past the space in 'Emissive Color'",
  slab?.connections?.includes("emissive"), JSON.stringify(slab?.connections));
check("it saved", slab?.saved === true, String(slab?.saved));

const readSlab = await step("read the slab material back", () => sub.substrateRead({
  asset_path: `${ROOT}/M_Slab`,
}));
check("it reads as a Substrate graph", readSlab?.is_substrate_graph === true, String(readSlab?.is_substrate_graph));
check("both Substrate nodes are counted", readSlab?.substrate_node_count === 2,
  String(readSlab?.substrate_node_count));
check("constants were used, so no parameters are exposed",
  readSlab?.scalar_parameters?.length === 0 && readSlab?.vector_parameters?.length === 0,
  JSON.stringify({ s: readSlab?.scalar_parameters, v: readSlab?.vector_parameters }));

// --- parameters -------------------------------------------------------------

console.log("\n=== parameters ===");
const param = await step("build the same material with instance parameters",
  () => sub.substrateMaterialCreate({
    asset_path: `${ROOT}/M_SlabParams`,
    base_color: [0.5, 0.5, 0.9],
    metallic: 0.0,
    roughness: 0.3,
    use_parameters: true,
    ...(TEX ? { base_color_texture: TEX } : {}),
  }));
check("scalar parameters are exposed",
  param?.scalar_parameters?.includes("Metallic") && param?.scalar_parameters?.includes("Roughness"),
  JSON.stringify(param?.scalar_parameters));
if (TEX) {
  check("the texture became a texture parameter",
    param?.texture_parameters?.includes("BaseColorTexture"),
    JSON.stringify(param?.texture_parameters));
  check("a base_color_texture takes precedence over the base_color constant",
    !param?.vector_parameters?.includes("BaseColor"),
    JSON.stringify(param?.vector_parameters));
} else {
  check("a vector parameter is exposed for base colour",
    param?.vector_parameters?.includes("BaseColor"), JSON.stringify(param?.vector_parameters));
}

// --- other shading models ---------------------------------------------------

console.log("\n=== other shading models ===");
const unlit = await step("build an unlit material", () => sub.substrateMaterialCreate({
  asset_path: `${ROOT}/M_Unlit`, shading: "unlit", emissive_color: [1.0, 0.4, 0.0],
}));
check("the root node is the unlit BSDF", unlit?.root_node === "SubstrateUnlitBSDF", String(unlit?.root_node));
check("emissive resolved on a node that spells it without a space",
  unlit?.bsdf_pins?.includes("EmissiveColor") && unlit?.connections?.includes("emissive"),
  JSON.stringify({ pins: unlit?.bsdf_pins, conn: unlit?.connections }));
check("no converter for a node that has no F0",
  !unlit?.nodes_created?.some(n => /MetalnessToDiffuse/.test(n.node)),
  JSON.stringify(unlit?.nodes_created?.map(n => n.node)));

const toon = await step("build a toon material", () => sub.substrateMaterialCreate({
  asset_path: `${ROOT}/M_Toon`, shading: "toon",
  base_color: [0.2, 0.7, 0.3], metallic: 0.0, roughness: 0.8,
}));
check("toon takes base colour directly, with no converter",
  toon?.connections?.includes("BaseColor -> base color") &&
  !toon?.nodes_created?.some(n => /MetalnessToDiffuse/.test(n.node)),
  JSON.stringify({ conn: toon?.connections, nodes: toon?.nodes_created?.map(n => n.node) }));

const clear = await step("build a clear coat material", () => sub.substrateMaterialCreate({
  asset_path: `${ROOT}/M_ClearCoat`, shading: "clearcoat",
  base_color: [0.1, 0.1, 0.1], metallic: 1.0, roughness: 0.1,
  blend_mode: "opaque", two_sided: true,
}));
check("clear coat also routes through the converter",
  clear?.connections?.some(c => /converter\.F0/.test(c)), JSON.stringify(clear?.connections));
check("two_sided was applied", clear?.two_sided === true, String(clear?.two_sided));

// --- validation -------------------------------------------------------------

console.log("\n=== validation ===");
const noPath = await sub.substrateMaterialCreate({ base_color: [1, 1, 1] });
check("no asset_path is refused", noPath.ok === false && /asset_path is required/.test(noPath.error), noPath.error);

const badShading = await sub.substrateMaterialCreate({
  asset_path: `${ROOT}/M_Bad`, shading: "gouraud",
});
check("an unknown shading model lists the real ones",
  badShading.ok === false && /clearcoat.*eye.*hair.*slab|slab/.test(badShading.error), badShading.error);

const dupe = await sub.substrateMaterialCreate({ asset_path: `${ROOT}/M_Slab`, roughness: 0.5 });
check("creating over an existing asset is refused",
  dupe.ok === false && /already exists/.test(dupe.error), dupe.error);

const overwritten = await step("overwrite:true replaces it", () => sub.substrateMaterialCreate({
  asset_path: `${ROOT}/M_Slab`, roughness: 0.9, overwrite: true,
}));
check("the replacement has only what was asked for this time",
  overwritten?.connections?.length === 1 && overwritten.connections[0] === "roughness",
  JSON.stringify(overwritten?.connections));

// hair has no metallic pin; asking for one must say so rather than quietly skip
const hairMetal = await sub.substrateMaterialCreate({
  asset_path: `${ROOT}/M_HairBad`, shading: "hair", metallic: 0.5,
});
check("a pin the shading model does not have is reported, not ignored",
  hairMetal.ok === false && /has no metallic input/.test(hairMetal.error), hairMetal.error);
// A freshly created Material often refuses to delete, so this case is caught
// before anything is created rather than rolled back afterwards.
const hairGone = await py(`print('GONE=%s' % (not unreal.EditorAssetLibrary.does_asset_exist('${ROOT}/M_HairBad')))`);
check("no asset was created for the rejected request at all", /GONE=True/.test(hairGone), hairGone.trim());

const badTex = await sub.substrateMaterialCreate({
  asset_path: `${ROOT}/M_BadTex`, base_color_texture: `${ROOT}/NoSuchTexture`,
});
check("a missing texture is rejected", badTex.ok === false && /texture not found/.test(badTex.error), badTex.error);

const badBlend = await sub.substrateMaterialCreate({
  asset_path: `${ROOT}/M_BadBlend`, roughness: 0.5, blend_mode: "xor",
});
check("an unknown blend mode lists the real ones",
  badBlend.ok === false && /blend_mode must be one of/.test(badBlend.error), badBlend.error);

const strayCheck = await py(`
lib = unreal.EditorAssetLibrary
names = [p.split('/')[-1].split('.')[0] for p in lib.list_assets('${ROOT}', recursive=True)]
print('STRAY=' + ','.join(sorted(n for n in names if 'Bad' in n)))
`);
check("none of the rejected requests left an asset behind",
  /STRAY=\s*$/m.test(strayCheck) || /STRAY=$/.test(strayCheck.trim()), strayCheck.trim());

const readInstance = await sub.substrateRead({ asset_path: `${ROOT}/M_Slab.M_Slab` });
check("reading with an object path still resolves or fails clearly",
  readInstance.ok === true || /not found/.test(readInstance.error || ""),
  readInstance.error || "ok");

// --- teardown ---------------------------------------------------------------
//
// A material that has compiled cannot always be deleted on the first attempt, so
// retry across separate calls — a sleep inside one call blocks the game thread
// and nothing progresses.

console.log("\n=== teardown ===");
let gone = false;
for (let attempt = 1; attempt <= 6 && !gone; attempt++) {
  const out = await py(`
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
  gone = /DIR_EXISTS=False/.test(out);
  if (!gone) await new Promise(r => setTimeout(r, 2000));
}
check("scratch assets removed", gone, `still present after 6 attempts — a compiling material can hold a reference`);

console.log(`\n━━━ substrate: ${pass} passed, ${fail} failed ━━━`);
process.exit(fail ? 1 : 0);
