// Copyright 2025-2026 NGG. All Rights Reserved.
// pcg.unit.test.js — unit tests for the PCG MCP tools.
// pcg.js drives UE by POSTing Python scripts to /editor/exec_python. The mock
// bridge captures each script, lets assertions check it was built correctly,
// and returns sentinel-wrapped JSON so runPy's parsing is exercised end-to-end.
//
// No live Unreal Editor needed. Complement to the live smoke test in
// pcg.test.mjs (which requires a running UE with the bridge plugin).
//
// Run: node --test pcg.unit.test.js

import { test, before, after, beforeEach } from "node:test";
import assert from "node:assert/strict";
import { createServer } from "node:http";

import * as pcg from "./pcg.js";

const RESULT_BEGIN = "__NGG_PCG_RESULT_BEGIN__";
const RESULT_END   = "__NGG_PCG_RESULT_END__";

// pcg.js no longer interpolates the payload as plaintext JSON into the Python
// script — it base64-encodes it (json.loads(base64.b64decode("..."))) so asset
// names/paths containing quotes, newlines, or backslashes can't corrupt or
// inject the script. Tests therefore decode the base64 blob(s) and assert on
// the structured payload instead of regex-matching the script text.
function decodedArgs(script) {
  const out = [];
  const re = /base64\.b64decode\("([A-Za-z0-9+/=]+)"\)/g;
  let m;
  while ((m = re.exec(script)) !== null) {
    out.push(JSON.parse(Buffer.from(m[1], "base64").toString("utf8")));
  }
  return out;
}
// First decoded payload — the common single-payload case.
function argsOf(script) {
  const all = decodedArgs(script);
  assert.ok(all.length >= 1, "script should embed a base64-encoded payload");
  return all[0];
}

// ---------------------------------------------------------------------------
// Mock /editor/exec_python endpoint.
// ---------------------------------------------------------------------------

let server;
let serverUrl;
let capturedScripts;  // array of { command, mode }
let replyPayload;     // value to encode between sentinels (default success envelope)
let replyRawLog;      // if set, replaces sentinel reply with a literal log array

function readBody(req) {
  return new Promise((resolve) => {
    let buf = "";
    req.on("data", (c) => (buf += c));
    req.on("end", () => resolve(buf));
  });
}

function sentinelLog(obj) {
  return [{ type: "Info", output: `${RESULT_BEGIN}${JSON.stringify(obj)}${RESULT_END}\n` }];
}

before(async () => {
  server = createServer(async (req, res) => {
    const bodyText = await readBody(req);
    let parsedBody = {};
    try { parsedBody = bodyText ? JSON.parse(bodyText) : {}; } catch {}
    capturedScripts.push(parsedBody);

    res.writeHead(200, { "Content-Type": "application/json" });
    if (replyRawLog) {
      res.end(JSON.stringify({ success: true, log: replyRawLog }));
    } else {
      res.end(JSON.stringify({ success: true, log: sentinelLog(replyPayload) }));
    }
  });

  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const { port } = server.address();
  serverUrl = `http://127.0.0.1:${port}`;
  process.env.NGG_BRIDGE_URL = serverUrl;
});

after(() => {
  delete process.env.NGG_BRIDGE_URL;
  return new Promise((resolve) => server.close(resolve));
});

beforeEach(() => {
  capturedScripts = [];
  replyPayload = { ok: true };
  replyRawLog = null;
});

// ---------------------------------------------------------------------------
// pcgListNodeTypes
// ---------------------------------------------------------------------------

test("pcgListNodeTypes — full list embeds every curated entry in the Python script", async () => {
  replyPayload = {
    ok: true,
    classes: [
      { name: "PCGSurfaceSamplerSettings", category: "Samplers" },
      { name: "PCGTransformPointsSettings", category: "Points" },
    ],
    count: 2,
  };
  const mcp = await pcg.pcgListNodeTypes({});
  assert.equal(capturedScripts.length, 1);
  // Every curated class name should be embedded in the posted payload.
  const names = argsOf(capturedScripts[0].command).map((e) => e.name);
  assert.ok(names.includes("PCGSurfaceSamplerSettings"));
  assert.ok(names.includes("PCGSpatialNoiseSettings"));
  assert.ok(names.includes("PCGSelectGrammarSettings"));
  // MCP envelope carries the sentinel JSON back as text.
  const parsed = JSON.parse(mcp.content[0].text);
  assert.equal(parsed.count, 2);
  assert.equal(parsed.classes[0].name, "PCGSurfaceSamplerSettings");
});

