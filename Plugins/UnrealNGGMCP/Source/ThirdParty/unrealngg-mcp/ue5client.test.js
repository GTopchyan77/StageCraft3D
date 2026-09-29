// Copyright 2025-2026 NGG. All Rights Reserved.
// ue5client.test.js — integration tests for the ue5client HTTP wrapper.
// Spins up a real local HTTP mock of the UnrealNGGMCP bridge and exercises
// every public ue5client function against it. No live Unreal Editor required.
//
// Run: node --test ue5client.test.js

import { test, before, after, beforeEach } from "node:test";
import assert from "node:assert/strict";
import { createServer } from "node:http";
import { URL } from "node:url";
import os from "node:os";
import path from "node:path";
import fsExtra from "node:fs";

import * as ue5 from "./ue5client.js";

// ---------------------------------------------------------------------------
// Mock bridge
// ---------------------------------------------------------------------------
// Behaviour is driven by a per-test `handler` closure so each case can
// arrange whatever response it expects.

let server;
let serverUrl;
let handler;        // (req, body) => { status, body, headers?, delayMs? }
let requestLog;     // array of { method, path, query, headers, body }

function readBody(req) {
  return new Promise((resolve) => {
    let buf = "";
    req.on("data", (c) => (buf += c));
    req.on("end", () => resolve(buf));
  });
}

before(async () => {
  server = createServer(async (req, res) => {
    const bodyText = await readBody(req);
    let parsedBody = null;
    if (bodyText) {
      try { parsedBody = JSON.parse(bodyText); } catch { parsedBody = bodyText; }
    }
    const parsedUrl = new URL(req.url, "http://localhost");
    requestLog.push({
      method:  req.method,
      path:    parsedUrl.pathname,
      query:   Object.fromEntries(parsedUrl.searchParams.entries()),
      headers: req.headers,
      body:    parsedBody,
    });

    const out = handler ? handler(req, parsedBody, parsedUrl) : { status: 200, body: { ok: true } };
    if (out.delayMs) await new Promise((r) => setTimeout(r, out.delayMs));
    res.writeHead(out.status ?? 200, {
      "Content-Type": "application/json",
      ...(out.headers ?? {}),
    });
    res.end(typeof out.body === "string" ? out.body : JSON.stringify(out.body ?? {}));
  });

  await new Promise((resolve) => {
    server.listen(0, "127.0.0.1", () => {
      const { port } = server.address();
      serverUrl = `http://127.0.0.1:${port}`;
      resolve();
    });
  });

  process.env.NGG_BRIDGE_URL = serverUrl;
});

after(() => {
  delete process.env.NGG_BRIDGE_URL;
  delete process.env.NGG_BRIDGE_TIMEOUT_MS;
  delete process.env.NGG_BRIDGE_TOKEN;
  return new Promise((resolve) => server.close(resolve));
});

beforeEach(() => {
  handler = null;
  requestLog = [];
  delete process.env.NGG_BRIDGE_TOKEN;
  delete process.env.NGG_BRIDGE_TIMEOUT_MS;
});

// ---------------------------------------------------------------------------
// healthCheck
// ---------------------------------------------------------------------------

test("healthCheck GETs /health and parses JSON body", async () => {
  handler = () => ({ status: 200, body: { status: "ok", project: "MyGame" } });
  const result = await ue5.healthCheck();
  assert.equal(result.status, "ok");
  assert.equal(result.project, "MyGame");
  assert.equal(requestLog.length, 1);
  assert.equal(requestLog[0].method, "GET");
  assert.equal(requestLog[0].path, "/health");
});

// ---------------------------------------------------------------------------
// listAssets / getAsset — query-string encoding
// ---------------------------------------------------------------------------

test("listAssets encodes the path into the query string", async () => {
  handler = () => ({ status: 200, body: { assets: [] } });
  await ue5.listAssets("/Game/Data/Exercises");
  assert.equal(requestLog[0].path, "/assets/list");
  assert.equal(requestLog[0].query.path, "/Game/Data/Exercises");
});

test("getAsset encodes the path into the query string", async () => {
  handler = () => ({ status: 200, body: { properties: {} } });
  await ue5.getAsset("/Game/Data/DA_Foo");
  assert.equal(requestLog[0].path, "/assets/get");
  assert.equal(requestLog[0].query.path, "/Game/Data/DA_Foo");
});

test("listAssets uses default '/Game' when called without args", async () => {
  handler = () => ({ status: 200, body: { assets: [] } });
  await ue5.listAssets();
  assert.equal(requestLog[0].query.path, "/Game");
});

// ---------------------------------------------------------------------------
// setAssetProperty / createAsset / deleteAsset — POST body shape
// ---------------------------------------------------------------------------

test("setAssetProperty POSTs path/property/value as JSON", async () => {
  handler = () => ({ status: 200, body: { success: true } });
  await ue5.setAssetProperty("/Game/X", "MyProp", 42);
  const r = requestLog[0];
  assert.equal(r.method, "POST");
  assert.equal(r.path, "/assets/set_property");
  assert.deepEqual(r.body, { path: "/Game/X", property: "MyProp", value: 42 });
});

test("createAsset uses 'class' key (UE5 plugin spelling)", async () => {
  handler = () => ({ status: 200, body: { created: "/Game/Y" } });
  await ue5.createAsset("MyDataAsset", "/Game/Y");
  assert.deepEqual(requestLog[0].body, { class: "MyDataAsset", path: "/Game/Y" });
});

test("deleteAsset POSTs asset_path", async () => {
  handler = () => ({ status: 200, body: { deleted: true } });
  await ue5.deleteAsset("/Game/Goner");
  assert.equal(requestLog[0].path, "/assets/delete");
  assert.deepEqual(requestLog[0].body, { asset_path: "/Game/Goner" });
});

// ---------------------------------------------------------------------------
// saveAll
// ---------------------------------------------------------------------------

test("saveAll POSTs {} to /editor/save_all", async () => {
  handler = () => ({ status: 200, body: { saved: 3 } });
  await ue5.saveAll();
  assert.equal(requestLog[0].path, "/editor/save_all");
  assert.equal(requestLog[0].method, "POST");
  assert.deepEqual(requestLog[0].body, {});
});

// ---------------------------------------------------------------------------
// batch
// ---------------------------------------------------------------------------

test("batch forwards the full operations array in one POST", async () => {
  handler = () => ({
    status: 200,
    body: { results: [{ index: 0, status: 200, body: {} }, { index: 1, status: 200, body: {} }] },
  });
  const ops = [
    { method: "POST", path: "/editor/save_all",     body: {} },
    { method: "POST", path: "/assets/set_property", body: { path: "/Game/X", property: "A", value: 1 } },
  ];
  const result = await ue5.batch(ops);
  assert.equal(requestLog.length, 1);
  assert.equal(requestLog[0].path, "/editor/batch");
  assert.deepEqual(requestLog[0].body.operations, ops);
  assert.equal(result.results.length, 2);
});

// ---------------------------------------------------------------------------
// Error & timeout behaviour
// ---------------------------------------------------------------------------

test("non-2xx JSON body surfaces error.error field in thrown message", async () => {
  handler = () => ({ status: 500, body: { error: "kaboom" } });
  await assert.rejects(
    () => ue5.healthCheck(),
    (err) => /UE5 bridge error 500/.test(err.message) && /kaboom/.test(err.message),
  );
});

test("a 403 about AuthToken is rewritten into actionable instructions", async () => {
  // The bridge's own wording points at DefaultEngine.ini — the tracked file the
  // token must NOT live in. The client must redirect the user to UserEngine.ini.
  handler = () => ({
    status: 403,
    body: { error: "editor/exec_python is disabled: no AuthToken configured. Set one in DefaultEngine.ini under [UnrealNGGMCP] AuthToken=<token>." },
  });
  await assert.rejects(
    () => ue5.execPython("print(1)"),
    (err) => {
      assert.match(err.message, /UE5 bridge error 403/);
      assert.match(err.message, /no AuthToken configured/);
      assert.match(err.message, /npm run set-token/);
      assert.match(err.message, /Config\/UserEngine\.ini/);
      // The misleading "set it in DefaultEngine.ini" instruction must be gone —
      // DefaultEngine.ini may only appear as the warning not to use it.
      assert.ok(
        !/Set one in DefaultEngine\.ini/.test(err.message),
        `stale instruction survived:\n${err.message}`
      );
      // exec_python is the shared substrate for the pcg_* family; the blast
      // radius has to be spelled out or a failing pcg_* call looks unrelated.
      assert.match(err.message, /pcg_\*/);
      return true;
    },
  );
});

test("a 403 on a non-python route omits the pcg_* note but keeps the fix", async () => {
  handler = () => ({
    status: 403,
    body: { error: "editor/shutdown is disabled: no AuthToken configured. Set one in DefaultEngine.ini under [UnrealNGGMCP] AuthToken=<token>." },
  });
  await assert.rejects(
    () => ue5.healthCheck(),
    (err) => {
      assert.match(err.message, /npm run set-token/);
      assert.ok(!/pcg_\*/.test(err.message), `unrelated pcg note leaked:\n${err.message}`);
      return true;
    },
  );
});

test("an unrelated 403 passes through untouched", async () => {
  // Only the token case is rewritten; the loopback-peer refusal must not get a
  // "run set-token" suggestion that would send the user down the wrong path.
  handler = () => ({
    status: 403,
    body: { error: "Forbidden: this bridge serves loopback clients only." },
  });
  await assert.rejects(
    () => ue5.healthCheck(),
    (err) => {
      assert.match(err.message, /loopback clients only/);
      assert.ok(!/set-token/.test(err.message), `unwanted rewrite:\n${err.message}`);
      return true;
    },
  );
});

test("checkBridgeProtocol accepts a matching plugin", () => {
  const r = ue5.checkBridgeProtocol({ bridge_protocol: ue5.EXPECTED_BRIDGE_PROTOCOL });
  assert.equal(r.ok, true);
  assert.equal(r.warning, null);
});

test("checkBridgeProtocol stays quiet for a plugin with no version surface", () => {
  // Predates B6. Warning about it would be noise, not signal.
  for (const health of [{}, { status: "ok" }, { bridge_protocol: null }]) {
    const r = ue5.checkBridgeProtocol(health);
    assert.equal(r.ok, true, `unexpected complaint for ${JSON.stringify(health)}`);
    assert.equal(r.warning, null);
  }
  assert.equal(ue5.checkBridgeProtocol(undefined).ok, true);
});

test("checkBridgeProtocol flags a skew in both directions", () => {
  const newer = ue5.checkBridgeProtocol({ bridge_protocol: ue5.EXPECTED_BRIDGE_PROTOCOL + 1 });
  assert.equal(newer.ok, false);
  assert.match(newer.warning, /plugin is newer/i);
  assert.match(newer.warning, /Rebuild/i, "warning must say what to do about it");

  const older = ue5.checkBridgeProtocol({ bridge_protocol: ue5.EXPECTED_BRIDGE_PROTOCOL - 1 });
  assert.equal(older.ok, false);
  assert.match(older.warning, /plugin is older/i);
});

test("getAuthTokenStatus reports the ini layer that supplied the token", () => {
  const tmpDir  = fsExtra.mkdtempSync(path.join(os.tmpdir(), "ngg-ini-"));
  const userIni = path.join(tmpDir, "UserEngine.ini");
  fsExtra.writeFileSync(userIni, "[UnrealNGGMCP]\nAuthToken=abc\n");

  const savedEnv = process.env.NGG_BRIDGE_TOKEN;
  delete process.env.NGG_BRIDGE_TOKEN;
  ue5._setIniPathsForTests([userIni]);
  try {
    const status = ue5.getAuthTokenStatus();
    assert.equal(status.configured, true);
    assert.equal(status.source, "UserEngine.ini");
    assert.deepEqual(status.disables, []);
    // The token itself must never appear in diagnostics.
    assert.ok(!JSON.stringify(status).includes("abc"), "token leaked into status");
  } finally {
    ue5._setIniPathsForTests(null);
    if (savedEnv !== undefined) process.env.NGG_BRIDGE_TOKEN = savedEnv;
    fsExtra.rmSync(tmpDir, { recursive: true, force: true });
  }
});

test("getAuthTokenStatus names what breaks when no token is configured", () => {
  const tmpDir  = fsExtra.mkdtempSync(path.join(os.tmpdir(), "ngg-ini-"));
  const userIni = path.join(tmpDir, "nope.ini");

  const savedEnv = process.env.NGG_BRIDGE_TOKEN;
  delete process.env.NGG_BRIDGE_TOKEN;
  ue5._setIniPathsForTests([userIni]);
  try {
    const status = ue5.getAuthTokenStatus();
    assert.equal(status.configured, false);
    assert.equal(status.source, null);
    assert.ok(status.disables.some(d => /pcg/i.test(d)), `pcg_* not listed: ${status.disables}`);
    assert.ok(status.disables.includes("exec_python"));
  } finally {
    ue5._setIniPathsForTests(null);
    if (savedEnv !== undefined) process.env.NGG_BRIDGE_TOKEN = savedEnv;
    fsExtra.rmSync(tmpDir, { recursive: true, force: true });
  }
});

