#!/usr/bin/env node
// Copyright 2025-2026 NGG. All Rights Reserved.
// gamedev.live.test.mjs — live verification of the gamedev.js tools and the
// newly exposed C++ routes, against a running editor. Scratch assets go under
// /Game/__NGGGamedevTest__ and are deleted at the end (pass --keep to keep).
//
// Run: node gamedev.live.test.mjs

process.env.NGG_BRIDGE_TIMEOUT_MS = process.env.NGG_BRIDGE_TIMEOUT_MS || "60000";

import fs from "node:fs";
import * as ue5 from "./ue5client.js";
import * as gamedev from "./gamedev.js";

const ROOT = "/Game/__NGGGamedevTest__";
const KEEP = process.argv.includes("--keep");

let pass = 0, fail = 0;
const failures = [];

function header(name) {
  process.stdout.write(`\n━━━ ${name} ${"━".repeat(Math.max(2, 60 - name.length))}\n`);
}
async function check(name, fn) {
  try {
    await fn();
    process.stdout.write(`  PASS  ${name}\n`);
    pass++;
  } catch (err) {
    process.stdout.write(`  FAIL  ${name}\n        ${err.message}\n`);
    failures.push({ name, error: err.message });
    fail++;
  }
}
function assert(cond, msg) { if (!cond) throw new Error(msg); }
function assertOk(r, msg) { assert(r && r.ok, `${msg}: ${r && r.error}`); return r.data; }

// Safe deletion via Python (same approach as the other live suites).
async function safeDelete(paths) {
  const list = JSON.stringify(paths);
  await ue5.execPython(
    `import unreal\n` +
    `for p in ${list}:\n` +
    `    if unreal.EditorAssetLibrary.does_asset_exist(p):\n` +
    `        unreal.EditorAssetLibrary.delete_asset(p)\n`,
    "execute_file"
  );
}

// ---------------------------------------------------------------------------
header("Bridge");
await check("editor reachable", async () => {
  const h = await ue5.healthCheck();
  assert(h.status === "ok", "health not ok");
});

// Pre-clean leftovers from any earlier aborted run. CRITICAL: calling
// /editor/create_blueprint on an asset path that already exists deadlocks the
// editor's game thread in FlushAsyncLoading (existing-asset load path), so the
// suite must always start from an empty scratch folder.
await check("pre-clean scratch folder", async () => {
  await ue5.execPython(
    `import unreal\n` +
    `eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)\n` +
    `for a in list(eas.get_all_level_actors()):\n` +
    `    if a.get_actor_label() in ("NGG_ShotTarget", "NGG_TestCam"):\n` +
    `        eas.destroy_actor(a)\n` +
    `if unreal.EditorAssetLibrary.does_directory_exist("${ROOT}"):\n` +
    `    unreal.EditorAssetLibrary.delete_directory("${ROOT}")\n`,
    "execute_file"
  );
});

// ---------------------------------------------------------------------------
header("Newly exposed C++ routes");

await check("GET /project_info returns project + engine paths", async () => {
  const info = await ue5.getProjectInfo();
  assert(info.project_name && info.uproject_path && info.engine_dir, "missing fields");
});

await check("create BP + duplicate via /assets/duplicate", async () => {
  await ue5.createBlueprint("Actor", `${ROOT}/BP_DupSrc`);
  const r = await ue5.duplicateAsset(`${ROOT}/BP_DupSrc`, `${ROOT}/BP_DupCopy`);
  assert(r.success, "duplicate did not report success");
});

await check("/assets/set_map_entries — endpoint wired (structured error on non-map)", async () => {
  try {
    await ue5.setAssetMapEntries(`${ROOT}/BP_DupSrc`, "NoSuchMapProperty", [{ key: "A", value: "B" }]);
    throw new Error("expected a 4xx for a missing property");
  } catch (err) {
    assert(/UE5 bridge error 4\d\d/.test(err.message), `unexpected error shape: ${err.message}`);
  }
});

