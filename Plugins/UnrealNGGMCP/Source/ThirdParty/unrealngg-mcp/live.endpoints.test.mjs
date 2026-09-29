#!/usr/bin/env node
// Copyright 2025-2026 NGG. All Rights Reserved.
// live.endpoints.test.mjs — exhaustive end-to-end test against a LIVE Unreal
// Editor. Whereas live.test.mjs is a fast smoke test, this file aims to hit
// EVERY route registered in NGGHttpServer.cpp::RegisterRoutes() at least once,
// proving the C++ plugin handles each endpoint without crashing or 5xx-ing.
//
// Pass = handler returned a non-5xx HTTP response (2xx, or a structured 4xx
// the handler chose to emit — both prove the route is wired and the parser
// works). Fail = network error, 5xx, or unexpected payload shape.
//
// Prerequisites:
//   1. Unreal Editor is open with this project
//   2. UnrealNGGMCP plugin is enabled
//   3. Output Log shows "UnrealNGGMCP: HTTP bridge listening on http://localhost:6776"
//
// Run:
//   node live.endpoints.test.mjs            (scratch under /Game/__NGGEndpointTest__)
//   node live.endpoints.test.mjs --keep     (don't delete scratch assets)
//
// Exit code 0 on success, 1 on any failure.
//
// DESTRUCTIVE endpoints intentionally skipped (would kill the editor / make
// the suite non-re-runnable): /editor/shutdown, /editor/build_and_run,
// /editor/kill_and_restart. They are listed in the summary as SKIP.

process.env.NGG_BRIDGE_TIMEOUT_MS = process.env.NGG_BRIDGE_TIMEOUT_MS || "60000";

import * as ue5 from "./ue5client.js";

const SCRATCH_ROOT = "/Game/__NGGEndpointTest__";
const KEEP         = process.argv.includes("--keep");

const ENGINE_SKELETON = "/Engine/EngineMeshes/SkeletalCube_Skeleton";
const ENGINE_CUBE     = "/Engine/BasicShapes/Cube";
const ENGINE_MATERIAL = "/Engine/BasicShapes/BasicShapeMaterial";

let pass = 0;
let fail = 0;
let skip = 0;
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

// Accept either a 2xx response OR a 4xx the server emits via JsonError —
// both prove the route is bound and parses input. Only network / 5xx fails.
async function checkEndpointWired(name, fn) {
  try {
    await fn();
    process.stdout.write(`  PASS  ${name}\n`);
    pass++;
  } catch (err) {
    const msg = err.message;
    // bridgeFetch wraps 4xx into "UE5 bridge error 4xx: ...". Treat 4xx as
    // "endpoint is wired" — the handler ran and rejected our (intentionally
    // skeletal) payload with a structured error.
    if (/UE5 bridge error 4\d\d:/.test(msg)) {
      process.stdout.write(`  PASS  ${name}  (endpoint wired; got 4xx as expected for skeletal input)\n`);
      pass++;
      return;
    }
    process.stdout.write(`  FAIL  ${name}\n        ${msg}\n`);
    failures.push({ name, error: msg });
    fail++;
  }
}

function note(text) {
  process.stdout.write(`  SKIP  ${text}\n`);
  skip++;
}

function assert(cond, msg) {
  if (!cond) throw new Error(msg);
}

// Safe delete via Python — same approach as live.test.mjs to avoid the
// ObjectTools::ForceDeleteObjects selection-update crash.
//
// Extra care for UWorld (level) assets: if the asset being deleted is the
// currently-loaded editor world, ForceDeleteObjects → LevelInstanceSubsystem
// ::OnAssetsPreDelete will crash dereferencing FName entries of the world
// being torn out under it. We open a different scratch level first so the
// target world is no longer the active editor world.
async function safeDeleteAsset(assetPath) {
  const py =
    `import unreal\n` +
    `lib = unreal.EditorAssetLibrary\n` +
    `path = "${assetPath}"\n` +
    `if lib.does_asset_exist(path):\n` +
    `    try:\n` +
    `        loaded = lib.load_asset(path)\n` +
    `        is_world = isinstance(loaded, unreal.World)\n` +
    `    except Exception:\n` +
    `        is_world = False\n` +
    `    if is_world:\n` +
    `        try:\n` +
    `            current = unreal.EditorLevelLibrary.get_editor_world()\n` +
    `            current_pkg = current.get_outermost().get_path_name() if current else ""\n` +
    `            if current_pkg == path:\n` +
    `                # Switch off the target world without writing a new asset.\n` +
    `                try:\n` +
    `                    unreal.EditorLoadingAndSavingUtils.new_blank_map(False)\n` +
    `                except Exception:\n` +
    `                    try:\n` +
    `                        unreal.EditorLevelLibrary.load_level("/Engine/Maps/Templates/Template_Default")\n` +
    `                    except Exception:\n` +
    `                        pass\n` +
    `        except Exception as e:\n` +
    `            unreal.log_warning("safeDeleteAsset world-switch failed: " + str(e))\n` +
    `    try:\n` +
    `        lib.delete_asset(path)\n` +
    `    except Exception as e:\n` +
    `        unreal.log_warning(\"safeDeleteAsset delete failed: \" + str(e))\n`;
  try { await ue5.execPython(py, "execute_file"); } catch { /* best-effort */ }
}