test("NGG_BRIDGE_TOKEN env is reported as the token source", () => {
  const savedEnv = process.env.NGG_BRIDGE_TOKEN;
  process.env.NGG_BRIDGE_TOKEN = "from-env";
  try {
    const status = ue5.getAuthTokenStatus();
    assert.equal(status.configured, true);
    assert.match(status.source, /NGG_BRIDGE_TOKEN/);
  } finally {
    if (savedEnv !== undefined) process.env.NGG_BRIDGE_TOKEN = savedEnv;
    else delete process.env.NGG_BRIDGE_TOKEN;
  }
});

test("timeout produces a descriptive error mentioning the TIMEOUT_MS value", async () => {
  process.env.NGG_BRIDGE_TIMEOUT_MS = "50";
  handler = () => ({ status: 200, body: { ok: true }, delayMs: 200 });
  await assert.rejects(
    () => ue5.healthCheck(),
    (err) => /timed out after 50ms/.test(err.message),
  );
});

test("connection failure to an unreachable port produces a bridge-connection error", async () => {
  process.env.NGG_BRIDGE_URL = "http://127.0.0.1:1";  // almost certainly not listening
  process.env.NGG_BRIDGE_TIMEOUT_MS = "500";
  try {
    await assert.rejects(
      () => ue5.healthCheck(),
      // Either connect-refused path, or the timeout path — both indicate
      // the bridge is unreachable and both come from bridgeFetch.
      (err) => /bridge connection failed|bridge request timed out/i.test(err.message),
    );
  } finally {
    process.env.NGG_BRIDGE_URL = serverUrl;
    delete process.env.NGG_BRIDGE_TIMEOUT_MS;
  }
});

// ---------------------------------------------------------------------------
// Previously-unexposed C++ routes
// ---------------------------------------------------------------------------

test("duplicateAsset POSTs source + dest to /assets/duplicate", async () => {
  handler = () => ({ status: 200, body: { success: true } });
  await ue5.duplicateAsset("/Game/BP_A", "/Game/BP_B");
  assert.equal(requestLog[0].method, "POST");
  assert.equal(requestLog[0].path, "/assets/duplicate");
  assert.deepEqual(requestLog[0].body, { source: "/Game/BP_A", dest: "/Game/BP_B" });
});

test("setAssetMapEntries POSTs path/property/entries", async () => {
  handler = () => ({ status: 200, body: { success: true } });
  const entries = [{ key: "Standard", value: "/Game/BP_Bomb.BP_Bomb_C" }];
  await ue5.setAssetMapEntries("/Game/DA_Reg", "BombClasses", entries);
  assert.equal(requestLog[0].path, "/assets/set_map_entries");
  assert.deepEqual(requestLog[0].body, {
    path: "/Game/DA_Reg", property: "BombClasses", entries,
  });
});

test("bpAddInterface POSTs blueprint + interface, compile only when given", async () => {
  handler = () => ({ status: 200, body: { success: true } });
  await ue5.bpAddInterface({ blueprint: "/Game/BP_A", interface: "/Game/BPI_X" });
  assert.equal(requestLog[0].path, "/bp/add_interface");
  assert.deepEqual(requestLog[0].body, { blueprint: "/Game/BP_A", interface: "/Game/BPI_X" });

  await ue5.bpAddInterface({ blueprint: "/Game/BP_A", interface: "/Game/BPI_X", compile: false });
  assert.equal(requestLog[1].body.compile, false);
});

test("bpImplementInterfaceFunction POSTs blueprint + function", async () => {
  handler = () => ({ status: 200, body: { success: true } });
  await ue5.bpImplementInterfaceFunction({ blueprint: "/Game/BP_A", function: "GetScore" });
  assert.equal(requestLog[0].path, "/bp/implement_interface_function");
  assert.deepEqual(requestLog[0].body, { blueprint: "/Game/BP_A", function: "GetScore" });
});

test("bpRefreshAllNodes POSTs blueprint with optional compile/save", async () => {
  handler = () => ({ status: 200, body: { success: true } });
  await ue5.bpRefreshAllNodes({ blueprint: "/Game/BP_A", compile: true, save: true });
  assert.equal(requestLog[0].path, "/bp/refresh_all_nodes");
  assert.deepEqual(requestLog[0].body, { blueprint: "/Game/BP_A", compile: true, save: true });
});

// ---------------------------------------------------------------------------
// Auth token forwarding
// ---------------------------------------------------------------------------

test("Authorization header is sent when NGG_BRIDGE_TOKEN is set", async () => {
  process.env.NGG_BRIDGE_TOKEN = "my-secret";
  handler = () => ({ status: 200, body: { ok: true } });
  await ue5.healthCheck();
  assert.equal(requestLog[0].headers.authorization, "Bearer my-secret");
});

test("Authorization header is absent when NGG_BRIDGE_TOKEN is empty and no ini token exists", async () => {
  // Point the ini fallback at a nonexistent file — otherwise the real
  // project's DefaultEngine.ini token would (correctly) be picked up.
  ue5._setIniPathForTests(path.join(os.tmpdir(), "ngg-no-such-dir", "DefaultEngine.ini"));
  handler = () => ({ status: 200, body: { ok: true } });
  try {
    await ue5.healthCheck();
    assert.equal(requestLog[0].headers.authorization, undefined);
  } finally {
    ue5._setIniPathForTests(null);
  }
});

test("env changes after import still take effect (lazy binding)", async () => {
  // Validates that BASE_URL/TOKEN are read on each request, not at import.
  process.env.NGG_BRIDGE_TOKEN = "first";
  handler = () => ({ status: 200, body: { ok: true } });
  await ue5.healthCheck();
  assert.equal(requestLog[0].headers.authorization, "Bearer first");

  process.env.NGG_BRIDGE_TOKEN = "second";
  await ue5.healthCheck();
  assert.equal(requestLog[1].headers.authorization, "Bearer second");
});

// ---------------------------------------------------------------------------
// Exercise creation / mutation endpoints
// ---------------------------------------------------------------------------

test("createExerciseFull POSTs to /adl/exercise/create_full", async () => {
  handler = () => ({ status: 200, body: { created: true, asset_path: "/Game/Data/DA_X" } });
  const payload = {
    exercise_id: "ADL_01",
    display_name: "Test",
    asset_path: "/Game/Data/DA_X",
    environment_level: "",
    default_difficulty: "Medium",
    phases: [{ phase_display_name: "P", steps: [] }],
  };
  await ue5.createExerciseFull(payload);
  assert.equal(requestLog[0].path, "/adl/exercise/create_full");
  assert.deepEqual(requestLog[0].body, payload);
});

test("addPhaseToExercise uses asset_path + phase in the body", async () => {
  handler = () => ({ status: 200, body: { phase_index: 0 } });
  const phase = { phase_display_name: "New", steps: [] };
  await ue5.addPhaseToExercise("/Game/X", phase);
  assert.equal(requestLog[0].path, "/adl/exercise/add_phase");
  assert.deepEqual(requestLog[0].body, { asset_path: "/Game/X", phase });
});

test("addStepToExercise uses asset_path + phase_index + step", async () => {
  handler = () => ({ status: 200, body: { phase_index: 1, step_index: 0 } });
  const step = { step_id: "S1", required_interaction_tag: "T", step_type: "Grab", time_limit_seconds: 0 };
  await ue5.addStepToExercise("/Game/X", 1, step);
  assert.deepEqual(requestLog[0].body, { asset_path: "/Game/X", phase_index: 1, step });
});

test("getExercise encodes the id as a query param", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.getExercise("ADL_01_JamSandwich");
  assert.equal(requestLog[0].path, "/adl/exercise/get");
  assert.equal(requestLog[0].query.id, "ADL_01_JamSandwich");
});

// ---------------------------------------------------------------------------
// Blueprint / editor wrappers
// ---------------------------------------------------------------------------

test("createBlueprint sends parent_class + asset_path", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.createBlueprint("AActor", "/Game/BP_Thing");
  assert.equal(requestLog[0].path, "/editor/create_blueprint");
  assert.deepEqual(requestLog[0].body, { parent_class: "AActor", asset_path: "/Game/BP_Thing" });
});

test("setBlueprintDefaults sends properties array untouched", async () => {
  handler = () => ({ status: 200, body: {} });
  const props = [{ name: "A", value: "1" }, { name: "B", value: "two" }];
  await ue5.setBlueprintDefaults("/Game/BP_Thing", props);
  assert.deepEqual(requestLog[0].body, { asset_path: "/Game/BP_Thing", properties: props });
});

test("reparentBlueprint sends asset_path + new_parent", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.reparentBlueprint("/Game/BP_Thing", "UMyWidget");
  assert.deepEqual(requestLog[0].body, { asset_path: "/Game/BP_Thing", new_parent: "UMyWidget" });
});

test("configureIMC sends imc_path + mappings", async () => {
  handler = () => ({ status: 200, body: { mappings_added: 4 } });
  const mappings = [{ action_path: "/Game/IA_Move", key: "D", modifiers: [] }];
  await ue5.configureIMC("/Game/IMC_Default", mappings);
  assert.equal(requestLog[0].path, "/input/configure_imc");
  assert.deepEqual(requestLog[0].body, { imc_path: "/Game/IMC_Default", mappings });
});

test("spawnActorInLevel fills defaults for rotation + label when omitted", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.spawnActorInLevel("APlayerStart", { x: 0, y: 0, z: 0 });
  assert.deepEqual(requestLog[0].body, {
    actor_class: "APlayerStart",
    location: { x: 0, y: 0, z: 0 },
    rotation: { pitch: 0, yaw: 0, roll: 0 },
    label: "",
  });
});

test("spawnActorInLevel omits static_mesh key when falsy", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.spawnActorInLevel("APlayerStart", { x: 0, y: 0, z: 0 }, undefined, "Start", undefined);
  assert.equal(requestLog[0].body.static_mesh, undefined);
});

test("spawnActorInLevel forwards static_mesh when provided", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.spawnActorInLevel(
    "AStaticMeshActor",
    { x: 1, y: 2, z: 3 },
    { pitch: 0, yaw: 90, roll: 0 },
    "Cube",
    "/Engine/BasicShapes/Cube",
  );
  assert.equal(requestLog[0].body.static_mesh, "/Engine/BasicShapes/Cube");
  assert.equal(requestLog[0].body.label, "Cube");
  assert.deepEqual(requestLog[0].body.rotation, { pitch: 0, yaw: 90, roll: 0 });
});

test("listActors appends ?class_filter when provided", async () => {
  handler = () => ({ status: 200, body: { actors: [] } });
  await ue5.listActors("AStaticMeshActor");
  assert.equal(requestLog[0].path, "/editor/list_actors");
  assert.equal(requestLog[0].query.class_filter, "AStaticMeshActor");
});

test("listActors omits query string when no filter", async () => {
  handler = () => ({ status: 200, body: { actors: [] } });
  await ue5.listActors();
  assert.equal(requestLog[0].query.class_filter, undefined);
});

test("updateActor only includes fields that were provided", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.updateActor("MyActor", { location: { x: 1, y: 2, z: 3 } });
  assert.deepEqual(requestLog[0].body, {
    actor_label: "MyActor",
    location: { x: 1, y: 2, z: 3 },
  });
  // Confirm nothing else leaked in
  assert.equal(requestLog[0].body.rotation, undefined);
  assert.equal(requestLog[0].body.new_label, undefined);
  assert.equal(requestLog[0].body.component_properties, undefined);
});

// ---------------------------------------------------------------------------
// Niagara / mesh / BP graph — smoke-check that wrappers hit the right path
// ---------------------------------------------------------------------------

test("createNiagaraSystem includes template_path only when truthy", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.createNiagaraSystem({ asset_path: "/Game/NS_X" });
  assert.deepEqual(requestLog[0].body, { asset_path: "/Game/NS_X" });

  await ue5.createNiagaraSystem({ asset_path: "/Game/NS_X", template_path: "/Game/NS_Tmpl" });
  assert.deepEqual(requestLog[1].body, { asset_path: "/Game/NS_X", template_path: "/Game/NS_Tmpl" });
});

test("configureNiagaraSystem omits unset fields (spread preserves only != null)", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.configureNiagaraSystem({ asset_path: "/Game/NS_X", spawn_count: 10 });
  assert.equal(requestLog[0].body.asset_path, "/Game/NS_X");
  assert.equal(requestLog[0].body.spawn_count, 10);
  assert.equal(requestLog[0].body.lifetime_min, undefined);
  assert.equal(requestLog[0].body.color_r, undefined);
});

test("meshCreate wraps the handle in the body", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.meshCreate("h1");
  assert.equal(requestLog[0].path, "/mesh/create");
  assert.deepEqual(requestLog[0].body, { handle: "h1" });
});

test("bpReadGraph GETs with blueprint + optional graph in query", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.bpReadGraph({ blueprint: "/Game/BP_Foo", graph: "EventGraph" });
  assert.equal(requestLog[0].method, "GET");
  assert.equal(requestLog[0].path, "/bp/read_graph");
  assert.equal(requestLog[0].query.blueprint, "/Game/BP_Foo");
  assert.equal(requestLog[0].query.graph, "EventGraph");
});

