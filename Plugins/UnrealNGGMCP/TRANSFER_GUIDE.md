# How to move UnrealNGGMCP to another UE5 project

This guide shows exactly what to copy and what to change.
Replace `MyGame` with your actual project name throughout.

---

## What you are copying

| What | From | To |
|---|---|---|
| UE5 plugin (C++) + bundled Node sidecars | `Plugins/UnrealNGGMCP/` | `YourProject/Plugins/UnrealNGGMCP/` |

The Node MCP server and chat sidecar live inside the plugin at
`Plugins/UnrealNGGMCP/Source/ThirdParty/unrealngg-mcp/` and
`Plugins/UnrealNGGMCP/Source/ThirdParty/ngg-sidecar/`, so a single folder copy moves the
whole system. Do not copy any `node_modules/` subfolder — run `npm install`
inside each sidecar folder after the copy.

The Blueprint→C++ port pipeline `Plugins/UnrealNGGMCP/Content/Python/bp-extract/` (Python ≥3.10,
stdlib-only — **no install**) also travels in the same copy. It is project-agnostic: drop an
optional `BlueprintConvert.json` at your project root to pin the mirror token / module, or let it
auto-derive from the editor's `/project_info`. To bootstrap a project's conversion vault, copy
`Content/Python/bp-extract/vault-template/` → `YourProject/BlueprintConvertInfo/`. See the
[bp-extract conversion pipeline](README.md#bp-extract-conversion-pipeline) section in the README.

The ADL module (`UnrealNGGMCP_ADL`) is project-specific and only useful
if your project has its own `UAdlExerciseDefinition` data-asset class.
Unless that's the case, **do not copy it**.

---

## Step 1 — Copy the plugin

```
YourProject/
  Plugins/
    UnrealNGGMCP/           ← create this folder
      UnrealNGGMCP.uplugin
      Source/
        UnrealNGGMCP/       ← the generic core (copy as-is)
```

Copy only these paths from `Plugins/UnrealNGGMCP/`:

```
UnrealNGGMCP.uplugin
Source/UnrealNGGMCP/
```

Do **not** copy `Source/UnrealNGGMCP_ADL/` unless your target project provides its own `UAdlExerciseDefinition` class.

---

## Step 2 — Strip the ADL module from the .uplugin

Open `YourProject/Plugins/UnrealNGGMCP/UnrealNGGMCP.uplugin` and make sure `Modules` contains only the one generic module:

```json
{
    "FileVersion": 3,
    "Version": 1,
    "VersionName": "1.0",
    "FriendlyName": "UnrealNGGMCP",
    "Description": "Embedded HTTP bridge for Claude Code / MCP clients.",
    "Category": "NGG",
    "CreatedBy": "NGG",
    "CanContainContent": false,
    "Installed": false,
    "Modules": [
        {
            "Name": "UnrealNGGMCP",
            "Type": "Editor",
            "LoadingPhase": "PostEngineInit"
        }
    ]
}
```

Remove the second `UnrealNGGMCP_ADL` entry if it is present.

---

## Step 3 — Enable the plugin in your .uproject

Open `YourProject/YourProject.uproject` and add to the `Plugins` array:

```json
{
    "Name": "UnrealNGGMCP",
    "Enabled": true,
    "EditorOnly": true
}
```

Full example:

```json
{
    "FileVersion": 3,
    "EngineAssociation": "5.4",
    "Modules": [
        { "Name": "MyGame", "Type": "Runtime", "LoadingPhase": "Default" }
    ],
    "Plugins": [
        { "Name": "EnhancedInput", "Enabled": true },
        { "Name": "UnrealNGGMCP",  "Enabled": true, "EditorOnly": true }
    ]
}
```

---

## Step 4 — Regenerate project files

Right-click `YourProject.uproject` in Explorer → **Generate Visual Studio project files**.

Or from the command line:

```bat
"C:\Program Files\Epic Games\UE_5.4\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe" ^
  -projectfiles ^
  -project="C:\YourProject\YourProject.uproject" ^
  -game -engine
```

---

## Step 5 — Build

Open the generated `.sln` in Visual Studio, select the **Development Editor** configuration, and build `YourProjectEditor`.

The `UnrealNGGMCP` module has zero dependencies on any game module — it compiles against standard UE5 engine modules only.

Expected output at the end of compilation:

```
Compiling UnrealNGGMCP...
Linking UnrealEditor-UnrealNGGMCP.dll...
```

---

## Step 6 — Install MCP server + sidecar dependencies

The MCP server and chat sidecar were copied along with the plugin in Step 1
(they live inside the plugin folder). Their Node dependencies install
themselves: the editor plugin runs `npm install` for both packages at editor
startup (progress toast in the editor), and `bootstrap.js` — the `.mcp.json`
entry point in Step 7 — does the same the first time Claude Code spawns the
MCP server from a terminal. Node.js 18+ just needs to be installed.

Manual fallback (e.g. no network at first run):

```bash
cd YourProject/Plugins/UnrealNGGMCP/Source/ThirdParty/unrealngg-mcp
npm install

cd YourProject/Plugins/UnrealNGGMCP/Source/ThirdParty/ngg-sidecar
npm install
```

Do **not** check `node_modules/` into source control — `.gitignore` should
exclude both.

---

## Step 7 — Create .mcp.json at the project root

Create `YourProject/.mcp.json`:

```json
{
  "mcpServers": {
    "ue5-ngg": {
      "command": "node",
      "args": ["Plugins/UnrealNGGMCP/Source/ThirdParty/unrealngg-mcp/bootstrap.js"],
      "cwd": "C:\\YourProject",
      "env": {
        "NGG_BRIDGE_URL": "http://localhost:6776",
        "NGG_BRIDGE_TIMEOUT_MS": "15000"
      }
    }
  }
}
```

- `cwd` must be the **absolute path** to your project root (the folder containing `.uproject`).
- `"command": "node"` resolves via the system `PATH` on every OS, so it works
  regardless of where Node.js is installed. Only hardcode an absolute path to
  the `node` binary if `node` is not on the `PATH` of the shell that runs `claude`.

---

## Step 8 — (Optional) Configure port and auth token

Add this section to `YourProject/Config/DefaultEngine.ini`:

```ini
[UnrealNGGMCP]
; Port the bridge listens on. Default: 6776. Change if another process uses it.
Port=6776
; Uncomment and set a token to require Authorization: Bearer <token> on all requests.
; AuthToken=your-secret-token
```

If you change the port, also update `NGG_BRIDGE_URL` in `.mcp.json`.

---

## Step 9 — Verify

1. Open `YourProject` in the UE5 Editor.
2. Look for this line in the Output Log (Window → Output Log):

   ```
   LogNGGBridge: UnrealNGGMCP: HTTP bridge listening on http://localhost:6776
   ```

3. Open Claude Code in the project folder and run:

   ```
   ue5_health_check
   ```

   Expected:

   ```json
   { "status": "ok", "project": "YourProject", "port": 6776 }
   ```

---

## What you get without the ADL module

All generic editor operations work out of the box:

- Create, read, update, delete any UAsset
- Create Blueprint classes and Widget Blueprints
- Spawn, move, and delete actors in the current level
- Set World Settings, CDO properties, component defaults
- Create materials and levels
- List Gameplay Tags
- Batch multiple operations in one HTTP call

What you **don't** get without `UnrealNGGMCP_ADL`:

- `/adl/exercise/*` endpoints
- `ue5_create_exercise`, `ue5_add_phase_to_exercise`, `ue5_add_step_to_exercise`, `ue5_get_exercise` MCP tools

---

## Adding your own project-specific endpoints

If you have project-specific UObject types (like an `UAdlExerciseDefinition` data-asset), create a second module inside the plugin that depends on `UnrealNGGMCP` and registers routes via `FNGGHttpServer::RegisterDynamicRoute()`. Use `Source/UnrealNGGMCP_ADL/` as a template — it demonstrates the exact pattern.

See `README.md` → **Adding custom routes (extension API)** for a code example.

---

## File checklist

```
YourProject/
├── .mcp.json                              ← created in Step 7
├── Config/
│   └── DefaultEngine.ini                 ← [UnrealNGGMCP] section added in Step 8
├── Plugins/
│   └── UnrealNGGMCP/
│       ├── UnrealNGGMCP.uplugin          ← ADL module entry removed in Step 2
│       └── Source/
│           ├── UnrealNGGMCP/             ← copied in Step 1
│           └── ThirdParty/
│               ├── unrealngg-mcp/        ← npm install in Step 6
│               ├── ngg-sidecar/          ← npm install in Step 6
│               └── bp-extract/           ← Python ≥3.10, stdlib (no install); + vault-template/ seed
├── BlueprintConvert.json                 ← optional: pin mirror token / module for bp-extract
└── YourProject.uproject                  ← plugin entry added in Step 3
```