test("pcgListNodeTypes — category filter narrows the list passed to Python", async () => {
  replyPayload = { ok: true, classes: [], count: 0 };
  await pcg.pcgListNodeTypes({ category: "Samplers" });
  const names = argsOf(capturedScripts[0].command).map((e) => e.name);
  assert.ok(names.includes("PCGSurfaceSamplerSettings"));
  assert.ok(names.includes("PCGSplineSamplerSettings"));
  // A non-Sampler class should NOT appear once filtered.
  assert.ok(!names.includes("PCGSpatialNoiseSettings"),
    "spatial node must not leak into Samplers-only filter");
});

test("pcgListNodeTypes — filter is case-insensitive", async () => {
  replyPayload = { ok: true, classes: [], count: 0 };
  await pcg.pcgListNodeTypes({ category: "SAMPLERS" });
  const names = argsOf(capturedScripts[0].command).map((e) => e.name);
  assert.ok(names.includes("PCGSurfaceSamplerSettings"));
});

// ---------------------------------------------------------------------------
// pcgCreateGraph
// ---------------------------------------------------------------------------

test("pcgCreateGraph — path + standalone + overwrite are forwarded via JSON payload", async () => {
  replyPayload = { ok: true, path: "/Game/PCG/MyGraph", standalone: true };
  const mcp = await pcg.pcgCreateGraph({
    asset_path: "/Game/PCG/MyGraph",
    standalone: true,
    overwrite:  true,
  });
  const a = argsOf(capturedScripts[0].command);
  // The payload JSON is base64-embedded in the Python script.
  assert.equal(a.path, "/Game/PCG/MyGraph");
  assert.equal(a.standalone, true);
  assert.equal(a.overwrite, true);
  const parsed = JSON.parse(mcp.content[0].text);
  assert.equal(parsed.path, "/Game/PCG/MyGraph");
});

test("pcgCreateGraph — defaults standalone=false and overwrite=false when omitted", async () => {
  replyPayload = { ok: true, path: "/Game/PCG/Plain" };
  await pcg.pcgCreateGraph({ asset_path: "/Game/PCG/Plain" });
  const a = argsOf(capturedScripts[0].command);
  assert.equal(a.standalone, false);
  assert.equal(a.overwrite, false);
});

test("pcgCreateGraph — surfaces Python-side error via isError envelope", async () => {
  replyPayload = { ok: false, error: "asset already exists: /Game/PCG/X (pass overwrite=True)" };
  const mcp = await pcg.pcgCreateGraph({ asset_path: "/Game/PCG/X" });
  assert.equal(mcp.isError, true);
  assert.match(mcp.content[0].text, /asset already exists/);
});

// ---------------------------------------------------------------------------
// pcgAddNode
// ---------------------------------------------------------------------------

test("pcgAddNode — graph + settings_class forwarded; node_title defaults to null", async () => {
  replyPayload = {
    ok: true,
    graph: "/Game/PCG/X",
    settings_class: "PCGSurfaceSamplerSettings",
    node_name: "SurfaceSampler_0",
    node_title: "",
  };
  const mcp = await pcg.pcgAddNode({
    graph_path:     "/Game/PCG/X",
    settings_class: "PCGSurfaceSamplerSettings",
  });
  const a = argsOf(capturedScripts[0].command);
  assert.equal(a.graph, "/Game/PCG/X");
  assert.equal(a.cls, "PCGSurfaceSamplerSettings");
  assert.equal(a.title, null);
  const parsed = JSON.parse(mcp.content[0].text);
  assert.equal(parsed.node_name, "SurfaceSampler_0");
});

test("pcgAddNode — node_title is forwarded when provided", async () => {
  replyPayload = { ok: true, node_name: "Thing_0" };
  await pcg.pcgAddNode({
    graph_path:     "/Game/PCG/X",
    settings_class: "PCGTransformPointsSettings",
    node_title:     "My Transform",
  });
  assert.equal(argsOf(capturedScripts[0].command).title, "My Transform");
});

test("pcgAddNode — unknown class returns Python-side error verbatim", async () => {
  replyPayload = { ok: false, error: "unknown PCG settings class: PCGDoesNotExistSettings" };
  const mcp = await pcg.pcgAddNode({
    graph_path:     "/Game/PCG/X",
    settings_class: "PCGDoesNotExistSettings",
  });
  assert.equal(mcp.isError, true);
  assert.match(mcp.content[0].text, /unknown PCG settings class/);
});

