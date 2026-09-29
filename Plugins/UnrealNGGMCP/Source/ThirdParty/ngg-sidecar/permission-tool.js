#!/usr/bin/env node
// Copyright 2025-2026 NGG. All Rights Reserved.
// permission-tool.js
//
// Tiny MCP stdio server that exposes ONE tool: `permission_gate`.
// Claude Code calls this tool via `--permission-prompt-tool
// mcp__permissions__permission_gate` before executing any tool.
//
// The tool forwards the permission request over HTTP to the main ngg-sidecar
// process (loopback port 6779) and returns the user's decision verbatim.
//
// This file intentionally does NOT depend on @modelcontextprotocol/sdk — the
// MCP stdio transport is newline-delimited JSON-RPC 2.0, and this file hand-
// rolls only the subset Claude Code requires: `initialize`, `tools/list`,
// `tools/call`. Keeping it dependency-free means the sidecar can be shipped
// without an `npm install` step.
//
// Environment:
//   NGG_PERMISSION_URL   full URL to POST permission payloads to (default: http://127.0.0.1:6779/permission)

import http from "node:http";

const PERMISSION_URL = process.env.NGG_PERMISSION_URL || "http://127.0.0.1:6779/permission";

// ---------- stderr logging (stdout is reserved for MCP JSON-RPC) ----------
function log(...args) {
  const line = `[permission-tool ${new Date().toISOString()}] ` +
    args.map(a => typeof a === "string" ? a : JSON.stringify(a)).join(" ") + "\n";
  process.stderr.write(line);
}

// ---------- JSON-RPC framing (newline-delimited on stdin/stdout) ----------
let stdinBuf = "";
process.stdin.setEncoding("utf8");
process.stdin.on("data", (chunk) => {
  stdinBuf += chunk;
  let idx;
  while ((idx = stdinBuf.indexOf("\n")) >= 0) {
    const line = stdinBuf.slice(0, idx).trim();
    stdinBuf = stdinBuf.slice(idx + 1);
    if (!line) continue;
    let msg;
    try { msg = JSON.parse(line); }
    catch (err) { log(`bad stdin json: ${err.message} line=${line.slice(0, 200)}`); continue; }
    handleMessage(msg).catch(err => log(`handleMessage threw: ${err.stack || err.message}`));
  }
});
process.stdin.on("end", () => { log("stdin closed — exiting"); process.exit(0); });

function send(obj) {
  try { process.stdout.write(JSON.stringify(obj) + "\n"); }
  catch (err) { log(`stdout write failed: ${err.message}`); }
}

function ok(id, result) { send({ jsonrpc: "2.0", id, result }); }
function err(id, code, message) { send({ jsonrpc: "2.0", id, error: { code, message } }); }

// ---------- MCP handlers ----------
const PROTOCOL_VERSION = "2024-11-05";
const SERVER_INFO = { name: "ngg-permissions", version: "1.0.0" };

const PERMISSION_GATE_TOOL = {
  name: "permission_gate",
  description:
    "Gatekeeper invoked by Claude Code before any tool call. Forwards the " +
    "request to the ngg-sidecar UI, which asks the user to approve or deny.",
  inputSchema: {
    type: "object",
    properties: {
      tool_name: { type: "string" },
      input:     { type: "object" },
      tool_use_id: { type: "string" },
    },
    required: ["tool_name", "input"],
    additionalProperties: true,
  },
};

async function handleMessage(msg) {
  const { id, method, params } = msg;
  if (method === "initialize") {
    ok(id, {
      protocolVersion: PROTOCOL_VERSION,
      capabilities: { tools: {} },
      serverInfo: SERVER_INFO,
    });
    return;
  }
  if (method === "notifications/initialized") {
    // Notification — no response.
    return;
  }
  if (method === "tools/list") {
    ok(id, { tools: [PERMISSION_GATE_TOOL] });
    return;
  }
  if (method === "tools/call") {
    const { name, arguments: args } = params || {};
    if (name !== "permission_gate") {
      err(id, -32601, `unknown tool: ${name}`);
      return;
    }
    try {
      const decision = await askMainSidecar(args || {});
      // Claude Code's permission-prompt-tool contract expects the TOOL RESULT
      // to have content[0].text contain a JSON-serialised object of the form
      //   { "behavior": "allow", "updatedInput": {...} }
      // or
      //   { "behavior": "deny", "message": "reason" }
      ok(id, {
        content: [{ type: "text", text: JSON.stringify(decision) }],
      });
    } catch (e) {
      log(`permission forward failed: ${e.stack || e.message}`);
      ok(id, {
        content: [{ type: "text", text: JSON.stringify({
          behavior: "deny",
          message: `permission sidecar unreachable: ${e.message}`,
        }) }],
      });
    }
    return;
  }
  if (method === "ping") { ok(id, {}); return; }
  // Unknown method — respond with error so Claude's RPC layer doesn't hang.
  if (typeof id !== "undefined") err(id, -32601, `method not found: ${method}`);
}

// ---------- HTTP bridge to main sidecar ----------
function askMainSidecar(args) {
  return new Promise((resolve, reject) => {
    const body = JSON.stringify({
      tool_name: args.tool_name,
      input: args.input ?? {},
      tool_use_id: args.tool_use_id ?? null,
    });
    let url;
    try { url = new URL(PERMISSION_URL); }
    catch (e) { reject(new Error(`bad NGG_PERMISSION_URL: ${e.message}`)); return; }

    const req = http.request({
      hostname: url.hostname,
      port: url.port || 80,
      path: url.pathname + (url.search || ""),
      method: "POST",
      headers: {
        "content-type": "application/json",
        "content-length": Buffer.byteLength(body),
      },
      // No timeout — wait indefinitely for the user to approve/deny in the chat window.
    }, (res) => {
      let data = "";
      res.setEncoding("utf8");
      res.on("data", (c) => { data += c; });
      res.on("end", () => {
        if (res.statusCode !== 200) {
          reject(new Error(`permission HTTP ${res.statusCode}: ${data.slice(0, 200)}`));
          return;
        }
        try { resolve(JSON.parse(data)); }
        catch (e) { reject(new Error(`permission bad json: ${e.message}`)); }
      });
    });
    req.on("error", reject);
    req.write(body);
    req.end();
  });
}

log(`ready (permission URL = ${PERMISSION_URL})`);