test("bpCompile passes save flag only when provided", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.bpCompile({ blueprint: "/Game/BP_Foo" });
  assert.deepEqual(requestLog[0].body, { blueprint: "/Game/BP_Foo" });

  await ue5.bpCompile({ blueprint: "/Game/BP_Foo", save: true });
  assert.deepEqual(requestLog[1].body, { blueprint: "/Game/BP_Foo", save: true });
});

test("execPython defaults mode to execute_file", async () => {
  handler = () => ({ status: 200, body: { log: [] } });
  await ue5.execPython("print('hi')");
  assert.deepEqual(requestLog[0].body, { command: "print('hi')", mode: "execute_file" });

  await ue5.execPython("1+1", "evaluate_statement");
  assert.equal(requestLog[1].body.mode, "evaluate_statement");
});

// ---------------------------------------------------------------------------
// importAsset
// ---------------------------------------------------------------------------

test("importAsset sends source_path + dest_path and optional asset_name", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.importAsset("C:/x.wav", "/Game/Audio");
  assert.deepEqual(requestLog[0].body, { source_path: "C:/x.wav", dest_path: "/Game/Audio" });

  await ue5.importAsset("C:/x.wav", "/Game/Audio", "Renamed");
  assert.deepEqual(requestLog[1].body, {
    source_path: "C:/x.wav",
    dest_path:   "/Game/Audio",
    asset_name:  "Renamed",
  });
});

// ---------------------------------------------------------------------------
// Remaining wrappers — one test each verifies path + body shape.
// ---------------------------------------------------------------------------

test("reimportAsset POSTs /editor/reimport with path", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.reimportAsset("/Game/X");
  assert.equal(requestLog[0].path, "/editor/reimport");
  assert.equal(requestLog[0].method, "POST");
  assert.deepEqual(requestLog[0].body, { path: "/Game/X" });
});

test("listGameplayTags GETs /gameplay_tags/list", async () => {
  handler = () => ({ status: 200, body: { tags: [] } });
  await ue5.listGameplayTags();
  assert.equal(requestLog[0].method, "GET");
  assert.equal(requestLog[0].path, "/gameplay_tags/list");
});

test("createWidgetBlueprint includes parent_class, asset_path, and widgets", async () => {
  handler = () => ({ status: 200, body: {} });
  const widgets = [{ type: "Button", name: "Btn_Play" }];
  await ue5.createWidgetBlueprint("UMainMenuWidget", "/Game/UI/WBP_Main", widgets);
  assert.equal(requestLog[0].path, "/editor/create_widget_blueprint");
  assert.deepEqual(requestLog[0].body, {
    parent_class: "UMainMenuWidget",
    asset_path:   "/Game/UI/WBP_Main",
    widgets,
  });
});

test("setWorldSettings forwards the settings object verbatim", async () => {
  handler = () => ({ status: 200, body: {} });
  const settings = {
    game_mode_class:         "/Game/BP_GameMode.BP_GameMode_C",
    player_controller_class: "/Game/BP_PC.BP_PC_C",
    default_pawn_class:      "/Game/BP_Pawn.BP_Pawn_C",
  };
  await ue5.setWorldSettings(settings);
  assert.equal(requestLog[0].path, "/editor/set_world_settings");
  assert.deepEqual(requestLog[0].body, settings);
});

test("setComponentDefaults sends asset_path, component_name, properties", async () => {
  handler = () => ({ status: 200, body: {} });
  const props = [{ name: "Intensity", value: "1000.0" }];
  await ue5.setComponentDefaults("/Game/BP_X", "DirLight", props);
  assert.equal(requestLog[0].path, "/editor/set_component_defaults");
  assert.deepEqual(requestLog[0].body, {
    asset_path:     "/Game/BP_X",
    component_name: "DirLight",
    properties:     props,
  });
});

test("addComponentToBlueprint defaults component_name to '' and properties to []", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.addComponentToBlueprint("/Game/BP_X", "UStaticMeshComponent");
  assert.deepEqual(requestLog[0].body, {
    asset_path:      "/Game/BP_X",
    component_class: "UStaticMeshComponent",
    component_name:  "",
    properties:      [],
    attach_parent:   "",
  });
});

test("addComponentToBlueprint forwards name and properties when provided", async () => {
  handler = () => ({ status: 200, body: {} });
  const props = [{ name: "Mobility", value: "Static" }];
  await ue5.addComponentToBlueprint("/Game/BP_X", "UStaticMeshComponent", "Mesh", props);
  assert.deepEqual(requestLog[0].body, {
    asset_path:      "/Game/BP_X",
    component_class: "UStaticMeshComponent",
    component_name:  "Mesh",
    properties:      props,
    attach_parent:   "",
  });
});

test("addComponentToBlueprint forwards attach_parent when provided", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.addComponentToBlueprint("/Game/BP_X", "UCameraComponent", "Camera", [], "SpringArm");
  assert.deepEqual(requestLog[0].body, {
    asset_path:      "/Game/BP_X",
    component_class: "UCameraComponent",
    component_name:  "Camera",
    properties:      [],
    attach_parent:   "SpringArm",
  });
});

test("openLevel POSTs level_path to /editor/open_level", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.openLevel("/Game/Maps/L_Arena");
  assert.equal(requestLog[0].path, "/editor/open_level");
  assert.deepEqual(requestLog[0].body, { level_path: "/Game/Maps/L_Arena" });
});

test("createLevel POSTs level_path to /editor/create_level", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.createLevel("/Game/Maps/L_New");
  assert.equal(requestLog[0].path, "/editor/create_level");
  assert.deepEqual(requestLog[0].body, { level_path: "/Game/Maps/L_New" });
});

test("createLevel forwards partitioned when asked for a World Partition map", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.createLevel("/Game/Maps/L_Open", { partitioned: true });
  assert.deepEqual(requestLog[0].body, { level_path: "/Game/Maps/L_Open", partitioned: true });
});

test("createLevel omits partitioned rather than sending false by default", async () => {
  // An older plugin build ignores the field, but sending it unasked would make
  // the request differ from what every existing caller produces.
  handler = () => ({ status: 200, body: {} });
  await ue5.createLevel("/Game/Maps/L_Plain");
  assert.deepEqual(requestLog[0].body, { level_path: "/Game/Maps/L_Plain" });
});

test("deleteActor POSTs actor_label to /editor/delete_actor", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.deleteActor("MyCube");
  assert.equal(requestLog[0].path, "/editor/delete_actor");
  assert.deepEqual(requestLog[0].body, { actor_label: "MyCube" });
});

test("updateActor includes all four optional fields when provided", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.updateActor("A1", {
    location: { x: 1, y: 2, z: 3 },
    rotation: { pitch: 0, yaw: 45, roll: 0 },
    newLabel: "A1_Renamed",
    componentProperties: [{ component: "Mesh", properties: [{ name: "Mobility", value: "Movable" }] }],
  });
  assert.deepEqual(requestLog[0].body, {
    actor_label:          "A1",
    location:             { x: 1, y: 2, z: 3 },
    rotation:             { pitch: 0, yaw: 45, roll: 0 },
    new_label:            "A1_Renamed",
    component_properties: [{ component: "Mesh", properties: [{ name: "Mobility", value: "Movable" }] }],
  });
});

test("styleWidgets POSTs widget_blueprint + styles array", async () => {
  handler = () => ({ status: 200, body: {} });
  const styles = [{ widget_name: "Title", font_size: 42, color: "#FFAA00" }];
  await ue5.styleWidgets("/Game/UI/WBP_Main", styles);
  assert.equal(requestLog[0].path, "/editor/style_widgets");
  assert.deepEqual(requestLog[0].body, { widget_blueprint: "/Game/UI/WBP_Main", styles });
});

test("setNiagaraEmitterParams only forwards provided fields", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.setNiagaraEmitterParams({
    asset_path:    "/Game/VFX/NS_X",
    emitter_index: 0,
    list_only:     true,
  });
  assert.equal(requestLog[0].path, "/editor/set_niagara_emitter_params");
  assert.deepEqual(requestLog[0].body, {
    asset_path:    "/Game/VFX/NS_X",
    emitter_index: 0,
    list_only:     true,
  });
});

test("setNiagaraEmitterParams passes raw_params array through", async () => {
  handler = () => ({ status: 200, body: {} });
  const raw = [{ name: "Constants.Module.SpawnCount", value: 100 }];
  await ue5.setNiagaraEmitterParams({ asset_path: "/Game/VFX/NS_X", raw_params: raw });
  assert.deepEqual(requestLog[0].body.raw_params, raw);
});

test("addWidgetToBlueprint POSTs widget_blueprint + widgets", async () => {
  handler = () => ({ status: 200, body: {} });
  const widgets = [{ type: "TextBlock", name: "Lbl_Score" }];
  await ue5.addWidgetToBlueprint("/Game/UI/WBP_HUD", widgets);
  assert.equal(requestLog[0].path, "/editor/add_widget_to_blueprint");
  assert.deepEqual(requestLog[0].body, { widget_blueprint: "/Game/UI/WBP_HUD", widgets });
});

test("configureAnimBlueprint sends asset_path, states, transitions", async () => {
  handler = () => ({ status: 200, body: {} });
  const states      = [{ name: "Idle", animation: "/Game/Anim_Idle" }];
  const transitions = [{ from: "Idle", to: "Run", condition: "bIsRunning" }];
  await ue5.configureAnimBlueprint("/Game/ABP_Char", states, transitions);
  assert.equal(requestLog[0].path, "/editor/configure_anim_blueprint");
  assert.deepEqual(requestLog[0].body, { asset_path: "/Game/ABP_Char", states, transitions });
});

test("setLevelEnvironment spreads options on top of level_path", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.setLevelEnvironment("/Game/Maps/L_X", {
    fog:               { density: 0.1 },
    directional_light: { intensity: 3.0 },
  });
  assert.equal(requestLog[0].path, "/editor/set_level_environment");
  assert.deepEqual(requestLog[0].body, {
    level_path:        "/Game/Maps/L_X",
    fog:               { density: 0.1 },
    directional_light: { intensity: 3.0 },
  });
});

test("createMaterialInstance omits optional fields when absent", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.createMaterialInstance({
    asset_path:      "/Game/Mat/MI_X",
    parent_material: "/Game/Mat/M_Parent",
  });
  assert.equal(requestLog[0].path, "/editor/create_material_instance");
  assert.deepEqual(requestLog[0].body, {
    asset_path:      "/Game/Mat/MI_X",
    parent_material: "/Game/Mat/M_Parent",
  });
});

test("createMaterialInstance forwards scalar/vector overrides and BP application", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.createMaterialInstance({
    asset_path:      "/Game/Mat/MI_X",
    parent_material: "/Game/Mat/M_Parent",
    scalar_params:   [{ name: "Rough", value: 0.2 }],
    vector_params:   [{ name: "Tint",  value: { r: 1, g: 0.5, b: 0.2, a: 1 } }],
    blueprint_path:  "/Game/BP_X",
    component_name:  "Mesh",
  });
  const body = requestLog[0].body;
  assert.equal(body.scalar_params[0].name, "Rough");
  assert.equal(body.vector_params[0].value.r, 1);
  assert.equal(body.blueprint_path, "/Game/BP_X");
  assert.equal(body.component_name, "Mesh");
});

test("createMaterialInstance forwards texture_params when provided", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.createMaterialInstance({
    asset_path:      "/Game/Mat/MI_Tex",
    parent_material: "/Game/Mat/M_Parent",
    texture_params:  [{ name: "Albedo", texture_path: "/Game/Tex/T_Brick" }],
  });
  const body = requestLog[0].body;
  assert.deepEqual(body.texture_params, [{ name: "Albedo", texture_path: "/Game/Tex/T_Brick" }]);
});

test("createMaterialInstance omits texture_params when absent", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.createMaterialInstance({
    asset_path:      "/Game/Mat/MI_X",
    parent_material: "/Game/Mat/M_Parent",
  });
  assert.ok(!("texture_params" in requestLog[0].body), "texture_params should be omitted when not passed");
});

// ---------------------------------------------------------------------------
// mesh/* composition wrappers
// ---------------------------------------------------------------------------

test("meshAppendPrimitive POSTs handle, shape, and optional transform/params", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.meshAppendPrimitive({ handle: "h", shape: "Box" });
  assert.equal(requestLog[0].path, "/mesh/append_primitive");
  assert.deepEqual(requestLog[0].body, { handle: "h", shape: "Box" });

  await ue5.meshAppendPrimitive({
    handle:    "h",
    shape:     "Sphere",
    transform: { location: { x: 0, y: 0, z: 10 } },
    params:    { radius: 50 },
  });
  assert.deepEqual(requestLog[1].body, {
    handle:    "h",
    shape:     "Sphere",
    transform: { location: { x: 0, y: 0, z: 10 } },
    params:    { radius: 50 },
  });
});

