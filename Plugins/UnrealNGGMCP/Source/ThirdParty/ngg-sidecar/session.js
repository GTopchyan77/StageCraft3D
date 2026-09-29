// Copyright 2025-2026 NGG. All Rights Reserved.
// Session: one Claude Code subprocess + a buffered transcript of stream-json events.
// Survives client disconnects so the UE5 plugin can reconnect and replay on editor restart.

import { spawn } from "node:child_process";
import { EventEmitter } from "node:events";
import path from "node:path";
import fs from "node:fs";
import os from "node:os";
import crypto from "node:crypto";
import { fileURLToPath } from "node:url";

const MAX_BUFFER = 5000; // events kept for replay on reconnect

// Read-only tools that should be auto-approved without prompting the user.
// The plugin UI will never see permission requests for these.
//
// Two layers:
//   1. Exact-name matches (built-in Claude Code tools)
//   2. Prefix matches for MCP server tools whose names follow stable read-only
//      patterns. Adding the prefix once auto-approves every matching tool that
//      ships in the future.
export const READ_ONLY_TOOLS = new Set([
  "Read", "Glob", "Grep", "LS",
  "BashOutput", "TodoWrite",
  "WebFetch", "WebSearch",
]);

// Each entry is matched against the tool name with String.startsWith.
// Keep these strictly read-only — a misclassification here turns into a silent
// auto-approve of a destructive call.
const READ_ONLY_PREFIXES = [
  // ue5-ngg pure inspectors. _list_*, _get_*, _read_*, _lint*, _health_check
  // and the few _list_* helpers that don't follow the verb_target pattern.
  "mcp__ue5-ngg__ue5_list_",
  "mcp__ue5-ngg__ue5_get_",
  "mcp__ue5-ngg__ue5_bp_read_",
  "mcp__ue5-ngg__ue5_bp_lint",            // covers ue5_bp_lint and ue5_bp_lint_project
  "mcp__ue5-ngg__ue5_bp_list_",
  "mcp__ue5-ngg__ue5_bt_read_",
  "mcp__ue5-ngg__ue5_gas_read_",
  "mcp__ue5-ngg__ue5_health_check",
  "mcp__ue5-ngg__ue5_list_gameplay_tags",
  "mcp__ue5-ngg__ue5_list_node_types",
  "mcp__ue5-ngg__pcg_list_node_types",
  "mcp__ue5-ngg__youtube_get_transcript",
  // context7 doc lookups (both the claude.ai MCP and the local plugin variant).
  "mcp__claude_ai_Context7__query-docs",
  "mcp__claude_ai_Context7__resolve-library-id",
  "mcp__plugin_context7_context7__query-docs",
  "mcp__plugin_context7_context7__resolve-library-id",
];

export function isReadOnlyTool(name) {
  if (!name) return false;
  if (READ_ONLY_TOOLS.has(name)) return true;
  for (const p of READ_ONLY_PREFIXES) {
    if (name.startsWith(p)) return true;
  }
  return false;
}

// Resolve the path of `permission-tool.js` that sits next to this module so
// it works regardless of the cwd the sidecar is launched from.
const __dirname = path.dirname(fileURLToPath(import.meta.url));
const PERMISSION_TOOL_SCRIPT = path.join(__dirname, "permission-tool.js");

