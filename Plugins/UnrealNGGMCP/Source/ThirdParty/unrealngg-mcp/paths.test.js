// Copyright 2025-2026 NGG. All Rights Reserved.
// paths.test.js — unit tests for the project + engine path discovery helpers.
// Run: node --test paths.test.js

import { test, before, after } from "node:test";
import assert from "node:assert/strict";
import fs from "fs";
import os from "os";
import path from "path";

import {
  findUProject,
  readEngineAssociation,
  discoverEngineDir,
  discoverBuildTargets,
  projectNameFrom,
} from "./paths.js";

// ---------------------------------------------------------------------------
// Shared tmp fixture — a fake .uproject layout on disk.
// ---------------------------------------------------------------------------

let tmpRoot;
let projectDir;
let uprojectPath;

before(() => {
  tmpRoot = fs.mkdtempSync(path.join(os.tmpdir(), "ngg-paths-test-"));

  // <tmp>/parent/MyProj/
  projectDir = path.join(tmpRoot, "parent", "MyProj");
  fs.mkdirSync(projectDir, { recursive: true });

  // Fake .uproject with an EngineAssociation field
  uprojectPath = path.join(projectDir, "MyProj.uproject");
  fs.writeFileSync(
    uprojectPath,
    JSON.stringify({
      FileVersion: 3,
      EngineAssociation: "5.7",
      Category: "",
      Description: "",
    }, null, 2),
    "utf8"
  );

  // Source folder with three Target.cs files (editor / game / server)
  const sourceDir = path.join(projectDir, "Source");
  fs.mkdirSync(sourceDir, { recursive: true });
  fs.writeFileSync(path.join(sourceDir, "MyProj.Target.cs"),        "// stub");
  fs.writeFileSync(path.join(sourceDir, "MyProjEditor.Target.cs"),  "// stub");
  fs.writeFileSync(path.join(sourceDir, "MyProjServer.Target.cs"),  "// stub");

  // A deep subdirectory so findUProject() has something to walk up from.
  fs.mkdirSync(path.join(projectDir, "Content", "Game", "UI"), { recursive: true });
});

after(() => {
  fs.rmSync(tmpRoot, { recursive: true, force: true });
});

// ---------------------------------------------------------------------------
// findUProject
// ---------------------------------------------------------------------------

test("findUProject locates .uproject when startDir IS the project root", () => {
  const found = findUProject(projectDir);
  assert.equal(found, uprojectPath);
});

test("findUProject walks up from a nested subdirectory", () => {
  const deep = path.join(projectDir, "Content", "Game", "UI");
  const found = findUProject(deep);
  assert.equal(found, uprojectPath);
});

test("findUProject returns null when no .uproject exists anywhere above", () => {
  const isolated = fs.mkdtempSync(path.join(os.tmpdir(), "ngg-no-uproject-"));
  try {
    const found = findUProject(isolated);
    assert.equal(found, null);
  } finally {
    fs.rmSync(isolated, { recursive: true, force: true });
  }
});

test("findUProject stops after walking past the filesystem root", () => {
  // Running with a non-existent path shouldn't hang or crash.
  const ghost = path.join(tmpRoot, "does", "not", "exist", "deep", "path");
  const found = findUProject(ghost);
  assert.equal(found, null);
});

// ---------------------------------------------------------------------------
// readEngineAssociation
// ---------------------------------------------------------------------------

test("readEngineAssociation returns the EngineAssociation string", () => {
  assert.equal(readEngineAssociation(uprojectPath), "5.7");
});

test("readEngineAssociation returns null for missing file", () => {
  const missing = path.join(tmpRoot, "nope.uproject");
  assert.equal(readEngineAssociation(missing), null);
});

test("readEngineAssociation returns null for malformed JSON", () => {
  const bad = path.join(tmpRoot, "bad.uproject");
  fs.writeFileSync(bad, "not json at all", "utf8");
  assert.equal(readEngineAssociation(bad), null);
});

test("readEngineAssociation returns null when field is absent", () => {
  const noAssoc = path.join(tmpRoot, "noAssoc.uproject");
  fs.writeFileSync(noAssoc, JSON.stringify({ FileVersion: 3 }), "utf8");
  assert.equal(readEngineAssociation(noAssoc), null);
});

