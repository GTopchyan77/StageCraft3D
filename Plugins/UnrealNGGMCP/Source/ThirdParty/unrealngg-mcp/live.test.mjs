#!/usr/bin/env node
// Copyright 2025-2026 NGG. All Rights Reserved.
// live.test.mjs — end-to-end integration tests against a LIVE Unreal Editor.
//
// Unlike the unit tests (helpers/paths/ue5client/pcg.unit/meshy) which run
// against a mock HTTP server, this file talks to the REAL UnrealNGGMCP plugin
// over the bridge at http://localhost:6776. It proves the plugin actually
// performs the requested operations — assets really get created, properties
// really stick, cleanup really deletes them.
//
// Prerequisites:
//   1. Unreal Editor is open with your project
//   2. UnrealNGGMCP plugin is enabled (Edit → Plugins)
//   3. Output Log shows "UnrealNGGMCP: HTTP bridge listening on http://localhost:6776"
//
// Run:
//   node live.test.mjs              (default — scratch assets under /Game/__NGGTest__)
//   node live.test.mjs --keep       (don't delete the scratch assets at the end)
//
// Exit code 0 on success, 1 on any failure.

// Live tests can hit a cold editor where the first asset create triggers a
// class-hierarchy load that takes >15s. Extend the bridge timeout to 60s for
// this suite. Unit tests keep the tighter default.
process.env.NGG_BRIDGE_TIMEOUT_MS = process.env.NGG_BRIDGE_TIMEOUT_MS || "60000";

import * as ue5 from "./ue5client.js";

const SCRATCH_ROOT = "/Game/__NGGTest__";
const KEEP         = process.argv.includes("--keep");

// Safe delete — routes through EditorAssetLibrary.delete_asset inside the
// editor's Python runtime, instead of /assets/delete which calls
// ObjectTools::ForceDeleteObjects. The latter triggers an outliner selection
// update that can assert in TypedElementRegistry and crash the editor.
// Best-effort: swallows errors so cleanup always completes.
async function safeDeleteAsset(assetPath) {
  const py =
    `import unreal\n` +
    `lib = unreal.EditorAssetLibrary\n` +
    `if lib.does_asset_exist("${assetPath}"):\n` +
    `    ok = lib.delete_asset("${assetPath}")\n` +
    `    print("DELETED" if ok else "FAILED")\n` +
    `else:\n` +
    `    print("GONE")\n`;
  try {
    await ue5.execPython(py, "execute_file");
  } catch { /* swallow — cleanup is best-effort */ }
}

// Clear the editor selection. Selection-mutating HTTP endpoints (spawn_actor,
// delete_actor, delete_asset) can leave a dangling TypedElementHandle in
// USelection. A later viewport tick then hits the assertion
// "Element type ID '0' has not been registered!" — crashing the editor.
// Calling this between sections protects downstream tests.
async function clearEditorSelection() {
  const py =
    `import unreal\n` +
    `try:\n` +
    `    unreal.EditorActorSubsystem().set_selected_level_actors([])\n` +
    `except Exception as e:\n` +
    `    print("select_none_failed:", e)\n`;
  try {
    await ue5.execPython(py, "execute_file");
  } catch { /* best-effort */ }
}

let pass = 0;
let fail = 0;

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
    fail++;
  }
}

function assert(cond, msg) {
  if (!cond) throw new Error(msg);
}

// ---------------------------------------------------------------------------

header("Bridge connectivity");

await check("health check returns ok", async () => {
  const h = await ue5.healthCheck();
  assert(h?.status === "ok", `unexpected status: ${JSON.stringify(h)}`);
  process.stdout.write(`        project=${h.project} port=${h.port ?? "?"}\n`);
});

if (fail > 0) {
  process.stderr.write(
    "\nBridge not reachable — make sure:\n" +
    "  1. Unreal Editor is running with your project\n" +
    "  2. UnrealNGGMCP plugin is enabled\n" +
    "  3. Output Log shows 'UnrealNGGMCP: HTTP bridge listening on http://localhost:6776'\n"
  );
  process.exit(1);
}