// On Windows, decide how to spawn `claude` without shell:true. shell:true
// wraps the spawn in cmd.exe and breaks proc.kill — only cmd.exe dies and
// the underlying claude process is orphaned. We resolve the real executable
// in PATH and prefer .exe over .cmd. If only a .cmd wrapper is present we
// still avoid shell:true by invoking it through cmd.exe explicitly so the
// kill path can taskkill the tree by PID.
function resolveClaudeSpawn(claudePath) {
  if (process.platform !== "win32") {
    return { command: claudePath, prefixArgs: [] };
  }
  const found = findOnWindowsPath(claudePath);
  if (!found) {
    // Let spawn fail with ENOENT; the caller logs the error.
    return { command: claudePath, prefixArgs: [] };
  }
  if (/\.exe$/i.test(found)) {
    return { command: found, prefixArgs: [] };
  }
  if (/\.(cmd|bat)$/i.test(found)) {
    // Try to bypass the wrapper entirely by spawning the underlying cli.js
    // via the same node.exe we are running. The npm-generated wrapper
    // contains a pattern like `"%dp0%\node_modules\<pkg>\<cli.js>"` — if we
    // can extract it, we get a single-process spawn with clean kill semantics.
    try {
      const txt = fs.readFileSync(found, "utf8");
      const m = txt.match(/["']%dp0%[\\\/]([^"']+\.[mc]?js)["']/i);
      if (m) {
        const cliJs = path.join(path.dirname(found), m[1]);
        if (fs.existsSync(cliJs)) {
          return { command: process.execPath, prefixArgs: [cliJs] };
        }
      }
    } catch { /* fall through */ }
    // Fallback: spawn cmd.exe explicitly. Still better than shell:true
    // because we control the parent (cmd.exe at known PID) and can clean
    // it plus its tree with taskkill on shutdown.
    return { command: "cmd.exe", prefixArgs: ["/d", "/s", "/c", found] };
  }
  return { command: found, prefixArgs: [] };
}

function findOnWindowsPath(name) {
  if (path.isAbsolute(name) && fs.existsSync(name)) return name;
  const dirs = (process.env.PATH || "").split(path.delimiter).filter(Boolean);
  const exts = path.extname(name)
    ? [""]
    : (process.env.PATHEXT || ".COM;.EXE;.BAT;.CMD")
        .split(";").map(e => e.toLowerCase()).filter(Boolean);
  for (const dir of dirs) {
    for (const ext of exts) {
      const p = path.join(dir, name + ext);
      if (fs.existsSync(p)) return p;
    }
  }
  return null;
}

// Kill a process and all its descendants. On Windows, proc.kill only
// terminates the immediate child — taskkill /F /T walks the tree by PID
// and cleans up wrappers (cmd.exe) plus any tools claude itself spawned.
// On POSIX, proc.kill(signal) preserves SIGINT semantics so cancel still
// lets claude abort the current task gracefully.
function killProcess(proc, signal = "SIGTERM") {
  if (!proc || !proc.pid) return;
  if (process.platform === "win32") {
    try {
      const tk = spawn("taskkill", ["/F", "/T", "/PID", String(proc.pid)], {
        stdio: "ignore",
        windowsHide: true,
      });
      tk.on("error", () => { try { proc.kill(signal); } catch {} });
      return;
    } catch {
      // fall through to plain kill
    }
  }
  try { proc.kill(signal); } catch {}
}

/**
 * Build a merged MCP config file in the OS temp dir that combines:
 *   - the project's .mcp.json (if any) — so ue5-ngg etc. stay available
 *   - a new "permissions" server that runs permission-tool.js via node
 * Returns the absolute path of the temp merged file.
 */
function buildMergedMcpConfig(cwd, permissionUrl) {
  let base = { mcpServers: {} };
  const projCfg = path.join(cwd, ".mcp.json");
  if (fs.existsSync(projCfg)) {
    try {
      const txt = fs.readFileSync(projCfg, "utf8");
      const parsed = JSON.parse(txt);
      // Accept both { mcpServers: {...} } and a bare map for backward compat.
      if (parsed && parsed.mcpServers && typeof parsed.mcpServers === "object") {
        base.mcpServers = { ...parsed.mcpServers };
      } else if (parsed && typeof parsed === "object") {
        base.mcpServers = { ...parsed };
      }
    } catch {
      // Leave base as empty — the session log will note this.
    }
  }
  base.mcpServers.permissions = {
    command: process.execPath, // current node binary
    args: [PERMISSION_TOOL_SCRIPT],
    env: { NGG_PERMISSION_URL: permissionUrl },
  };
  const fname = `ngg-mcp-${crypto.randomBytes(6).toString("hex")}.json`;
  const out = path.join(os.tmpdir(), fname);
  fs.writeFileSync(out, JSON.stringify(base, null, 2));
  return out;
}