async function clearEditorSelection() {
  const py =
    `import unreal\n` +
    `try:\n` +
    `    unreal.EditorActorSubsystem().set_selected_level_actors([])\n` +
    `except Exception: pass\n`;
  try { await ue5.execPython(py, "execute_file"); } catch { /* best-effort */ }
}

// ===========================================================================
// 0. Bridge connectivity
// ===========================================================================

header("Bridge connectivity");

await check("GET /health returns ok", async () => {
  const h = await ue5.healthCheck();
  assert(h?.status === "ok", `unexpected: ${JSON.stringify(h)}`);
});

if (fail > 0) {
  process.stderr.write(
    "\nBridge not reachable. Open the Unreal Editor with the UnrealNGGMCP " +
    "plugin enabled at http://localhost:6776 and re-run.\n"
  );
  process.exit(1);
}

// Pre-clean stale scratch so the suite is re-runnable
header("Pre-cleanup (idempotent)");
const STALE = [
  `${SCRATCH_ROOT}/BP_Test`,
  `${SCRATCH_ROOT}/BP_Reparent`,
  `${SCRATCH_ROOT}/WBP_Test`,
  `${SCRATCH_ROOT}/S_Test`,
  `${SCRATCH_ROOT}/E_Test`,
  `${SCRATCH_ROOT}/BPI_Test`,
  `${SCRATCH_ROOT}/ABP_Test`,
  `${SCRATCH_ROOT}/M_Test`,
  `${SCRATCH_ROOT}/MI_Test`,
  `${SCRATCH_ROOT}/MI_Tex_Test`,
  `${SCRATCH_ROOT}/Curve_Float_Test`,
  `${SCRATCH_ROOT}/Curve_Color_Test`,
  `${SCRATCH_ROOT}/NS_Test`,
  `${SCRATCH_ROOT}/BT_Test`,
  `${SCRATCH_ROOT}/BB_Test`,
  `${SCRATCH_ROOT}/AS_Test`,
  `${SCRATCH_ROOT}/GA_Test`,
  `${SCRATCH_ROOT}/GE_Test`,
  `${SCRATCH_ROOT}/SM_Test`,
  `${SCRATCH_ROOT}/L_Test`,
];
for (const p of STALE) await safeDeleteAsset(p);
process.stdout.write(`  evicted ${STALE.length} stale paths\n`);

// ===========================================================================
// 1. Infrastructure: /health, /project_info
// ===========================================================================

header("Infrastructure");

await check("GET /project_info returns project metadata", async () => {
  const r = await ue5.getProjectInfo();
  assert(r && typeof r === "object", `unexpected: ${JSON.stringify(r)}`);
});

// ===========================================================================
// 2. Generic asset CRUD
// ===========================================================================

header("Asset CRUD");

await check("GET /assets/list /Game returns array", async () => {
  const r = await ue5.listAssets("/Game");
  assert(Array.isArray(r.assets) || Array.isArray(r), `unexpected shape`);
});

await check("GET /assets/get on engine cube returns object", async () => {
  const r = await ue5.getAsset(ENGINE_CUBE);
  assert(typeof r === "object" && r !== null, `unexpected`);
});

await checkEndpointWired("POST /assets/create — endpoint reachable", async () => {
  // Bare UDataAsset is abstract; expect the handler to refuse with 4xx.
  await ue5.createAsset("DataAsset", `${SCRATCH_ROOT}/__DAProbe`);
});

await checkEndpointWired("POST /assets/set_property — endpoint reachable", async () => {
  // Setting on a non-existent path should yield a structured 4xx — proves the
  // route + JSON body parsing both work.
  await ue5.setAssetProperty(`${SCRATCH_ROOT}/__DoesNotExist`, "X", "1");
});

await checkEndpointWired("POST /assets/set_map_entries — endpoint reachable", async () => {
  // Hit the bridge directly since ue5client has no helper for this one yet.
  const url = `${ue5.getBridgeUrl()}/assets/set_map_entries`;
  const res = await fetch(url, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ asset_path: `${SCRATCH_ROOT}/__NoAsset`, property: "X", entries: [] }),
  });
  if (!res.ok && res.status >= 500) throw new Error(`5xx: ${res.status}`);
});

// ===========================================================================
// 3. Editor utilities
// ===========================================================================

header("Editor utilities");

await check("POST /editor/save_all", async () => { await ue5.saveAll(); });

await checkEndpointWired("POST /editor/reimport — endpoint reachable", async () => {
  await ue5.reimportAsset(`${SCRATCH_ROOT}/__DoesNotExist`);
});