// ---------------------------------------------------------------------------
// pcgConnectPins
// ---------------------------------------------------------------------------

test("pcgConnectPins — all four node/pin fields are forwarded to Python", async () => {
  replyPayload = { ok: true };
  await pcg.pcgConnectPins({
    graph_path: "/Game/PCG/X",
    from_node:  "SurfaceSampler_0",
    from_pin:   "Out",
    to_node:    "__output__",
    to_pin:     "Out",
  });
  const a = argsOf(capturedScripts[0].command);
  assert.equal(a.from_node, "SurfaceSampler_0");
  assert.equal(a.from_pin, "Out");
  assert.equal(a.to_node, "__output__");
  assert.equal(a.to_pin, "Out");
});

test("pcgConnectPins — Python-side 'from_node not found' bubbles up as error", async () => {
  replyPayload = { ok: false, error: "from_node not found: Bogus_0" };
  const mcp = await pcg.pcgConnectPins({
    graph_path: "/Game/PCG/X",
    from_node:  "Bogus_0",
    from_pin:   "Out",
    to_node:    "__output__",
    to_pin:     "Out",
  });
  assert.equal(mcp.isError, true);
  assert.match(mcp.content[0].text, /from_node not found/);
});

// ---------------------------------------------------------------------------
// pcgSaveGraph
// ---------------------------------------------------------------------------

test("pcgSaveGraph — payload carries graph path", async () => {
  replyPayload = { ok: true, path: "/Game/PCG/X" };
  await pcg.pcgSaveGraph({ graph_path: "/Game/PCG/X" });
  assert.equal(argsOf(capturedScripts[0].command).graph, "/Game/PCG/X");
});

test("pcgSaveGraph — 'asset not found' Python error surfaced", async () => {
  replyPayload = { ok: false, error: "asset not found: /Game/PCG/DoesNotExist" };
  const mcp = await pcg.pcgSaveGraph({ graph_path: "/Game/PCG/DoesNotExist" });
  assert.equal(mcp.isError, true);
  assert.match(mcp.content[0].text, /asset not found/);
});

// ---------------------------------------------------------------------------
// runPy sentinel-parsing edge cases
// ---------------------------------------------------------------------------

test("runPy — missing sentinel pair surfaces a diagnostic error with log", async () => {
  // Replace the normal sentinel reply with a log that has no sentinel wrapper.
  replyRawLog = [{ type: "Info", output: "print-only output, no sentinel\n" }];
  const mcp = await pcg.pcgSaveGraph({ graph_path: "/Game/PCG/X" });
  assert.equal(mcp.isError, true);
  assert.match(mcp.content[0].text, /no result sentinel/);
});

test("runPy — malformed JSON between sentinels produces 'bad result json'", async () => {
  replyRawLog = [{ type: "Info", output: `${RESULT_BEGIN}not-json-at-all${RESULT_END}\n` }];
  const mcp = await pcg.pcgSaveGraph({ graph_path: "/Game/PCG/X" });
  assert.equal(mcp.isError, true);
  assert.match(mcp.content[0].text, /bad result json/);
});

test("runPy — script body contains prelude helpers (ngg_result + ngg_error)", async () => {
  replyPayload = { ok: true };
  await pcg.pcgSaveGraph({ graph_path: "/Game/PCG/X" });
  const script = capturedScripts[0].command;
  assert.match(script, /def ngg_result/);
  assert.match(script, /def ngg_error/);
  // The body is wrapped in def _ngg_main so early-exit via return works.
  assert.match(script, /def _ngg_main/);
  // Top-level try/except around _ngg_main.
  assert.match(script, /except Exception as _e:/);
});

test("runPy — execPython mode is execute_file (multi-line scripts)", async () => {
  replyPayload = { ok: true };
  await pcg.pcgSaveGraph({ graph_path: "/Game/PCG/X" });
  assert.equal(capturedScripts[0].mode, "execute_file");
});

// ---------------------------------------------------------------------------
// pcgReadGraph
// ---------------------------------------------------------------------------