export class ClaudeSession extends EventEmitter {
  constructor({ id, cwd, claudePath = "claude", logger = console, permissionUrl }) {
    super();
    this.id = id;
    this.cwd = cwd;
    this.claudePath = claudePath;
    this.logger = logger;
    this.permissionUrl = permissionUrl || "http://127.0.0.1:6779/permission";

    this.proc = null;
    this.buffer = [];        // array of stream-json events for replay
    this.thinking = false;
    this.stdinBuf = "";
    this.stdoutBuf = "";
    this.exitReason = null;
    this.mcpConfigPath = null;
    // Optional model override for the next `claude` subprocess spawn.
    // Empty string = let Claude CLI pick its default.
    this.model = "";
    // When set_model arrives while claude is thinking, we defer the process
    // restart until the current task finishes (result event) rather than
    // killing the subprocess mid-task. This flag tracks that deferral.
    this.pendingModelRestart = false;
    // When true, spawn claude with --dangerously-skip-permissions so no tool
    // call ever routes through the sidecar/UE permission gate. Identical UX to
    // running `claude --dangerously-skip-permissions` in the terminal — used
    // for fast iteration when the developer trusts the prompt.
    this.bypassPermissions = false;
    this.pendingPermissionRestart = false;
    // Track whether we used the safe permission-prompt path or had to fall
    // back to --dangerously-skip-permissions. Surfaced in the first system
    // event so the plugin UI can show it.
    this.permissionMode = "unknown";

    // Cumulative usage accounting (populated from stream-json `result` events).
    this.usage = {
      turns:              0,
      inputTokens:        0,
      outputTokens:       0,
      cacheReadTokens:    0,
      cacheWriteTokens:   0,
      totalCostUsd:       0,
      totalDurationMs:    0,
    };
  }

