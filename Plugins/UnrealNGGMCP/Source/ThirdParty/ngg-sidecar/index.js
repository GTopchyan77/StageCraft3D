#!/usr/bin/env node
// Copyright 2025-2026 NGG. All Rights Reserved.
// ngg-sidecar: persistent daemon that hosts a Claude Code subprocess so the UE5
// plugin's chat window survives editor restarts and rebuilds.
//
// Protocol (WebSocket JSON lines on :6778):
//   plugin -> sidecar: {type:"subscribe",sessionId,replay?,cwd?}
//                      {type:"prompt",text}
//                      {type:"cancel"}
//                      {type:"clear"}
//                      {type:"set_model", model}     (empty string clears override)
//                      {type:"set_permission_bypass", enabled}
//                      {type:"permission_response", id, decision:"approve"|"approve_always"|"deny"}
//                      {type:"list_models"}          (request the live model catalog)
//   sidecar -> plugin: {type:"ready",sessionId,version,pid}
//                      {type:"event",event:<claude stream-json object>}
//                      {type:"status",state:"idle"|"thinking"|"cancelled"}
//                      {type:"error",message}
//                      {type:"permission_request", id, tool, input}
//                      {type:"models", models:[{id,display_name}], source:"api"|"error", message?}
//
// HTTP:
//   GET  /health              -> { ok, version, sessions }
//   POST /shutdown            -> graceful exit
//   POST /permission (6779)   -> internal-only: permission-tool.js forwards here

import http from "node:http";
import fs from "node:fs";
import path from "node:path";
import os from "node:os";
import crypto from "node:crypto";
import { spawn } from "node:child_process";
import { WebSocketServer } from "ws";
import { ClaudeSession, READ_ONLY_TOOLS, isReadOnlyTool } from "./session.js";

const PORT             = parseInt(process.env.NGG_SIDECAR_PORT ?? "6778", 10);
const PERMISSION_PORT  = parseInt(process.env.NGG_PERMISSION_PORT ?? "6779", 10);
const PERMISSION_URL   = `http://127.0.0.1:${PERMISSION_PORT}/permission`;
const CLAUDE_BIN       = process.env.NGG_CLAUDE_PATH ?? "claude";
// Bumped together with kExpectedSidecarVersion in
// Plugins/UnrealNGGMCP/.../Chat/SidecarLauncher.cpp and the "version" field
// in package.json. Mismatches cause the chat window to shut this process
// down on connect and respawn the matching one.
const VERSION          = "1.5.0";
const IDLE_TIMEOUT_MS  = parseInt(process.env.NGG_SIDECAR_IDLE_MS ?? "0", 10); // 0 = never auto-exit

// ---------- logging ----------
function defaultLogDir() {
  const p = process.platform;
  if (p === "win32") return path.join(process.env.LOCALAPPDATA ?? os.tmpdir(), "NGG");
  if (p === "darwin") return path.join(os.homedir(), "Library", "Logs", "NGG");
  return path.join(os.homedir(), ".local", "state", "NGG");
}
const LOG_DIR  = process.env.NGG_SIDECAR_LOG_DIR ?? defaultLogDir();
// Per-project log filename. PORT is project-unique (the UE launcher derives it
// from the .uproject path hash), so embedding it here keeps each project's
// sidecar in its own log file. Without this, two projects share `sidecar.log`
// and on Windows the second sidecar's `>"sidecar.log"` shell redirect can
// collide with the first sidecar's open handle, killing the spawn before node
// ever starts.
const LOG_FILE = path.join(LOG_DIR, `sidecar-${PORT}.log`);
try { fs.mkdirSync(LOG_DIR, { recursive: true }); } catch {}
const logStream = fs.createWriteStream(LOG_FILE, { flags: "a" });
// On Windows the previous sidecar process may still hold the file handle
// briefly after exit, causing EBUSY on the first write. Absorb the error
// so it doesn't bubble up as an uncaughtException and kill the new process.
let logStreamOk = true;
logStream.on("error", (err) => {
  logStreamOk = false;
  process.stderr.write(`[ngg-sidecar] log stream error (writes degraded to stderr): ${err.message}\n`);
});

function ts() { return new Date().toISOString(); }
function log(...args) {
  const line = `[${ts()}] ${args.map(a => typeof a === "string" ? a : JSON.stringify(a)).join(" ")}\n`;
  if (logStreamOk) logStream.write(line);
  process.stdout.write(line);
}
function logErr(...args) {
  const line = `[${ts()}] ERROR ${args.map(a => typeof a === "string" ? a : JSON.stringify(a)).join(" ")}\n`;
  if (logStreamOk) logStream.write(line);
  process.stderr.write(line);
}
const logger = { log, error: logErr };

