# ngg-sidecar

A persistent Node.js daemon that hosts a headless Claude Code subprocess on
behalf of the UE5 `UnrealNGGMCP` plugin's in-editor chat window. It lives
outside the editor process so the conversation survives:

- Editor restarts (crash, `/kill_editor`, manual close)
- Plugin DLL rebuilds (Live Coding / full compile)
- Chat window close/reopen

The editor's Slate chat widget (`SNGGChatWindow`) connects to this sidecar
over a local WebSocket, sends prompts, and streams events back.

```
┌───────────────────────────────┐
│  UE5 editor (plugin Chat tab) │
│  SNGGChatWindow (Slate)       │
└──────────────┬────────────────┘
               │ WebSocket :6778
               ▼
┌───────────────────────────────┐
│  ngg-sidecar (this package)   │
│  - session registry           │
│  - replay buffer              │
│  - permission bridge          │
└──────────────┬────────────────┘
               │ spawn + stream-json stdio
               ▼
┌───────────────────────────────┐
│  claude CLI (headless)        │
│  --output-format stream-json  │
│  --permission-prompt-tool ... │
└───────────────────────────────┘
```

---

## Protocol

### WebSocket (default port 6778)

Messages are JSON-encoded, one object per WebSocket frame.

**Plugin → sidecar:**

| Type | Fields | Purpose |
|---|---|---|
| `subscribe` | `sessionId`, `replay?`, `cwd?` | Attach to a session. `replay:true` floods the client with the full buffered history. |
| `prompt` | `text`, `images?` | Submit a user turn. Images are base64; the sidecar writes them to temp files and inlines `@<path>` refs. |
| `cancel` | — | Interrupt the current claude subprocess. |
| `clear` | — | Clear the conversation and replay buffer. |
| `set_model` | `model` | Set `--model <name>` for the next spawn. Empty string clears. |
| `set_permission_bypass` | `enabled` | When on, next spawn passes `--dangerously-skip-permissions` (no permission gate). |
| `permission_response` | `id`, `decision` | Answer a pending permission prompt. `decision` ∈ {`approve`, `approve_always`, `deny`}. |

**Sidecar → plugin:**

| Type | Fields | Purpose |
|---|---|---|
| `ready` | `sessionId` | Subscribe succeeded. |
| `event` | `event` | A raw Claude stream-json event (system init, tool_use, tool_result, assistant, user, result, …). |
| `status` | `state` | Lifecycle signal: `idle` / `thinking` / `cancelled`. |
| `error` | `message` | Fatal error for the client to surface. |
| `permission_request` | `id`, `tool`, `input` | A tool needs user authorization. |

Replayed `permission_request` events carry `settled: true` and `decision: "approve"|"deny"` once the decision is known, so historical prompts render as passive text instead of live buttons.

### HTTP

| Method | Path | Port | Purpose |
|---|---|---|---|
| `GET` | `/health` | 6778 | `{ok:true, version, sessions}` |
| `POST` | `/shutdown` | 6778 | Graceful exit |
| `POST` | `/permission` | **6779** | **Internal** — `permission-tool.js` (registered as `--permission-prompt-tool` with Claude CLI) forwards here. Do not hit this from outside. |

---

## How a turn flows

1. User types a prompt in the editor's Chat window.
2. Plugin ws-sends `{type:"prompt", text}`.
3. `ClaudeSession.prompt()`:
   - Intercepts local slash commands (`/mcp`, `/help`, `/clear`, `/cost`, `/usage`, `/login`, `/logout`, `/status`) — these never hit the Claude CLI.
   - If `proc` isn't running, spawns `claude ... --output-format stream-json --input-format stream-json --permission-prompt-tool ngg-permission`.
   - Writes `{type:"user",message:{role:"user",content:<text>}}` + newline to `proc.stdin`.
4. Claude CLI streams JSON events on stdout. Each event is:
   - Recorded in the session's in-memory replay buffer (capped at 5000 entries).
   - Broadcast to every subscribed WebSocket client.
5. When Claude wants to use an unauthorized tool, it invokes the permission-prompt tool. That tool (`permission-tool.js`, running as a local MCP server on a separate loopback port) POSTs to `/permission` on this sidecar. The sidecar:
   - Generates a `permission_request` message with a random UUID.
   - Broadcasts it to all WS clients **and** buffers it for replay.
   - Awaits a matching `permission_response` from any client (10-minute timeout).
   - Replies to the `/permission` POST with `{behavior:"allow"|"deny"}` which Claude then honors.