  start() {
    if (this.proc) return;

    // Write a merged MCP config: project .mcp.json + our permissions server.
    try {
      this.mcpConfigPath = buildMergedMcpConfig(this.cwd, this.permissionUrl);
      this.logger.log(`[session ${this.id}] merged MCP config: ${this.mcpConfigPath}`);
      // Read back to surface server names in the chat window
      const cfgText = fs.readFileSync(this.mcpConfigPath, "utf8");
      const cfgParsed = JSON.parse(cfgText);
      const mcpNames = Object.keys(cfgParsed.mcpServers || {}).filter(n => n !== "permissions");
      if (mcpNames.length > 0) {
        this._recordAndBroadcast({ type: "system", subtype: `mcp:${mcpNames.join(",")}` });
      }
    } catch (err) {
      this.logger.error(`[session ${this.id}] could not build merged MCP config: ${err.message}`);
      this.mcpConfigPath = null;
    }

    const args = [
      "--output-format", "stream-json",
      "--input-format",  "stream-json",
      "--verbose",
    ];

    if (this.model) {
      args.push("--model", this.model);
    }

    if (this.bypassPermissions) {
      // Explicit user opt-in to bypass the permission gate entirely. We still
      // pass --mcp-config so the project's MCP servers (ue5-ngg, context7, …)
      // remain available — only the permission-prompt-tool is omitted.
      if (this.mcpConfigPath) {
        args.push("--mcp-config", this.mcpConfigPath);
      }
      args.push("--dangerously-skip-permissions");
      this.permissionMode = "bypass";
      this.logger.log(`[session ${this.id}] BYPASS mode: --dangerously-skip-permissions (user opted in)`);
    } else if (this.mcpConfigPath) {
      args.push("--mcp-config", this.mcpConfigPath);
      args.push("--permission-prompt-tool", "mcp__permissions__permission_gate");
      this.permissionMode = "prompt-tool";
    } else {
      // Fallback: if we could not write the merged config for some reason,
      // fall back to the legacy unattended mode so the developer isn't
      // blocked by a broken permission pipeline.
      args.push("--dangerously-skip-permissions");
      this.permissionMode = "dangerously-skip";
      this.logger.log(`[session ${this.id}] FALLBACK: using --dangerously-skip-permissions`);
    }

    // Headless mode: pass empty prompt via -p so it accepts stream-json stdin.
    args.push("-p", "");

    const { command, prefixArgs } = resolveClaudeSpawn(this.claudePath);
    const fullArgs = [...prefixArgs, ...args];
    this.logger.log(`[session ${this.id}] spawning: ${command} ${fullArgs.join(" ")} (cwd=${this.cwd})`);

    try {
      this.proc = spawn(command, fullArgs, {
        cwd: this.cwd,
        stdio: ["pipe", "pipe", "pipe"],
        env: process.env,
        shell: false,
        windowsHide: true,
      });
    } catch (err) {
      this.logger.error(`[session ${this.id}] spawn failed: ${err.message}`);
      this._broadcast({ type: "error", message: `Failed to launch claude: ${err.message}` });
      return;
    }

    this.proc.stdout.setEncoding("utf8");
    this.proc.stderr.setEncoding("utf8");

    this.proc.stdout.on("data", (chunk) => this._onStdout(chunk));
    this.proc.stderr.on("data", (chunk) => this.logger.error(`[claude stderr] ${chunk.trim()}`));

    this.proc.on("exit", (code, signal) => {
      this.exitReason = `exit code=${code} signal=${signal}`;
      this.logger.log(`[session ${this.id}] claude exited: ${this.exitReason}`);
      const wasIntentional = this.intentionalRestart === true;
      this.intentionalRestart = false;
      this.pendingModelRestart = false;
      this.pendingPermissionRestart = false;
      this.proc = null;
      this.thinking = false;
      this._broadcast({ type: "status", state: "idle" });
      if (!wasIntentional) {
        this._broadcast({ type: "error", message: `Claude process exited (${this.exitReason}). Send a new prompt to restart.` });
      }
      if (this.mcpConfigPath) {
        try { fs.unlinkSync(this.mcpConfigPath); } catch {}
        this.mcpConfigPath = null;
      }
    });

    this.proc.on("error", (err) => {
      this.logger.error(`[session ${this.id}] process error: ${err.message}`);
      this._broadcast({ type: "error", message: err.message });
    });

    // Emit a synthetic system event announcing the permission mode so the
    // UI can surface whether the real MCP permission flow is in use or if
    // we fell back. Wrapped as a stream-json `system` event to reuse the
    // existing rendering path.
    this._recordAndBroadcast({
      type: "system",
      subtype: `permission_mode:${this.permissionMode}`,
    });
  }

  _onStdout(chunk) {
    this.stdoutBuf += chunk;
    let idx;
    while ((idx = this.stdoutBuf.indexOf("\n")) >= 0) {
      const line = this.stdoutBuf.slice(0, idx).trim();
      this.stdoutBuf = this.stdoutBuf.slice(idx + 1);
      if (!line) continue;
      let event;
      try { event = JSON.parse(line); }
      catch (err) {
        this.logger.error(`[session ${this.id}] bad json from claude: ${line.slice(0, 200)}`);
        continue;
      }
      this._recordAndBroadcast(event);

      // Track thinking state from result events
      if (event.type === "result") {
        this.thinking = false;
        this._accumulateUsage(event);
        this._broadcast({ type: "status", state: "idle" });
        // Apply any model change that was deferred while we were thinking.
        if (this.pendingModelRestart && this.proc) {
          this.pendingModelRestart = false;
          this.logger.log(`[session ${this.id}] applying deferred model restart after task completion`);
          this.intentionalRestart = true;
          killProcess(this.proc, "SIGTERM");
        }
        // Same deferral for permission-mode flips.
        else if (this.pendingPermissionRestart && this.proc) {
          this.pendingPermissionRestart = false;
          this.logger.log(`[session ${this.id}] applying deferred permission-mode restart after task completion`);
          this.intentionalRestart = true;
          killProcess(this.proc, "SIGTERM");
        }
      }
    }
  }

  _recordAndBroadcast(event) {
    this.buffer.push({ type: "event", event });
    if (this.buffer.length > MAX_BUFFER) this.buffer.splice(0, this.buffer.length - MAX_BUFFER);
    this._broadcast({ type: "event", event });
  }