await check("/editor/reimport — endpoint wired (404 on missing asset, never a modal)", async () => {
  // NEVER call reimport on an asset without an import source here — that
  // opens a modal file dialog and freezes the editor (learned the hard way).
  // A nonexistent path 404s before FReimportManager engages: proves wiring.
  try {
    await ue5.reimportAsset(`${ROOT}/DoesNotExist_XYZ`);
    throw new Error("expected a 404 for a missing asset");
  } catch (err) {
    assert(/UE5 bridge error 4\d\d/.test(err.message), `unexpected error shape: ${err.message}`);
  }
});

await check("reimport guard flags assets with no import source", async () => {
  const g = await gamedev.reimportSourceCheck({ asset_path: `${ROOT}/BP_DupSrc` });
  assert(g.ok === false && /no import source/.test(g.error), `guard did not trip: ${JSON.stringify(g)}`);
});

await check("bp_add_interface + implement_interface_function + refresh_all_nodes", async () => {
  await ue5.createBlueprintInterface({
    asset_path: `${ROOT}/BPI_Scoring`,
    functions: [{ name: "GetScore", outputs: [{ name: "Score", type: "int" }] }],
    save: true,
  });
  const add = await ue5.bpAddInterface({ blueprint: `${ROOT}/BP_DupSrc`, interface: `${ROOT}/BPI_Scoring` });
  assert(add.success, "add_interface failed");
  const impl = await ue5.bpImplementInterfaceFunction({ blueprint: `${ROOT}/BP_DupSrc`, function: "GetScore" });
  assert(impl.success, "implement_interface_function failed");
  const refresh = await ue5.bpRefreshAllNodes({ blueprint: `${ROOT}/BP_DupSrc`, compile: true });
  assert(refresh.success && refresh.compiled_clean, `refresh not clean: ${JSON.stringify(refresh)}`);
});

// ---------------------------------------------------------------------------
header("Viewport camera + screenshot");

await check("set explicit camera location/rotation", async () => {
  assertOk(await gamedev.setViewportCamera({ location: [0, 0, 500], rotation: [-30, 0, 0] }), "setViewportCamera");
});

let shotActor = null;
await check("spawn a cube and frame it with focus_actor", async () => {
  await ue5.spawnActorInLevel("StaticMeshActor", [0, 0, 100], [0, 0, 0], "NGG_ShotTarget", "/Engine/BasicShapes/Cube");
  shotActor = "NGG_ShotTarget";
  const d = assertOk(await gamedev.setViewportCamera({ focus_actor: "NGG_ShotTarget" }), "focus_actor");
  assert(d.framed === "NGG_ShotTarget", "did not frame the actor");
});

await check("viewport screenshot lands on disk", async () => {
  const d = assertOk(await gamedev.viewportScreenshot({ filename: "ngg_live_test", resolution_x: 1280, resolution_y: 720 }), "screenshot");
  assert(fs.existsSync(d.screenshot), `file missing: ${d.screenshot}`);
  assert(d.size_bytes > 10000, `file suspiciously small: ${d.size_bytes}`);
  fs.rmSync(d.screenshot, { force: true });
});

// ---------------------------------------------------------------------------
header("Editor log");

await check("get_log returns recent lines from the live log", async () => {
  const d = assertOk(await gamedev.getLog({ lines: 20 }), "getLog");
  assert(d.lines.length > 0 && d.lines.length <= 20, "no lines returned");
});

await check("get_log severity filter returns only flagged lines", async () => {
  const d = assertOk(await gamedev.getLog({ severity: "warning", lines: 10 }), "getLog severity");
  for (const l of d.lines) assert(/Warning|Error/.test(l), `unfiltered line: ${l}`);
});

// ---------------------------------------------------------------------------
header("Play-In-Editor");