test("pcgReadGraph — forwards graph path and returns nodes + edges", async () => {
  replyPayload = {
    ok: true,
    graph: "/Game/PCG/X",
    nodes: [
      { name: "InputNode_0",  role: "input",  settings_class: "PCGGraphInputOutputSettings",
        title: "Input",  position: [0, 0], input_pins: [], output_pins: ["Out"] },
      { name: "Sampler_0",    role: "node",   settings_class: "PCGSurfaceSamplerSettings",
        title: "Sampler", position: [200, 0], input_pins: ["In"], output_pins: ["Out"] },
      { name: "OutputNode_0", role: "output", settings_class: "PCGGraphInputOutputSettings",
        title: "Output", position: [400, 0], input_pins: ["In"], output_pins: [] },
    ],
    edges: [
      { from_node: "InputNode_0",  from_pin: "Out", to_node: "Sampler_0",    to_pin: "In" },
      { from_node: "Sampler_0",    from_pin: "Out", to_node: "OutputNode_0", to_pin: "In" },
    ],
  };
  const mcp = await pcg.pcgReadGraph({ graph_path: "/Game/PCG/X" });
  assert.equal(argsOf(capturedScripts[0].command).graph, "/Game/PCG/X");
  const parsed = JSON.parse(mcp.content[0].text);
  assert.equal(parsed.nodes.length, 3);
  assert.equal(parsed.edges.length, 2);
});

// ---------------------------------------------------------------------------
// pcgListSettingsProperties
// ---------------------------------------------------------------------------

test("pcgListSettingsProperties — forwards settings_class and surfaces property list", async () => {
  replyPayload = {
    ok: true,
    settings_class: "PCGStaticMeshSpawnerSettings",
    properties: [
      { name: "mesh_selector_parameters", type: "PCGMeshSelectorBase", default: "" },
      { name: "instance_data_packer",     type: "PCGInstanceDataPackerBase", default: "" },
    ],
    count: 2,
  };
  const mcp = await pcg.pcgListSettingsProperties({ settings_class: "PCGStaticMeshSpawnerSettings" });
  assert.equal(argsOf(capturedScripts[0].command).cls, "PCGStaticMeshSpawnerSettings");
  const parsed = JSON.parse(mcp.content[0].text);
  assert.equal(parsed.count, 2);
});

test("pcgListSettingsProperties — unknown class bubbles up as error", async () => {
  replyPayload = { ok: false, error: "unknown PCG settings class: PCGDoesNotExistSettings" };
  const mcp = await pcg.pcgListSettingsProperties({ settings_class: "PCGDoesNotExistSettings" });
  assert.equal(mcp.isError, true);
  assert.match(mcp.content[0].text, /unknown PCG settings class/);
});

// ---------------------------------------------------------------------------
// pcgSetNodeSettingsProperty
// ---------------------------------------------------------------------------

test("pcgSetNodeSettingsProperty — primitive value path forwards property + value", async () => {
  replyPayload = { ok: true, property: "looping", value: "True" };
  await pcg.pcgSetNodeSettingsProperty({
    graph_path: "/Game/PCG/X",
    node:       "SurfaceSampler_0",
    property:   "looping",
    value:      true,
  });
  const a = argsOf(capturedScripts[0].command);
  assert.equal(a.node, "SurfaceSampler_0");
  assert.equal(a.property, "looping");
  assert.equal(a.value, true);
  // No asset_value when only primitive given.
  assert.equal(a.asset_value, null);
});

test("pcgSetNodeSettingsProperty — asset_value path forwards content reference", async () => {
  replyPayload = { ok: true };
  await pcg.pcgSetNodeSettingsProperty({
    graph_path:  "/Game/PCG/X",
    node:        "StaticMeshSpawner_0",
    property:    "mesh",
    asset_value: "/Game/Meshes/SM_Tree",
  });
  assert.equal(argsOf(capturedScripts[0].command).asset_value, "/Game/Meshes/SM_Tree");
});

test("pcgSetNodeSettingsProperty — missing node returns Python error verbatim", async () => {
  replyPayload = { ok: false, error: "node not found: Ghost_0" };
  const mcp = await pcg.pcgSetNodeSettingsProperty({
    graph_path: "/Game/PCG/X", node: "Ghost_0", property: "seed", value: 42,
  });
  assert.equal(mcp.isError, true);
  assert.match(mcp.content[0].text, /node not found/);
});

// ---------------------------------------------------------------------------
// pcgSetNodePosition / pcgRemoveNode / pcgDisconnectPins / pcgRenameNode
// ---------------------------------------------------------------------------