// ---------- session registry ----------
/** @type {Map<string, ClaudeSession>} */
const sessions = new Map();

function getOrCreateSession(id) {
  let s = sessions.get(id);
  if (!s) {
    s = new ClaudeSession({
      id,
      cwd: process.cwd(),
      claudePath: CLAUDE_BIN,
      logger,
      permissionUrl: PERMISSION_URL,
    });
    sessions.set(id, s);
    log(`session created: ${id}`);
  }
  return s;
}

// ---------- model catalog ----------
// The chat window's model dropdown is populated from the live Anthropic Models
// API (GET /v1/models) rather than a hardcoded list, so it auto-follows new
// releases. Auth reuses whatever the `claude` CLI already uses: an explicit
// ANTHROPIC_API_KEY / ANTHROPIC_AUTH_TOKEN if present, otherwise the OAuth
// access token the CLI stored in ~/.claude/.credentials.json. The plugin keeps
// its own small hardcoded fallback list, so a failure here just means the
// dropdown shows that fallback instead of breaking.
const MODELS_CACHE_MS = 5 * 60 * 1000;
let cachedModels = null; // { at:number, models:[{id,display_name}] }

function resolveAnthropicAuth() {
  const base = { "anthropic-version": "2023-06-01" };
  if (process.env.ANTHROPIC_API_KEY) {
    return { ...base, "x-api-key": process.env.ANTHROPIC_API_KEY };
  }
  if (process.env.ANTHROPIC_AUTH_TOKEN) {
    return { ...base, "Authorization": `Bearer ${process.env.ANTHROPIC_AUTH_TOKEN}`, "anthropic-beta": "oauth-2025-04-20" };
  }
  try {
    const credPath = path.join(os.homedir(), ".claude", ".credentials.json");
    const cred = JSON.parse(fs.readFileSync(credPath, "utf8"));
    const tok = cred && cred.claudeAiOauth && cred.claudeAiOauth.accessToken;
    if (tok) {
      return { ...base, "Authorization": `Bearer ${tok}`, "anthropic-beta": "oauth-2025-04-20" };
    }
  } catch { /* no creds file — fall through */ }
  return null;
}

async function fetchModels() {
  if (cachedModels && (Date.now() - cachedModels.at) < MODELS_CACHE_MS) {
    return cachedModels.models;
  }
  if (typeof fetch !== "function") throw new Error("global fetch unavailable (node < 18)");
  const headers = resolveAnthropicAuth();
  if (!headers) throw new Error("no Anthropic credentials (set ANTHROPIC_API_KEY or sign in with `claude /login`)");

  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), 10000);
  try {
    const r = await fetch("https://api.anthropic.com/v1/models?limit=100", { headers, signal: ctrl.signal });
    if (!r.ok) throw new Error(`models API HTTP ${r.status}`);
    const j = await r.json();
    const models = Array.isArray(j.data)
      ? j.data
          .filter(m => m && typeof m.id === "string")
          .map(m => ({ id: m.id, display_name: (typeof m.display_name === "string" && m.display_name) ? m.display_name : m.id }))
      : [];
    cachedModels = { at: Date.now(), models };
    return models;
  } finally {
    clearTimeout(timer);
  }
}

// ---------- pending permission requests ----------
// id -> { resolve, reject, timeoutHandle, tool, input, sessionId }
const pendingPermissions = new Map();
// id -> "approve" | "deny"  (persisted so replay can mark them settled)
const resolvedPermissions = new Map();

function createPermissionRequest({ tool, input, sessionId }) {
  const id = crypto.randomUUID();
  const promise = new Promise((resolve, reject) => {
    // No timeout — wait indefinitely for the user to approve/deny in the chat window.
    pendingPermissions.set(id, { resolve, reject, timeoutHandle: null, tool, input, sessionId });
  });
  return { id, promise };
}

function settlePermission(id, decisionObj) {
  const entry = pendingPermissions.get(id);
  if (!entry) { log(`permission_response for unknown id=${id} — ignoring`); return false; }
  if (entry.timeoutHandle !== null) clearTimeout(entry.timeoutHandle);
  pendingPermissions.delete(id);
  entry.resolve(decisionObj);
  const decision = decisionObj.behavior === "allow" ? "approve" : "deny";
  resolvedPermissions.set(id, decision);

  // Also mutate the buffered permission_request event across every session so
  // future replays carry the settled flag directly. Previously the `settled`
  // marker only came from the resolvedPermissions map, which is lost on
  // sidecar restart — leaving historical prompts rendered with active-looking
  // buttons after a reconnect.
  for (const s of sessions.values()) {
    if (!Array.isArray(s.buffer)) continue;
    for (const m of s.buffer) {
      if (m && m.type === "permission_request" && m.id === id) {
        m.settled  = true;
        m.decision = decision;
      }
    }
  }
  return true;
}

