# Claude Chat Window — UE5 Editor Guide

The **Claude Chat** window is a dockable Slate panel inside the Unreal Editor that runs a real Claude Code conversation against your project — without leaving the editor. It is part of the `UnrealNGGMCP` plugin.

This doc covers:

1. [What it is](#what-it-is)
2. [Opening the window](#opening-the-window)
3. [Anatomy of the window](#anatomy-of-the-window)
4. [Visual tutorials](#visual-tutorials)
5. [Sending prompts](#sending-prompts)
6. [Slash commands](#slash-commands)
7. [`@` file/folder references](#-filefolder-references)
8. [Image paste](#image-paste)
9. [Permission prompts](#permission-prompts)
10. [Bypass-permissions toggle](#bypass-permissions-toggle)
11. [Plan-mode toggle](#plan-mode-toggle)
12. [Selection context (`+ Selection`)](#selection-context--selection)
13. [Blueprint-editor selection banner](#blueprint-editor-selection-banner)
14. [Model selector](#model-selector)
15. [Effort selector (extended thinking)](#effort-selector-extended-thinking)
16. [Token-usage pill](#token-usage-pill)
17. [Conversation persistence (sidecar)](#conversation-persistence-sidecar)
18. [Reconnect / Restart Sidecar](#reconnect--restart-sidecar)
19. [Cancel / Clear](#cancel--clear)
20. [Input history](#input-history)
21. [Color legend](#color-legend)
22. [Settings persistence](#settings-persistence)
23. [Troubleshooting](#troubleshooting)

---

## What it is

```
┌──────────────────────────────────┐
│  UE5 Editor (Claude Chat tab)    │
│  SNGGChatWindow (Slate)          │
└─────────────────┬────────────────┘
                  │ WebSocket :6778
                  ▼
┌──────────────────────────────────┐
│  ngg-sidecar (Node daemon)       │
│  - keeps the conversation alive  │
│  - replays history on reconnect  │
│  - bridges permission prompts    │
└─────────────────┬────────────────┘
                  │ spawn + stream-json stdio
                  ▼
┌──────────────────────────────────┐
│  claude CLI (headless)           │
└──────────────────────────────────┘
```

Because the Claude process lives in the **sidecar** daemon (not the editor), your conversation survives:

- Editor restarts (close, crash, `kill_editor`)
- Plugin DLL rebuilds (Live Coding or full compile)
- Closing and reopening the Chat tab

When the window reattaches, it replays the buffered history so the conversation looks exactly like you left it.

---

## Opening the window

Two equivalent entry points:

| Entry point | Where |
|---|---|
| **Toolbar button** | Level Editor → Play toolbar → **Claude** |
| **Window menu** | Window → Developer Tools → **Claude Chat** |

The first time you open it, the plugin auto-spawns the `ngg-sidecar` daemon if it isn't already running. The status text reads `Connecting…` until the WebSocket handshake completes, then switches to `Ready`.

---

## Anatomy of the window

```
┌──────────────────────────────────────────────────────────────────────────┐
│  [Status text]  [Throbber]  [Model ▼] [Effort ▼]  [Usage]  [Reconnect]   │  ← top bar
│                                                  [↻ Sidecar] [Bypass] [Plan]
├──────────────────────────────────────────────────────────────────────────┤
│  [BP-selection banner — only visible when BP nodes are selected]         │
├──────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  Terminal-style transcript (mono, colored, selectable, copyable)         │
│                                                                          │
│  [user]      hello                                                       │
│  [assistant] Hi — how can I help?                                        │
│  [tool_use]  Read(/Game/Foo.uasset)                                      │
│  [tool_ok]   {"path":"/Game/Foo",...}                                    │
│                                                                          │
├──────────────────────────────────────────────────────────────────────────┤
│  [Pending permission rows — only visible when claude is asking]          │
├──────────────────────────────────────────────────────────────────────────┤
│  ┌────────────────────────────────────────────────┐ [Send]               │
│  │ Ask Claude... (Enter, Ctrl+Enter, @ for file) │ [Cancel]              │
│  └────────────────────────────────────────────────┘ [Clear]              │
│                                                     [+ Selection (n)]    │
└──────────────────────────────────────────────────────────────────────────┘
```

---

## Visual tutorials

Short clips demonstrating each key feature. See [`media/README.md`](media/README.md) for capture specs (≤ 1280 × 720, ≤ 5 MB, 5–15 s GIF or up to ~30 s MP4) and recommended tools (ScreenToGif, ShareX, OBS).

> **Status:** placeholders. Drop the recorded files into `Docs/media/` using the exact filenames below — the embed lines pick them up automatically.

### 1. Open the tab

**What it shows:** Click the **Claude** toolbar button → tab docks → status bar flips from `Connecting…` to `Ready`.

**Capture (~8 s):** start with the editor's default Play toolbar visible; click **Claude**; let the tab finish docking; pause one beat after `Ready` appears.

![Open the Claude Chat tab](media/01-open-tab.gif)

### 2. First prompt + streaming response

**What it shows:** Type a short prompt, press Enter, watch `[user]` appear cyan, then `[assistant]` stream a green response token-by-token.

**Capture (~12 s):** type *"hi, what does this project do?"*; press Enter; capture until the first paragraph of the response is on screen.

![First prompt and streaming response](media/02-first-prompt.gif)

### 3. Slash-command popup

**What it shows:** `/` opens the popup, typing filters, ↓ moves the highlight, Tab accepts.

**Capture (~10 s):** type `/`, then `re`, then `view`; arrow-down to `/review`; press Tab; clip ends as the input box shows `/review`.

![Slash command popup with filtering](media/03-slash-commands.gif)

### 4. `@` file/folder picker

**What it shows:** `@` opens the project file picker, fuzzy filtering, Enter inserts the relative path.

**Capture (~10 s):** type *"explain @Sour"*; show the picker filtering to `Source/...` files; arrow to a file; press Enter; clip ends with `@Source/ThrowBomb/Core/...` in the input box.

![@ file/folder picker](media/04-at-mentions.gif)

### 5. Image paste

**What it shows:** Take a screenshot to clipboard, focus the chat input, Ctrl+V → `[system] Image captured…` line; send a prompt and the image goes with it.

**Capture (~12 s):** Win+Shift+S a region of the editor → click input box → Ctrl+V → show the system line → type *"what does this widget do?"* → Send.

![Pasting a screenshot from clipboard](media/05-image-paste.gif)

### 6. Permission prompts

**What it shows:** Claude requests a tool call; `[perm]` row appears with **Approve / Always / Deny**; click one; row collapses to a passive system line.

**Capture (~12 s):** start with Bypass OFF; ask *"list files in Source/"*; when the `[perm] Bash` row appears, hover the buttons, click **Approve**; clip ends as the row collapses and the tool result streams.

![Permission prompt — Approve / Always / Deny](media/06-permission-prompts.gif)

### 7. Bypass-permissions toggle

**What it shows:** Click `Bypass: OFF` → button label flips to `Bypass: ON` → next tool call runs without a prompt.

**Capture (~10 s):** click the button; status briefly shows `Spawning…`; ask *"list files in Source/"*; the tool runs immediately, no `[perm]` row.

![Bypass-permissions toggle](media/07-bypass-toggle.gif)

### 8. `+ Selection` toggle

**What it shows:** Select an actor in the level → label updates to `+ Selection (1)` → toggle ON → next prompt is prefixed with a `<ue5_selection>` block.

**Capture (~10 s):** select 2 actors in the viewport; click the toggle; type *"what are these?"*; Send; in the transcript, hover the user line so the `<ue5_selection>` prefix is visible.

![Selection-context toggle](media/09-selection-toggle.gif)

### 9. Blueprint-editor selection banner

**What it shows:** Open a Blueprint, marquee-select a few nodes — the chat window grows a blue banner *"3 nodes selected in BP_X — try: …"*.

**Capture (~10 s):** open `BP_ThrowBombGameMode`; select 3 nodes in the event graph; switch focus back to chat; banner appears; deselect; banner hides.

![Blueprint-editor selection banner](media/10-bp-selection-banner.gif)

### 10. Model selector

**What it shows:** Open the model dropdown → pick a different model → status bar updates the resolved model id (e.g. `claude-sonnet-4-6-...`).

**Capture (~10 s):** click the model combo (start on Opus 4.7); pick **Sonnet 4.6**; show `Spawning…`; resolved id under the combo updates.

![Model selector](media/11-model-selector.gif)

### 11. Effort selector

**What it shows:** Open the effort dropdown → pick `high` → next prompt is sent with `think harder` prepended (visible in the user line).

**Capture (~8 s):** pick `high`; type *"refactor this function"*; Send; the user line shows the trigger phrase.

![Effort (extended thinking) selector](media/12-effort-selector.gif)

### 12. Conversation persistence

**What it shows:** Send a few messages → close the editor → reopen → reopen the Chat tab → conversation replays exactly.

**Capture (~25 s, MP4 recommended):** chat 2–3 turns; File → Exit; reopen the project; open the Claude tab; show the transcript replaying. Use `media/13-conversation-persistence.mp4` and update the embed below.

<video src="media/13-conversation-persistence.mp4" controls width="720"></video>

### 13. Cancel / Clear

**What it shows:** Send a long-running prompt → click **Cancel** mid-response → Claude stops; click **Clear** → transcript and token counter reset.

**Capture (~12 s):** ask *"explain everything in Source/ThrowBomb/Grid/"*; mid-stream click **Cancel**; show the response truncates; click **Clear**; transcript empties; token pill resets to `0 tok`.

![Cancel and Clear](media/14-cancel-clear.gif)

---

## Sending prompts

| Action | How |
|---|---|
| Send the message | **Enter** (no modifier) or click **Send** |
| Insert a newline | **Ctrl+Enter** (also Shift+Enter / Alt+Enter) |
| Cancel current response | Click **Cancel** while Claude is thinking or running tools |

The input box auto-grows up to a few visible lines, then scrolls.

---

## Slash commands

Type `/` at the start of the input to open a filtered popup of slash commands. Keep typing to filter.

| Key | Action |
|---|---|
| `↑` / `↓` | Move highlight |
| `Enter` or `Tab` | Accept the highlighted command |
| `Esc` | Close the popup |

Bundled commands (Claude Code built-ins + skills exposed by your environment):

```
/help              Show Claude Code help
/clear             Clear the current conversation
/compact           Summarize conversation to free context
/cost              Show token usage for current session
/mcp               List connected MCP servers and their tools
/usage             Show API usage stats
/resume            Resume a previous session
/model             Switch Claude model for this session
/login             Open a terminal to sign in to Claude
/logout            Sign out of Claude (in a new terminal)
/status            Show Claude CLI auth + account status
/export            Export conversation to file
/init              Initialize CLAUDE.md in the project
/review            Review a pull request
/security-review   Run a security review on the current branch
/simplify          Review changed code for quality & simplicity
/loop              Run a command on a recurring schedule
/schedule          Manage scheduled background agents
/claude-api        Build or migrate Claude API apps
/ue5-niagara       Niagara VFX creation and tuning help
/ue5-umg-widgets   UMG widget layout and design help
/update-config     Configure Claude Code settings/hooks
/fewer-permission-prompts
/keybindings-help
```

Any other skill registered in your Claude Code environment is also dispatched by typing `/<skill-name>` — the popup just lists the curated set.

---

## `@` file/folder references

Type `@` followed by part of a filename to open a fuzzy file/folder picker scoped to the project root. Heavy folders (`Binaries`, `Intermediate`, `Saved`, `node_modules`, `.git`) are skipped.

| Key | Action |
|---|---|
| `↑` / `↓` | Move highlight |
| `Enter` or `Tab` | Insert the selected path as `@<relative-path>` |
| `Esc` | Close the popup |

Claude reads `@<path>` references as file attachments. Use this to hand it specific code without copy-pasting.

---

## Image paste

**Ctrl+V** with a bitmap on the clipboard captures the image, encodes it as PNG, and queues it for the next message. A system line confirms:

```
[system] Image captured (1920x1080) — will be sent with next message [total: 1]
```

You can attach multiple images before pressing Send. Text paste (no image on clipboard) falls through to the text box's normal paste handler.

---

## Permission prompts

When Claude wants to call a tool that isn't already pre-allowed (e.g. `Bash`, `Write`, `Edit`), the sidecar surfaces a permission row above the input box:

```
[perm]  Bash → ls Source/                     [Approve] [Always] [Deny]
```

| Button | Effect |
|---|---|
| **Approve** | Allow this single call. |
| **Always** | Allow this call **and** auto-approve future calls to the same tool for the rest of the session. |
| **Deny** | Reject the call; Claude is told you said no. |

After a decision, the buttons collapse and the row becomes a passive `Permission was granted: Bash`-style system line in the transcript.

The "Always" set is **project-scoped and persisted**, so once you grant a tool you don't get re-prompted next session.

---

## Bypass-permissions toggle

Top-bar button: `Bypass: OFF` ↔ `Bypass: ON`.

- When **ON**, the sidecar respawns the Claude CLI with `--dangerously-skip-permissions`. No per-call permission gate, no roundtrip — fastest possible tool calls.
- When **OFF** (default), every non-allowlisted tool call surfaces a [perm] row.

Use only when you trust the prompt. Saved per project.

---

## Selection context (`+ Selection`)

Bottom-right toggle, label updates live: `+ Selection (0)`, `+ Selection (3)`, etc.

When **ON**, every prompt you send is automatically prefixed with a `<ue5_selection>` block describing your current editor selection:

- Selected actors in the level (label, class, location)
- Selected assets in the Content Browser (path, class)

This is the no-friction way to ask things like *"explain what these assets do"* or *"refactor these actors to use a shared component"* without paraphrasing the selection.

The toggle is local to the Chat window (not persisted) — tapping it once is the workflow.

---

## Blueprint-editor selection banner

If you have **nodes selected inside an open Blueprint editor**, a slim banner appears between the top bar and the transcript:

```
3 nodes selected in BP_ThrowBombGameMode — try: "explain these nodes" or "refactor this branch"
```

The banner refreshes ~2× per second and disappears when you deselect. It's a hint that Claude has access to the BP selection through the `ue5_bp_get_selection` MCP tool — you can ask things like:

- *"What does the selected branch do?"*
- *"Replace these three nodes with a single function call."*
- *"Why does this connection look wrong?"*

Claude calls the MCP tool itself to read the selection — no manual copy-paste of node IDs needed.

---

## Model selector

Top-bar dropdown. Changes the `--model` flag the sidecar passes when (re)spawning Claude.

| Group | Entries |
|---|---|
| **Pinned current-gen** | Opus 4.7, Sonnet 4.6, Haiku 4.5 |
| **Latest aliases** (auto-follow new releases) | Opus (latest), Sonnet (latest), Haiku (latest) |
| **Older pinned** | Opus 4.6, Sonnet 4.5, Opus 3 |
| **Default** | Let the Claude CLI pick (no `--model` flag) |

The chosen model is saved per project. After picking a new model, the next prompt triggers a sidecar respawn so the change takes effect immediately. The status bar shows the **resolved** model id reported by the CLI's `init` event (e.g. `claude-opus-4-7-20251115`) so you can confirm what an alias mapped to.

---

## Effort selector (extended thinking)

Top-bar dropdown. Maps to Claude Code's "thinking budget" trigger keywords, which scale how much the model deliberates before answering.

| Label | Trigger phrase prepended | When to use |
|---|---|---|
| `low` | `think` | Small tweaks, quick lookups |
| `medium` | `think hard` | Default for non-trivial tasks |
| `high` | `think harder` | Multi-file refactors, tricky bugs |
| `xhigh` | `ultrathink` | Architecture-level decisions |
| `max` | `ultrathink` | Same as xhigh; reserved for "go all out" |
| `auto` | *(nothing)* | Let Claude decide — no keyword injected |

The chosen effort is prepended to your prompt as a one-liner (`think hard\n\n<your prompt>`). Slash commands like `/clear` or `/login` are sent untouched — the trigger is only injected for free-form prompts. Persisted per project.

---

## Token-usage pill

The status bar shows cumulative usage for the current conversation:

```
1.2k in / 4.7k out · 2 turns
```

Includes input, output, and (when reported) cache-read / cache-write counts. Resets on **Clear** or `/clear`. Persisted per project so the count survives editor restarts.

Cost display is intentionally off by default — the pill is for awareness, not billing. Use `/cost` if you want a detailed breakdown.

---

## Conversation persistence (sidecar)

The sidecar holds:

- The currently running Claude CLI subprocess
- A replay buffer of every `assistant`, `user`, `tool_use`, `tool_result`, `system`, and `result` event since the last clear
- The pending-permissions queue

When the editor reconnects (after a restart, rebuild, or tab close), the sidecar **replays** the buffer to the new client. Already-decided permission prompts come back as passive system lines, not interactive rows.

Stop the sidecar to fully reset:

- Click **↻ Sidecar** in the top bar, **or**
- Use the OS task manager to kill `node.exe ngg-sidecar` (the next chat reconnect will respawn it)

---

## Reconnect / Restart Sidecar

| Button | What it does |
|---|---|
| **Reconnect** *(visible only when disconnected)* | Re-runs the WebSocket handshake against the existing sidecar. Use after a transient network blip. |
| **↻ Sidecar** | Kills the daemon and respawns a fresh one. Use after editing `Plugins/UnrealNGGMCP/Source/ThirdParty/ngg-sidecar/*.js` — the daemon caches its old code across editor restarts, so this is the only way to pick up sidecar changes. |

A 5-second port-release grace and spawn retry are built in, so the restart works reliably even if the OS hasn't fully released port 6778 yet.

---

## Cancel / Clear

| Button | Effect |
|---|---|
| **Cancel** | Interrupts the running Claude turn (ends generation, stops in-flight tools). The transcript is preserved. |
| **Clear** | Wipes the conversation and the sidecar's replay buffer. Resets the token-usage counter. Equivalent to `/clear`. |

---

## Input history

The input box supports a per-project prompt history (last 50 entries):

| Key | Action |
|---|---|
| `↑` (single-line input, on first line) | Previous prompt |
| `↓` (single-line input, on last line)  | Next prompt |
| Edit + `Enter` | Send the (possibly edited) historical prompt |

History navigation is suppressed while you're typing a slash-command (the popup owns Up/Down) or when the input contains a newline (so multi-line edits aren't disrupted).

---

## Color legend

The transcript renders each event kind with a stable color and prefix:

| Kind | Prefix | Color |
|---|---|---|
| User | `[user]` | cyan |
| Assistant | `[assistant]` | green |
| Thinking (extended thinking) | `[thinking]` | purple |
| Tool call | `[tool_use]` | yellow |
| Tool success | `[tool_ok]` | green |
| Tool error | `[tool_err]` | red |
| Error | `[error]` | red |
| System | `[system]` | gray |
| Permission prompt | `[perm]` | orange |

Fenced code blocks (` ``` `) are highlighted: bright green for the body, with token-level coloring for JSON and C++ (keys, strings, numbers, keywords, types, comments, preprocessor, identifiers). The whole transcript is selectable — `Ctrl+A` then `Ctrl+C` copies everything.

---

## Settings persistence

These settings are saved to `<Project>/Saved/Config/<Platform>/EditorPerProjectUserSettings.ini` under section `[NGGChat]`:

| Key | Purpose |
|---|---|
| `PromptHistory` | Last 50 prompts you sent |
| `Model` | Selected model CLI argument |
| `Effort` | Selected effort id (`low`, `medium`, `high`, `xhigh`, `max`, `auto`) |
| `AutoApproveTools` | Tools you marked **Always** |
| `BypassPermissions` | Bypass toggle state |

Token-usage counters are persisted alongside these so the running totals survive an editor restart.

Because everything is project-scoped, opening a different `.uproject` gives you a clean slate — your model choice for project A doesn't leak into project B.

---

## Troubleshooting

### Status stuck on "Connecting…" / "Sidecar not responding"
1. Check the Output Log for `LogNGGChat` — the sidecar spawn happens on first connect.
2. Confirm Node.js is on PATH (the plugin spawns `node.exe ngg-sidecar/index.js`).
3. Click **↻ Sidecar** to force a fresh spawn.
4. If port 6778 is already in use, set `NGG_SIDECAR_PORT` to a free port.

### "Bypass" or "Plan" toggle didn't take effect
Both flags are applied at sidecar (re)spawn time. The sidecar respawns Claude after a flag change — give it a moment, then submit your next prompt. The status text briefly shows `Spawning…` during the swap.

### Permission prompts don't appear
- Make sure **Bypass** is OFF.
- Some tools are pre-allowed via `.claude/settings.json` and skip the gate by design — that's not a bug.

### "No image in clipboard" after Ctrl+V
The capture path only handles bitmap clipboard formats (e.g. screenshots, image-editor copies). Copying an image **file** from File Explorer puts a file reference on the clipboard, not a bitmap — paste it as `@<path>` instead.

### Sidecar code changes aren't picked up
The daemon caches its JavaScript. Click **↻ Sidecar** (or kill the Node process manually) to reload from disk.

### "@" picker is missing files
`InitAtMentions` skips heavy folders (`Binaries`, `Intermediate`, `Saved`, `node_modules`, `.git`) and caps the index at a few thousand entries. If your project has more files than the cap, narrow your query — fuzzy matching ranks deeper paths once you type more characters.

### Where do I find the raw chat code?
- Slate widget: `Plugins/UnrealNGGMCP/Source/UnrealNGGMCP/Private/Chat/SNGGChatWindow.{h,cpp}`
- Sidecar client: `Plugins/UnrealNGGMCP/Source/UnrealNGGMCP/Private/Chat/FSidecarClient.{h,cpp}`
- Tab spawner / toolbar button: `Plugins/UnrealNGGMCP/Source/UnrealNGGMCP/Private/Chat/NGGChatTabSpawner.{h,cpp}`
- Daemon: `Plugins/UnrealNGGMCP/Source/ThirdParty/ngg-sidecar/` (see its own `README.md` for protocol details)