test("meshBoolean sends handle/other_handle/op and optional transform", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.meshBoolean({ handle: "a", other_handle: "b", op: "Subtract" });
  assert.equal(requestLog[0].path, "/mesh/boolean");
  assert.deepEqual(requestLog[0].body, { handle: "a", other_handle: "b", op: "Subtract" });

  await ue5.meshBoolean({
    handle: "a", other_handle: "b", op: "Union",
    transform: { scale: { x: 2, y: 2, z: 2 } },
  });
  assert.equal(requestLog[1].body.transform.scale.x, 2);
});

test("meshTransform only includes provided fields", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.meshTransform({ handle: "h", scale: { x: 1.5, y: 1.5, z: 1.5 } });
  assert.equal(requestLog[0].path, "/mesh/transform");
  assert.deepEqual(requestLog[0].body, { handle: "h", scale: { x: 1.5, y: 1.5, z: 1.5 } });
  assert.equal(requestLog[0].body.location, undefined);
  assert.equal(requestLog[0].body.rotation, undefined);
});

test("meshDeform required fields + optional upper/lower", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.meshDeform({ handle: "h", op: "Twist", axis: "Z", amount: 45 });
  assert.deepEqual(requestLog[0].body, { handle: "h", op: "Twist", axis: "Z", amount: 45 });

  await ue5.meshDeform({ handle: "h", op: "Taper", axis: "Y", amount: 0.5, upper: 1.0, lower: -1.0 });
  assert.equal(requestLog[1].body.upper, 1.0);
  assert.equal(requestLog[1].body.lower, -1.0);
});

test("meshRemesh only includes provided numeric fields", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.meshRemesh({ handle: "h" });
  assert.deepEqual(requestLog[0].body, { handle: "h" });

  await ue5.meshRemesh({ handle: "h", target_edge_length: 4, iterations: 5, smoothing: 0.25 });
  assert.equal(requestLog[1].body.target_edge_length, 4);
  assert.equal(requestLog[1].body.iterations, 5);
  assert.equal(requestLog[1].body.smoothing, 0.25);
});

test("meshBakeStatic requires handle + asset_path", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.meshBakeStatic({ handle: "h", asset_path: "/Game/M_Baked" });
  assert.equal(requestLog[0].path, "/mesh/bake_static");
  assert.deepEqual(requestLog[0].body, { handle: "h", asset_path: "/Game/M_Baked" });
});

test("meshDeleteHandle POSTs handle", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.meshDeleteHandle("h");
  assert.equal(requestLog[0].path, "/mesh/delete_handle");
  assert.deepEqual(requestLog[0].body, { handle: "h" });
});

// ---------------------------------------------------------------------------
// bp/* graph authoring wrappers
// ---------------------------------------------------------------------------

test("bpAddNode sends blueprint + node_class and omits falsy optionals", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.bpAddNode({ blueprint: "/Game/BP_X", node_class: "K2Node_CallFunction" });
  assert.equal(requestLog[0].path, "/bp/add_node");
  assert.deepEqual(requestLog[0].body, {
    blueprint:  "/Game/BP_X",
    node_class: "K2Node_CallFunction",
  });
});

test("bpAddNode includes graph/node_id/config/pin_defaults/position when provided", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.bpAddNode({
    blueprint:    "/Game/BP_X",
    graph:        "EventGraph",
    node_class:   "K2Node_CallFunction",
    node_id:      "n1",
    config:       { func: "Print" },
    pin_defaults: { InString: "Hello" },
    position:     { x: 100, y: 200 },
  });
  const b = requestLog[0].body;
  assert.equal(b.graph, "EventGraph");
  assert.equal(b.node_id, "n1");
  assert.deepEqual(b.config, { func: "Print" });
  assert.deepEqual(b.pin_defaults, { InString: "Hello" });
  assert.deepEqual(b.position, { x: 100, y: 200 });
});

test("bpConnectPins forwards all six fields + optional graph", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.bpConnectPins({
    blueprint: "/Game/BP_X",
    graph:     "EventGraph",
    from_node: "n1", from_pin: "Out",
    to_node:   "n2", to_pin:   "In",
  });
  assert.equal(requestLog[0].path, "/bp/connect_pins");
  assert.deepEqual(requestLog[0].body, {
    blueprint: "/Game/BP_X",
    graph:     "EventGraph",
    from_node: "n1", from_pin: "Out",
    to_node:   "n2", to_pin:   "In",
  });
});

test("bpAddLogic sends nodes/connections/auto_layout/compile/save", async () => {
  handler = () => ({ status: 200, body: {} });
  const nodes       = [{ id: "n1", class: "K2Node_Event" }];
  const connections = [{ from: "n1.Then", to: "n2.Exec" }];
  await ue5.bpAddLogic({
    blueprint: "/Game/BP_X",
    graph:     "EventGraph",
    nodes,
    connections,
    auto_layout: true,
    compile:     true,
    save:        false,
  });
  const b = requestLog[0].body;
  assert.deepEqual(b.nodes, nodes);
  assert.deepEqual(b.connections, connections);
  assert.equal(b.auto_layout, true);
  assert.equal(b.compile, true);
  assert.equal(b.save, false);
});

test("bpAddLogic omits connections/flags when not given", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.bpAddLogic({ blueprint: "/Game/BP_X", nodes: [] });
  const b = requestLog[0].body;
  assert.deepEqual(b, { blueprint: "/Game/BP_X", nodes: [] });
});

test("bpDeleteNode accepts either node_id or node_guid", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.bpDeleteNode({ blueprint: "/Game/BP_X", node_id: "n1" });
  assert.deepEqual(requestLog[0].body, { blueprint: "/Game/BP_X", node_id: "n1" });

  await ue5.bpDeleteNode({
    blueprint: "/Game/BP_X",
    node_guid: "8C016DD4-4CED-352B-33E4-A6A63B2B6179",
    compile:   false,
    save:      false,
  });
  assert.equal(requestLog[1].body.node_guid, "8C016DD4-4CED-352B-33E4-A6A63B2B6179");
  assert.equal(requestLog[1].body.compile, false);
  assert.equal(requestLog[1].body.save, false);
});

test("bpReadGraph omits ?graph= when graph not specified", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.bpReadGraph({ blueprint: "/Game/BP_X" });
  assert.equal(requestLog[0].path, "/bp/read_graph");
  assert.equal(requestLog[0].query.blueprint, "/Game/BP_X");
  assert.equal(requestLog[0].query.graph, undefined);
});

test("bpLint forwards auto_fix/compile/save when provided", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.bpLint({ blueprint: "/Game/BP_X", auto_fix: true, compile: true, save: false });
  assert.equal(requestLog[0].path, "/bp/lint");
  assert.deepEqual(requestLog[0].body, {
    blueprint: "/Game/BP_X",
    auto_fix:  true,
    compile:   true,
    save:      false,
  });
});

test("bpLint requires only blueprint when flags omitted", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.bpLint({ blueprint: "/Game/BP_X" });
  assert.deepEqual(requestLog[0].body, { blueprint: "/Game/BP_X" });
});

test("bpLintProject defaults path_prefix elision when not provided", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.bpLintProject({ auto_fix: true });
  assert.equal(requestLog[0].path, "/bp/lint_project");
  assert.deepEqual(requestLog[0].body, { auto_fix: true });

  await ue5.bpLintProject({ path_prefix: "/Game/Core", compile: true, save: true });
  assert.deepEqual(requestLog[1].body, { path_prefix: "/Game/Core", compile: true, save: true });
});

// ---------------------------------------------------------------------------
// Trivial / utility helpers
// ---------------------------------------------------------------------------

test("getBridgeUrl returns the active bridge URL from env", async () => {
  // serverUrl is set into NGG_BRIDGE_URL by `before()`, so we should read that back
  assert.equal(ue5.getBridgeUrl(), serverUrl);
});

test("getProjectInfo GETs /project_info and parses JSON", async () => {
  handler = () => ({ status: 200, body: { project_name: "ThrowBomb", engine_root: "C:/UE_5.4" } });
  const result = await ue5.getProjectInfo();
  assert.equal(result.project_name, "ThrowBomb");
  assert.equal(requestLog[0].method, "GET");
  assert.equal(requestLog[0].path, "/project_info");
});

// ---------------------------------------------------------------------------
// removeComponentFromBlueprint
// ---------------------------------------------------------------------------

test("removeComponentFromBlueprint POSTs asset_path + component_name", async () => {
  handler = () => ({ status: 200, body: { success: true } });
  await ue5.removeComponentFromBlueprint("/Game/BP_X", "Sphere");
  assert.equal(requestLog[0].method, "POST");
  assert.equal(requestLog[0].path, "/editor/remove_component_from_blueprint");
  assert.deepEqual(requestLog[0].body, {
    asset_path: "/Game/BP_X",
    component_name: "Sphere",
  });
});

// ---------------------------------------------------------------------------
// Widget tree manipulation (get/remove/reparent/rename/compile)
// ---------------------------------------------------------------------------

test("getWidgetTree POSTs widget_blueprint", async () => {
  handler = () => ({ status: 200, body: { widget_blueprint: "/Game/UI/WBP_HUD", root: {} } });
  await ue5.getWidgetTree("/Game/UI/WBP_HUD");
  assert.equal(requestLog[0].path, "/editor/get_widget_tree");
  assert.deepEqual(requestLog[0].body, { widget_blueprint: "/Game/UI/WBP_HUD" });
});

test("removeWidgetFromBlueprint omits cascade when undefined and forwards it when set", async () => {
  handler = () => ({ status: 200, body: { success: true } });
  await ue5.removeWidgetFromBlueprint("/Game/UI/WBP_HUD", "HPBar");
  assert.deepEqual(requestLog[0].body, {
    widget_blueprint: "/Game/UI/WBP_HUD",
    widget_name: "HPBar",
  });

  await ue5.removeWidgetFromBlueprint("/Game/UI/WBP_HUD", "HPBar", false);
  assert.deepEqual(requestLog[1].body, {
    widget_blueprint: "/Game/UI/WBP_HUD",
    widget_name: "HPBar",
    cascade: false,
  });
});

test("reparentWidget omits child_index when undefined and forwards when provided", async () => {
  handler = () => ({ status: 200, body: { success: true } });
  await ue5.reparentWidget("/Game/UI/WBP_HUD", "HPBar", "RootCanvas");
  assert.deepEqual(requestLog[0].body, {
    widget_blueprint: "/Game/UI/WBP_HUD",
    widget_name: "HPBar",
    new_parent: "RootCanvas",
  });

  await ue5.reparentWidget("/Game/UI/WBP_HUD", "HPBar", "RootCanvas", 0);
  assert.equal(requestLog[1].body.child_index, 0);
});

test("renameWidget POSTs the rename payload", async () => {
  handler = () => ({ status: 200, body: { success: true } });
  await ue5.renameWidget("/Game/UI/WBP_HUD", "HPBar", "HealthBar");
  assert.equal(requestLog[0].path, "/editor/rename_widget");
  assert.deepEqual(requestLog[0].body, {
    widget_blueprint: "/Game/UI/WBP_HUD",
    old_name: "HPBar",
    new_name: "HealthBar",
  });
});

test("compileWidgetBlueprint POSTs widget_blueprint", async () => {
  handler = () => ({ status: 200, body: { success: true, status: "OK" } });
  await ue5.compileWidgetBlueprint("/Game/UI/WBP_HUD");
  assert.equal(requestLog[0].path, "/editor/compile_widget_blueprint");
  assert.deepEqual(requestLog[0].body, { widget_blueprint: "/Game/UI/WBP_HUD" });
});

// ---------------------------------------------------------------------------
// createMaterial — full kitchen sink + omission tests
// ---------------------------------------------------------------------------

test("createMaterial sends only asset_path when no optional fields given", async () => {
  handler = () => ({ status: 200, body: { success: true } });
  await ue5.createMaterial({ asset_path: "/Game/Materials/M_Foo" });
  assert.equal(requestLog[0].path, "/editor/create_material");
  assert.deepEqual(requestLog[0].body, { asset_path: "/Game/Materials/M_Foo" });
});

test("createMaterial forwards every optional field when provided", async () => {
  handler = () => ({ status: 200, body: { success: true } });
  await ue5.createMaterial({
    asset_path: "/Game/Materials/M_Foo",
    base_color: { r: 0.5, g: 0.5, b: 0.5 },
    use_parameters: true,
    pixel_art: false,
    texture_path: "/Game/Tex/T_X",
    brightness_default: 1.5,
    blueprint_path: "/Game/BP_Y",
    component_name: "Mesh",
  });
  assert.deepEqual(requestLog[0].body, {
    asset_path: "/Game/Materials/M_Foo",
    base_color: { r: 0.5, g: 0.5, b: 0.5 },
    use_parameters: true,
    pixel_art: false,
    texture_path: "/Game/Tex/T_X",
    brightness_default: 1.5,
    blueprint_path: "/Game/BP_Y",
    component_name: "Mesh",
  });
});

// ---------------------------------------------------------------------------
// readMaterial — inspection getter
// ---------------------------------------------------------------------------