await check("POST /editor/open_level — bogus level returns 4xx, not 5xx", async () => {
  try {
    await ue5.openLevel(`${SCRATCH_ROOT}/__DoesNotExist`);
  } catch (err) {
    assert(/4\d\d|not found|fail|invalid|level/i.test(err.message), `unexpected: ${err.message}`);
  }
});

// ===========================================================================
// 4. Gameplay tags
// ===========================================================================

header("Gameplay tags");

await check("GET /gameplay_tags/list returns array", async () => {
  const r = await ue5.listGameplayTags();
  assert(r && (Array.isArray(r.tags) || Array.isArray(r)), `unexpected shape`);
});

// ===========================================================================
// 5. Blueprint create / reparent / set defaults / add+remove component
// ===========================================================================

header("Blueprint authoring");

const BP_PATH = `${SCRATCH_ROOT}/BP_Test`;
const BP_REPARENT = `${SCRATCH_ROOT}/BP_Reparent`;

await check("POST /editor/create_blueprint inherits AActor", async () => {
  const r = await ue5.createBlueprint("Actor", BP_PATH);
  assert(r?.created || r?.asset_path, `unexpected: ${JSON.stringify(r)}`);
});

await check("POST /editor/create_blueprint (second BP for reparent test)", async () => {
  const r = await ue5.createBlueprint("Actor", BP_REPARENT);
  assert(r?.created || r?.asset_path, `unexpected: ${JSON.stringify(r)}`);
});

await check("POST /editor/set_blueprint_defaults (empty props is a valid no-op)", async () => {
  await ue5.setBlueprintDefaults(BP_PATH, []);
});

await check("POST /editor/reparent_blueprint to Pawn", async () => {
  await ue5.reparentBlueprint(BP_REPARENT, "Pawn");
});

await check("POST /editor/add_component_to_blueprint (StaticMeshComponent)", async () => {
  await ue5.addComponentToBlueprint(BP_PATH, "StaticMeshComponent", "TestMesh", []);
});

await check("POST /editor/add_component_to_blueprint nests Camera under SpringArm via attach_parent", async () => {
  await ue5.addComponentToBlueprint(BP_PATH, "SpringArmComponent", "TestSpringArm", []);
  const res = await ue5.addComponentToBlueprint(
    BP_PATH, "CameraComponent", "TestCamera", [], "TestSpringArm");
  assert(res.attached_to === "TestSpringArm",
    `expected Camera attached under TestSpringArm, got '${res.attached_to}'`);
});

await check("POST /editor/add_component_to_blueprint rejects unknown attach_parent", async () => {
  let threw = false;
  try {
    await ue5.addComponentToBlueprint(BP_PATH, "CameraComponent", "TestCamera2", [], "NoSuchComponent");
  } catch {
    threw = true;
  }
  assert(threw, "expected a 4xx error for an unknown attach_parent");
});

await check("POST /editor/set_component_defaults sets a property on the new component", async () => {
  await ue5.setComponentDefaults(BP_PATH, "TestMesh", [
    { name: "StaticMesh", value: ENGINE_CUBE },
  ]);
});

await check("POST /editor/remove_component_from_blueprint removes the component", async () => {
  await ue5.removeComponentFromBlueprint(BP_PATH, "TestMesh");
});

// ===========================================================================
// 6. Blueprint variables / functions / macros / list / compile / read_graph
// ===========================================================================

header("Blueprint graph authoring");

await check("POST /bp/create_variable on test BP", async () => {
  await ue5.bpCreateVariable({
    blueprint: BP_PATH,
    name: "Speed",
    type: "float",
    default_value: 100,
    save: false,
  });
});

await check("GET /bp/list_variables shows the new variable", async () => {
  const r = await ue5.bpListVariables({ blueprint: BP_PATH });
  const names = (r?.variables ?? []).map((v) => v.name);
  assert(names.includes("Speed"), `Speed missing from ${JSON.stringify(names)}`);
});

await check("POST /bp/create_function", async () => {
  await ue5.bpCreateFunction({ blueprint: BP_PATH, name: "TestFunc", compile: false, save: false });
});

await check("POST /bp/create_macro", async () => {
  await ue5.bpCreateMacro({ blueprint: BP_PATH, name: "TestMacro", compile: false, save: false });
});

await check("POST /bp/compile", async () => {
  await ue5.bpCompile({ blueprint: BP_PATH, save: false });
});

await check("GET /bp/read_graph returns graph structure", async () => {
  const r = await ue5.bpReadGraph({ blueprint: BP_PATH });
  assert(r !== undefined, `no response`);
});

await check("GET /bp/get_selection (no explicit BP) returns object", async () => {
  // No open BP editor → handler returns "no selection" rather than 5xx.
  try {
    const r = await ue5.bpGetSelection({});
    assert(r && typeof r === "object", `unexpected: ${JSON.stringify(r)}`);
  } catch (err) {
    // Tolerate 4xx "no active editor".
    assert(/4\d\d|no.*editor|selection/i.test(err.message), `unexpected: ${err.message}`);
  }
});