test("pcgSetNodePosition — forwards x and y as integers", async () => {
  replyPayload = { ok: true, x: 320, y: 100 };
  await pcg.pcgSetNodePosition({
    graph_path: "/Game/PCG/X", node: "Sampler_0", x: 320, y: 100,
  });
  const a = argsOf(capturedScripts[0].command);
  assert.equal(a.x, 320);
  assert.equal(a.y, 100);
});

test("pcgRemoveNode — payload carries graph and node name", async () => {
  replyPayload = { ok: true, removed: "Sampler_0" };
  await pcg.pcgRemoveNode({ graph_path: "/Game/PCG/X", node: "Sampler_0" });
  assert.equal(argsOf(capturedScripts[0].command).node, "Sampler_0");
});

test("pcgDisconnectPins — all four addressing fields are forwarded", async () => {
  replyPayload = { ok: true };
  await pcg.pcgDisconnectPins({
    graph_path: "/Game/PCG/X",
    from_node: "Sampler_0", from_pin: "Out",
    to_node:   "__output__", to_pin: "In",
  });
  const a = argsOf(capturedScripts[0].command);
  assert.equal(a.from_node, "Sampler_0");
  assert.equal(a.to_node, "__output__");
});

test("pcgRenameNode — title is forwarded", async () => {
  replyPayload = { ok: true, title: "Trees" };
  await pcg.pcgRenameNode({
    graph_path: "/Game/PCG/X", node: "Sampler_0", new_title: "Trees",
  });
  assert.equal(argsOf(capturedScripts[0].command).title, "Trees");
});

// ---------------------------------------------------------------------------
// pcgAddComponentToActor / pcgSetComponentProperty / pcgGenerateComponent /
// pcgCleanupComponent
// ---------------------------------------------------------------------------

test("pcgAddComponentToActor — actor label + graph path are forwarded", async () => {
  replyPayload = { ok: true, actor: "BP_PCGActor_1", graph: "/Game/PCG/X", component_name: "PCGComponent_0" };
  await pcg.pcgAddComponentToActor({
    actor_label: "BP_PCGActor_1",
    graph_path:  "/Game/PCG/X",
  });
  const a = argsOf(capturedScripts[0].command);
  assert.equal(a.actor, "BP_PCGActor_1");
  assert.equal(a.graph, "/Game/PCG/X");
});

test("pcgSetComponentProperty — actor + property + value are forwarded", async () => {
  replyPayload = { ok: true, property: "seed", value: "42" };
  await pcg.pcgSetComponentProperty({
    actor_label: "BP_PCGActor_1",
    property:    "seed",
    value:       42,
  });
  const a = argsOf(capturedScripts[0].command);
  assert.equal(a.property, "seed");
  assert.equal(a.value, 42);
});

test("pcgSetComponentProperty — string value preserved for enum coercion on Python side", async () => {
  replyPayload = { ok: true };
  await pcg.pcgSetComponentProperty({
    actor_label: "BP_PCGActor_1",
    property:    "generation_trigger",
    value:       "GenerateOnLoad",
  });
  assert.equal(argsOf(capturedScripts[0].command).value, "GenerateOnLoad");
});

test("pcgGenerateComponent — force flag defaults to true and is forwarded", async () => {
  replyPayload = { ok: true };
  await pcg.pcgGenerateComponent({ actor_label: "BP_PCGActor_1" });
  assert.equal(argsOf(capturedScripts[0].command).force, true);
});

test("pcgGenerateComponent — explicit force=false is forwarded", async () => {
  replyPayload = { ok: true };
  await pcg.pcgGenerateComponent({ actor_label: "BP_PCGActor_1", force: false });
  assert.equal(argsOf(capturedScripts[0].command).force, false);
});

test("pcgCleanupComponent — remove_components flag defaults to true", async () => {
  replyPayload = { ok: true };
  await pcg.pcgCleanupComponent({ actor_label: "BP_PCGActor_1" });
  assert.equal(argsOf(capturedScripts[0].command).rm, true);
});

// ---------------------------------------------------------------------------
// pcgSetGraphProperty / pcgAddGraphParameter / pcgListGraphParameters /
// pcgRemoveGraphParameter
// ---------------------------------------------------------------------------