test("readMaterial POSTs asset_path to /editor/read_material", async () => {
  handler = () => ({ status: 200, body: { success: true, class: "Material", scalar_params: [] } });
  const r = await ue5.readMaterial({ asset_path: "/Game/Materials/M_Foo" });
  assert.equal(requestLog[0].method, "POST");
  assert.equal(requestLog[0].path, "/editor/read_material");
  assert.deepEqual(requestLog[0].body, { asset_path: "/Game/Materials/M_Foo" });
  assert.equal(r.class, "Material");
});

// ---------------------------------------------------------------------------
// createColorCurve / createFloatCurve / readCurve
// ---------------------------------------------------------------------------

test("createColorCurve sends asset_path + keys and omits linear when absent", async () => {
  handler = () => ({ status: 200, body: { success: true } });
  await ue5.createColorCurve({
    asset_path: "/Game/Curves/Curve_Color",
    keys: [{ time: 0, r: 0, g: 1, b: 0 }, { time: 1, r: 1, g: 0, b: 0 }],
  });
  assert.equal(requestLog[0].path, "/editor/create_color_curve");
  assert.deepEqual(requestLog[0].body, {
    asset_path: "/Game/Curves/Curve_Color",
    keys: [{ time: 0, r: 0, g: 1, b: 0 }, { time: 1, r: 1, g: 0, b: 0 }],
  });
});

test("createFloatCurve sends asset_path + keys and omits linear when absent", async () => {
  handler = () => ({ status: 200, body: { success: true } });
  await ue5.createFloatCurve({
    asset_path: "/Game/Curves/Curve_Intensity",
    keys: [{ time: 0, value: 0 }, { time: 1, value: 1 }],
  });
  assert.equal(requestLog[0].path, "/editor/create_float_curve");
  assert.deepEqual(requestLog[0].body, {
    asset_path: "/Game/Curves/Curve_Intensity",
    keys: [{ time: 0, value: 0 }, { time: 1, value: 1 }],
  });
});

test("createFloatCurve forwards linear:false (not dropped by the != null guard)", async () => {
  handler = () => ({ status: 200, body: { success: true } });
  await ue5.createFloatCurve({
    asset_path: "/Game/Curves/Curve_Stepped",
    keys: [{ time: 0, value: 0 }],
    linear: false,
  });
  assert.equal(requestLog[0].body.linear, false);
});

test("readCurve POSTs asset_path to /editor/read_curve", async () => {
  handler = () => ({ status: 200, body: { success: true, type: "float", keys: [] } });
  const r = await ue5.readCurve({ asset_path: "/Game/Curves/Curve_Intensity" });
  assert.equal(requestLog[0].method, "POST");
  assert.equal(requestLog[0].path, "/editor/read_curve");
  assert.deepEqual(requestLog[0].body, { asset_path: "/Game/Curves/Curve_Intensity" });
  assert.equal(r.type, "float");
});

// ---------------------------------------------------------------------------
// Anim graph splice nodes (8 helpers)
// ---------------------------------------------------------------------------

test("animAddCopyBone sends required source/target and omits unset optionals", async () => {
  handler = () => ({ status: 200, body: { node_id: "n1" } });
  await ue5.animAddCopyBone({
    anim_bp_path: "/Game/Anim/ABP_X",
    source_bone:  "hand_l",
    target_bone:  "hand_r",
  });
  assert.equal(requestLog[0].path, "/editor/anim/add_copy_bone");
  assert.deepEqual(requestLog[0].body, {
    anim_bp_path: "/Game/Anim/ABP_X",
    source_bone:  "hand_l",
    target_bone:  "hand_r",
  });
});

test("animAddCopyBone forwards rotation/scale/space/offsets when provided", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.animAddCopyBone({
    anim_bp_path: "/Game/Anim/ABP_X",
    source_bone:  "a",
    target_bone:  "b",
    copy_translation: true,
    copy_rotation:    false,
    copy_scale:       true,
    control_space:    "ComponentSpace",
    node_offset_x:    100,
    node_offset_y:    200,
  });
  assert.deepEqual(requestLog[0].body, {
    anim_bp_path: "/Game/Anim/ABP_X",
    source_bone:  "a",
    target_bone:  "b",
    copy_translation: true,
    copy_rotation:    false,
    copy_scale:       true,
    control_space:    "ComponentSpace",
    node_offset_x:    100,
    node_offset_y:    200,
  });
});

test("animAddHandIKRetargeting requires the four hand bones, omits unset optionals", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.animAddHandIKRetargeting({
    anim_bp_path: "/Game/Anim/ABP_X",
    right_hand_fk: "hand_r",
    left_hand_fk:  "hand_l",
    right_hand_ik: "ik_hand_r",
    left_hand_ik:  "ik_hand_l",
  });
  assert.equal(requestLog[0].path, "/editor/anim/add_hand_ik_retargeting");
  assert.deepEqual(requestLog[0].body, {
    anim_bp_path: "/Game/Anim/ABP_X",
    right_hand_fk: "hand_r",
    left_hand_fk:  "hand_l",
    right_hand_ik: "ik_hand_r",
    left_hand_ik:  "ik_hand_l",
  });
});

test("animAddHandIKRetargeting forwards all optional fields when set", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.animAddHandIKRetargeting({
    anim_bp_path: "/Game/Anim/ABP_X",
    right_hand_fk: "a", left_hand_fk: "b", right_hand_ik: "c", left_hand_ik: "d",
    ik_bones_to_move: ["ik_hand_r"],
    hand_fk_weight:   0.5,
    per_axis_alpha:   { x: 1, y: 0, z: 1 },
    alpha_curve_name: "RetargetAlpha",
    alpha_scale:      1.0,
    alpha_bias:       0.0,
    node_offset_x:    10,
    node_offset_y:    20,
  });
  const b = requestLog[0].body;
  assert.equal(b.hand_fk_weight, 0.5);
  assert.deepEqual(b.ik_bones_to_move, ["ik_hand_r"]);
  assert.deepEqual(b.per_axis_alpha, { x: 1, y: 0, z: 1 });
  assert.equal(b.alpha_curve_name, "RetargetAlpha");
});

test("skeletonAddVirtualBone POSTs skeleton/source/target and omits vb_name", async () => {
  handler = () => ({ status: 200, body: { success: true } });
  await ue5.skeletonAddVirtualBone({
    skeleton_path: "/Game/Anim/SK_Hero_Skeleton",
    source_bone: "hand_l",
    target_bone: "hand_r",
  });
  assert.equal(requestLog[0].path, "/editor/skeleton/add_virtual_bone");
  assert.deepEqual(requestLog[0].body, {
    skeleton_path: "/Game/Anim/SK_Hero_Skeleton",
    source_bone: "hand_l",
    target_bone: "hand_r",
  });

  await ue5.skeletonAddVirtualBone({
    skeleton_path: "/Game/SK", source_bone: "a", target_bone: "b", vb_name: "VB_Custom",
  });
  assert.equal(requestLog[1].body.vb_name, "VB_Custom");
});

test("animAddTwoBoneIK only requires anim_bp + ik_bone; optionals are omitted", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.animAddTwoBoneIK({ anim_bp_path: "/Game/Anim/ABP_X", ik_bone: "hand_l" });
  assert.equal(requestLog[0].path, "/editor/anim/add_two_bone_ik");
  assert.deepEqual(requestLog[0].body, {
    anim_bp_path: "/Game/Anim/ABP_X",
    ik_bone: "hand_l",
  });
});

test("animAddTwoBoneIK forwards space + offset overrides", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.animAddTwoBoneIK({
    anim_bp_path: "/Game/Anim/ABP_X",
    ik_bone: "hand_l",
    effector_location_space: "BoneSpace",
    joint_target_location_space: "ComponentSpace",
    take_rotation_from_effector: true,
    effector_target_bone: "ik_target_l",
    joint_target_bone: "elbow_target_l",
    joint_target_offset: { x: 0, y: 10, z: 0 },
    node_offset_x: 5,
    node_offset_y: -5,
  });
  const b = requestLog[0].body;
  assert.equal(b.effector_location_space, "BoneSpace");
  assert.equal(b.take_rotation_from_effector, true);
  assert.deepEqual(b.joint_target_offset, { x: 0, y: 10, z: 0 });
});

test("animAddLayeredBoneBlend only requires anim_bp_path", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.animAddLayeredBoneBlend({ anim_bp_path: "/Game/Anim/ABP_X" });
  assert.equal(requestLog[0].path, "/editor/anim/add_layered_bone_blend");
  assert.deepEqual(requestLog[0].body, { anim_bp_path: "/Game/Anim/ABP_X" });
});

test("animAddLayeredBoneBlend forwards branch_filters/blend_weights/etc", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.animAddLayeredBoneBlend({
    anim_bp_path: "/Game/Anim/ABP_X",
    graph: "AnimGraph",
    branch_filters: [{ bone_name: "spine_03", blend_depth: 1 }],
    blend_weights: [1.0],
    blend_mode: "BranchFilter",
    alpha: 0.75,
    splice_before_output: true,
    node_offset_x: 0,
    node_offset_y: 0,
  });
  const b = requestLog[0].body;
  assert.equal(b.graph, "AnimGraph");
  assert.deepEqual(b.branch_filters, [{ bone_name: "spine_03", blend_depth: 1 }]);
  assert.equal(b.alpha, 0.75);
  assert.equal(b.splice_before_output, true);
});

test("animAddSequencePlayer always includes sequence even when not provided", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.animAddSequencePlayer({ anim_bp_path: "/Game/Anim/ABP_X", sequence: "/Game/Anim/A_Run" });
  assert.equal(requestLog[0].path, "/editor/anim/add_sequence_player");
  assert.deepEqual(requestLog[0].body, {
    anim_bp_path: "/Game/Anim/ABP_X",
    sequence:     "/Game/Anim/A_Run",
  });

  await ue5.animAddSequencePlayer({
    anim_bp_path: "/Game/Anim/ABP_X",
    sequence: "/Game/Anim/A_Run",
    graph: "AnimGraph",
    loop: false,
    play_rate: 1.5,
    start_position: 0.25,
    node_offset_x: 1,
    node_offset_y: 2,
  });
  const b = requestLog[1].body;
  assert.equal(b.loop, false);
  assert.equal(b.play_rate, 1.5);
  assert.equal(b.start_position, 0.25);
});

test("animAddModifyBone POSTs bone_name + anim_bp_path, omitting unset optionals", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.animAddModifyBone({ anim_bp_path: "/Game/Anim/ABP_X", bone_name: "head" });
  assert.deepEqual(requestLog[0].body, {
    anim_bp_path: "/Game/Anim/ABP_X",
    bone_name:    "head",
  });
});

test("animAddModifyBone forwards modes + transforms when set", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.animAddModifyBone({
    anim_bp_path: "/Game/Anim/ABP_X",
    bone_name: "head",
    translation_mode: "Add",
    rotation_mode:    "Replace",
    scale_mode:       "Ignore",
    translation_space: "BoneSpace",
    rotation_space:    "ComponentSpace",
    scale_space:       "WorldSpace",
    translation: { x: 0, y: 0, z: 1 },
    rotation:    { pitch: 0, yaw: 90, roll: 0 },
    scale:       { x: 1, y: 1, z: 1 },
    alpha:       1.0,
    splice_before_output: true,
    node_offset_x: 0,
    node_offset_y: 0,
  });
  const b = requestLog[0].body;
  assert.equal(b.translation_mode, "Add");
  assert.equal(b.rotation_space, "ComponentSpace");
  assert.deepEqual(b.rotation, { pitch: 0, yaw: 90, roll: 0 });
  assert.equal(b.alpha, 1.0);
});

test("animAddLookAt only requires anim_bp_path + bone_to_modify", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.animAddLookAt({ anim_bp_path: "/Game/Anim/ABP_X", bone_to_modify: "head" });
  assert.equal(requestLog[0].path, "/editor/anim/add_look_at");
  assert.deepEqual(requestLog[0].body, {
    anim_bp_path:    "/Game/Anim/ABP_X",
    bone_to_modify:  "head",
  });
});

test("animAddLookAt forwards axis/target/socket/clamp/interp when set", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.animAddLookAt({
    anim_bp_path: "/Game/Anim/ABP_X",
    bone_to_modify: "head",
    look_at_axis: "X",
    look_at_axis_local: true,
    look_at_target_bone: "weapon",
    look_at_socket: "Muzzle",
    look_at_location: { x: 0, y: 0, z: 100 },
    look_at_location_space: "WorldSpace",
    look_at_clamp: 45.0,
    interpolation_type: "EaseInOut",
    interpolation_time: 0.2,
    alpha: 0.5,
    splice_before_output: false,
    node_offset_x: 1,
    node_offset_y: 2,
  });
  const b = requestLog[0].body;
  assert.equal(b.look_at_axis, "X");
  assert.equal(b.look_at_axis_local, true);
  assert.equal(b.look_at_socket, "Muzzle");
  assert.deepEqual(b.look_at_location, { x: 0, y: 0, z: 100 });
  assert.equal(b.interpolation_type, "EaseInOut");
});