6. A `result` event marks turn end. Status flips to `idle`.

---

## Session model

One `ClaudeSession` per `sessionId`. Typically a project uses `sessionId = "default"` (chat window doesn't expose multi-session UI yet). The Session owns:

| Field | Notes |
|---|---|
| `proc` | The spawned `claude` child. Respawned on model change or crash; replay buffer persists across respawns. |
| `buffer` | Array of `{type:"event", event}` and protocol messages (e.g. `permission_request`). Sent to clients that `subscribe` with `replay:true`. Capped at `MAX_BUFFER = 5000`. |
| `thinking` | `true` while Claude is mid-turn. |
| `usage` | Running totals: turns, input/output/cache tokens, cost, duration. Accumulated from every `result` event. |
| `cwd` | Working directory passed to `claude`. Settable pre-spawn via `setCwd`. |
| `model` | `--model` override. Changing it kills the current `proc` (next prompt respawns). |
| `mcpConfigPath` | Temp JSON file wiring up `ngg-permission` as a local MCP server for the `claude` child. |

---

## Environment variables

| Variable | Default | Purpose |
|---|---|---|
| `NGG_SIDECAR_PORT` | `6778` | WebSocket + health HTTP port |
| `NGG_PERMISSION_PORT` | `6779` | Internal permission bridge (loopback only) |
| `NGG_CLAUDE_PATH` | `claude` | Path to the Claude Code CLI binary |
| `NGG_SIDECAR_LOG_DIR` | OS-appropriate default | Directory for `sidecar-<port>.log` |
| `NGG_SIDECAR_IDLE_MS` | `0` (never) | Auto-exit after this many ms of zero clients |

The log filename embeds `NGG_SIDECAR_PORT` so multiple projects running their
own sidecars never collide on the log file (Windows holds an exclusive write
share on shell-redirected stdout, which would otherwise kill the second
sidecar's spawn).

**Default log paths** (where `<port>` is `NGG_SIDECAR_PORT`):

- Windows: `%LOCALAPPDATA%\NGG\sidecar-<port>.log`
- macOS: `~/Library/Logs/NGG/sidecar-<port>.log`
- Linux: `~/.local/state/NGG/sidecar-<port>.log`

---

## Install

```bash
cd Plugins/UnrealNGGMCP/Source/ThirdParty/ngg-sidecar
npm install        # pulls ws
```

The plugin's `FSidecarLauncher` autoboots the sidecar on editor startup if it
isn't already running. Running `node index.js` directly is useful for
debugging — hit `GET /health` to confirm.

---

## Files

```
Plugins/UnrealNGGMCP/Source/ThirdParty/ngg-sidecar/
├── index.js             HTTP + WebSocket server, session registry, permission bridge
├── session.js           ClaudeSession: spawns + streams the claude CLI, local-slash interception
├── permission-tool.js   MCP server registered as --permission-prompt-tool; forwards to /permission
└── package.json
```

---

## Troubleshooting

**Port 6778 already in use** — another sidecar is running, or a previous instance didn't exit cleanly. `POST /shutdown` to the old one or kill the stale `node` process.

**Chat window says "sidecar did not exit within 6s — reconnecting anyway"** — the sidecar was restarting. Normal during plugin rebuild; reconnect should succeed within a few seconds.

**Permission prompts never appear in the chat** — the `claude` CLI may have been spawned without `--permission-prompt-tool`. Check the sidecar log for the spawn command line and confirm `ngg-permission` is wired in.

**Historical permission prompts still show live buttons after reconnect** — the in-memory `resolvedPermissions` map plus the buffered `permission_request` both carry the decision. If this fails, the buffer was cleared (e.g., `/clear`) or the decision was sent before `settlePermission` got a chance to mutate the buffered event.

**Turn never finishes (`status` stays `thinking`)** — the claude child died mid-turn without emitting a `result`. Check `sidecar-<port>.log` for `claude exited`. `cancel` will force the session back to idle.

**Images don't resolve** — the plugin passes base64 image bytes; the sidecar writes them to `<LOG_DIR>/images/clip_<ts>_<rand>.png` and inlines an `@<path>` ref. If the ref resolves to something Claude can't read (wrong permissions, path with unescaped spaces), the image is ignored. Check the sidecar log for `image written for @ref:` lines.
