// Copyright 2025-2026 NGG. All Rights Reserved.
// helpers.test.js — unit tests for jsonPreprocess + env().
// Run: node --test helpers.test.js

import { test } from "node:test";
import assert from "node:assert/strict";

import { jsonPreprocess, env } from "./helpers.js";

// ---------------------------------------------------------------------------
// jsonPreprocess
// ---------------------------------------------------------------------------

test("jsonPreprocess passes through non-strings unchanged", () => {
  assert.deepEqual(jsonPreprocess({ x: 1 }), { x: 1 });
  assert.deepEqual(jsonPreprocess([1, 2, 3]), [1, 2, 3]);
  assert.equal(jsonPreprocess(42), 42);
  assert.equal(jsonPreprocess(true), true);
  assert.equal(jsonPreprocess(null), null);
  assert.equal(jsonPreprocess(undefined), undefined);
});

test("jsonPreprocess parses JSON object strings", () => {
  assert.deepEqual(jsonPreprocess('{"x":1,"y":"two"}'), { x: 1, y: "two" });
});

test("jsonPreprocess parses JSON array strings", () => {
  assert.deepEqual(jsonPreprocess("[1,2,3]"), [1, 2, 3]);
});

test("jsonPreprocess returns string unchanged when not valid JSON", () => {
  assert.equal(jsonPreprocess("not json"), "not json");
  assert.equal(jsonPreprocess("abc {"), "abc {");
  assert.equal(jsonPreprocess(""), "");
});

test("jsonPreprocess parses JSON scalar strings", () => {
  // Valid JSON scalars — these SHOULD parse, matching JSON.parse behaviour.
  assert.equal(jsonPreprocess("42"), 42);
  assert.equal(jsonPreprocess("true"), true);
  assert.equal(jsonPreprocess("null"), null);
  assert.equal(jsonPreprocess('"hi"'), "hi");
});

test("jsonPreprocess handles nested objects", () => {
  const nested = '{"a":{"b":[1,2,{"c":true}]}}';
  assert.deepEqual(jsonPreprocess(nested), { a: { b: [1, 2, { c: true }] } });
});

// ---------------------------------------------------------------------------
// env()
// ---------------------------------------------------------------------------

test("env returns fallback when var is unset", () => {
  delete process.env.__NGG_TEST_X;
  assert.equal(env("__NGG_TEST_X", "fallback"), "fallback");
});

test("env returns fallback when var is empty string", () => {
  process.env.__NGG_TEST_X = "";
  assert.equal(env("__NGG_TEST_X", "fallback"), "fallback");
  delete process.env.__NGG_TEST_X;
});

test("env returns value when var is set", () => {
  process.env.__NGG_TEST_X = "real";
  assert.equal(env("__NGG_TEST_X", "fallback"), "real");
  delete process.env.__NGG_TEST_X;
});

test("env returns undefined when var unset and no fallback", () => {
  delete process.env.__NGG_TEST_X;
  assert.equal(env("__NGG_TEST_X"), undefined);
});
