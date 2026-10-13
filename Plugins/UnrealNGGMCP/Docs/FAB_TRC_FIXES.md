# Fab TRC — fix log (latest review response: 2026-07-08)

This file is maintainer documentation. It is **not** shipped: the packaging
script excludes it by name (`/XF FAB_TRC_FIXES.md`), it is not listed in
`Config/FilterPlugin.ini`, and the script's coverage check hard-fails if it
ever lands in the staged package. **It contains live reviewer test keys — never
add it to the filter or the zip.**

## Review response received 2026-07-08 (round 4)

**Fail — "Technical Information text must identify dependencies (if any),
prerequisites, or other requirements for use of the asset."** Reviewer note:
*"The project files includes Third Party (TP) code. However, the listing
option for TP was not enabled."*

Two-part problem, two-part fix:

1. **The TP listing option was not enabled on the product page.** This is the
   root cause and it is a **manual portal step, not a repo change**: on the
   Fab publisher portal, edit the listing and set the third-party software
   option to **[THIS PRODUCT USES THIRD-PARTY SOFTWARE]** (Yes), then fill the
   per-library questions from the "Third-Party Software form" section below
   (the five items: @modelcontextprotocol/sdk, zod, ws, youtube-transcript,
   Meshy.ai). Round 3's checklist already said this, but the option evidently
   did not end up enabled on the submitted listing — after setting it, **save
   and re-open the listing page to confirm the option stuck** before
   submitting. The Formstack TPS Declaration Form (already submitted in
   round 3) complements, but does not replace, this on-page option.

2. **Technical Information now names the dependencies explicitly.** The
   previous block said "their third-party npm libraries are fetched at install
   time" without identifying them. The block below now lists every
   third-party item by name/version/license, plus the prerequisites (Node.js
   18+, npm, MCP client) as its own bullet, so the Technical Information text
   itself identifies all dependencies, prerequisites, and requirements.

### Reviewer-notes addition for this item (paste with the template)

```
THIRD-PARTY LISTING OPTION + DEPENDENCIES IN TECHNICAL INFORMATION: the
product page option [THIS PRODUCT USES THIRD-PARTY SOFTWARE] is now enabled,
with the five third-party items declared in the listing's third-party
software section: @modelcontextprotocol/sdk 1.27.1 (MIT), zod 3.25.76 (MIT),
ws 8.20.0 (MIT), youtube-transcript 1.3.1 (MIT), and the optional Meshy.ai
web API (no code distributed). The same five items were declared on Epic's
TPS Declaration Form (Formstack). The Technical Information text now
explicitly identifies these dependencies and all prerequisites (Node.js 18+
and npm; the in-editor chat additionally needs an MCP client such as the
Claude Code CLI). None of the third-party libraries ship in the package -
they are fetched on the end user's machine by a one-time npm install, pinned
by the shipped human-readable package-lock.json.
```

**Fail — "C++ third-party code and libraries are in a ThirdParty folder
located inside the Source folder. Python third-party code and libraries
should be placed under Content/Python/Lib/site-packages/."**

Root cause: `bp-extract` — our **first-party**, pure-Python (stdlib-only)
Blueprint→C++ helper — lived under `Source/ThirdParty/bp-extract/` (it was
moved there in round 2 alongside the Node tools). To a reviewer that reads as
Python third-party code outside `Content/Python/Lib/site-packages/`. Fix:

1. **Moved the Python out of `Source/ThirdParty/`:**
   `Source/ThirdParty/bp-extract/` → `Content/Python/bp-extract/` (git mv,
   history preserved). `Content/Python/` is the engine-standard home for
   plugin Python. There is deliberately **no `Content/Python/Lib/site-packages/`**
   because the plugin distributes zero third-party Python — bp-extract uses
   only the Python standard library.
2. `UnrealNGGMCP.uplugin`: `CanContainContent` flipped to `true` (the plugin
   now has a `Content/` folder).
3. `FilterPlugin.ini`: the `/Source/ThirdParty/bp-extract/...` entry replaced
   with `/Content/Python/...` (declared explicitly even though `Content/` is
   standard, so no pipeline strips the loose `.py`/`.md` files).