// Add and connect a node so /bp/add_node, /bp/connect_pins, /bp/add_logic,
// /bp/delete_node all exercise once.
let printNodeId = null;
await check("POST /bp/add_node (K2Node_CallFunction PrintString)", async () => {
  const r = await ue5.bpAddNode({
    blueprint:  BP_PATH,
    graph:      "EventGraph",
    node_class: "/Script/BlueprintGraph.K2Node_CallFunction",
    node_id:    "PrintHello",
    config:     { function: { name: "PrintString", class: "/Script/Engine.KismetSystemLibrary" } },
  });
  printNodeId = r?.node_id || "PrintHello";
});

await checkEndpointWired("POST /bp/connect_pins — endpoint reachable", async () => {
  // Connecting to nonexistent target pin is acceptable; we just need the route hit.
  await ue5.bpConnectPins({
    blueprint: BP_PATH,
    graph:     "EventGraph",
    from_node: printNodeId ?? "PrintHello", from_pin: "then",
    to_node:   "__nope__",                  to_pin:   "execute",
  });
});

await check("POST /bp/add_logic with empty nodes is a valid no-op", async () => {
  await ue5.bpAddLogic({ blueprint: BP_PATH, graph: "EventGraph", nodes: [], compile: false, save: false });
});

await check("POST /bp/delete_node removes the PrintString we added", async () => {
  await ue5.bpDeleteNode({
    blueprint: BP_PATH,
    graph:     "EventGraph",
    node_id:   "PrintHello",
    compile:   false,
    save:      false,
  });
});

await check("POST /bp/lint runs lint on the test BP", async () => {
  await ue5.bpLint({ blueprint: BP_PATH });
});

await check("POST /bp/lint_project under scratch root completes", async () => {
  await ue5.bpLintProject({ path_prefix: SCRATCH_ROOT });
});

// ===========================================================================
// 7. Asset creation: structs / enums / interfaces / anim BPs
// ===========================================================================

header("Asset creation (struct/enum/interface/animBP)");

await check("POST /asset/create_blueprint_struct", async () => {
  // Hit the helper. Bridge has no helper for createBlueprintStruct yet? It does.
  await ue5.createBlueprintStruct({
    asset_path: `${SCRATCH_ROOT}/S_Test`,
    fields:     [{ name: "X", type: "int" }],
    save:       true,
  });
});

await check("POST /asset/create_blueprint_enum", async () => {
  await ue5.createBlueprintEnum({
    asset_path: `${SCRATCH_ROOT}/E_Test`,
    entries:    ["A", "B", "C"],
    save:       true,
  });
});

await check("POST /asset/create_blueprint_interface", async () => {
  await ue5.createBlueprintInterface({
    asset_path: `${SCRATCH_ROOT}/BPI_Test`,
    functions:  [{ name: "Ping" }],
    save:       true,
  });
});

await check("POST /asset/create_anim_blueprint (engine SkeletalCube skeleton)", async () => {
  await ue5.createAnimBlueprint({
    asset_path:      `${SCRATCH_ROOT}/ABP_Test`,
    target_skeleton: ENGINE_SKELETON,
    save:            true,
  });
});

// ===========================================================================
// 8. Anim graph node splices (need ABP_Test as target)
// ===========================================================================

header("Anim graph splice nodes");

const ABP_PATH = `${SCRATCH_ROOT}/ABP_Test`;

await checkEndpointWired("POST /editor/anim/add_sequence_player (no sequence) — endpoint reachable", async () => {
  await ue5.animAddSequencePlayer({ anim_bp_path: ABP_PATH, sequence: "" });
});

await checkEndpointWired("POST /editor/anim/add_copy_bone — endpoint reachable", async () => {
  await ue5.animAddCopyBone({ anim_bp_path: ABP_PATH, source_bone: "Box", target_bone: "Box" });
});

await checkEndpointWired("POST /editor/anim/add_two_bone_ik — endpoint reachable", async () => {
  await ue5.animAddTwoBoneIK({ anim_bp_path: ABP_PATH, ik_bone: "Box" });
});

await checkEndpointWired("POST /editor/anim/add_hand_ik_retargeting — endpoint reachable", async () => {
  await ue5.animAddHandIKRetargeting({
    anim_bp_path: ABP_PATH,
    right_hand_fk: "Box", left_hand_fk: "Box",
    right_hand_ik: "Box", left_hand_ik: "Box",
  });
});

await checkEndpointWired("POST /editor/anim/add_layered_bone_blend — endpoint reachable", async () => {
  await ue5.animAddLayeredBoneBlend({ anim_bp_path: ABP_PATH });
});

await checkEndpointWired("POST /editor/anim/add_modify_bone — endpoint reachable", async () => {
  await ue5.animAddModifyBone({ anim_bp_path: ABP_PATH, bone_name: "Box" });
});