test("pcgSetGraphProperty — property + value forwarded; auto-wrap of Text handled in Python", async () => {
  replyPayload = { ok: true, property: "description", value: "Forest biome graph" };
  await pcg.pcgSetGraphProperty({
    graph_path: "/Game/PCG/X",
    property:   "description",
    value:      "Forest biome graph",
  });
  assert.equal(argsOf(capturedScripts[0].command).property, "description");
});

test("pcgAddGraphParameter — name + type forwarded", async () => {
  replyPayload = { ok: true, name: "Density", type: "float" };
  await pcg.pcgAddGraphParameter({
    graph_path: "/Game/PCG/X",
    name:       "Density",
    type:       "float",
  });
  const a = argsOf(capturedScripts[0].command);
  assert.equal(a.name, "Density");
  assert.equal(a.type, "float");
});

test("pcgListGraphParameters — graph path forwarded; parameter array surfaces", async () => {
  replyPayload = {
    ok: true,
    graph: "/Game/PCG/X",
    parameters: [
      { name: "Density",    type: "Float" },
      { name: "TreeMesh",   type: "Object" },
    ],
    count: 2,
  };
  const mcp = await pcg.pcgListGraphParameters({ graph_path: "/Game/PCG/X" });
  const parsed = JSON.parse(mcp.content[0].text);
  assert.equal(parsed.count, 2);
});

test("pcgRemoveGraphParameter — name is forwarded", async () => {
  replyPayload = { ok: true, removed: "Density" };
  await pcg.pcgRemoveGraphParameter({ graph_path: "/Game/PCG/X", name: "Density" });
  assert.equal(argsOf(capturedScripts[0].command).name, "Density");
});

// ---------------------------------------------------------------------------
// pcgCreateGraphInstance / pcgSetGraphInstanceParameter
// ---------------------------------------------------------------------------

test("pcgCreateGraphInstance — base + path + overwrite forwarded", async () => {
  replyPayload = { ok: true, path: "/Game/PCG/InstA", base_graph: "/Game/PCG/X" };
  await pcg.pcgCreateGraphInstance({
    asset_path: "/Game/PCG/InstA",
    base_graph: "/Game/PCG/X",
    overwrite:  true,
  });
  const a = argsOf(capturedScripts[0].command);
  assert.equal(a.path, "/Game/PCG/InstA");
  assert.equal(a.base, "/Game/PCG/X");
  assert.equal(a.overwrite, true);
});

test("pcgSetGraphInstanceParameter — primitive value path", async () => {
  replyPayload = { ok: true };
  await pcg.pcgSetGraphInstanceParameter({
    instance_path: "/Game/PCG/InstA",
    name:          "Density",
    value:         0.75,
  });
  const a = argsOf(capturedScripts[0].command);
  assert.equal(a.inst, "/Game/PCG/InstA");
  assert.equal(a.name, "Density");
  assert.equal(a.value, 0.75);
});

test("pcgSetGraphInstanceParameter — asset_value path forwards content reference", async () => {
  replyPayload = { ok: true };
  await pcg.pcgSetGraphInstanceParameter({
    instance_path: "/Game/PCG/InstA",
    name:          "TreeMesh",
    asset_value:   "/Game/Meshes/SM_Oak",
  });
  assert.equal(argsOf(capturedScripts[0].command).asset_value, "/Game/Meshes/SM_Oak");
});

// ---------------------------------------------------------------------------
// pcgOpenInEditor / pcgRegenerateAll
// ---------------------------------------------------------------------------

test("pcgOpenInEditor — payload carries graph path", async () => {
  replyPayload = { ok: true };
  await pcg.pcgOpenInEditor({ graph_path: "/Game/PCG/X" });
  assert.equal(argsOf(capturedScripts[0].command).graph, "/Game/PCG/X");
});

test("pcgRegenerateAll — filter + force are forwarded; defaults when omitted", async () => {
  replyPayload = { ok: true, count: 3, actors: ["A", "B", "C"] };
  await pcg.pcgRegenerateAll();
  const a1 = argsOf(capturedScripts[0].command);
  // Default: no filter, force=true.
  assert.equal(a1.filter, null);
  assert.equal(a1.force, true);

  // With filter override.
  replyPayload = { ok: true, count: 1, actors: ["A"] };
  await pcg.pcgRegenerateAll({ graph_filter: "/Game/PCG/X", force: false });
  const a2 = argsOf(capturedScripts[1].command);
  assert.equal(a2.filter, "/Game/PCG/X");
  assert.equal(a2.force, false);
});
