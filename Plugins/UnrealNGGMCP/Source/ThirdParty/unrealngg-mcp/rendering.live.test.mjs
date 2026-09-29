#!/usr/bin/env node
// Copyright 2025-2026 NGG. All Rights Reserved.
// rendering.live.test.mjs — end-to-end exercise of the Nanite / Lumen / renderer
// settings tools against a RUNNING UE5 editor.
//
// Works on a scratch level and a duplicated mesh so the project's own content is
// never touched, restores the level that was open, and removes the ini key it
// writes.
//
// Run:  node rendering.live.test.mjs

import * as render from "./rendering.js";
import * as ue5 from "./ue5client.js";

const ROOT = "/Game/__NGGRenderTest__";
const LEVEL = `${ROOT}/L_Render`;
const MESH = `${ROOT}/SM_NaniteProbe`;
const TEST_KEY = "r.NGGRenderSelfTest";

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
ues = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
w = ues.get_editor_world()
print('ORIGINAL=' + (str(w.get_path_name()).split('.')[0] if w else ''))

ar = unreal.AssetRegistryHelpers.get_asset_registry()
f = unreal.ARFilter(class_paths=[unreal.TopLevelAssetPath('/Script/Engine','StaticMesh')],
                    package_paths=['/Game'], recursive_paths=True)
src = ''
for a in ar.get_assets(f):
    p = str(a.package_name)
    if p.startswith('${ROOT}'):
        continue
    src = p
    break
print('SOURCE=' + src)
if src:
    unreal.EditorAssetLibrary.duplicate_asset(src, '${MESH}')
    print('DUP=%s' % unreal.EditorAssetLibrary.does_asset_exist('${MESH}'))