await checkEndpointWired("POST /editor/anim/add_look_at — endpoint reachable", async () => {
  await ue5.animAddLookAt({ anim_bp_path: ABP_PATH, bone_to_modify: "Box" });
});

await checkEndpointWired("POST /editor/anim/add_aim_offset_blend_space — endpoint reachable", async () => {
  await ue5.animAddAimOffsetBlendSpace({ anim_bp_path: ABP_PATH, blend_space: "" });
});

await checkEndpointWired("POST /editor/anim/delete_node — endpoint reachable", async () => {
  await ue5.animDeleteNode({ anim_bp_path: ABP_PATH, node_id: "__nope__" });
});

await checkEndpointWired("POST /editor/skeleton/add_virtual_bone — endpoint reachable", async () => {
  await ue5.skeletonAddVirtualBone({ skeleton_path: ENGINE_SKELETON, source_bone: "Box", target_bone: "Box" });
});

await checkEndpointWired("POST /editor/configure_anim_blueprint — endpoint reachable", async () => {
  await ue5.configureAnimBlueprint(ABP_PATH, [], []);
});

// ===========================================================================
// 9. Widget Blueprint: create / add / get tree / style / rename / reparent / remove / compile
// ===========================================================================

header("Widget Blueprint authoring");

const WBP_PATH = `${SCRATCH_ROOT}/WBP_Test`;

await check("POST /editor/create_widget_blueprint", async () => {
  await ue5.createWidgetBlueprint("UserWidget", WBP_PATH, [], "CanvasPanel");
});

await check("POST /editor/add_widget_to_blueprint adds a TextBlock", async () => {
  await ue5.addWidgetToBlueprint(WBP_PATH, [
    { name: "MyText", type: "TextBlock", parent: "RootWidget" },
  ]);
});

await check("POST /editor/get_widget_tree returns hierarchy", async () => {
  const r = await ue5.getWidgetTree(WBP_PATH);
  assert(r && (r.root || r.widget_blueprint), `unexpected shape: ${JSON.stringify(r).slice(0, 200)}`);
});

await check("POST /editor/style_widgets applies a text style", async () => {
  await ue5.styleWidgets(WBP_PATH, [
    { name: "MyText", text: "Hello" },
  ]);
});

await check("POST /editor/rename_widget MyText → MyLabel", async () => {
  await ue5.renameWidget(WBP_PATH, "MyText", "MyLabel");
});

await checkEndpointWired("POST /editor/reparent_widget — endpoint reachable", async () => {
  // Reparenting to a non-existent panel should yield 4xx — proves the route.
  await ue5.reparentWidget(WBP_PATH, "MyLabel", "__NoSuchPanel__");
});

await check("POST /editor/remove_widget_from_blueprint", async () => {
  await ue5.removeWidgetFromBlueprint(WBP_PATH, "MyLabel", true);
});

await check("POST /editor/compile_widget_blueprint", async () => {
  await ue5.compileWidgetBlueprint(WBP_PATH);
});

// ===========================================================================
// 10. Materials / Niagara / Level environment
// ===========================================================================

header("Materials / Niagara / Level environment");

await check("POST /editor/create_material creates a parent material", async () => {
  await ue5.createMaterial({
    asset_path:    `${SCRATCH_ROOT}/M_Test`,
    use_parameters: true,
  });
});

await check("POST /editor/create_material_instance", async () => {
  await ue5.createMaterialInstance({
    asset_path:      `${SCRATCH_ROOT}/MI_Test`,
    parent_material: `${SCRATCH_ROOT}/M_Test`,
  });
});

await check("POST /editor/read_material reports params of the parametric M_Test", async () => {
  // M_Test was created with use_parameters:true → it must expose the
  // ScalarParameter 'BrightnessMultiplier' and VectorParameter 'BaseColor'.
  const r = await ue5.readMaterial({ asset_path: `${SCRATCH_ROOT}/M_Test` });
  assert(r && r.success === true, `read_material did not succeed: ${JSON.stringify(r)}`);
  assert(r.class === "Material", `expected class Material, got ${r.class}`);
  assert(Array.isArray(r.scalar_params), "scalar_params should be an array");
  assert(Array.isArray(r.vector_params), "vector_params should be an array");
  const scalarNames = r.scalar_params.map((p) => p.name);
  const vectorNames = r.vector_params.map((p) => p.name);
  assert(scalarNames.includes("BrightnessMultiplier"),
    `expected BrightnessMultiplier scalar param, got ${JSON.stringify(scalarNames)}`);
  assert(vectorNames.includes("BaseColor"),
    `expected BaseColor vector param, got ${JSON.stringify(vectorNames)}`);
});

await check("POST /editor/read_material reports an instance's parent", async () => {
  const r = await ue5.readMaterial({ asset_path: `${SCRATCH_ROOT}/MI_Test` });
  assert(r.class === "MaterialInstance", `expected MaterialInstance, got ${r.class}`);
  assert(typeof r.parent === "string" && r.parent.includes("M_Test"),
    `expected parent to reference M_Test, got ${r.parent}`);
});

