#!/usr/bin/env node
// Copyright 2025-2026 NGG. All Rights Reserved.
// pcg.test.mjs — smoke test for the Phase A PCG endpoints.
// Hits the running UE editor via the same code path the MCP tools use.
//
// Usage:
//   node Plugins/UnrealNGGMCP/Source/ThirdParty/unrealngg-mcp/pcg.test.mjs
//   node Plugins/UnrealNGGMCP/Source/ThirdParty/unrealngg-mcp/pcg.test.mjs --keep   # don't delete the test graph at the end

import * as pcg from "./pcg.js";
import * as ue5 from "./ue5client.js";
import { safeDeleteAssets } from "./pcg.test.util.mjs";

const GRAPH_PATH = "/Game/PCG/Tests_PhaseA";
const KEEP = process.argv.includes("--keep");

let pass = 0;
let fail = 0;
const results = [];

function header(name) {
  const line = `\n━━━ ${name} ${"━".repeat(Math.max(2, 60 - name.length))}`;
  process.stdout.write(line + "\n");
}

function unwrap(mcp) {
  const payload = mcp?.content?.[0]?.text;
  if (mcp?.isError) return { ok: false, error: payload };
  try {
    return { ok: true, data: JSON.parse(payload) };
  } catch {
    return { ok: true, data: payload };
  }
}

async function check(name, fn) {
  try {
    const r = await fn();
    if (r === false) {
      process.stdout.write(`FAIL  ${name}\n`);
      fail++;
      results.push({ name, ok: false });
      return false;
    }
    process.stdout.write(`PASS  ${name}\n`);
    pass++;
    results.push({ name, ok: true });
    return true;
  } catch (err) {
    process.stdout.write(`FAIL  ${name}\n        ${err.stack ?? err.message}\n`);
    fail++;
    results.push({ name, ok: false, error: err.message });
    return false;
  }
}

// ---------------------------------------------------------------------------

header("Prerequisites");

await check("health check returns ok", async () => {
  const h = await ue5.healthCheck();
  if (h?.status !== "ok") throw new Error(`unexpected status: ${JSON.stringify(h)}`);
  // NGG_EXPECTED_PROJECT pins the project name if set; otherwise accept any
  // live editor (the smoke test is already anchored by the graph path below).
  const expected = process.env.NGG_EXPECTED_PROJECT;
  if (expected && h?.project !== expected) {
    throw new Error(`unexpected project: ${h.project} (expected ${expected})`);
  }
  return true;
});

header("pcg_list_node_types");

let allTypes;
await check("full list returns >= 50 classes", async () => {
  const r = unwrap(await pcg.pcgListNodeTypes({}));
  if (!r.ok) throw new Error(r.error);
  allTypes = r.data.classes;
  if (!Array.isArray(allTypes)) throw new Error("classes is not an array");
  if (allTypes.length < 50) throw new Error(`only ${allTypes.length} classes returned`);
  return true;
});

await check("list includes PCGSurfaceSamplerSettings", async () => {
  if (!allTypes?.some((c) => c.name === "PCGSurfaceSamplerSettings")) {
    throw new Error("surface sampler missing from list");
  }
  return true;
});

await check("every entry has {name, category}", async () => {
  for (const c of allTypes) {
    if (!c.name || !c.category) throw new Error(`malformed entry: ${JSON.stringify(c)}`);
  }
  return true;
});

await check("category filter narrows list", async () => {
  const r = unwrap(await pcg.pcgListNodeTypes({ category: "Samplers" }));
  if (!r.ok) throw new Error(r.error);
  if (r.data.count < 3 || r.data.count > 6) {
    throw new Error(`expected ~4 samplers, got ${r.data.count}`);
  }
  for (const c of r.data.classes) {
    if (c.category !== "Samplers") throw new Error(`wrong category in filter: ${c.category}`);
  }
  return true;
});

header("pcg_create_graph");

await check("create fresh graph (overwrite=true)", async () => {
  const r = unwrap(await pcg.pcgCreateGraph({
    asset_path: GRAPH_PATH,
    standalone: false,
    overwrite: true,
  }));
  if (!r.ok) throw new Error(r.error);
  if (r.data.path !== GRAPH_PATH) throw new Error(`wrong path returned: ${r.data.path}`);
  return true;
});

await check("create without overwrite fails when asset exists", async () => {
  const r = unwrap(await pcg.pcgCreateGraph({
    asset_path: GRAPH_PATH,
    overwrite: false,
  }));
  if (r.ok) throw new Error("expected error, got success");
  if (!/already exists/i.test(r.error)) throw new Error(`wrong error: ${r.error}`);
  return true;
});

header("pcg_add_node");

let samplerName, transformName;