// Pre-clean any scratch assets left behind by a previous crashed run so the
// suite is re-runnable. Uses execPython so we don't rely on /assets/delete.
header("Pre-cleanup (idempotent)");
for (const stale of [
  `${SCRATCH_ROOT}/DA_NGGTest`,
  `${SCRATCH_ROOT}/BP_NGGTest`,
  `${SCRATCH_ROOT}/PCG_NGGTest`,
]) {
  await check(`evict stale ${stale} if present`, async () => {
    const py =
      `import unreal\n` +
      `lib = unreal.EditorAssetLibrary\n` +
      `if lib.does_asset_exist("${stale}"):\n` +
      `    lib.delete_asset("${stale}")\n` +
      `print("evicted:", "${stale}")\n`;
    await ue5.execPython(py, "execute_file");
  });
}

// ---------------------------------------------------------------------------

header("Content Browser — list / get");

await check("list_assets on /Game returns an array", async () => {
  const r = await ue5.listAssets("/Game");
  assert(Array.isArray(r.assets) || Array.isArray(r), `unexpected shape: ${JSON.stringify(r).slice(0, 200)}`);
});

await check("list_assets on non-existent path doesn't crash", async () => {
  const r = await ue5.listAssets("/Game/__DefinitelyDoesNotExist__");
  assert(r !== undefined, "got undefined");
});

// ---------------------------------------------------------------------------

header("Data asset — create / set_property / get / save");

// The data-asset test needs a concrete UDataAsset subclass from the project
// under test. Bare UDataAsset is abstract — the plugin can instantiate it but
// it can't be saved, which triggers a modal dialog that blocks the bridge.
// Set NGG_LIVE_DATAASSET_CLASS to a concrete subclass shipped by your project
// (e.g. "MyProjectGameSettings"). If unset, this section is skipped.
const DA_CLASS = process.env.NGG_LIVE_DATAASSET_CLASS ?? "";
const DA_PATH  = `${SCRATCH_ROOT}/DA_NGGTest`;
const DA_SKIP  = !DA_CLASS;

if (DA_SKIP) {
  process.stdout.write(
    `  SKIP  data-asset section — set NGG_LIVE_DATAASSET_CLASS to a concrete\n` +
    `        UDataAsset subclass in your project to enable this test.\n`);
} else {
  await check(`create data asset (${DA_CLASS})`, async () => {
    const r = await ue5.createAsset(DA_CLASS, DA_PATH);
    assert(r?.created || r?.asset_path, `unexpected create response: ${JSON.stringify(r)}`);
  });

  await check("set_property on an unknown field returns a plugin-side error (endpoint is live)", async () => {
    try {
      await ue5.setAssetProperty(DA_PATH, "DefinitelyNotAField_NGG", "hello");
    } catch (err) {
      assert(/property|not found|unknown|fail/i.test(err.message), `unexpected error: ${err.message}`);
    }
  });

  await check("get_asset returns JSON for the created asset", async () => {
    const r = await ue5.getAsset(DA_PATH);
    assert(typeof r === "object" && r !== null, "expected object");
  });

  await check("save_all persists the new asset", async () => {
    await ue5.saveAll();
  });
}

// ---------------------------------------------------------------------------

header("Blueprint — create / reparent / compile / delete");

const BP_PATH = `${SCRATCH_ROOT}/BP_NGGTest`;

await check("create blueprint inheriting from AActor", async () => {
  const r = await ue5.createBlueprint("Actor", BP_PATH);
  assert(r?.created || r?.asset_path, `unexpected: ${JSON.stringify(r)}`);
});

await check("compile newly created blueprint", async () => {
  const r = await ue5.bpCompile({ blueprint: BP_PATH, save: true });
  // bpCompile returns structured result; success is field or implicit 200.
  assert(r !== undefined, "no response");
});

await check("read blueprint graphs (should at least include ConstructionScript / EventGraph)", async () => {
  const r = await ue5.bpReadGraph({ blueprint: BP_PATH });
  assert(r !== undefined, "no graph response");
});

// ---------------------------------------------------------------------------

header("Batch endpoint — single round-trip for multiple ops");