  _broadcast(msg) {
    this.emit("message", msg);
  }

  /**
   * Append a protocol-level message (not a stream-json event) to the replay
   * buffer AND broadcast it to all clients. Used for `permission_request`
   * so late-joining clients can still see pending prompts.
   */
  broadcastProtocol(msg) {
    this.buffer.push(msg);
    if (this.buffer.length > MAX_BUFFER) this.buffer.splice(0, this.buffer.length - MAX_BUFFER);
    this._broadcast(msg);
  }

  _accumulateUsage(resultEvent) {
    try {
      this.usage.turns += 1;
      if (typeof resultEvent.total_cost_usd === "number") {
        this.usage.totalCostUsd += resultEvent.total_cost_usd;
      }
      if (typeof resultEvent.duration_ms === "number") {
        this.usage.totalDurationMs += resultEvent.duration_ms;
      } else if (typeof resultEvent.total_duration_ms === "number") {
        this.usage.totalDurationMs += resultEvent.total_duration_ms;
      }
      const u = resultEvent.usage || {};
      if (typeof u.input_tokens === "number")                this.usage.inputTokens       += u.input_tokens;
      if (typeof u.output_tokens === "number")               this.usage.outputTokens      += u.output_tokens;
      if (typeof u.cache_read_input_tokens === "number")     this.usage.cacheReadTokens   += u.cache_read_input_tokens;
      if (typeof u.cache_creation_input_tokens === "number") this.usage.cacheWriteTokens  += u.cache_creation_input_tokens;
    } catch (err) {
      this.logger.error(`[session ${this.id}] usage accumulate failed: ${err.message}`);
    }
  }

  /**
   * Intercept UI-only Claude Code slash commands (/mcp, /help, /cost, /usage,
   * /clear) that the headless stream-json runtime does not implement. Skill
   * commands like /ue5-niagara, /simplify, /review MUST fall through so the
   * markdown-skill expansion on Claude's side still runs.
   *
   * Returns { handled: true, output } for a recognized local command, or
   * { handled: false } otherwise.
   */
  tryHandleLocalSlash(text) {
    const trimmed = (text || "").trim();
    if (!trimmed.startsWith("/")) return { handled: false };

    // First whitespace-delimited token, without the leading slash.
    const firstSpace = trimmed.search(/\s/);
    const head = (firstSpace === -1 ? trimmed : trimmed.slice(0, firstSpace)).toLowerCase();

    switch (head) {
      case "/mcp":   return { handled: true, output: this._localMcpList() };
      case "/help":  return { handled: true, output: this._localHelp() };
      case "/clear": {
        this.clear();
        return { handled: true, output: "Conversation cleared." };
      }
      // Interactive CLI commands — the Claude TUI renders tabbed views
      // (Status/Config/Usage/Stats) for /usage and /cost that can't be
      // reproduced in a headless stream. Launch the real CLI in a new
      // terminal so the user sees plan quota, session cost, etc. The
      // status-bar pill still shows per-turn input/output token counts
      // derived from stream-json result events.
      case "/cost":
      case "/usage":
      case "/login":
      case "/logout":
      case "/status":
        return { handled: true, output: this._launchClaudeCliCommand(head.slice(1)) };
      default:
        // Unknown slash command — pass through to Claude.
        return { handled: false };
    }
  }

