// Copyright 2025-2026 NGG. All Rights Reserved.
// set-token.test.js — unit tests for the UserEngine.ini token writer.

import test from "node:test";
import assert from "node:assert/strict";
import { readCurrentToken, withToken } from "./set-token.js";

test("withToken appends the section to an empty file", () => {
  const out = withToken("", "abc123");
  assert.match(out, /\[UnrealNGGMCP\]\nAuthToken=abc123\n$/);
  assert.equal(readCurrentToken(out), "abc123");
});

test("withToken replaces an existing token in place", () => {
  const before = "[UnrealNGGMCP]\nAuthToken=old-token\n";
  const out = withToken(before, "new-token");
  assert.equal(readCurrentToken(out), "new-token");
  assert.ok(!out.includes("old-token"), `stale token survived:\n${out}`);
});

test("withToken preserves other sections and comments", () => {
  const before =
    "; a leading comment\n" +
    "[/Script/Engine.Engine]\n" +
    "bSomething=True\n" +
    "\n" +
    "[UnrealNGGMCP]\n" +
    "AuthToken=old\n" +
    "\n" +
    "[AnotherSection]\n" +
    "Foo=Bar\n";

  const out = withToken(before, "fresh");
  assert.equal(readCurrentToken(out), "fresh");
  assert.ok(out.includes("; a leading comment"), "comment was dropped");
  assert.ok(out.includes("bSomething=True"),     "earlier section was dropped");
  assert.ok(out.includes("[AnotherSection]"),    "later section was dropped");
  assert.ok(out.includes("Foo=Bar"),             "later section body was dropped");
});

test("withToken fills in a section that exists but has no AuthToken", () => {
  const before = "[UnrealNGGMCP]\n; token goes here\n\n[Other]\nX=1\n";
  const out = withToken(before, "filled");
  assert.equal(readCurrentToken(out), "filled");
  assert.ok(out.includes("[Other]"), "trailing section was dropped");
  assert.ok(out.includes("; token goes here"), "in-section comment was dropped");
});

test("withToken collapses duplicate AuthToken lines to one", () => {
  const before = "[UnrealNGGMCP]\nAuthToken=first\nAuthToken=second\n";
  const out = withToken(before, "only");
  assert.equal(readCurrentToken(out), "only");
  assert.equal((out.match(/^AuthToken=/gm) || []).length, 1, `not collapsed:\n${out}`);
});

test("readCurrentToken ignores AuthToken outside our section", () => {
  const text = "[SomeoneElse]\nAuthToken=not-ours\n\n[UnrealNGGMCP]\nAuthToken=ours\n";
  assert.equal(readCurrentToken(text), "ours");
});

test("readCurrentToken returns empty when the section has no token", () => {
  assert.equal(readCurrentToken("[UnrealNGGMCP]\n; nothing here\n"), "");
  assert.equal(readCurrentToken(""), "");
});