test("animAddAimOffsetBlendSpace requires anim_bp_path + blend_space", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.animAddAimOffsetBlendSpace({
    anim_bp_path: "/Game/Anim/ABP_X",
    blend_space:  "/Game/Anim/AO_Aim",
  });
  assert.equal(requestLog[0].path, "/editor/anim/add_aim_offset_blend_space");
  assert.deepEqual(requestLog[0].body, {
    anim_bp_path: "/Game/Anim/ABP_X",
    blend_space:  "/Game/Anim/AO_Aim",
  });

  await ue5.animAddAimOffsetBlendSpace({
    anim_bp_path: "/Game/Anim/ABP_X",
    blend_space:  "/Game/Anim/AO_Aim",
    graph: "AnimGraph",
    alpha: 0.5,
    splice_before_output: true,
    node_offset_x: 1, node_offset_y: 2,
  });
  const b = requestLog[1].body;
  assert.equal(b.graph, "AnimGraph");
  assert.equal(b.alpha, 0.5);
  assert.equal(b.splice_before_output, true);
});

test("animDeleteNode forwards reconnect_pose when provided", async () => {
  handler = () => ({ status: 200, body: { success: true } });
  await ue5.animDeleteNode({ anim_bp_path: "/Game/Anim/ABP_X", node_id: "n_abc" });
  assert.equal(requestLog[0].path, "/editor/anim/delete_node");
  assert.deepEqual(requestLog[0].body, {
    anim_bp_path: "/Game/Anim/ABP_X",
    node_id:      "n_abc",
  });

  await ue5.animDeleteNode({ anim_bp_path: "/Game/Anim/ABP_X", node_id: "n_abc", reconnect_pose: false });
  assert.equal(requestLog[1].body.reconnect_pose, false);
});

// ---------------------------------------------------------------------------
// meshAddSocket
// ---------------------------------------------------------------------------

test("meshAddSocket POSTs only required fields by default", async () => {
  handler = () => ({ status: 200, body: { success: true } });
  await ue5.meshAddSocket({ mesh_path: "/Game/SM_X", socket_name: "Muzzle" });
  assert.equal(requestLog[0].path, "/mesh/add_socket");
  assert.deepEqual(requestLog[0].body, {
    mesh_path:   "/Game/SM_X",
    socket_name: "Muzzle",
  });
});

test("meshAddSocket forwards bone/location/rotation/scale/replace/target when provided", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.meshAddSocket({
    mesh_path:   "/Game/SK_Hero",
    socket_name: "Hand_L_Socket",
    bone_name:   "hand_l",
    location:    { x: 0, y: 0, z: 0 },
    rotation:    { pitch: 0, yaw: 0, roll: 0 },
    scale:       { x: 1, y: 1, z: 1 },
    replace:     true,
    target:      "Skeleton",
  });
  const b = requestLog[0].body;
  assert.equal(b.bone_name, "hand_l");
  assert.equal(b.replace, true);
  assert.equal(b.target, "Skeleton");
  assert.deepEqual(b.location, { x: 0, y: 0, z: 0 });
});

// ---------------------------------------------------------------------------
// Blueprint asset creation (struct / enum / interface / anim BP)
// ---------------------------------------------------------------------------

test("createBlueprintStruct defaults fields to [] and omits save", async () => {
  handler = () => ({ status: 200, body: { success: true } });
  await ue5.createBlueprintStruct({ asset_path: "/Game/S_Foo" });
  assert.equal(requestLog[0].path, "/asset/create_blueprint_struct");
  assert.deepEqual(requestLog[0].body, { asset_path: "/Game/S_Foo", fields: [] });
});

test("createBlueprintStruct forwards fields + save", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.createBlueprintStruct({
    asset_path: "/Game/S_Foo",
    fields: [{ name: "X", type: "int" }],
    save: true,
  });
  assert.deepEqual(requestLog[0].body, {
    asset_path: "/Game/S_Foo",
    fields: [{ name: "X", type: "int" }],
    save: true,
  });
});

test("createBlueprintEnum defaults entries to [] and posts to correct path", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.createBlueprintEnum({ asset_path: "/Game/E_Foo", entries: ["A", "B"], save: false });
  assert.equal(requestLog[0].path, "/asset/create_blueprint_enum");
  assert.deepEqual(requestLog[0].body, {
    asset_path: "/Game/E_Foo",
    entries: ["A", "B"],
    save: false,
  });
});

test("createBlueprintInterface posts to /asset/create_blueprint_interface with functions[]", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.createBlueprintInterface({ asset_path: "/Game/BPI_Foo" });
  assert.equal(requestLog[0].path, "/asset/create_blueprint_interface");
  assert.deepEqual(requestLog[0].body, { asset_path: "/Game/BPI_Foo", functions: [] });
});

test("createAnimBlueprint requires target_skeleton, optional parent_class + save", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.createAnimBlueprint({
    asset_path: "/Game/Anim/ABP_X",
    target_skeleton: "/Game/Anim/SK_Hero_Skeleton",
  });
  assert.equal(requestLog[0].path, "/asset/create_anim_blueprint");
  assert.deepEqual(requestLog[0].body, {
    asset_path: "/Game/Anim/ABP_X",
    target_skeleton: "/Game/Anim/SK_Hero_Skeleton",
  });

  await ue5.createAnimBlueprint({
    asset_path: "/Game/Anim/ABP_X",
    target_skeleton: "/Game/Anim/SK_Hero_Skeleton",
    parent_class: "/Script/Engine.AnimInstance",
    save: true,
  });
  const b = requestLog[1].body;
  assert.equal(b.parent_class, "/Script/Engine.AnimInstance");
  assert.equal(b.save, true);
});

// ---------------------------------------------------------------------------
// bp function/macro/variable + listing/selection
// ---------------------------------------------------------------------------

test("bpCreateFunction POSTs to /bp/create_function and only includes provided opts", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.bpCreateFunction({ blueprint: "/Game/BP_X", name: "DoThing" });
  assert.equal(requestLog[0].path, "/bp/create_function");
  assert.deepEqual(requestLog[0].body, { blueprint: "/Game/BP_X", name: "DoThing" });

  await ue5.bpCreateFunction({
    blueprint: "/Game/BP_X",
    name: "DoThing",
    inputs:  [{ name: "X", type: "int" }],
    outputs: [{ name: "Result", type: "bool" }],
    is_pure: true,
    is_const: false,
    category: "Combat",
    compile: true,
    save:    true,
  });
  const b = requestLog[1].body;
  assert.equal(b.is_pure, true);
  assert.equal(b.is_const, false);
  assert.equal(b.category, "Combat");
});

test("bpCreateMacro POSTs to /bp/create_macro with optional fields", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.bpCreateMacro({ blueprint: "/Game/BP_X", name: "Macro_X" });
  assert.equal(requestLog[0].path, "/bp/create_macro");
  assert.deepEqual(requestLog[0].body, { blueprint: "/Game/BP_X", name: "Macro_X" });
});

test("bpCreateVariable maps the `private` keyword arg back to JSON 'private'", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.bpCreateVariable({
    blueprint: "/Game/BP_X",
    name: "Speed",
    type: "float",
    default_value: 100,
    category: "Locomotion",
    instance_editable: true,
    blueprint_read_only: false,
    expose_on_spawn: true,
    private: true,
    compile: true,
    save: false,
  });
  const b = requestLog[0].body;
  assert.equal(requestLog[0].path, "/bp/create_variable");
  assert.equal(b.private, true);
  assert.equal(b.default_value, 100);
  assert.equal(b.expose_on_spawn, true);
  assert.equal(b.save, false);
});

test("bpCreateVariable only requires name+type+blueprint", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.bpCreateVariable({ blueprint: "/Game/BP_X", name: "X", type: "int" });
  assert.deepEqual(requestLog[0].body, {
    blueprint: "/Game/BP_X",
    name: "X",
    type: "int",
  });
});

test("bpDeleteFunction POSTs to /bp/delete_function and omits unset opts", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.bpDeleteFunction({ blueprint: "/Game/BP_X", name: "PlayFoleyEvent" });
  assert.equal(requestLog[0].path, "/bp/delete_function");
  assert.deepEqual(requestLog[0].body, { blueprint: "/Game/BP_X", name: "PlayFoleyEvent" });

  await ue5.bpDeleteFunction({ blueprint: "/Game/BP_X", name: "PlayFoleyEvent", compile: false, save: true });
  assert.equal(requestLog[1].body.compile, false);
  assert.equal(requestLog[1].body.save, true);
});

test("bpDeleteVariable POSTs to /bp/delete_variable and omits unset opts", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.bpDeleteVariable({ blueprint: "/Game/BP_X", name: "FoleyEventBank" });
  assert.equal(requestLog[0].path, "/bp/delete_variable");
  assert.deepEqual(requestLog[0].body, { blueprint: "/Game/BP_X", name: "FoleyEventBank" });
});

test("bpListVariables GETs /bp/list_variables?blueprint=", async () => {
  handler = () => ({ status: 200, body: { variables: [] } });
  await ue5.bpListVariables({ blueprint: "/Game/BP_X" });
  assert.equal(requestLog[0].method, "GET");
  assert.equal(requestLog[0].path, "/bp/list_variables");
  assert.equal(requestLog[0].query.blueprint, "/Game/BP_X");
});

test("bpGetSelection omits all query params when nothing is set", async () => {
  handler = () => ({ status: 200, body: { nodes: [] } });
  await ue5.bpGetSelection({});
  assert.equal(requestLog[0].method, "GET");
  assert.equal(requestLog[0].path, "/bp/get_selection");
  assert.deepEqual(requestLog[0].query, {});
});

test("bpGetSelection encodes booleans as '1' / '0' and forwards blueprint", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.bpGetSelection({ blueprint: "/Game/BP_X", include_pins: true, include_boundary: false });
  assert.equal(requestLog[0].query.blueprint, "/Game/BP_X");
  assert.equal(requestLog[0].query.include_pins, "1");
  assert.equal(requestLog[0].query.include_boundary, "0");
});

// ---------------------------------------------------------------------------
// Behavior Trees
// ---------------------------------------------------------------------------

test("btCreateTree POSTs asset_path and omits blackboard_path/save when unset", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.btCreateTree({ asset_path: "/Game/AI/BT_Enemy" });
  assert.equal(requestLog[0].path, "/bt/create_tree");
  assert.deepEqual(requestLog[0].body, { asset_path: "/Game/AI/BT_Enemy" });

  await ue5.btCreateTree({
    asset_path: "/Game/AI/BT_Enemy",
    blackboard_path: "/Game/AI/BB_Enemy",
    save: true,
  });
  assert.deepEqual(requestLog[1].body, {
    asset_path: "/Game/AI/BT_Enemy",
    blackboard_path: "/Game/AI/BB_Enemy",
    save: true,
  });
});

test("btCreateBlackboard POSTs asset_path and optional parent", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.btCreateBlackboard({ asset_path: "/Game/AI/BB_X" });
  assert.equal(requestLog[0].path, "/bt/create_blackboard");
  assert.deepEqual(requestLog[0].body, { asset_path: "/Game/AI/BB_X" });

  await ue5.btCreateBlackboard({
    asset_path: "/Game/AI/BB_X",
    parent_blackboard_path: "/Game/AI/BB_Base",
  });
  assert.equal(requestLog[1].body.parent_blackboard_path, "/Game/AI/BB_Base");
});

test("btAddBlackboardKeys defaults keys[] to []", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.btAddBlackboardKeys({ asset_path: "/Game/AI/BB_X" });
  assert.equal(requestLog[0].path, "/bt/add_blackboard_keys");
  assert.deepEqual(requestLog[0].body, { asset_path: "/Game/AI/BB_X", keys: [] });
});

test("btAddBlackboardKeys forwards keys list", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.btAddBlackboardKeys({
    asset_path: "/Game/AI/BB_X",
    keys: [{ name: "Target", type: "Object" }],
    save: true,
  });
  assert.deepEqual(requestLog[0].body, {
    asset_path: "/Game/AI/BB_X",
    keys: [{ name: "Target", type: "Object" }],
    save: true,
  });
});

test("btAddLogic POSTs behavior_tree + nodes[]=[] by default", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.btAddLogic({ behavior_tree: "/Game/AI/BT_X" });
  assert.equal(requestLog[0].path, "/bt/add_logic");
  assert.deepEqual(requestLog[0].body, { behavior_tree: "/Game/AI/BT_X", nodes: [] });
});

test("btAddLogic forwards decorators/services/connections/clear/compile/save", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.btAddLogic({
    behavior_tree: "/Game/AI/BT_X",
    nodes:       [{ id: "root", type: "Selector" }],
    decorators:  [{ on: "root", type: "Cooldown" }],
    services:    [{ on: "root", type: "KeepFocus" }],
    connections: [{ from: "root", to: "child" }],
    clear: true, compile: true, save: false,
  });
  const b = requestLog[0].body;
  assert.equal(b.clear, true);
  assert.deepEqual(b.decorators, [{ on: "root", type: "Cooldown" }]);
  assert.deepEqual(b.connections, [{ from: "root", to: "child" }]);
});

test("btReadTree GETs /bt/read_tree with behavior_tree query param", async () => {
  handler = () => ({ status: 200, body: { graph: {} } });
  await ue5.btReadTree({ behavior_tree: "/Game/AI/BT_X" });
  assert.equal(requestLog[0].method, "GET");
  assert.equal(requestLog[0].path, "/bt/read_tree");
  assert.equal(requestLog[0].query.behavior_tree, "/Game/AI/BT_X");
});