// ---------- HTTP server on PORT (health + shutdown + ws upgrade) ----------
const httpServer = http.createServer((req, res) => {
  if (req.method === "GET" && req.url === "/health") {
    res.writeHead(200, { "content-type": "application/json" });
    res.end(JSON.stringify({
      ok: true,
      version: VERSION,
      port: PORT,
      pid: process.pid,
      sessions: Array.from(sessions.keys()),
      permission_port: PERMISSION_PORT,
    }));
    return;
  }
  if (req.method === "POST" && req.url === "/shutdown") {
    res.writeHead(200, { "content-type": "application/json" });
    res.end(JSON.stringify({ ok: true, shutting_down: true }));
    log("shutdown requested via HTTP");
    shutdown(0);
    return;
  }
  res.writeHead(404);
  res.end();
});

// ---------- internal permission HTTP server on PERMISSION_PORT ----------
// Bound to 127.0.0.1 only — external clients cannot hit this.
const permissionServer = http.createServer((req, res) => {
  if (!(req.method === "POST" && req.url === "/permission")) {
    res.writeHead(404); res.end(); return;
  }
  let body = "";
  req.setEncoding("utf8");
  req.on("data", (c) => { body += c; if (body.length > 2 * 1024 * 1024) req.destroy(); });
  req.on("end", async () => {
    let payload;
    try { payload = JSON.parse(body); }
    catch (e) {
      res.writeHead(400, { "content-type": "application/json" });
      res.end(JSON.stringify({ behavior: "deny", message: `bad json: ${e.message}` }));
      return;
    }
    const tool = String(payload.tool_name ?? "");
    const input = payload.input ?? {};

    // Read-only tools auto-approve without bothering the user.
    if (isReadOnlyTool(tool)) {
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify({ behavior: "allow", updatedInput: input }));
      log(`permission auto-allow (read-only): ${tool}`);
      return;
    }

    // Otherwise, broadcast to every connected WS client and wait.
    // (We broadcast to all because we don't know which plugin instance the
    // user is looking at — multiple editor windows on one session is rare
    // but not impossible. First response wins.)
    const { id, promise } = createPermissionRequest({ tool, input, sessionId: null });

    const requestMsg = { type: "permission_request", id, tool, input };
    let subscribersNotified = 0;
    for (const ws of wss.clients) {
      if (ws.readyState !== ws.OPEN) continue;
      try { ws.send(JSON.stringify(requestMsg)); subscribersNotified++; } catch {}
    }
    // Also record in the attached session buffer so editor reconnects can
    // see the pending prompt.
    for (const s of sessions.values()) s.broadcastProtocol(requestMsg);

    log(`permission_request id=${id} tool=${tool} — notified ${subscribersNotified} clients`);

    if (subscribersNotified === 0) {
      // No one is listening — deny by default to avoid hanging Claude.
      pendingPermissions.delete(id);
      resolvedPermissions.set(id, "deny");
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify({ behavior: "deny", message: "no UE editor connected to approve" }));
      return;
    }

    try {
      const decision = await promise;
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify(decision));
    } catch (e) {
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify({ behavior: "deny", message: e.message }));
    }
  });
});

// ---------- websocket server ----------
const wss = new WebSocketServer({ server: httpServer });