await check("POST /editor/read_material 404s on a missing material", async () => {
  let threw = false;
  try { await ue5.readMaterial({ asset_path: `${SCRATCH_ROOT}/__NoSuchMaterial` }); }
  catch (e) { threw = /bridge error 4\d\d/.test(e.message); }
  assert(threw, "expected a 4xx for a non-existent material");
});

await check("POST /editor/create_material_instance accepts texture_params", async () => {
  // The handler counts textures it could LoadObject; a bogus path → 0, which
  // still proves texture_params was parsed and the override loop ran. The
  // field's presence + numeric type is the contract under test.
  const r = await ue5.createMaterialInstance({
    asset_path:      `${SCRATCH_ROOT}/MI_Tex_Test`,
    parent_material: `${SCRATCH_ROOT}/M_Test`,
    texture_params:  [{ name: "Albedo", texture_path: `${SCRATCH_ROOT}/__NoSuchTexture` }],
  });
  assert(typeof r.texture_params_set === "number",
    `expected numeric texture_params_set, got ${JSON.stringify(r)}`);
});

await check("POST /editor/create_float_curve → read_curve round-trips keys", async () => {
  const keys = [{ time: 0, value: 0 }, { time: 0.5, value: 1 }, { time: 1, value: 0 }];
  const created = await ue5.createFloatCurve({ asset_path: `${SCRATCH_ROOT}/Curve_Float_Test`, keys });
  assert(created.success === true && created.key_count === 3,
    `create_float_curve unexpected: ${JSON.stringify(created)}`);
  const read = await ue5.readCurve({ asset_path: `${SCRATCH_ROOT}/Curve_Float_Test` });
  assert(read.type === "float", `expected type float, got ${read.type}`);
  assert(read.keys.length === 3, `expected 3 keys, got ${read.keys.length}`);
  assert(Math.abs(read.keys[1].time - 0.5) < 1e-4 && Math.abs(read.keys[1].value - 1) < 1e-4,
    `mid key mismatch: ${JSON.stringify(read.keys[1])}`);
});

await check("POST /editor/create_color_curve → read_curve reports color keys", async () => {
  const keys = [{ time: 0, r: 0, g: 1, b: 0 }, { time: 1, r: 1, g: 0, b: 0 }];
  await ue5.createColorCurve({ asset_path: `${SCRATCH_ROOT}/Curve_Color_Test`, keys });
  const read = await ue5.readCurve({ asset_path: `${SCRATCH_ROOT}/Curve_Color_Test` });
  assert(read.type === "color", `expected type color, got ${read.type}`);
  assert(read.keys.length === 2, `expected 2 keys, got ${read.keys.length}`);
  const k0 = read.keys[0];
  assert(["r", "g", "b", "a"].every((c) => typeof k0[c] === "number"),
    `color key missing rgba channels: ${JSON.stringify(k0)}`);
});

await check("POST /editor/read_curve 404s on a missing curve", async () => {
  let threw = false;
  try { await ue5.readCurve({ asset_path: `${SCRATCH_ROOT}/__NoSuchCurve` }); }
  catch (e) { threw = /bridge error 4\d\d/.test(e.message); }
  assert(threw, "expected a 4xx for a non-existent curve");
});

await check("POST /editor/create_niagara_system creates blank NS", async () => {
  await ue5.createNiagaraSystem({ asset_path: `${SCRATCH_ROOT}/NS_Test` });
});

await checkEndpointWired("POST /editor/configure_niagara_system — endpoint reachable", async () => {
  await ue5.configureNiagaraSystem({ asset_path: `${SCRATCH_ROOT}/NS_Test`, spawn_count: 5 });
});

await checkEndpointWired("POST /editor/set_niagara_emitter_params — endpoint reachable", async () => {
  await ue5.setNiagaraEmitterParams({ asset_path: `${SCRATCH_ROOT}/NS_Test`, list_only: true });
});

await checkEndpointWired("POST /editor/set_level_environment — endpoint reachable", async () => {
  // Bogus level on purpose — we just need to confirm route + parser are alive.
  await ue5.setLevelEnvironment(`${SCRATCH_ROOT}/__NoSuchLevel`, { fog: { density: 0.02 } });
});

// ===========================================================================
// 11. Behavior Tree authoring
// ===========================================================================

header("Behavior Tree");

const BT_PATH = `${SCRATCH_ROOT}/BT_Test`;
const BB_PATH = `${SCRATCH_ROOT}/BB_Test`;

await check("POST /bt/create_blackboard", async () => {
  await ue5.btCreateBlackboard({ asset_path: BB_PATH, save: true });
});

await check("POST /bt/create_tree links blackboard", async () => {
  await ue5.btCreateTree({ asset_path: BT_PATH, blackboard_path: BB_PATH, save: true });
});