  /**
   * Launch the user's `claude` CLI in a new visible terminal window so an
   * interactive command (login, logout, status) can prompt the user. The
   * headless stream-json subprocess cannot run these — they need a real TTY.
   * Supported on Windows (cmd), macOS (Terminal.app) and Linux (x-terminal-emulator).
   */
  _launchClaudeCliCommand(subcommand) {
    const safe = /^[a-z0-9_-]+$/i.test(subcommand) ? subcommand : "";
    if (!safe) return `Refusing to launch '/${subcommand}' — unsupported command name.`;

    const cliPath = this.claudePath || "claude";
    const platform = process.platform;

    // Some commands are top-level subcommands (no leading slash), others are
    // slash-prefixed. remote-control is a subcommand: `claude remote-control`.
    const cmdArg = safe === "remote-control" ? safe : `/${safe}`;

    try {
      if (platform === "win32") {
        // `start "Title" cmd /K "<cmd>"` opens a new console that stays open
        // after the command finishes. Use spawn with shell:true so the
        // `start` built-in is available.
        const line = `start "Claude ${safe}" cmd /K "\"${cliPath}\" ${cmdArg}"`;
        const child = spawn(line, {
          cwd: this.cwd,
          shell: true,
          detached: true,
          stdio: "ignore",
          env: process.env,
        });
        child.unref();
      } else if (platform === "darwin") {
        const script = `tell application "Terminal" to do script "${cliPath.replace(/"/g, '\\"')} ${cmdArg}"`;
        const child = spawn("osascript", ["-e", script], { detached: true, stdio: "ignore" });
        child.unref();
      } else {
        const term = process.env.TERMINAL || "x-terminal-emulator";
        const child = spawn(term, ["-e", `${cliPath} ${cmdArg}`], { detached: true, stdio: "ignore" });
        child.unref();
      }
    } catch (err) {
      return `Failed to launch 'claude ${cmdArg}' in a terminal: ${err.message}\n` +
             `Run it manually: ${cliPath} ${cmdArg}`;
    }

    const hint = safe === "login"
      ? "After completing OAuth in the new terminal, come back here and send a prompt — the sidecar will pick up the new credentials."
      : safe === "logout"
      ? "Credentials cleared. Run /login to re-auth."
      : safe === "remote-control"
      ? "The terminal shows your session URL and QR code. Open the Claude mobile app → Remote tab to connect."
      : "Check the new terminal for output.";
    return `Launched 'claude /${safe}' in a new terminal window.\n${hint}`;
  }

  _localHelp() {
    return [
      "Available commands:",
      "  /help                     Show this help",
      "  /clear                    Clear the current conversation",
      "  /compact                  Summarize conversation to free context",
      "  /cost, /usage             Show token usage + cost for this session",
      "  /mcp                      List configured MCP servers",
      "  /resume                   Resume a previous session",
      "  /model                    Switch Claude model",
      "  /export                   Export conversation to file",
      "  /init                     Initialize CLAUDE.md in the project",
      "  /review                   Review a pull request (skill)",
      "  /security-review          Security review of current branch (skill)",
      "  /simplify                 Review changed code (skill)",
      "  /ue5-niagara              Niagara VFX help (skill)",
      "  /ue5-umg-widgets          UMG widget help (skill)",
      "  /update-config            Configure Claude Code settings/hooks",
      "  /fewer-permission-prompts Reduce permission prompts",
      "  /keybindings-help         Customize keyboard shortcuts",
      "",
      "UI commands handled by the sidecar: /help, /mcp, /cost, /usage, /clear.",
      "All other slash commands are forwarded to Claude.",
    ].join("\n");
  }

  _localMcpList() {
    if (!this.mcpConfigPath || !fs.existsSync(this.mcpConfigPath)) {
      return "No merged MCP config available for this session.";
    }
    let parsed;
    try {
      parsed = JSON.parse(fs.readFileSync(this.mcpConfigPath, "utf8"));
    } catch (err) {
      return `Failed to read merged MCP config: ${err.message}`;
    }
    const servers = (parsed && parsed.mcpServers) || {};
    const names = Object.keys(servers);
    if (names.length === 0) return "No MCP servers configured.";

    const lines = [`Connected MCP servers (${names.length}):`];
    // Pad the name column for readability.
    const nameWidth = Math.min(24, Math.max(...names.map(n => n.length)));
    for (const name of names) {
      const cfg = servers[name] || {};
      const cmd = cfg.command || "";
      const args = Array.isArray(cfg.args) ? cfg.args.join(" ") : "";
      const desc = [cmd, args].filter(Boolean).join(" ").trim();
      lines.push(`  • ${name.padEnd(nameWidth)}  ${desc}`);
    }
    lines.push("");
    lines.push("(run /help for command list)");
    return lines.join("\n");
  }

