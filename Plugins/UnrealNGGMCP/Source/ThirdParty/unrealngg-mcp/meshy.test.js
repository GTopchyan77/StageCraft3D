// Copyright 2025-2026 NGG. All Rights Reserved.
// meshy.test.js — unit tests for meshy.resolveApiKey.
// Covers the three credential resolution paths (env var, .meshy.json file,
// neither). Doesn't touch the live Meshy API.
//
// Run: node --test meshy.test.js

import { test, before, after, beforeEach } from "node:test";
import assert from "node:assert/strict";
import fs from "fs";
import os from "os";
import path from "path";

import { resolveApiKey } from "./meshy.js";

let tmpRoot;
let savedEnvKey;

before(() => {
  tmpRoot = fs.mkdtempSync(path.join(os.tmpdir(), "ngg-meshy-test-"));
  savedEnvKey = process.env.MESHY_API_KEY;
});

after(() => {
  fs.rmSync(tmpRoot, { recursive: true, force: true });
  if (savedEnvKey === undefined) delete process.env.MESHY_API_KEY;
  else process.env.MESHY_API_KEY = savedEnvKey;
});

beforeEach(() => {
  delete process.env.MESHY_API_KEY;
});

// ---------------------------------------------------------------------------

test("resolveApiKey — uses MESHY_API_KEY env var when set", () => {
  process.env.MESHY_API_KEY = "msy-from-env";
  const r = resolveApiKey(tmpRoot);
  assert.equal(r.key, "msy-from-env");
  assert.equal(r.source, "env:MESHY_API_KEY");
});

test("resolveApiKey — trims whitespace around env var value", () => {
  process.env.MESHY_API_KEY = "   msy-padded   ";
  const r = resolveApiKey(tmpRoot);
  assert.equal(r.key, "msy-padded");
});

test("resolveApiKey — falls back to .meshy.json when env var is absent", () => {
  const projectRoot = fs.mkdtempSync(path.join(tmpRoot, "proj-"));
  fs.writeFileSync(
    path.join(projectRoot, ".meshy.json"),
    JSON.stringify({ api_key: "msy-from-file" }),
    "utf8",
  );
  const r = resolveApiKey(projectRoot);
  assert.equal(r.key, "msy-from-file");
  assert.match(r.source, /^file:/);
  assert.ok(r.source.includes(".meshy.json"));
});

test("resolveApiKey — env var wins over config file", () => {
  process.env.MESHY_API_KEY = "msy-env-wins";
  const projectRoot = fs.mkdtempSync(path.join(tmpRoot, "proj-"));
  fs.writeFileSync(
    path.join(projectRoot, ".meshy.json"),
    JSON.stringify({ api_key: "msy-loser" }),
    "utf8",
  );
  const r = resolveApiKey(projectRoot);
  assert.equal(r.key, "msy-env-wins");
  assert.equal(r.source, "env:MESHY_API_KEY");
});

test("resolveApiKey — ignores empty env var and falls through to file", () => {
  process.env.MESHY_API_KEY = "   ";
  const projectRoot = fs.mkdtempSync(path.join(tmpRoot, "proj-"));
  fs.writeFileSync(
    path.join(projectRoot, ".meshy.json"),
    JSON.stringify({ api_key: "msy-rescue" }),
    "utf8",
  );
  const r = resolveApiKey(projectRoot);
  assert.equal(r.key, "msy-rescue");
});

test("resolveApiKey — throws helpful error when neither source is available", () => {
  const bareProject = fs.mkdtempSync(path.join(tmpRoot, "bare-"));
  assert.throws(
    () => resolveApiKey(bareProject),
    (err) => /MESHY_API_KEY/.test(err.message) && /\.meshy\.json/.test(err.message),
  );
});

test("resolveApiKey — throws when .meshy.json is malformed", () => {
  const projectRoot = fs.mkdtempSync(path.join(tmpRoot, "bad-"));
  fs.writeFileSync(path.join(projectRoot, ".meshy.json"), "not json", "utf8");
  assert.throws(
    () => resolveApiKey(projectRoot),
    (err) => /Failed to read/.test(err.message) && /\.meshy\.json/.test(err.message),
  );
});

test("resolveApiKey — treats empty api_key in file as 'not set' (falls through to error)", () => {
  const projectRoot = fs.mkdtempSync(path.join(tmpRoot, "empty-"));
  fs.writeFileSync(
    path.join(projectRoot, ".meshy.json"),
    JSON.stringify({ api_key: "   " }),
    "utf8",
  );
  assert.throws(() => resolveApiKey(projectRoot), /MESHY_API_KEY/);
});

test("resolveApiKey — ignores file when api_key field is missing entirely", () => {
  const projectRoot = fs.mkdtempSync(path.join(tmpRoot, "noKey-"));
  fs.writeFileSync(
    path.join(projectRoot, ".meshy.json"),
    JSON.stringify({ other_setting: true }),
    "utf8",
  );
  assert.throws(() => resolveApiKey(projectRoot), /MESHY_API_KEY/);
});
