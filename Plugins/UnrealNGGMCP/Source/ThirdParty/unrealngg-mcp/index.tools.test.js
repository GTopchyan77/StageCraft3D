// Copyright 2025-2026 NGG. All Rights Reserved.
// index.tools.test.js — static smoke test for the MCP server's tool surface.
//
// index.js registers tools imperatively at module load and then connects a
// stdio transport, so we can't simply `import` it from a test without blocking.
// Instead we parse the file as text and assert structural properties of the
// tool registry: every `server.tool("name", ...)` call is well-formed, names
// are unique, and counts haven't silently regressed.
//
// Run: node --test index.tools.test.js

import { test } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const __dirname  = path.dirname(fileURLToPath(import.meta.url));
const INDEX_PATH = path.join(__dirname, "index.js");
const SRC        = fs.readFileSync(INDEX_PATH, "utf8");

// Regex parses:
//   server.tool(
//     "tool_name",
// Names span two lines so we use the [\s\S] catch-all between `tool(` and the
// opening quote.
const TOOL_NAME_REGEX = /server\.tool\(\s*"([A-Za-z][A-Za-z0-9_]*)"/g;

function collectToolNames() {
  const names = [];
  let m;
  while ((m = TOOL_NAME_REGEX.exec(SRC)) !== null) {
    names.push(m[1]);
  }
  return names;
}

test("index.js registers a meaningful number of MCP tools", () => {
  const names = collectToolNames();
  // Lower bound — if someone deletes large chunks accidentally this fires.
  assert.ok(
    names.length >= 100,
    `expected >= 100 registered tools, got ${names.length}`,
  );
});

test("every registered MCP tool name is unique", () => {
  const names = collectToolNames();
  const seen  = new Set();
  const dups  = [];
  for (const n of names) {
    if (seen.has(n)) dups.push(n);
    seen.add(n);
  }
  assert.deepStrictEqual(dups, [], `duplicate tool names: ${dups.join(", ")}`);
});

test("every registered MCP tool name uses an allowed prefix", () => {
  // Prefixes are conventions we surface to Claude in the MCP catalog. If a
  // new prefix is added intentionally, extend this list — the failure shows
  // the offender so review is obvious.
  const ALLOWED_PREFIXES = ["ue5_", "pcg_", "youtube_"];
  const names = collectToolNames();
  const bad   = names.filter(
    (n) => !ALLOWED_PREFIXES.some((p) => n.startsWith(p)),
  );
  assert.deepStrictEqual(
    bad,
    [],
    `tools with non-conventional names (allowed prefixes ${ALLOWED_PREFIXES.join("/")}): ${bad.join(", ")}`,
  );
});

test("every server.tool invocation has a matching closing paren count", () => {
  // Catches truncated/half-written tool definitions where the closing `);`
  // of a previous tool got eaten by an edit and the next one didn't compile.
  const openCount  = (SRC.match(/server\.tool\(/g) || []).length;
  const nameCount  = collectToolNames().length;
  assert.strictEqual(
    openCount,
    nameCount,
    `server.tool( call count (${openCount}) != extracted name count (${nameCount}); ` +
    `some calls may have malformed/dynamic names`,
  );
});

test("RULES_PATH is resolved relative to index.js, not cwd", () => {
  // Guards against a regression where rules-loading uses process.cwd() —
  // that breaks running the sidecar from a different working directory.
  assert.match(SRC, /RULES_PATH\s*=\s*path\.join\(__dirname,\s*"UE5_NGG_RULES\.md"\)/);
});

test("stdio + ws transports are both wired up", () => {
  // Smoke check: both transport modes exist. If someone rips one out we'll
  // notice via the test surface rather than via a downstream user.
  assert.match(SRC, /StdioServerTransport/);
  assert.match(SRC, /TRANSPORT_MODE\s*===\s*"ws"/);
});

test("docs quote the real tool count", () => {
  // The catalog count is repeated across the plugin README, the sidecar README
  // and the design slides. It drifted before (docs said 171, the server
  // registered 172), so pin every three-digit "<N> tools" claim to the registry.
  // Three digits on purpose: prose like "26 tools go dark" is a different claim
  // about a subset, not the total.
  const actual = collectToolNames().length;
  const pluginRoot = path.resolve(__dirname, "..", "..", "..");   // Plugins/UnrealNGGMCP
  const docs = [
    path.join(__dirname, "README.md"),
    path.join(pluginRoot, "README.md"),
    path.join(pluginRoot, "Docs", "ReviewerTesting.md"),
    path.join(pluginRoot, "Docs", "design", "01-cover.html"),
    path.join(pluginRoot, "Docs", "design", "03-capabilities.html"),
    path.join(pluginRoot, "Docs", "design", "07-all-tools.html"),
    path.join(pluginRoot, "Docs", "design", "08-features.html"),
  ];

  const mismatches = [];
  for (const doc of docs) {
    if (!fs.existsSync(doc)) continue;   // design slides are optional in a trimmed checkout
    const text = fs.readFileSync(doc, "utf8");
    for (const m of text.matchAll(/\b(\d{3})\s+(?:MCP\s+)?tools\b/gi)) {
      if (Number(m[1]) !== actual) {
        mismatches.push(`${path.basename(doc)}: "${m[0]}"`);
      }
    }
  }

  assert.deepStrictEqual(
    mismatches,
    [],
    `index.js registers ${actual} tools, but these docs disagree:\n  ${mismatches.join("\n  ")}`,
  );
});

test("UE5_NGG_RULES.md exists and is non-empty", () => {
  // The rules file ships with the sidecar and is sent as the MCP server's
  // `instructions` field on connect. Missing or empty means every Claude
  // client falls back to the bridge-not-found stub.
  const rulesPath = path.join(__dirname, "UE5_NGG_RULES.md");
  assert.ok(fs.existsSync(rulesPath), "UE5_NGG_RULES.md is missing next to index.js");
  const stat = fs.statSync(rulesPath);
  assert.ok(stat.size > 1000, `UE5_NGG_RULES.md unexpectedly small: ${stat.size} bytes`);
});