await check("pie_status reports not running", async () => {
  const d = assertOk(await gamedev.pieStatus(), "pieStatus");
  assert(d.in_pie === false, "PIE unexpectedly already running");
});

await check("pie_start (simulate) → status true → pie_stop → status false", async () => {
  assertOk(await gamedev.pieStart({ simulate: true }), "pieStart");
  await new Promise((r) => setTimeout(r, 2500));
  const mid = assertOk(await gamedev.pieStatus(), "pieStatus mid");
  assert(mid.in_pie === true, "PIE did not start");
  assertOk(await gamedev.pieStop(), "pieStop");
  await new Promise((r) => setTimeout(r, 2500));
  const end = assertOk(await gamedev.pieStatus(), "pieStatus end");
  assert(end.in_pie === false, "PIE did not stop");
});

// ---------------------------------------------------------------------------
header("DataTables");

await check("create struct + datatable_create with rows", async () => {
  await ue5.createBlueprintStruct({
    asset_path: `${ROOT}/S_ItemRow`,
    fields: [
      { name: "DisplayName", type: "string" },
      { name: "Price", type: "int" },
    ],
    save: true,
  });
  const d = assertOk(await gamedev.datatableCreate({
    asset_path: `${ROOT}/DT_Items`,
    row_struct: `${ROOT}/S_ItemRow`,
    rows: [
      { Name: "Sword", DisplayName: "Iron Sword", Price: 120 },
      { Name: "Shield", DisplayName: "Oak Shield", Price: 80 },
    ],
    overwrite: true,
  }), "datatableCreate");
  assert(d.rows === 2, `expected 2 rows, got ${d.rows}`);
});

await check("datatable_read round-trips the rows", async () => {
  const d = assertOk(await gamedev.datatableRead({ asset_path: `${ROOT}/DT_Items` }), "datatableRead");
  assert(d.row_names.length === 2, `expected 2 row names, got ${d.row_names.length}`);
  assert(JSON.stringify(d.rows).includes("Iron Sword"), "row data missing");
});

await check("datatable_set_rows replaces rows", async () => {
  assertOk(await gamedev.datatableSetRows({
    asset_path: `${ROOT}/DT_Items`,
    rows: [{ Name: "Potion", DisplayName: "Small Potion", Price: 10 }],
  }), "datatableSetRows");
  const d = assertOk(await gamedev.datatableRead({ asset_path: `${ROOT}/DT_Items` }), "re-read");
  assert(d.row_names.length === 1 && d.row_names[0] === "Potion", `rows not replaced: ${d.row_names}`);
});

// ---------------------------------------------------------------------------
header("Material graph authoring");

await check("build a scalar-driven emissive material node graph", async () => {
  await ue5.execPython(
    `import unreal\n` +
    `tools = unreal.AssetToolsHelpers.get_asset_tools()\n` +
    `if not unreal.EditorAssetLibrary.does_asset_exist("${ROOT}/M_GraphTest"):\n` +
    `    tools.create_asset("M_GraphTest", "${ROOT}", unreal.Material, unreal.MaterialFactoryNew())\n`,
    "execute_file"
  );
  const p1 = assertOk(await gamedev.materialAddExpression({
    material_path: `${ROOT}/M_GraphTest`,
    expression_class: "VectorParameter",
    node_x: -400, node_y: 0,
    properties: { parameter_name: "GlowColor", default_value: [0.0, 1.0, 0.5, 1.0] },
  }), "add VectorParameter");
  const p2 = assertOk(await gamedev.materialAddExpression({
    material_path: `${ROOT}/M_GraphTest`,
    expression_class: "Multiply",
    node_x: -200, node_y: 0,
  }), "add Multiply");
  assertOk(await gamedev.materialConnect({
    material_path: `${ROOT}/M_GraphTest`,
    from_node: p1.node_id, to_node: p2.node_id, to_input: "A",
  }), "connect param → multiply.A");
  assertOk(await gamedev.materialConnect({
    material_path: `${ROOT}/M_GraphTest`,
    from_node: p2.node_id, material_property: "EMISSIVE_COLOR",
  }), "connect multiply → EmissiveColor");
});