// ---------------------------------------------------------------------------
// discoverBuildTargets
// ---------------------------------------------------------------------------

test("discoverBuildTargets picks up all .Target.cs variants", () => {
  const t = discoverBuildTargets(uprojectPath);
  assert.equal(t.editor, "MyProjEditor");
  assert.equal(t.game,   "MyProj");
  assert.equal(t.server, "MyProjServer");
  assert.equal(t.client, undefined);
});

test("discoverBuildTargets falls back to defaults when Source is missing", () => {
  const orphan = path.join(tmpRoot, "orphan", "Orphan.uproject");
  fs.mkdirSync(path.dirname(orphan), { recursive: true });
  fs.writeFileSync(orphan, "{}", "utf8");
  const t = discoverBuildTargets(orphan);
  assert.equal(t.editor, "OrphanEditor");
  assert.equal(t.game,   "Orphan");
});

test("discoverBuildTargets accepts an injected readdirSync for pure-unit testing", () => {
  const fake = () => ["Foo.Target.cs", "FooEditor.Target.cs", "FooClient.Target.cs"];
  const t = discoverBuildTargets("/whatever/Foo.uproject", { readdirSync: fake });
  assert.equal(t.game,   "Foo");
  assert.equal(t.editor, "FooEditor");
  assert.equal(t.client, "FooClient");
});

// ---------------------------------------------------------------------------
// projectNameFrom
// ---------------------------------------------------------------------------

test("projectNameFrom strips directory and .uproject extension", () => {
  assert.equal(projectNameFrom(uprojectPath), "MyProj");
  assert.equal(projectNameFrom("C:/Some/Path/Thing.uproject"), "Thing");
  assert.equal(projectNameFrom("Thing.uproject"), "Thing");
});

// ---------------------------------------------------------------------------
// discoverEngineDir — env var branch (deterministic, no filesystem scan)
// ---------------------------------------------------------------------------

test("discoverEngineDir returns NGG_UE_INSTALL_DIR when it points at a real engine root", () => {
  const env = { NGG_UE_INSTALL_DIR: "Z:\\MyEngine" };
  // The env var is only honoured when Build.bat actually exists under it, so the
  // server can't hand back a bogus path that would fail at build time.
  const buildBat = path.join("Z:\\MyEngine", "Engine", "Build", "BatchFiles", "Build.bat");
  const result = discoverEngineDir("5.7", {
    env,
    existsSync: (p) => p === buildBat,
    execSync:   () => "",
  });
  assert.equal(result, "Z:\\MyEngine");
});

test("discoverEngineDir ignores NGG_UE_INSTALL_DIR with no Build.bat and falls through", () => {
  // Env var set but the path has no engine — must not be returned blindly.
  const env = { NGG_UE_INSTALL_DIR: "Z:\\NotAnEngine" };
  assert.throws(() => discoverEngineDir("5.7", {
    env,
    existsSync: () => false,
    execSync:   () => { throw new Error("not registered"); },
  }), /NGG_UE_INSTALL_DIR/);
});

test("discoverEngineDir honours registry-reported path when Build.bat exists there", () => {
  const regPath = "D:\\CustomEngine\\UE";
  const existsSync = (p) => p === path.join(regPath, "Engine", "Build", "BatchFiles", "Build.bat");
  const execSync   = () => `    5.7    REG_SZ    ${regPath}\r\n`;
  const result = discoverEngineDir("5.7", { env: {}, existsSync, execSync });
  assert.equal(result, regPath);
});

test("discoverEngineDir throws with a helpful message when nothing is found", () => {
  const result = () => discoverEngineDir("5.7", {
    env: {},
    existsSync: () => false,
    execSync:   () => { throw new Error("not registered"); },
  });
  assert.throws(result, /NGG_UE_INSTALL_DIR/);
});

test("discoverEngineDir env var wins over registry", () => {
  const execSync = () => "    5.7    REG_SZ    D:\\FromRegistry\r\n";
  const existsSync = () => true;
  const result = discoverEngineDir("5.7", {
    env: { NGG_UE_INSTALL_DIR: "E:\\FromEnv" },
    existsSync,
    execSync,
  });
  assert.equal(result, "E:\\FromEnv");
});
