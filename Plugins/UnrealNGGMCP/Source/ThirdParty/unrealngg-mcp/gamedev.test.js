// Copyright 2025-2026 NGG. All Rights Reserved.
// gamedev.test.js — unit tests for the Node-side parts of gamedev.js
// (log reading, gameplay-tag ini editing, input validation). The Python-backed
// functions are covered by the live suite (gamedev.live.test.mjs) since their
// behavior lives inside the editor.
//
// Run: node --test gamedev.test.js

import { test, beforeEach, after } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";

import * as gamedev from "./gamedev.js";

const tmpDirs = [];
function makeTmpDir(prefix) {
  const d = fs.mkdtempSync(path.join(os.tmpdir(), prefix));
  tmpDirs.push(d);
  return d;
}

after(() => {
  for (const d of tmpDirs) fs.rmSync(d, { recursive: true, force: true });
});

beforeEach(() => {
  // Point the bridge at a dead port so best-effort editor calls (e.g. the
  // gameplay-tag live refresh) fail fast instead of hitting a real editor.
  process.env.NGG_BRIDGE_URL = "http://127.0.0.1:1";
  process.env.NGG_BRIDGE_TIMEOUT_MS = "300";
});

// ---------------------------------------------------------------------------
// getLog
// ---------------------------------------------------------------------------

function writeLog(savedDir, lines) {
  const logsDir = path.join(savedDir, "Logs");
  fs.mkdirSync(logsDir, { recursive: true });
  fs.writeFileSync(path.join(logsDir, "MyGame.log"), lines.join("\n"), "utf8");
}

test("getLog returns the tail of the newest log file", async () => {
  const saved = makeTmpDir("ngg-gd-log-");
  writeLog(saved, ["line1", "line2", "line3", "line4"]);
  const r = await gamedev.getLog({ lines: 2 }, saved);
  assert.equal(r.ok, true);
  assert.deepEqual(r.data.lines, ["line3", "line4"]);
  assert.equal(r.data.matched_lines, 4);
});

test("getLog severity=error keeps only error lines", async () => {
  const saved = makeTmpDir("ngg-gd-log-");
  writeLog(saved, [
    "[2026.07.04-10.00.00:000][  0]LogTemp: Display: fine",
    "[2026.07.04-10.00.01:000][  1]LogBlueprint: Error: broken node",
    "[2026.07.04-10.00.02:000][  2]LogTemp: Warning: eh",
  ]);
  const r = await gamedev.getLog({ severity: "error" }, saved);
  assert.equal(r.ok, true);
  assert.equal(r.data.lines.length, 1);
  assert.match(r.data.lines[0], /broken node/);
});

test("getLog severity=warning keeps warnings and errors", async () => {
  const saved = makeTmpDir("ngg-gd-log-");
  writeLog(saved, [
    "LogTemp: Display: fine",
    "LogBlueprint: Error: broken",
    "LogTemp: Warning: eh",
  ]);
  const r = await gamedev.getLog({ severity: "warning" }, saved);
  assert.equal(r.ok, true);
  assert.equal(r.data.lines.length, 2);
});

test("getLog category + contains filters compose", async () => {
  const saved = makeTmpDir("ngg-gd-log-");
  writeLog(saved, [
    "LogNiagara: Display: spawned emitter",
    "LogBlueprint: Display: compiled BP_Door",
    "LogBlueprint: Display: compiled BP_Window",
  ]);
  const r = await gamedev.getLog({ category: "LogBlueprint", contains: "bp_door" }, saved);
  assert.equal(r.ok, true);
  assert.equal(r.data.lines.length, 1);
  assert.match(r.data.lines[0], /BP_Door/);
});

test("getLog errors cleanly when the Logs dir is missing", async () => {
  const saved = makeTmpDir("ngg-gd-log-");
  const r = await gamedev.getLog({}, saved);
  assert.equal(r.ok, false);
  assert.match(r.error, /cannot read/);
});

test("getLog picks the most recently modified log file", async () => {
  const saved = makeTmpDir("ngg-gd-log-");
  const logsDir = path.join(saved, "Logs");
  fs.mkdirSync(logsDir, { recursive: true });
  fs.writeFileSync(path.join(logsDir, "Old.log"), "old line", "utf8");
  const past = new Date(Date.now() - 60_000);
  fs.utimesSync(path.join(logsDir, "Old.log"), past, past);
  fs.writeFileSync(path.join(logsDir, "New.log"), "new line", "utf8");
  const r = await gamedev.getLog({}, saved);
  assert.equal(r.ok, true);
  assert.match(r.data.log_file, /New\.log$/);
  assert.deepEqual(r.data.lines, ["new line"]);
});