// ---------------------------------------------------------------------------
header("Sequencer");

await check("sequencer_create makes a LevelSequence with rate + range", async () => {
  const d = assertOk(await gamedev.sequencerCreate({
    asset_path: `${ROOT}/LS_Test`, frame_rate: 24, length_seconds: 3, overwrite: true,
  }), "sequencerCreate");
  assert(d.frame_rate === 24 && d.playback_end === 72, `bad range: ${JSON.stringify(d)}`);
});

await check("sequencer_bind_actor adds binding + transform keys", async () => {
  const d = assertOk(await gamedev.sequencerBindActor({
    sequence_path: `${ROOT}/LS_Test`,
    actor_label: "NGG_ShotTarget",
    transform_keys: [
      { time: 0, location: [0, 0, 100] },
      { time: 2, location: [0, 300, 100], rotation: [0, 90, 0] },
    ],
  }), "sequencerBindActor");
  assert(d.keyframes === 2, `expected 2 keys, got ${d.keyframes}`);
});

await check("sequencer_add_camera spawns cam + camera-cut track", async () => {
  const d = assertOk(await gamedev.sequencerAddCamera({
    sequence_path: `${ROOT}/LS_Test`,
    camera_label: "NGG_TestCam",
    location: [400, 0, 250],
    look_at_actor: "NGG_ShotTarget",
  }), "sequencerAddCamera");
  assert(d.camera === "NGG_TestCam", "camera label mismatch");
});

// ---------------------------------------------------------------------------
header("Audio");

await check("create_sound_cue from a generated SoundWave", async () => {
  // Generate a tiny procedural SoundWave in-place so the test needs no .wav fixture.
  await ue5.execPython(
    `import unreal\n` +
    `tools = unreal.AssetToolsHelpers.get_asset_tools()\n` +
    `if not unreal.EditorAssetLibrary.does_asset_exist("${ROOT}/SW_Blip"):\n` +
    `    w = tools.create_asset("SW_Blip", "${ROOT}", unreal.SoundWave, None)\n` +
    `    unreal.EditorAssetLibrary.save_asset("${ROOT}/SW_Blip", only_if_is_dirty=False)\n`,
    "execute_file"
  );
  const d = assertOk(await gamedev.createSoundCue({
    asset_path: `${ROOT}/SC_Blip`,
    sound_wave: `${ROOT}/SW_Blip`,
    volume: 0.8,
    overwrite: true,
  }), "createSoundCue");
  assert(d.asset_path === `${ROOT}/SC_Blip`, "path mismatch");
});

// ---------------------------------------------------------------------------
header("Audio round 2");

await check("create_sound_attenuation with falloff", async () => {
  const d = assertOk(await gamedev.createSoundAttenuation({
    asset_path: `${ROOT}/ATT_Test`, falloff_distance: 2500, shape: "Sphere", overwrite: true,
  }), "createSoundAttenuation");
  assert(d.asset_path === `${ROOT}/ATT_Test`, "path mismatch");
});

await check("spawn_ambient_sound with attenuation", async () => {
  const d = assertOk(await gamedev.spawnAmbientSound({
    sound: `${ROOT}/SC_Blip`, location: [0, 200, 100], actor_label: "NGG_Ambient",
    volume: 0.7, attenuation: `${ROOT}/ATT_Test`,
  }), "spawnAmbientSound");
  assert(d.actor === "NGG_Ambient", "label mismatch");
});

await check("play_sound_preview", async () => {
  assertOk(await gamedev.playSoundPreview({ sound: `${ROOT}/SC_Blip`, volume: 0.4 }), "playSoundPreview");
});

// ---------------------------------------------------------------------------
header("UMG round 2");

