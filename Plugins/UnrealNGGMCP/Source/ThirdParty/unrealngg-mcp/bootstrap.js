#!/usr/bin/env node
// Copyright 2025-2026 NGG. All Rights Reserved.
// bootstrap.js — dependency self-install shim for the unrealngg-mcp server.
//
// Claude Code launches THIS file (see .mcp.json) instead of index.js. On a
// fresh clone/install node_modules does not exist yet, and index.js's static
// imports (@modelcontextprotocol/sdk, zod) would crash before any code runs.
// This shim runs `npm install` when dependencies are missing or stale, then
// dynamically imports index.js — so the very first `claude` run in a terminal
// works without the user ever typing `npm install`.
//
// Freshness uses the same node_modules/.ngg-deps-stamp scheme as the editor
// plugin's FNodeDepsInstaller (MD5 hex of package-lock.json), so the editor
// bootstrap and this shim never re-install behind each other's back.
//
// Constraints:
//  - MUST import only node: builtins — it runs before npm install.
//  - MUST NOT write to stdout — that is the MCP stdio channel. All logging
//    goes to stderr (Claude Code shows it in the MCP server logs).

import { existsSync, readFileSync, writeFileSync } from "node:fs";
import { createHash } from "node:crypto";
import { spawnSync } from "node:child_process";
import { dirname, join, delimiter } from "node:path";
import { fileURLToPath } from "node:url";
import process from "node:process";

const PKG_DIR = dirname(fileURLToPath(import.meta.url));
const STAMP_PATH = join(PKG_DIR, "node_modules", ".ngg-deps-stamp");

const log = (msg) => process.stderr.write(`[unrealngg-mcp bootstrap] ${msg}\n`);

function lockHash() {
  const lock = join(PKG_DIR, "package-lock.json");
  const src = existsSync(lock) ? lock : join(PKG_DIR, "package.json");
  return createHash("md5").update(readFileSync(src)).digest("hex");
}

function needsInstall() {
  if (!existsSync(join(PKG_DIR, "node_modules"))) return true;
  try {
    return readFileSync(STAMP_PATH, "utf8").trim() !== lockHash();
  } catch {
    return true; // no stamp (manual install) — one npm no-op run writes it
  }
}

/** [command, args, useShell] for `npm install`, most robust option first. */
function npmInvocation() {
  const installArgs = ["install", "--no-audit", "--no-fund", "--loglevel=error"];

  // Drive npm-cli.js with the exact node binary running us. Immune to two
  // real failure modes: PATH lacking npm (GUI-spawned processes), and the
  // npm.cmd %~dp0 quirk where a bare-name invocation with our cwd makes the
  // wrapper look for npm-cli.js inside THIS package and die MODULE_NOT_FOUND.
  const cli = join(dirname(process.execPath), "node_modules", "npm", "bin", "npm-cli.js");
  if (existsSync(cli)) return [process.execPath, [cli, ...installArgs], false];

  // Fallback: absolute npm path from a PATH scan (bare .cmd also needs
  // shell:true on current Node, which is why the path gets quoted).
  const npmName = process.platform === "win32" ? "npm.cmd" : "npm";
  for (const dir of (process.env.PATH || "").split(delimiter)) {
    if (!dir) continue;
    const candidate = join(dir, npmName);
    if (existsSync(candidate)) {
      return process.platform === "win32"
        ? [`"${candidate}"`, installArgs, true]
        : [candidate, installArgs, false];
    }
  }
  return [npmName, installArgs, process.platform === "win32"];
}

if (needsInstall()) {
  log("First run: installing npm dependencies (this can take a minute)...");
  const [cmd, args, shell] = npmInvocation();
  // npm's stdout is routed to OUR stderr (fd 2): stdout must stay clean for MCP.
  const result = spawnSync(cmd, args, { cwd: PKG_DIR, stdio: ["ignore", 2, 2], shell });

  if (result.error || result.status !== 0) {
    log(`npm install failed (${result.error ? result.error.message : `exit code ${result.status}`}).`);
    log("Install Node.js 18+ from https://nodejs.org, or run manually:");
    log(`  cd "${PKG_DIR}" && npm install`);
    process.exit(1);
  }
  if (!existsSync(join(PKG_DIR, "node_modules"))) {
    log("npm install reported success but node_modules was not created — aborting.");
    process.exit(1);
  }
  writeFileSync(STAMP_PATH, lockHash());
  log("Dependencies installed — starting MCP server.");
}

await import("./index.js");