  _localCost() {
    const u = this.usage;
    const fmt = (n) => n.toLocaleString("en-US");
    const dur = (u.totalDurationMs / 1000).toFixed(1) + "s";
    const cost = "$" + u.totalCostUsd.toFixed(4);
    return [
      "Session cost so far:",
      `  turns:           ${u.turns}`,
      `  input tokens:    ${fmt(u.inputTokens)}`,
      `  output tokens:   ${fmt(u.outputTokens)}`,
      `  cache read:      ${fmt(u.cacheReadTokens)}`,
      `  cache write:     ${fmt(u.cacheWriteTokens)}`,
      `  total cost USD:  ${cost}`,
      `  total duration:  ${dur}`,
    ].join("\n");
  }

  // images: optional array of {mediaType, data (base64)} objects
  prompt(text, images = []) {
    // If images were pasted, save each to a temp file and inject @path references
    // into the prompt text. Claude Code's @file expansion converts them to
    // vision content blocks — the stream-json inline-base64 path is not supported
    // by all CLI versions.
    let outgoing = text;
    if (Array.isArray(images) && images.length > 0) {
      const clipDir = path.join(os.tmpdir(), "ngg-clip");
      try {
        fs.mkdirSync(clipDir, { recursive: true });
        for (const img of images) {
          const ext = (img.mediaType || "image/png").split("/").pop() || "png";
          const fname = `clip_${Date.now()}_${crypto.randomBytes(4).toString("hex")}.${ext}`;
          const fpath = path.join(clipDir, fname);
          fs.writeFileSync(fpath, Buffer.from(img.data, "base64"));
          // Forward slashes; quote if spaces present
          const ref = fpath.replace(/\\/g, "/");
          outgoing += `\n${ref.includes(" ") ? `@"${ref}"` : `@${ref}`}`;
          this.logger.log(`[session ${this.id}] image written for @ref: ${fpath}`);
        }
      } catch (err) {
        this.logger.error(`[session ${this.id}] image temp-write failed: ${err.message}`);
      }
    }

    // Intercept UI-only Claude Code slash commands locally so they don't
    // vanish into the "not available in this environment" response.
    const local = this.tryHandleLocalSlash(outgoing);
    if (local.handled) {
      this.logger.log(`[session ${this.id}] local slash handled: ${outgoing.trim().split(/\s/)[0]}`);
      this._recordAndBroadcast({
        type: "user",
        message: { role: "user", content: [{ type: "text", text: outgoing }] },
      });
      this._recordAndBroadcast({
        type: "system",
        subtype: "local_command",
        text: local.output,
      });
      // Client flips to "thinking" optimistically on every prompt send. Local
      // slashes never enter the model — clear that state so the UI doesn't
      // spin forever on /cost, /usage, /help, /mcp, /clear, etc.
      this._broadcast({ type: "status", state: "idle" });
      return;
    }

    if (!this.proc) this.start();
    if (!this.proc) return;

    this.thinking = true;
    this._broadcast({ type: "status", state: "thinking" });

    // Record the user message in the replay buffer so reconnects can see it.
    const userEvent = {
      type: "user",
      message: { role: "user", content: [{ type: "text", text: outgoing }] },
    };
    this.buffer.push({ type: "event", event: userEvent });
    if (this.buffer.length > MAX_BUFFER) this.buffer.splice(0, this.buffer.length - MAX_BUFFER);

    const msg = JSON.stringify({
      type: "user",
      message: { role: "user", content: outgoing },
    }) + "\n";

    try {
      this.proc.stdin.write(msg);
      this.logger.log(`[session ${this.id}] prompt sent (${outgoing.length} chars${images.length ? `, ${images.length} image(s) via @ref` : ""})`);
    } catch (err) {
      this.logger.error(`[session ${this.id}] stdin write failed: ${err.message}`);
      this._broadcast({ type: "error", message: `Failed to send prompt: ${err.message}` });
      this.thinking = false;
      this._broadcast({ type: "status", state: "idle" });
    }
  }

