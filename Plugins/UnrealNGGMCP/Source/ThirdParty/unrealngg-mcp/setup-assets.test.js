// Copyright 2025-2026 NGG. All Rights Reserved.
// setup-assets.test.js — unit tests for the bundled skills/agents installer.
// Uses temp source + project dirs so nothing touches the real .claude/.

import { test, after } from "node:test";
import assert from "node:assert/strict";
import fs from "fs";
import os from "os";
import path from "path";
import { installClaudeAssets } from "./setup-assets.js";

// Track every temp dir created so we can remove them when the suite finishes,
// instead of littering the OS temp directory on every run.
const tmpDirs = [];
function tmp() {
  const d = fs.mkdtempSync(path.join(os.tmpdir(), "ngg-assets-"));
  tmpDirs.push(d);
  return d;
}

after(() => {
  for (const d of tmpDirs) {
    try { fs.rmSync(d, { recursive: true, force: true }); } catch { /* best effort */ }
  }
});

// (Re)write a bundled-source tree: manifest.json + the given { relPath: content }.
function writeSrc(dir, version, files) {
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(path.join(dir, "manifest.json"), JSON.stringify({ version }));
  for (const [rel, content] of Object.entries(files)) {
    const p = path.join(dir, rel);
    fs.mkdirSync(path.dirname(p), { recursive: true });
    fs.writeFileSync(p, content);
  }
}

const dest = (proj, rel) => path.join(proj, ".claude", rel);

test("fresh install writes files and a manifest", () => {
  const src = tmp(), proj = tmp();
  writeSrc(src, "1.0.0", { "skills/a/SKILL.md": "A", "agents/x.md": "X" });

  const s = installClaudeAssets({ sourceDir: src, projectDir: proj });

  assert.equal(s.ran, true);
  assert.equal(s.installed, 2);
  assert.equal(s.version, "1.0.0");
  assert.equal(fs.readFileSync(dest(proj, "skills/a/SKILL.md"), "utf8"), "A");
  assert.equal(fs.readFileSync(dest(proj, "agents/x.md"), "utf8"), "X");
  assert.ok(fs.existsSync(dest(proj, ".ngg-assets.json")));
});

test("manifest.json itself is not copied into .claude", () => {
  const src = tmp(), proj = tmp();
  writeSrc(src, "1.0.0", { "skills/a/SKILL.md": "A" });
  installClaudeAssets({ sourceDir: src, projectDir: proj });
  assert.ok(!fs.existsSync(dest(proj, "manifest.json")));
});

test("re-run is idempotent (everything unchanged)", () => {
  const src = tmp(), proj = tmp();
  writeSrc(src, "1.0.0", { "skills/a/SKILL.md": "A" });
  installClaudeAssets({ sourceDir: src, projectDir: proj });

  const s = installClaudeAssets({ sourceDir: src, projectDir: proj });
  assert.equal(s.installed, 0);
  assert.equal(s.updated, 0);
  assert.equal(s.unchanged, 1);
});

test("user edits are preserved, never clobbered, even on a version bump", () => {
  const src = tmp(), proj = tmp();
  writeSrc(src, "1.0.0", { "skills/a/SKILL.md": "A" });
  installClaudeAssets({ sourceDir: src, projectDir: proj });

  fs.writeFileSync(dest(proj, "skills/a/SKILL.md"), "USER EDIT");
  writeSrc(src, "2.0.0", { "skills/a/SKILL.md": "A v2" }); // bundled content also changed

  const s = installClaudeAssets({ sourceDir: src, projectDir: proj });
  assert.equal(s.skipped, 1);
  assert.equal(s.updated, 0);
  assert.equal(fs.readFileSync(dest(proj, "skills/a/SKILL.md"), "utf8"), "USER EDIT");
});

test("an unmodified file is refreshed when bundled content changes", () => {
  const src = tmp(), proj = tmp();
  writeSrc(src, "1.0.0", { "skills/a/SKILL.md": "A" });
  installClaudeAssets({ sourceDir: src, projectDir: proj });

  writeSrc(src, "2.0.0", { "skills/a/SKILL.md": "A v2" });

  const s = installClaudeAssets({ sourceDir: src, projectDir: proj });
  assert.equal(s.updated, 1);
  assert.equal(fs.readFileSync(dest(proj, "skills/a/SKILL.md"), "utf8"), "A v2");
});

test("a newly bundled file installs on a later run", () => {
  const src = tmp(), proj = tmp();
  writeSrc(src, "1.0.0", { "skills/a/SKILL.md": "A" });
  installClaudeAssets({ sourceDir: src, projectDir: proj });

  writeSrc(src, "1.1.0", { "skills/a/SKILL.md": "A", "agents/x.md": "X" });

  const s = installClaudeAssets({ sourceDir: src, projectDir: proj });
  assert.equal(s.installed, 1);
  assert.equal(fs.readFileSync(dest(proj, "agents/x.md"), "utf8"), "X");
});

test("top-level README.md and .gitkeep placeholders are not installed", () => {
  const src = tmp(), proj = tmp();
  writeSrc(src, "1.0.0", {
    "README.md": "folder docs",
    "skills/.gitkeep": "",
    "agents/.gitkeep": "",
    "skills/a/SKILL.md": "A",
  });

  const s = installClaudeAssets({ sourceDir: src, projectDir: proj });
  assert.equal(s.installed, 1); // only SKILL.md
  assert.ok(fs.existsSync(dest(proj, "skills/a/SKILL.md")));
  assert.ok(!fs.existsSync(dest(proj, "README.md")));
  assert.ok(!fs.existsSync(dest(proj, "skills/.gitkeep")));
  assert.ok(!fs.existsSync(dest(proj, "agents/.gitkeep")));
});

test("an empty content tree installs nothing but still runs", () => {
  const src = tmp(), proj = tmp();
  writeSrc(src, "1.0.0", { "README.md": "docs", "skills/.gitkeep": "", "agents/.gitkeep": "" });
  const s = installClaudeAssets({ sourceDir: src, projectDir: proj });
  assert.equal(s.ran, true);
  assert.equal(s.installed, 0);
});

test("missing sourceDir is reported, not thrown", () => {
  const proj = tmp();
  const s = installClaudeAssets({ sourceDir: path.join(proj, "nope"), projectDir: proj });
  assert.equal(s.ran, false);
  assert.match(s.reason, /not found/);
});

test("missing projectDir is reported, not thrown", () => {
  const src = tmp();
  writeSrc(src, "1.0.0", { "skills/a/SKILL.md": "A" });
  const s = installClaudeAssets({ sourceDir: src, projectDir: null });
  assert.equal(s.ran, false);
  assert.match(s.reason, /project directory/);
});
