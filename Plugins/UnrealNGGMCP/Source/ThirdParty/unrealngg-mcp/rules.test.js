// Copyright 2025-2026 NGG. All Rights Reserved.
// rules.test.js — unit tests for the UE5_NGG_RULES.md core/resource split.
//
// The split moves ~85% of the rules document out of the handshake. The thing
// that must never happen is a rule going missing: not in the core, not in any
// resource. Most of these tests are about that.

import { test } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { splitRules, RESOURCE_URI_PREFIX } from "./rules.js";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const RULES_RAW = fs.readFileSync(path.join(__dirname, "UE5_NGG_RULES.md"), "utf8");

test("the shipped rules document splits cleanly", () => {
  const r = splitRules(RULES_RAW);
  assert.equal(r.complete, true, `split fell back; missing anchors: ${r.missing.join("; ")}`);
  assert.ok(r.sections.length >= 5, `expected several sections, got ${r.sections.length}`);
});

test("no rule text is lost — every source line lands in the core or a section", () => {
  const r = splitRules(RULES_RAW);
  const sourceLines = RULES_RAW.split(/\r?\n/).map(l => l.trim()).filter(Boolean);

  // The core gets a generated index appended, so compare on content rather than
  // on length: build the set of lines the client can actually reach.
  const reachable = new Set();
  for (const line of r.core.split(/\r?\n/)) {
    const t = line.trim();
    if (t) reachable.add(t);
  }
  for (const s of r.sections) {
    for (const line of s.text.split(/\r?\n/)) {
      const t = line.trim();
      if (t) reachable.add(t);
    }
  }

  const lost = sourceLines.filter(l => !reachable.has(l));
  assert.deepStrictEqual(
    lost.slice(0, 10),
    [],
    `${lost.length} source line(s) are unreachable after the split, e.g.:\n  ${lost.slice(0, 10).join("\n  ")}`,
  );
});

test("sections do not overlap — each section starts at its own anchor", () => {
  const r = splitRules(RULES_RAW);
  for (const s of r.sections) {
    const firstLine = s.text.split(/\r?\n/)[0];
    assert.ok(
      firstLine.startsWith("#"),
      `section ${s.slug} starts mid-paragraph: "${firstLine.slice(0, 60)}"`,
    );
  }
  // A section's opening heading must not appear inside the previous one.
  for (let i = 1; i < r.sections.length; i++) {
    const heading = r.sections[i].text.split(/\r?\n/)[0];
    assert.ok(
      !r.sections[i - 1].text.includes(heading),
      `section ${r.sections[i - 1].slug} swallowed the heading of ${r.sections[i].slug}`,
    );
  }
});

test("the core keeps the always-on rules", () => {
  const r = splitRules(RULES_RAW);
  // These are the rules whose violation breaks the workflow regardless of which
  // subsystem is being worked on, so they must not be demoted to a resource.
  for (const needle of [
    "MCP-first for editor work",
    "Health-check first",
    "Save after mutating",
    "Close the asset before mutating",
  ]) {
    assert.ok(r.core.includes(needle), `core lost an always-on rule: ${needle}`);
  }
});

test("the core advertises every resource URI", () => {
  const r = splitRules(RULES_RAW);
  // Without the index the model has no way to learn the resources exist.
  for (const s of r.sections) {
    assert.ok(r.core.includes(s.uri), `core does not mention ${s.uri}`);
  }
  assert.match(r.core, /resources\/read/, "core does not say how to fetch a resource");
});

test("every resource URI is unique and namespaced", () => {
  const r = splitRules(RULES_RAW);
  const uris = r.sections.map(s => s.uri);
  assert.equal(new Set(uris).size, uris.length, `duplicate resource URIs: ${uris}`);
  for (const uri of uris) {
    assert.ok(uri.startsWith(RESOURCE_URI_PREFIX), `unexpected URI namespace: ${uri}`);
  }
});

test("every section carries a description that says when to read it", () => {
  const r = splitRules(RULES_RAW);
  for (const s of r.sections) {
    assert.ok(s.title && s.title.length > 3,  `section ${s.slug} has no title`);
    assert.ok(s.description.length > 40, `section ${s.slug} description is too thin to route on`);
  }
});

test("a missing anchor falls back to sending the whole document", () => {
  // Someone renames a heading: we must degrade to the old behaviour rather than
  // slicing at the wrong place and dropping rules.
  const mangled = RULES_RAW.replace("### 13. Gameplay Ability System", "### 13. Abilities");
  const r = splitRules(mangled);
  assert.equal(r.complete, false);
  assert.equal(r.sections.length, 0);
  assert.equal(r.core, mangled, "fallback must hand back the document verbatim");
  assert.ok(r.missing.length > 0, "fallback did not report which anchor moved");
});

test("out-of-order anchors also fall back", () => {
  // Reordering chapters would make the line-range slicing mix unrelated rules.
  const lines = RULES_RAW.split(/\r?\n/);
  const gasAt      = lines.findIndex(l => l.startsWith("### 13. Gameplay Ability System"));
  const profileAt  = lines.findIndex(l => l.startsWith("### 14. Performance profiling"));
  assert.ok(gasAt > 0 && profileAt > gasAt, "fixture assumption broken");

  // Move the profiling heading above the GAS one.
  const reordered = [
    ...lines.slice(0, gasAt),
    lines[profileAt],
    ...lines.slice(gasAt, profileAt),
    ...lines.slice(profileAt + 1),
  ].join("\n");

  const r = splitRules(reordered);
  assert.equal(r.complete, false, "reordered document was split anyway");
  assert.equal(r.sections.length, 0);
});

test("an empty or stub document does not throw", () => {
  for (const input of ["", "This MCP server bridges to an Unreal Engine 5 editor."]) {
    const r = splitRules(input);
    assert.equal(r.complete, false);
    assert.equal(r.core, input);
    assert.deepEqual(r.sections, []);
  }
});

test("the split actually shrinks the handshake", () => {
  // The whole point. If a future edit moves everything back into the core this
  // fails rather than silently costing tokens again.
  const r = splitRules(RULES_RAW);
  assert.ok(
    r.core.length < RULES_RAW.length * 0.3,
    `core is ${r.core.length} of ${RULES_RAW.length} bytes — the split is not buying anything`,
  );
});