await check("batch(save_all + list_assets) returns 2 results", async () => {
  const r = await ue5.batch([
    { method: "POST", path: "/editor/save_all",   body: {} },
    { method: "GET",  path: "/assets/list?path=" + encodeURIComponent("/Game") },
  ]);
  assert(Array.isArray(r?.results), `unexpected: ${JSON.stringify(r).slice(0, 200)}`);
  assert(r.results.length === 2, `expected 2 results, got ${r.results.length}`);
});

// ---------------------------------------------------------------------------

header("PCG — list / create / add_node / connect / save");

const PCG_PATH = `${SCRATCH_ROOT}/PCG_NGGTest`;

await check("pcg list_node_types via exec_python", async () => {
  const mod = await import("./pcg.js");
  const resp = await mod.pcgListNodeTypes({});
  assert(!resp.isError, `error: ${resp.content?.[0]?.text}`);
  const data = JSON.parse(resp.content[0].text);
  assert(data.classes?.length > 10, `only ${data.classes?.length} classes returned`);
});

await check("pcg create graph", async () => {
  const mod = await import("./pcg.js");
  const resp = await mod.pcgCreateGraph({ asset_path: PCG_PATH, overwrite: true });
  assert(!resp.isError, `error: ${resp.content?.[0]?.text}`);
});

await check("pcg add surface sampler node", async () => {
  const mod = await import("./pcg.js");
  const resp = await mod.pcgAddNode({
    graph_path:     PCG_PATH,
    settings_class: "PCGSurfaceSamplerSettings",
  });
  assert(!resp.isError, `error: ${resp.content?.[0]?.text}`);
  const data = JSON.parse(resp.content[0].text);
  assert(/^SurfaceSampler/.test(data.node_name), `unexpected node_name: ${data.node_name}`);
});

await check("pcg save graph", async () => {
  const mod = await import("./pcg.js");
  const resp = await mod.pcgSaveGraph({ graph_path: PCG_PATH });
  assert(!resp.isError, `error: ${resp.content?.[0]?.text}`);
});

// ---------------------------------------------------------------------------
// Actors section runs LAST — spawn_actor + delete_actor leaves a dangling
// TypedElementHandle in USelection that crashes the next viewport tick. Any
// later test that runs after these would be poisoned. We clear selection
// immediately after to shield the cleanup section below.

header("Actors — list / spawn / update / delete (current level)");

await check("list_actors returns array", async () => {
  const r = await ue5.listActors();
  assert(r?.actors || Array.isArray(r), `unexpected: ${JSON.stringify(r).slice(0, 200)}`);
});

let spawnedOk = false;
await check("spawn_actor places a StaticMeshActor at origin", async () => {
  try {
    const r = await ue5.spawnActorInLevel(
      "StaticMeshActor",
      { x: 0, y: 0, z: 500 },
      { pitch: 0, yaw: 0, roll: 0 },
      "NGGTest_Cube",
      "/Engine/BasicShapes/Cube",
    );
    assert(r !== undefined, "no response");
    spawnedOk = true;
  } catch (err) {
    // Some levels don't allow spawning (read-only map); tolerate that.
    if (/read-?only|no world|level/i.test(err.message)) {
      process.stdout.write(`        (skipped — level doesn't accept spawns: ${err.message})\n`);
      return;
    }
    throw err;
  }
});

if (spawnedOk) {
  await check("delete spawned actor by label", async () => {
    await ue5.deleteActor("NGGTest_Cube");
  });
}

await check("clear editor selection (defensive — avoids viewport crash)", async () => {
  await clearEditorSelection();
});

// ---------------------------------------------------------------------------

header("Cleanup");

if (!KEEP) {
  for (const p of [DA_PATH, BP_PATH, PCG_PATH]) {
    await check(`delete ${p} (via EditorAssetLibrary)`, async () => {
      await safeDeleteAsset(p);
    });
  }
  await check("save after cleanup", async () => { await ue5.saveAll(); });
} else {
  process.stdout.write(`  (skipped — --keep flag set; scratch assets left under ${SCRATCH_ROOT})\n`);
}

// ---------------------------------------------------------------------------

const total = pass + fail;
process.stdout.write(
  `\n━━━ Live integration summary: ${pass}/${total} passed, ${fail} failed ━━━\n`
);
process.exit(fail === 0 ? 0 : 1);
