// Copyright 2025-2026 NGG. All Rights Reserved.
// newtools.test.js — unit coverage for the pure helpers in the rendering,
// interchange, substrate and landscape modules. Everything else in those files
// runs inside the editor and is covered by their live suites.

import { test } from "node:test";
import assert from "node:assert/strict";

import { isValidCvarKey, isValidCvarValue, REPORTED_CVARS, RENDERER_SETTINGS_SECTION } from "./rendering.js";
import { mergeSettings, IMPORT_SHORTCUTS } from "./interchange.js";
import { SHADING_MODELS } from "./substrate.js";
import { NO_LANDSCAPE_MESSAGE } from "./landscape.js";

test("renderer setting names accept real cvars", () => {
  for (const name of REPORTED_CVARS) {
    assert.equal(isValidCvarKey(name), true, `${name} should be a valid key`);
  }
  assert.equal(isValidCvarKey("r.Substrate"), true);
  assert.equal(isValidCvarKey("bDefaultFeatureAutoExposure"), true);
});

test("renderer setting names reject anything that could forge ini structure", () => {
  // These land verbatim in DefaultEngine.ini and in a console command.
  assert.equal(isValidCvarKey("r.Foo]\n[Evil"), false);
  assert.equal(isValidCvarKey("r.Foo Bar"), false);
  assert.equal(isValidCvarKey("[Section]"), false);
  assert.equal(isValidCvarKey(""), false);
  assert.equal(isValidCvarKey("1LeadingDigit"), false);
  assert.equal(isValidCvarKey(null), false);
  assert.equal(isValidCvarKey("r." + "x".repeat(200)), false);
});

test("renderer setting values reject line breaks and brackets", () => {
  assert.equal(isValidCvarValue(1), true);
  assert.equal(isValidCvarValue(true), true);
  assert.equal(isValidCvarValue("Lumen"), true);
  assert.equal(isValidCvarValue(null), true, "null means 'remove the key'");
  assert.equal(isValidCvarValue("1\nBar=2"), false);
  assert.equal(isValidCvarValue("1\r\nBar=2"), false);
  assert.equal(isValidCvarValue("a]b"), false);
});

test("the renderer settings section is the one UE reads", () => {
  assert.equal(RENDERER_SETTINGS_SECTION, "/Script/Engine.RendererSettings");
});

test("interchange shortcuts map onto dotted pipeline paths", () => {
  const merged = mergeSettings({ build_nanite: true, import_lods: false }, {});
  assert.deepEqual(merged, {
    "mesh_pipeline.build_nanite": true,
    "common_meshes_properties.import_lods": false,
  });
});

test("a shortcut set to false is kept, not treated as absent", () => {
  const merged = mergeSettings({ import_materials: false }, {});
  assert.equal(merged["material_pipeline.import_materials"], false);
});

test("undefined and null shortcuts are dropped", () => {
  const merged = mergeSettings({ build_nanite: undefined, import_lods: null }, {});
  assert.deepEqual(merged, {});
});

test("explicit pipeline_settings win over a shortcut for the same property", () => {
  const merged = mergeSettings(
    { build_nanite: true },
    { "mesh_pipeline.build_nanite": false }
  );
  assert.equal(merged["mesh_pipeline.build_nanite"], false);
});

test("unrelated arguments are not swept into the settings map", () => {
  const merged = mergeSettings(
    { asset_path: "/Game/X", overwrite: true, source_file: "C:/a.fbx", build_nanite: true },
    {}
  );
  assert.deepEqual(Object.keys(merged), ["mesh_pipeline.build_nanite"]);
});

test("no shortcut points at a property 5.8 deprecated", () => {
  // Reading a deprecated property throws in UE 5.8, so a shortcut aimed at one
  // would fail every call. These three were renamed; see interchange.js.
  const deprecated = [
    "mesh_pipeline.import_collision",
    "mesh_pipeline.combine_static_meshes",
    "common_meshes_properties.auto_detect_mesh_type",
  ];
  for (const path of Object.values(IMPORT_SHORTCUTS)) {
    assert.equal(deprecated.includes(path), false, `${path} is deprecated in UE 5.8`);
  }
});

test("every shortcut path is either root-level or group.property", () => {
  for (const [name, path] of Object.entries(IMPORT_SHORTCUTS)) {
    const parts = path.split(".");
    assert.ok(parts.length <= 2, `${name} -> ${path} nests deeper than the resolver expects`);
    assert.ok(parts.every(p => p.length > 0), `${name} -> ${path} has an empty segment`);
  }
});

test("substrate advertises the shading models the tool descriptions name", () => {
  for (const name of ["slab", "unlit", "clearcoat", "toon", "hair", "eye", "water"]) {
    assert.ok(SHADING_MODELS.includes(name), `${name} missing from SHADING_MODELS`);
  }
});

test("the no-landscape message says why and what to do instead", () => {
  // This is the whole value of the landscape read tool on a project without
  // one: it has to stop the caller looking for a create API that isn't there.
  assert.match(NO_LANDSCAPE_MESSAGE, /cannot create one from script/);
  assert.match(NO_LANDSCAPE_MESSAGE, /Landscape mode/);
});