await check("add surface sampler", async () => {
  const r = unwrap(await pcg.pcgAddNode({
    graph_path: GRAPH_PATH,
    settings_class: "PCGSurfaceSamplerSettings",
  }));
  if (!r.ok) throw new Error(r.error);
  samplerName = r.data.node_name;
  if (!/^SurfaceSampler/.test(samplerName)) {
    throw new Error(`unexpected node name: ${samplerName}`);
  }
  return true;
});

await check("add transform points", async () => {
  const r = unwrap(await pcg.pcgAddNode({
    graph_path: GRAPH_PATH,
    settings_class: "PCGTransformPointsSettings",
  }));
  if (!r.ok) throw new Error(r.error);
  transformName = r.data.node_name;
  if (!/^TransformPoints/.test(transformName)) {
    throw new Error(`unexpected node name: ${transformName}`);
  }
  return true;
});

await check("add_node with unknown class returns error", async () => {
  const r = unwrap(await pcg.pcgAddNode({
    graph_path: GRAPH_PATH,
    settings_class: "PCGDoesNotExistSettings",
  }));
  if (r.ok) throw new Error("expected error, got success");
  if (!/unknown PCG settings class/i.test(r.error)) throw new Error(`wrong error: ${r.error}`);
  return true;
});

header("pcg_connect_pins");

await check("connect input → sampler", async () => {
  const r = unwrap(await pcg.pcgConnectPins({
    graph_path: GRAPH_PATH,
    from_node: "__input__",
    from_pin: "In",
    to_node: samplerName,
    to_pin: "In",
  }));
  if (!r.ok) throw new Error(r.error);
  return true;
});

await check("connect sampler → transform", async () => {
  const r = unwrap(await pcg.pcgConnectPins({
    graph_path: GRAPH_PATH,
    from_node: samplerName,
    from_pin: "Out",
    to_node: transformName,
    to_pin: "In",
  }));
  if (!r.ok) throw new Error(r.error);
  return true;
});

await check("connect transform → output", async () => {
  const r = unwrap(await pcg.pcgConnectPins({
    graph_path: GRAPH_PATH,
    from_node: transformName,
    from_pin: "Out",
    to_node: "__output__",
    to_pin: "Out",
  }));
  if (!r.ok) throw new Error(r.error);
  return true;
});

await check("connect with unknown from_node errors", async () => {
  const r = unwrap(await pcg.pcgConnectPins({
    graph_path: GRAPH_PATH,
    from_node: "DoesNotExist_0",
    from_pin: "Out",
    to_node: transformName,
    to_pin: "In",
  }));
  if (r.ok) throw new Error("expected error, got success");
  if (!/from_node not found/i.test(r.error)) throw new Error(`wrong error: ${r.error}`);
  return true;
});

header("pcg_save_graph");

await check("save returns ok", async () => {
  const r = unwrap(await pcg.pcgSaveGraph({ graph_path: GRAPH_PATH }));
  if (!r.ok) throw new Error(r.error);
  if (!r.data.ok) throw new Error(`save returned ok=false: ${JSON.stringify(r.data)}`);
  return true;
});

header("Introspection via exec_python");

await check("graph has 2 user nodes on disk", async () => {
  const resp = await ue5.execPython(
    `import unreal\n` +
      `g = unreal.EditorAssetLibrary.load_asset("${GRAPH_PATH}")\n` +
      `nodes = g.get_editor_property("nodes")\n` +
      `print("COUNT=%d" % len(nodes))\n` +
      `for n in nodes: print("NODE=%s" % n.get_name())`,
    "execute_file"
  );
  const log = (resp.log ?? []).map((e) => e.output).join("");
  const countMatch = log.match(/COUNT=(\d+)/);
  if (!countMatch) throw new Error(`no COUNT line: ${log}`);
  const n = parseInt(countMatch[1], 10);
  if (n !== 2) throw new Error(`expected 2 nodes, got ${n}. log=${log}`);
  return true;
});

// ---------------------------------------------------------------------------

header("Cleanup");
if (!KEEP) {
  // Use the hardened safe-delete: closes asset editors, GCs, plural-deletes,
  // then renames to /Game/PCG/_TRASH_* if native refs still pin the asset.
  // Either outcome frees the original content path for re-use on next run.
  await check("safe-delete test graph", async () => {
    const r = await safeDeleteAssets([GRAPH_PATH]);
    if ((r.failed ?? []).includes(GRAPH_PATH)) {
      throw new Error(`asset still occupies ${GRAPH_PATH}`);
    }
    if ((r.trashed ?? []).length) {
      process.stdout.write(`        (trashed: ${r.trashed.map((t) => t.to).join(", ")})\n`);
    }
    return true;
  });
} else {
  process.stdout.write(`(skipped — --keep flag set; asset left at ${GRAPH_PATH})\n`);
}

// ---------------------------------------------------------------------------

const total = pass + fail;
process.stdout.write(`\n━━━ Summary: ${pass}/${total} passed, ${fail} failed ━━━\n`);
process.exit(fail === 0 ? 0 : 1);