test("stateTreeReadTree GETs /statetree/read_tree with state_tree query param", async () => {
  handler = () => ({ status: 200, body: { states: [] } });
  await ue5.stateTreeReadTree({ state_tree: "/Game/AI/ST_X" });
  assert.equal(requestLog[0].method, "GET");
  assert.equal(requestLog[0].path, "/statetree/read_tree");
  assert.equal(requestLog[0].query.state_tree, "/Game/AI/ST_X");
});

test("stateTreeRepointNode POSTs /statetree/repoint_node; optional fields omitted unless set", async () => {
  handler = () => ({ status: 200, body: { repointed_count: 1 } });
  await ue5.stateTreeRepointNode({
    state_tree: "/Game/AI/ST_X",
    from_class: "STC_CheckCooldown_C",
  });
  assert.equal(requestLog[0].method, "POST");
  assert.equal(requestLog[0].path, "/statetree/repoint_node");
  assert.deepEqual(requestLog[0].body, {
    state_tree: "/Game/AI/ST_X",
    from_class: "STC_CheckCooldown_C",
  });

  await ue5.stateTreeRepointNode({
    state_tree: "/Game/AI/ST_X",
    from_class: "STC_CheckCooldown_C",
    to_class: "/Script/GameAnimationSample.GameAnimCheckCooldownCondition",
    save: true,
    dry_run: false,
  });
  assert.equal(requestLog[1].body.to_class, "/Script/GameAnimationSample.GameAnimCheckCooldownCondition");
  assert.equal(requestLog[1].body.save, true);
  assert.equal(requestLog[1].body.dry_run, false);
});

// ---------------------------------------------------------------------------
// Gameplay Ability System (GAS)
// ---------------------------------------------------------------------------

test("gasSetupActor only requires blueprint_path; replication_mode/save optional", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.gasSetupActor({ blueprint_path: "/Game/BP_Char" });
  assert.equal(requestLog[0].path, "/gas/setup_actor");
  assert.deepEqual(requestLog[0].body, { blueprint_path: "/Game/BP_Char" });

  await ue5.gasSetupActor({
    blueprint_path: "/Game/BP_Char",
    replication_mode: "Mixed",
    save: true,
  });
  assert.equal(requestLog[1].body.replication_mode, "Mixed");
});

test("gasCreateAttributeSet forwards attributes[] and parent_class", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.gasCreateAttributeSet({ asset_path: "/Game/AS_X" });
  assert.deepEqual(requestLog[0].body, { asset_path: "/Game/AS_X" });

  await ue5.gasCreateAttributeSet({
    asset_path:  "/Game/AS_X",
    parent_class: "/Script/GameplayAbilities.AttributeSet",
    attributes:  [{ name: "Health", default: 100 }],
    save: true,
  });
  const b = requestLog[1].body;
  assert.equal(b.parent_class, "/Script/GameplayAbilities.AttributeSet");
  assert.deepEqual(b.attributes, [{ name: "Health", default: 100 }]);
});

test("gasCreateAbility forwards every optional ability field", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.gasCreateAbility({
    asset_path: "/Game/GA_Fireball",
    parent_class: "/Script/GameplayAbilities.GameplayAbility",
    net_execution_policy: "LocalPredicted",
    instancing_policy:    "InstancedPerActor",
    ability_tags:         ["Ability.Fire"],
    block_ability_tags:   ["State.Stunned"],
    cancel_abilities_tags:["Ability.Heal"],
    cost_effect:     "/Game/GE_FireballCost",
    cooldown_effect: "/Game/GE_FireballCooldown",
    activation_group: "Independent",
    save: true,
  });
  const b = requestLog[0].body;
  assert.equal(requestLog[0].path, "/gas/create_ability");
  assert.equal(b.net_execution_policy, "LocalPredicted");
  assert.deepEqual(b.ability_tags, ["Ability.Fire"]);
  assert.equal(b.activation_group, "Independent");
});

test("gasCreateAbility only requires asset_path", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.gasCreateAbility({ asset_path: "/Game/GA_X" });
  assert.deepEqual(requestLog[0].body, { asset_path: "/Game/GA_X" });
});

test("gasCreateEffect forwards every optional effect field", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.gasCreateEffect({
    asset_path: "/Game/GE_Burn",
    parent_class: "/Script/GameplayAbilities.GameplayEffect",
    duration_policy: "HasDuration",
    duration: 5,
    period:   1,
    stacking_type: "AggregateBySource",
    stack_limit:   3,
    modifiers: [{ attribute: "Health", op: "Add", value: -10 }],
    granted_tags: ["State.Burning"],
    application_required_tags: ["Target.Alive"],
    ongoing_required_tags:     ["State.Alive"],
    immunity_tags: ["State.Immune.Fire"],
    save: true,
  });
  const b = requestLog[0].body;
  assert.equal(requestLog[0].path, "/gas/create_effect");
  assert.equal(b.duration_policy, "HasDuration");
  assert.equal(b.period, 1);
  assert.equal(b.stack_limit, 3);
  assert.deepEqual(b.modifiers, [{ attribute: "Health", op: "Add", value: -10 }]);
  assert.deepEqual(b.immunity_tags, ["State.Immune.Fire"]);
});

test("gasConfigureAsc POSTs blueprint_path + optional replication_mode/save", async () => {
  handler = () => ({ status: 200, body: {} });
  await ue5.gasConfigureAsc({ blueprint_path: "/Game/BP_X", replication_mode: "Minimal" });
  assert.equal(requestLog[0].path, "/gas/configure_asc");
  assert.deepEqual(requestLog[0].body, {
    blueprint_path: "/Game/BP_X",
    replication_mode: "Minimal",
  });
});

test("gasReadSetup GETs /gas/read_setup with blueprint_path query", async () => {
  handler = () => ({ status: 200, body: { has_asc: true } });
  await ue5.gasReadSetup({ blueprint_path: "/Game/BP_X" });
  assert.equal(requestLog[0].method, "GET");
  assert.equal(requestLog[0].path, "/gas/read_setup");
  assert.equal(requestLog[0].query.blueprint_path, "/Game/BP_X");
});

// ---------------------------------------------------------------------------
// Bridge discovery file (multi-project port isolation)
// ---------------------------------------------------------------------------
// The plugin writes <Project>/Saved/UnrealNGGMCP/bridge.json with the port it
// actually bound to (after auto-scanning if the default was busy). The sidecar
// reads that file so two editors on the same machine each talk to their own
// bridge without manual env config.

test("getBridgeUrl reads bridge.json when NGG_BRIDGE_URL is not set", () => {
  const tmpDir = fsExtra.mkdtempSync(path.join(os.tmpdir(), "ngg-discovery-"));
  const tmpFile = path.join(tmpDir, "bridge.json");
  fsExtra.writeFileSync(tmpFile, JSON.stringify({ port: 9999, pid: 1, started_at: "x" }));

  const savedEnv = process.env.NGG_BRIDGE_URL;
  delete process.env.NGG_BRIDGE_URL;
  ue5._setDiscoveryPathForTests(tmpFile);

  try {
    assert.equal(ue5.getBridgeUrl(), "http://localhost:9999");
  } finally {
    ue5._setDiscoveryPathForTests(null);
    if (savedEnv !== undefined) process.env.NGG_BRIDGE_URL = savedEnv;
    fsExtra.rmSync(tmpDir, { recursive: true, force: true });
  }
});

test("NGG_BRIDGE_URL env beats bridge.json", () => {
  const tmpDir = fsExtra.mkdtempSync(path.join(os.tmpdir(), "ngg-discovery-"));
  const tmpFile = path.join(tmpDir, "bridge.json");
  fsExtra.writeFileSync(tmpFile, JSON.stringify({ port: 9999 }));

  const savedEnv = process.env.NGG_BRIDGE_URL;
  process.env.NGG_BRIDGE_URL = "http://localhost:1234";
  ue5._setDiscoveryPathForTests(tmpFile);

  try {
    assert.equal(ue5.getBridgeUrl(), "http://localhost:1234");
  } finally {
    ue5._setDiscoveryPathForTests(null);
    if (savedEnv !== undefined) process.env.NGG_BRIDGE_URL = savedEnv;
    else delete process.env.NGG_BRIDGE_URL;
    fsExtra.rmSync(tmpDir, { recursive: true, force: true });
  }
});

test("getBridgeUrl falls back to default when discovery file is missing and env unset", () => {
  const tmpDir = fsExtra.mkdtempSync(path.join(os.tmpdir(), "ngg-discovery-"));
  const tmpFile = path.join(tmpDir, "does-not-exist.json");

  const savedEnv = process.env.NGG_BRIDGE_URL;
  delete process.env.NGG_BRIDGE_URL;
  ue5._setDiscoveryPathForTests(tmpFile);

  try {
    assert.equal(ue5.getBridgeUrl(), ue5.DEFAULT_BRIDGE_URL);
  } finally {
    ue5._setDiscoveryPathForTests(null);
    if (savedEnv !== undefined) process.env.NGG_BRIDGE_URL = savedEnv;
    fsExtra.rmSync(tmpDir, { recursive: true, force: true });
  }
});

test("malformed bridge.json is ignored and default is used", () => {
  const tmpDir = fsExtra.mkdtempSync(path.join(os.tmpdir(), "ngg-discovery-"));
  const tmpFile = path.join(tmpDir, "bridge.json");
  fsExtra.writeFileSync(tmpFile, "{ not valid json");

  const savedEnv = process.env.NGG_BRIDGE_URL;
  delete process.env.NGG_BRIDGE_URL;
  ue5._setDiscoveryPathForTests(tmpFile);

  try {
    assert.equal(ue5.getBridgeUrl(), ue5.DEFAULT_BRIDGE_URL);
  } finally {
    ue5._setDiscoveryPathForTests(null);
    if (savedEnv !== undefined) process.env.NGG_BRIDGE_URL = savedEnv;
    fsExtra.rmSync(tmpDir, { recursive: true, force: true });
  }
});

test("bridge.json with out-of-range port is ignored", () => {
  const tmpDir = fsExtra.mkdtempSync(path.join(os.tmpdir(), "ngg-discovery-"));
  const tmpFile = path.join(tmpDir, "bridge.json");
  fsExtra.writeFileSync(tmpFile, JSON.stringify({ port: 99999 }));

  const savedEnv = process.env.NGG_BRIDGE_URL;
  delete process.env.NGG_BRIDGE_URL;
  ue5._setDiscoveryPathForTests(tmpFile);

  try {
    assert.equal(ue5.getBridgeUrl(), ue5.DEFAULT_BRIDGE_URL);
  } finally {
    ue5._setDiscoveryPathForTests(null);
    if (savedEnv !== undefined) process.env.NGG_BRIDGE_URL = savedEnv;
    fsExtra.rmSync(tmpDir, { recursive: true, force: true });
  }
});

// ---------------------------------------------------------------------------
// Auth token fallback (DefaultEngine.ini [UnrealNGGMCP] AuthToken)
// ---------------------------------------------------------------------------
// The plugin reads its token from DefaultEngine.ini; the client falls back to
// the same file when NGG_BRIDGE_TOKEN is unset so standalone runs (live test
// suites, `node index.js` by hand) authenticate without env setup.

test("auth token falls back to DefaultEngine.ini when NGG_BRIDGE_TOKEN is unset", () => {
  const tmpDir = fsExtra.mkdtempSync(path.join(os.tmpdir(), "ngg-ini-"));
  const tmpIni = path.join(tmpDir, "DefaultEngine.ini");
  fsExtra.writeFileSync(
    tmpIni,
    "[/Script/Engine.Engine]\nAuthToken=decoy-from-wrong-section\n\n" +
    "[UnrealNGGMCP]\nAuthToken=abc123\n\n[OtherSection]\nFoo=Bar\n"
  );

  const savedEnv = process.env.NGG_BRIDGE_TOKEN;
  delete process.env.NGG_BRIDGE_TOKEN;
  ue5._setIniPathForTests(tmpIni);

  try {
    assert.equal(ue5._getAuthTokenForTests(), "abc123");
  } finally {
    ue5._setIniPathForTests(null);
    if (savedEnv !== undefined) process.env.NGG_BRIDGE_TOKEN = savedEnv;
    fsExtra.rmSync(tmpDir, { recursive: true, force: true });
  }
});

test("UserEngine.ini layer beats DefaultEngine.ini", () => {
  // The secret lives in the gitignored UserEngine.ini layer; a token left behind
  // in the tracked DefaultEngine.ini must not shadow it.
  const tmpDir    = fsExtra.mkdtempSync(path.join(os.tmpdir(), "ngg-ini-"));
  const userIni   = path.join(tmpDir, "UserEngine.ini");
  const configIni = path.join(tmpDir, "DefaultEngine.ini");
  fsExtra.writeFileSync(userIni,   "[UnrealNGGMCP]\nAuthToken=from-user\n");
  fsExtra.writeFileSync(configIni, "[UnrealNGGMCP]\nAuthToken=stale-from-config\n");

  const savedEnv = process.env.NGG_BRIDGE_TOKEN;
  delete process.env.NGG_BRIDGE_TOKEN;
  ue5._setIniPathsForTests([userIni, configIni]);

  try {
    assert.equal(ue5._getAuthTokenForTests(), "from-user");
  } finally {
    ue5._setIniPathsForTests(null);
    if (savedEnv !== undefined) process.env.NGG_BRIDGE_TOKEN = savedEnv;
    fsExtra.rmSync(tmpDir, { recursive: true, force: true });
  }
});