4. All path references updated: README.md (incl. the `py Plugins\...` command
   examples), TRANSFER_GUIDE.md, THIRD_PARTY_NOTICES.md + its generator
   `gen-third-party-notices.mjs`, and the vault-template docs. No C++, MCP
   server, or packaging-script code referenced the old path (verified by
   grep). The remaining `Source/ThirdParty/` content is exactly what the TRC
   wants there: the two Node tools whose third-party npm libraries are
   declared on the TPS form.

### Reviewer-notes addition for the Python placement item (paste with the template)

```
PYTHON CODE PLACEMENT: the only Python in the product, bp-extract, now lives
at Content/Python/bp-extract/. It is first-party code written by us (not
third-party) and uses only the Python standard library - the plugin
distributes no third-party Python code or libraries at all, which is why
there is no Content/Python/Lib/site-packages/ folder (nothing to put there;
no pip install is required or performed). The Source/ThirdParty/ folder now
contains only the two Node.js tools whose third-party npm libraries are
declared on the Third-Party Software form.
```

## Review response received 2026-07-06 (round 3)

**Fail — "All code dependent on Unreal Engine source code must be included in
the plugin folder in human-readable source form. Externally compiled static
libraries or DLLs that contain no Unreal Engine source code dependencies may
be included and are subject to review."** (No separate reviewer note — likely
tied to the CreateProc/executables comment on the "assets complete" item.)

Audit result: the package already complies — verified by scanning every staged
file's *contents* (not just extensions): all files are pure text except the
declared PNG images (icon + doc illustrations); the only UE-dependent code is
the C++ module, shipped fully as `.cpp`/`.h`/`.Build.cs`; there are no static
libraries, DLLs, executables, or minified code. Fixes made the compliance
explicit and mechanical:

1. `Package-UnrealNGGMCP.ps1` gained a **byte-level human-readable check**:
   any staged non-PNG file containing binary content aborts packaging (on top
   of the existing exe/dll/cmd/bat/msi/so/dylib/node/lib/pdb name blocklist).
2. README gained a **"Source-form compliance"** subsection stating the above,
   including that the npm libraries are not distributed in any form but
   fetched by the user from the public registry, pinned by the shipped
   human-readable `package-lock.json`, and fully auditable.

### Reviewer-notes addition for the source-form item (paste with the template)

```
SOURCE FORM / NO COMPILED LIBRARIES: everything in the package is
human-readable source. The only code dependent on Unreal Engine source is the
C++ module UnrealNGGMCP, included entirely as .cpp/.h/.Build.cs. The bundled
Node tools are plain .js/.mjs source and bp-extract is plain .py source. The
package contains no compiled static libraries, DLLs, executables, or
minified code - the only binary files are PNG images (plugin icon and
documentation illustrations). Our packaging pipeline enforces this: it
rejects executable/library file types by name and additionally scans every
file's bytes, refusing to package any non-text file. The npm dependencies of
the Node tools are not distributed in any form; they are fetched by npm on
the end user's machine from the public registry, pinned exactly by the
shipped human-readable package-lock.json, and are all auditable open-source
JavaScript (declared on the TPS form).
```

**Fail — "For Plugins that use Third Party Code, the Third Party Software form
is accurately filled out."** Reviewer note: *"Any plugin which would like to
include a code file from another person or compiled library must declare that
their product uses Third Party Software and fill out our Third Party Software
Declaration Form"* — with a link to the actual form:
<https://epicgames.formstack.com/forms/third_party_software_declaration_form>

What round 2 missed: answering the third-party questions on the **product
page** is not the whole requirement — Epic wants their standalone **Formstack
declaration form** submitted as well. This is a manual step; do it once per
submission round (resubmitting the form with the same product name updates the
declaration).

### How to submit the TPS Declaration Form (manual, before resubmitting)

1. On the product page keep **[THIS PRODUCT USES THIRD-PARTY SOFTWARE]**
   selected with the per-library answers already documented in the
   "Third-Party Software form" section below.
