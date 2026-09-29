// Copyright 2025-2026 NGG. All Rights Reserved.
// helpers.js — unrealngg-mcp
// Tiny, side-effect-free helpers shared between index.js and tests.

/**
 * Preprocess helper for Zod schemas: if the caller passed a JSON string, try
 * to parse it; otherwise return the value unchanged. Lets MCP tools accept
 * both `{x:1}` (object) and `'{"x":1}'` (JSON string) for the same schema.
 *
 * @param {unknown} val
 * @returns {unknown}
 */
export function jsonPreprocess(val) {
  if (typeof val === "string") {
    try { return JSON.parse(val); } catch { return val; }
  }
  return val;
}

/**
 * Read env var at call time (defers binding so tests can override after import).
 *
 * @param {string}  name
 * @param {string}  [fallback]
 * @returns {string|undefined}
 */
export function env(name, fallback) {
  const v = process.env[name];
  return v === undefined || v === "" ? fallback : v;
}