test("DefaultEngine.ini still works when the UserEngine.ini layer is absent", () => {
  // Back-compat: existing installs keep their token in DefaultEngine.ini until
  // they move it, and a fresh clone has no UserEngine.ini at all.
  const tmpDir    = fsExtra.mkdtempSync(path.join(os.tmpdir(), "ngg-ini-"));
  const userIni   = path.join(tmpDir, "no-such-UserEngine.ini");
  const configIni = path.join(tmpDir, "DefaultEngine.ini");
  fsExtra.writeFileSync(configIni, "[UnrealNGGMCP]\nAuthToken=from-config\n");

  const savedEnv = process.env.NGG_BRIDGE_TOKEN;
  delete process.env.NGG_BRIDGE_TOKEN;
  ue5._setIniPathsForTests([userIni, configIni]);

  try {
    assert.equal(ue5._getAuthTokenForTests(), "from-config");
  } finally {
    ue5._setIniPathsForTests(null);
    if (savedEnv !== undefined) process.env.NGG_BRIDGE_TOKEN = savedEnv;
    fsExtra.rmSync(tmpDir, { recursive: true, force: true });
  }
});

test("a tokenless UserEngine.ini falls through to DefaultEngine.ini", () => {
  // A UserEngine.ini that exists for other overrides but carries no token must
  // not shadow the DefaultEngine.ini fallback — otherwise the token silently
  // resolves to empty and every authenticated route starts 401-ing.
  const tmpDir    = fsExtra.mkdtempSync(path.join(os.tmpdir(), "ngg-ini-"));
  const userIni   = path.join(tmpDir, "UserEngine.ini");
  const configIni = path.join(tmpDir, "DefaultEngine.ini");
  fsExtra.writeFileSync(userIni,   "[/Script/Engine.Engine]\nbSomething=True\n");
  fsExtra.writeFileSync(configIni, "[UnrealNGGMCP]\nAuthToken=from-config\n");

  const savedEnv = process.env.NGG_BRIDGE_TOKEN;
  delete process.env.NGG_BRIDGE_TOKEN;
  ue5._setIniPathsForTests([userIni, configIni]);

  try {
    assert.equal(ue5._getAuthTokenForTests(), "from-config");
  } finally {
    ue5._setIniPathsForTests(null);
    if (savedEnv !== undefined) process.env.NGG_BRIDGE_TOKEN = savedEnv;
    fsExtra.rmSync(tmpDir, { recursive: true, force: true });
  }
});

test("NGG_BRIDGE_TOKEN env beats the ini fallback", () => {
  const tmpDir = fsExtra.mkdtempSync(path.join(os.tmpdir(), "ngg-ini-"));
  const tmpIni = path.join(tmpDir, "DefaultEngine.ini");
  fsExtra.writeFileSync(tmpIni, "[UnrealNGGMCP]\nAuthToken=from-ini\n");

  const savedEnv = process.env.NGG_BRIDGE_TOKEN;
  process.env.NGG_BRIDGE_TOKEN = "from-env";
  ue5._setIniPathForTests(tmpIni);

  try {
    assert.equal(ue5._getAuthTokenForTests(), "from-env");
  } finally {
    ue5._setIniPathForTests(null);
    if (savedEnv !== undefined) process.env.NGG_BRIDGE_TOKEN = savedEnv;
    else delete process.env.NGG_BRIDGE_TOKEN;
    fsExtra.rmSync(tmpDir, { recursive: true, force: true });
  }
});

test("auth token is empty when ini is missing and env unset", () => {
  const tmpDir = fsExtra.mkdtempSync(path.join(os.tmpdir(), "ngg-ini-"));
  const tmpIni = path.join(tmpDir, "does-not-exist.ini");

  const savedEnv = process.env.NGG_BRIDGE_TOKEN;
  delete process.env.NGG_BRIDGE_TOKEN;
  ue5._setIniPathForTests(tmpIni);

  try {
    assert.equal(ue5._getAuthTokenForTests(), "");
  } finally {
    ue5._setIniPathForTests(null);
    if (savedEnv !== undefined) process.env.NGG_BRIDGE_TOKEN = savedEnv;
    fsExtra.rmSync(tmpDir, { recursive: true, force: true });
  }
});

test("ini without an [UnrealNGGMCP] section yields empty token", () => {
  const tmpDir = fsExtra.mkdtempSync(path.join(os.tmpdir(), "ngg-ini-"));
  const tmpIni = path.join(tmpDir, "DefaultEngine.ini");
  fsExtra.writeFileSync(tmpIni, "[SomeSection]\nAuthToken=not-ours\n");

  const savedEnv = process.env.NGG_BRIDGE_TOKEN;
  delete process.env.NGG_BRIDGE_TOKEN;
  ue5._setIniPathForTests(tmpIni);

  try {
    assert.equal(ue5._getAuthTokenForTests(), "");
  } finally {
    ue5._setIniPathForTests(null);
    if (savedEnv !== undefined) process.env.NGG_BRIDGE_TOKEN = savedEnv;
    fsExtra.rmSync(tmpDir, { recursive: true, force: true });
  }
});

// ---------------------------------------------------------------------------
// 401 diagnostics
// ---------------------------------------------------------------------------
// The editor provisions an AuthToken into <Project>/Config/DefaultEngine.ini on
// first run, so a 401 now almost always means the two ends read different
// sources. The error has to name the one this process used.

test("401 names NGG_BRIDGE_TOKEN as the override when the env var is set", async () => {
  process.env.NGG_BRIDGE_TOKEN = "stale-token";
  handler = () => ({ status: 401, body: { error: "Unauthorized: missing or invalid Authorization header" } });

  await assert.rejects(ue5.healthCheck(), (err) => {
    assert.match(err.message, /^UE5 bridge error 401:/);
    assert.match(err.message, /NGG_BRIDGE_TOKEN is set in this process's environment/);
    // Names the gitignored layer, not the tracked one: that is where the editor
    // provisions the token and where the hint must send the reader.
    assert.match(err.message, /UserEngine\.ini/);
    // The token itself must never be echoed into an error string.
    assert.doesNotMatch(err.message, /stale-token/);
    return true;
  });
});

test("401 points at the ini when the token came from there", async () => {
  const tmpDir = fsExtra.mkdtempSync(path.join(os.tmpdir(), "ngg-ini-"));
  const tmpIni = path.join(tmpDir, "DefaultEngine.ini");
  fsExtra.writeFileSync(tmpIni, "[UnrealNGGMCP]\nAuthToken=from-ini\n");
  ue5._setIniPathForTests(tmpIni);
  handler = () => ({ status: 401, body: { error: "Unauthorized" } });

  try {
    await assert.rejects(ue5.healthCheck(), (err) => {
      assert.match(err.message, /different project/);
      assert.ok(err.message.includes(tmpIni));
      assert.doesNotMatch(err.message, /from-ini/);
      return true;
    });
  } finally {
    ue5._setIniPathForTests(null);
    fsExtra.rmSync(tmpDir, { recursive: true, force: true });
  }
});

test("401 with no token anywhere tells the developer to restart the editor", async () => {
  ue5._setIniPathForTests(path.join(os.tmpdir(), "ngg-no-such-dir", "DefaultEngine.ini"));
  handler = () => ({ status: 401, body: { error: "Unauthorized" } });

  try {
    await assert.rejects(ue5.healthCheck(), (err) => {
      assert.match(err.message, /No token was sent/);
      assert.match(err.message, /Restart the Unreal Editor/);
      return true;
    });
  } finally {
    ue5._setIniPathForTests(null);
  }
});

test("non-401 errors keep the plain message", async () => {
  handler = () => ({ status: 404, body: { error: "Asset not found" } });
  await assert.rejects(ue5.healthCheck(), (err) => {
    assert.equal(err.message, "UE5 bridge error 404: Asset not found");
    return true;
  });
});

test("default discovery path resolves to <Project>/Saved/UnrealNGGMCP/bridge.json", () => {
  // Sidecar lives at <Project>/Plugins/UnrealNGGMCP/Source/ThirdParty/unrealngg-mcp/ so the
  // computed path must end with the per-project Saved/ directory.
  const p = ue5._getDiscoveryPathForTests();
  const normalized = p.replace(/\\/g, "/");
  assert.ok(
    normalized.endsWith("/Saved/UnrealNGGMCP/bridge.json"),
    `unexpected discovery path: ${p}`
  );
  // "endsWith" alone also accepts <Project>/Plugins/Saved/..., which is what an
  // off-by-one in the walk up from the sidecar directory produces. The root must
  // be the project, so the plugin's own subtree must not appear in the path.
  assert.ok(
    !normalized.includes("/Plugins/"),
    `discovery path must sit at the project root, not inside Plugins/: ${p}`
  );
});

test("default ini fallback consults UserEngine before DefaultEngine, at the project root", () => {
  // Same off-by-one risk as the discovery path: landing in <Project>/Plugins/Config
  // silently disables the token fallback and every request comes back 401.
  const paths = ue5._getIniPathsForTests().map(p => p.replace(/\\/g, "/"));
  assert.equal(paths.length, 2, `expected two ini layers, got ${JSON.stringify(paths)}`);
  // UserEngine.ini must come first: it is the gitignored layer that carries the
  // secret, and GConfig's GameDirUser layer outranks ProjectDefault too.
  assert.ok(
    paths[0].endsWith("/Config/UserEngine.ini"),
    `unexpected first ini layer: ${paths[0]}`
  );
  assert.ok(
    paths[1].endsWith("/Config/DefaultEngine.ini"),
    `unexpected second ini layer: ${paths[1]}`
  );
  for (const p of paths) {
    assert.ok(
      !p.includes("/Plugins/"),
      `ini path must sit at the project root, not inside Plugins/: ${p}`
    );
  }
});

// ---------------------------------------------------------------------------
// NGG_PROJECT_ROOT
// ---------------------------------------------------------------------------
// The walk up from this file finds the project only for the standard
// <Project>/Plugins/<PluginFolder> layout. For anything else — a plugin grouped
// under Plugins/<Group>/<PluginFolder>, or one still installed in the engine —
// the editor writes NGG_PROJECT_ROOT into the generated .mcp.json, and both
// project-relative paths must follow it.

test("NGG_PROJECT_ROOT overrides the walked project root for both derived paths", () => {
  const saved = process.env.NGG_PROJECT_ROOT;
  const fakeRoot = path.join(os.tmpdir(), "ngg-fake-project");
  process.env.NGG_PROJECT_ROOT = fakeRoot;

  try {
    assert.equal(
      ue5._getDiscoveryPathForTests(),
      path.join(fakeRoot, "Saved", "UnrealNGGMCP", "bridge.json")
    );
    assert.deepEqual(
      ue5._getIniPathsForTests(),
      [
        path.join(fakeRoot, "Config", "UserEngine.ini"),
        path.join(fakeRoot, "Config", "DefaultEngine.ini"),
      ]
    );
  } finally {
    if (saved === undefined) delete process.env.NGG_PROJECT_ROOT;
    else process.env.NGG_PROJECT_ROOT = saved;
  }
});

test("NGG_PROJECT_ROOT is read per call, not cached from module load", () => {
  // Resolution has to stay lazy: the variable is read from a .mcp.json the
  // editor rewrites when the plugin moves, so a value captured at import would
  // survive the move and keep pointing at the old project.
  const saved = process.env.NGG_PROJECT_ROOT;
  const before = ue5._getIniPathsForTests();

  try {
    process.env.NGG_PROJECT_ROOT = path.join(os.tmpdir(), "ngg-other-project");
    assert.notDeepEqual(ue5._getIniPathsForTests(), before);

    delete process.env.NGG_PROJECT_ROOT;
    assert.deepEqual(ue5._getIniPathsForTests(), before);
  } finally {
    if (saved === undefined) delete process.env.NGG_PROJECT_ROOT;
    else process.env.NGG_PROJECT_ROOT = saved;
  }
});

test("relative NGG_PROJECT_ROOT is resolved to an absolute path", () => {
  const saved = process.env.NGG_PROJECT_ROOT;
  process.env.NGG_PROJECT_ROOT = ".";

  try {
    const paths = ue5._getIniPathsForTests();
    for (const p of paths) {
      assert.ok(path.isAbsolute(p), `ini path must be absolute, got: ${p}`);
    }
    assert.deepEqual(paths, [
      path.join(process.cwd(), "Config", "UserEngine.ini"),
      path.join(process.cwd(), "Config", "DefaultEngine.ini"),
    ]);
  } finally {
    if (saved === undefined) delete process.env.NGG_PROJECT_ROOT;
    else process.env.NGG_PROJECT_ROOT = saved;
  }
});