wss.on("connection", (ws, req) => {
  log(`ws client connected from ${req.socket.remoteAddress}`);
  /** @type {ClaudeSession | null} */
  let attached = null;
  let onMessage = null;

  const send = (msg) => {
    try { ws.send(JSON.stringify(msg)); } catch (err) { logErr(`ws send failed: ${err.message}`); }
  };

  const detach = () => {
    if (attached && onMessage) attached.off("message", onMessage);
    attached = null;
    onMessage = null;
  };

  ws.on("message", (raw) => {
    let msg;
    try { msg = JSON.parse(raw.toString()); }
    catch (err) { send({ type: "error", message: `bad json: ${err.message}` }); return; }

    switch (msg.type) {
      case "subscribe": {
        const id = String(msg.sessionId ?? "default");
        detach();
        attached = getOrCreateSession(id);
        if (msg.cwd) attached.setCwd(String(msg.cwd));
        onMessage = (m) => send(m);
        attached.on("message", onMessage);

        if (msg.replay) {
          const replay = attached.replay();
          log(`replaying ${replay.length} events to ${id}`);
          for (const m of replay) {
            if (m.type === "permission_request" && resolvedPermissions.has(m.id)) {
              send({ ...m, settled: true, decision: resolvedPermissions.get(m.id) });
            } else {
              send(m);
            }
          }
        }
        send({ type: "ready", sessionId: id, version: VERSION, pid: process.pid });
        send({ type: "status", state: attached.thinking ? "thinking" : "idle" });
        break;
      }
      case "prompt": {
        if (!attached) { send({ type: "error", message: "not subscribed" }); return; }
        const imgs = Array.isArray(msg.images) ? msg.images : [];
        attached.prompt(String(msg.text ?? ""), imgs);
        break;
      }
      case "cancel": {
        if (!attached) return;
        attached.cancel();
        break;
      }
      case "clear": {
        if (!attached) return;
        attached.clear();
        break;
      }
      case "set_model": {
        if (!attached) { send({ type: "error", message: "not subscribed" }); return; }
        attached.setModel(String(msg.model ?? ""));
        break;
      }
      case "set_permission_bypass": {
        if (!attached) { send({ type: "error", message: "not subscribed" }); return; }
        attached.setPermissionBypass(!!msg.enabled);
        break;
      }
      case "list_models": {
        // Account-wide — no session attachment required.
        fetchModels()
          .then((models) => {
            send({ type: "models", models, source: "api" });
            log(`list_models -> ${models.length} models`);
          })
          .catch((err) => {
            log(`list_models failed: ${err.message}`);
            send({ type: "models", models: [], source: "error", message: err.message });
          });
        break;
      }
      case "permission_response": {
        const pid = String(msg.id ?? "");
        const decision = String(msg.decision ?? "deny");
        // Map plugin-facing decision → Claude-facing schema.
        // `approve_always` is handled plugin-side (it sends "approve" plus
        // caches the tool locally). For robustness, we also accept it here
        // and treat it as "approve" if it ever arrives.
        let out;
        if (decision === "approve" || decision === "approve_always") {
          // Echo the input back unchanged as updatedInput.
          const entry = pendingPermissions.get(pid);
          out = { behavior: "allow", updatedInput: entry ? entry.input : {} };
        } else {
          out = { behavior: "deny", message: "user denied the tool call" };
        }
        const settled = settlePermission(pid, out);
        log(`permission_response id=${pid} decision=${decision} settled=${settled}`);
        break;
      }
      default:
        send({ type: "error", message: `unknown message type: ${msg.type}` });
    }
  });

  ws.on("close", () => {
    log("ws client disconnected (session kept alive)");
    detach();
  });

  ws.on("error", (err) => logErr(`ws error: ${err.message}`));
});

// ---------- idle auto-exit (optional) ----------
if (IDLE_TIMEOUT_MS > 0) {
  setInterval(() => {
    if (wss.clients.size === 0) {
      log(`idle timeout reached with no clients — exiting`);
      shutdown(0);
    }
  }, IDLE_TIMEOUT_MS);
}

// ---------- startup / shutdown ----------
function shutdown(code = 0) {
  log("shutting down");
  for (const s of sessions.values()) s.shutdown();
  wss.close();
  httpServer.close(() => {
    try { permissionServer.close(); } catch {}
    logStream.end();
    process.exit(code);
  });
  setTimeout(() => process.exit(code), 2000).unref();
}

process.on("SIGINT",  () => shutdown(0));
process.on("SIGTERM", () => shutdown(0));
process.on("uncaughtException", (err) => {
  // Write to stderr directly — logStream may itself be the source of the error.
  process.stderr.write(`[ngg-sidecar] uncaught: ${err.stack ?? err.message}\n`);
  if (logStreamOk) logStream.write(`[${ts()}] ERROR uncaught: ${err.stack ?? err.message}\n`);
});

httpServer.listen(PORT, "127.0.0.1", () => {
  log(`ngg-sidecar v${VERSION} listening on http://127.0.0.1:${PORT}`);
  log(`log file: ${LOG_FILE}`);
  log(`claude bin: ${CLAUDE_BIN}`);
});

httpServer.on("error", (err) => {
  logErr(`http listen failed: ${err.message}`);
  if (err.code === "EADDRINUSE") {
    logErr(`port ${PORT} in use — another sidecar is probably already running. Exiting.`);
  }
  process.exit(1);
});

permissionServer.listen(PERMISSION_PORT, "127.0.0.1", () => {
  log(`permission bridge listening on http://127.0.0.1:${PERMISSION_PORT}/permission`);
});
permissionServer.on("error", (err) => {
  logErr(`permission http listen failed: ${err.message}`);
  if (err.code === "EADDRINUSE") {
    logErr(`permission port ${PERMISSION_PORT} in use — another sidecar is probably already running.`);
  }
});