await check("create WBP + widgets for round-2 tests", async () => {
  await ue5.createWidgetBlueprint("UserWidget", `${ROOT}/WBP_R2`);
  await ue5.addWidgetToBlueprint(`${ROOT}/WBP_R2`, [
    { type: "Button", name: "StartButton" },
    { type: "TextBlock", name: "TitleText" },
  ]);
});

await check("set_widget_properties sets text + opacity on TextBlock", async () => {
  const d = assertOk(await gamedev.setWidgetProperties({
    widget_blueprint: `${ROOT}/WBP_R2`,
    widget_name: "TitleText",
    properties: { text: "Hello NGG", render_opacity: 0.5 },
  }), "setWidgetProperties");
  assert(d.applied_properties.includes("text") && d.applied_properties.includes("render_opacity"),
    `not applied: ${JSON.stringify(d)}`);
  await ue5.compileWidgetBlueprint(`${ROOT}/WBP_R2`);
});

await check("widget_bind_event binds Button OnClicked (mark var + bound event node)", async () => {
  await ue5.styleWidgets(`${ROOT}/WBP_R2`, [{ widget_name: "StartButton", is_variable: true }]);
  await ue5.compileWidgetBlueprint(`${ROOT}/WBP_R2`);
  const result = await ue5.bpAddLogic({
    blueprint: `${ROOT}/WBP_R2`,
    nodes: [{ id: "evt_click", type: "component_event", component: "StartButton", delegate: "OnClicked" }],
    connections: [],
    compile: true,
  });
  assert(result && !result.error, `bpAddLogic failed: ${JSON.stringify(result)}`);
});

// ---------------------------------------------------------------------------
header("GAS round 2");

await check("gas_add_attribute appends to an AttributeSet BP", async () => {
  await ue5.gasCreateAttributeSet({
    asset_path: `${ROOT}/AS_R2`,
    attributes: [{ name: "Health", default_value: 100 }],
    save: false,
  });
  const r = await ue5.bpCreateVariable({
    blueprint: `${ROOT}/AS_R2`, name: "Stamina",
    type: "struct:GameplayAttributeData", compile: true, save: false,
  });
  assert(r && !r.error, `create variable failed: ${JSON.stringify(r)}`);
});

await check("gas_list_attributes reflects both attributes", async () => {
  const d = assertOk(await gamedev.gasListAttributes({ asset_path: `${ROOT}/AS_R2` }), "gasListAttributes");
  const names = d.attributes.map((a) => a.name);
  assert(names.includes("health") || names.includes("Health"), `Health missing: ${names}`);
  assert(names.includes("stamina") || names.includes("Stamina"), `Stamina missing: ${names}`);
});

await check("gas_grant_on_beginplay wires GiveAbility + ApplyEffect", async () => {
  await ue5.gasSetupActor({ blueprint_path: `${ROOT}/BP_DupSrc`, save: false });
  await ue5.gasCreateAbility({ asset_path: `${ROOT}/GA_R2`, save: false });
  await ue5.gasCreateEffect({ asset_path: `${ROOT}/GE_R2`, duration_policy: "Instant", save: false });
  const setup = await ue5.gasReadSetup({ blueprint_path: `${ROOT}/BP_DupSrc` });
  const ascName = setup?.ability_system_component?.component_name ?? setup?.asc?.component_name ?? setup?.component_name;
  assert(ascName, `no ASC name in read_setup: ${JSON.stringify(setup)}`);
  const asClass = (p) => `${p}.${p.split("/").pop()}_C`;
  const result = await ue5.bpAddLogic({
    blueprint: `${ROOT}/BP_DupSrc`,
    nodes: [
      { id: "ngg_gas_begin", type: "event", event: "ReceiveBeginPlay" },
      { id: "ngg_gas_asc", type: "variable_get", variable: ascName },
      { id: "ngg_gas_give_0", type: "call_function", function: "K2_GiveAbility",
        class: "/Script/GameplayAbilities.AbilitySystemComponent",
        defaults: { AbilityClass: asClass(`${ROOT}/GA_R2`) } },
      { id: "ngg_gas_apply_0", type: "call_function", function: "BP_ApplyGameplayEffectToSelf",
        class: "/Script/GameplayAbilities.AbilitySystemComponent",
        defaults: { GameplayEffectClass: asClass(`${ROOT}/GE_R2`), Level: "1" } },
    ],
    connections: [
      { from: "ngg_gas_begin.then", to: "ngg_gas_give_0.execute" },
      { from: `ngg_gas_asc.${ascName}`, to: "ngg_gas_give_0.target" },
      { from: "ngg_gas_give_0.then", to: "ngg_gas_apply_0.execute" },
      { from: `ngg_gas_asc.${ascName}`, to: "ngg_gas_apply_0.target" },
    ],
    auto_layout: true,
    compile: true,
  });
  assert(result && !result.error, `bpAddLogic failed: ${JSON.stringify(result)}`);
});

