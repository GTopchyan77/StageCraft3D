// Copyright 2025-2026 NGG. All Rights Reserved.
// setup-assets.js — first-run installer for the ue5-ngg MCP server's bundled
// Claude Code skills and agents.
//
// On startup the server copies the files bundled under ./assets (skills/,
// agents/) into the project's .claude/ directory, so any project that uses this
// plugin automatically gets the NGG authoring skills + the unreal-author
// subagent — the same "ships with the plugin, applies on any PC" model the
// UE5_NGG_RULES.md instructions already use. Skills/agents are loaded by Claude
// Code at session start, so freshly installed files are picked up on the next
// Claude Code restart.
//
// Update policy: version-tracked, never clobber user edits. A manifest at
// .claude/.ngg-assets.json records the hash of every file we wrote. On a later
// run we (a) install files that are missing, (b) refresh files the user hasn't
// touched when the bundled version changed, and (c) leave any user-modified
// file alone.

import fs from "fs";
import path from "path";
import crypto from "crypto";

const MANIFEST_NAME = ".ngg-assets.json";

function sha256(buf) {
  return crypto.createHash("sha256").update(buf).digest("hex");
}

// Recursively collect file paths under `dir`, returned relative to `base`.
function walkFiles(dir, base = dir, out = []) {
  let entries;
  try {
    entries = fs.readdirSync(dir, { withFileTypes: true });
  } catch {
    return out;
  }
  for (const e of entries) {
    const full = path.join(dir, e.name);
    // Skip symlinks entirely — a symlinked directory can point back up the tree
    // and send this recursion into an infinite loop / stack overflow.
    if (e.isSymbolicLink()) continue;
    if (e.isDirectory()) walkFiles(full, base, out);
    else if (e.isFile()) out.push(path.relative(base, full));
  }
  return out;
}

function readManifest(claudeDir) {
  try {
    const raw = fs.readFileSync(path.join(claudeDir, MANIFEST_NAME), "utf8");
    const m = JSON.parse(raw);
    if (m && typeof m === "object" && m.files && typeof m.files === "object") return m;
  } catch {
    /* missing or malformed — treat as a fresh install */
  }
  return { version: null, files: {} };
}

/**
 * Install the bundled skills/agents under `sourceDir` into
 * `<projectDir>/.claude`.
 *
 * @param {object}   opts
 * @param {string}   opts.sourceDir   Directory holding manifest.json + skills/ + agents/.
 * @param {string}   opts.projectDir  UE project root; assets land in its .claude/.
 * @param {Function} [opts.log]       Optional logger for per-file notes.
 * @returns {{ran:boolean, version:string|null, installed:number, updated:number,
 *            skipped:number, unchanged:number, reason?:string}}
 */
export function installClaudeAssets({ sourceDir, projectDir, log = () => {} } = {}) {
  const summary = { ran: false, version: null, installed: 0, updated: 0, skipped: 0, unchanged: 0 };

  if (!sourceDir || !fs.existsSync(sourceDir)) {
    summary.reason = `bundled assets dir not found: ${sourceDir}`;
    return summary;
  }
  if (!projectDir) {
    summary.reason = "no project directory resolved — skipping asset install";
    return summary;
  }

  // Bundled content version (independent of the npm package version).
  let version = "0.0.0";
  try {
    const mf = JSON.parse(fs.readFileSync(path.join(sourceDir, "manifest.json"), "utf8"));
    if (mf && typeof mf.version === "string") version = mf.version;
  } catch {
    /* no manifest version — fall back to 0.0.0 */
  }
  summary.version = version;

  const claudeDir = path.join(projectDir, ".claude");
  const manifest = readManifest(claudeDir);
  const nextFiles = { ...manifest.files };
  let dirty = false;

  // Every file under assets/ except repo-housekeeping files: the top-level
  // manifest.json / README.md (which document the folder, not skill/agent
  // content) and any .gitkeep placeholder used to retain empty dirs in git.
  const TOP_LEVEL_SKIP = new Set(["manifest.json", "README.md"]);
  const relFiles = walkFiles(sourceDir).filter((r) => {
    const posix = r.split(path.sep).join("/");
    if (!posix.includes("/") && TOP_LEVEL_SKIP.has(posix)) return false;
    if (path.basename(posix) === ".gitkeep") return false;
    return true;
  });

  for (const rel of relFiles) {
    const relKey = rel.split(path.sep).join("/"); // stable POSIX key for the manifest
    const src = fs.readFileSync(path.join(sourceDir, rel));
    const srcHash = sha256(src);
    const dest = path.join(claudeDir, rel);

    if (!fs.existsSync(dest)) {
      fs.mkdirSync(path.dirname(dest), { recursive: true });
      fs.writeFileSync(dest, src);
      nextFiles[relKey] = srcHash;
      summary.installed++;
      dirty = true;
      continue;
    }

    const curHash = sha256(fs.readFileSync(dest));
    const prev = manifest.files[relKey];

    if (curHash === srcHash) {
      // Already matches the bundled content.
      nextFiles[relKey] = srcHash;
      summary.unchanged++;
    } else if (prev && curHash === prev) {
      // We wrote `prev`; the user hasn't modified it; bundled content changed → refresh.
      fs.writeFileSync(dest, src);
      nextFiles[relKey] = srcHash;
      summary.updated++;
      dirty = true;
    } else {
      // User-modified (or a pre-existing unmanaged file) — never clobber.
      summary.skipped++;
      log(`[ue5-ngg] preserving user-modified ${relKey} (not overwriting with bundled version)`);
    }
  }

  summary.ran = true;

  if (dirty || manifest.version !== version) {
    try {
      fs.mkdirSync(claudeDir, { recursive: true });
      fs.writeFileSync(
        path.join(claudeDir, MANIFEST_NAME),
        JSON.stringify({ version, installedAt: new Date().toISOString(), files: nextFiles }, null, 2) + "\n"
      );
    } catch (err) {
      log(`[ue5-ngg] failed to write asset manifest: ${err.message}`);
    }
  }

  return summary;
}
