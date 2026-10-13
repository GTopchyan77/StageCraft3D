# Evaluation & Testing Guide (for reviewers and new users)

This guide walks through testing **every feature of UnrealNGGMCP end-to-end**.
Most of the product — the in-editor HTTP bridge and all 171 MCP tools — requires
**no account, sign-up, license, or API key**. Only two optional features need
credentials, covered in Part B with exact steps and where the test keys are
provided.

| Feature | Account / key needed? | How to test |
|---|---|---|
| UE5 editor HTTP bridge (core) | **None** | [A1](#a1--test-the-http-bridge-no-key) |
| All 171 MCP tools | **None** (free MCP Inspector) | [A2](#a2--test-all-mcp-tools-with-mcp-inspector-no-key) |
| MCP server unit tests | **None** | [A3](#a3--run-the-automated-test-suite-no-key) |
| In-editor Chat window | Anthropic credentials | [B1](#b1--test-the-in-editor-chat-window) |
| `ue5_meshy_generate` (optional) | Meshy.ai API key | [B2](#b2--test-ue5_meshy_generate-optional) |

**Prerequisites for everything below:** Unreal Engine 5.4+, Node.js 18+, and a
UE5 project with the plugin enabled (see the README's
[Quick Start](../README.md#quick-start)). On first editor launch the plugin
auto-installs the Node dependencies of its bundled tools — wait for the
"install finished" toast, or run `npm install` manually in
`Source/ThirdParty/unrealngg-mcp` and `Source/ThirdParty/ngg-sidecar`.

---

## Part A — Full feature test with **no account or API key**

### A1 — Test the HTTP bridge (no key)

1. Open the project in the UE5 Editor. Confirm in the Output Log:

   ```
   LogNGGBridge: UnrealNGGMCP: HTTP bridge listening on http://localhost:6776
   ```

2. From any terminal (or a browser for the GET endpoints):

   ```powershell
   # Health check
   curl.exe "http://localhost:6776/health"
   # → {"status":"ok","project":"<YourProject>","port":6776}

   # List assets under /Game
   curl.exe "http://localhost:6776/assets/list?path=/Game"

   # List actors in the open level
   curl.exe "http://localhost:6776/editor/list_actors"

   # Create a Blueprint (mutation example) — appears in the Content Browser
   curl.exe -X POST "http://localhost:6776/editor/create_blueprint" ^
     -H "Content-Type: application/json" ^
     -d "{\"parent_class\":\"AActor\",\"asset_path\":\"/Game/BP_ReviewTest\"}"
   ```

Every feature of the editor plugin itself is exposed this way — the full
endpoint list is in the README's [HTTP API reference](../README.md#http-api-reference).
No credentials are involved at any point; the server listens on localhost only.

### A2 — Test all MCP tools with MCP Inspector (no key)

The bundled MCP server does not require Claude or any AI account — it works
with **any** MCP client. The easiest key-free client is Anthropic's open-source
**MCP Inspector** (a local web UI, fetched by `npx`, no sign-up):

1. Keep the UE5 Editor open (bridge running, step A1).
2. From the **project root** run:

   ```powershell
   npx @modelcontextprotocol/inspector node Plugins/UnrealNGGMCP/Source/ThirdParty/unrealngg-mcp/bootstrap.js
   ```

3. A browser tab opens. Click **Connect**, then **List Tools** — all 171
   `ue5_*` / `pcg_*` tools appear with their argument schemas.
4. Exercise any tool against the live editor, e.g.:
   - `ue5_health_check` (no args) → `{"status":"ok", …}`
   - `ue5_list_assets` with `{"path": "/Game"}`
   - `ue5_create_material` with `{"asset_path": "/Game/M_ReviewTest", "base_color": {"r": 1, "g": 0, "b": 0}}` — the material appears in the Content Browser
   - `ue5_bp_read_graph` on any Blueprint in the project

This exercises the complete product chain (MCP client → Node MCP server →
HTTP bridge → UE5 editor) with zero credentials.

### A3 — Run the automated test suite (no key)

```powershell
cd Plugins/UnrealNGGMCP/Source/ThirdParty/unrealngg-mcp
npm test          # 124 unit tests against a mock bridge — no editor, no keys, ~1s
npm run test:live # 26 live tests against the real editor (editor must be open)
```

---

## Part B — Features that require credentials

### B1 — Test the in-editor Chat window

The Chat tab (**Window → Developer Tools → Claude Chat**) runs a real AI
conversation, so it needs Anthropic credentials — either of:

- **Claude Code sign-in (recommended):** install the CLI once
  (`npm install -g @anthropic-ai/claude-code`), run `claude` in any terminal
  and complete `/login`. The chat sidecar automatically reuses the CLI's
  stored credentials — nothing else to configure.
- **API key:** set the environment variable `ANTHROPIC_API_KEY`
  (System → Environment Variables, or in the shell that launches the editor),
  then restart the editor. Keys are created at <https://console.anthropic.com>.

> **For Fab reviewers:** a working test API key is supplied in the
> **review notes of the Fab submission** — set it as `ANTHROPIC_API_KEY` per
> above. Please do not redistribute it; it is spend-limited and rotated after
> review.

Then, in the editor:

1. Open **Window → Developer Tools → Claude Chat**.
2. Type a prompt, e.g. *"Create a Blueprint named BP_ChatTest derived from
   Actor and compile it."* Approve the tool-permission prompts that appear.
3. Watch the asset appear in the Content Browser. Try `/mcp`, `@`-mentions,
   image paste, and the model/effort selectors described in
   [ChatWindow.md](ChatWindow.md).

### B2 — Test `ue5_meshy_generate` (optional)

This single optional tool calls the external Meshy.ai text-to-3D API. It is
**inert without a key**: invoking it with no key returns a clear error telling
you where to put one — that error path is itself the expected key-free
behavior. To test generation:

1. Get a key at <https://www.meshy.ai> (a free tier with trial credits exists),
   **or use the Meshy test key supplied in the Fab review notes**.
2. Provide it either way:
   - env var `MESHY_API_KEY`, or
   - a file `.meshy.json` at the project root: `{"api_key": "msy-..."}`
3. In MCP Inspector (A2) or any MCP client, call `ue5_meshy_generate` with
   `{"prompt": "a small wooden barrel", "asset_name": "SM_Barrel"}`. The
   generated mesh is imported into `/Game/Generated` when Meshy finishes
   (typically 1–3 minutes).

---

## Troubleshooting during evaluation

- **Bridge not reachable** → the editor must be open with the plugin enabled;
  see the README's [Troubleshooting](../README.md#troubleshooting).
- **Chat window says "no Anthropic credentials"** → complete B1's credential
  step and restart the editor.
- **Inspector shows no tools** → run the `npx` command from the project root
  (it must find the `.uproject`), and confirm `node --version` is ≥ 18.
