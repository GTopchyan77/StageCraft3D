// Copyright 2025-2026 NGG. All Rights Reserved.
// toolsets.test.js — unit tests for tool-catalog grouping and filtering.

import { test } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import {
  toolsetFor,
  resolveToolsets,
  assertToolsetCoverage,
  ALL_TOOLSETS,
  ALWAYS_ON,
} from "./toolsets.js";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const INDEX_SRC = fs.readFileSync(path.join(__dirname, "index.js"), "utf8");

function registeredToolNames() {
  return [...INDEX_SRC.matchAll(/server\.tool\(\s*"([A-Za-z][A-Za-z0-9_]*)"/g)].map(m => m[1]);
}

test("every registered tool matches an explicit toolset rule", () => {
  // toolsetFor() falls back to "core" for unknown names so a new tool is never
  // dropped from every config — but landing in that fallback silently would
  // grow the always-on set. Adding a tool means adding a rule.
  const uncovered = assertToolsetCoverage(registeredToolNames());
  assert.deepStrictEqual(
    uncovered,
    [],
    `these tools have no toolset rule and would silently join "core":\n  ${uncovered.join("\n  ")}`,
  );
});

test("toolset assignment is stable for the families it groups", () => {
  const expected = {
    pcg_create_graph:            "pcg",
    pcg_set_node_position:       "pcg",
    ue5_gas_create_ability:      "gas",
    ue5_bt_add_logic:            "ai",
    ue5_st_read_tree:            "ai",
    ue5_profile_trace_start:     "profiling",
    ue5_mesh_boolean:            "mesh",
    ue5_meshy_generate:          "meshy",   // must not be swallowed by ue5_mesh_
    ue5_anim_add_look_at:        "anim",
    ue5_create_anim_blueprint:   "anim",
    ue5_create_niagara_system:   "niagara",
    ue5_sequencer_create:        "sequencer",
    ue5_datatable_read:          "datatable",
    ue5_create_sound_cue:        "audio",
    ue5_material_connect:        "material",
    ue5_style_widgets:           "umg",
    ue5_bp_add_logic:            "blueprint",
    ue5_create_blueprint:        "blueprint",
    ue5_spawn_actor:             "level",
    ue5_pie_start:               "level",
    ue5_configure_imc:           "input",
    ue5_add_gameplay_tags:       "gameplaytags",
    ue5_create_exercise:         "exercises",
    youtube_get_transcript:      "youtube",
    // core: needed by any session regardless of subsystem
    ue5_health_check:            "core",
    ue5_save_all:                "core",
    ue5_list_assets:             "core",
    ue5_batch:                   "core",
    ue5_set_asset_property:      "core",
  };
  for (const [tool, set] of Object.entries(expected)) {
    assert.equal(toolsetFor(tool), set, `${tool} landed in "${toolsetFor(tool)}", expected "${set}"`);
  }
});

test("ue5_meshy_generate is not captured by the mesh rule", () => {
  // Regression: /^ue5_mesh_/ would match ue5_meshy_... if written carelessly,
  // silently moving the Meshy API tool into the geometry set.
  assert.equal(toolsetFor("ue5_meshy_generate"), "meshy");
  assert.equal(toolsetFor("ue5_mesh_create"),    "mesh");
});

test("no configuration registers everything by default", () => {
  const r = resolveToolsets({});
  assert.equal(r.mode, "all");
  assert.deepEqual([...r.enabled].sort(), [...ALL_TOOLSETS].sort());
  assert.deepEqual(r.unknown, []);
});

test("NGG_TOOLSETS acts as an allowlist and always keeps core", () => {
  const r = resolveToolsets({ NGG_TOOLSETS: "blueprint,umg" });
  assert.equal(r.mode, "allowlist");
  assert.ok(r.enabled.has("blueprint"));
  assert.ok(r.enabled.has("umg"));
  assert.ok(r.enabled.has("core"), "core must survive an allowlist that omits it");
  assert.ok(!r.enabled.has("pcg"));
});

test("NGG_TOOLSETS_EXCLUDE acts as a denylist", () => {
  const r = resolveToolsets({ NGG_TOOLSETS_EXCLUDE: "pcg, gas" });
  assert.equal(r.mode, "denylist");
  assert.ok(!r.enabled.has("pcg"));
  assert.ok(!r.enabled.has("gas"));
  assert.ok(r.enabled.has("blueprint"));
  assert.ok(r.enabled.has("core"));
});

test("core cannot be excluded", () => {
  // Dropping health_check / save_all / list_assets would leave a session unable
  // to do anything at all, including diagnose itself.
  for (const set of ALWAYS_ON) {
    const r = resolveToolsets({ NGG_TOOLSETS_EXCLUDE: set });
    assert.ok(r.enabled.has(set), `${set} was excluded despite being always-on`);
  }
});

test("an allowlist wins over a denylist when both are set", () => {
  const r = resolveToolsets({ NGG_TOOLSETS: "pcg", NGG_TOOLSETS_EXCLUDE: "pcg" });
  assert.equal(r.mode, "allowlist");
  assert.ok(r.enabled.has("pcg"), "the allowlist should decide when both are present");
});

test("unknown set names are reported, not silently ignored", () => {
  const r = resolveToolsets({ NGG_TOOLSETS: "core,nonsense,alsobogus" });
  assert.deepEqual(r.unknown.sort(), ["alsobogus", "nonsense"]);
  assert.ok(r.enabled.has("core"));
  assert.ok(!r.enabled.has("nonsense"));
});

test("separators and casing are forgiving", () => {
  const a = resolveToolsets({ NGG_TOOLSETS: "BluePrint, UMG" });
  assert.ok(a.enabled.has("blueprint") && a.enabled.has("umg"));

  const b = resolveToolsets({ NGG_TOOLSETS: "blueprint umg" });
  assert.ok(b.enabled.has("blueprint") && b.enabled.has("umg"));

  const c = resolveToolsets({ NGG_TOOLSETS: "  " });
  assert.equal(c.mode, "all", "a whitespace-only value must not register an empty catalog");
});

test("every declared toolset actually has tools in it", () => {
  // A set nobody can select is dead config surface.
  const names = registeredToolNames();
  const counts = new Map(ALL_TOOLSETS.map(s => [s, 0]));
  for (const n of names) counts.set(toolsetFor(n), (counts.get(toolsetFor(n)) ?? 0) + 1);
  const empty = [...counts.entries()].filter(([, n]) => n === 0).map(([s]) => s);
  assert.deepStrictEqual(empty, [], `toolsets with no tools: ${empty.join(", ")}`);
});

test("the toolset partition is complete and non-overlapping", () => {
  const names = registeredToolNames();
  const total = ALL_TOOLSETS.reduce(
    (sum, set) => sum + names.filter(n => toolsetFor(n) === set).length,
    0,
  );
  assert.equal(total, names.length, "tools are counted in more than one set, or in none");
});
