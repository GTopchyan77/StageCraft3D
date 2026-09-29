#!/usr/bin/env node
// Copyright 2025-2026 NGG. All Rights Reserved.
// landscape.live.test.mjs — exercise of the landscape tools against a RUNNING
// UE5 editor.
//
// A landscape cannot be created from script in UE 5.8 (see landscape.js), so
// this suite runs in two halves:
//
//   * the parts that need no landscape — the empty-level report, and every
//     validation path — always run;
//   * the setup happy path runs only if the open level already has a landscape,
//     and says loudly when it is skipped rather than reporting a pass.
//
// To cover the second half: open a level, create a landscape in Landscape mode,
// and run this again.
//
// Run:  node landscape.live.test.mjs

import * as land from "./landscape.js";
import * as ue5 from "./ue5client.js";

const ROOT = "/Game/__NGGLandscapeTest__";

let pass = 0, fail = 0, skipped = 0;

function check(name, cond, detail) {
  if (cond) { pass++; console.log(`  PASS  ${name}`); }
  else { fail++; console.log(`  FAIL  ${name}${detail ? "\n        " + detail : ""}`); }
}

function skip(name, why) {
  skipped++;
  console.log(`  SKIP  ${name}\n        ${why}`);
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

// --- read -------------------------------------------------------------------

console.log("\n=== read ===");
const read = await step("report the level's landscapes", () => land.landscapeRead({}));
check("the open level is named", typeof read?.level === "string" && read.level.length > 0, String(read?.level));
check("counts are reported separately for parents and streaming proxies",
  typeof read?.landscape_count === "number" && typeof read?.streaming_proxy_count === "number",
  JSON.stringify({ p: read?.landscape_count, s: read?.streaming_proxy_count }));

const HAS_LANDSCAPE = (read?.landscape_count ?? 0) > 0;

if (!HAS_LANDSCAPE) {
  check("with no landscape, the answer says so and why script cannot make one",
    /cannot create one from script/.test(String(read?.note)) &&
    /Landscape mode/.test(String(read?.note)), String(read?.note));
  check("and the list is empty rather than absent",
    Array.isArray(read?.landscapes) && read.landscapes.length === 0, JSON.stringify(read?.landscapes));
} else {
  check("each landscape reports its material slot",
    read.landscapes.every(l => "landscape_material" in l), JSON.stringify(read.landscapes[0]));
  check("target layers are listed", Array.isArray(read.landscapes[0].target_layers),
    JSON.stringify(read.landscapes[0].target_layers));
}

const readProxies = await step("read with streaming proxies included",
  () => land.landscapeRead({ include_proxies: true }));
check("the proxy list is present when asked for",
  Array.isArray(readProxies?.streaming_proxies), JSON.stringify(readProxies?.streaming_proxies));

// --- validation -------------------------------------------------------------

console.log("\n=== validation ===");
const nothing = await land.landscapeSetup({});
check("a call that changes nothing is refused",
  nothing.ok === false && /nothing to do/.test(nothing.error), nothing.error);

if (!HAS_LANDSCAPE) {
  const noLandscape = await land.landscapeSetup({ material: "/Engine/BasicShapes/BasicShapeMaterial" });
  check("setup on a level with no landscape explains the manual step",
    noLandscape.ok === false && /Landscape mode/.test(noLandscape.error), noLandscape.error);
}

const badLabel = await land.landscapeSetup({
  landscape_label: "NoSuchLandscape", material: "/Engine/BasicShapes/BasicShapeMaterial",
});
check("an unknown landscape label is rejected",
  badLabel.ok === false && (/no landscape labelled/.test(badLabel.error) || /Landscape mode/.test(badLabel.error)),
  badLabel.error);

if (HAS_LANDSCAPE) {
  const badMat = await land.landscapeSetup({ material: `${ROOT}/NoSuchMaterial` });
  check("a missing material is rejected", badMat.ok === false && /not found/.test(badMat.error), badMat.error);

  const wrongMat = await land.landscapeSetup({ material: "/Engine/BasicShapes/Cube" });
  check("a non-material path says what it actually is",
    wrongMat.ok === false && /not a Material/.test(wrongMat.error), wrongMat.error);

  const noName = await land.landscapeSetup({ layers: [{ hardness: 0.5 }] });
  check("a layer with no name is rejected",
    noName.ok === false && /needs a 'name'/.test(noName.error), noName.error);

  const badColor = await land.landscapeSetup({ layers: [{ name: "Grass", debug_color: [1] }] });
  check("a malformed debug_color is rejected",
    badColor.ok === false && /\[r, g, b\]/.test(badColor.error), badColor.error);

  const strays = await py(`
lib = unreal.EditorAssetLibrary
d = '${ROOT}'
print('STRAY=%s' % (len(lib.list_assets(d, recursive=True)) if lib.does_directory_exist(d) else 0))
`);
  check("no LayerInfo assets were created by the rejected calls", /STRAY=0/.test(strays), strays.trim());

  console.log("\n=== setup ===");
  const setup = await step("bind paint layers to the landscape", () => land.landscapeSetup({
    layers: [
      { name: "NGGTestGrass", layer_info_path: `${ROOT}/LI_NGGTestGrass`, hardness: 0.4, debug_color: [0, 1, 0] },
      { name: "NGGTestRock", layer_info_path: `${ROOT}/LI_NGGTestRock` },
    ],
    save: false,
  }));
  check("both layers were applied", setup?.layers_applied?.length === 2, JSON.stringify(setup?.layers_applied));
  check("the LayerInfo assets were created",
    setup?.layers_applied?.every(l => l.layer_info_created === true), JSON.stringify(setup?.layers_applied));
  check("the target layer map on the landscape now holds them",
    setup?.state?.target_layers?.some(l => l.name === "NGGTestGrass" && l.layer_info),
    JSON.stringify(setup?.state?.target_layers));

  const again = await step("re-running reuses the existing LayerInfo assets", () => land.landscapeSetup({
    layers: [{ name: "NGGTestGrass", layer_info_path: `${ROOT}/LI_NGGTestGrass` }],
    save: false,
  }));
  check("the second run did not create a new asset",
    again?.layers_applied?.[0]?.layer_info_created === false, JSON.stringify(again?.layers_applied));

  console.log("\n=== teardown ===");
  const cleanup = await py(`
lib = unreal.EditorAssetLibrary
eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
for a in eas.get_all_level_actors():
    if isinstance(a, unreal.Landscape):
        tl = a.get_editor_property('target_layers')
        for n in ['NGGTestGrass', 'NGGTestRock']:
            if n in tl:
                del tl[n]
        a.set_editor_property('target_layers', tl)
if lib.does_directory_exist('${ROOT}'):
    for p in lib.list_assets('${ROOT}', recursive=True):
        try:
            lib.delete_asset(p.split('.')[0])
        except Exception:
            pass
    lib.delete_directory('${ROOT}')
print('DIR_EXISTS=%s' % lib.does_directory_exist('${ROOT}'))
`);
  check("scratch LayerInfo assets removed", /DIR_EXISTS=False/.test(cleanup), cleanup.trim().slice(-300));
} else {
  skip("setup happy path",
    "the open level has no landscape. UE 5.8 cannot create one from script, so this half " +
    "cannot run here. Create a landscape in Landscape mode and run this suite again.");
}

console.log(`\n━━━ landscape: ${pass} passed, ${fail} failed, ${skipped} skipped ━━━`);
process.exit(fail ? 1 : 0);