// ---------------------------------------------------------------------------
header("Gameplay tags");

await check("add_gameplay_tags writes ini and skips duplicates on re-run", async () => {
  const tag = "NGGTest.Live.Tag";
  const first = await gamedev.addGameplayTags({ tags: [{ tag, comment: "live test tag" }] });
  assert(first.ok, `first add failed: ${first.error}`);
  const second = await gamedev.addGameplayTags({ tags: [{ tag }] });
  assert(second.ok && second.data.skipped_existing.includes(tag), "duplicate not skipped");
  // Clean the test tag back out of the ini.
  const ini = first.data.ini;
  const text = fs.readFileSync(ini, "utf8")
    .split(/\r?\n/).filter((l) => !l.includes(`Tag="${tag}"`)).join("\n");
  fs.writeFileSync(ini, text, "utf8");
});

// ---------------------------------------------------------------------------
header("Level-actor components");

await check("add_component_to_actor attaches a PointLightComponent", async () => {
  const d = assertOk(await gamedev.addComponentToActor({
    actor_label: "NGG_ShotTarget",
    component_class: "PointLightComponent",
    component_label: "NGG_TestLight",
    properties: { intensity: 5000 },
  }), "addComponentToActor");
  assert(d.applied_properties.includes("intensity"), "property not applied");
});

// ---------------------------------------------------------------------------
header("Cleanup");

if (!KEEP) {
  await check("destroy test actors", async () => {
    await ue5.execPython(
      `import unreal\n` +
      `eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)\n` +
      `for a in list(eas.get_all_level_actors()):\n` +
      `    if a.get_actor_label() in ("NGG_ShotTarget", "NGG_TestCam", "NGG_Ambient"):\n` +
      `        eas.destroy_actor(a)\n`,
      "execute_file"
    );
  });
  await check("delete scratch assets", async () => {
    await safeDelete([
      `${ROOT}/BP_DupSrc`, `${ROOT}/BP_DupCopy`, `${ROOT}/BPI_Scoring`,
      `${ROOT}/S_ItemRow`, `${ROOT}/DT_Items`, `${ROOT}/M_GraphTest`,
      `${ROOT}/LS_Test`, `${ROOT}/SW_Blip`, `${ROOT}/SC_Blip`,
      `${ROOT}/ATT_Test`, `${ROOT}/WBP_R2`, `${ROOT}/AS_R2`,
      `${ROOT}/GA_R2`, `${ROOT}/GE_R2`,
    ]);
  });
  await check("save after cleanup", async () => { await ue5.saveAll(); });
}

process.stdout.write(`\n━━━ Summary: ${pass}/${pass + fail} passed, ${fail} failed ━━━\n`);
if (failures.length) {
  for (const f of failures) process.stdout.write(`  FAILED: ${f.name}\n`);
}
process.exit(fail ? 1 : 0);