await check("POST /bt/add_blackboard_keys", async () => {
  await ue5.btAddBlackboardKeys({
    asset_path: BB_PATH,
    keys: [{ name: "TargetActor", type: "Object" }],
    save: true,
  });
});

await check("POST /bt/add_logic with single Selector root", async () => {
  await ue5.btAddLogic({
    behavior_tree: BT_PATH,
    nodes:    [{ id: "root", type: "Selector" }],
    save:     true,
    compile:  true,
  });
});

await check("GET /bt/read_tree returns graph", async () => {
  const r = await ue5.btReadTree({ behavior_tree: BT_PATH });
  assert(r && typeof r === "object", `unexpected`);
});

// ===========================================================================
// 12. Gameplay Ability System
// ===========================================================================

header("Gameplay Ability System");

await check("POST /gas/setup_actor adds ASC to test BP", async () => {
  await ue5.gasSetupActor({ blueprint_path: BP_PATH, replication_mode: "Mixed", save: false });
});

await check("POST /gas/create_attribute_set", async () => {
  await ue5.gasCreateAttributeSet({
    asset_path: `${SCRATCH_ROOT}/AS_Test`,
    attributes: [{ name: "Health", default: 100 }],
    save:       true,
  });
});

await check("POST /gas/create_ability", async () => {
  await ue5.gasCreateAbility({ asset_path: `${SCRATCH_ROOT}/GA_Test`, save: true });
});

await check("POST /gas/create_effect", async () => {
  await ue5.gasCreateEffect({ asset_path: `${SCRATCH_ROOT}/GE_Test`, save: true });
});

await check("POST /gas/configure_asc on test BP", async () => {
  await ue5.gasConfigureAsc({ blueprint_path: BP_PATH, replication_mode: "Minimal", save: false });
});

await check("GET /gas/read_setup returns ASC info", async () => {
  const r = await ue5.gasReadSetup({ blueprint_path: BP_PATH });
  assert(r && typeof r === "object", `unexpected`);
});

// ===========================================================================
// 13. Mesh (Geometry Script) — handle lifecycle
// ===========================================================================

header("Geometry Script mesh");

const MESH_HANDLE = "ngg_test_handle";

await check("POST /mesh/create creates dynamic mesh handle", async () => {
  await ue5.meshCreate(MESH_HANDLE);
});

await check("POST /mesh/append_primitive appends a box", async () => {
  await ue5.meshAppendPrimitive({
    handle: MESH_HANDLE,
    shape:  "Box",
    params: { dimensions: { x: 100, y: 100, z: 100 } },
  });
});

await check("POST /mesh/transform translates mesh", async () => {
  await ue5.meshTransform({
    handle: MESH_HANDLE,
    location: { x: 10, y: 0, z: 0 },
  });
});

await checkEndpointWired("POST /mesh/boolean — endpoint reachable", async () => {
  // Need a second handle for a real boolean. Hit the endpoint with a missing
  // counterpart so we exercise routing + parser without buying complexity.
  await ue5.meshBoolean({ handle: MESH_HANDLE, other_handle: "__nope__", op: "Union" });
});

await check("POST /mesh/deform bends the mesh", async () => {
  // Supported ops on the bridge: Bend, Twist, Taper, Flare.
  await ue5.meshDeform({ handle: MESH_HANDLE, op: "Bend", amount: 0.1 });
});

await check("POST /mesh/remesh subdivides", async () => {
  await ue5.meshRemesh({ handle: MESH_HANDLE, target_edge_length: 50, iterations: 1 });
});

await check("POST /mesh/bake_static creates a UStaticMesh asset", async () => {
  await ue5.meshBakeStatic({ handle: MESH_HANDLE, asset_path: `${SCRATCH_ROOT}/SM_Test` });
});

await checkEndpointWired("POST /mesh/add_socket — endpoint reachable", async () => {
  await ue5.meshAddSocket({
    mesh_path:   `${SCRATCH_ROOT}/SM_Test`,
    socket_name: "TestSocket",
  });
});

await check("POST /mesh/delete_handle releases the mesh handle", async () => {
  await ue5.meshDeleteHandle(MESH_HANDLE);
});

// ===========================================================================
// 14. Enhanced Input
// ===========================================================================

header("Enhanced Input");

await checkEndpointWired("POST /input/configure_imc — endpoint reachable", async () => {
  // Without a real IMC asset we expect a structured 4xx, which proves route + parsing.
  await ue5.configureIMC(`${SCRATCH_ROOT}/__NoSuchIMC`, []);
});

// ===========================================================================
// 15. Level lifecycle
// ===========================================================================

header("Levels");

await check("POST /editor/create_level creates scratch L_Test", async () => {
  await ue5.createLevel(`${SCRATCH_ROOT}/L_Test`);
});

await checkEndpointWired("POST /editor/set_world_settings — endpoint reachable", async () => {
  // Best-effort: changes apply to current editor world. Empty payload should
  // either no-op (200) or yield a structured 4xx.
  await ue5.setWorldSettings({});
});