2. Open <https://epicgames.formstack.com/forms/third_party_software_declaration_form>
   signed in as the publisher account (artavazdng256@gmail.com).
3. Fill the publisher/product fields: publisher name as on Fab, contact
   email, product name **AI Editor Bridge for UE5 — MCP Server + In-Editor
   Chat** (use the exact listing title).
4. Declare the five items using the answer sheet below — the form repeats
   its software-detail questions per item (use "add another" / list them all
   if it is a single free-text section).
5. Submit, and mention in the reviewer notes that the TPS Declaration Form
   was submitted on <date> for this product, listing the five items.

### TPS Declaration Form answer sheet (one row per third-party software)

All versions are the exact pins from the shipped `package-lock.json` files.
None of these is compiled into, statically/dynamically linked to, or
distributed with the plugin — items 1–4 are Node.js libraries the end user
fetches via `npm install`; item 5 is an external web API with no code at all.

| Field | 1. @modelcontextprotocol/sdk | 2. zod | 3. ws | 4. youtube-transcript | 5. Meshy.ai |
|---|---|---|---|---|---|
| Version | 1.27.1 | 3.25.76 | 8.20.0 | 1.3.1 | n/a (web API) |
| Author / source | Anthropic — npmjs.com/package/@modelcontextprotocol/sdk | Colin McDonnell — npmjs.com/package/zod | websockets/ws — npmjs.com/package/ws | npmjs.com/package/youtube-transcript | Meshy — https://www.meshy.ai |
| License | MIT | MIT | MIT | MIT | Proprietary ToS (user's own account) |
| License URL | github.com/modelcontextprotocol/typescript-sdk/blob/main/LICENSE | github.com/colinhacks/zod/blob/main/LICENSE | github.com/websockets/ws/blob/master/LICENSE | github.com/Kakulukian/youtube-transcript/blob/master/LICENSE | meshy.ai/terms |
| What it does / why needed | MCP protocol SDK — the bundled MCP server uses it to talk to MCP clients (Claude Code) | Validates MCP tool input parameters | Localhost WebSocket server for the chat sidecar daemon | Fetches public YouTube subtitles for the optional transcript tool | Optional text-to-3D generation API for `ue5_meshy_generate` |
| Distributed with product? | No — fetched by end user via `npm install`, pinned by shipped package-lock.json | No — same | No — same | No — same | No — remote HTTPS API, no code exists |
| Linked into binary? | No (Node.js script dependency, separate process) | No | No | No | No |
| Sends data to third parties? | No — stdio + localhost only | No — no network access | No — localhost only | Only when the user invokes the tool: requests the transcript from YouTube's public endpoints | Only when the user invokes the tool with their own API key: prompt sent to Meshy, model downloaded |

Everything else in the package is first-party: the C++ module, the two Node
tools' own source files (including `meshy.js`, our own small HTTPS client for
Meshy's public API), and the pure-Python (stdlib-only) `bp-extract`.

### Reviewer-notes addition for the TPS item (paste with the template)

```
THIRD-PARTY SOFTWARE: the Third Party Software Declaration Form
(epicgames.formstack.com/forms/third_party_software_declaration_form) was
submitted on <FILL IN DATE> for this product, declaring the five items:
@modelcontextprotocol/sdk 1.27.1 (MIT), zod 3.25.76 (MIT), ws 8.20.0 (MIT),
youtube-transcript 1.3.1 (MIT), and the Meshy.ai web API (optional, no code
distributed). None of them ships in the package or is linked into the plugin
binary - the four npm libraries are fetched by the end user via npm install
(pinned by the shipped package-lock.json), and Meshy.ai is a remote HTTPS API
that is inert unless the user supplies their own key. The product page's
third-party software section is filled out accordingly.
```

**Fail — "FilterPlugin.ini filters in custom folders the publisher intends to
distribute (Docs or similar)."** Reviewer note: *"The FilterPlugin.ini needs
to list all folders or files that are outside the expected file structure and
are intended to be included."*

The package and the filter had drifted: the shipped README links to
`Docs/ChatWindow.md`, `Docs/Tutorial.md`, `Docs/ReviewerTesting.md` and embeds
`Docs/images/*.png`, but the whole `Docs/` folder was stripped from the zip
and absent from `FilterPlugin.ini`; meanwhile the six `*.mjs` live-test files
and `.npmignore`/`.gitignore` shipped without being declared. Fix:

1. **Ship + declare the user-facing docs:** `FilterPlugin.ini` now lists
   `/Docs/ChatWindow.md`, `/Docs/Tutorial.md`, `/Docs/ReviewerTesting.md`,
   `/Docs/images/...`, and `/Source/ThirdParty/unrealngg-mcp/*.mjs`. The
   packaging script no longer excludes `Docs/` wholesale — it excludes only
   the maintainer-only pieces: `FAB_TRC_FIXES.md` (this file — it contains the
   reviewer test keys), `Docs/design/`, `Docs/media/`, plus the dev artifacts
   `.npmignore`, `.gitignore`, and the two `gen-*.mjs` generators. Everything
   excluded is deliberately absent from the filter too.
2. **Drift can't recur:** `Package-UnrealNGGMCP.ps1` gained a hard
   verification step — every staged file must be either standard plugin
   structure (`.uplugin`, `Config/`, `Resources/`, `Content/`,
   `Source/UnrealNGGMCP/`) or matched by a `FilterPlugin.ini` entry, else the
   script aborts and prints the undeclared paths. Verified both ways: a clean
   run packages 106 files, and a planted undeclared file makes it fail.

**Fail — "All assets are complete and function as intended."** Reviewer note:
*"We do not allow executables or program installer files. Your plugin launches
external processes using FPlatformProcess::CreateProc(...), including cmd.exe
and node.exe, and some launches are hidden/really hidden. Please clearly
document why this is required for the MCP/Node sidecar workflow and confirm
that no arbitrary commands or executables can be passed or executed."*

Two-part fix:

1. **Hardening (code change):** the only two launch sites that accepted any
   request data were `/editor/build_and_run` and `/editor/kill_and_restart`
   (`NGGHttpServer.cpp`), whose `target`/`config` body fields were interpolated
   into the PowerShell command line unvalidated. Both fields are now strictly
   validated before anything is launched: `target` must match
   `[A-Za-z0-9_-]{1,128}` and `config` must be one of Debug / DebugGame /
   Development / Test / Shipping; anything else returns HTTP 400. With that,
   every command line the plugin can ever execute is a fixed template over
   hard-coded executables, engine-derived paths, plugin-folder paths, integer
   ports, and these two whitelisted tokens. (Both routes were already disabled
   by default — HTTP 403 unless the user opts in by setting `AuthToken`.)

2. **Documentation:** new README section **"External processes — inventory
   and security model"** — a complete table of all four `CreateProc` sites
   (first-run `npm install`, chat-sidecar daemon, the two build/relaunch
   routes), why each is required, why the windows are hidden (background
   console tools; output goes to logged files + editor toasts, not discarded),
   and the guarantees above. It also states explicitly that the plugin ships
   no executables and never downloads or runs arbitrary binaries.

Include the "External processes" answer below in the reviewer notes on
resubmission.

### Reviewer-notes addition for the CreateProc question (paste with the template)

```
WHY THE PLUGIN LAUNCHES EXTERNAL PROCESSES (FPlatformProcess::CreateProc)
Full write-up: README section "External processes - inventory and security model".
The plugin ships no executables. There are exactly four launch sites:
1) First-run "npm install" (cmd.exe, hidden) inside the two bundled Node tool
   folders - required because Fab does not allow shipping node_modules or
   executables, so the pinned dependencies (package-lock.json) are installed
   once on the user's machine. Output goes to Saved/UnrealNGGMCP/npm-install-*.log
   with an in-editor progress toast.
2) The chat sidecar daemon (node.exe via cmd.exe "start /B", hidden/really
   hidden) - a localhost-only background daemon with no UI of its own (the
   Slate chat tab is the UI); it is launched detached so the conversation
   survives editor restarts. Its stdout goes to a log file under Saved/.
3+4) /editor/build_and_run and /editor/kill_and_restart (powershell.exe,
   running only the engine's own Build.bat and UnrealEditor.exe from
   FPaths::EngineDir()) - lets the AI rebuild the project and relaunch the
   editor. DISABLED BY DEFAULT: both return HTTP 403 unless the user
   explicitly configures an AuthToken, and then require that bearer token.
No arbitrary commands or executables can be passed or executed: every
executable path is a hard-coded literal, engine-derived, or resolved from the
local PATH; no HTTP/MCP/chat data reaches a command line except the build
routes' "target" and "config" fields, which are validated against
[A-Za-z0-9_-]{1,128} and the fixed UBT config list (Debug/DebugGame/
Development/Test/Shipping) - anything else is rejected with HTTP 400 and
nothing is launched. Hidden windows are used only because these are background
console tools; their output is captured to log files, not discarded.
```

**Fail — "1. If needed, the Publisher provides either linked or in-editor
documentation/tutorials."** Reviewer note: *"In order to properly review
products that require signing up/license or API keys, any test material must
be provided so that the product's features can be tested to the fullest before
publishing."*

Two-part fix:

1. **Documentation:** added [`Docs/ReviewerTesting.md`](ReviewerTesting.md)
   (linked from the README, which is the listing's Documentation URL). It shows
   that the HTTP bridge and all 171 MCP tools are testable with **no account or
   key** (curl + the free MCP Inspector + `npm test`), and gives exact
   credential steps for the only two keyed features: the in-editor Chat
   (Anthropic) and the optional `ue5_meshy_generate` (Meshy.ai).

2. **Test material for the reviewer (manual step on resubmission):** Fab still
   requires the keys themselves. Before resubmitting:
   - Anthropic test key: **done** — filled into the template below. Make sure
     its workspace has a **low monthly spend limit** (e.g. $5–10) on
     <https://console.anthropic.com>, and rotate/delete it after the review.
   - Meshy.ai test key: **done** — `<MESHY_TEST_KEY — not stored in the repo>`
     is filled into the template below. The example project's `.mcp.json` has
     an empty `MESHY_API_KEY` env field ready for it (kept empty in the repo
     so the key never lands on GitHub) — the reviewer pastes the key there or
     sets it as an environment variable. Rotate/delete the key on
     <https://www.meshy.ai> after the review.
   - Paste the template below into the Fab submission's **notes for the
     reviewer** field, filling in the remaining Anthropic key.

### Reviewer-notes template (paste into the Fab submission, fill in keys)

```
TESTING THE PRODUCT — full guide: https://github.com/Artavazdng256/Unreal_NGG_MCP/blob/main/Docs/ReviewerTesting.md

Most of the product needs NO account or API key:
- The in-editor HTTP bridge: open the project, then curl http://localhost:6776/health
- All 171 MCP tools: from the project root run
  npx @modelcontextprotocol/inspector node Plugins/UnrealNGGMCP/Source/ThirdParty/unrealngg-mcp/bootstrap.js
  (free local web UI, no sign-up) and invoke any tool, e.g. ue5_health_check, ue5_create_material.
- Automated tests: npm test inside Plugins/UnrealNGGMCP/Source/ThirdParty/unrealngg-mcp

Test key for the in-editor Chat window (the only feature needing an account):
  ANTHROPIC_API_KEY = <ANTHROPIC_TEST_KEY — not stored in the repo>
Set it as an environment variable, restart the editor, open Window -> Developer Tools -> Claude Chat.
The key is spend-limited and will be rotated after review.

Optional Meshy.ai text-to-3D tool (ue5_meshy_generate):
  MESHY_API_KEY = <MESHY_TEST_KEY — not stored in the repo>
  (paste it into the empty MESHY_API_KEY field in the example project's
  .mcp.json, or set it as an environment variable; the key is spend-limited
  and will be rotated after review)
```

## Review of 2026-06 (round 2) — 2 fails and their fixes

**Fail 1 — "C++ third-party code and libraries are in a ThirdParty folder
located inside the Source folder."** Reviewer note: *"All ThirdParty code must
be in clearly marked folder that is directly under the Source folder"*, listing
Meshy.ai, youtube-transcript, @modelcontextprotocol/sdk, zod, ws.

Fix: both Node packages moved from `Tools/` to `Source/ThirdParty/`:
- `Tools/unrealngg-mcp/` → `Source/ThirdParty/unrealngg-mcp/`
- `Tools/ngg-sidecar/`   → `Source/ThirdParty/ngg-sidecar/`

Everything third-party the reviewer flagged now lives (or npm-installs) under
`Source/ThirdParty/`: the npm libraries are declared in those packages'
`package.json`/`package-lock.json` and install into `node_modules` inside those
same folders; the Meshy.ai API client (`meshy.js`) is in
`Source/ThirdParty/unrealngg-mcp/`. All C++ launch/install paths, `.mcp.json`,
`FilterPlugin.ini`, and docs were updated. The remaining first-party tooling
(`bp-extract` and the two dev-only gen-*.mjs scripts) moved from `Tools/` to
`Source/ThirdParty/` as well, so the plugin root no longer has a `Tools/` folder
(bp-extract is pure Python standard library — no third-party code).

**Fail 2 — "For Plugins that use Third Party Code, the Third Party Software
form is accurately filled out."** The previous submission declared *no*
third-party software; the reviewer rejected that. On the product page you must
select **[THIS PRODUCT USES THIRD-PARTY SOFTWARE]** and answer every question.
Use the per-library answers in the "Third-Party Software form" section below.

## How to resubmit

1. Run `pwsh -File Scripts\Package-UnrealNGGMCP.ps1` from the repo root.
   It stages the plugin, **verifies** there are no executables, license files,
   node_modules, missing copyright headers, or over-long paths, and writes
   `Dist\UnrealNGGMCP.zip`. Never zip the plugin folder by hand — the previous
   rejection happened because the working-copy zip included
   `Tools/*/node_modules/` (thousands of third-party files, LICENSE files,
   and `.cmd` shims).
2. Upload `Dist\UnrealNGGMCP.zip` as the Project File.
3. Paste the Technical Information block below into the listing.
4. Answer the Third-Party Software form as described below.
5. Submit Epic's **TPS Declaration Form** (Formstack) — see "How to submit the
   TPS Declaration Form" above; fill its date into the TPS reviewer-notes
   block.
6. Create the test API key(s) and paste the filled-in reviewer-notes template
   (top of this file) into the submission's notes-for-reviewer field,
   together with the CreateProc and TPS additions.

## Technical Information block (paste into the Fab listing)

> **Fab field limit: 1900 characters.** Fab's rich-text field counts each blank
> line and mid-sentence wrap as paragraph/line-break markup, so a double-spaced,
> hard-wrapped block inflates well past 1900 even when the raw text is smaller.
> Paste the **compact, no-blank-line** version below (~1827 chars — close to
> the cap; re-count after any edit). Do not
> re-introduce blank lines between sections or break sentences across lines.
>
> If Fab has **separate inputs** for Number of Blueprints, Number of C++ Classes,
> Network Replicated, Supported Platforms, etc. (its usual layout), the 1900-char
> cap applies only to the free-text **Important/Additional Notes** box — put just
> the three-bullet notes portion there (~1012 chars) and fill the rest field by field.

```
Features:
- Embedded HTTP bridge in the UE5 editor exposing 170+ asset/Blueprint/material/Niagara/UMG/PCG/GAS/Sequencer operations to MCP clients
- In-editor AI chat tab (Slate) driven by a local Node.js sidecar
- Blueprint linting, reading and authoring over MCP
- Pure-Python Blueprint-to-C++ extraction helper (bp-extract)
Code Modules: UnrealNGGMCP (Editor)
Number of Blueprints: 0
Number of C++ Classes: 6
Network Replicated: No
Supported Development Platforms: Windows: Yes, Mac: Yes, Linux: Yes
Supported Target Build Platforms: None — editor-only plugin, no runtime module; adds nothing to packaged builds and can stay enabled when packaging for any platform.
Documentation: https://github.com/Artavazdng256/Unreal_NGG_MCP#readme
Example Project: https://github.com/Artavazdng256/Unreal_NGG_MCP_Example.git
Important/Additional Notes:
- Prerequisites: Node.js 18+ and npm (nodejs.org). The in-editor chat additionally requires an MCP-capable client such as the Claude Code CLI, installed separately. The optional bp-extract helper (Content/Python) needs Python 3.10+ — standard library only, no pip packages.
- Third-party software (declared in the listing's third-party software section): @modelcontextprotocol/sdk 1.27.1 (MIT), zod 3.25.76 (MIT), ws 8.20.0 (MIT), youtube-transcript 1.3.1 (MIT), and the optional Meshy.ai web API (user-supplied key; no code distributed). None ship in the package: the two bundled Node tools (MCP server, chat sidecar) under Source/ThirdParty ship as source only, and their libraries are fetched by a one-time npm install in each tool folder, pinned by the shipped package-lock.json (see README).
- Auto-enables the built-in engine plugins it depends on: Enhanced Input, Niagara, Python Editor Script Plugin, Geometry Script, Gameplay Abilities, StateTree, PropertyBindingUtils, PCG.
```

## Third-Party Software form

On the product page select **[THIS PRODUCT USES THIRD-PARTY SOFTWARE]** and
declare the five items the reviewer listed. None of them is compiled into the
plugin binary — they are Node.js libraries fetched by `npm install` into
`Source/ThirdParty/*/node_modules` (pinned by the shipped
`package-lock.json`), or in Meshy.ai's case an external web API. Copy the
answers below into the form.

### 1. @modelcontextprotocol/sdk (Anthropic — MIT license)

- **What does this software do?** Official JavaScript SDK for the Model
  Context Protocol (MCP): message framing, tool registration, and stdio
  transport for MCP servers.
- **Why does your plugin need it?** The bundled MCP server
  (`Source/ThirdParty/unrealngg-mcp`) uses it to communicate with MCP clients
  such as Claude Code, which then drive the editor through the plugin's local
  HTTP bridge.
- **Is the library statically or dynamically linked?** Neither. It is not
  linked into the plugin binary and is not distributed in the package. It is a
  Node.js script dependency loaded at runtime by an external Node.js process;
  end users fetch it via `npm install` (version pinned by the shipped
  `package-lock.json`).
- **Will this software send data back to the creator?** No. It communicates
  only locally (stdio with the MCP client and localhost HTTP to the editor).
  No diagnostics, telemetry, or usage data.

### 2. zod (MIT license)

- **What does this software do?** JavaScript schema declaration and validation
  library.
- **Why does your plugin need it?** The MCP server uses it to validate the
  input parameters of its MCP tools.
- **Is the library statically or dynamically linked?** Neither — same as
  above: runtime Node.js dependency, fetched by the end user via
  `npm install`, not distributed in the package, not linked into the plugin
  binary.
- **Will this software send data back to the creator?** No. Purely local
  computation; no network access at all.

### 3. ws (MIT license)

- **What does this software do?** WebSocket client/server implementation for
  Node.js.
- **Why does your plugin need it?** The chat sidecar daemon
  (`Source/ThirdParty/ngg-sidecar`) uses it to host a WebSocket server on
  localhost that the in-editor chat window connects to.
- **Is the library statically or dynamically linked?** Neither — runtime
  Node.js dependency, fetched by the end user via `npm install`, not
  distributed in the package, not linked into the plugin binary.
- **Will this software send data back to the creator?** No. It only serves
  connections on localhost; no diagnostics, telemetry, or usage data.

### 4. youtube-transcript (MIT license)

- **What does this software do?** Fetches the public subtitles/transcript of a
  YouTube video.
- **Why does your plugin need it?** Powers the optional
  `youtube_get_transcript` MCP tool, letting the user pull a tutorial video's
  transcript into their AI session as reference material.
- **Is the library statically or dynamically linked?** Neither — runtime
  Node.js dependency, fetched by the end user via `npm install`, not
  distributed in the package, not linked into the plugin binary.
- **Will this software send data back to the creator?** It sends nothing to
  the library's author. When (and only when) the user invokes the tool, it
  requests the transcript from YouTube's public endpoints. No diagnostics,
  telemetry, or usage data.

### 5. Meshy.ai (external web service — no code distributed)

- **What does this software do?** Meshy.ai is a third-party AI 3D-model
  generation web API. No Meshy code ships with the plugin; our own file
  `Source/ThirdParty/unrealngg-mcp/meshy.js` is a small HTTPS client for its
  public REST API.
- **Why does your plugin need it?** Powers the optional `ue5_meshy_generate`
  MCP tool: text-to-3D mesh generation imported into the editor. The feature
  is inert unless the user supplies their own Meshy API key.
- **Is the library statically or dynamically linked?** Neither — it is a
  remote web API accessed over HTTPS at runtime; nothing is linked or
  distributed.
- **Will this software send data back to the creator?** Only when the user
  explicitly invokes the tool with their own API key: the text prompt is sent
  to Meshy.ai and the generated model is downloaded, under the user's own
  Meshy account/terms. Nothing is sent otherwise, and nothing is ever sent to
  us (the publisher).

### Also relevant

- `Content/Python/bp-extract` (first-party) is pure Python standard library —
  no third-party code, hence no `Content/Python/Lib/site-packages/` folder.
- `THIRD_PARTY_NOTICES.md` is not shipped in the package (Fab flags
  license-related files); it remains in the source repo for attribution of
  the npm ecosystem the tools pull in at install time.

## Example project

Fab wants an example project that *depends on* the plugin but does not
*contain* it. To produce one: copy this repo, delete `Plugins/UnrealNGGMCP/`,
`Dist/`, `Scripts/`, and the usual `Binaries/Intermediate/Saved/DerivedDataCache`
folders, zip, and host the download yourself (e.g. a GitHub release). Do not
include the plugin folder in that zip.

## What was fixed per TRC item

| TRC check | Fix |
|---|---|
| Technical Information fields | Block above — paste into listing |
| .uproject has unused plugins disabled | Removed `PCG` (now auto-enabled as a `.uplugin` dependency) and `ModelingToolsEditorMode` from `UnrealNGGMCPExample.uproject`; kept StateTree/GameplayStateTree (used by the template's NPC StateTree assets) |
| All executables must be removed | node_modules no longer packaged; packaging script hard-fails on any exe/dll/cmd/bat/etc. |
| .uplugin EngineVersion | Added `"EngineVersion": "5.7.0"` |
| PlatformAllowList per module | Added `"PlatformAllowList": ["Win64", "Mac", "Linux"]` to the UnrealNGGMCP module |
| Copyright notice in all code files | `Copyright 2025-2026 NGG. All Rights Reserved.` added to all 39 shipped JS/MJS/Python files; existing 18 C++/C# headers normalized to the same publisher/year |
| No errors or consequential warnings | Was gated on executables removal — resolved by the packaging fix |
| No unused/local folders, license files removed | node_modules (with its LICENSE files) and `THIRD_PARTY_NOTICES.md` excluded from the package |
| ThirdParty code placement + TPS form | 2026-07-06 re-review failed both again — fixed by moving the Node packages to `Source/ThirdParty/` and declaring the five third-party items on the TPS form (see top of this file) |
| Technical Information identifies dependencies + TP listing option | 2026-07-08 (round 4) — Technical Information block now names every third-party item (name/version/license) and the prerequisites; the product-page option [THIS PRODUCT USES THIRD-PARTY SOFTWARE] must be enabled and verified saved before resubmitting (see top of this file) |
| Python code placement (Content/Python) | 2026-07-08 (round 4) — first-party `bp-extract` moved `Source/ThirdParty/` → `Content/Python/bp-extract/`; `CanContainContent` enabled; filter + all docs updated; no third-party Python exists, so no `Lib/site-packages/` (see top of this file) |