// ---------------------------------------------------------------------------
// addGameplayTags
// ---------------------------------------------------------------------------

test("addGameplayTags creates the ini with section + tag lines", async () => {
  const proj = makeTmpDir("ngg-gd-proj-");
  fs.mkdirSync(path.join(proj, "Config"), { recursive: true });
  const r = await gamedev.addGameplayTags(
    { tags: [{ tag: "Ability.Dash", comment: "dash it" }] },
    proj
  );
  assert.equal(r.ok, true);
  assert.deepEqual(r.data.added, ["Ability.Dash"]);
  const ini = fs.readFileSync(path.join(proj, "Config", "DefaultGameplayTags.ini"), "utf8");
  assert.match(ini, /\[\/Script\/GameplayTags\.GameplayTagsSettings\]/);
  assert.match(ini, /\+GameplayTagList=\(Tag="Ability\.Dash",DevComment="dash it"\)/);
});

test("addGameplayTags skips tags that already exist", async () => {
  const proj = makeTmpDir("ngg-gd-proj-");
  fs.mkdirSync(path.join(proj, "Config"), { recursive: true });
  fs.writeFileSync(
    path.join(proj, "Config", "DefaultGameplayTags.ini"),
    '[/Script/GameplayTags.GameplayTagsSettings]\n+GameplayTagList=(Tag="Ability.Dash",DevComment="")\n',
    "utf8"
  );
  const r = await gamedev.addGameplayTags(
    { tags: [{ tag: "Ability.Dash" }, { tag: "Ability.Slide" }] },
    proj
  );
  assert.equal(r.ok, true);
  assert.deepEqual(r.data.added, ["Ability.Slide"]);
  assert.deepEqual(r.data.skipped_existing, ["Ability.Dash"]);
});

test("addGameplayTags appends the settings section to a foreign ini", async () => {
  const proj = makeTmpDir("ngg-gd-proj-");
  fs.mkdirSync(path.join(proj, "Config"), { recursive: true });
  fs.writeFileSync(
    path.join(proj, "Config", "DefaultGameplayTags.ini"),
    "[SomeOtherSection]\nFoo=Bar\n",
    "utf8"
  );
  const r = await gamedev.addGameplayTags({ tags: [{ tag: "UI.Menu.Open" }] }, proj);
  assert.equal(r.ok, true);
  const ini = fs.readFileSync(path.join(proj, "Config", "DefaultGameplayTags.ini"), "utf8");
  assert.match(ini, /\[SomeOtherSection\]/);
  assert.match(ini, /\+GameplayTagList=\(Tag="UI\.Menu\.Open"/);
});

test("addGameplayTags rejects empty and malformed tag names", async () => {
  const proj = makeTmpDir("ngg-gd-proj-");
  let r = await gamedev.addGameplayTags({ tags: [] }, proj);
  assert.equal(r.ok, false);

  r = await gamedev.addGameplayTags({ tags: [{ tag: "Bad Tag!" }] }, proj);
  assert.equal(r.ok, false);
  assert.match(r.error, /invalid tag name/);

  r = await gamedev.addGameplayTags({ tags: [{ tag: 'Evil",Injected=(' }] }, proj);
  assert.equal(r.ok, false);
});

test("addGameplayTags escapes double quotes in comments", async () => {
  const proj = makeTmpDir("ngg-gd-proj-");
  fs.mkdirSync(path.join(proj, "Config"), { recursive: true });
  const r = await gamedev.addGameplayTags(
    { tags: [{ tag: "State.Stunned", comment: 'the "hard" CC' }] },
    proj
  );
  assert.equal(r.ok, true);
  const ini = fs.readFileSync(path.join(proj, "Config", "DefaultGameplayTags.ini"), "utf8");
  assert.match(ini, /DevComment="the 'hard' CC"/);
});

// ---------------------------------------------------------------------------
// mcpResponse envelope
// ---------------------------------------------------------------------------

test("mcpResponse wraps ok results as JSON text and errors as isError", () => {
  const ok = gamedev.mcpResponse({ ok: true, data: { a: 1 } });
  assert.equal(ok.isError, undefined);
  assert.match(ok.content[0].text, /"a": 1/);

  const bad = gamedev.mcpResponse({ ok: false, error: "boom" });
  assert.equal(bad.isError, true);
  assert.equal(bad.content[0].text, "boom");
});