  cancel() {
    if (!this.proc || !this.thinking) return;
    this.logger.log(`[session ${this.id}] cancel (SIGINT)`);
    killProcess(this.proc, "SIGINT");
    this.thinking = false;
    this._broadcast({ type: "status", state: "cancelled" });
  }

  clear() {
    this.logger.log(`[session ${this.id}] clear — killing claude and resetting buffer`);
    if (this.proc) {
      killProcess(this.proc, "SIGTERM");
      this.proc = null;
    }
    this.buffer = [];
    this.thinking = false;
    this.pendingModelRestart = false;
    this.pendingPermissionRestart = false;
    this.usage = {
      turns: 0, inputTokens: 0, outputTokens: 0,
      cacheReadTokens: 0, cacheWriteTokens: 0,
      totalCostUsd: 0, totalDurationMs: 0,
    };
    this._broadcast({ type: "status", state: "idle" });
  }

  replay() {
    return this.buffer.slice();
  }

  /**
   * Set the Claude model name used on the next spawn. Empty string or null
   * clears the override (Claude CLI chooses). If a claude subprocess is
   * already running, kill it so the next prompt respawns with the new
   * --model flag. Replay buffer is preserved.
   */
  setModel(name) {
    const next = (typeof name === "string" ? name.trim() : "");
    if (next === this.model) return;
    this.logger.log(`[session ${this.id}] model: "${this.model}" -> "${next}"`);
    this.model = next;
    this._recordAndBroadcast({
      type: "system",
      subtype: `model:${next || "default"}`,
    });
    if (this.proc) {
      if (this.thinking) {
        // Claude is mid-task — defer the restart so we don't disrupt it.
        // The kill will be applied once the current result event arrives.
        this.logger.log(`[session ${this.id}] model change deferred — claude is thinking, will restart after task`);
        this.pendingModelRestart = true;
      } else {
        this.logger.log(`[session ${this.id}] killing idle claude so next prompt uses new model`);
        this.intentionalRestart = true;
        killProcess(this.proc, "SIGTERM");
      }
    }
    // Do NOT eagerly respawn here — claude CLI in headless stream-json mode
    // waits for stdin before emitting its `init` event, so a pre-prompt spawn
    // just burns a subprocess that sits idle until it gets killed on the
    // next model switch. The next real prompt will spawn with the new model.
  }

  /**
   * Toggle the bypass-permissions mode. When true, the next claude subprocess
   * spawn passes --dangerously-skip-permissions. If a subprocess is already
   * running, kill it (or defer the kill if mid-task) so the next prompt picks
   * up the new mode.
   */
  setPermissionBypass(enabled) {
    const next = !!enabled;
    if (next === this.bypassPermissions) return;
    this.logger.log(`[session ${this.id}] bypass permissions: ${this.bypassPermissions} -> ${next}`);
    this.bypassPermissions = next;
    this._recordAndBroadcast({
      type: "system",
      subtype: `permission_bypass:${next ? "on" : "off"}`,
    });
    if (this.proc) {
      if (this.thinking) {
        this.logger.log(`[session ${this.id}] permission-mode change deferred — claude is thinking, will restart after task`);
        this.pendingPermissionRestart = true;
      } else {
        this.logger.log(`[session ${this.id}] killing idle claude so next prompt uses new permission mode`);
        this.intentionalRestart = true;
        killProcess(this.proc, "SIGTERM");
      }
    }
  }

  setCwd(newCwd) {
    if (!newCwd || newCwd === this.cwd) return;
    if (this.proc) {
      this.logger.log(`[session ${this.id}] cwd change requested while claude running — ignoring (${this.cwd} -> ${newCwd})`);
      return;
    }
    this.logger.log(`[session ${this.id}] cwd: ${this.cwd} -> ${newCwd}`);
    this.cwd = newCwd;
  }

  shutdown() {
    if (this.proc) {
      killProcess(this.proc, "SIGTERM");
      this.proc = null;
    }
    if (this.mcpConfigPath) {
      try { fs.unlinkSync(this.mcpConfigPath); } catch {}
      this.mcpConfigPath = null;
    }
  }
}