`);

const ORIGINAL_LEVEL = (setup.match(/ORIGINAL=(\S*)/) || [])[1];
const SOURCE_MESH = (setup.match(/SOURCE=(\S*)/) || [])[1];

if (!SOURCE_MESH || !/DUP=True/.test(setup)) {
  console.log("SKIP: no static mesh in /Game to duplicate for the Nanite test");
  console.log(setup);
  process.exit(0);
}
console.log(`\nscratch mesh from: ${SOURCE_MESH}`);
console.log(`level to restore:  ${ORIGINAL_LEVEL}`);

const madeLevel = await ue5.createLevel(LEVEL);
if (!madeLevel || madeLevel.success === false) {
  console.log(`SKIP: could not create the scratch level ${LEVEL}: ${JSON.stringify(madeLevel)}`);
  process.exit(0);
}

// --- nanite -----------------------------------------------------------------

console.log("\n=== nanite ===");
const before = await step("read Nanite state of the scratch mesh",
  () => render.renderRead({ static_meshes: [MESH] }));
check("the mesh report carries Nanite fields",
  before?.static_meshes?.[0] && typeof before.static_meshes[0].enabled === "boolean",
  JSON.stringify(before?.static_meshes?.[0]));
const wasEnabled = before?.static_meshes?.[0]?.enabled;

const off = await step("turn Nanite off", () => render.naniteConfigure({
  static_meshes: [MESH], enabled: false,
}));
check("the change is reported as applied", off?.applied?.includes("enabled"), JSON.stringify(off?.applied));
check("after says disabled", off?.meshes?.[0]?.after?.enabled === false, JSON.stringify(off?.meshes?.[0]?.after));
check("before is kept for comparison", off?.meshes?.[0]?.before?.enabled === wasEnabled,
  `${off?.meshes?.[0]?.before?.enabled} vs ${wasEnabled}`);

const on = await step("turn Nanite on with a fallback target", () => render.naniteConfigure({
  static_meshes: [MESH],
  enabled: true,
  fallback_target: "percent_triangles",
  fallback_percent_triangles: 0.5,
  shape_preservation: "preserve_area",
}));
check("enabled again", on?.meshes?.[0]?.after?.enabled === true, JSON.stringify(on?.meshes?.[0]?.after));
check("fallback target took", /PERCENT_TRIANGLES/.test(String(on?.meshes?.[0]?.after?.fallback_target)),
  String(on?.meshes?.[0]?.after?.fallback_target));
check("fallback percent took", Math.abs(on?.meshes?.[0]?.after?.fallback_percent_triangles - 0.5) < 1e-6,
  String(on?.meshes?.[0]?.after?.fallback_percent_triangles));
// 5.8 turned the old bPreserveArea bool into a three-way enum; passing a bool
// here throws inside the engine, which is what this pins down.
check("shape_preservation takes the 5.8 enum, not the old bool",
  /PRESERVE_AREA/.test(String(on?.meshes?.[0]?.after?.shape_preservation)),
  String(on?.meshes?.[0]?.after?.shape_preservation));
check("the mesh was saved", on?.meshes?.[0]?.saved === true, String(on?.meshes?.[0]?.saved));

console.log("\n=== nanite validation ===");
const noMesh = await render.naniteConfigure({ static_meshes: [], enabled: true });
check("empty mesh list is refused", noMesh.ok === false && /empty/.test(noMesh.error), noMesh.error);

const noSettings = await render.naniteConfigure({ static_meshes: [MESH] });
check("no settings is refused", noSettings.ok === false && /no settings given/.test(noSettings.error), noSettings.error);

const missing = await render.naniteConfigure({ static_meshes: [`${ROOT}/NoSuchMesh`], enabled: true });
check("a missing mesh is rejected", missing.ok === false && /not found/.test(missing.error), missing.error);

const wrongType = await render.naniteConfigure({ static_meshes: [LEVEL], enabled: true });
check("a non-mesh path says what it actually is",
  wrongType.ok === false && /not a StaticMesh/.test(wrongType.error), wrongType.error);

const badTarget = await render.naniteConfigure({
  static_meshes: [MESH], fallback_target: "sideways",
});
check("unknown fallback_target lists the valid ones",
  badTarget.ok === false && /auto.*percent_triangles.*relative_error/.test(badTarget.error), badTarget.error);

const badShape = await render.naniteConfigure({
  static_meshes: [MESH], shape_preservation: "squish",
});
check("unknown shape_preservation lists the valid ones",
  badShape.ok === false && /none.*preserve_area.*voxelize/.test(badShape.error), badShape.error);

// --- lumen ------------------------------------------------------------------

console.log("\n=== lumen ===");
const lum = await step("configure Lumen on the scratch level", () => render.lumenConfigure({
  global_illumination: "lumen",
  reflections: "lumen",
  lumen_scene_lighting_quality: 2.0,
  lumen_reflection_quality: 1.5,
  lumen_max_reflection_bounces: 3,
}));
check("an unbounded volume was created", lum?.created_volume === true && lum?.unbound === true,
  JSON.stringify({ created: lum?.created_volume, unbound: lum?.unbound }));
check("override flags were set, not just the values",
  lum?.overrides_set?.includes("override_dynamic_global_illumination_method") &&
  lum?.overrides_set?.includes("override_lumen_scene_lighting_quality"),
  JSON.stringify(lum?.overrides_set));
check("GI method reads back as Lumen",
  /LUMEN/.test(String(lum?.settings?.dynamic_global_illumination_method?.value)),
  String(lum?.settings?.dynamic_global_illumination_method?.value));
check("the numeric knob survived the struct copy-back",
  String(lum?.settings?.lumen_scene_lighting_quality?.value).startsWith("2"),
  String(lum?.settings?.lumen_scene_lighting_quality?.value));
check("int-typed bounce count was not turned into a float",
  String(lum?.settings?.lumen_max_reflection_bounces?.value) === "3",
  String(lum?.settings?.lumen_max_reflection_bounces?.value));

const reuse = await step("a second call reuses the same volume", () => render.lumenConfigure({
  ray_lighting_mode: "hit_lighting",
  advanced: { lumen_surface_cache_resolution: 0.75 },
}));
check("no second volume was spawned", reuse?.created_volume === false, String(reuse?.created_volume));
check("ray lighting mode applied",
  /HIT_LIGHTING/.test(String(reuse?.settings?.lumen_ray_lighting_mode?.value)),
  String(reuse?.settings?.lumen_ray_lighting_mode?.value));
check("advanced property applied with its override flag",
  reuse?.overrides_set?.includes("override_lumen_surface_cache_resolution"),
  JSON.stringify(reuse?.overrides_set));

console.log("\n=== lumen validation ===");
const badGi = await render.lumenConfigure({ global_illumination: "raytraced" });
check("unknown global_illumination lists the valid ones",
  badGi.ok === false && /lumen.*none.*plugin|lumen.*plugin.*screen_space|none.*plugin/.test(badGi.error), badGi.error);

const badRefl = await render.lumenConfigure({ reflections: "pathtraced" });
check("unknown reflections is rejected",
  badRefl.ok === false && /reflections must be one of/.test(badRefl.error), badRefl.error);

const nothing = await render.lumenConfigure({});
check("a call that changes nothing is refused",
  nothing.ok === false && /nothing to change/.test(nothing.error), nothing.error);

const badLabel = await render.lumenConfigure({ volume_label: "NoSuchVolume", global_illumination: "lumen" });
check("an unknown volume label lists the real ones",
  badLabel.ok === false && /no PostProcessVolume labelled/.test(badLabel.error), badLabel.error);

const badAdvanced = await render.lumenConfigure({ advanced: { not_a_real_property: 1 } });
check("a bogus advanced property explains the naming",
  badAdvanced.ok === false && /snake_case/.test(badAdvanced.error), badAdvanced.error);

const noCreate = await render.lumenConfigure({
  volume_label: "NGG_GlobalPostProcess", create_if_missing: false, global_illumination: "lumen",
});
check("targeting the created volume by label works", noCreate.ok === true, noCreate.error);

// --- render read ------------------------------------------------------------

console.log("\n=== render read ===");
const rd = await step("read the level's rendering state", () => render.renderRead({}));
check("the scratch level is the one reported", /L_Render/.test(String(rd?.level)), String(rd?.level));
check("the post process volume shows up",
  rd?.post_process_volumes?.some(v => v.label === "NGG_GlobalPostProcess" && v.unbound),
  JSON.stringify(rd?.post_process_volumes?.map(v => v.label)));
check("only overridden settings are listed as overridden",
  rd?.post_process_volumes?.find(v => v.label === "NGG_GlobalPostProcess")
     ?.overridden?.dynamic_global_illumination_method !== undefined,
  JSON.stringify(rd?.post_process_volumes?.find(v => v.label === "NGG_GlobalPostProcess")?.overridden));
check("console variables were read",
  typeof rd?.console_variables?.["r.Substrate"] === "number",
  JSON.stringify(rd?.console_variables));
check("the project renderer ini section was found",
  rd?.project_renderer_settings && typeof rd.project_renderer_settings === "object",
  JSON.stringify(rd?.project_renderer_settings)?.slice(0, 200));

const rdBadMesh = await render.renderRead({ static_meshes: [`${ROOT}/Nope`] });
check("reading a missing mesh is rejected", rdBadMesh.ok === false && /not found/.test(rdBadMesh.error), rdBadMesh.error);

// --- project settings -------------------------------------------------------

console.log("\n=== project renderer settings ===");
const wrote = await step("add a key to DefaultEngine.ini", () => render.renderProjectSettings({
  settings: { [TEST_KEY]: 1 },
}));
check("the key was added", wrote?.actions?.[0]?.action === "added", JSON.stringify(wrote?.actions));
check("the section is echoed back with the key", wrote?.section_now?.[TEST_KEY] === "1",
  JSON.stringify(wrote?.section_now?.[TEST_KEY]));
check("the live-apply attempt is reported", Array.isArray(wrote?.applied_live) && wrote.applied_live.length === 1,
  JSON.stringify(wrote?.applied_live));
check("the answer says the running editor will not pick it up", /takes effect on the next editor start/.test(String(wrote?.note)),
  String(wrote?.note)?.slice(0, 120));

const rewrote = await step("change the same key", () => render.renderProjectSettings({
  settings: { [TEST_KEY]: 2 }, apply_live: false,
}));
check("a second write reports 'changed' with the old value",
  rewrote?.actions?.[0]?.action === "changed" && rewrote?.actions?.[0]?.from === "1",
  JSON.stringify(rewrote?.actions));

const readBack = await render.renderRead({});
check("the new key shows up in ue5_render_read",
  readBack.data?.project_renderer_settings?.[TEST_KEY] === "2",
  JSON.stringify(readBack.data?.project_renderer_settings?.[TEST_KEY]));

const removed = await step("remove the key again", () => render.renderProjectSettings({
  settings: { [TEST_KEY]: null }, apply_live: false,
}));
check("removal is reported", removed?.actions?.[0]?.action === "removed", JSON.stringify(removed?.actions));
check("the key is gone from the section", removed?.section_now?.[TEST_KEY] === undefined,
  JSON.stringify(removed?.section_now?.[TEST_KEY]));

const removeAgain = await render.renderProjectSettings({
  settings: { [TEST_KEY]: null }, apply_live: false,
});
check("removing a key that is not there says 'absent', not 'removed'",
  removeAgain.data?.actions?.[0]?.action === "absent", JSON.stringify(removeAgain.data?.actions));

console.log("\n=== project settings validation ===");
const emptySettings = await render.renderProjectSettings({ settings: {} });
check("empty settings is refused", emptySettings.ok === false && /empty/.test(emptySettings.error), emptySettings.error);

const badKey = await render.renderProjectSettings({ settings: { "r.Foo]\n[Evil": 1 } });
check("a key that would forge an ini section is refused",
  badKey.ok === false && /not a valid renderer setting name/.test(badKey.error), badKey.error);

const badValue = await render.renderProjectSettings({ settings: { "r.Foo": "1\nBar=2" } });
check("a value containing a newline is refused",
  badValue.ok === false && /line break or bracket/.test(badValue.error), badValue.error);

// --- teardown ---------------------------------------------------------------

console.log("\n=== teardown ===");
if (ORIGINAL_LEVEL) await ue5.openLevel(ORIGINAL_LEVEL);

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
import os
cfg = os.path.join(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_config_dir()), 'DefaultEngine.ini')
with open(cfg, 'r', encoding='utf-8-sig', errors='replace') as f:
    print('INI_CLEAN=%s' % ('${TEST_KEY}' not in f.read()))
`);
check("scratch assets removed", /DIR_EXISTS=False/.test(cleanup), cleanup.trim().slice(-300));
check("no test key left in DefaultEngine.ini", /INI_CLEAN=True/.test(cleanup), cleanup.trim().slice(-300));

console.log(`\n━━━ rendering: ${pass} passed, ${fail} failed ━━━`);
process.exit(fail ? 1 : 0);
