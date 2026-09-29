#!/usr/bin/env node
// Copyright 2025-2026 NGG. All Rights Reserved.
// set-token.js — unrealngg-mcp
//
// Generates a bridge auth token and writes it to <Project>/Config/UserEngine.ini
// under [UnrealNGGMCP] AuthToken=…
//
// UserEngine.ini is GConfig's `GameDirUser` layer — the last entry in
// GConfigLayers (Engine/Source/Runtime/Core/Public/Misc/ConfigHierarchy.h), so
// it overrides Config/DefaultEngine.ini and the editor never rewrites it. It is
// gitignored, which is why the secret goes here and not into the tracked
// DefaultEngine.ini.
//
// Usage:
//   npm run set-token            # generate a fresh 64-hex token
//   npm run set-token -- <token> # install a specific token
//   npm run set-token -- --print # show the current token, change nothing
//
// The editor reads the token at startup, so restart it after changing this.

import fs from "fs";
import path from "path";
import crypto from "crypto";
import { fileURLToPath } from "url";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
// <Project>/Plugins/UnrealNGGMCP/Source/ThirdParty/unrealngg-mcp -> <Project>
const PROJECT_ROOT = path.resolve(__dirname, "..", "..", "..", "..", "..");
const USER_INI     = path.join(PROJECT_ROOT, "Config", "UserEngine.ini");

const SECTION = "[UnrealNGGMCP]";
const HEADER = `; Local, machine-specific engine config — NOT tracked in git (see .gitignore).
;
; This is the \`GameDirUser\` layer, the last entry in GConfigLayers
; (Engine/Source/Runtime/Core/Public/Misc/ConfigHierarchy.h), so it overrides
; Config/DefaultEngine.ini and the editor never rewrites it. That makes it the
; right home for the bridge secret: DefaultEngine.ini is tracked in git and
; would publish the token into the repository history.
;
; Written by \`npm run set-token\` — safe to edit by hand.
`;

// Pull the current token out of the ini, or "" if there is none.
function readCurrentToken(text) {
  let inSection = false;
  for (const rawLine of text.split(/\r?\n/)) {
    const line = rawLine.trim();
    if (line.startsWith("[")) {
      inSection = line === SECTION;
      continue;
    }
    if (!inSection) continue;
    const m = line.match(/^AuthToken\s*=\s*(\S+)/);
    if (m) return m[1];
  }
  return "";
}

// Replace AuthToken inside [UnrealNGGMCP], preserving every other section and
// any comments the user added. Appends the section when it isn't there yet.
function withToken(text, token) {
  const lines = text.split(/\r?\n/);
  const out = [];
  let inSection = false;
  let wrote = false;

  for (const rawLine of lines) {
    const line = rawLine.trim();
    if (line.startsWith("[")) {
      // Leaving our section without having written the key — add it now, so a
      // section that exists but has no AuthToken still gets one.
      if (inSection && !wrote) {
        out.push(`AuthToken=${token}`);
        wrote = true;
      }
      inSection = line === SECTION;
      out.push(rawLine);
      continue;
    }
    if (inSection && /^AuthToken\s*=/.test(line)) {
      if (!wrote) {
        out.push(`AuthToken=${token}`);
        wrote = true;
      }
      continue;   // drop duplicate AuthToken lines
    }
    out.push(rawLine);
  }

  if (inSection && !wrote) {
    out.push(`AuthToken=${token}`);
    wrote = true;
  }
  if (!wrote) {
    if (out.length && out[out.length - 1].trim() !== "") out.push("");
    out.push(SECTION, `AuthToken=${token}`);
  }

  let result = out.join("\n");
  if (!result.endsWith("\n")) result += "\n";
  return result;
}

function main(argv) {
  const args = argv.slice(2);
  const existing = fs.existsSync(USER_INI) ? fs.readFileSync(USER_INI, "utf8") : "";

  if (args[0] === "--print") {
    const token = readCurrentToken(existing);
    if (token) {
      console.log(token);
      return 0;
    }
    console.error(`No AuthToken in ${USER_INI}. Run: npm run set-token`);
    return 1;
  }

  let token = args[0];
  if (token && !/^[A-Za-z0-9_-]{16,128}$/.test(token)) {
    console.error("Token must be 16-128 chars of [A-Za-z0-9_-].");
    return 1;
  }
  if (!token) token = crypto.randomBytes(32).toString("hex");

  const next = withToken(existing || HEADER, token);
  fs.mkdirSync(path.dirname(USER_INI), { recursive: true });
  fs.writeFileSync(USER_INI, next, "utf8");

  console.log(`Wrote AuthToken to ${USER_INI}`);
  console.log(`  ${token.slice(0, 8)}…${token.slice(-4)} (${token.length} chars)`);
  console.log("Restart the editor for the bridge to pick it up.");
  return 0;
}

// Only act when run as a script — the unit tests import the helpers above.
if (process.argv[1] && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url))) {
  process.exit(main(process.argv));
}

export { readCurrentToken, withToken, USER_INI };