await checkEndpointWired("POST /assets/import — endpoint reachable", async () => {
  // Bogus source path → expect 4xx.
  await ue5.importAsset(`C:/__NoSuchFile__.wav`, SCRATCH_ROOT);
});

// ===========================================================================
// 16. Actors — list / update / delete (smoke covered; we add update here)
// ===========================================================================

header("Actors (update path)");

let actorSpawnedOk = false;
await check("POST /editor/spawn_actor_in_level — re-spawn StaticMeshActor", async () => {
  try {
    await ue5.spawnActorInLevel(
      "StaticMeshActor",
      { x: 0, y: 0, z: 600 },
      { pitch: 0, yaw: 0, roll: 0 },
      "NGGEndpointTest_Cube",
      ENGINE_CUBE,
    );
    actorSpawnedOk = true;
  } catch (err) {
    if (/read-?only|no world|level/i.test(err.message)) {
      note(`spawn skipped — level read-only (${err.message})`);
      return;
    }
    throw err;
  }
});

if (actorSpawnedOk) {
  await check("POST /editor/update_actor relabels and moves the actor", async () => {
    await ue5.updateActor("NGGEndpointTest_Cube", {
      location: { x: 100, y: 0, z: 600 },
      newLabel: "NGGEndpointTest_Cube2",
    });
  });

  await check("POST /editor/delete_actor removes the test actor", async () => {
    await ue5.deleteActor("NGGEndpointTest_Cube2");
  });

  await check("clear editor selection (defensive)", clearEditorSelection);
} else {
  note("update_actor/delete_actor skipped — spawn unavailable");
}

// ===========================================================================
// 17. Batch endpoint
// ===========================================================================

header("Batch");

await check("POST /editor/batch executes multiple ops", async () => {
  const r = await ue5.batch([
    { method: "POST", path: "/editor/save_all", body: {} },
    { method: "GET",  path: "/assets/list?path=" + encodeURIComponent(SCRATCH_ROOT) },
  ]);
  assert(Array.isArray(r?.results) && r.results.length === 2, `unexpected: ${JSON.stringify(r).slice(0, 200)}`);
});

// ===========================================================================
// 18. Python exec
// ===========================================================================

header("Python exec");

await check("POST /editor/exec_python runs a trivial statement", async () => {
  await ue5.execPython("print('NGG endpoint test python')", "execute_file");
});

// ===========================================================================
// 19. Destructive endpoints — listed but skipped
// ===========================================================================

header("Destructive endpoints (intentionally not run)");
note("POST /editor/shutdown          — would close the editor");
note("POST /editor/build_and_run     — full project build");
note("POST /editor/kill_and_restart  — full editor restart");

// ===========================================================================
// Cleanup
// ===========================================================================

header("Cleanup");

if (!KEEP) {
  const SCRATCH = [
    `${SCRATCH_ROOT}/BP_Test`,
    `${SCRATCH_ROOT}/BP_Reparent`,
    `${SCRATCH_ROOT}/WBP_Test`,
    `${SCRATCH_ROOT}/S_Test`,
    `${SCRATCH_ROOT}/E_Test`,
    `${SCRATCH_ROOT}/BPI_Test`,
    `${SCRATCH_ROOT}/ABP_Test`,
    `${SCRATCH_ROOT}/M_Test`,
    `${SCRATCH_ROOT}/MI_Test`,
    `${SCRATCH_ROOT}/MI_Tex_Test`,
    `${SCRATCH_ROOT}/Curve_Float_Test`,
    `${SCRATCH_ROOT}/Curve_Color_Test`,
    `${SCRATCH_ROOT}/NS_Test`,
    `${SCRATCH_ROOT}/BT_Test`,
    `${SCRATCH_ROOT}/BB_Test`,
    `${SCRATCH_ROOT}/AS_Test`,
    `${SCRATCH_ROOT}/GA_Test`,
    `${SCRATCH_ROOT}/GE_Test`,
    `${SCRATCH_ROOT}/SM_Test`,
    `${SCRATCH_ROOT}/L_Test`,
  ];
  for (const p of SCRATCH) {
    await check(`delete ${p}`, async () => { await safeDeleteAsset(p); });
  }
  await check("save after cleanup", async () => { await ue5.saveAll(); });
} else {
  process.stdout.write(`  (skipped — --keep flag set; scratch under ${SCRATCH_ROOT})\n`);
}

// ===========================================================================
// Summary
// ===========================================================================

const total = pass + fail;
process.stdout.write(
  `\n━━━ Endpoint coverage summary: ${pass}/${total} passed, ${fail} failed, ${skip} skipped ━━━\n`
);
if (failures.length) {
  process.stdout.write(`\nFailures:\n`);
  for (const f of failures) process.stdout.write(`  • ${f.name}: ${f.error}\n`);
}
process.exit(fail === 0 ? 0 : 1);
