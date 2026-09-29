#!/usr/bin/env node
// Copyright 2025-2026 NGG. All Rights Reserved.
// index.js — unrealngg-mcp
// MCP server that bridges Claude Code to the UE5 UnrealNGGMCP plugin.
// Run via: node index.js  (stdio transport — Claude Code spawns this as a child process)

import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { z } from "zod";
import * as ue5 from "./ue5client.js";
import * as pcg from "./pcg.js";
import * as gamedev from "./gamedev.js";
import * as sequencer from "./sequencer.js";
import * as worldpartition from "./worldpartition.js";
import * as metasound from "./metasound.js";
import * as rigging from "./rigging.js";
import * as rendering from "./rendering.js";
import * as assetconfig from "./assetconfig.js";
import * as interchange from "./interchange.js";
import * as substrate from "./substrate.js";
import * as landscape from "./landscape.js";
import * as profile from "./profile.js";
import { resolveApiKey as meshyResolveApiKey, generateTextTo3D as meshyGenerate } from "./meshy.js";
import { YoutubeTranscript } from "youtube-transcript";
import { execSync, spawn, spawnSync } from "child_process";
import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";
import { jsonPreprocess } from "./helpers.js";
import { installClaudeAssets } from "./setup-assets.js";
import { splitRules } from "./rules.js";
import { resolveToolsets, toolsetFor, ALL_TOOLSETS } from "./toolsets.js";
import {
  findUProject,
  readEngineAssociation,
  discoverEngineDir,
  discoverBuildTargets,
  projectNameFrom,
} from "./paths.js";

// ---------------------------------------------------------------------------
// Lazy project + engine resolution (Windows-only)
//
// Project info is resolved per-call instead of at startup, so a single sidecar
// process correctly tracks whichever UE editor is currently running on the
// bridge port. Resolution priority:
//   1. The bridge's GET /project_info — ground truth from the running editor.
//   2. Fallback: local resolution from cwd (.uproject discovery + engine
//      registry / well-known install paths). Used when the bridge is down,
//      e.g. for ue5_launch_editor or ue5_build immediately after kill.
//
// Cached for 5s so back-to-back tool calls don't hammer the bridge.
// ---------------------------------------------------------------------------

const PROJECT_CACHE_MS = 5000;
let cachedProject = null;
let cachedAt = 0;
// Track which side last produced the cache so a bridge-state change
// (editor connect/disconnect) invalidates a now-stale entry instead of
// being masked for up to PROJECT_CACHE_MS. A cached "local" result means the
// bridge was DOWN when it was produced; a cached "bridge" result means it was
// UP. If the live bridge state differs from what the cache assumes, the cache
// must be discarded immediately.
let cachedBridgeUp = false;

// Exposed so an explicit bridge-state signal (e.g. a health check that flips
// reachability) can drop a stale project cache without waiting for the TTL.
function invalidateProjectCache() {
  cachedProject = null;
  cachedAt = 0;
}

async function resolveProject() {
  const now = Date.now();
  const cacheFresh = cachedProject && (now - cachedAt) < PROJECT_CACHE_MS;

  // Always probe the bridge — it's the only way to detect a connect/disconnect
  // transition. The probe is cheap (a single GET with a short timeout) and its
  // result is the authoritative project info when the editor is up.
  try {
    const info = await ue5.getProjectInfo();
    // Bridge is UP. If the cache was built while the bridge was UP and is still
    // fresh, reuse it (avoids re-marshalling the same paths back-to-back). A
    // cache built while the bridge was DOWN is stale now → rebuild.
    if (cacheFresh && cachedBridgeUp && cachedProject?.source === "bridge") {
      return cachedProject;
    }
    cachedBridgeUp = true;
    cachedProject = {
      uproject_path: info.uproject_path,
      project_name:  info.project_name,
      engine_dir:    info.engine_dir,
      editor_target: info.editor_target,
      game_target:   info.game_target,
      server_target: info.server_target,
      client_target: info.client_target,
      editor_exe:    path.join(info.engine_dir, "Engine", "Binaries", "Win64", "UnrealEditor.exe"),
      build_bat:     path.join(info.engine_dir, "Engine", "Build", "BatchFiles", "Build.bat"),
      source:        "bridge",
    };
    cachedAt = now;
    return cachedProject;
  } catch (_bridgeErr) {
    // Bridge is DOWN. Reuse a fresh local cache only if it too was produced
    // while the bridge was down — otherwise a just-disconnected editor would
    // keep serving stale bridge paths.
    if (cacheFresh && !cachedBridgeUp && cachedProject?.source === "local") {
      return cachedProject;
    }
    cachedBridgeUp = false;
    // Bridge unreachable — fall back to local resolution from cwd.
    const uproject = process.env.NGG_UE_PROJECT_PATH ?? findUProject();
    if (!uproject) {
      throw new Error(
        "No .uproject found and the UE bridge is unreachable. " +
        "Either start the Unreal Editor (the bridge will then serve project info), " +
        "or run the MCP server from inside a UE project, " +
        "or set NGG_UE_PROJECT_PATH to the .uproject path."
      );
    }
    const assoc     = readEngineAssociation(uproject);
    const engineDir = discoverEngineDir(assoc);
    const targets   = discoverBuildTargets(uproject);
    cachedProject = {
      uproject_path: uproject,
      project_name:  projectNameFrom(uproject),
      engine_dir:    engineDir,
      editor_target: targets.editor,
      game_target:   targets.game,
      server_target: targets.server,
      client_target: targets.client,
      editor_exe:    path.join(engineDir, "Engine", "Binaries", "Win64", "UnrealEditor.exe"),
      build_bat:     path.join(engineDir, "Engine", "Build", "BatchFiles", "Build.bat"),
      source:        "local",
    };
    cachedAt = now;
    return cachedProject;
  }
}

// Transport mode: stdio (default, used by Claude Code) or ws (WebSocket, for persistent connections)
// Set NGG_MCP_TRANSPORT=ws and NGG_MCP_WS_PORT=<port> (default 6777) to use WebSocket transport.
const TRANSPORT_MODE = process.env.NGG_MCP_TRANSPORT ?? "stdio";
const WS_PORT_PARSED  = parseInt(process.env.NGG_MCP_WS_PORT ?? "6777", 10);
const WS_PORT         = Number.isNaN(WS_PORT_PARSED) ? 6777 : WS_PORT_PARSED;

// ---------------------------------------------------------------------------
// Server instantiation
// ---------------------------------------------------------------------------

// Instructions are loaded from `UE5_NGG_RULES.md` next to this file and sent
// as the MCP server's `instructions` field on every handshake. Claude Code
// (and any MCP-aware client) injects them into the model's system context on
// connection — so any project that uses this plugin gets the same workflow
// rules automatically, on any PC, without touching per-project CLAUDE.md or
// other developer-owned files. To change the rules, edit the .md and reload
// the MCP client (e.g. `/mcp` in Claude Code).
const __dirname = path.dirname(fileURLToPath(import.meta.url));
const RULES_PATH = path.join(__dirname, "UE5_NGG_RULES.md");

let RULES_RAW;
try {
  RULES_RAW = fs.readFileSync(RULES_PATH, "utf8");
} catch (err) {
  // Fail loud but don't crash — the server still works without instructions.
  console.error(`[ue5-ngg] Failed to load rules from ${RULES_PATH}: ${err.message}`);
  RULES_RAW =
    "This MCP server bridges to an Unreal Engine 5 editor. " +
    "Rules file UE5_NGG_RULES.md was missing — see the plugin source.";
}

// Only the always-applicable rules go into `instructions`; the subsystem
// chapters (GAS, Behavior Trees, profiling, the Blueprint schema reference) are
// registered as resources below and read on demand. If the document's structure
// ever stops matching rules.js, splitRules() hands back the whole file and we
// send it as before — a bloated handshake beats a silently dropped rule.
const RULES = splitRules(RULES_RAW);
const UE5_NGG_INSTRUCTIONS = RULES.core;

console.error(
  `[ue5-ngg] rules: ${RULES_RAW.length} bytes on disk -> ` +
  `${UE5_NGG_INSTRUCTIONS.length} bytes in instructions + ` +
  `${RULES.sections.length} resource(s) ` +
  `(${RULES_RAW.startsWith("This MCP") ? "FALLBACK — rules file missing"
    : RULES.complete ? "OK"
    : `UNSPLIT — headings moved: ${RULES.missing.join("; ")}`})`
);

const server = new McpServer(
  {
    name:    "ue5-ngg",
    version: "1.0.0",
  },
  {
    instructions: UE5_NGG_INSTRUCTIONS,
  }
);

// ---------------------------------------------------------------------------
// Toolset filtering
// ---------------------------------------------------------------------------
// Every tool registration below goes through this wrapper, so a project can
// register only the sets it uses (NGG_TOOLSETS / NGG_TOOLSETS_EXCLUDE).
// Unset => everything, i.e. the previous behaviour.
//
// Note for future edits: index.tools.test.js parses this file as text and
// counts registration call sites, so don't write that call's literal spelling
// in a comment — it inflates the count and fails the suite.
const TOOLSETS = resolveToolsets(process.env);
const skippedToolsets = new Map();   // set -> count, for the startup banner

const serverToolDirect = server.tool.bind(server);
server.tool = function toolWithToolsetFilter(name, ...rest) {
  const set = toolsetFor(name);
  if (!TOOLSETS.enabled.has(set)) {
    skippedToolsets.set(set, (skippedToolsets.get(set) ?? 0) + 1);
    return undefined;
  }
  return serverToolDirect(name, ...rest);
};

if (TOOLSETS.unknown.length > 0) {
  console.error(
    `[ue5-ngg] unknown toolset name(s) ignored: ${TOOLSETS.unknown.join(", ")}. ` +
    `Known sets: ${ALL_TOOLSETS.join(", ")}`
  );
}

// ---------------------------------------------------------------------------
// Rule chapters as MCP resources
// ---------------------------------------------------------------------------
// Registered even when the split failed (sections is then empty), so a client
// never sees a half-populated resource list.
for (const section of RULES.sections) {
  server.registerResource(
    `rules-${section.slug}`,
    section.uri,
    {
      title:       section.title,
      description: section.description,
      mimeType:    "text/markdown",
    },
    async (uri) => ({
      contents: [{ uri: uri.href, mimeType: "text/markdown", text: section.text }],
    })
  );
}

// ---------------------------------------------------------------------------
// First-run install of bundled Claude Code skills + agents
//
// The plugin ships skills/agents under ./assets. On startup we copy them into
// the project's .claude/ directory (the same per-project, ships-with-the-plugin
// model as UE5_NGG_RULES.md). Version-tracked + never clobbers user edits — see
// setup-assets.js. Pure local filesystem work, independent of the editor bridge.
//
// Caveat: Claude Code loads skills/agents at session start, so files installed
// here are picked up on the *next* Claude Code restart, not the current one.
// Set NGG_SKIP_SKILL_INSTALL=1 to disable.
// ---------------------------------------------------------------------------

function installAssetsNow() {
  const uproject   = process.env.NGG_UE_PROJECT_PATH ?? findUProject();
  const projectDir = uproject ? path.dirname(uproject) : null;
  const sourceDir  = path.join(__dirname, "assets");
  const summary    = installClaudeAssets({ sourceDir, projectDir, log: (m) => console.error(m) });
  return { projectDir, sourceDir, summary };
}

function maybeInstallClaudeAssets() {
  if (process.env.NGG_SKIP_SKILL_INSTALL === "1") {
    console.error("[ue5-ngg] skill/agent install skipped (NGG_SKIP_SKILL_INSTALL=1)");
    return;
  }
  try {
    const { projectDir, summary } = installAssetsNow();
    if (!summary.ran) {
      console.error(`[ue5-ngg] skill/agent install: ${summary.reason ?? "did not run"}`);
      return;
    }
    console.error(
      `[ue5-ngg] skills/agents v${summary.version} → ${path.join(projectDir, ".claude")} ` +
      `(installed ${summary.installed}, updated ${summary.updated}, unchanged ${summary.unchanged}, preserved ${summary.skipped})`
    );
    if (summary.installed > 0 || summary.updated > 0) {
      console.error("[ue5-ngg] NOTE: restart Claude Code (or reload the window) to pick up the new/updated skills & agents.");
    }
  } catch (err) {
    console.error(`[ue5-ngg] skill/agent install failed (non-fatal): ${err.message}`);
  }
}

maybeInstallClaudeAssets();

// ---------------------------------------------------------------------------
// Shared Zod schemas for reuse across tools
// ---------------------------------------------------------------------------

const StepSchema = z.object({
  step_id: z
    .string()
    .describe("Unique identifier for this step, e.g. 'Step_01_GrabPlate'"),
  required_interaction_tag: z
    .string()
    .describe("Gameplay Tag that must fire to satisfy this step, e.g. 'Interaction.Grab.Plate'"),
  step_type: z
    .enum(["Grab", "Place", "Toggle", "Motion", "Navigate"])
    .describe("The class of physical action required"),
  time_limit_seconds: z
    .number()
    .min(0)
    .default(0)
    .describe("Per-step time limit in seconds. 0 means unlimited."),
  hint_text_easy: z
    .string()
    .default("")
    .describe("Hint shown at Easy difficulty (Italian default)"),
  hint_text_medium: z
    .string()
    .default("")
    .describe("Hint shown at Medium difficulty"),
  hint_text_hard: z
    .string()
    .default("")
    .describe("Hint shown at Hard difficulty (minimal guidance)"),
});

const PhaseSchema = z.object({
  phase_display_name: z
    .string()
    .describe("Human-readable phase name, e.g. 'Preparation'"),
  steps: z
    .array(StepSchema)
    .min(1)
    .describe("Ordered list of steps in this phase"),
});

// ---------------------------------------------------------------------------
// Tool: ue5_health_check
// ---------------------------------------------------------------------------

server.tool(
  "ue5_health_check",
  `Check whether the UE5 Editor is running and the UnrealNGGMCP plugin is active at ${ue5.getBridgeUrl()}.`,
  {},
  async () => {
    try {
      const result = await ue5.healthCheck();
      // Bridge reachable. If the project cache was last built while the bridge
      // was down, drop it so the next resolveProject() picks up live editor info.
      if (!cachedBridgeUp) invalidateProjectCache();
      const rulesOk = !UE5_NGG_INSTRUCTIONS.startsWith("This MCP server bridges");
      // Surface the token state here rather than letting the first pcg_* call
      // fail with an opaque 403 about exec_python.
      const auth = ue5.getAuthTokenStatus();
      // A plugin/sidecar version skew otherwise shows up as an assortment of
      // odd failures rather than one diagnosable message.
      const protocol = ue5.checkBridgeProtocol(result);
      return {
        content: [{
          type: "text",
          text: JSON.stringify({
            ...result,
            ...(protocol.warning ? { protocol_warning: protocol.warning } : {}),
            rules_ok:    rulesOk,
            rules_bytes: UE5_NGG_INSTRUCTIONS.length,
            rules_lines: UE5_NGG_INSTRUCTIONS.split('\n').length,
            auth_token:  auth.configured
              ? { configured: true, source: auth.source }
              : {
                  configured: false,
                  disabled_features: auth.disables,
                  fix: "Run `npm run set-token` in Plugins/UnrealNGGMCP/Source/ThirdParty/unrealngg-mcp/, then restart the editor. It writes the token to Config/UserEngine.ini (gitignored).",
                },
          }, null, 2),
        }],
      };
    } catch (err) {
      // Bridge unreachable. If the project cache was last built while the bridge
      // was up, drop it so we don't keep serving stale editor paths.
      if (cachedBridgeUp) invalidateProjectCache();
      const bridgeUrl = ue5.getBridgeUrl();
      return {
        content: [
          {
            type: "text",
            text: `Bridge not reachable: ${err.message}\n\nTo fix:\n1. Open the Unreal Editor with your project\n2. Ensure the UnrealNGGMCP plugin is enabled in Edit → Plugins\n3. Check the Output Log for 'UnrealNGGMCP: HTTP bridge listening on ${bridgeUrl}'`,
          },
        ],
        isError: true,
      };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_setup_skills
// ---------------------------------------------------------------------------

server.tool(
  "ue5_setup_skills",
  [
    "Install (or refresh) the ue5-ngg Claude Code skills and agents bundled with",
    "this MCP server into the project's .claude/ directory. Runs automatically on",
    "server startup; call this to force a refresh after a plugin update. Files you",
    "have edited yourself are preserved. Newly installed/updated skills & agents are",
    "picked up after the next Claude Code restart (skills load at session start).",
  ].join(" "),
  {},
  async () => {
    try {
      const { projectDir, summary } = installAssetsNow();
      if (!summary.ran) {
        return {
          content: [{ type: "text", text: `Skill/agent install did not run: ${summary.reason ?? "unknown"}` }],
          isError: true,
        };
      }
      const text = [
        `Installed ue5-ngg skills/agents v${summary.version} into ${path.join(projectDir, ".claude")}`,
        `  installed:                 ${summary.installed}`,
        `  updated:                   ${summary.updated}`,
        `  unchanged:                 ${summary.unchanged}`,
        `  preserved (user-modified): ${summary.skipped}`,
        "",
        summary.installed > 0 || summary.updated > 0
          ? "Restart Claude Code (or reload the window) to pick up the changes."
          : "Everything was already up to date.",
      ].join("\n");
      return { content: [{ type: "text", text }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_list_assets
// ---------------------------------------------------------------------------

server.tool(
  "ue5_list_assets",
  "List all assets at a given Content Browser path in the UE5 project.",
  {
    path: z
      .string()
      .default("/Game/Data")
      .describe("Content browser path to list, e.g. '/Game/Data/Exercises'"),
  },
  async ({ path }) => {
    try {
      const result = await ue5.listAssets(path);
      return {
        content: [{ type: "text", text: JSON.stringify(result, null, 2) }],
      };
    } catch (err) {
      return {
        content: [{ type: "text", text: err.message }],
        isError: true,
      };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_get_asset
// ---------------------------------------------------------------------------

server.tool(
  "ue5_get_asset",
  "Get all UPROPERTY fields of any UE5 asset as structured JSON, by its full content path.",
  {
    asset_path: z
      .string()
      .describe("Full content path, e.g. '/Game/Data/Exercises/DA_ADL01_JamSandwich'"),
  },
  async ({ asset_path }) => {
    try {
      const result = await ue5.getAsset(asset_path);
      return {
        content: [{ type: "text", text: JSON.stringify(result, null, 2) }],
      };
    } catch (err) {
      return {
        content: [{ type: "text", text: err.message }],
        isError: true,
      };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_get_exercise
// ---------------------------------------------------------------------------

server.tool(
  "ue5_get_exercise",
  "Get the full definition of an AdlExerciseDefinition Data Asset, looked up by ExerciseID.",
  {
    exercise_id: z
      .string()
      .describe("The ExerciseID FName value, e.g. 'ADL_01_JamSandwich'"),
  },
  async ({ exercise_id }) => {
    try {
      const result = await ue5.getExercise(exercise_id);
      return {
        content: [{ type: "text", text: JSON.stringify(result, null, 2) }],
      };
    } catch (err) {
      return {
        content: [{ type: "text", text: err.message }],
        isError: true,
      };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_create_exercise
// ---------------------------------------------------------------------------

server.tool(
  "ue5_create_exercise",
  [
    "Create a new AdlExerciseDefinition Data Asset in the UE5 editor with all its phases and steps.",
    "The asset is created at /Game/Data/Exercises/DA_<exercise_id> by default unless asset_path is specified.",
    "This is idempotent — if the asset already exists it will be updated in place.",
    "After creation, call ue5_save_all to persist the asset to disk.",
  ].join(" "),
  {
    exercise_id: z
      .string()
      .regex(/^[A-Za-z0-9_]+$/, "Must be alphanumeric with underscores, e.g. 'ADL_01_JamSandwich'")
      .describe("Unique exercise identifier — becomes the ExerciseID FName on the asset"),
    display_name: z
      .string()
      .describe("Human-readable exercise title shown in the therapist UI (Italian default)"),
    asset_path: z
      .string()
      .optional()
      .describe(
        "Override the content path. Defaults to '/Game/Data/Exercises/DA_<exercise_id>'"
      ),
    environment_level: z
      .string()
      .default("")
      .describe("Soft reference to the streaming Level Instance, e.g. '/Game/Environments/Kitchen/L_Env_Kitchen'"),
    default_difficulty: z
      .enum(["Easy", "Medium", "Hard"])
      .default("Medium")
      .describe("Default difficulty setting for this exercise"),
    phases: z
      .array(PhaseSchema)
      .min(1)
      .describe("Ordered list of ADL phases that make up this exercise"),
  },
  async ({ exercise_id, display_name, asset_path, environment_level, default_difficulty, phases }) => {
    const resolvedPath =
      asset_path ?? `/Game/Data/Exercises/DA_${exercise_id}`;

    try {
      const result = await ue5.createExerciseFull({
        exercise_id,
        display_name,
        asset_path:         resolvedPath,
        environment_level:  environment_level ?? "",
        default_difficulty: default_difficulty ?? "Medium",
        phases,
      });

      return {
        content: [
          {
            type: "text",
            text: [
              result.created
                ? `Created new exercise asset at: ${result.asset_path}`
                : `Updated existing exercise asset at: ${result.asset_path}`,
              "",
              "Next step: call ue5_save_all to persist to disk.",
              "",
              JSON.stringify(result, null, 2),
            ].join("\n"),
          },
        ],
      };
    } catch (err) {
      return {
        content: [{ type: "text", text: err.message }],
        isError: true,
      };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_add_phase_to_exercise
// ---------------------------------------------------------------------------

server.tool(
  "ue5_add_phase_to_exercise",
  "Append a new phase (with its steps) to an existing AdlExerciseDefinition asset.",
  {
    asset_path: z
      .string()
      .describe("Full content path of the exercise asset"),
    phase: PhaseSchema,
  },
  async ({ asset_path, phase }) => {
    try {
      const result = await ue5.addPhaseToExercise(asset_path, phase);
      return {
        content: [
          {
            type: "text",
            text: `Phase appended at index ${result.phase_index}.\n\n${JSON.stringify(result, null, 2)}`,
          },
        ],
      };
    } catch (err) {
      return {
        content: [{ type: "text", text: err.message }],
        isError: true,
      };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_add_step_to_exercise
// ---------------------------------------------------------------------------

server.tool(
  "ue5_add_step_to_exercise",
  "Append a single step to a specific phase of an existing AdlExerciseDefinition asset.",
  {
    asset_path: z
      .string()
      .describe("Full content path of the exercise asset"),
    phase_index: z
      .number()
      .int()
      .min(0)
      .describe("Zero-based index of the phase to append to"),
    step: StepSchema,
  },
  async ({ asset_path, phase_index, step }) => {
    try {
      const result = await ue5.addStepToExercise(asset_path, phase_index, step);
      return {
        content: [
          {
            type: "text",
            text: `Step appended at phase[${result.phase_index}].steps[${result.step_index}].\n\n${JSON.stringify(result, null, 2)}`,
          },
        ],
      };
    } catch (err) {
      return {
        content: [{ type: "text", text: err.message }],
        isError: true,
      };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_set_asset_property
// ---------------------------------------------------------------------------

server.tool(
  "ue5_set_asset_property",
  [
    "Set a single UPROPERTY on any UE5 asset by its content path and property name.",
    "Use this for simple scalar overrides (e.g. fixing a display name or difficulty).",
    "For full exercise creation use ue5_create_exercise instead.",
  ].join(" "),
  {
    asset_path: z
      .string()
      .describe("Full content path of the target asset"),
    property_name: z
      .string()
      .describe("Exact UPROPERTY name as declared in C++ (case-sensitive)"),
    value: z
      .union([z.string(), z.number(), z.boolean()])
      .describe("New value — strings are imported via UE5's ImportText, numbers/booleans are stringified first"),
  },
  async ({ asset_path, property_name, value }) => {
    try {
      const result = await ue5.setAssetProperty(asset_path, property_name, value);
      return {
        content: [{ type: "text", text: JSON.stringify(result, null, 2) }],
      };
    } catch (err) {
      return {
        content: [{ type: "text", text: err.message }],
        isError: true,
      };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_create_data_asset
// ---------------------------------------------------------------------------

server.tool(
  "ue5_create_data_asset",
  [
    "Create a UDataAsset subclass instance in the UE5 Editor.",
    "Optionally set initial UPROPERTY values on the newly created asset.",
    "Property values are set via UE5's ImportText, so use UE5 text format for complex types",
    "(e.g. TMap: '((Key=EnumValue,Value=BlueprintPath),(...))' ).",
    "Call ue5_save_all afterwards to persist to disk.",
  ].join(" "),
  {
    asset_path: z
      .string()
      .describe("Destination content path, e.g. '/Game/MyGame/Data/DA_BombRegistry'"),
    class_name: z
      .string()
      .describe("UDataAsset subclass name (with or without U prefix), e.g. 'UBombRegistryDataAsset' or 'BombRegistryDataAsset'"),
    properties: z
      .array(
        z.object({
          name: z.string().describe("UPROPERTY name (case-sensitive)"),
          value: z
            .union([z.string(), z.number(), z.boolean()])
            .describe("Property value — use UE5 ImportText format for complex types"),
        })
      )
      .optional()
      .describe("Optional initial properties to set on the created asset"),
  },
  async ({ asset_path, class_name, properties }) => {
    try {
      const createResult = await ue5.createAsset(class_name, asset_path);

      const lines = [`Data Asset created: ${createResult.created ?? asset_path}`];
      const warnings = [];
      let propsRequested = 0;
      let propsSet = 0;

      // Set initial properties if provided
      if (properties && properties.length > 0) {
        propsRequested = properties.length;
        for (const prop of properties) {
          try {
            await ue5.setAssetProperty(asset_path, prop.name, prop.value);
            lines.push(`  Set ${prop.name} ✓`);
            propsSet++;
          } catch (propErr) {
            warnings.push(`  Failed to set ${prop.name}: ${propErr.message}`);
          }
        }
      }

      // Total failure: properties were requested but none could be set.
      // The asset itself exists, but the caller's intent (initial values) failed.
      const allPropsFailed = propsRequested > 0 && propsSet === 0;

      const text = [
        ...lines,
        ...(warnings.length > 0 ? ["\nWarnings:", ...warnings] : []),
        ...(allPropsFailed
          ? [`\nERROR: all ${propsRequested} property set(s) failed — the asset was created but no initial values were applied.`]
          : []),
        "\nNext step: call ue5_save_all to persist to disk.",
      ].join("\n");

      return {
        content: [
          {
            type: "text",
            text: text + "\n\n" + JSON.stringify(createResult, null, 2),
          },
        ],
        isError: allPropsFailed,
      };
    } catch (err) {
      return {
        content: [{ type: "text", text: err.message }],
        isError: true,
      };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_save_all
// ---------------------------------------------------------------------------

server.tool(
  "ue5_save_all",
  "Save all dirty (unsaved) content packages in the UE5 Editor. Call this after creating or modifying assets.",
  {},
  async () => {
    try {
      const result = await ue5.saveAll();
      return {
        content: [{ type: "text", text: JSON.stringify(result, null, 2) }],
      };
    } catch (err) {
      return {
        content: [{ type: "text", text: err.message }],
        isError: true,
      };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_list_gameplay_tags
// ---------------------------------------------------------------------------

server.tool(
  "ue5_list_gameplay_tags",
  [
    "List all Gameplay Tags registered in the UE5 project.",
    "Use this to discover valid values for FAdlStep.RequiredInteractionTag before authoring exercise steps.",
  ].join(" "),
  {},
  async () => {
    try {
      const result = await ue5.listGameplayTags();
      // Format for readability: one tag per line in the tag list
      const tagLines = (result.tags ?? [])
        .map((t) => `  ${t.tag}`)
        .join("\n");
      return {
        content: [
          {
            type: "text",
            text: `${result.count} registered gameplay tags:\n\n${tagLines}\n\n${JSON.stringify(result, null, 2)}`,
          },
        ],
      };
    } catch (err) {
      return {
        content: [{ type: "text", text: err.message }],
        isError: true,
      };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_create_blueprint
// ---------------------------------------------------------------------------

server.tool(
  "ue5_create_blueprint",
  "Create a Blueprint class in the UE5 Editor as a child of a given C++ or Blueprint parent class.",
  {
    parent_class: z
      .string()
      .describe("C++ class name (e.g. 'AAdlSequencerActor') or Blueprint content path"),
    asset_path: z
      .string()
      .describe("Destination content path, e.g. '/Game/Blueprints/BP_AdlSequencerActor'"),
  },
  async ({ parent_class, asset_path }) => {
    try {
      const result = await ue5.createBlueprint(parent_class, asset_path);
      const statusLine = result.already_existed
        ? `Blueprint already existed at: ${result.asset_path}`
        : `Blueprint created at: ${result.asset_path} (parent: ${result.parent_class})`;
      return {
        content: [
          {
            type: "text",
            text: `${statusLine}\n\nNext step: call ue5_save_all to persist to disk.\n\n${JSON.stringify(result, null, 2)}`,
          },
        ],
      };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_create_widget_blueprint
// ---------------------------------------------------------------------------

const WIDGET_TYPES = [
  "Button", "TextBlock", "RichTextBlock", "ComboBoxString",
  "EditableText", "EditableTextBox", "MultiLineEditableText", "MultiLineEditableTextBox",
  "ProgressBar", "Image", "Slider", "CheckBox", "Spacer",
  "CanvasPanel", "Overlay", "VerticalBox", "HorizontalBox",
  "GridPanel", "UniformGridPanel", "WrapBox", "ScrollBox",
  "Border", "SizeBox", "ScaleBox", "WidgetSwitcher", "NamedSlot",
  "SafeZone", "Throbber", "CircularThrobber", "BackgroundBlur",
  "RetainerBox", "InvalidationBox", "MenuAnchor",
  "UserWidget",
];

const ROOT_PANEL_TYPES = [
  "CanvasPanel", "Overlay", "VerticalBox", "HorizontalBox",
  "GridPanel", "UniformGridPanel", "ScrollBox", "Border",
  "SizeBox", "ScaleBox",
];

server.tool(
  "ue5_create_widget_blueprint",
  [
    "Create a Widget Blueprint in the UE5 Editor with a given parent C++ class and populate its widget tree with named widgets.",
    "Widget names must match the UPROPERTY(meta=(BindWidget)) names declared in the parent C++ class.",
    "Use root_type to pick a non-Canvas root panel.",
    "Use type='UserWidget' + user_widget_class='/Game/UI/WBP_X' to embed an existing widget blueprint as a child.",
  ].join(" "),
  {
    parent_class: z.string().describe("C++ UserWidget subclass name (e.g. 'URehabMainMenuWidget')"),
    asset_path:   z.string().describe("Destination content path, e.g. '/Game/UI/WBP_MainMenu'"),
    root_type:    z.enum(ROOT_PANEL_TYPES).optional()
      .describe("Root panel widget type (default: CanvasPanel)"),
    widgets: z.array(z.object({
      type:              z.enum(WIDGET_TYPES).describe("UMG widget type"),
      name:              z.string().describe("FName — match BindWidget property names in the C++ parent"),
      parent:            z.string().optional().describe("FName of an already-listed parent widget (panel). If omitted, widget goes on the root."),
      user_widget_class: z.string().optional().describe("Required when type='UserWidget' — content path to a Widget Blueprint, e.g. '/Game/UI/WBP_Card'"),
    })).describe("Widgets to create in the widget tree"),
  },
  async ({ parent_class, asset_path, widgets, root_type }) => {
    try {
      const result = await ue5.createWidgetBlueprint(parent_class, asset_path, widgets, root_type);
      const statusLine = result.already_existed
        ? `Widget Blueprint already existed at: ${result.asset_path}`
        : `Widget Blueprint created at: ${result.asset_path} with ${result.widget_count} widget(s)`;
      return {
        content: [
          {
            type: "text",
            text: `${statusLine}\n\nNext step: call ue5_save_all to persist to disk.\n\n${JSON.stringify(result, null, 2)}`,
          },
        ],
      };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_set_blueprint_defaults
// ---------------------------------------------------------------------------

server.tool(
  "ue5_set_blueprint_defaults",
  [
    "Set UPROPERTY default values on a Blueprint's Class Default Object (CDO).",
    "Use this to assign IMC assets, widget classes, mesh references, and other object references that cannot be set via ue5_set_asset_property.",
    "Property values for object references must be full content paths.",
  ].join(" "),
  {
    asset_path: z
      .string()
      .describe("Content path of the Blueprint asset"),
    properties: z
      .array(
        z.object({
          name: z
            .string()
            .describe("UPROPERTY name (case-sensitive, as declared in C++)"),
          value: z
            .string()
            .describe("Value string — for object references use the full content path"),
        })
      )
      .describe("Properties to set on the CDO"),
  },
  async ({ asset_path, properties }) => {
    try {
      const result = await ue5.setBlueprintDefaults(asset_path, properties);
      // Total failure: properties were requested but none were actually set.
      const totalFailure = properties.length > 0 && (result.properties_set ?? 0) === 0;
      return {
        content: [
          {
            type: "text",
            text: [
              `Set ${result.properties_set} of ${properties.length} properties on CDO of ${asset_path}.`,
              totalFailure
                ? `ERROR: none of the ${properties.length} requested properties were set.`
                : null,
              result.warnings?.length > 0
                ? `Warnings:\n${result.warnings.map((w) => `  - ${w}`).join("\n")}`
                : "No warnings.",
              "",
              "Next step: call ue5_save_all to persist to disk.",
              "",
              JSON.stringify(result, null, 2),
            ].filter((l) => l !== null).join("\n"),
          },
        ],
        isError: totalFailure,
      };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_reparent_blueprint
// ---------------------------------------------------------------------------

server.tool(
  "ue5_reparent_blueprint",
  "Change the parent class of a Blueprint or Widget Blueprint. Pass a C++ class name (e.g. 'MyGameHUDWidget') or a Blueprint content path as new_parent. Recompiles and marks the asset dirty — call ue5_save_all afterwards.",
  {
    asset_path: z
      .string()
      .describe("Content path of the Blueprint to reparent, e.g. '/Game/MyGame/UI/WBP_MyGameHUD'"),
    new_parent: z
      .string()
      .describe("New parent class: C++ class name (e.g. 'MyGameHUDWidget') or Blueprint content path (e.g. '/Game/MyParentBP')"),
  },
  async ({ asset_path, new_parent }) => {
    try {
      const result = await ue5.reparentBlueprint(asset_path, new_parent);
      return {
        content: [
          {
            type: "text",
            text: [
              `Reparented ${asset_path}`,
              `  Old parent: ${result.old_parent}`,
              `  New parent: ${result.new_parent}`,
              "",
              "Next step: call ue5_save_all to persist to disk.",
            ].join("\n"),
          },
        ],
      };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_set_world_settings
// ---------------------------------------------------------------------------

server.tool(
  "ue5_set_world_settings",
  [
    "Set the Game Mode, Player Controller, and Default Pawn class on the currently open level's World Settings.",
    "For C++ classes use /Script/ModuleName.ClassName. For Blueprint classes use the content path (with or without _C suffix).",
    "Player Controller and Default Pawn are applied to the Game Mode's CDO, not World Settings directly.",
  ].join(" "),
  {
    game_mode_class: z
      .string()
      .default("")
      .describe("Class path for the Game Mode, e.g. '/Script/MyProject.MyGameMode' or '/Game/Blueprints/BP_MyGameMode'"),
    player_controller_class: z
      .string()
      .default("")
      .describe("Class path for the Player Controller Blueprint, e.g. '/Game/Blueprints/BP_RehabPlayerController'"),
    default_pawn_class: z
      .string()
      .default("")
      .describe("Class path for the Default Pawn Blueprint, e.g. '/Game/Blueprints/BP_VRPlayerPawn'"),
  },
  async ({ game_mode_class, player_controller_class, default_pawn_class }) => {
    try {
      const result = await ue5.setWorldSettings({
        game_mode_class,
        player_controller_class,
        default_pawn_class,
      });
      return {
        content: [
          {
            type: "text",
            text: [
              `World settings applied to level: ${result.world}`,
              result.game_mode          ? `  Game Mode:          ${result.game_mode}`          : "",
              result.player_controller  ? `  Player Controller:  ${result.player_controller}`  : "",
              result.default_pawn       ? `  Default Pawn:       ${result.default_pawn}`       : "",
              "",
              "Next step: call ue5_save_all to persist to disk.",
              "",
              JSON.stringify(result, null, 2),
            ]
              .filter((l) => l !== "")
              .join("\n"),
          },
        ],
      };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_spawn_actor
// ---------------------------------------------------------------------------

server.tool(
  "ue5_spawn_actor",
  [
    "Spawn an actor in the currently open UE5 editor level at a given world-space location.",
    "Use this to place ATeleportAnchor actors in environments, or any other actor class.",
    "For Blueprint actors provide the full content path (e.g. '/Game/Blueprints/BP_TeleportAnchor').",
    "Location is in centimetres (UE5 default unit). The spawned actor is selected in the Outliner.",
    "Returns a 409 error if an actor with the same label already exists — check before spawning.",
    "For StaticMeshActor use the static_mesh param to assign the mesh in the same call (e.g. '/Engine/BasicShapes/Cube').",
  ].join(" "),
  {
    actor_class: z
      .string()
      .optional()
      .describe(
        "C++ class name (e.g. 'AStaticMeshActor') or Blueprint content path. Use this OR blueprint_path."
      ),
    blueprint_path: z
      .string()
      .optional()
      .describe(
        "Alias for actor_class — Blueprint content path (e.g. '/Game/Blueprints/Grid/BP_GridManager'). Use this OR actor_class."
      ),
    location: z.preprocess(jsonPreprocess,
      z.object({ x: z.number(), y: z.number(), z: z.number() })
      .describe("World-space location in centimetres")),
    rotation: z.preprocess(jsonPreprocess,
      z.object({ pitch: z.number(), yaw: z.number(), roll: z.number() })
      .optional()
      .describe("Rotation in degrees (defaults to zero rotation if omitted)")),
    label: z
      .string()
      .optional()
      .describe("Actor label shown in the Outliner — must be unique in the level"),
    static_mesh: z
      .string()
      .optional()
      .describe("Content path of the UStaticMesh to assign immediately after spawn (e.g. '/Engine/BasicShapes/Cube')"),
    properties: z.preprocess(jsonPreprocess,
      z.array(z.object({
        name: z.string().describe("Property name"),
        value: z.string().describe("Property value"),
      }))
      .optional()
      .describe("Properties to set on the actor after spawning (calls update_actor internally)")),
  },
  async ({ actor_class, blueprint_path, location, rotation, label, static_mesh, properties }) => {
    const resolvedClass = actor_class || blueprint_path;
    if (!resolvedClass) {
      return { content: [{ type: "text", text: "Either actor_class or blueprint_path is required" }], isError: true };
    }
    try {
      const result = await ue5.spawnActorInLevel(resolvedClass, location, rotation, label, static_mesh);

      // If properties were provided, set them via update_actor (component_name="" targets the actor itself)
      let propsResult = null;
      if (properties && properties.length > 0 && result.actor_name) {
        const componentProperties = properties.map(p => ({
          component_name: "",
          property_name: p.name,
          value: p.value,
        }));
        propsResult = await ue5.updateActor(result.actor_name, { componentProperties });
      }

      const lines = [
        `Spawned '${result.actor_name}' (class: ${result.class})`,
        `  Location: x=${result.location.x}, y=${result.location.y}, z=${result.location.z}`,
        `  Rotation: pitch=${result.rotation.pitch}, yaw=${result.rotation.yaw}, roll=${result.rotation.roll}`,
      ];
      if (propsResult) {
        lines.push(`  Properties set: ${properties.map(p => p.name).join(", ")}`);
      }
      lines.push("", "Next step: call ue5_save_all to persist to disk.", "", JSON.stringify(result, null, 2));

      return {
        content: [{ type: "text", text: lines.join("\n") }],
      };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_set_component_defaults
// ---------------------------------------------------------------------------

server.tool(
  "ue5_set_component_defaults",
  [
    "Set properties on a named subobject component of a Blueprint's CDO.",
    "Use this for nested component properties like HUDWidgetComponent.WidgetClass",
    "or LeftHandMesh.SkeletalMesh that cannot be set via ue5_set_blueprint_defaults.",
  ].join(" "),
  {
    asset_path: z
      .string()
      .describe("Content path of the Blueprint asset"),
    component_name: z
      .string()
      .describe("Name of the component subobject (e.g. 'HUDWidgetComponent')"),
    properties: z
      .array(
        z.object({
          name: z
            .string()
            .describe("Property name on the component (case-sensitive)"),
          value: z
            .string()
            .describe("Value string — for object references use the full content path"),
        })
      )
      .describe("Properties to set on the component"),
  },
  async ({ asset_path, component_name, properties }) => {
    try {
      const result = await ue5.setComponentDefaults(asset_path, component_name, properties);
      // Total failure: properties were requested but none were actually set.
      const totalFailure = properties.length > 0 && (result.properties_set ?? 0) === 0;
      return {
        content: [
          {
            type: "text",
            text: [
              `Set ${result.properties_set} of ${properties.length} properties on '${result.component_name}' (${result.component_class}) of ${asset_path}.`,
              totalFailure
                ? `ERROR: none of the ${properties.length} requested properties were set.`
                : null,
              result.warnings?.length > 0
                ? `Warnings:\n${result.warnings.map((w) => `  - ${w}`).join("\n")}`
                : "No warnings.",
              "",
              "Next step: call ue5_save_all to persist to disk.",
              "",
              JSON.stringify(result, null, 2),
            ].filter((l) => l !== null).join("\n"),
          },
        ],
        isError: totalFailure,
      };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_get_component_defaults
// ---------------------------------------------------------------------------

server.tool(
  "ue5_get_component_defaults",
  [
    "Read the configured default values of a Blueprint's components — the read",
    "counterpart of ue5_set_component_defaults. For each property it reports both",
    "this Blueprint's value and the component-class CDO value, so you can see",
    "exactly what was overridden (and verify a prior set, e.g. a Character mesh's",
    "RelativeLocation/RelativeRotation). Resolves SCS components, inherited C++",
    "components, and native CDO subobjects. By default only properties that differ",
    "from the class CDO are returned; pass all_props=true for the full set.",
  ].join(" "),
  {
    asset_path: z
      .string()
      .describe("Content path of the Blueprint asset"),
    component_name: z
      .string()
      .optional()
      .describe("Single component variable name (e.g. 'CharacterMesh0'). Omit to dump all SCS components."),
    all_props: z
      .boolean()
      .optional()
      .describe("true to emit every editable property; default emits only properties that differ from the component class CDO."),
  },
  async ({ asset_path, component_name, all_props }) => {
    try {
      const result = await ue5.getComponentDefaults(asset_path, component_name, all_props);
      return {
        content: [{ type: "text", text: JSON.stringify(result, null, 2) }],
      };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_add_component_to_blueprint
// ---------------------------------------------------------------------------

server.tool(
  "ue5_add_component_to_blueprint",
  [
    "Add a new component to a Blueprint's SimpleConstructionScript (component tree).",
    "Use this to add components like UCesiumIonRasterOverlay, UStaticMeshComponent, etc.",
    "Optionally set initial properties on the component in the same call.",
    "Pass attach_parent to nest a scene component under another component (e.g. a CameraComponent under a SpringArmComponent) instead of the default scene root.",
  ].join(" "),
  {
    asset_path: z
      .string()
      .describe("Content path of the Blueprint asset"),
    component_class: z
      .string()
      .describe(
        "Component class name (e.g. 'UCesiumIonRasterOverlay', 'UStaticMeshComponent')"
      ),
    component_name: z
      .string()
      .optional()
      .describe("Name for the new component (defaults to class name if omitted)"),
    attach_parent: z
      .string()
      .optional()
      .describe(
        "Name of an existing scene component to attach this new (scene) component under, " +
          "e.g. attach a 'Camera' under a 'SpringArm'. Omit to attach to the default scene " +
          "root. Ignored for non-scene components."
      ),
    properties: z
      .array(
        z.object({
          name: z.string().describe("Property name (case-sensitive)"),
          value: z
            .string()
            .describe(
              "Value string — for object references use the full content path"
            ),
        })
      )
      .optional()
      .describe("Optional initial properties to set on the new component"),
  },
  async ({ asset_path, component_class, component_name, properties, attach_parent }) => {
    try {
      const result = await ue5.addComponentToBlueprint(
        asset_path,
        component_class,
        component_name || "",
        properties || [],
        attach_parent || ""
      );
      return {
        content: [
          {
            type: "text",
            text: [
              `Added component '${result.component_name}' (${result.component_class}) to ${asset_path}.`,
              result.attached_to
                ? `Attached under '${result.attached_to}'.`
                : "",
              result.properties_set > 0
                ? `Set ${result.properties_set} initial properties.`
                : "",
              result.warnings?.length > 0
                ? `Warnings:\n${result.warnings.map((w) => `  - ${w}`).join("\n")}`
                : "No warnings.",
              "",
              "Next step: call ue5_save_all to persist to disk.",
              "",
              JSON.stringify(result, null, 2),
            ].join("\n"),
          },
        ],
      };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_remove_component_from_blueprint
// ---------------------------------------------------------------------------

server.tool(
  "ue5_remove_component_from_blueprint",
  [
    "Remove a component from a Blueprint's SimpleConstructionScript (component tree) by name.",
    "Children of the removed component are re-parented so they aren't lost.",
    "Refuses to delete the default scene root.",
    "Use this to fix up duplicate or orphaned components left over by an interrupted edit.",
  ].join(" "),
  {
    asset_path: z
      .string()
      .describe("Content path of the Blueprint asset"),
    component_name: z
      .string()
      .describe("Name of the component to remove (e.g. 'Box')"),
  },
  async ({ asset_path, component_name }) => {
    try {
      const result = await ue5.removeComponentFromBlueprint(asset_path, component_name);
      return {
        content: [
          {
            type: "text",
            text: [
              `Removed component '${result.component_name}' from ${asset_path}.`,
              result.promoted_children > 0
                ? `Promoted ${result.promoted_children} child component(s) to the parent.`
                : "",
              "",
              "Next step: call ue5_save_all to persist to disk.",
              "",
              JSON.stringify(result, null, 2),
            ].filter(Boolean).join("\n"),
          },
        ],
      };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_create_level
// ---------------------------------------------------------------------------

server.tool(
  "ue5_open_level",
  "Open an existing level in the UE5 editor by its content path. Use this to switch the editor to a different map before spawning actors or modifying world settings.",
  {
    level_path: z
      .string()
      .describe("Content path of the level to open, e.g. '/Game/Maps/L_Persistent_Exercise'"),
  },
  async ({ level_path }) => {
    try {
      const result = await ue5.openLevel(level_path);
      return {
        content: [{ type: "text", text: `Opened level: ${result.world}\n\n${JSON.stringify(result, null, 2)}` }],
      };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_create_level",
  [
    "Create a new empty persistent level in the UE5 editor and save it to disk. The level is opened in the editor after creation.",
    "Set partitioned:true for a World Partition map (streaming grid, external actors) instead of a classic monolithic level.",
  ].join(" "),
  {
    level_path: z
      .string()
      .describe("Content path for the level, e.g. '/Game/Maps/L_Persistent_Exercise'"),
    partitioned: z
      .boolean()
      .optional()
      .describe("Create a World Partition map instead of a classic level (default false)"),
  },
  async ({ level_path, partitioned }) => {
    try {
      const result = await ue5.createLevel(level_path, { partitioned });
      const status = result.already_existed
        ? `Level already exists at: ${result.level_path}`
        : `Level created at: ${result.level_path} (${result.file_path})`;
      return {
        content: [{ type: "text", text: `${status}\n\n${JSON.stringify(result, null, 2)}` }],
      };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_list_actors
// ---------------------------------------------------------------------------

server.tool(
  "ue5_list_actors",
  "List all actors in the currently open UE5 editor level. Optionally filter by class name substring.",
  {
    class_filter: z
      .string()
      .optional()
      .describe("Optional class name substring to filter actors (e.g. 'TeleportAnchor')"),
  },
  async ({ class_filter }) => {
    try {
      const result = await ue5.listActors(class_filter);
      const lines = result.actors.map(
        (a) => `  ${a.name} (${a.class}) @ [${a.location.x}, ${a.location.y}, ${a.location.z}]`
      );
      return {
        content: [
          {
            type: "text",
            text: [
              `${result.count} actor(s) in '${result.world}':`,
              ...lines,
              "",
              JSON.stringify(result, null, 2),
            ].join("\n"),
          },
        ],
      };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_update_actor
// ---------------------------------------------------------------------------

server.tool(
  "ue5_update_actor",
  [
    "Update an existing actor's location, rotation, label, or component properties in the current editor level.",
    "Finds the actor by its current label.",
    "Use component_properties to set properties like StaticMesh on a spawned StaticMeshActor.",
    "Example: component_name='StaticMeshComponent0', property_name='StaticMesh', value='/Engine/BasicShapes/Cube'.",
    "Leave component_name empty to set a property directly on the actor itself.",
  ].join(" "),
  {
    actor_label: z
      .string()
      .describe("Current label of the actor to update (shown in Outliner)"),
    location: z
      .object({ x: z.number(), y: z.number(), z: z.number() })
      .optional()
      .describe("New world-space location in centimetres"),
    rotation: z
      .object({ pitch: z.number(), yaw: z.number(), roll: z.number() })
      .optional()
      .describe("New rotation in degrees"),
    new_label: z
      .string()
      .optional()
      .describe("New label for the actor"),
    properties: z.preprocess(jsonPreprocess,
      z.array(z.object({
        name: z.string().describe("Property name on the actor (e.g. 'PlatformIndex')"),
        value: z.string().describe("Value string — for asset references use the full content path"),
      }))
      .optional()
      .describe("Set UPROPERTY values directly on the actor (shorthand — maps to component_properties with empty component_name)")),
    component_properties: z.preprocess(jsonPreprocess,
      z.array(
        z.object({
          component_name: z
            .string()
            .describe("Component name (e.g. 'StaticMeshComponent0'). Empty string to target the actor itself."),
          property_name: z
            .string()
            .describe("Property name on the component or actor (e.g. 'StaticMesh')"),
          value: z
            .string()
            .describe("Value string — for asset references use the full content path (e.g. '/Engine/BasicShapes/Cube')"),
        })
      )
      .optional()
      .describe("Set properties on actor components — use this to assign a StaticMesh after spawning a StaticMeshActor")),
  },
  async ({ actor_label, location, rotation, new_label, properties, component_properties }) => {
    // Merge shorthand properties into component_properties
    const allCompProps = [...(component_properties || [])];
    if (properties) {
      for (const p of properties) {
        allCompProps.push({ component_name: "", property_name: p.name, value: p.value });
      }
    }
    try {
      const result = await ue5.updateActor(actor_label, { location, rotation, newLabel: new_label, componentProperties: allCompProps.length > 0 ? allCompProps : undefined });
      return {
        content: [
          {
            type: "text",
            text: `Updated '${result.actor_label}' (${result.class})\n\n${JSON.stringify(result, null, 2)}`,
          },
        ],
      };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_delete_actor
// ---------------------------------------------------------------------------

server.tool(
  "ue5_delete_actor",
  "Delete an actor from the current editor level by its label.",
  {
    actor_label: z
      .string()
      .describe("Label of the actor to delete (shown in Outliner)"),
  },
  async ({ actor_label }) => {
    try {
      const result = await ue5.deleteActor(actor_label);
      return {
        content: [
          {
            type: "text",
            text: `Deleted '${result.deleted}' (${result.class})\n\n${JSON.stringify(result, null, 2)}`,
          },
        ],
      };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_style_widgets
// ---------------------------------------------------------------------------

const MarginSchema = z.object({
  left:   z.number().default(0),
  top:    z.number().default(0),
  right:  z.number().default(0),
  bottom: z.number().default(0),
});

const StyleEntrySchema = z.object({
  widget_name: z.string()
    .describe("Name of the widget in the WidgetTree — must match the FName exactly"),

  // ---- universal (any UWidget) ----
  visibility: z.enum(["Visible", "Hidden", "Collapsed", "HitTestInvisible", "SelfHitTestInvisible"]).optional()
    .describe("ESlateVisibility for any UWidget"),
  tooltip_text: z.string().optional().describe("Tooltip text (any UWidget)"),
  is_enabled: z.boolean().optional().describe("Enabled state (any UWidget)"),
  is_variable: z.boolean().optional().describe("Expose as BindWidget-compatible variable (any UWidget)"),
  render_translation_x: z.number().optional(),
  render_translation_y: z.number().optional(),
  render_angle: z.number().optional().describe("Render transform rotation in degrees"),
  render_scale_x: z.number().optional(),
  render_scale_y: z.number().optional(),
  render_shear_x: z.number().optional(),
  render_shear_y: z.number().optional(),
  render_pivot_x: z.number().optional().describe("0-1, default 0.5"),
  render_pivot_y: z.number().optional().describe("0-1, default 0.5"),
  clip_to_bounds: z.boolean().optional().describe("Shortcut for clipping=ClipToBounds vs Inherit"),
  clipping: z.enum(["Inherit", "ClipToBounds", "ClipToBoundsAlways", "ClipToBoundsWithoutIntersecting", "OnDemand"]).optional(),

  // ---- generic slot (any panel slot type) ----
  slot_padding: MarginSchema.optional().describe("Slot padding for VBox/HBox/Overlay/Grid/UniformGrid/ScrollBox/Border/WrapBox/ScaleBox/SafeZone slots"),
  slot_h_align: z.enum(["Left", "Center", "Right", "Fill"]).optional(),
  slot_v_align: z.enum(["Top", "Center", "Bottom", "Fill"]).optional(),
  slot_size_rule: z.enum(["Auto", "Fill"]).optional().describe("VerticalBoxSlot/HorizontalBoxSlot only"),
  slot_size_value: z.number().optional().describe("Fill weight when slot_size_rule=Fill"),
  slot_row: z.number().int().optional().describe("GridSlot/UniformGridSlot row index"),
  slot_column: z.number().int().optional().describe("GridSlot/UniformGridSlot column index"),
  slot_row_span: z.number().int().optional().describe("GridSlot only"),
  slot_column_span: z.number().int().optional().describe("GridSlot only"),
  slot_z_order: z.number().int().optional().describe("CanvasPanelSlot ZOrder / GridSlot Layer"),

  // ---- UCanvasPanelSlot ----
  width_override:  z.number().optional(),
  height_override: z.number().optional(),
  anchor: z.enum([
    "top-left", "top-center", "top-right",
    "center-left", "center", "center-right",
    "bottom-left", "bottom-center", "bottom-right",
  ]).optional().describe("CanvasPanelSlot anchor preset"),
  position_x:  z.number().optional(),
  position_y:  z.number().optional(),
  alignment_x: z.number().min(0).max(1).optional().describe("CanvasPanelSlot pivot X"),
  alignment_y: z.number().min(0).max(1).optional().describe("CanvasPanelSlot pivot Y"),
  z_order:     z.number().int().optional().describe("CanvasPanelSlot Z order"),
  auto_size:   z.boolean().optional().describe("CanvasPanelSlot auto-size"),
  padding:     MarginSchema.optional().describe("Legacy: CanvasPanelSlot offsets (top-left + bottom-right)"),

  // ---- UTextBlock ----
  text: z.string().optional(),
  font_size: z.number().optional(),
  font_bold: z.boolean().optional().describe("Deprecated alias — prefer font_typeface"),
  font_typeface: z.string().optional().describe("Regular | Bold | Italic | BoldItalic | Light | Black"),
  color: z.string().optional().describe("'#RRGGBB' or '#RRGGBBAA'"),
  auto_wrap: z.boolean().optional(),
  horizontal_alignment: z.enum(["Left", "Center", "Right"]).optional().describe("Text justification"),
  shadow_offset_x: z.number().optional(),
  shadow_offset_y: z.number().optional(),
  shadow_color: z.string().optional(),
  outline_size: z.number().optional(),
  outline_color: z.string().optional(),
  min_desired_width: z.number().optional(),

  // ---- URichTextBlock ----
  text_style_set: z.string().optional().describe("Content path to a UDataTable of FRichTextStyleRow rows"),

  // ---- UButton ----
  background_color: z.string().optional().describe("Button tint applied to Normal/Hovered/Pressed if specific colors not given"),
  hovered_color: z.string().optional(),
  pressed_color: z.string().optional(),
  disabled_color: z.string().optional(),
  content_padding: MarginSchema.optional().describe("UButton normal/pressed padding, or UBorder/UBackgroundBlur content padding"),
  click_method: z.enum(["DownAndUp", "MouseDown", "MouseUp", "PreciseClick"]).optional(),
  is_focusable: z.boolean().optional(),

  // ---- UImage ----
  brush_texture: z.string().optional().describe("Content path to a UTexture2D"),
  brush_tint: z.string().optional().describe("'#RRGGBB' tint multiplier"),
  brush_size_x: z.number().optional(),
  brush_size_y: z.number().optional(),
  brush_draw_as: z.enum(["Box", "Border", "Image", "RoundedBox", "NoDrawType"]).optional(),
  brush_margin: MarginSchema.optional(),
  brush_tiling: z.enum(["NoTile", "Horizontal", "Vertical", "Both"]).optional(),

  // ---- UBorder ----
  content_color: z.string().optional(),

  // ---- UCheckBox ----
  is_checked: z.boolean().optional(),
  check_type: z.string().optional(),

  // ---- UProgressBar ----
  fill_color: z.string().optional().describe("Alias for color on UProgressBar"),
  percent: z.number().min(0).max(1).optional(),
  bar_fill_type: z.enum(["LeftToRight", "RightToLeft", "FillFromCenter", "FillFromCenterHorizontal", "FillFromCenterVertical", "TopToBottom", "BottomToTop"]).optional(),
  marquee: z.boolean().optional(),

  // ---- USlider ----
  value: z.number().optional().describe("USlider value / WidgetSwitcher won't accept this — use active_widget_index"),
  min_value: z.number().optional(),
  max_value: z.number().optional(),
  step_size: z.number().optional(),
  orientation: z.enum(["Horizontal", "Vertical"]).optional().describe("USlider / UScrollBox"),
  slider_bar_color: z.string().optional(),
  slider_handle_color: z.string().optional(),

  // ---- UScrollBox ----
  scroll_bar_visibility: z.enum(["Visible", "Hidden", "Collapsed", "HitTestInvisible", "SelfHitTestInvisible"]).optional(),
  allow_overscroll: z.boolean().optional(),
  scroll_bar_thickness: z.number().optional(),
  always_show_scrollbar: z.boolean().optional(),

  // ---- USizeBox ----
  min_width: z.number().optional(),
  min_height: z.number().optional(),
  max_width: z.number().optional(),
  max_height: z.number().optional(),
  min_aspect_ratio: z.number().optional(),
  max_aspect_ratio: z.number().optional(),
  clear_width_override: z.boolean().optional(),
  clear_height_override: z.boolean().optional(),

  // ---- USpacer ----
  size_x: z.number().optional(),
  size_y: z.number().optional(),

  // ---- UWidgetSwitcher ----
  active_widget_index: z.number().int().optional(),

  // ---- Throbber / CircularThrobber ----
  number_of_pieces: z.number().int().optional(),
  period: z.number().optional(),
  radius: z.number().optional(),
  animate_horizontally: z.boolean().optional(),
  animate_vertically: z.boolean().optional(),
  animate_opacity: z.boolean().optional(),

  // ---- UBackgroundBlur ----
  blur_strength: z.number().optional(),
  blur_radius: z.number().int().optional(),

  // ---- USafeZone ----
  pad_left: z.boolean().optional(),
  pad_top: z.boolean().optional(),
  pad_right: z.boolean().optional(),
  pad_bottom: z.boolean().optional(),
  pad_sides: z.object({
    left: z.boolean().optional(),
    top: z.boolean().optional(),
    right: z.boolean().optional(),
    bottom: z.boolean().optional(),
  }).optional(),

  // ---- UScaleBox ----
  stretch: z.enum(["None", "Fill", "ScaleToFit", "ScaleToFitX", "ScaleToFitY", "ScaleToFill", "ScaleBySafeZone", "UserSpecified"]).optional(),
  stretch_direction: z.enum(["Both", "DownOnly", "UpOnly"]).optional(),
  user_specified_scale: z.number().optional(),

  // ---- UComboBoxString ----
  add_option: z.union([z.string(), z.array(z.string())]).optional(),
  clear_options: z.boolean().optional(),
  selected_option: z.string().optional(),
}).passthrough();

server.tool(
  "ue5_style_widgets",
  [
    "Apply visual styling, layout, and property edits to named widgets in an existing Widget Blueprint.",
    "Each style entry targets one widget by widget_name and applies only the fields you provide.",
    "Inapplicable keys are skipped silently — the response reports applied[] and skipped[] per widget.",
    "Supports any widget class (TextBlock, RichTextBlock, Button, Image, Border, CheckBox, ProgressBar, Slider, ScrollBox, SizeBox, Spacer, WidgetSwitcher, CircularThrobber, BackgroundBlur, SafeZone, ScaleBox, ComboBoxString, …).",
    "Supports any slot class via the slot_* keys (CanvasPanelSlot, VerticalBoxSlot, HorizontalBoxSlot, OverlaySlot, GridSlot, UniformGridSlot, WrapBoxSlot, ScrollBoxSlot, BorderSlot, ScaleBoxSlot).",
    "Supports render transform (render_translation_x/y, render_angle, render_scale_x/y, render_pivot_x/y), tooltip_text, is_enabled, is_variable, and clipping on any UWidget.",
    "Call ue5_compile_widget_blueprint after structural edits and ue5_save_all to persist.",
  ].join(" "),
  {
    widget_blueprint: z
      .string()
      .describe("Content path of the Widget Blueprint to style, e.g. '/Game/UI/WBP_MainMenu'"),
    styles: z
      .array(StyleEntrySchema)
      .min(1)
      .describe("Array of per-widget styling operations"),
  },
  async ({ widget_blueprint, styles }) => {
    try {
      const result = await ue5.styleWidgets(widget_blueprint, styles);

      const lines = (result.results ?? []).map((r) => {
        if (r.status === "not_found") {
          return `  [not_found] ${r.widget_name}`;
        }
        const applied  = (r.applied  ?? []).join(", ") || "none";
        const skipped  = (r.skipped  ?? []).join(", ") || "none";
        return `  [${r.widget_class}] ${r.widget_name}: applied=[${applied}] skipped=[${skipped}]`;
      });

      return {
        content: [
          {
            type: "text",
            text: [
              `Styled ${widget_blueprint}`,
              `  Total properties applied: ${result.total_applied}`,
              `  Total properties skipped: ${result.total_skipped}`,
              "",
              "Per-widget results:",
              ...lines,
              "",
              "Next step: call ue5_save_all to persist to disk.",
              "",
              JSON.stringify(result, null, 2),
            ].join("\n"),
          },
        ],
      };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_delete_asset
// ---------------------------------------------------------------------------

server.tool(
  "ue5_delete_asset",
  "Delete a content asset (Data Asset, Blueprint, Widget Blueprint, Material, Level, etc.) from the UE5 project. The asset is force-deleted along with its file on disk. Call ue5_save_all afterwards to ensure the project state is clean.",
  {
    asset_path: z
      .string()
      .describe("Full content path of the asset to delete, e.g. '/Game/Data/Exercises/DA_ADL_06_IronClothes'"),
  },
  async ({ asset_path }) => {
    try {
      const result = await ue5.deleteAsset(asset_path);
      return {
        content: [
          {
            type: "text",
            text: [
              `Deleted asset: ${result.deleted}`,
              `  Class: ${result.class}`,
              `  Objects removed: ${result.count}`,
              "",
              "Next step: call ue5_save_all to persist the deletion to disk.",
              "",
              JSON.stringify(result, null, 2),
            ].join("\n"),
          },
        ],
      };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_batch
// ---------------------------------------------------------------------------

server.tool(
  "ue5_batch",
  [
    "Execute multiple UE5 editor operations in a single HTTP round-trip.",
    "Each operation maps to a raw HTTP call to the UnrealNGGMCP bridge.",
    "Use this to batch-create assets, set properties, and save in one call instead of N separate tool calls.",
    "Operations are executed sequentially in order; a failed step does not abort subsequent ones.",
    "Results are returned per-operation with their individual status codes and response bodies.",
  ].join(" "),
  {
    operations: z
      .array(
        z.object({
          method: z
            .enum(["GET", "POST"])
            .describe("HTTP method for this operation"),
          path: z
            .string()
            .describe("Bridge endpoint path, e.g. '/editor/save_all' or '/assets/set_property'"),
          body: z
            .preprocess(jsonPreprocess, z.record(z.unknown()))
            .default({})
            .describe("Request body object (for POST operations)"),
        })
      )
      .min(1)
      .describe("Ordered list of operations to execute in one round-trip"),
  },
  async ({ operations }) => {
    try {
      const result = await ue5.batch(operations);
      const lines = (result.results ?? []).map((r) => {
        const ok = r.status >= 200 && r.status < 300;
        return `  [${ok ? "OK" : "ERR"}] ${r.index}: status=${r.status}`;
      });
      const anyError = (result.results ?? []).some((r) => r.status < 200 || r.status >= 300);
      return {
        content: [
          {
            type: "text",
            text: [
              `Batch executed ${operations.length} operation(s) — ${anyError ? "some errors" : "all succeeded"}:`,
              ...lines,
              "",
              JSON.stringify(result, null, 2),
            ].join("\n"),
          },
        ],
        isError: anyError,
      };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_configure_imc
// ---------------------------------------------------------------------------

server.tool(
  "ue5_configure_imc",
  "Clear an InputMappingContext and write key mappings for one or more InputActions. " +
  "Supports modifiers: 'Negate', 'SwizzleAxis', 'DeadZone', 'Scalar'. " +
  "Use this instead of ue5_set_asset_property for IMC mappings — the generic property endpoint cannot handle the complex FEnhancedActionKeyMapping struct array.",
  {
    imc_path: z
      .string()
      .describe("Content path of the InputMappingContext, e.g. '/Game/MyGame/Input/IMC_MyGame'"),
    mappings: z
      .array(
        z.object({
          action_path: z
            .string()
            .describe("Content path of the InputAction, e.g. '/Game/MyGame/Input/IA_Move'"),
          key: z
            .string()
            .describe("Key name exactly as UE5 expects it, e.g. 'W', 'S', 'A', 'D', 'Gamepad_LeftX'"),
          modifiers: z
            .array(z.enum(["Negate", "SwizzleAxis", "DeadZone", "Scalar"]))
            .default([])
            .describe("Ordered list of input modifiers to apply to this mapping"),
        })
      )
      .min(1)
      .describe(
        "Mappings to write. The IMC is cleared first, then all entries are added in order. " +
        "Typical WASD setup for an Axis2D action: " +
        "D=[], A=[Negate], W=[SwizzleAxis], S=[SwizzleAxis,Negate]"
      ),
  },
  async ({ imc_path, mappings }) => {
    try {
      const result = await ue5.configureIMC(imc_path, mappings);
      const lines = [
        `IMC configured: ${result.imc_path}`,
        `Mappings added: ${result.mappings_added}`,
        ...(result.warnings?.length > 0
          ? [`Warnings:\n${result.warnings.map((w) => `  - ${w}`).join("\n")}`]
          : []),
        "",
        "Next step: call ue5_save_all to persist to disk.",
      ];
      return { content: [{ type: "text", text: lines.join("\n") }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_kill_editor
// ---------------------------------------------------------------------------

server.tool(
  "ue5_kill_editor",
  "Force-kill the running Unreal Editor process (UnrealEditor.exe). " +
  "Use this before running an external build — Live Coding blocks Build.bat while the editor is open. " +
  "After killing, call ue5_build, then ue5_launch_editor to reopen.",
  {},
  async () => {
    try {
      // taskkill exits 0 when a process was killed. Exit code 128 means "no
      // such process" — i.e. the editor wasn't running, which is not an error.
      // Branch on the exit code rather than matching localized stdout text so
      // this works on non-English Windows.
      execSync("taskkill /F /IM UnrealEditor.exe", { encoding: "utf8", stdio: "pipe" });
      return { content: [{ type: "text", text: "UnrealEditor.exe terminated. Safe to run ue5_build now." }] };
    } catch (err) {
      const out = (err.stdout ?? "") + (err.stderr ?? "") + (err.message ?? "");
      // 128 = process not found (errorlevel from taskkill when no matching task).
      if (err.status === 128) {
        return { content: [{ type: "text", text: "UnrealEditor.exe was not running (nothing to kill)." }] };
      }
      // Fallback for unusual configurations: still don't treat "not running" as
      // an error if the localized output hints at it.
      if (out.includes("not found") || out.includes("no tasks") || out.includes("not running")) {
        return { content: [{ type: "text", text: "UnrealEditor.exe was not running (nothing to kill)." }] };
      }
      return { content: [{ type: "text", text: `taskkill failed: ${out}` }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_build
// ---------------------------------------------------------------------------

server.tool(
  "ue5_build",
  "Compile the UE5 project using Build.bat (editor or game target). " +
  "The editor must NOT be running when this is called — use ue5_kill_editor first. " +
  "Returns the last 60 lines of build output and a pass/fail status. " +
  "Project + engine are resolved from the running bridge if available, otherwise from cwd.",
  {
    target: z
      .enum(["editor", "game"])
      .default("editor")
      .describe("Build target: 'editor' (default) or 'game' (standalone)"),
  },
  async ({ target }) => {
    let proj;
    try {
      proj = await resolveProject();
    } catch (err) {
      return { content: [{ type: "text", text: `Build FAILED: ${err.message}` }], isError: true };
    }
    const targetName = target === "editor" ? proj.editor_target : proj.game_target;
    // Invoke Build.bat directly via spawnSync (shell:false) so paths containing
    // spaces or single quotes are passed verbatim as argv entries instead of
    // being string-interpolated into a PowerShell -Command script (which broke
    // on quotes). Success/failure is decided by the actual Build.bat exit code,
    // not by grepping the output for English "Succeeded"/"up-to-date" strings.
    try {
      // Run through `cmd.exe /c` (Build.bat is a batch file) but keep shell:false
      // so Node does NOT re-parse/expand the argv — each entry is passed as a
      // distinct, individually-quoted argument. UE paths never contain a literal
      // double-quote, so this is safe even with spaces or single quotes.
      const result = spawnSync(
        process.env.ComSpec || "cmd.exe",
        ["/c", proj.build_bat, targetName, "Win64", "Development", proj.uproject_path],
        { encoding: "utf8", timeout: 300_000, windowsHide: true, maxBuffer: 64 * 1024 * 1024 }
      );
      if (result.error) throw result.error;
      const rawOutput = (result.stdout ?? "") + (result.stderr ?? "");
      // Tail to the last 60 lines for readability (previously done by Select-Object).
      const output = rawOutput.split(/\r?\n/).slice(-60).join("\n");
      // Build.bat returns 0 only on a successful (or up-to-date) build.
      const success = result.status === 0;
      return {
        content: [{
          type: "text",
          text: `Build ${success ? "SUCCEEDED" : `FAILED (exit code ${result.status})`}:\n\n${output}`,
        }],
        isError: !success,
      };
    } catch (err) {
      const output = (err.stdout ?? "") + (err.stderr ?? "") + (err.message ?? "");
      return {
        content: [{
          type: "text",
          text: `Build FAILED:\n\n${output}`,
        }],
        isError: true,
      };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_create_niagara_system
// ---------------------------------------------------------------------------

server.tool(
  "ue5_create_niagara_system",
  [
    "Create a Niagara particle system (UNiagaraSystem) asset in the UE5 Editor.",
    "Creates a blank system at asset_path, or clones an existing one if template_path is provided.",
    "After creation the asset must be opened in the Niagara editor to add and configure emitter modules",
    "(e.g. Spawn Burst Instantaneous, Initialize Particle, Add Velocity, Gravity Force, Sprite Renderer).",
    "Call ue5_save_all after creation to persist the asset to disk.",
  ].join(" "),
  {
    asset_path: z
      .string()
      .describe(
        "Destination content path for the new system, e.g. '/Game/MyGame/VFX/NS_BoxDestroy'"
      ),
    template_path: z
      .string()
      .optional()
      .describe(
        "Optional content path of an existing UNiagaraSystem to clone from, " +
        "e.g. '/Game/VFX/NS_SimpleSpriteBurst'. If omitted a blank system is created."
      ),
  },
  async ({ asset_path, template_path }) => {
    try {
      const result = await ue5.createNiagaraSystem({ asset_path, template_path });
      const statusLine = result.already_existed
        ? `Niagara system already existed at: ${result.asset_path}`
        : `Niagara system created at: ${result.asset_path}`;
      return {
        content: [
          {
            type: "text",
            text: [
              statusLine,
              result.next_step ?? "",
              "",
              JSON.stringify(result, null, 2),
            ].join("\n"),
          },
        ],
      };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_configure_niagara_system
// ---------------------------------------------------------------------------

server.tool(
  "ue5_configure_niagara_system",
  [
    "Configure an existing UNiagaraSystem with a sprite-burst emitter and particle parameters.",
    "If the system has no emitters, the endpoint automatically searches engine content for a",
    "sprite-burst template emitter and adds it.",
    "It then attempts to set spawn count, lifetime, colour, sprite size, velocity, and gravity",
    "via the system's user-exposed parameter store.",
    "Returns a full report: which parameters were set, which were not exposed (need manual config),",
    "and a list of all currently exposed user parameters.",
    "Call ue5_save_all after this tool to persist.",
  ].join(" "),
  {
    asset_path: z
      .string()
      .describe("Content path to an existing UNiagaraSystem, e.g. '/Game/MyGame/VFX/NS_BoxDestroy'"),
    spawn_count: z
      .number()
      .optional()
      .describe("Number of particles to spawn in the burst (default 10)"),
    lifetime_min: z
      .number()
      .optional()
      .describe("Minimum particle lifetime in seconds (default 0.6)"),
    lifetime_max: z
      .number()
      .optional()
      .describe("Maximum particle lifetime in seconds (default 0.8)"),
    color_r: z
      .number()
      .optional()
      .describe("Linear-space red channel of emitter colour (default 0.212 ≈ #8B hex channel)"),
    color_g: z
      .number()
      .optional()
      .describe("Linear-space green channel (default 0.139)"),
    color_b: z
      .number()
      .optional()
      .describe("Linear-space blue channel (default 0.071)"),
    sprite_size: z
      .number()
      .optional()
      .describe("Sprite size in Unreal Units (default 3.0)"),
    velocity: z
      .number()
      .optional()
      .describe("Outward launch speed in cm/s (default 200)"),
    gravity_z: z
      .number()
      .optional()
      .describe("Gravity acceleration on Z axis in cm/s² (default -980)"),
    loop_behavior: z
      .enum(["Once", "Infinite", "Multiple"])
      .optional()
      .describe("System loop behavior (default 'Once' — fires once and stops)"),
    reset_emitters: z
      .boolean()
      .optional()
      .describe("Clear and re-add the template emitter (default true). Set false to only update parameters on an already-configured system."),
  },
  async (params) => {
    try {
      const result = await ue5.configureNiagaraSystem(params);

      const lines = [
        result.emitter_added
          ? `Emitter added from template: ${result.template_used || "(unknown)"}`
          : `Emitter already present (no template add needed)`,
        "",
        `Parameters set (${result.parameters?.set?.length ?? 0}): ${(result.parameters?.set ?? []).join(", ") || "none"}`,
        `Not exposed    (${result.parameters?.not_exposed?.length ?? 0}): ${(result.parameters?.not_exposed ?? []).join(", ") || "none"}`,
        `All exposed    : ${(result.parameters?.all_exposed ?? []).join(", ") || "none"}`,
      ];

      if (result.manual_config_required) {
        lines.push("", `Manual config needed: ${result.manual_config_required}`);
      }

      lines.push("", "Steps:", ...(result.steps ?? []).map(s => `  • ${s}`));
      lines.push("", "Call ue5_save_all to persist.");
      lines.push("", JSON.stringify(result, null, 2));

      return { content: [{ type: "text", text: lines.join("\n") }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_set_niagara_emitter_params
// ---------------------------------------------------------------------------

server.tool(
  "ue5_set_niagara_emitter_params",
  [
    "Set Niagara emitter Rapid Iteration Parameters (RIPs) directly inside the emitter module stack.",
    "Unlike ue5_configure_niagara_system which only sets User.* exposed parameters,",
    "this endpoint reaches into every script's baked RIP store so it works even on",
    "template emitters (e.g. SimpleSpriteBurst) that don't expose their inputs as User params.",
    "Call with list_only=true first to discover all available RIP names and types on the emitter,",
    "then re-call with the exact names in raw_params or use the named shorthand params",
    "(spawn_count, lifetime_min/max, color_r/g/b, sprite_size, velocity_min/max, gravity_z).",
    "Call ue5_save_all after this tool to persist.",
  ].join(" "),
  {
    asset_path: z
      .string()
      .describe("Content path to an existing UNiagaraSystem, e.g. '/Game/MyGame/VFX/NS_BoxDestroyFX'"),
    emitter_index: z
      .number()
      .optional()
      .describe("Index of the emitter inside the system (default 0)"),
    list_only: z
      .boolean()
      .optional()
      .describe("If true, return all RIP names and types without modifying anything (default false). Use this to discover exact parameter names."),
    remove_emitter: z
      .boolean()
      .optional()
      .describe("If true, remove the emitter at emitter_index from the system. Ignores all other params."),
    spawn_count: z
      .number()
      .optional()
      .describe("Particle burst count — matches RIPs containing 'spawncount' / 'burstcount'"),
    lifetime_min: z
      .number()
      .optional()
      .describe("Minimum particle lifetime in seconds — matches 'lifetimemin' / 'lifetime.minimum'"),
    lifetime_max: z
      .number()
      .optional()
      .describe("Maximum particle lifetime in seconds — matches 'lifetimemax' / 'lifetime.maximum'"),
    color_r: z.number().optional().describe("Linear-space red channel (provide all three channels together)"),
    color_g: z.number().optional().describe("Linear-space green channel"),
    color_b: z.number().optional().describe("Linear-space blue channel"),
    sprite_size: z
      .number()
      .optional()
      .describe("Sprite size in Unreal Units — matches 'spritesize' / 'uniformsprite'"),
    velocity_min: z
      .number()
      .optional()
      .describe("Minimum outward speed in cm/s — matches 'speedmin' / 'velocitymin'"),
    velocity_max: z
      .number()
      .optional()
      .describe("Maximum outward speed in cm/s — matches 'speedmax' / 'velocitymax'"),
    gravity_z: z
      .number()
      .optional()
      .describe("Gravity on Z axis in cm/s² — matches 'gravityz' / 'gravityacceleration'"),
    raw_params: z
      .array(z.object({
        name:  z.string().describe("Exact full RIP name, e.g. 'Constants.Emitter.SpawnBurstInstantaneous.SpawnCount'"),
        value: z.number().describe("Value to set (float or int; color via color_r/g/b instead)"),
      }))
      .optional()
      .describe("Precise name-value pairs for exact RIP targeting. Use list_only=true to get names."),
  },
  async (params) => {
    try {
      const result = await ue5.setNiagaraEmitterParams(params);

      if (params.list_only) {
        const lines = [
          `RIPs found: ${result.rip_count} across emitter index ${params.emitter_index ?? 0}`,
          "",
          ...(result.rapid_iteration_params ?? []).map(
            p => `  ${p.name}  [${p.type}]  (${p.script})`
          ),
          "",
          result.hint ?? "",
        ];
        return { content: [{ type: "text", text: lines.join("\n") }] };
      }

      const lines = [
        `RIPs in emitter: ${result.rip_count}`,
        `Set   (${result.params_set?.length ?? 0}): ${(result.params_set ?? []).join(", ") || "none"}`,
        `Missed(${result.params_not_found?.length ?? 0}): ${(result.params_not_found ?? []).join(", ") || "none"}`,
        `Compiled: ${result.compiled ? "yes" : "no (nothing changed)"}`,
      ];

      if (result.hint) lines.push("", `Hint: ${result.hint}`);

      lines.push("", "All RIPs:");
      for (const rip of (result.all_rips ?? [])) lines.push(`  ${rip}`);
      lines.push("", "Call ue5_save_all to persist.");
      lines.push("", JSON.stringify(result, null, 2));

      return { content: [{ type: "text", text: lines.join("\n") }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_add_widget_to_blueprint
// ---------------------------------------------------------------------------

server.tool(
  "ue5_add_widget_to_blueprint",
  "Append UMG widgets to an existing Widget Blueprint's widget tree without clearing existing widgets. " +
  "All widget names must be unique within the tree — duplicates are skipped. " +
  "Widgets added in the same call can reference each other by name using the parent field. " +
  "Use ue5_style_widgets afterwards to set slot_* properties, positions, sizes, colors, and text. " +
  "Use type='UserWidget' + user_widget_class to embed an existing Widget Blueprint as a child.",
  {
    widget_blueprint: z.string().describe("Content path of the Widget Blueprint, e.g. '/Game/MyGame/UI/WBP_Settings'"),
    widgets: z.array(z.object({
      type:              z.enum(WIDGET_TYPES).describe("UMG widget class"),
      name:              z.string().describe("Unique FName for this widget — must not already exist in the tree"),
      parent:            z.string().optional().describe("FName of an existing (or just-added) parent panel widget"),
      user_widget_class: z.string().optional().describe("Required when type='UserWidget' — content path to a Widget Blueprint to embed"),
    })).min(1).describe("Widgets to add"),
  },
  async (params) => {
    try {
      const result = await ue5.addWidgetToBlueprint(params.widget_blueprint, params.widgets);
      return {
        content: [{
          type: "text",
          text: [
            `Widget blueprint: ${result.widget_blueprint}`,
            `Widgets added:   ${result.widgets_added}`,
            `Widgets skipped: ${result.widgets_skipped}`,
            "",
            "Call ue5_style_widgets to set positions/sizes/colors, then ue5_save_all.",
          ].join("\n"),
        }],
      };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_get_widget_tree
// ---------------------------------------------------------------------------

server.tool(
  "ue5_get_widget_tree",
  "Inspect the full widget hierarchy of a Widget Blueprint. Returns nested JSON with each widget's name, class, slot details (canvas/vbox/hbox/grid/overlay/etc.), render transform, tooltip, is_enabled, brush info, text content, and children.",
  {
    widget_blueprint: z.string().describe("Content path of the Widget Blueprint, e.g. '/Game/UI/WBP_MainMenu'"),
  },
  async ({ widget_blueprint }) => {
    try {
      const result = await ue5.getWidgetTree(widget_blueprint);
      return {
        content: [{
          type: "text",
          text: JSON.stringify(result, null, 2),
        }],
      };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_remove_widget_from_blueprint
// ---------------------------------------------------------------------------

server.tool(
  "ue5_remove_widget_from_blueprint",
  "Delete a widget from a Widget Blueprint's tree. By default deletes the widget and all of its descendants. Pass cascade=false to refuse deletion if the widget has children. The root widget cannot be removed.",
  {
    widget_blueprint: z.string().describe("Content path of the Widget Blueprint"),
    widget_name:      z.string().describe("FName of the widget to remove"),
    cascade:          z.boolean().optional().describe("If false, fails when widget has children (default true)"),
  },
  async ({ widget_blueprint, widget_name, cascade }) => {
    try {
      const result = await ue5.removeWidgetFromBlueprint(widget_blueprint, widget_name, cascade);
      return {
        content: [{
          type: "text",
          text: [
            `Removed from ${result.widget_blueprint}:`,
            `  ${(result.removed ?? []).join(", ") || "(nothing)"}`,
            "",
            "Next step: ue5_compile_widget_blueprint, then ue5_save_all.",
          ].join("\n"),
        }],
      };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_reparent_widget
// ---------------------------------------------------------------------------

server.tool(
  "ue5_reparent_widget",
  "Move a widget under a different parent panel within the same Widget Blueprint. Slot properties are reset by UE when the widget is attached to a new panel — call ue5_style_widgets with slot_* keys afterwards if needed. Cannot reparent the root widget; cannot reparent under self or a descendant.",
  {
    widget_blueprint: z.string().describe("Content path of the Widget Blueprint"),
    widget_name:      z.string().describe("FName of the widget to move"),
    new_parent:       z.string().describe("FName of the new parent panel widget"),
    child_index:      z.number().int().optional().describe("Insert position in new parent's children (default: append)"),
  },
  async ({ widget_blueprint, widget_name, new_parent, child_index }) => {
    try {
      const result = await ue5.reparentWidget(widget_blueprint, widget_name, new_parent, child_index);
      return {
        content: [{
          type: "text",
          text: [
            `Reparented ${result.widget_name} → ${result.new_parent}`,
            `New slot class: ${result.new_slot_class ?? "(unknown)"}`,
            "",
            "Next step: ue5_style_widgets with slot_* keys, then ue5_compile_widget_blueprint + ue5_save_all.",
          ].join("\n"),
        }],
      };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_rename_widget
// ---------------------------------------------------------------------------

server.tool(
  "ue5_rename_widget",
  "Rename a widget within a Widget Blueprint, preserving its slot, properties, and children. The new name becomes the FName used by BindWidget lookups in C++. New name must be unique within the tree.",
  {
    widget_blueprint: z.string().describe("Content path of the Widget Blueprint"),
    old_name:         z.string().describe("Current FName"),
    new_name:         z.string().describe("New FName — must be unique in the tree"),
  },
  async ({ widget_blueprint, old_name, new_name }) => {
    try {
      const result = await ue5.renameWidget(widget_blueprint, old_name, new_name);
      return {
        content: [{
          type: "text",
          text: [
            `Renamed ${result.old_name} → ${result.new_name} in ${result.widget_blueprint}`,
            "",
            "Next step: ue5_compile_widget_blueprint, then ue5_save_all.",
          ].join("\n"),
        }],
      };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_compile_widget_blueprint
// ---------------------------------------------------------------------------

server.tool(
  "ue5_compile_widget_blueprint",
  "Compile a Widget Blueprint after structural edits (create/add/remove/reparent/rename or style changes). Required before BindWidget lookups, runtime CreateWidget, or assigning the WBP to a TSubclassOf<UUserWidget> property.",
  {
    widget_blueprint: z.string().describe("Content path of the Widget Blueprint"),
  },
  async ({ widget_blueprint }) => {
    try {
      const result = await ue5.compileWidgetBlueprint(widget_blueprint);
      const lines = [
        `Compile ${result.widget_blueprint}: ${result.status}`,
      ];
      if ((result.warnings ?? []).length) {
        lines.push("Warnings:", ...result.warnings.map((w) => `  - ${w}`));
      }
      if ((result.errors ?? []).length) {
        lines.push("Errors:", ...result.errors.map((e) => `  - ${e}`));
      }
      lines.push("", "Next step: ue5_save_all.");
      return { content: [{ type: "text", text: lines.join("\n") }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_configure_anim_blueprint
// ---------------------------------------------------------------------------

server.tool(
  "ue5_configure_anim_blueprint",
  "Wire an Animation Blueprint's state machine with animation sequences and blend spaces. " +
  "Creates states, assigns sequence/blend-space players, and adds transitions. " +
  "After calling, use ue5_save_all to persist.",
  {
    asset_path: z.string().describe("Content path to the AnimBlueprint, e.g. '/Game/MyGame/Blueprints/ABP_MyGameCharacter'"),
    states: z.array(z.object({
      name: z.string().describe("State name, e.g. 'Idle'"),
      animation: z.string().describe("Content path to the animation asset (UAnimSequence or UBlendSpace)"),
      loop: z.boolean().optional().default(true).describe("Whether the animation should loop"),
    })).min(1).describe("Array of state definitions"),
    transitions: z.array(z.object({
      from: z.string().describe("Source state name"),
      to: z.string().describe("Target state name"),
      condition: z.string().describe("Transition condition expression, e.g. 'Speed > 10.0'"),
    })).optional().default([]).describe("Array of transition definitions"),
  },
  async (params) => {
    try {
      const result = await ue5.configureAnimBlueprint(
        params.asset_path,
        params.states,
        params.transitions
      );
      return {
        content: [{
          type: "text",
          text: [
            `AnimBlueprint: ${result.asset_path}`,
            `States created:      ${result.states_created}`,
            `Transitions created: ${result.transitions_created}`,
            `Total states:        ${result.total_states}`,
            "",
            "Call ue5_save_all to persist.",
          ].join("\n"),
        }],
      };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_set_level_environment
// ---------------------------------------------------------------------------

server.tool(
  "ue5_set_level_environment",
  "Open a level and configure ExponentialHeightFog, DirectionalLight, and SkyLight actors " +
  "for themed environments (night, lava, etc.). Spawns actors if missing, updates if existing. " +
  "After calling, use ue5_save_all to persist.",
  {
    level_path: z.string().describe("Content path to the level, e.g. '/Game/MyGame/Maps/Lvl_MyGame_Night'"),
    fog: z.object({
      enabled: z.boolean().optional().default(true),
      inscattering_color: z.object({
        r: z.number(), g: z.number(), b: z.number(), a: z.number().optional().default(1.0),
      }).optional(),
      density: z.number().optional(),
      height_falloff: z.number().optional(),
    }).optional().describe("Fog configuration"),
    directional_light: z.object({
      intensity: z.number().optional(),
      light_color: z.object({
        r: z.number(), g: z.number(), b: z.number(),
      }).optional(),
    }).optional().describe("Directional light configuration"),
    sky_light: z.object({
      intensity: z.number().optional(),
      light_color: z.object({
        r: z.number(), g: z.number(), b: z.number(),
      }).optional(),
    }).optional().describe("Sky light configuration"),
  },
  async (params) => {
    try {
      const result = await ue5.setLevelEnvironment(params.level_path, {
        fog: params.fog,
        directional_light: params.directional_light,
        sky_light: params.sky_light,
      });
      return {
        content: [{
          type: "text",
          text: [
            `Level: ${result.level_path}`,
            `Actions: ${result.actions?.join(", ") ?? "none"}`,
            "",
            "Call ue5_save_all to persist.",
          ].join("\n"),
        }],
      };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_import_asset
// ---------------------------------------------------------------------------

server.tool(
  "ue5_import_asset",
  "Import an external file from disk into the UE5 content browser. " +
  "Supports any format the editor can import: wav, ogg, mp3 (audio), " +
  "png, jpg, tga (textures), fbx, obj (meshes), etc. " +
  "The file must exist on the local disk before calling this tool.",
  {
    source_path: z.string().describe("Absolute disk path of the file to import, e.g. 'C:/audio/SFX_Explosion.wav'"),
    dest_path:   z.string().describe("Content browser folder to import into, e.g. '/Game/MyGame/Audio'"),
    asset_name:  z.string().optional().describe("Override the asset name (default: filename stem without extension)"),
  },
  async (params) => {
    try {
      const result = await ue5.importAsset(params.source_path, params.dest_path, params.asset_name);
      return {
        content: [{
          type: "text",
          text: [
            `Imported: ${result.source_path}`,
            `Asset path: ${result.asset_path}`,
            "",
            "Call ue5_save_all to persist.",
          ].join("\n"),
        }],
      };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_launch_editor
// ---------------------------------------------------------------------------

server.tool(
  "ue5_launch_editor",
  "Launch the current project via its .uproject file (goes through UnrealVersionSelector, same as a double-click). " +
  "No -log flag — no extra console window, just the editor. " +
  "Project is resolved from the running bridge if available, otherwise from cwd. " +
  "Returns immediately; call ue5_health_check in a loop (or wait ~30 seconds) to confirm the MCP bridge is ready.",
  {},
  async () => {
    let proj;
    try {
      proj = await resolveProject();
    } catch (err) {
      return { content: [{ type: "text", text: `Failed to resolve project: ${err.message}` }], isError: true };
    }
    try {
      // Launch via the .uproject file association so UnrealVersionSelector
      // resolves the engine and invokes it the same way a double-click would.
      // We shell-execute through explorer.exe (NOT `cmd /c start`): explorer
      // takes the path as a single argv argument, so characters that cmd treats
      // specially (`&`, `^`) and spaces in the path are NOT misinterpreted —
      // `cmd /c start` would split/expand them. windowsHide suppresses any
      // flash; detached+unref lets the editor outlive this MCP process. No -log
      // flag is passed so no extra console window appears.
      const child = spawn("explorer.exe", [proj.uproject_path], {
        detached: true,
        stdio: "ignore",
        windowsHide: true,
      });
      child.unref();
      return {
        content: [{
          type: "text",
          text:
            `Launching ${proj.uproject_path} via UnrealVersionSelector (no -log, no console window). ` +
            `Wait ~30 seconds then call ue5_health_check to confirm the bridge is ready.`,
        }],
      };
    } catch (err) {
      return { content: [{ type: "text", text: `Failed to launch editor: ${err.message}` }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_create_material
// ---------------------------------------------------------------------------

server.tool(
  "ue5_create_material",
  [
    "Create a PARENT UMaterial. For runtime DMI color/parameter workflows, pass use_parameters:true to get a Material with VectorParameter 'BaseColor' + ScalarParameter 'BrightnessMultiplier' wired to the BaseColor pin.",
    "Important: the default /Engine/BasicShapes materials have NO parameters — SetVectorParameterValue on a DMI built from them silently does nothing. Always create a parent material with use_parameters:true if you intend to swap colors at runtime.",
    "Use parameter name 'BaseColor' (not 'Color') in your SetVectorParameterValue calls in Blueprint.",
    "Pass unlit:true to route the color to Emissive Color and set Shading Model = Unlit — the material then renders the exact authored color independent of scene lighting (cables, holograms, glows, debug viz). Combine with use_parameters:true so the color stays runtime-drivable.",
    "Example: {\"asset_path\":\"/Game/Materials/M_ParamColor\",\"use_parameters\":true,\"base_color\":{\"r\":0.5,\"g\":0.5,\"b\":0.5}}",
    "Optional: pass blueprint_path + component_name to also assign the new material to a BP component as override material slot 0.",
  ].join(" "),
  {
    asset_path:         z.string().describe("Destination content path, e.g. '/Game/Materials/M_ParamColor'"),
    base_color:         z.preprocess(jsonPreprocess, z.object({ r: z.number(), g: z.number(), b: z.number() })).optional().describe("Linear-space RGB (default warm brown)"),
    use_parameters:     z.boolean().optional().describe("Add BaseColor VectorParameter + BrightnessMultiplier ScalarParameter (default false; pass true for DMI workflows)"),
    pixel_art:          z.boolean().optional().describe("Add UV-border outline shader"),
    texture_path:       z.string().optional().describe("Optional texture content path; implies use_parameters"),
    brightness_default: z.number().optional().describe("Default BrightnessMultiplier scalar value (default 1.0)"),
    unlit:              z.boolean().optional().describe("Route color to Emissive Color + set Shading Model = Unlit so it renders lighting-independent (default false)"),
    blueprint_path:     z.string().optional().describe("Optional: BP whose component should receive this material as override slot 0"),
    component_name:     z.string().optional().describe("Required if blueprint_path is set"),
  },
  async ({ asset_path, base_color, use_parameters, pixel_art, texture_path, brightness_default, unlit, blueprint_path, component_name }) => {
    try {
      const r = await ue5.createMaterial({ asset_path, base_color, use_parameters, pixel_art, texture_path, brightness_default, unlit, blueprint_path, component_name });
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_create_color_curve
// ---------------------------------------------------------------------------

server.tool(
  "ue5_create_color_curve",
  [
    "Create (or overwrite) a UCurveLinearColor asset from a list of RGB keys — the idiomatic UE way to author a color gradient (e.g. a load ramp green→orange→red).",
    "Each key is { time, r, g, b } (alpha is set to 1). 'time' is the X axis you sample with (for the power cable ramp this is the load ratio 0..1.5).",
    "linear:true (default) interpolates between keys; linear:false makes hard stepped bands. To avoid a muddy yellow blend between green and orange, add 'hold' keys (e.g. green at 0.0 AND 0.6, orange at 0.7 AND 0.95, red at 1.0).",
    "Assign the resulting curve to a slot like CableColorRampAsset.ColorCurve, or sample it in Blueprint via GetLinearColorValue.",
    "Example: {\"asset_path\":\"/Game/Materials/Curve_CableColor\",\"keys\":[{\"time\":0,\"r\":0.05,\"g\":0.85,\"b\":0.15},{\"time\":0.7,\"r\":1,\"g\":0.45,\"b\":0},{\"time\":1,\"r\":1,\"g\":0,\"b\":0}]}",
  ].join(" "),
  {
    asset_path: z.string().describe("Destination content path, e.g. '/Game/Materials/Curve_CableColor'"),
    keys:       z.preprocess(jsonPreprocess, z.array(z.object({ time: z.number(), r: z.number(), g: z.number(), b: z.number() }))).describe("Color keys in ascending time order; each { time, r, g, b } (alpha = 1)"),
    linear:     z.boolean().optional().describe("true (default) = linear interp between keys; false = constant/stepped bands"),
  },
  async ({ asset_path, keys, linear }) => {
    try {
      const r = await ue5.createColorCurve({ asset_path, keys, linear });
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_create_float_curve
// ---------------------------------------------------------------------------

server.tool(
  "ue5_create_float_curve",
  [
    "Create (or overwrite) a UCurveFloat asset — the scalar counterpart of ue5_create_color_curve. Use it for a value that changes over an input axis: light intensity, animation speed, spawn rate, alpha fade, a damage falloff curve.",
    "Each key is { time, value }. 'time' is the X axis you sample with; 'value' is the scalar output.",
    "linear:true (default) interpolates between keys; linear:false makes hard stepped bands.",
    "Sample it in Blueprint via GetFloatValue, or assign it to any FRuntimeFloatCurve / UCurveFloat* asset slot.",
    "Example: {\"asset_path\":\"/Game/Curves/Curve_Intensity\",\"keys\":[{\"time\":0,\"value\":0},{\"time\":0.5,\"value\":1},{\"time\":1,\"value\":0}]}",
  ].join(" "),
  {
    asset_path: z.string().describe("Destination content path, e.g. '/Game/Curves/Curve_Intensity'"),
    keys:       z.preprocess(jsonPreprocess, z.array(z.object({ time: z.number(), value: z.number() }))).describe("Float keys in ascending time order; each { time, value }"),
    linear:     z.boolean().optional().describe("true (default) = linear interp between keys; false = constant/stepped bands"),
  },
  async ({ asset_path, keys, linear }) => {
    try {
      const r = await ue5.createFloatCurve({ asset_path, keys, linear });
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_read_curve
// ---------------------------------------------------------------------------

server.tool(
  "ue5_read_curve",
  [
    "Read back a curve asset's keys. Works for both UCurveFloat and UCurveLinearColor — the editor is the source of truth for what's actually authored.",
    "Returns type:\"float\" with keys [{ time, value }], or type:\"color\" with keys [{ time, r, g, b, a }].",
    "Use it to verify a curve before sampling it, or to read an existing curve before editing it.",
  ].join(" "),
  {
    asset_path: z.string().describe("Content path of the curve asset, e.g. '/Game/Curves/Curve_Intensity'"),
  },
  async ({ asset_path }) => {
    try {
      const r = await ue5.readCurve({ asset_path });
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_create_material_instance
// ---------------------------------------------------------------------------

server.tool(
  "ue5_create_material_instance",
  [
    "Create a UMaterialInstanceConstant from a parent material in the UE5 Editor.",
    "Set scalar, vector, and texture parameter overrides on the instance.",
    "Re-calling with the same asset_path updates the existing instance in place (idempotent) — use it to change parameters later, no separate setter needed.",
    "Optionally apply the MI to a Blueprint's mesh component in one call.",
  ].join(" "),
  {
    asset_path: z.string().describe(
      "Content path for the new Material Instance, e.g. '/Game/Materials/MI_BoxHover'"
    ),
    parent_material: z.string().describe(
      "Content path of the parent material, e.g. '/Game/Materials/M_Box'"
    ),
    scalar_params: z.array(z.object({
      name:  z.string().describe("Scalar parameter name"),
      value: z.number().describe("Scalar parameter value"),
    })).optional().describe(
      "Scalar parameter overrides to set on the material instance"
    ),
    vector_params: z.array(z.object({
      name:  z.string().describe("Vector parameter name"),
      value: z.object({
        r: z.number().describe("Red channel (0-1)"),
        g: z.number().describe("Green channel (0-1)"),
        b: z.number().describe("Blue channel (0-1)"),
        a: z.number().optional().default(1).describe("Alpha channel (0-1, default 1)"),
      }).describe("RGBA color value"),
    })).optional().describe(
      "Vector parameter overrides to set on the material instance"
    ),
    texture_params: z.array(z.object({
      name:         z.string().describe("Texture parameter name"),
      texture_path: z.string().describe("Content path of the texture asset to bind"),
    })).optional().describe(
      "Texture parameter overrides; each { name, texture_path }. Skipped silently if the texture fails to load."
    ),
    blueprint_path: z.string().optional().describe(
      "Optional: apply the MI to this Blueprint's mesh component"
    ),
    component_name: z.string().optional().describe(
      "Optional: component name to apply MI to (default: StaticMeshComponent0)"
    ),
  },
  async ({ asset_path, parent_material, scalar_params, vector_params, texture_params, blueprint_path, component_name }) => {
    try {
      const result = await ue5.createMaterialInstance({
        asset_path,
        parent_material,
        scalar_params,
        vector_params,
        texture_params,
        blueprint_path,
        component_name,
      });
      const statusLine = result.already_existed
        ? `Material instance already existed at: ${result.asset_path}`
        : `Material instance created at: ${result.asset_path}`;
      const details = [
        statusLine,
        `Parent: ${result.parent_material}`,
        `Scalar params set: ${result.scalar_params_set}`,
        `Vector params set: ${result.vector_params_set}`,
        `Texture params set: ${result.texture_params_set}`,
        result.applied_to_blueprint ? "Applied to Blueprint component." : "",
      ].filter(Boolean).join("\n");
      return {
        content: [{
          type: "text",
          text: `${details}\n\nNext step: call ue5_save_all to persist to disk.\n\n${JSON.stringify(result, null, 2)}`,
        }],
      };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_read_material
// ---------------------------------------------------------------------------

server.tool(
  "ue5_read_material",
  [
    "Inspect a material or material instance in the UE5 Editor and return its parameters and render settings — the asset registry is the source of truth.",
    "Reports class (Material vs MaterialInstance), parent (for instances), domain, blend_mode, shading_model, and every scalar / vector / texture parameter with its CURRENT value.",
    "Call this BEFORE SetScalarParameterValue / SetVectorParameterValue (in Blueprint or on a DMI) to confirm the parameter name actually exists — the default /Engine/BasicShapes materials have NO parameters, so setting them silently does nothing.",
    "Example: {\"asset_path\":\"/Game/Materials/MI_BoxHover\"}",
  ].join(" "),
  {
    asset_path: z.string().describe("Content path of the material or material instance, e.g. '/Game/Materials/M_Box'"),
  },
  async ({ asset_path }) => {
    try {
      const r = await ue5.readMaterial({ asset_path });
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_create_post_process_material
// ---------------------------------------------------------------------------

server.tool(
  "ue5_create_post_process_material",
  [
    "Build a UMaterial with MaterialDomain = MD_PostProcess that highlights meshes with RenderCustomDepthPass enabled, but ONLY when those meshes are occluded by another opaque pixel (i.e. behind a wall).",
    "Used for the 'enemy through walls' outline effect. The material reads SceneTexture(SceneDepth), SceneTexture(CustomDepth), and SceneTexture(PostProcessInput0), then routes If(CustomDepth>SceneDepth ? Lerp(OriginalColor, HighlightColor, hasCustomDepth) : OriginalColor) → EmissiveColor.",
    "After creating the material you must (a) call ue5_spawn_post_process_volume to add it to the scene, and (b) enable RenderCustomDepthPass on every mesh that should be highlighted (use ue5_set_component_defaults with property bRenderCustomDepth = True).",
    "Example: { asset_path: '/Game/Materials/M_EnemyHighlight', highlight_color: { r: 1, g: 0.05, b: 0.05 } }",
  ].join(" "),
  {
    asset_path: z.string().describe("Destination content path, e.g. '/Game/Materials/M_EnemyHighlight'"),
    effect: z.enum(["occlusion_outline"]).optional().describe("Effect preset (only 'occlusion_outline' supported today)"),
    highlight_color: z.preprocess(jsonPreprocess,
      z.object({ r: z.number(), g: z.number(), b: z.number() })
    ).optional().describe("Linear-space RGB highlight color (default red {r:1, g:0.05, b:0.05})"),
  },
  async ({ asset_path, effect, highlight_color }) => {
    try {
      const r = await ue5.createPostProcessMaterial({ asset_path, effect, highlight_color });
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_spawn_post_process_volume
// ---------------------------------------------------------------------------

server.tool(
  "ue5_spawn_post_process_volume",
  [
    "Spawn (or reuse) an APostProcessVolume in the current editor world, configure it as unbounded, and add the supplied material to PostProcessSettings.WeightedBlendables.",
    "Idempotent: calling with the same actor_label twice updates the existing volume in place rather than duplicating it.",
    "Open the target level first with ue5_open_level so the volume is added to the right map. Save with ue5_save_all afterwards.",
    "Example: { material_path: '/Game/Materials/M_EnemyHighlight', actor_label: 'PP_EnemyHighlight' }",
  ].join(" "),
  {
    material_path: z.string().describe("Content path of a UMaterialInterface (UMaterial or UMaterialInstance)"),
    actor_label: z.string().optional().describe("Label of the volume actor (default 'PP_<MaterialName>')"),
    unbounded: z.boolean().optional().describe("When true (default), the post-process applies regardless of camera location"),
    priority: z.number().optional().describe("Volume priority (default 0) — higher overrides lower in overlapping volumes"),
  },
  async ({ material_path, actor_label, unbounded, priority }) => {
    try {
      const r = await ue5.spawnPostProcessVolume({ material_path, actor_label, unbounded, priority });
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Mesh composition tools (DynamicMesh handles)
// ---------------------------------------------------------------------------

const tuple3 = z.preprocess(jsonPreprocess,
  z.array(z.number()).length(3).describe("3-tuple [x,y,z]"));

server.tool(
  "ue5_mesh_create",
  "Create a new empty DynamicMesh handle on the UE5 plugin side. Returns the handle id for use in subsequent mesh_* calls.",
  {
    handle: z.string().describe("Unique handle id to create (caller-chosen)"),
  },
  async ({ handle }) => {
    try {
      const result = await ue5.meshCreate(handle);
      return { content: [{ type: "text", text: JSON.stringify(result) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_mesh_append_primitive",
  "Append a primitive shape (Box|Sphere|Cylinder|Cone|Torus|Capsule) to an existing mesh handle with an optional transform and per-shape params.",
  {
    handle: z.string().describe("Target mesh handle"),
    shape: z.enum(["Box", "Sphere", "Cylinder", "Cone", "Torus", "Capsule"])
      .describe("Primitive shape to append"),
    transform: z.preprocess(jsonPreprocess,
      z.object({
        location: tuple3.optional(),
        rotation: tuple3.optional(),
        scale:    tuple3.optional(),
      }).optional()).describe("Optional transform { location, rotation, scale }"),
    params: z.preprocess(jsonPreprocess,
      z.record(z.any()).optional()).describe("Per-shape parameters (e.g. radius, height, segments)"),
  },
  async ({ handle, shape, transform, params }) => {
    try {
      const result = await ue5.meshAppendPrimitive({ handle, shape, transform, params });
      return { content: [{ type: "text", text: JSON.stringify(result) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_mesh_boolean",
  "Perform a CSG boolean (Union|Subtract|Intersect) between two mesh handles. Result is written into 'handle'.",
  {
    handle: z.string().describe("Primary (destination) mesh handle"),
    other_handle: z.string().describe("Secondary mesh handle"),
    op: z.enum(["Union", "Subtract", "Intersect"]).describe("Boolean op"),
    transform: z.preprocess(jsonPreprocess,
      z.object({
        location: tuple3.optional(),
        rotation: tuple3.optional(),
        scale:    tuple3.optional(),
      }).optional()).describe("Optional transform applied to other_handle before the op"),
  },
  async ({ handle, other_handle, op, transform }) => {
    try {
      const result = await ue5.meshBoolean({ handle, other_handle, op, transform });
      return { content: [{ type: "text", text: JSON.stringify(result) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_mesh_transform",
  "Apply a transform (location/rotation/scale) to every vertex of a mesh handle.",
  {
    handle: z.string().describe("Target mesh handle"),
    location: tuple3.optional().describe("Translation [x,y,z]"),
    rotation: tuple3.optional().describe("Rotation [pitch,yaw,roll]"),
    scale:    tuple3.optional().describe("Scale [x,y,z]"),
  },
  async ({ handle, location, rotation, scale }) => {
    try {
      const result = await ue5.meshTransform({ handle, location, rotation, scale });
      return { content: [{ type: "text", text: JSON.stringify(result) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_mesh_deform",
  "Apply a deformation (Bend|Twist|Taper|Flare) along an axis to a mesh handle, bounded by upper/lower range.",
  {
    handle: z.string().describe("Target mesh handle"),
    op: z.enum(["Bend", "Twist", "Taper", "Flare"]).describe("Deformation op"),
    axis: z.enum(["X", "Y", "Z"]).describe("Axis along which to apply the deformation"),
    amount: z.number().describe("Deformation strength"),
    upper: z.number().optional().describe("Upper bound along axis"),
    lower: z.number().optional().describe("Lower bound along axis"),
  },
  async ({ handle, op, axis, amount, upper, lower }) => {
    try {
      const result = await ue5.meshDeform({ handle, op, axis, amount, upper, lower });
      return { content: [{ type: "text", text: JSON.stringify(result) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_mesh_remesh",
  "Remesh a mesh handle to a target edge length with iterations and optional smoothing.",
  {
    handle: z.string().describe("Target mesh handle"),
    target_edge_length: z.number().optional().describe("Target edge length (UU)"),
    iterations: z.number().int().optional().describe("Remesh iteration count"),
    smoothing: z.number().optional().describe("Smoothing factor (0-1 typical)"),
  },
  async ({ handle, target_edge_length, iterations, smoothing }) => {
    try {
      const result = await ue5.meshRemesh({ handle, target_edge_length, iterations, smoothing });
      return { content: [{ type: "text", text: JSON.stringify(result) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_mesh_bake_static",
  "Bake a DynamicMesh handle into a UStaticMesh asset at the given content path.",
  {
    handle: z.string().describe("Source mesh handle"),
    asset_path: z.string().describe("Destination content path, e.g. '/Game/Meshes/SM_MyMesh'"),
  },
  async ({ handle, asset_path }) => {
    try {
      const result = await ue5.meshBakeStatic({ handle, asset_path });
      return { content: [{ type: "text", text: JSON.stringify(result) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_mesh_delete_handle",
  "Release a DynamicMesh handle on the UE5 plugin side, freeing its memory.",
  {
    handle: z.string().describe("Handle id to delete"),
  },
  async ({ handle }) => {
    try {
      const result = await ue5.meshDeleteHandle(handle);
      return { content: [{ type: "text", text: JSON.stringify(result) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_anim_add_copy_bone",
  "Insert a Copy Bone skeletal control node into an AnimBP's AnimGraph and splice it before the Output Pose. " +
  "Configures source/target bone, copy flags (translation/rotation/scale), and control space. Compiles the AnimBP. " +
  "Common use: copy ik_hand_l to a virtual bone parented to weapon_r so the left hand follows the weapon.",
  {
    anim_bp_path: z.string().describe("Content path of the AnimBlueprint"),
    source_bone:  z.string().describe("Source bone name (FName) — what the node reads from"),
    target_bone:  z.string().describe("Target bone name (FName) — what the node writes to (often a virtual bone)"),
    copy_translation: z.boolean().optional().describe("Default true"),
    copy_rotation:    z.boolean().optional().describe("Default true"),
    copy_scale:       z.boolean().optional().describe("Default false"),
    control_space:    z.enum(["WorldSpace","ComponentSpace","ParentBoneSpace","BoneSpace"]).optional()
                       .describe("Default ComponentSpace"),
    node_offset_x:    z.number().optional().describe("X offset from Output Pose. Default -250."),
    node_offset_y:    z.number().optional().describe("Y offset from Output Pose. Default 150."),
  },
  async (args) => {
    try {
      const result = await ue5.animAddCopyBone(args);
      return { content: [{ type: "text", text: JSON.stringify(result) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_anim_add_hand_ik_retargeting",
  "Insert a Hand IK Retargeting skeletal control node into an AnimBP's AnimGraph. " +
  "Used so characters of different proportions can grip the same prop — moves IK hand bones to FK hand positions. " +
  "Optionally drive Alpha from a curve (curve_name + scale + bias). Compiles the AnimBP.",
  {
    anim_bp_path:  z.string().describe("Content path of the AnimBlueprint"),
    right_hand_fk: z.string().describe("Right hand FK bone name, e.g. 'hand_r'"),
    left_hand_fk:  z.string().describe("Left hand FK bone name, e.g. 'hand_l'"),
    right_hand_ik: z.string().describe("Right hand IK bone name, e.g. 'ik_hand_r'"),
    left_hand_ik:  z.string().describe("Left hand IK bone name, e.g. 'ik_hand_l'"),
    ik_bones_to_move: z.preprocess(jsonPreprocess, z.array(z.string()).optional())
      .describe("Array of IK bone names the node will move, e.g. ['ik_hand_gun']"),
    hand_fk_weight: z.number().optional().describe("0=left, 1=right, 0.5=equal. Default 0.5."),
    per_axis_alpha: z.preprocess(jsonPreprocess, z.array(z.number()).length(3).optional())
      .describe("[x,y,z] axis weights. Default [1,1,1]."),
    alpha_curve_name: z.string().optional()
      .describe("Curve name to drive Alpha (switches AlphaInputType to Curve). Tutorial uses 'disable_hand_ik_retargeting'."),
    alpha_scale: z.number().optional().describe("AlphaScaleBias.Scale (default 1.0; tutorial uses -1)"),
    alpha_bias:  z.number().optional().describe("AlphaScaleBias.Bias  (default 0.0; tutorial uses 1)"),
    node_offset_x: z.number().optional().describe("Default -250"),
    node_offset_y: z.number().optional().describe("Default -150"),
  },
  async (args) => {
    try {
      const result = await ue5.animAddHandIKRetargeting(args);
      return { content: [{ type: "text", text: JSON.stringify(result) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_anim_delete_node",
  "Remove a node from an AnimBP's AnimGraph by its GUID. If reconnect_pose=true (default), " +
  "the upstream pose feeding the node's input is rewired to the consumer of its output, " +
  "so the chain stays intact. Returns the deleted node's class name. Compiles the AnimBP after.",
  {
    anim_bp_path: z.string().describe("Content path of the AnimBlueprint"),
    node_id:      z.string().describe(
      "Node GUID — accepted in both 32-hex-no-hyphens form (the format ue5_anim_add_* tools return) and the canonical hyphenated form."),
    reconnect_pose: z.preprocess(jsonPreprocess, z.boolean().optional()).describe(
      "If true (default), reconnect upstream pose pin → downstream pose pin after deletion. Set false to leave the chain broken."),
  },
  async (args) => {
    try {
      const result = await ue5.animDeleteNode(args);
      return { content: [{ type: "text", text: JSON.stringify(result) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_skeleton_add_virtual_bone",
  "Add a virtual bone to a USkeleton. The virtual bone is parented to source_bone but tracks the position " +
  "of target_bone — useful for 'follow the weapon' rigs (parent=weapon_r, target=ik_hand_l → VB lives in " +
  "weapon space at the IK hand's position). Provide vb_name for a deterministic name.",
  {
    skeleton_path: z.string().describe("Content path of the USkeleton"),
    source_bone:   z.string().describe("Parent bone the virtual bone lives under, e.g. 'weapon_r'"),
    target_bone:   z.string().describe("Bone the virtual bone tracks, e.g. 'ik_hand_l'"),
    vb_name:       z.string().optional().describe(
      "Optional explicit name. If omitted, UE auto-names as 'VB_<source>_<target>'."),
  },
  async (args) => {
    try {
      const result = await ue5.skeletonAddVirtualBone(args);
      return { content: [{ type: "text", text: JSON.stringify(result) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_anim_add_two_bone_ik",
  "Insert a Two Bone IK skeletal control node into an AnimBlueprint's AnimGraph and splice it " +
  "between the current pose source and the Output Pose. Configures the bone, effector/joint location spaces, " +
  "and bTakeRotationFromEffectorSpace. EffectorLocation, JointTargetLocation, and Alpha pins are left exposed " +
  "for binding in the editor (right-click pin -> Bind, or promote to variable). Compiles the AnimBP. " +
  "Use this for two-handed weapon grips, foot IK, hand-on-target adjustments.",
  {
    anim_bp_path: z.string().describe(
      "Content path of the AnimBlueprint, e.g. '/Game/Characters/Mannequins/Anims/Unarmed/ABP_Unarmed'"),
    ik_bone: z.string().describe("Bone driven by the IK solver, e.g. 'hand_l' or 'foot_r'"),
    effector_location_space: z.enum([
      "WorldSpace", "ComponentSpace", "ParentBoneSpace", "BoneSpace",
    ]).optional().describe("Reference frame for the effector. Default WorldSpace."),
    joint_target_location_space: z.enum([
      "WorldSpace", "ComponentSpace", "ParentBoneSpace", "BoneSpace",
    ]).optional().describe("Reference frame for the joint target. Default ComponentSpace."),
    take_rotation_from_effector: z.preprocess(jsonPreprocess, z.boolean().optional()).describe(
      "If true, the end bone takes its rotation from the effector. Default true. " +
      "Tutorial: false for right hand, true for left hand."),
    effector_target_bone: z.string().optional().describe(
      "If supplied, sets EffectorTarget.BoneReference to this bone (e.g. 'ik_hand_r'). " +
      "Use with effector_location_space='BoneSpace' for bone-driven IK targeting (the tutorial pattern)."),
    joint_target_bone: z.string().optional().describe(
      "If supplied, sets JointTarget.BoneReference to this bone (e.g. 'lower_arm_r'). " +
      "Use with joint_target_location_space='BoneSpace'."),
    joint_target_offset: z.preprocess(jsonPreprocess, z.array(z.number()).length(3).optional())
      .describe("Per-axis offset [x,y,z] applied to JointTargetLocation. Tutorial uses [0,50,0]."),
    node_offset_x: z.number().optional().describe(
      "X offset (UU on the graph) from the Output Pose node. Default -250."),
    node_offset_y: z.number().optional().describe(
      "Y offset (UU on the graph) from the Output Pose node. Default 0."),
  },
  async ({ anim_bp_path, ik_bone, effector_location_space, joint_target_location_space,
           take_rotation_from_effector, effector_target_bone, joint_target_bone,
           joint_target_offset, node_offset_x, node_offset_y }) => {
    try {
      const result = await ue5.animAddTwoBoneIK({
        anim_bp_path, ik_bone,
        effector_location_space, joint_target_location_space,
        take_rotation_from_effector,
        effector_target_bone, joint_target_bone, joint_target_offset,
        node_offset_x, node_offset_y,
      });
      return { content: [{ type: "text", text: JSON.stringify(result) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_anim_add_layered_bone_blend",
  "Insert a Layered Bone Blend node into an AnimBP's AnimGraph and (by default) splice it before the " +
  "Output Pose, taking the existing pose source into Base Pose. Blend Pose 0 (and any extra blend poses) " +
  "are LEFT UNCONNECTED — feed them via ue5_bp_connect_pins (e.g. from a SequencePlayer added with " +
  "ue5_anim_add_sequence_player). Use this for the rifle-pose-over-locomotion pattern: locomotion goes " +
  "into Base Pose, the rifle idle SequencePlayer goes into Blend Pose 0 with a spine_03 branch filter.",
  {
    anim_bp_path:   z.string().describe(
      "Content path of the AnimBlueprint, e.g. '/Game/Characters/Mannequins/Anims/Unarmed/ABP_Unarmed'"),
    graph:          z.string().optional().describe("Graph name. Default 'AnimGraph'."),
    branch_filters: z.preprocess(jsonPreprocess, z.array(z.object({
      bone_name:   z.string(),
      blend_depth: z.number().int().optional(),
    })).optional()).describe(
      "One entry per blend pose (BranchFilter mode). Common: [{bone_name:'spine_03', blend_depth:1}]."),
    blend_weights:  z.preprocess(jsonPreprocess, z.array(z.number()).optional())
      .describe("One weight per blend pose. Default all 1.0."),
    blend_mode:     z.enum(["BranchFilter","BoneMask"]).optional()
      .describe("Default 'BranchFilter'. 'BoneMask' uses BlendMasks (blend profiles) instead of branch filters."),
    alpha:          z.number().optional().describe("Reserved (LayeredBoneBlend has no plain Alpha). Default 1.0."),
    splice_before_output: z.preprocess(jsonPreprocess, z.boolean().optional()).describe(
      "If true (default), splice the LBB between the Output Pose's current upstream and the Output Pose. " +
      "If false, just spawn the node and leave wiring to the caller."),
    node_offset_x:  z.number().optional().describe("X offset from Output Pose. Default -250."),
    node_offset_y:  z.number().optional().describe("Y offset from Output Pose. Default -250."),
  },
  async (args) => {
    try {
      const result = await ue5.animAddLayeredBoneBlend(args);
      return { content: [{ type: "text", text: JSON.stringify(result) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_anim_add_modify_bone",
  "Insert a Transform (Modify) Bone node (UAnimGraphNode_ModifyBone) into an AnimBP's AnimGraph and " +
  "configure its rotation / translation / scale mode + space. Optionally splices it before the Output Pose " +
  "so it modifies whatever pose chain is currently feeding the root. The Translation/Rotation/Scale pins " +
  "are left UNCONNECTED so the AnimBP can drive them from BlueprintThreadSafeUpdateAnimation each frame. " +
  "Use this for AnimBP-driven aim (rotate spine_03, RotationMode=Add, RotationSpace=ComponentSpace) — " +
  "the whole upper body follows the aim and the held weapon points naturally at the target.",
  {
    anim_bp_path:      z.string().describe(
      "Content path of the AnimBlueprint, e.g. '/Game/Characters/Mannequins/Anims/Unarmed/ABP_Unarmed'"),
    graph:             z.string().optional().describe("Graph name. Default 'AnimGraph'."),
    bone_name:         z.string().describe("Bone to modify, e.g. 'spine_03'."),
    translation_mode:  z.enum(["Ignore","Add","Replace"]).optional()
      .describe("How to apply Translation. Default 'Ignore'."),
    rotation_mode:     z.enum(["Ignore","Add","Replace"]).optional()
      .describe("How to apply Rotation. Default 'Add' (intended for spine-aim use case)."),
    scale_mode:        z.enum(["Ignore","Add","Replace"]).optional()
      .describe("How to apply Scale. Default 'Ignore'."),
    translation_space: z.enum(["WorldSpace","ComponentSpace","ParentBoneSpace","BoneSpace"]).optional()
      .describe("Reference frame for Translation. Default 'ComponentSpace'."),
    rotation_space:    z.enum(["WorldSpace","ComponentSpace","ParentBoneSpace","BoneSpace"]).optional()
      .describe("Reference frame for Rotation. Default 'ComponentSpace'."),
    scale_space:       z.enum(["WorldSpace","ComponentSpace","ParentBoneSpace","BoneSpace"]).optional()
      .describe("Reference frame for Scale. Default 'ComponentSpace'."),
    translation:       z.preprocess(jsonPreprocess, z.array(z.number()).length(3).optional())
      .describe("Default [x,y,z] baked into the node (overridden when the Translation pin is wired). Default [0,0,0]."),
    rotation:          z.preprocess(jsonPreprocess, z.array(z.number()).length(3).optional())
      .describe("Default [pitch,yaw,roll] baked into the node. Default [0,0,0]."),
    scale:             z.preprocess(jsonPreprocess, z.array(z.number()).length(3).optional())
      .describe("Default [x,y,z] scale. Default [1,1,1]."),
    alpha:             z.number().optional().describe("Blend alpha (0=node has no effect, 1=full). Default 1.0."),
    splice_before_output: z.preprocess(jsonPreprocess, z.boolean().optional()).describe(
      "If true, splice the ModifyBone between the Output Pose's current upstream and the Output Pose. " +
      "If false (default), spawn the node and leave wiring to the caller."),
    node_offset_x:     z.number().optional().describe("X offset from Output Pose. Default -250."),
    node_offset_y:     z.number().optional().describe("Y offset from Output Pose. Default 0."),
  },
  async (args) => {
    try {
      const result = await ue5.animAddModifyBone(args);
      return { content: [{ type: "text", text: JSON.stringify(result) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_anim_add_look_at",
  "Insert a LookAt skeletal control node (UAnimGraphNode_LookAt) into an AnimBP's AnimGraph and " +
  "(optionally) splice it before the Output Pose. LookAt solves: given a bone, a local axis, and a " +
  "world-space target, compute the rotation that aligns that axis through the target — correctly " +
  "accounting for the spatial offset between the controlled bone (e.g. spine_03) and the gun. " +
  "Use this for upper-body aim where a manual ModifyBone(spine, rotation=Aim) would point the spine " +
  "at the target but NOT the gun. The LookAtLocation pin is left UNCONNECTED so the AnimBP can drive " +
  "it from BlueprintThreadSafeUpdateAnimation each frame (e.g. read GetAimWorldTarget() into an AnimBP " +
  "FVector variable, then ue5_bp_connect_pins it to LookAtLocation). Compiles the AnimBP.",
  {
    anim_bp_path:           z.string().describe(
      "Content path of the AnimBlueprint, e.g. '/Game/Characters/Mannequins/Anims/Unarmed/ABP_Unarmed'"),
    graph:                  z.string().optional().describe("Graph name. Default 'AnimGraph'."),
    bone_to_modify:         z.string().describe(
      "Bone the LookAt rotates, e.g. 'spine_03' (chest — closest to the arms+gun, gives the most accurate pointing geometry)."),
    look_at_axis:           z.preprocess(jsonPreprocess, z.array(z.number()).length(3).optional())
      .describe(
        "Local axis on bone_to_modify that should be aligned through the target. Default [1,0,0]. " +
        "UE5 mannequin spine forward axis is typically [0,1,0] (Y) — pass that explicitly when known. " +
        "If the gun ends up pointing 90° off, flip to a different axis."),
    look_at_axis_local:     z.preprocess(jsonPreprocess, z.boolean().optional())
      .describe("If true (default), look_at_axis is in the bone's local space; if false, world space."),
    look_at_target_bone:    z.string().optional().describe(
      "Optional — if set, LookAt follows that bone's transform instead of LookAtLocation."),
    look_at_socket:         z.string().optional().describe(
      "Optional — socket on the skeleton to follow. Wins over look_at_target_bone if both are set."),
    look_at_location:       z.preprocess(jsonPreprocess, z.array(z.number()).length(3).optional())
      .describe(
        "World-space target location (when no target bone/socket). Most callers leave this at [0,0,0] " +
        "and bind the LookAtLocation pin via ue5_bp_connect_pins to a Get of an AnimBP FVector variable."),
    look_at_location_space: z.enum(["WorldSpace","ComponentSpace","ParentBoneSpace","BoneSpace"]).optional()
      .describe("Reference frame for look_at_location. Default 'WorldSpace'. Currently informational; FAnimNode_LookAt has no explicit space enum."),
    look_at_clamp:          z.number().optional().describe(
      "Look-at clamp in degrees. 0 = no clamp (full range); >0 caps the rotation. Default 0."),
    interpolation_type:     z.enum(["None","Linear","Cubic","Sinusoidal","EaseInOut","EaseInOutExponent2","EaseInOutExponent3","EaseInOutExponent4","EaseInOutExponent5"]).optional()
      .describe("Interpolation curve for the smoothed alpha. Default 'Linear'. ('None' is treated as Linear with interpolation_time=0.)"),
    interpolation_time:     z.number().optional().describe(
      "Smoothing time in seconds. Default 0.1. 0 = snap (no interpolation)."),
    alpha:                  z.number().optional().describe("Skeletal control blend alpha (0=no effect, 1=full). Default 1.0."),
    splice_before_output:   z.preprocess(jsonPreprocess, z.boolean().optional()).describe(
      "If true (default), splice the LookAt between the Output Pose's current upstream and the Output Pose. " +
      "If false, just spawn the node and leave wiring to the caller."),
    node_offset_x:          z.number().optional().describe("X offset from Output Pose. Default -250."),
    node_offset_y:          z.number().optional().describe("Y offset from Output Pose. Default 0."),
  },
  async (args) => {
    try {
      const result = await ue5.animAddLookAt(args);
      return { content: [{ type: "text", text: JSON.stringify(result) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_anim_add_sequence_player",
  "Spawn a Sequence Player node in an AnimBP's AnimGraph, configured to play a specific UAnimSequence. " +
  "By default this does NOT splice — it's a leaf node the caller wires up later via ue5_bp_connect_pins " +
  "(typically into a Layered Bone Blend's Blend Pose input). Compiles the AnimBP.",
  {
    anim_bp_path:   z.string().describe(
      "Content path of the AnimBlueprint, e.g. '/Game/Characters/Mannequins/Anims/Unarmed/ABP_Unarmed'"),
    graph:          z.string().optional().describe("Graph name. Default 'AnimGraph'."),
    sequence:       z.string().describe(
      "Hard reference to a UAnimSequence asset, e.g. '/Game/Anims/MM_Rifle_Idle.MM_Rifle_Idle'"),
    loop:           z.preprocess(jsonPreprocess, z.boolean().optional()).describe("Default true."),
    play_rate:      z.number().optional().describe("Default 1.0."),
    start_position: z.number().optional().describe("Default 0.0 (seconds)."),
    node_offset_x:  z.number().optional().describe("X offset from Output Pose. Default -500."),
    node_offset_y:  z.number().optional().describe("Y offset from Output Pose. Default -250."),
  },
  async (args) => {
    try {
      const result = await ue5.animAddSequencePlayer(args);
      return { content: [{ type: "text", text: JSON.stringify(result) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_anim_add_aim_offset_blend_space",
  "Spawn an AimOffset Player node (UAnimGraphNode_RotationOffsetBlendSpace) in an AnimBP's AnimGraph " +
  "and (by default) splice it before the Output Pose so it applies an authored aim-offset blend space " +
  "(UAimOffsetBlendSpace / UAimOffsetBlendSpace1D) on top of whatever pose currently feeds the root. " +
  "This is the proper UE5 shooter-pattern aim system — the blend space supplies pre-authored " +
  "'aim up / center / down' (and optionally left/right) per-bone deltas, so the AnimBP only has to " +
  "feed normalized pitch/yaw floats. Avoids the per-frame FRotator hacks that hand-rolled " +
  "ModifyBone(spine_03) approaches need. The X / Y / Alpha pins are LEFT UNCONNECTED so the AnimBP " +
  "can drive them from BlueprintThreadSafeUpdateAnimation each frame — wire them with " +
  "ue5_bp_connect_pins. For a 1D Y-only blend space (e.g. AO_Rifle), only Y matters; X (and Z if " +
  "exposed) can stay at default. Compiles the AnimBP.",
  {
    anim_bp_path:           z.string().describe(
      "Content path of the AnimBlueprint, e.g. '/Game/Characters/Mannequins/Anims/Unarmed/ABP_Unarmed'"),
    graph:                  z.string().optional().describe("Graph name. Default 'AnimGraph'."),
    blend_space:            z.string().describe(
      "Hard reference to a UAimOffsetBlendSpace or UAimOffsetBlendSpace1D asset, " +
      "e.g. '/Game/Characters/Mannequins/Anims/Rifle/AIM/AO_Rifle.AO_Rifle'"),
    alpha:                  z.number().optional().describe(
      "Baked default for the Alpha pin (0=AimOffset has no effect, 1=full). The pin can override at runtime. Default 1.0."),
    splice_before_output:   z.preprocess(jsonPreprocess, z.boolean().optional()).describe(
      "If true (default), splice the AimOffset between the Output Pose's current upstream and the Output Pose, " +
      "so existing locomotion/LayeredBoneBlend feeds into BasePose. If false, just spawn the node and leave wiring to the caller."),
    node_offset_x:          z.number().optional().describe("X offset from Output Pose. Default -250."),
    node_offset_y:          z.number().optional().describe("Y offset from Output Pose. Default 0."),
  },
  async (args) => {
    try {
      const result = await ue5.animAddAimOffsetBlendSpace(args);
      return { content: [{ type: "text", text: JSON.stringify(result) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_mesh_add_socket",
  "Add a socket to a UStaticMesh, USkeletalMesh, or USkeleton asset. " +
  "Asset class is auto-detected from mesh_path; pass 'target' to disambiguate. " +
  "For skeletal/skeleton sockets, 'bone_name' is required and validated against the skeleton. " +
  "Pass replace=true to overwrite an existing socket of the same name.",
  {
    mesh_path:   z.string().describe(
      "Content path of the asset, e.g. '/Game/Meshes/SM_Box.SM_Box' or '/Game/Characters/SK_Hero'"),
    socket_name: z.string().describe("Socket name to create (FName)"),
    bone_name:   z.string().optional().describe(
      "Bone the socket attaches to. Required for SkeletalMesh and Skeleton sockets, ignored for StaticMesh."),
    location: z.preprocess(jsonPreprocess, tuple3.optional())
      .describe("Relative location [x,y,z] (UU). Default [0,0,0]."),
    rotation: z.preprocess(jsonPreprocess, tuple3.optional())
      .describe("Relative rotation [pitch,yaw,roll] (degrees). Default [0,0,0]."),
    scale: z.preprocess(jsonPreprocess, tuple3.optional())
      .describe("Relative scale [x,y,z]. Default [1,1,1]."),
    replace: z.boolean().optional().describe(
      "If true, overwrite an existing socket of the same name. Default false (returns 409 on conflict)."),
    target: z.enum(["auto", "static_mesh", "skeletal_mesh", "skeleton"]).optional()
      .describe("Override target asset class. Default 'auto' (uses the asset's actual class)."),
  },
  async ({ mesh_path, socket_name, bone_name, location, rotation, scale, replace, target }) => {
    try {
      const result = await ue5.meshAddSocket({
        mesh_path, socket_name, bone_name, location, rotation, scale, replace, target,
      });
      return { content: [{ type: "text", text: JSON.stringify(result) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: youtube_get_transcript — fetch captions/subtitles from a YouTube URL
// ---------------------------------------------------------------------------
// Uses the unofficial timed-text endpoint via the youtube-transcript npm package.
// No API key, no OAuth. Auto-generated captions are returned when human captions
// are absent. Caveats:
//   * YouTube can change the endpoint at any time and break this package.
//   * Channels can disable captions entirely → tool returns an error.
//   * Server IPs (cloud) are sometimes rate-limited; local dev usually fine.
//   * Auto-captions have transcription errors (e.g. "lowerarm" → "lower arm").

server.tool(
  "youtube_get_transcript",
  "Fetch the transcript / captions of a YouTube video. Returns the full plain text plus " +
  "timestamped segments. Use this to read tutorials and apply their steps without asking the user " +
  "to copy-paste. Caveats: auto-generated captions can have transcription errors; some channels " +
  "disable captions; YouTube may rate-limit if called repeatedly in quick succession.",
  {
    url:  z.string().describe("YouTube URL (full https://...) or bare video ID"),
    lang: z.string().optional().describe(
      "ISO language code, e.g. 'en', 'es', 'fr'. If omitted, the package picks the first available track."),
  },
  async ({ url, lang }) => {
    try {
      const opts = lang ? { lang } : {};
      const segments = await YoutubeTranscript.fetchTranscript(url, opts);
      if (!segments || segments.length === 0) {
        return {
          content: [{ type: "text", text: JSON.stringify({
            ok: false,
            error: "No transcript segments returned (captions may be disabled on this video)",
            url,
          }) }],
          isError: true,
        };
      }
      // Normalize segments — youtube-transcript returns { text, duration, offset } where
      // offset/duration are in milliseconds. Project them to a friendlier shape.
      const normalized = segments.map(s => ({
        start: Math.round((s.offset ?? 0)),
        dur:   Math.round((s.duration ?? 0)),
        text:  (s.text ?? "").replace(/&#39;/g, "'").replace(/&amp;/g, "&"),
      }));
      const plain_text = normalized.map(s => s.text).join(" ");
      return { content: [{ type: "text", text: JSON.stringify({
        ok: true,
        url,
        language: lang ?? "auto",
        segment_count: normalized.length,
        plain_text,
        segments: normalized,
      }) }] };
    } catch (err) {
      return {
        content: [{ type: "text", text: JSON.stringify({
          ok: false,
          error: err.message,
          hint: "Common causes: invalid URL, captions disabled, video unavailable, or YouTube " +
                "changed the endpoint (the youtube-transcript package may need updating).",
          url,
        }) }],
        isError: true,
      };
    }
  }
);

// ---------------------------------------------------------------------------
// Tool: ue5_meshy_generate — Meshy.ai text-to-3D → import into UE5
// ---------------------------------------------------------------------------

server.tool(
  "ue5_meshy_generate",
  "Generate a 3D mesh from a text prompt via Meshy.ai v2 (preview, optionally refine), " +
  "save it to RawAssets/Meshy/<asset_name>.<ext>, and import it into UE5 as a Static Mesh. " +
  "API key comes from env MESHY_API_KEY or <project_root>/.meshy.json ({\"api_key\":\"msy-...\"}). " +
  "Preview takes 1-3 min; refine adds 2-5 min.",
  {
    prompt:          z.string().describe("Text prompt describing the model"),
    art_style:       z.enum(["realistic", "sculpture"]).optional()
                       .describe("Meshy art style (default 'realistic')"),
    negative_prompt: z.string().optional()
                       .describe("Things to avoid in the generated mesh"),
    asset_name:      z.string().describe("Name for the local file and imported Static Mesh"),
    dest_path:       z.string().optional()
                       .describe("UE5 content folder to import into (default '/Game/Generated')"),
    refine:          z.boolean().optional()
                       .describe("If true, run the refine phase after preview (extra cost/time)"),
  },
  async ({ prompt, art_style, negative_prompt, asset_name, dest_path, refine }) => {
    try {
      const uproject = findUProject();
      if (!uproject) {
        throw new Error("Could not locate .uproject — run from inside the UE5 project tree.");
      }
      const projectRoot = path.dirname(uproject);

      const { key: apiKey, source: keySource } = meshyResolveApiKey(projectRoot);

      const safeName   = asset_name.replace(/[^A-Za-z0-9_\-]/g, "_");
      const meshyDir   = path.join(projectRoot, "RawAssets", "Meshy");
      const destNoExt  = path.join(meshyDir, safeName);
      const destFolder = dest_path ?? "/Game/Generated";

      const statusLog = [];
      const onStatus  = (phase, status) => statusLog.push(`${phase}:${status}`);

      const gen = await meshyGenerate({
        apiKey,
        prompt,
        art_style:       art_style ?? "realistic",
        negative_prompt,
        refine:          !!refine,
        destFileNoExt:   destNoExt,
        onStatus,
      });

      const importResult = await ue5.importAsset(gen.file, destFolder, safeName);

      const payload = {
        ok:          true,
        key_source:  keySource,
        task_id:     gen.final_task_id,
        preview_task_id: gen.preview_task_id,
        ...(gen.refine_task_id ? { refine_task_id: gen.refine_task_id } : {}),
        asset_path:  importResult?.asset_path ?? `${destFolder}/${safeName}`,
        local_file:  gen.file,
        ext:         gen.ext,
        bytes:       gen.bytes,
        ...(importResult?.tris != null ? { tris: importResult.tris } : {}),
        status_log:  statusLog,
      };
      return { content: [{ type: "text", text: JSON.stringify(payload, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Blueprint graph authoring — helpers
// ---------------------------------------------------------------------------

// Silently fix the two most common model mistakes before they hit the bridge:
//
//   1. _FloatFloat → _DoubleDouble  (UE5.0+ renamed all KismetMathLibrary
//      float operators; the model's training data still contains the old names)
//
//   2. EventTick / EventBeginPlay / etc. → ReceiveTick / ReceiveBeginPlay
//      (model uses Blueprint display-name prefix "Event" instead of the
//      internal "Receive" prefix that the bridge requires for override events)
//
// Both transformations are safe: no legitimate call_function uses _FloatFloat,
// and no override event uses the "Event" UFunction prefix internally.
function normalizeNodes(nodes) {
  if (!Array.isArray(nodes)) return nodes;
  return nodes.map(node => {
    if (!node || typeof node !== "object") return node;
    const out = { ...node };

    // 1. _FloatFloat → _DoubleDouble in call_function names
    if (typeof out.function === "string" && out.function.includes("_FloatFloat")) {
      out.function = out.function.replace(/_FloatFloat/g, "_DoubleDouble");
    }

    // 2. EventX → ReceiveX for override event nodes only
    if (out.type === "event") {
      for (const key of ["event", "event_name"]) {
        if (typeof out[key] === "string" && out[key].startsWith("Event")) {
          out[key] = "Receive" + out[key].slice(5);
        }
      }
    }

    return out;
  });
}

// ---------------------------------------------------------------------------
// Blueprint graph authoring — create & wire K2 nodes in Event Graphs
// ---------------------------------------------------------------------------
// PRIMARY entry point for Blueprint authoring is ue5_bp_add_logic — it takes a
// high-level nodes[]+connections[] spec, creates everything, auto-lays out, and
// optionally compiles. Use the other tools (add_node / connect_pins / compile /
// read_graph) as escape hatches when add_logic's vocabulary is insufficient.
// ---------------------------------------------------------------------------

server.tool(
  "ue5_create_blueprint_struct",
  [
    "Create a Blueprint Struct (UUserDefinedStruct, conventionally named S_Foo) with a list of fields.",
    "Each field has a name and a type (same type strings as ue5_bp_create_variable: bool, int32, float, FString, FVector, FLinearColor, MaterialInstanceDynamic, /Script/Engine.Actor, etc.).",
    "Example: {\"asset_path\":\"/Game/test/S_PlayerInfo\",\"fields\":[{\"name\":\"PlayerName\",\"type\":\"FString\"},{\"name\":\"Score\",\"type\":\"int32\"},{\"name\":\"Position\",\"type\":\"FVector\"}]}",
  ].join(" "),
  {
    asset_path: z.string().describe("Destination content path, e.g. '/Game/Data/S_PlayerInfo'"),
    fields:     z.preprocess(jsonPreprocess, z.array(z.any())).describe("Array of { name, type, default_value? } field specs"),
    save:       z.boolean().optional().describe("Save package after creation (default true)"),
  },
  async ({ asset_path, fields, save }) => {
    try {
      const r = await ue5.createBlueprintStruct({ asset_path, fields, save });
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_create_blueprint_enum",
  [
    "Create a Blueprint Enum (UUserDefinedEnum, conventionally named E_Foo) with a list of entries.",
    "Each entry has a name (required) and optional display_name + tooltip.",
    "Example: {\"asset_path\":\"/Game/test/E_GameState\",\"entries\":[{\"name\":\"MainMenu\",\"display_name\":\"Main Menu\"},{\"name\":\"Playing\"},{\"name\":\"Paused\"}]}",
  ].join(" "),
  {
    asset_path: z.string().describe("Destination content path, e.g. '/Game/Data/E_GameState'"),
    entries:    z.preprocess(jsonPreprocess, z.array(z.any())).describe("Array of { name, display_name?, tooltip? } enum entries"),
    save:       z.boolean().optional().describe("Save package after creation (default true)"),
  },
  async ({ asset_path, entries, save }) => {
    try {
      const r = await ue5.createBlueprintEnum({ asset_path, entries, save });
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_create_blueprint_interface",
  [
    "Create a Blueprint Interface (BPTYPE_Interface, conventionally named BPI_Foo) with a list of function signatures.",
    "Each function has a name and optional inputs/outputs arrays of { name, type } pin specs.",
    "Example: {\"asset_path\":\"/Game/test/BPI_Damage\",\"functions\":[{\"name\":\"OnHit\",\"inputs\":[{\"name\":\"Damage\",\"type\":\"float\"}]}]}",
  ].join(" "),
  {
    asset_path: z.string().describe("Destination content path, e.g. '/Game/Interfaces/BPI_Damage'"),
    functions:  z.preprocess(jsonPreprocess, z.array(z.any())).optional().describe("Optional array of { name, inputs?, outputs? } function signatures"),
    save:       z.boolean().optional().describe("Save package after creation (default true)"),
  },
  async ({ asset_path, functions, save }) => {
    try {
      const r = await ue5.createBlueprintInterface({ asset_path, functions, save });
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_create_anim_blueprint",
  [
    "Create an Animation Blueprint (UAnimBlueprint, conventionally named ABP_Foo) for a target skeleton.",
    "Pair with ue5_configure_anim_blueprint to wire state machines and transitions.",
    "Example: {\"asset_path\":\"/Game/Animations/ABP_Hero\",\"target_skeleton\":\"/Game/Characters/Hero/SK_Hero_Skeleton\"}",
  ].join(" "),
  {
    asset_path:      z.string().describe("Destination content path, e.g. '/Game/Animations/ABP_Hero'"),
    target_skeleton: z.string().describe("Content path of the USkeleton this AnimBP animates"),
    parent_class:    z.string().optional().describe("Parent class (default '/Script/Engine.AnimInstance')"),
    save:            z.boolean().optional().describe("Save package after creation (default true)"),
  },
  async ({ asset_path, target_skeleton, parent_class, save }) => {
    try {
      const r = await ue5.createAnimBlueprint({ asset_path, target_skeleton, parent_class, save });
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Behavior Tree authoring (Phase 1 — asset creation)
// ---------------------------------------------------------------------------
// AI logic in UE5 lives across two assets: a UBlackboardData (typed key/value
// store) and a UBehaviorTree (the decision graph). Phase 1 covers creating
// those assets and populating the Blackboard with keys. The BT graph itself
// (composites/tasks/decorators/services + parent-child wiring) ships in
// Phase 2 as ue5_bt_add_logic. Custom task / decorator / service classes can
// already be authored today via ue5_create_blueprint with parent_class set to
// "BTTask_BlueprintBase" / "BTDecorator_BlueprintBase" / "BTService_BlueprintBase".
// ---------------------------------------------------------------------------

server.tool(
  "ue5_bt_create_tree",
  [
    "Create a UBehaviorTree asset (conventionally named BT_Foo). Optionally link a UBlackboardData via blackboard_path so the BT root has its 'Blackboard Asset' set.",
    "Idempotent — if the BT already exists, returns success with already_existed=true and only updates the blackboard link if blackboard_path is provided.",
    "Example: {\"asset_path\":\"/Game/AI/BT_Enemy\",\"blackboard_path\":\"/Game/AI/BB_Enemy\"}",
  ].join(" "),
  {
    asset_path:      z.string().describe("Destination content path, e.g. '/Game/AI/BT_Enemy'"),
    blackboard_path: z.string().optional().describe("Optional UBlackboardData asset path to link as the tree's BlackboardAsset"),
    save:            z.boolean().optional().describe("Save package after creation (default true)"),
  },
  async ({ asset_path, blackboard_path, save }) => {
    try {
      const r = await ue5.btCreateTree({ asset_path, blackboard_path, save });
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_bt_create_blackboard",
  [
    "Create a UBlackboardData asset (conventionally named BB_Foo). Optionally inherit keys from another blackboard via parent_blackboard_path.",
    "Created empty by default — pair with ue5_bt_add_blackboard_keys to populate.",
    "Example: {\"asset_path\":\"/Game/AI/BB_Enemy\"}",
  ].join(" "),
  {
    asset_path:             z.string().describe("Destination content path, e.g. '/Game/AI/BB_Enemy'"),
    parent_blackboard_path: z.string().optional().describe("Optional parent UBlackboardData to inherit keys from"),
    save:                   z.boolean().optional().describe("Save package after creation (default true)"),
  },
  async ({ asset_path, parent_blackboard_path, save }) => {
    try {
      const r = await ue5.btCreateBlackboard({ asset_path, parent_blackboard_path, save });
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_bt_add_blackboard_keys",
  [
    "Append typed entries to an existing UBlackboardData. Skips entries whose name is already taken (idempotent — safe to re-run).",
    "Each key has { name, type, description?, instance_synced?, base_class? (Object/Class only), enum_path? (Enum only) }.",
    "Supported types: Bool, Int, Float, Vector, Rotator, Object, Class, Enum, Name, String. Object/Class entries can pass base_class as a class ref ('Actor', '/Script/Engine.Pawn', '/Game/.../BP_Foo'). Enum entries need enum_path pointing at a UEnum (e.g. '/Game/Data/E_State').",
    "Example: {\"asset_path\":\"/Game/AI/BB_Enemy\",\"keys\":[{\"name\":\"TargetActor\",\"type\":\"Object\",\"base_class\":\"Pawn\"},{\"name\":\"PatrolPoint\",\"type\":\"Vector\"},{\"name\":\"IsAlerted\",\"type\":\"Bool\"}]}",
  ].join(" "),
  {
    asset_path: z.string().describe("UBlackboardData content path"),
    keys:       z.preprocess(jsonPreprocess, z.array(z.any())).describe("Array of key specs — see description for fields"),
    save:       z.boolean().optional().describe("Save package after mutation (default true)"),
  },
  async ({ asset_path, keys, save }) => {
    try {
      const r = await ue5.btAddBlackboardKeys({ asset_path, keys, save });
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_bt_add_logic",
  [
    "PRIMARY Behavior Tree authoring tool. Wire a complete tree — composites, tasks, decorators, services, parent/child connections — in one call.",
    "Node types: 'composite' (with composite: 'Selector'|'Sequence'|'SimpleParallel') or 'task' (with task_class, e.g. 'BTTask_Wait', 'BTTask_MoveTo', or '/Game/AI/BTT_MyTask' for BP-derived).",
    "Decorators and services are sub-nodes attached to a parent: { parent: '<node_id>', decorator_class|service_class, blackboard_key?, properties? }. Their classes resolve from /Script/AIModule.* short names or full paths.",
    "Connections form parent→child pin links. The reserved id 'root' refers to the BT's auto-created root. Sibling order = order of appearance in connections[] (first connected = leftmost = first to execute).",
    "Per-node 'properties' is a {name:value} dict applied via UPROPERTY reflection. 'blackboard_key' is shorthand: sets FBlackboardKeySelector::SelectedKeyName on any *_BlackboardBase node and resolves it against the BT's linked blackboard.",
    "Pass clear:true to wipe any existing non-Root nodes first; otherwise the call refuses if the tree already has authored content (409).",
    "Example: a 3-leaf Selector under root with a Blackboard decorator: {\"behavior_tree\":\"/Game/AI/BT_Patrol\",\"nodes\":[{\"id\":\"sel\",\"type\":\"composite\",\"composite\":\"Selector\"},{\"id\":\"chase\",\"type\":\"task\",\"task_class\":\"BTTask_MoveTo\",\"blackboard_key\":\"TargetActor\"},{\"id\":\"wait\",\"type\":\"task\",\"task_class\":\"BTTask_Wait\",\"properties\":{\"WaitTime\":2}}],\"decorators\":[{\"parent\":\"chase\",\"decorator_class\":\"BTDecorator_Blackboard\",\"blackboard_key\":\"IsAlerted\"}],\"connections\":[{\"from\":\"root\",\"to\":\"sel\"},{\"from\":\"sel\",\"to\":\"chase\"},{\"from\":\"sel\",\"to\":\"wait\"}]}",
  ].join(" "),
  {
    behavior_tree: z.string().describe("Full UBehaviorTree content path, e.g. '/Game/AI/BT_Patrol'"),
    nodes:         z.preprocess(jsonPreprocess, z.array(z.any())).describe("Array of { id, type:'composite'|'task', composite?|task_class?, blackboard_key?, properties? } specs"),
    decorators:    z.preprocess(jsonPreprocess, z.array(z.any())).optional().describe("Array of { parent, decorator_class, blackboard_key?, properties? } sub-nodes"),
    services:      z.preprocess(jsonPreprocess, z.array(z.any())).optional().describe("Array of { parent, service_class, blackboard_key?, properties? } sub-nodes"),
    connections:   z.preprocess(jsonPreprocess, z.array(z.any())).optional().describe("Array of { from, to } parent→child links — use from:'root' for the BT root"),
    clear:         z.boolean().optional().describe("If true, delete any non-Root nodes before authoring (default false → refuse if tree non-empty)"),
    compile:       z.boolean().optional().describe("Run UBehaviorTreeGraph::UpdateAsset to mirror EdGraph → runtime tree (default true)"),
    save:          z.boolean().optional().describe("Save package after compile (default true)"),
  },
  async ({ behavior_tree, nodes, decorators, services, connections, clear, compile, save }) => {
    try {
      const r = await ue5.btAddLogic({ behavior_tree, nodes, decorators, services, connections, clear, compile, save });
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_bt_read_tree",
  [
    "Read a UBehaviorTree's EdGraph as JSON in the same shape ue5_bt_add_logic accepts (round-trip read → edit → write).",
    "Returns nodes[] (composites + tasks), decorators[], services[], and connections[] (parent→child, sorted by NodePosX = execution order). Sub-node ordering within a parent is the array order on the parent.",
    "The reserved id 'root' is used in connections[] for the BT's auto-created root node and is NOT included in nodes[].",
    "Example: ue5_bt_read_tree {\"behavior_tree\":\"/Game/AI/BT_Patrol\"}",
  ].join(" "),
  {
    behavior_tree: z.string().describe("Full UBehaviorTree content path"),
  },
  async ({ behavior_tree }) => {
    try {
      const r = await ue5.btReadTree({ behavior_tree });
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_st_read_tree",
  [
    "Read a UStateTree's authoring hierarchy as JSON: states (name/type/id/enabled) each with enter_conditions[], tasks[], transitions[] (trigger/to/link_type) and nested children[].",
    "Every task / enter-condition / evaluator node emits its display name, node_struct (e.g. StateTreeBlueprintTaskWrapper), and the wrapped class path — so you can inspect a StateTree (otherwise an opaque binary) and verify which C++/BP class each node uses.",
    "Example: ue5_st_read_tree {\"state_tree\":\"/Game/Blueprints/AI/StateTree/ST_NPC_SandboxCharacter_SmartObject\"}",
  ].join(" "),
  {
    state_tree: z.string().describe("Full UStateTree content path"),
  },
  async ({ state_tree }) => {
    try {
      const r = await ue5.stateTreeReadTree({ state_tree });
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_st_repoint_node",
  [
    "Repoint StateTree nodes from one wrapped class to another: every task / enter-condition / evaluator node whose class matches from_class is rewritten to to_class, with the node's instance properties migrated by name (e.g. CooldownName survives).",
    "This is the clean way to swap a BP StateTree task/condition for its C++ port — unlike a CoreRedirect it actually rewrites the stored reference, so there is no perpetual stale-package warning. from_class accepts a full class path or a bare name (with or without the BP _C suffix); to_class is a full class path (/Script/Module.CppClass or /Game/.../BP_Name.BP_Name_C).",
    "Defaults: compile=true, save=false. Pass dry_run=true to list the nodes that WOULD be repointed without mutating. After a real run, verify with ue5_st_read_tree.",
    "Example: ue5_st_repoint_node {\"state_tree\":\"/Game/Blueprints/AI/StateTree/ST_NPC_SandboxCharacter_SmartObject\",\"from_class\":\"STC_CheckCooldown_C\",\"to_class\":\"/Script/GameAnimationSample.GameAnimCheckCooldownCondition\",\"save\":true}",
  ].join(" "),
  {
    state_tree: z.string().describe("Full UStateTree content path"),
    from_class: z.string().describe("Current node class — full path or bare name (with/without the BP _C suffix)"),
    to_class: z.string().optional().describe("Target class full path (/Script/Module.CppClass or /Game/.../BP_Name.BP_Name_C). Optional only when dry_run=true"),
    compile: z.boolean().optional().describe("Recompile the StateTree after repointing (default true)"),
    save: z.boolean().optional().describe("Save the StateTree package after repointing (default false)"),
    dry_run: z.boolean().optional().describe("Report matching nodes without mutating (default false)"),
  },
  async ({ state_tree, from_class, to_class, compile, save, dry_run }) => {
    try {
      const r = await ue5.stateTreeRepointNode({ state_tree, from_class, to_class, compile, save, dry_run });
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_bp_create_function",
  [
    "Add a custom function graph to an existing Blueprint with typed inputs/outputs.",
    "Use the returned graph_name as the 'graph' field in ue5_bp_add_logic to populate the function body.",
    "Example: {\"blueprint\":\"/Game/test/BP_test\",\"name\":\"DoSomething\",\"inputs\":[{\"name\":\"Amount\",\"type\":\"float\"}],\"outputs\":[{\"name\":\"Success\",\"type\":\"bool\"}]}",
  ].join(" "),
  {
    blueprint: z.string().describe("Full blueprint content path, e.g. '/Game/Blueprints/BP_Foo'"),
    name:      z.string().describe("Function name (must be a valid identifier, unique within the BP)"),
    inputs:    z.preprocess(jsonPreprocess, z.array(z.any())).optional().describe("Array of { name, type } input pins"),
    outputs:   z.preprocess(jsonPreprocess, z.array(z.any())).optional().describe("Array of { name, type } output pins"),
    is_pure:   z.boolean().optional().describe("Mark as BlueprintPure (no exec pins)"),
    is_const:  z.boolean().optional().describe("Mark as const (no state mutation)"),
    category:  z.string().optional().describe("Variables panel category"),
    compile:   z.boolean().optional().describe("Compile after adding (default true)"),
    save:      z.boolean().optional().describe("Save after successful compile (default true)"),
  },
  async ({ blueprint, name, inputs, outputs, is_pure, is_const, category, compile, save }) => {
    try {
      const r = await ue5.bpCreateFunction({ blueprint, name, inputs, outputs, is_pure, is_const, category, compile, save });
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_bp_create_macro",
  [
    "Add a macro graph to an existing Blueprint with typed inputs/outputs.",
    "Macros differ from functions in that they're inlined at compile time and can have multiple exec inputs/outputs.",
    "Example: {\"blueprint\":\"/Game/test/BP_test\",\"name\":\"DoubleCheck\",\"inputs\":[{\"name\":\"In\",\"type\":\"int32\"}],\"outputs\":[{\"name\":\"Out\",\"type\":\"int32\"}]}",
  ].join(" "),
  {
    blueprint: z.string().describe("Full blueprint content path"),
    name:      z.string().describe("Macro name (must be a valid identifier, unique within the BP)"),
    inputs:    z.preprocess(jsonPreprocess, z.array(z.any())).optional().describe("Array of { name, type } input pins"),
    outputs:   z.preprocess(jsonPreprocess, z.array(z.any())).optional().describe("Array of { name, type } output pins"),
    category:  z.string().optional(),
    compile:   z.boolean().optional(),
    save:      z.boolean().optional(),
  },
  async ({ blueprint, name, inputs, outputs, category, compile, save }) => {
    try {
      const r = await ue5.bpCreateMacro({ blueprint, name, inputs, outputs, category, compile, save });
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_bp_delete_function",
  [
    "Remove a user-created function graph from a Blueprint (inverse of ue5_bp_create_function).",
    "Idempotent: if no function with that name exists, returns success with already_absent=true.",
    "Use after reparenting a BP onto a C++ class to clear a BP function whose name now collides with an inherited C++ function.",
    "Example: {\"blueprint\":\"/Game/Audio/Foley/AC_FoleyEvents\",\"name\":\"PlayFoleyEvent\"}",
  ].join(" "),
  {
    blueprint: z.string().describe("Full blueprint content path, e.g. '/Game/Blueprints/BP_Foo'"),
    name:      z.string().describe("Function name to remove"),
    compile:   z.boolean().optional().describe("Compile after removal (default true)"),
    save:      z.boolean().optional().describe("Save after successful compile (default true)"),
  },
  async ({ blueprint, name, compile, save }) => {
    try {
      const r = await ue5.bpDeleteFunction({ blueprint, name, compile, save });
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_bp_delete_variable",
  [
    "Remove a member variable from a Blueprint (inverse of ue5_bp_create_variable).",
    "Idempotent: if no variable with that name exists, returns success with already_absent=true.",
    "Use after reparenting a BP onto a C++ class to clear a BP variable whose name now collides with an inherited C++ UPROPERTY.",
    "Example: {\"blueprint\":\"/Game/Audio/Foley/AC_FoleyEvents\",\"name\":\"FoleyEventBank\"}",
  ].join(" "),
  {
    blueprint: z.string().describe("Full blueprint content path, e.g. '/Game/Blueprints/BP_Foo'"),
    name:      z.string().describe("Variable name to remove"),
    compile:   z.boolean().optional().describe("Compile after removal (default true)"),
    save:      z.boolean().optional().describe("Save after successful compile (default true)"),
  },
  async ({ blueprint, name, compile, save }) => {
    try {
      const r = await ue5.bpDeleteVariable({ blueprint, name, compile, save });
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_bp_heal_world_context",
  [
    "Repair the UE5 migration corruption where a static Blueprint function's entry node has a VISIBLE '__WorldContext' pin backed by a resolvable self-FunctionReference.",
    "That state makes UK2Node_FunctionEntry::AllocateDefaultPins build the auto world-context pin twice, tripping an ensure ('World context parameter pin already exists') — harmless in-editor but FATAL to a cook (packaged/UAT builds run under -CrashForUAT). Symptom: cook aborts on one specific BP with that ensure.",
    "The fix resets the entry FunctionReference to the healthy unresolvable state (function name preserved) so the pin regenerates hidden on the next reconstruct. Compiles + saves by default.",
    "Pass 'blueprint' (single) or 'blueprints' (array). Run with dry_run:true FIRST to get per-function diagnostics (entry-pin hidden state, resolved member, reference internals, WorldContext metadata) with NO mutation, then re-run without dry_run to heal.",
    "Example: {\"blueprint\":\"/Game/Blueprints/BP_Foo\",\"dry_run\":true}",
  ].join(" "),
  {
    blueprint:  z.string().optional().describe("Single blueprint content path, e.g. '/Game/Blueprints/BP_Foo'"),
    blueprints: z.array(z.string()).optional().describe("Batch mode: array of blueprint content paths"),
    compile:    z.boolean().optional().describe("Compile after healing (default true)"),
    save:       z.boolean().optional().describe("Save after successful compile (default true)"),
    dry_run:    z.boolean().optional().describe("Diagnose only — report per-function state with no mutation (default false)"),
  },
  async ({ blueprint, blueprints, compile, save, dry_run }) => {
    try {
      const r = await ue5.bpHealWorldContext({ blueprint, blueprints, compile, save, dry_run });
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_bp_create_variable",
  [
    "Add a member variable to a Blueprint. Use this BEFORE referencing a variable via variable_get/variable_set in ue5_bp_add_logic.",
    "Note: ue5_bp_add_logic also auto-creates self-context variables when their type can be inferred from a connection — this tool is for explicit cases or when inference can't work.",
    "Example: {\"blueprint\":\"/Game/test/BP_test\",\"name\":\"StartLocation\",\"type\":\"FVector\"}.",
    "Supported types — primitives: bool, int/int32, int64, float/double, string/FString, name/FName, text/FText, byte.",
    "Engine structs: FVector, FVector2D, FRotator, FTransform, FLinearColor, FColor.",
    "Object refs: bare class path '/Script/Engine.Actor', short class name 'Actor'/'MaterialInstanceDynamic', or explicit 'object:Foo'.",
    "Prefixed forms (work with full asset paths OR short names): 'class:Foo' (TSubclassOf), 'softclass:Foo' (TSoftClassPtr), 'softobject:Foo' (TSoftObjectPtr), 'interface:BPI_Foo', 'enum:E_Foo' (UEnum or UUserDefinedEnum), 'struct:F_Foo' (UScriptStruct or UUserDefinedStruct).",
    "Containers (wrap any of the above, parsed recursively, no nesting): 'array<int32>', 'set<FString>', 'map<FName, /Game/Data/E_GamePhase>'.",
    "Bare asset paths auto-detect: '/Game/Data/E_GamePhase' resolves to enum, '/Game/Data/F_BombData' to struct, '/Game/Blueprints/BP_Foo' to object ref.",
  ].join(" "),
  {
    blueprint: z.string().describe("Full blueprint content path, e.g. '/Game/Blueprints/BP_Foo'"),
    name:      z.string().describe("Variable name (must be a valid identifier)"),
    type:      z.string().describe("Type string — see list in description"),
    default_value:       z.string().optional().describe("Optional initial value as string (e.g. 'X=0,Y=0,Z=0' for FVector)"),
    category:            z.string().optional().describe("Variables panel category (default '')"),
    instance_editable:   z.boolean().optional().describe("Show in Details panel of placed instances (default false)"),
    blueprint_read_only: z.boolean().optional().describe("Read-only inside Blueprints (default false)"),
    expose_on_spawn:     z.boolean().optional().describe("Expose as a parameter on SpawnActor (default false)"),
    private:             z.boolean().optional().describe("Hide from Details panel of derived classes (default false)"),
    compile:             z.boolean().optional().describe("Compile after adding (default true)"),
    save:                z.boolean().optional().describe("Save package after successful compile (default true)"),
  },
  async ({ blueprint, name, type, default_value, category, instance_editable, blueprint_read_only, expose_on_spawn, private: isPrivate, compile, save }) => {
    try {
      const r = await ue5.bpCreateVariable({ blueprint, name, type, default_value, category, instance_editable, blueprint_read_only, expose_on_spawn, private: isPrivate, compile, save });
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_bp_list_variables",
  "Read all member variables on a Blueprint with their types, defaults, flags, and category. Use to inspect existing BPs before adding logic that references variables.",
  {
    blueprint: z.string().describe("Full blueprint content path, e.g. '/Game/Blueprints/BP_Foo'"),
  },
  async ({ blueprint }) => {
    try {
      const r = await ue5.bpListVariables({ blueprint });
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_bp_add_logic",
  [
    "PRIMARY Blueprint-authoring tool. Create and wire any set of K2 nodes in a Blueprint Event Graph in one call.",
    "Example: add a BoxCollision OnComponentHit event wired to PrintString(\"hello\"):",
    '{"blueprint":"/Game/test/BP_test","nodes":[{"id":"evt","type":"component_event","component":"BoxCollision","event":"OnComponentHit"},{"id":"print","type":"call_function","function":"PrintString","class":"/Script/Engine.KismetSystemLibrary","defaults":{"InString":"hello"}}],"connections":[{"from":"evt.then","to":"print.execute"}],"compile":true}',
    "Node types: event, component_event, call_function, variable_get, variable_set, branch, cast, sequence, self, knot, macro, custom_event.",
    "Connections use 'node_id.pin_name' with pin aliases (then / execute / true / false / condition / self / target / return).",
    "DECOMPOSITION: if the logic for a single event needs >~8 exec-bearing nodes, do NOT pile them all into one EventGraph batch. First call ue5_bp_create_function for each one-purpose helper (ValidateInput, SpawnBomb, ApplyDamage, …), then ue5_bp_add_logic each function body separately, and have the EventGraph orchestrate by calling them via call_function + self_context:true. See Rule 9 in UE5_NGG_RULES.md.",
  ].join(" "),
  {
    blueprint:   z.string().describe("Full blueprint content path, e.g. '/Game/Blueprints/BP_Foo'"),
    graph:       z.string().optional().describe("Graph name (default 'EventGraph')"),
    nodes:       z.preprocess(jsonPreprocess, z.array(z.any()))
                  .describe("Array of node specs: { id, type, ...type-specific fields, defaults?: {pin: value} }"),
    connections: z.preprocess(jsonPreprocess, z.array(z.any())).optional()
                  .describe("Array of { from: 'node.pin', to: 'node.pin' }"),
    auto_layout: z.boolean().optional().describe("Cascade nodes left-to-right (default true)"),
    compile:     z.boolean().optional().describe("Compile the blueprint after wiring (default true)"),
    save:        z.boolean().optional().describe("Save the blueprint package after successful compile (default true)"),
  },
  async ({ blueprint, graph, nodes, connections, auto_layout, compile, save }) => {
    try {
      const r = await ue5.bpAddLogic({ blueprint, graph, nodes: normalizeNodes(nodes), connections, auto_layout, compile, save });
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_bp_add_node",
  [
    "ADVANCED: create a single K2 node by UClass path. Prefer ue5_bp_add_logic unless you need a node type not covered there.",
    "Example: {\"blueprint\":\"/Game/test/BP_test\",\"node_class\":\"K2Node_CallFunction\",\"node_id\":\"print\",\"config\":{\"function\":{\"class\":\"/Script/Engine.KismetSystemLibrary\",\"name\":\"PrintString\"}},\"pin_defaults\":{\"InString\":\"hi\"}}",
    "node_id is a caller-chosen string you can use in ue5_bp_connect_pins later (expires after 10 min).",
  ].join(" "),
  {
    blueprint:    z.string().describe("Blueprint content path"),
    graph:        z.string().optional().describe("Graph name (default 'EventGraph')"),
    node_class:   z.string().describe("K2Node class — full '/Script/BlueprintGraph.K2Node_CallFunction' or short 'K2Node_CallFunction'"),
    node_id:      z.string().optional().describe("Caller-chosen id to reference this node in follow-up calls"),
    config:       z.preprocess(jsonPreprocess, z.any()).optional().describe("Class-specific config (see docs)"),
    pin_defaults: z.preprocess(jsonPreprocess, z.any()).optional().describe("{ pin_name: value } defaults"),
    position:     z.preprocess(jsonPreprocess, z.any()).optional().describe("{ x, y } node position"),
  },
  async (args) => {
    try {
      const r = await ue5.bpAddNode(args);
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_bp_connect_pins",
  [
    "ADVANCED: connect two pins by node_id (or raw node Guid) + pin name. Prefer ue5_bp_add_logic.",
    "Example: {\"blueprint\":\"/Game/test/BP_test\",\"from_node\":\"evt\",\"from_pin\":\"then\",\"to_node\":\"print\",\"to_pin\":\"execute\"}",
    "Pin name lookups are case-insensitive and understand aliases: then/execute/true/false/condition/self/target/return.",
  ].join(" "),
  {
    blueprint: z.string(),
    graph:     z.string().optional(),
    from_node: z.string().describe("Source node_id or Guid"),
    from_pin:  z.string().describe("Source pin name (case-insensitive; accepts aliases)"),
    to_node:   z.string().describe("Destination node_id or Guid"),
    to_pin:    z.string().describe("Destination pin name"),
  },
  async (args) => {
    try {
      const r = await ue5.bpConnectPins(args);
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_bp_compile",
  [
    "Compile a Blueprint and optionally save it. Returns compile errors/warnings.",
    "Example: {\"blueprint\":\"/Game/test/BP_test\",\"save\":true}",
  ].join(" "),
  {
    blueprint: z.string(),
    save:      z.boolean().optional().describe("If true AND compile succeeds, save the package"),
  },
  async ({ blueprint, save }) => {
    try {
      const r = await ue5.bpCompile({ blueprint, save });
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_bp_delete_node",
  [
    "Delete a node from a Blueprint graph by node_id (from a previous add) or raw node_guid.",
    "Use this to remove stale/duplicate/orphaned nodes (e.g. an empty Event AnyDamage left over by an earlier edit).",
    "Auto compile+save by default. Example: {\"blueprint\":\"/Game/test/BP_test\",\"node_guid\":\"8C016DD4-4CED-352B-33E4-A6A63B2B6179\"}.",
  ].join(" "),
  {
    blueprint: z.string(),
    graph:     z.string().optional().describe("Graph name; if omitted, searches all graphs on the BP"),
    node_id:   z.string().optional().describe("User-supplied node id from a previous /bp/add_* call"),
    node_guid: z.string().optional().describe("Raw node GUID (e.g. from ue5_bp_read_graph)"),
    compile:   z.boolean().optional().describe("Compile after delete (default true)"),
    save:      z.boolean().optional().describe("Save package after successful compile (default true)"),
  },
  async ({ blueprint, graph, node_id, node_guid, compile, save }) => {
    try {
      const r = await ue5.bpDeleteNode({ blueprint, graph, node_id, node_guid, compile, save });
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_bp_read_graph",
  [
    "Read a Blueprint graph (or all graphs on the BP) as structured JSON — same shape ue5_bp_add_logic accepts.",
    "Example: {\"blueprint\":\"/Game/test/BP_test\",\"graph\":\"EventGraph\"}. Omit 'graph' to get every graph on the BP.",
  ].join(" "),
  {
    blueprint: z.string(),
    graph:     z.string().optional(),
  },
  async ({ blueprint, graph }) => {
    try {
      const r = await ue5.bpReadGraph({ blueprint, graph });
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_bp_get_selection",
  [
    "Read the nodes the developer currently has selected in an open Blueprint editor, plus pin-boundary metadata for refactor / extract-to-function workflows.",
    "Returns: { blueprint, graph, graph_type, selection_count, nodes[], boundary{ external_inputs, external_outputs, external_exec_in, external_exec_out } }.",
    "If 'blueprint' is omitted, picks the most-recently-active BP editor that has a non-empty selection.",
    "Use this to (a) explain selected nodes to the user, (b) plan a BP-to-C++ conversion, or (c) prepare an extract-to-function call.",
  ].join(" "),
  {
    blueprint:        z.string().optional().describe("BP path (/Game/...). If omitted, server picks the focused BP editor with a selection."),
    include_pins:     z.boolean().optional().describe("Include full pin metadata for each selected node (default true)"),
    include_boundary: z.boolean().optional().describe("Include external-input/output pin classification (default true)"),
  },
  async ({ blueprint, include_pins, include_boundary }) => {
    try {
      const r = await ue5.bpGetSelection({ blueprint, include_pins, include_boundary });
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_bp_lint",
  [
    "Analyze one Blueprint for graph issues (duplicate events, stale refs, orphaned pins, compiler errors, etc.).",
    "With auto_fix:true, repairs what's safely repairable and compiles.",
    "Example: {\"blueprint\":\"/Game/test/BP_test\",\"auto_fix\":true}.",
  ].join(" "),
  {
    blueprint: z.string(),
    auto_fix:  z.boolean().optional().describe("If true, fix safely-repairable issues (default false — report only)"),
    compile:   z.boolean().optional().describe("Compile after fixes (default true when auto_fix is true)"),
    save:      z.boolean().optional().describe("Save package on successful compile (default true)"),
  },
  async ({ blueprint, auto_fix, compile, save }) => {
    try {
      const r = await ue5.bpLint({ blueprint, auto_fix, compile, save });
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_bp_lint_project",
  [
    "Lint every BP_* under path_prefix (default \"/Game\") and optionally auto-fix each.",
    "Returns per-BP reports keyed by path. Useful for CI or a pre-commit sweep.",
    "Example: {\"path_prefix\":\"/Game/test\",\"auto_fix\":true}.",
  ].join(" "),
  {
    path_prefix: z.string().optional().describe("Content-root prefix to scope the sweep (default \"/Game\")"),
    auto_fix:    z.boolean().optional(),
    compile:     z.boolean().optional(),
    save:        z.boolean().optional(),
  },
  async ({ path_prefix, auto_fix, compile, save }) => {
    try {
      const r = await ue5.bpLintProject({ path_prefix, auto_fix, compile, save });
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// PCG tools (Phase A) — procedural content generation graph authoring.
// All implementations live in pcg.js and drive the UE editor via exec_python.
// ---------------------------------------------------------------------------

server.tool(
  "pcg_list_node_types",
  "List PCG node (UPCGSettings) types available in the editor. Optionally filter by category (Samplers, Points, Spawners, Spatial, Splines, Polygons, ControlFlow, Metadata, IO, Getters, World, Grammar, Blueprint, Subgraph, Parameters).",
  {
    category: z
      .string()
      .optional()
      .describe("Category filter, case-insensitive. Omit for full list."),
  },
  async (args) => pcg.pcgListNodeTypes(args)
);

server.tool(
  "pcg_create_graph",
  "Create a new PCG Graph asset (UPCGGraph). Optionally mark it as standalone so it can run without a PCGComponent. Set overwrite=true to replace an existing asset at the same path.",
  {
    asset_path: z
      .string()
      .describe("Content-browser path for the new graph, e.g. '/Game/PCG/BP_Terrain'"),
    standalone: z
      .boolean()
      .optional()
      .default(false)
      .describe("If true, set bIsStandaloneGraph on the new graph"),
    overwrite: z
      .boolean()
      .optional()
      .default(false)
      .describe("If true and the asset exists, delete it first"),
  },
  async (args) => pcg.pcgCreateGraph(args)
);

server.tool(
  "pcg_add_node",
  "Add a node to a PCG Graph. settings_class is the UPCGSettings subclass short name (e.g. 'PCGSurfaceSamplerSettings'). Returns the new node's name for use with pcg_connect_pins.",
  {
    graph_path: z
      .string()
      .describe("Content path of the target UPCGGraph asset"),
    settings_class: z
      .string()
      .describe("Short UCLASS name of the settings type, e.g. 'PCGSurfaceSamplerSettings'"),
    node_title: z
      .string()
      .optional()
      .describe("Display title for the node. Defaults to the class's natural name."),
  },
  async (args) => pcg.pcgAddNode(args)
);

server.tool(
  "pcg_connect_pins",
  "Connect two pins in a PCG Graph. from_node / to_node accept either the name returned by pcg_add_node or the aliases '__input__' / '__output__' to reference the graph's In/Out nodes. Typical pin labels are 'In' / 'Out' on most settings.",
  {
    graph_path: z
      .string()
      .describe("Content path of the target UPCGGraph asset"),
    from_node: z
      .string()
      .describe("Source node name, or '__input__' for the graph input node"),
    from_pin: z
      .string()
      .default("Out")
      .describe("Source pin label (default 'Out')"),
    to_node: z
      .string()
      .describe("Destination node name, or '__output__' for the graph output node"),
    to_pin: z
      .string()
      .default("In")
      .describe("Destination pin label (default 'In')"),
  },
  async (args) => pcg.pcgConnectPins(args)
);

server.tool(
  "pcg_save_graph",
  "Save a PCG Graph asset to disk. Useful after a batch of edits to flush changes.",
  {
    graph_path: z
      .string()
      .describe("Content path of the target UPCGGraph asset"),
  },
  async (args) => pcg.pcgSaveGraph(args)
);

// ---- PCG introspection ----------------------------------------------------

server.tool(
  "pcg_read_graph",
  "Dump the full state of a PCG Graph: every node (with settings class, title, position, pin labels) and every edge. Use to verify a graph after authoring or to plan edits.",
  {
    graph_path: z.string().describe("Content path of the target UPCGGraph asset"),
  },
  async (args) => pcg.pcgReadGraph(args)
);

server.tool(
  "pcg_list_settings_properties",
  "List the editable UPROPERTYs of a UPCGSettings subclass (e.g. PCGStaticMeshSpawnerSettings → mesh selectors, descriptor, etc.). Returns each property's name, type, and default value — feed the name into pcg_set_node_settings_property.",
  {
    settings_class: z
      .string()
      .describe("Short UCLASS name, e.g. 'PCGStaticMeshSpawnerSettings'"),
  },
  async (args) => pcg.pcgListSettingsProperties(args)
);

// ---- PCG node mutation ----------------------------------------------------

server.tool(
  "pcg_set_node_settings_property",
  "Set a UPROPERTY on a node's settings (e.g. assign a StaticMesh to PCGStaticMeshSpawner, set point count on a sampler). Use 'value' for primitives. Use 'asset_value' (content path) when the property is an object reference like a StaticMesh, Material, or PCGGraph.",
  {
    graph_path: z.string().describe("Content path of the target UPCGGraph asset"),
    node:       z.string().describe("Node name (from pcg_add_node / pcg_read_graph) or '__input__' / '__output__'"),
    property:   z.string().describe("Property name on the settings object (use pcg_list_settings_properties to discover)"),
    value:      z.any().optional().describe("Primitive value (bool / int / float / string / array). Omit if using asset_value."),
    asset_value: z.string().optional().describe("Content path of an asset to assign (e.g. '/Game/Meshes/SM_Tree'). Takes precedence over 'value'."),
  },
  async (args) => pcg.pcgSetNodeSettingsProperty(args)
);

server.tool(
  "pcg_set_node_transform_ranges",
  "Set the Vector/Rotator transform-range properties on a transform-points style PCG node in one call (e.g. PCGTransformPointsSettings). Each field is optional; only supplied ones are set. scale_min/max and offset_min/max are Vectors; rotation_min/max are Rotators. Accepts arrays ([x,y,z] or [pitch,yaw,roll]), objects ({x,y,z} or {pitch,yaw,roll}), or JSON strings of either. More convenient than six separate pcg_set_node_settings_property calls.",
  {
    graph_path:   z.string().describe("Content path of the target UPCGGraph asset"),
    node:         z.string().describe("Node name (from pcg_add_node / pcg_read_graph)"),
    scale_min:    z.any().optional().describe("Vector [x,y,z] / {x,y,z}"),
    scale_max:    z.any().optional().describe("Vector [x,y,z] / {x,y,z}"),
    offset_min:   z.any().optional().describe("Vector [x,y,z] / {x,y,z}"),
    offset_max:   z.any().optional().describe("Vector [x,y,z] / {x,y,z}"),
    rotation_min: z.any().optional().describe("Rotator [pitch,yaw,roll] / {pitch,yaw,roll}"),
    rotation_max: z.any().optional().describe("Rotator [pitch,yaw,roll] / {pitch,yaw,roll}"),
  },
  async (args) => pcg.pcgSetNodeTransformRanges(args)
);

server.tool(
  "pcg_set_node_position",
  "Move a node in the PCG editor canvas. Use to keep auto-generated graphs visually readable.",
  {
    graph_path: z.string().describe("Content path of the target UPCGGraph asset"),
    node:       z.string().describe("Node name or '__input__' / '__output__'"),
    x:          z.number().describe("Canvas X coordinate"),
    y:          z.number().describe("Canvas Y coordinate"),
  },
  async (args) => pcg.pcgSetNodePosition(args)
);

server.tool(
  "pcg_remove_node",
  "Delete a node from a PCG Graph. The graph input/output nodes cannot be removed.",
  {
    graph_path: z.string().describe("Content path of the target UPCGGraph asset"),
    node:       z.string().describe("Node name to remove (from pcg_add_node / pcg_read_graph)"),
  },
  async (args) => pcg.pcgRemoveNode(args)
);

server.tool(
  "pcg_disconnect_pins",
  "Remove an edge between two pins in a PCG Graph. Same node/pin addressing as pcg_connect_pins.",
  {
    graph_path: z.string().describe("Content path of the target UPCGGraph asset"),
    from_node:  z.string().describe("Source node name or '__input__'"),
    from_pin:   z.string().default("Out").describe("Source pin label"),
    to_node:    z.string().describe("Destination node name or '__output__'"),
    to_pin:     z.string().default("In").describe("Destination pin label"),
  },
  async (args) => pcg.pcgDisconnectPins(args)
);

server.tool(
  "pcg_rename_node",
  "Change the display title of a node in a PCG Graph.",
  {
    graph_path: z.string().describe("Content path of the target UPCGGraph asset"),
    node:       z.string().describe("Node name"),
    new_title:  z.string().describe("New display title for the node"),
  },
  async (args) => pcg.pcgRenameNode(args)
);

// ---- PCG runtime: PCGComponent on level actors ----------------------------

server.tool(
  "pcg_add_component_to_actor",
  "Attach a UPCGComponent to a level actor and bind a PCG Graph to it. The actor is found by its display label in the current edited world. If a PCGComponent already exists on the actor, it is reused (only the graph binding changes).",
  {
    actor_label: z.string().describe("Display label of the target level actor (the name shown in the Outliner)"),
    graph_path:  z.string().describe("Content path of the UPCGGraph (or UPCGGraphInstance) to bind"),
  },
  async (args) => pcg.pcgAddComponentToActor(args)
);

server.tool(
  "pcg_set_component_property",
  "Set a UPROPERTY on the PCGComponent of a level actor. Common properties: 'seed', 'is_component_partitioned', 'generation_trigger' (use 'GenerateOnLoad' / 'GenerateOnDemand' / 'GenerateAtRuntime'), 'activated', 'regenerate_in_editor', 'input_type' ('Actor'/'Landscape'/'Other'), 'ignore_landscape_tracking'.",
  {
    actor_label: z.string().describe("Display label of the target level actor"),
    property:    z.string().describe("Property name on UPCGComponent"),
    value:       z.any().optional().describe("Value to assign. Strings are auto-coerced to the right enum for known enum properties (generation_trigger, input_type)."),
  },
  async (args) => pcg.pcgSetComponentProperty(args)
);

server.tool(
  "pcg_generate_component",
  "Trigger generate() on the PCGComponent attached to a level actor. Pass force=true to regenerate even if the component is marked clean.",
  {
    actor_label: z.string().describe("Display label of the target level actor"),
    force:       z.boolean().optional().default(true).describe("Force regeneration regardless of dirty state"),
  },
  async (args) => pcg.pcgGenerateComponent(args)
);

server.tool(
  "pcg_cleanup_component",
  "Trigger cleanup() on the PCGComponent attached to a level actor. With remove_components=true (default), spawned ISMs and managed actors are destroyed.",
  {
    actor_label:       z.string().describe("Display label of the target level actor"),
    remove_components: z.boolean().optional().default(true).describe("Destroy spawned components/actors (default true)"),
  },
  async (args) => pcg.pcgCleanupComponent(args)
);

// ---- PCG graph properties + user parameters -------------------------------

server.tool(
  "pcg_set_graph_property",
  "Set a graph-level UPROPERTY on a PCG Graph (description, category, is_editor_only, ignore_landscape_tracking, generation_radii, b_is_standalone_graph, etc.). Strings to 'description' / 'category' are auto-wrapped as FText.",
  {
    graph_path: z.string().describe("Content path of the target UPCGGraph asset"),
    property:   z.string().describe("Property name on UPCGGraph"),
    value:      z.any().describe("Value to assign (primitive, struct dict, or null)"),
  },
  async (args) => pcg.pcgSetGraphProperty(args)
);

server.tool(
  "pcg_add_graph_parameter",
  "Add a user parameter (visible to PCGUserParameterGet nodes inside the graph and overridable by PCGGraphInstance / PCGComponent). Type names: bool, int, int64, float, double, name, string, text, object, soft_object.",
  {
    graph_path: z.string().describe("Content path of the target UPCGGraph asset"),
    name:       z.string().describe("Parameter name"),
    type:       z.string().describe("Type name (bool / int / float / name / string / object / ...)"),
  },
  async (args) => pcg.pcgAddGraphParameter(args)
);

server.tool(
  "pcg_list_graph_parameters",
  "List the user parameters declared on a PCG Graph asset.",
  {
    graph_path: z.string().describe("Content path of the target UPCGGraph asset"),
  },
  async (args) => pcg.pcgListGraphParameters(args)
);

server.tool(
  "pcg_remove_graph_parameter",
  "Remove a user parameter from a PCG Graph by name.",
  {
    graph_path: z.string().describe("Content path of the target UPCGGraph asset"),
    name:       z.string().describe("Parameter name to remove"),
  },
  async (args) => pcg.pcgRemoveGraphParameter(args)
);

// ---- PCG graph instance assets --------------------------------------------

server.tool(
  "pcg_create_graph_instance",
  "Create a UPCGGraphInstance asset that references a base UPCGGraph and can override its user parameters. Use this to ship many tuned variants of a single source graph.",
  {
    asset_path: z.string().describe("Content path for the new PCGGraphInstance, e.g. '/Game/PCG/InstA'"),
    base_graph: z.string().describe("Content path of the base UPCGGraph to instance"),
    overwrite:  z.boolean().optional().default(false).describe("Replace any existing asset at asset_path"),
  },
  async (args) => pcg.pcgCreateGraphInstance(args)
);

server.tool(
  "pcg_set_graph_instance_parameter",
  "Set an override value on a UPCGGraphInstance for one of the base graph's user parameters. Use 'value' for primitives, 'asset_value' (content path) for object/soft-object parameters.",
  {
    instance_path: z.string().describe("Content path of the UPCGGraphInstance asset"),
    name:          z.string().describe("User parameter name to override"),
    value:         z.any().optional().describe("Primitive value (bool / int / float / string)"),
    asset_value:   z.string().optional().describe("Content path of an asset to assign (used when the parameter is an object reference)"),
  },
  async (args) => pcg.pcgSetGraphInstanceParameter(args)
);

// ---- PCG editor utilities -------------------------------------------------

server.tool(
  "pcg_open_in_editor",
  "Open a PCG Graph (or PCGGraphInstance) in the editor window so the user can inspect it visually.",
  {
    graph_path: z.string().describe("Content path of the asset to open"),
  },
  async (args) => pcg.pcgOpenInEditor(args)
);

server.tool(
  "pcg_regenerate_all",
  "Regenerate every PCGComponent in the current level. Optionally restrict to components bound to a specific graph asset (graph_filter). Equivalent to cleanup + generate(force=true) on each matching component.",
  {
    graph_filter: z.string().optional().describe("Optional content path of a UPCGGraph; only components bound to this graph are regenerated."),
    force:        z.boolean().optional().default(true).describe("Force regeneration regardless of dirty state"),
  },
  async (args) => pcg.pcgRegenerateAll(args)
);

// ---------------------------------------------------------------------------
// Gameplay Ability System (GAS) — single player, multiplayer, and AI modes
// ---------------------------------------------------------------------------
// GAS lifecycle: setup_actor → (create_attribute_set, create_ability, create_effect)
//                → configure_asc for replication mode → read_setup to verify.
// Replication modes: Full (listen-server players), Mixed (dedicated-server players),
//                    Minimal (AI pawns or non-owner spectators).
// Net execution policies for abilities: LocalPredicted (player input w/ client prediction),
//   LocalOnly (cosmetic, client-only), ServerInitiated (server starts, client echoes),
//   ServerOnly (AI logic or trusted server abilities).
// ---------------------------------------------------------------------------

server.tool(
  "ue5_gas_setup_actor",
  [
    "Add a UAbilitySystemComponent to a Blueprint actor, making it a GAS participant (works for players, AI, and standalone).",
    "Sets the replication mode for multiplayer: Full = listen-server players (all GEs replicated to all clients), Mixed = dedicated-server players (full data to owner only), Minimal = AI pawns or non-owning clients.",
    "Idempotent — if the ASC is already present, only updates the replication mode.",
    "After calling this, use ue5_gas_configure_asc to change the mode later, or ue5_gas_create_attribute_set to add attribute data.",
    "Example: {\"blueprint_path\":\"/Game/Characters/BP_ThrowBombCharacter\",\"replication_mode\":\"Mixed\"}",
  ].join(" "),
  {
    blueprint_path:   z.string().describe("Blueprint asset path, e.g. '/Game/Characters/BP_Foo'"),
    replication_mode: z.enum(["Full", "Mixed", "Minimal"]).optional().describe(
      "GAS replication mode — Full: all GEs replicated to all clients (listen-server), Mixed: full to owner only (dedicated-server players), Minimal: AI or non-replicating actors (default: Mixed)"
    ),
    save:             z.boolean().optional().describe("Save blueprint after modification (default true)"),
  },
  async ({ blueprint_path, replication_mode, save }) => {
    try {
      const r = await ue5.gasSetupActor({ blueprint_path, replication_mode, save });
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_gas_create_attribute_set",
  [
    "Create a Blueprint subclass of UAttributeSet with FGameplayAttributeData member variables (Health, MaxHealth, Stamina, etc.).",
    "Attributes are float-based values the ASC tracks and that GameplayEffects can Add/Multiply/Override.",
    "The Blueprint-generated class is automatically recognized by the ASC when added as a subobject — works with all replication modes.",
    "Pair with ue5_gas_setup_actor (to have an ASC) and ue5_gas_create_effect (to modify the attributes).",
    "Example: {\"asset_path\":\"/Game/GAS/AS_Character\",\"attributes\":[{\"name\":\"Health\",\"default_value\":100},{\"name\":\"MaxHealth\",\"default_value\":100},{\"name\":\"Stamina\",\"default_value\":50}]}",
  ].join(" "),
  {
    asset_path:   z.string().describe("Destination content path, e.g. '/Game/GAS/AS_Character'"),
    parent_class: z.string().optional().describe("Parent class path — omit to use UAttributeSet directly, or pass a C++ subclass path"),
    attributes:   z.preprocess(jsonPreprocess, z.array(z.any())).optional().describe(
      "Array of { name, default_value? } attribute specs — each becomes a FGameplayAttributeData Blueprint variable"
    ),
    save:         z.boolean().optional().describe("Save after creation (default true)"),
  },
  async ({ asset_path, parent_class, attributes, save }) => {
    try {
      const r = await ue5.gasCreateAttributeSet({ asset_path, parent_class, attributes, save });
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_gas_create_ability",
  [
    "Create a Blueprint subclass of UGameplayAbility with net policy, instancing, tags, cost, and cooldown configured.",
    "Net execution policy controls which context runs the ability — pick based on mode:",
    "  LocalPredicted: player-input abilities in multiplayer (client predicts, server validates — prevents lag feel).",
    "  LocalOnly: cosmetic-only, runs on client (e.g. play a sound, no state mutation).",
    "  ServerInitiated: server activates, client predicts — good for item-use or triggered abilities.",
    "  ServerOnly: AI abilities or server-only logic (no client involvement at all).",
    "Instancing: NonInstanced is fastest (no per-actor state); InstancedPerActor is the default (stateful); InstancedPerExecution creates a new instance per activation.",
    "ability_tags, block_ability_tags, cancel_abilities_tags accept arrays of GameplayTag strings (e.g. ['Ability.ThrowBomb']).",
    "cost_effect and cooldown_effect are /Game/ content paths to UGameplayEffect Blueprints.",
    "Example (multiplayer player throw): {\"asset_path\":\"/Game/GAS/Abilities/GA_ThrowBomb\",\"net_execution_policy\":\"LocalPredicted\",\"instancing_policy\":\"InstancedPerActor\",\"ability_tags\":[\"Ability.ThrowBomb\"],\"cooldown_effect\":\"/Game/GAS/Effects/GE_Cooldown_ThrowBomb\"}",
    "Example (AI shoot): {\"asset_path\":\"/Game/GAS/Abilities/GA_AIShoot\",\"net_execution_policy\":\"ServerOnly\",\"instancing_policy\":\"InstancedPerActor\",\"ability_tags\":[\"Ability.Shoot\"]}",
  ].join(" "),
  {
    asset_path:            z.string().describe("Destination content path, e.g. '/Game/GAS/Abilities/GA_ThrowBomb'"),
    parent_class:          z.string().optional().describe("Parent class path (default: UGameplayAbility)"),
    net_execution_policy:  z.enum(["LocalPredicted", "LocalOnly", "ServerInitiated", "ServerOnly"]).optional().describe(
      "When/where the ability executes: LocalPredicted (multiplayer players), ServerOnly (AI), ServerInitiated (server-triggered), LocalOnly (cosmetic) — default: LocalPredicted"
    ),
    instancing_policy:     z.enum(["NonInstanced", "InstancedPerActor", "InstancedPerExecution"]).optional().describe(
      "How ability instances are managed — NonInstanced: fastest, no per-actor state; InstancedPerActor: default; InstancedPerExecution: fresh instance each time"
    ),
    ability_tags:          z.preprocess(jsonPreprocess, z.array(z.string())).optional().describe("GameplayTags this ability has (e.g. ['Ability.ThrowBomb'])"),
    block_ability_tags:    z.preprocess(jsonPreprocess, z.array(z.string())).optional().describe("Tags of abilities to block while this ability is active"),
    cancel_abilities_tags: z.preprocess(jsonPreprocess, z.array(z.string())).optional().describe("Tags of abilities to cancel when this ability activates"),
    cost_effect:           z.string().optional().describe("Content path to cost UGameplayEffect Blueprint (e.g. '/Game/GAS/Effects/GE_Cost_Stamina')"),
    cooldown_effect:       z.string().optional().describe("Content path to cooldown UGameplayEffect Blueprint"),
    activation_group:      z.enum(["Independent", "Exclusive_Replaceable", "Exclusive_Blocking"]).optional().describe(
      "Exclusive activation group — Independent: can run alongside others; Exclusive_Replaceable: replaces other exclusive abilities; Exclusive_Blocking: blocks other exclusives"
    ),
    save:                  z.boolean().optional().describe("Save after creation (default true)"),
  },
  async ({ asset_path, parent_class, net_execution_policy, instancing_policy, ability_tags, block_ability_tags, cancel_abilities_tags, cost_effect, cooldown_effect, activation_group, save }) => {
    try {
      const r = await ue5.gasCreateAbility({ asset_path, parent_class, net_execution_policy, instancing_policy, ability_tags, block_ability_tags, cancel_abilities_tags, cost_effect, cooldown_effect, activation_group, save });
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_gas_create_effect",
  [
    "Create a Blueprint subclass of UGameplayEffect with duration, attribute modifiers, stacking, and tag requirements.",
    "Duration policies: Instant (applied once, then removed — for damage/healing), HasDuration (active for N seconds), Infinite (until explicitly removed).",
    "Modifiers: array of { attribute, operation, magnitude } — attribute is 'AttributeSetClassName.PropertyName' (e.g. 'AS_Character.Health'), operation is Add/Multiply/Override, magnitude is a number.",
    "Stacking: AggregateBySource (one stack per source actor), AggregateByTarget (one stack total per target), None (each application is independent).",
    "granted_tags are applied to the target while the effect is active (HasDuration/Infinite only).",
    "application_required_tags: target must have ALL of these for the effect to apply.",
    "ongoing_required_tags: target must keep ALL of these or the effect is removed.",
    "Example (instant damage): {\"asset_path\":\"/Game/GAS/Effects/GE_Damage_Bomb\",\"duration_policy\":\"Instant\",\"modifiers\":[{\"attribute\":\"AS_Character.Health\",\"operation\":\"Add\",\"magnitude\":-25}]}",
    "Example (heal-over-time): {\"asset_path\":\"/Game/GAS/Effects/GE_HealOverTime\",\"duration_policy\":\"HasDuration\",\"duration\":5,\"period\":1,\"modifiers\":[{\"attribute\":\"AS_Character.Health\",\"operation\":\"Add\",\"magnitude\":10}]}",
    "Example (stamina buff): {\"asset_path\":\"/Game/GAS/Effects/GE_StaminaBuff\",\"duration_policy\":\"Infinite\",\"modifiers\":[{\"attribute\":\"AS_Character.Stamina\",\"operation\":\"Multiply\",\"magnitude\":1.5}],\"granted_tags\":[\"Status.Buffed\"]}",
  ].join(" "),
  {
    asset_path:                z.string().describe("Destination content path, e.g. '/Game/GAS/Effects/GE_Damage'"),
    parent_class:              z.string().optional().describe("Parent class path (default: UGameplayEffect)"),
    duration_policy:           z.enum(["Instant", "HasDuration", "Infinite"]).optional().describe("How long the effect lasts (default: Instant)"),
    duration:                  z.number().optional().describe("Duration in seconds — only applies when duration_policy is HasDuration"),
    period:                    z.number().optional().describe("Periodic application interval in seconds (0 or omit = no period)"),
    stacking_type:             z.enum(["None", "AggregateBySource", "AggregateByTarget"]).optional().describe("Stacking mode (default: None — every application is independent)"),
    stack_limit:               z.number().int().optional().describe("Maximum stack count when stacking_type is set (default: 1)"),
    modifiers:                 z.preprocess(jsonPreprocess, z.array(z.any())).optional().describe(
      "Array of { attribute:'ClassName.Property', operation:'Add'|'Multiply'|'Override', magnitude:number } — attribute must reference a FGameplayAttributeData property"
    ),
    granted_tags:              z.preprocess(jsonPreprocess, z.array(z.string())).optional().describe("Tags granted to target while effect is active (HasDuration/Infinite only)"),
    application_required_tags: z.preprocess(jsonPreprocess, z.array(z.string())).optional().describe("Target must have ALL these tags for the effect to apply"),
    ongoing_required_tags:     z.preprocess(jsonPreprocess, z.array(z.string())).optional().describe("Target must keep ALL these tags or the effect is removed"),
    immunity_tags:             z.preprocess(jsonPreprocess, z.array(z.string())).optional().describe("Source immunity tags (note: full immunity setup may require UImmunityGameplayEffectComponent in UE5.3+)"),
    save:                      z.boolean().optional().describe("Save after creation (default true)"),
  },
  async ({ asset_path, parent_class, duration_policy, duration, period, stacking_type, stack_limit, modifiers, granted_tags, application_required_tags, ongoing_required_tags, immunity_tags, save }) => {
    try {
      const r = await ue5.gasCreateEffect({ asset_path, parent_class, duration_policy, duration, period, stacking_type, stack_limit, modifiers, granted_tags, application_required_tags, ongoing_required_tags, immunity_tags, save });
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_gas_configure_asc",
  [
    "Configure the AbilitySystemComponent replication mode on a Blueprint actor — update without re-adding the component.",
    "Replication modes: Full (listen-server players), Mixed (dedicated-server players — owner gets full data, others get minimal), Minimal (AI pawns, non-owner spectators).",
    "Call after ue5_gas_setup_actor when you need to change the replication mode later, or to verify configuration is correct.",
    "Example: {\"blueprint_path\":\"/Game/Characters/BP_AIBot\",\"replication_mode\":\"Minimal\"}",
  ].join(" "),
  {
    blueprint_path:   z.string().describe("Blueprint asset path"),
    replication_mode: z.enum(["Full", "Mixed", "Minimal"]).optional().describe("GAS replication mode (default: Mixed)"),
    save:             z.boolean().optional().describe("Save after modification (default true)"),
  },
  async ({ blueprint_path, replication_mode, save }) => {
    try {
      const r = await ue5.gasConfigureAsc({ blueprint_path, replication_mode, save });
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_gas_read_setup",
  [
    "Inspect the Gameplay Ability System configuration of a Blueprint asset.",
    "Classifies the Blueprint as: AbilitySystemActor (has ASC), GameplayAbility, GameplayEffect, AttributeSet, or Unknown.",
    "Returns: ASC presence + replication mode, attribute set components, ability tags + net policy + cost/cooldown refs, or effect duration/modifiers.",
    "Use this to verify GAS setup before implementing ability logic.",
    "Example: {\"blueprint_path\":\"/Game/Characters/BP_ThrowBombCharacter\"}",
  ].join(" "),
  {
    blueprint_path: z.string().describe("Blueprint asset path to inspect"),
  },
  async ({ blueprint_path }) => {
    try {
      const r = await ue5.gasReadSetup({ blueprint_path });
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ===========================================================================
// Unreal Insights profiling — capture + headless analysis
// ===========================================================================
// Capture commands run in-editor via /editor/exec_python (Trace.* console
// commands). Analysis spawns UnrealInsights.exe headless and parses the
// resulting CSV. See profile.js for the full command/flag references.
// ---------------------------------------------------------------------------

server.tool(
  "ue5_profile_trace_start",
  [
    "Start an Unreal Insights trace. Writes a .utrace to <Project>/Saved/Profiling/UnrealInsights/.",
    "Default channels: cpu,gpu,frame,bookmark,log (lean — covers >90% of perf questions).",
    "Add memalloc/loadtime/task/net only when investigating those specific subsystems — they bloat trace files fast.",
    "Editor must be running. Stop the trace with ue5_profile_trace_stop, then analyze with ue5_profile_analyze.",
  ].join(" "),
  {
    channels:  z.string().optional().describe("Comma-separated trace channels (default 'cpu,gpu,frame,bookmark,log')"),
    file_name: z.string().optional().describe("Just the basename, e.g. 'tick_test.utrace'. Default: timestamped name."),
  },
  async ({ channels, file_name }) => {
    try {
      const proj = await resolveProject();
      const r = await profile.traceStart({ channels, fileName: file_name, uprojectPath: proj.uproject_path });
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_profile_trace_stop",
  "Stop the active Unreal Insights trace. The .utrace is finalized at the path returned by ue5_profile_trace_start.",
  {},
  async () => {
    try {
      const r = await profile.traceStop();
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_profile_trace_status",
  "Print Trace.Status — current connection target, channel list, memory usage. Useful to confirm a trace is actually capturing.",
  {},
  async () => {
    try {
      const r = await profile.traceStatus();
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_profile_bookmark",
  "Emit a Trace.Bookmark event with the given name. Bookmarks show up in Insights' Frames/Timing track for navigating to a specific moment.",
  {
    name: z.string().describe("Bookmark label, e.g. 'BombSpawned' or 'PhaseTransition'"),
  },
  async ({ name }) => {
    try {
      const r = await profile.traceBookmark(name);
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_profile_region",
  [
    "Emit a Trace.RegionBegin or Trace.RegionEnd event. Regions span multiple frames and are the recommended way to slice a trace per-test.",
    "Always pair Begin/End with the same name. Use ue5_profile_analyze with region=<name> to get stats just for that region.",
  ].join(" "),
  {
    name:  z.string().describe("Region name, e.g. 'AttackPhase' — must match across Begin/End"),
    state: z.enum(["begin", "end"]).describe("Whether to begin or end the region"),
  },
  async ({ name, state }) => {
    try {
      const r = state === "begin" ? await profile.traceRegionBegin(name) : await profile.traceRegionEnd(name);
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_profile_list_traces",
  "List all .utrace files in <Project>/Saved/Profiling/UnrealInsights/ with size and mtime, newest first. Pure filesystem read — works whether the editor is running or not.",
  {},
  async () => {
    try {
      const proj = await resolveProject();
      const r = profile.listTraces(proj.uproject_path);
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_profile_analyze",
  [
    "Analyze a .utrace by spawning UnrealInsights.exe headless and exporting per-timer aggregated statistics.",
    "Returns the top-N timers ranked by TotalInclusiveTime (ms). Use this to answer 'what function spends the most time?'.",
    "Modes: 'top_functions' (no filter — full ranking), 'collision', 'physics', 'rendering', 'gameplay', 'loading', 'ui'",
    "(each applies a wildcard filter on timer names — e.g. collision matches *Collision*,*Sweep*,*Overlap*,*LineTrace*,*PrimitiveComponent*,*BodyInstance*).",
    "Falls back to driving Insights through in-editor Python (route='python') when the headless CLI cannot be invoked directly.",
  ].join(" "),
  {
    utrace:          z.string().describe("Absolute path to a .utrace file (use ue5_profile_list_traces to find)"),
    mode:            z.enum(["top_functions", "collision", "physics", "rendering", "gameplay", "loading", "ui", "raw"])
                       .optional()
                       .describe("Preset filter (default 'top_functions' — no filter, just sorted by inclusive time)"),
    top_n:           z.number().int().positive().optional().describe("Rows to return (default 30)"),
    timers_filter:   z.string().optional().describe("Override -timers= glob, e.g. '*Tick*,*PhysScene*'. Wildcard supported."),
    threads_filter:  z.string().optional().describe("Filter by thread name, e.g. 'GameThread' or 'Render*'"),
    region:          z.string().optional().describe("Restrict to a named region (set with ue5_profile_region)"),
    start_time:      z.number().optional().describe("Trace start time in seconds (default -infinity)"),
    end_time:        z.number().optional().describe("Trace end time in seconds (default +infinity)"),
    route:           z.enum(["auto", "insights", "python"]).optional().describe("Analysis route (default 'auto' = insights, fall back to python on error)"),
  },
  async ({ utrace, mode, top_n, timers_filter, threads_filter, region, start_time, end_time, route }) => {
    try {
      const proj = await resolveProject();
      const args = {
        utracePath: utrace,
        engineDir: proj.engine_dir,
        mode,
        topN: top_n,
        timersFilter: timers_filter,
        threadsFilter: threads_filter,
        region,
        startTime: start_time,
        endTime: end_time,
      };
      let r;
      if (route === "python") {
        r = await profile.analyzeViaPython(args);
      } else {
        try {
          r = await profile.analyze(args);
        } catch (err) {
          if (route === "auto" || route === undefined) {
            // Fall back to Python only if the editor is actually reachable.
            try { await ue5.healthCheck(); } catch (_) { throw err; }
            r = await profile.analyzeViaPython(args);
            r.fallback_reason = err.message;
          } else {
            throw err;
          }
        }
      }
      return { content: [{ type: "text", text: JSON.stringify(r, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ===========================================================================
// Previously-unexposed C++ routes
// ===========================================================================

server.tool(
  "ue5_project_info",
  "Get ground-truth project + engine info from the running editor: project name, .uproject path, engine install dir, and build target names (editor/game/server/client).",
  {},
  async () => {
    try {
      const result = await ue5.getProjectInfo();
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_duplicate_asset",
  "Duplicate any asset to a new content path (original untouched, copy saved). The standard duplicate-and-tweak workflow.",
  {
    source: z.string().describe("Content path of the asset to copy, e.g. '/Game/Blueprints/BP_Door'"),
    dest:   z.string().describe("Content path for the new copy, e.g. '/Game/Blueprints/BP_Door_Locked'"),
  },
  async ({ source, dest }) => {
    try {
      const result = await ue5.duplicateAsset(source, dest);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_set_asset_map_entries",
  "Replace all entries of a TMap property on an asset (e.g. TMap<Enum, TSubclassOf<...>> registries). Values that are asset/class paths are resolved editor-side with LoadObject — use the '_C' suffix for Blueprint classes.",
  {
    asset_path: z.string().describe("Full content path of the asset holding the map"),
    property:   z.string().describe("TMap UPROPERTY name, e.g. 'BombClasses'"),
    entries: z.array(z.object({
      key:   z.string().describe("Map key as text (enum entry name, FName, ...)"),
      value: z.string().describe("Map value as text; class refs like '/Game/BP_Foo.BP_Foo_C' are LoadObject-resolved"),
    })).describe("Entries to set (existing entries are cleared first)"),
  },
  async ({ asset_path, property, entries }) => {
    try {
      const result = await ue5.setAssetMapEntries(asset_path, property, entries);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_reimport_asset",
  "Reimport an asset from its original source file (FBX, WAV, PNG, ...) keeping all import settings. Use after the source file changed on disk. Refuses assets without an import source (that would freeze the editor behind a modal file dialog).",
  {
    asset_path: z.string().describe("Content path of the asset to reimport"),
  },
  async ({ asset_path }) => {
    try {
      // Guard: only hand assets to FReimportManager when they actually have a
      // reimportable source — anything else opens a modal picker and hangs a
      // headless editor session.
      const guard = await gamedev.reimportSourceCheck({ asset_path });
      if (!guard.ok) {
        return { content: [{ type: "text", text: guard.error }], isError: true };
      }
      if (!guard.data.source_exists) {
        return {
          content: [{
            type: "text",
            text: `source file is missing on disk: ${guard.data.source_file} — restore it (or reimport manually in the editor) before calling this tool`,
          }],
          isError: true,
        };
      }
      const result = await ue5.reimportAsset(asset_path);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_bp_add_interface",
  "Add a Blueprint Interface (BP interface asset path or /Script class path) to a Blueprint's implemented-interfaces list, then recompile. Pair with ue5_bp_implement_interface_function for functions that have return values.",
  {
    blueprint: z.string().describe("Content path of the Blueprint"),
    interface: z.string().describe("Interface to implement: BP interface asset path or '/Script/Module.UInterfaceName' class path"),
    compile:   z.boolean().optional().describe("Recompile after adding (default true)"),
  },
  async ({ blueprint, interface: iface, compile }) => {
    try {
      const result = await ue5.bpAddInterface({ blueprint, interface: iface, compile });
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_bp_implement_interface_function",
  "Create the implementation graph for an interface function the Blueprint already implements (functions WITH return values are not auto-graphed by ue5_bp_add_interface). Binds as the interface override — do NOT use ue5_bp_create_function for this, it would collide.",
  {
    blueprint: z.string().describe("Content path of the Blueprint"),
    function:  z.string().describe("Interface function name to implement"),
    compile:   z.boolean().optional().describe("Recompile after creating the graph (default true)"),
  },
  async ({ blueprint, function: func, compile }) => {
    try {
      const result = await ue5.bpImplementInterfaceFunction({ blueprint, function: func, compile });
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_bp_refresh_all_nodes",
  "Reconstruct every node in a Blueprint against current function/variable signatures — the canonical fix for stale nodes after reparenting, struct changes, or plugin migrations. Reports whether the Blueprint compiles clean afterwards.",
  {
    blueprint: z.string().describe("Content path of the Blueprint"),
    compile:   z.boolean().optional().describe("Recompile after refreshing (default true)"),
    save:      z.boolean().optional().describe("Mark package dirty for saving (default false)"),
  },
  async ({ blueprint, compile, save }) => {
    try {
      const result = await ue5.bpRefreshAllNodes({ blueprint, compile, save });
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ===========================================================================
// Gamedev workflow tools (gamedev.js — Python/Node-backed, no new C++)
// ===========================================================================

server.tool(
  "ue5_set_viewport_camera",
  "Point the level-editor viewport camera: either at an explicit location/rotation, or frame a named actor automatically (focus_actor). Use before ue5_viewport_screenshot to compose the shot.",
  {
    location:    z.array(z.number()).length(3).optional().describe("[X, Y, Z] camera location"),
    rotation:    z.array(z.number()).length(3).optional().describe("[Pitch, Yaw, Roll] camera rotation in degrees"),
    focus_actor: z.string().optional().describe("Actor label to frame automatically (overrides location/rotation)"),
    distance:    z.number().optional().describe("Camera distance when framing an actor (default: 3x actor bounds)"),
  },
  async (args) => gamedev.mcpResponse(await gamedev.setViewportCamera(args))
);

server.tool(
  "ue5_viewport_screenshot",
  "Capture the current editor camera view to a PNG (SceneCapture render — works even with the editor window in the background) and return its absolute file path — then Read the file to SEE the result of your edits. The visual feedback loop for levels, materials, meshes and VFX. Use ue5_set_viewport_camera first to compose the shot.",
  {
    filename:     z.string().optional().describe("Base filename without extension (default: timestamped)"),
    resolution_x: z.number().optional().describe("Width in px (default 1920)"),
    resolution_y: z.number().optional().describe("Height in px (default 1080)"),
  },
  async (args) => gamedev.mcpResponse(await gamedev.viewportScreenshot(args))
);

server.tool(
  "ue5_get_log",
  "Read the tail of the live editor output log with filters (severity, category, free text). Check for errors/warnings after mutations without waiting for something to visibly break.",
  {
    lines:    z.number().optional().describe("Max lines returned after filtering (default 100)"),
    severity: z.enum(["error", "warning"]).optional().describe("'error' = errors only, 'warning' = warnings + errors"),
    category: z.string().optional().describe("Log category filter, e.g. 'LogBlueprint'"),
    contains: z.string().optional().describe("Case-insensitive substring filter"),
  },
  async (args) => gamedev.mcpResponse(await gamedev.getLog(args))
);

server.tool(
  "ue5_pie_start",
  "Start a Play-In-Editor session (play or simulate mode) to exercise gameplay for real. Pair with ue5_get_log and ue5_viewport_screenshot to observe behavior, then ue5_pie_stop.",
  {
    simulate: z.boolean().optional().describe("true = Simulate (no player possession), false/omit = normal Play"),
  },
  async (args) => gamedev.mcpResponse(await gamedev.pieStart(args))
);

server.tool(
  "ue5_pie_stop",
  "Stop the running Play-In-Editor session.",
  {},
  async () => gamedev.mcpResponse(await gamedev.pieStop())
);

server.tool(
  "ue5_pie_status",
  "Check whether a Play-In-Editor session is currently running.",
  {},
  async () => gamedev.mcpResponse(await gamedev.pieStatus())
);

server.tool(
  "ue5_datatable_create",
  "Create a DataTable asset bound to a row struct (Blueprint struct asset path or /Script struct), optionally filling rows in the same call. Rows use UE's JSON format: [{\"Name\": \"RowA\", \"FieldName\": value, ...}].",
  {
    asset_path: z.string().describe("Destination content path, e.g. '/Game/Data/DT_Items'"),
    row_struct: z.string().describe("Row struct: BP struct asset path ('/Game/Data/S_ItemRow') or '/Script/Module.StructName'"),
    rows:       z.array(z.record(z.any())).optional().describe("Initial rows in UE row-JSON format (must include 'Name')"),
    overwrite:  z.boolean().optional().describe("Replace an existing asset at that path"),
  },
  async (args) => gamedev.mcpResponse(await gamedev.datatableCreate(args))
);

server.tool(
  "ue5_datatable_read",
  "Read a DataTable's row struct, row names, and all row data as JSON.",
  {
    asset_path: z.string().describe("Content path of the DataTable"),
  },
  async (args) => gamedev.mcpResponse(await gamedev.datatableRead(args))
);

server.tool(
  "ue5_datatable_set_rows",
  "Replace a DataTable's rows from JSON (UE row-JSON format: [{\"Name\": \"RowA\", ...fields}]). Existing rows are replaced wholesale; read-modify-write with ue5_datatable_read for partial edits.",
  {
    asset_path: z.string().describe("Content path of the DataTable"),
    rows:       z.array(z.record(z.any())).describe("Full row set in UE row-JSON format"),
  },
  async (args) => gamedev.mcpResponse(await gamedev.datatableSetRows(args))
);

server.tool(
  "ue5_material_add_expression",
  "Add a node to a Material's expression graph (MaterialExpression class, with or without the prefix: 'Multiply', 'TextureSample', 'ScalarParameter', ...). Returns node_id for ue5_material_connect. Recompiles the material.",
  {
    material_path:    z.string().describe("Content path of the parent Material (not an instance)"),
    expression_class: z.string().describe("Expression class, e.g. 'Multiply', 'MaterialExpressionScalarParameter'"),
    node_x:           z.number().optional().describe("Graph X position (default 0)"),
    node_y:           z.number().optional().describe("Graph Y position (default 0)"),
    properties:       z.record(z.any()).optional().describe("Editor properties to set, e.g. {parameter_name: 'Speed', default_value: 1.0}; color/vector as [r,g,b(,a)]; textures as asset paths"),
  },
  async (args) => gamedev.mcpResponse(await gamedev.materialAddExpression(args))
);

server.tool(
  "ue5_material_connect",
  "Wire material expression nodes together, or into a material output (material_property = BASE_COLOR, METALLIC, ROUGHNESS, EMISSIVE_COLOR, NORMAL, OPACITY, ...). Recompiles and saves.",
  {
    material_path:     z.string().describe("Content path of the Material"),
    from_node:         z.string().describe("Source node_id (from ue5_material_add_expression)"),
    from_output:       z.string().optional().describe("Source output pin name ('' = default)"),
    to_node:           z.string().optional().describe("Destination node_id (omit when targeting a material output)"),
    to_input:          z.string().optional().describe("Destination input pin name, e.g. 'A', 'B' ('' = default)"),
    material_property: z.string().optional().describe("Material output to drive instead of a node, e.g. 'BASE_COLOR'"),
  },
  async (args) => gamedev.mcpResponse(await gamedev.materialConnect(args))
);

server.tool(
  "ue5_sequencer_create",
  "Create a Level Sequence asset with a frame rate and duration — the starting point for cutscenes and scripted camera moves.",
  {
    asset_path:     z.string().describe("Destination content path, e.g. '/Game/Cinematics/LS_Intro'"),
    frame_rate:     z.number().optional().describe("Display rate in fps (default 30)"),
    length_seconds: z.number().optional().describe("Playback length in seconds (default 5)"),
    overwrite:      z.boolean().optional().describe("Replace an existing asset at that path"),
  },
  async (args) => gamedev.mcpResponse(await gamedev.sequencerCreate(args))
);

server.tool(
  "ue5_sequencer_bind_actor",
  "Bind a level actor into a Level Sequence (possessable), optionally adding a transform track with keyframes: [{time: seconds, location: [x,y,z], rotation?: [p,y,r]}].",
  {
    sequence_path:  z.string().describe("Content path of the Level Sequence"),
    actor_label:    z.string().describe("Label of the level actor to bind"),
    transform_keys: z.array(z.object({
      time:     z.number().describe("Key time in seconds"),
      location: z.array(z.number()).length(3).optional(),
      rotation: z.array(z.number()).length(3).optional(),
    })).optional().describe("Transform keyframes to add"),
  },
  async (args) => gamedev.mcpResponse(await gamedev.sequencerBindActor(args))
);

server.tool(
  "ue5_sequencer_add_camera",
  "Spawn a CineCameraActor, bind it into a Level Sequence, and add a camera-cut track covering the whole sequence. Optional look_at_actor aims the camera.",
  {
    sequence_path: z.string().describe("Content path of the Level Sequence"),
    camera_label:  z.string().optional().describe("Label for the spawned camera (default 'NGG_CineCamera')"),
    location:      z.array(z.number()).length(3).optional().describe("Camera location (default [500, 0, 300])"),
    look_at_actor: z.string().optional().describe("Actor label the camera should aim at"),
  },
  async (args) => gamedev.mcpResponse(await gamedev.sequencerAddCamera(args))
);

server.tool(
  "ue5_sequencer_read",
  [
    "Read a Level Sequence's full structure: display rate, playback range, marked frames, master tracks, and every binding with its tracks, sections and keyed channels.",
    "Call this BEFORE ue5_sequencer_set_keys — it reports the exact channel_name values ('Location.X', 'Rotation.Z', ...) that keying requires, and shows what is already keyed.",
    "This is the sequencer counterpart to ue5_bp_read_graph.",
  ].join(" "),
  {
    sequence_path: z.string().describe("Content path of the Level Sequence, e.g. '/Game/Cine/LS_Intro'"),
    include_keys:  z.boolean().optional().describe("Include each channel's keys (default true; false gives a lighter outline)"),
  },
  async (args) => sequencer.mcpResponse(await sequencer.sequencerRead(args))
);

server.tool(
  "ue5_sequencer_add_spawnable",
  [
    "Add a spawnable binding — an object the sequence creates itself instead of possessing from the level.",
    "Give either actor_class (an unreal.* class name such as 'CineCameraActor', 'PointLight', 'StaticMeshActor') or asset_path (a Blueprint or mesh to spawn from).",
    "Use ue5_sequencer_bind_actor instead when the object already exists in the level.",
  ].join(" "),
  {
    sequence_path: z.string().describe("Content path of the Level Sequence"),
    actor_class:   z.string().optional().describe("Class name to spawn, e.g. 'CineCameraActor'"),
    asset_path:    z.string().optional().describe("Or an asset path to spawn from, e.g. '/Game/BP/BP_Prop'"),
    display_name:  z.string().optional().describe("Name for the binding in the sequencer outliner"),
    save:          z.boolean().optional().describe("Save the sequence afterwards (default true)"),
  },
  async (args) => sequencer.mcpResponse(await sequencer.sequencerAddSpawnable(args))
);

server.tool(
  "ue5_sequencer_add_track",
  [
    "Add a track to a Level Sequence. With `binding` the track goes on that object; without one it becomes a master track on the sequence itself (where CameraCut and Event tracks belong).",
    "track_type accepts a short alias — transform, visibility, audio, animation, cameracut, event, float, double, bool, integer, color, vector, particle, cinematicshot — or an exact UClass name like 'MovieScene3DTransformTrack'.",
    "Property tracks (float/double/bool/integer/color/vector) also need property_path, e.g. 'RelativeLocation' or 'Intensity'; without it the track evaluates to nothing.",
    "Returns the section's channel names, which ue5_sequencer_set_keys takes.",
  ].join(" "),
  {
    sequence_path: z.string().describe("Content path of the Level Sequence"),
    track_type:    z.string().describe("Track alias ('transform') or UClass name ('MovieScene3DTransformTrack')"),
    binding:       z.string().optional().describe("Binding name to attach to; omit for a master track"),
    property_path: z.string().optional().describe("Property this track drives — required for property tracks"),
    add_section:   z.boolean().optional().describe("Also cut one section spanning the playback range (default true)"),
    save:          z.boolean().optional().describe("Save the sequence afterwards (default true)"),
  },
  async (args) => sequencer.mcpResponse(await sequencer.sequencerAddTrack(args))
);

server.tool(
  "ue5_sequencer_add_section",
  [
    "Cut an additional section on an existing track, bounded in seconds. Use for clips that do not span the whole sequence — a second animation, another shot.",
    "The track must already exist (ue5_sequencer_add_track). Pass row_index to stack overlapping sections on separate rows.",
  ].join(" "),
  {
    sequence_path: z.string().describe("Content path of the Level Sequence"),
    track_type:    z.string().describe("Which track to cut on — same aliases as ue5_sequencer_add_track"),
    binding:       z.string().optional().describe("Binding the track sits on; omit for a master track"),
    start_seconds: z.number().describe("Section start, in seconds"),
    end_seconds:   z.number().describe("Section end, in seconds (must be greater than start)"),
    row_index:     z.number().optional().describe("Row to place the section on, for overlapping clips"),
    save:          z.boolean().optional().describe("Save the sequence afterwards (default true)"),
  },
  async (args) => sequencer.mcpResponse(await sequencer.sequencerAddSection(args))
);

server.tool(
  "ue5_sequencer_set_keys",
  [
    "Key a section's channels. Each entry in keys[] is { channel, time, value, interpolation? } where `channel` is a channel_name — 'Location.X', 'Rotation.Z', 'Scale.Y' — and `time` is in seconds.",
    "Channels are addressed by NAME, not index: run ue5_sequencer_read (or read the channels returned by ue5_sequencer_add_track) to see what a section exposes.",
    "interpolation is one of auto, cubic, linear, constant. Values are coerced to the channel's own type, so bool channels take true/false and integer channels take whole numbers.",
  ].join(" "),
  {
    sequence_path: z.string().describe("Content path of the Level Sequence"),
    track_type:    z.string().describe("Which track to key — same aliases as ue5_sequencer_add_track"),
    binding:       z.string().optional().describe("Binding the track sits on; omit for a master track"),
    section_index: z.number().optional().describe("Which section on that track (default 0)"),
    keys: z.preprocess(jsonPreprocess, z.array(z.any())).describe(
      "Array of { channel, time, value, interpolation? } — e.g. [{\"channel\":\"Location.X\",\"time\":0,\"value\":0},{\"channel\":\"Location.X\",\"time\":2,\"value\":500}]"
    ),
    save:          z.boolean().optional().describe("Save the sequence afterwards (default true)"),
  },
  async (args) => sequencer.mcpResponse(await sequencer.sequencerSetKeys(args))
);

server.tool(
  "ue5_sequencer_set_playback_range",
  [
    "Change an existing sequence's length and/or display rate.",
    "Note that changing frame_rate re-times the ruler, not the content: keys are stored in tick resolution and keep their wall-clock timing.",
  ].join(" "),
  {
    sequence_path: z.string().describe("Content path of the Level Sequence"),
    start_seconds: z.number().optional().describe("New playback start, in seconds"),
    end_seconds:   z.number().optional().describe("New playback end, in seconds"),
    frame_rate:    z.number().optional().describe("New display rate in FPS, e.g. 24, 30, 60"),
    save:          z.boolean().optional().describe("Save the sequence afterwards (default true)"),
  },
  async (args) => sequencer.mcpResponse(await sequencer.sequencerSetPlaybackRange(args))
);

server.tool(
  "ue5_wp_read",
  [
    "Describe the currently open world for World Partition work: whether it is partitioned at all, its bounds, every Data Layer with its state, and a summary of the streaming manifest (actor counts by class, runtime grids, how many actors are spatially loaded).",
    "Call this before any other ue5_wp_* tool — they need to know which layers exist, and Data Layers require a partitioned map.",
    "include_actors lists individual actor descriptors, which can be long on a real map; class_filter narrows it.",
  ].join(" "),
  {
    include_actors: z.boolean().optional().describe("List every actor descriptor (default false)"),
    class_filter:   z.string().optional().describe("With include_actors, only descs whose class name contains this"),
  },
  async (args) => worldpartition.mcpResponse(await worldpartition.wpRead(args))
);

server.tool(
  "ue5_wp_create_data_layer",
  [
    "Create a Data Layer: the DataLayerAsset plus an instance of it in the current world. Both are reported — the asset is shared content that outlives this level, the instance is this level's use of it.",
    "type is 'runtime' (streams in and out during play, default) or 'editor' (an authoring-time grouping only).",
    "Requires a World Partition map — make one with ue5_create_level partitioned=true.",
  ].join(" "),
  {
    asset_path: z.string().describe("Where to create the DataLayerAsset, e.g. '/Game/DataLayers/DL_Foliage'"),
    type:       z.string().optional().describe("'runtime' (default) or 'editor'"),
    parent:     z.string().optional().describe("Short name of an existing layer to nest this one under"),
    initial_runtime_state: z.string().optional().describe("State the layer starts in: unloaded, loaded or activated"),
    is_private: z.boolean().optional().describe("Make it private to this level instead of a shared asset"),
    save:       z.boolean().optional().describe("Save the asset and level afterwards (default true)"),
  },
  async (args) => worldpartition.mcpResponse(await worldpartition.wpCreateDataLayer(args))
);

server.tool(
  "ue5_wp_set_actor_data_layers",
  "Add or remove level actors (by label) on a Data Layer. Returns the layer's full membership afterwards so the result is verifiable.",
  {
    layer:  z.string().describe("Short name of the Data Layer"),
    actors: z.preprocess(jsonPreprocess, z.array(z.string())).describe("Actor labels, e.g. [\"Rock_01\",\"Rock_02\"]"),
    mode:   z.string().optional().describe("'add' (default) or 'remove'"),
    save:   z.boolean().optional().describe("Save the level afterwards (default true)"),
  },
  async (args) => worldpartition.mcpResponse(await worldpartition.wpSetActorDataLayers(args))
);

server.tool(
  "ue5_wp_set_data_layer_state",
  "Change a Data Layer's editor visibility, whether it is loaded in the editor, and the runtime state it starts in (unloaded / loaded / activated).",
  {
    layer:            z.string().describe("Short name of the Data Layer"),
    visible:          z.boolean().optional().describe("Editor visibility"),
    loaded_in_editor: z.boolean().optional().describe("Whether its actors are loaded in the editor"),
    initial_runtime_state: z.string().optional().describe("unloaded, loaded or activated"),
    save:             z.boolean().optional().describe("Save the level afterwards (default true)"),
  },
  async (args) => worldpartition.mcpResponse(await worldpartition.wpSetDataLayerState(args))
);

server.tool(
  "ue5_wp_delete_data_layer",
  "Remove a Data Layer instance from this level. The DataLayerAsset itself is kept — it is shared content and other levels may instance it.",
  {
    layer: z.string().describe("Short name of the Data Layer"),
    save:  z.boolean().optional().describe("Save the level afterwards (default true)"),
  },
  async (args) => worldpartition.mcpResponse(await worldpartition.wpDeleteDataLayer(args))
);

server.tool(
  "ue5_wp_load_region",
  [
    "Load, unload, pin or unpin the actors of a partitioned world inside a box, so a region can be edited without opening the whole map.",
    "Pin keeps actors loaded regardless of the streaming source; load is a one-off.",
    "Use ue5_wp_read to get the world bounds first.",
  ].join(" "),
  {
    min:  z.preprocess(jsonPreprocess, z.array(z.number()).length(3)).describe("Region minimum [x, y, z]"),
    max:  z.preprocess(jsonPreprocess, z.array(z.number()).length(3)).describe("Region maximum [x, y, z]"),
    mode: z.string().optional().describe("'load' (default), 'unload', 'pin' or 'unpin'"),
  },
  async (args) => worldpartition.mcpResponse(await worldpartition.wpLoadRegion(args))
);

server.tool(
  "ue5_level_instance_create",
  [
    "Place an existing level inside the current one as a Level Instance actor.",
    "Note the direction: UE 5.8 does not expose LevelInstanceSubsystem to Python, so collapsing a selection of actors INTO a new level instance is not available here — this is the 'place level A inside level B' half.",
  ].join(" "),
  {
    level_asset: z.string().describe("Level to instance, e.g. '/Game/Maps/L_Room'"),
    location:    z.preprocess(jsonPreprocess, z.array(z.number()).length(3)).optional().describe("Placement [x, y, z] (default origin)"),
    rotation:    z.preprocess(jsonPreprocess, z.array(z.number()).length(3)).optional().describe("Rotation [pitch, yaw, roll]"),
    actor_label: z.string().optional().describe("Label for the spawned LevelInstance actor"),
    save:        z.boolean().optional().describe("Save the level afterwards (default true)"),
  },
  async (args) => worldpartition.mcpResponse(await worldpartition.levelInstanceCreate(args))
);

server.tool(
  "ue5_ikrig_create",
  [
    "Create an IK Rig for a skeletal mesh: a Full Body IK solver, IK goals on named bones, and the retarget chains an IK Retargeter later maps between characters.",
    "Order is handled for you (mesh -> solver -> goals -> chains): a goal cannot attach to a solver that does not exist yet, and the engine reports that as a silent failure.",
    "Note that auto_fbik adds its own goals for the limbs (LeftHandIK, RightHandIK, LeftFootIK, RightFootIK), so the result lists more goals than you asked for — yours are added alongside them.",
    "Only the Full Body IK solver is available — UE 5.8's Python bindings cannot pass an arbitrary solver class to IKRigController.add_solver. Add other solvers in the IK Rig editor.",
  ].join(" "),
  {
    asset_path:    z.string().describe("Destination path, e.g. '/Game/Rigs/IK_Quinn'"),
    skeletal_mesh: z.string().describe("Skeletal mesh to rig, e.g. '/Game/Characters/Meshes/SKM_Quinn'"),
    retarget_root: z.string().optional().describe("Root bone for retargeting, usually 'pelvis' or 'root'"),
    auto_fbik:     z.boolean().optional().describe("Add a Full Body IK solver (default true)"),
    goals: z.preprocess(jsonPreprocess, z.array(z.any())).optional().describe(
      "IK goals: [{ name, bone }] — e.g. [{\"name\":\"Hand_L\",\"bone\":\"hand_l\"}]"
    ),
    chains: z.preprocess(jsonPreprocess, z.array(z.any())).optional().describe(
      "Retarget chains: [{ name, start_bone, end_bone, goal? }]"
    ),
  },
  async (args) => rigging.mcpResponse(await rigging.ikRigCreate(args))
);

server.tool(
  "ue5_ikrig_read",
  "Read an IK Rig: its skeletal mesh, solver count, retarget root, IK goals with their bones, and retarget chains with their start/end bones.",
  {
    asset_path: z.string().describe("Content path of the IK Rig"),
  },
  async (args) => rigging.mcpResponse(await rigging.ikRigRead(args))
);

server.tool(
  "ue5_ikretargeter_create",
  [
    "Create an IK Retargeter that maps animation from one IK Rig onto another — the asset behind retargeting a character's animations to a different skeleton.",
    "Both rigs need retarget chains (see ue5_ikrig_create) or there is nothing to map. auto_map 'exact' matches chains by identical name, 'fuzzy' by similarity, 'none' leaves them unmapped.",
    "The standard retarget op stack is created too; without it the asset is inert.",
  ].join(" "),
  {
    asset_path:  z.string().describe("Destination path, e.g. '/Game/Rigs/RTG_QuinnToManny'"),
    source_rig:  z.string().describe("IK Rig to take animation FROM"),
    target_rig:  z.string().describe("IK Rig to apply animation TO"),
    auto_map:    z.string().optional().describe("'exact' (default), 'fuzzy' or 'none'"),
    source_preview_mesh: z.string().optional().describe("Preview mesh for the source side"),
    target_preview_mesh: z.string().optional().describe("Preview mesh for the target side"),
  },
  async (args) => rigging.mcpResponse(await rigging.ikRetargeterCreate(args))
);

server.tool(
  "ue5_ikretargeter_set_chain_mapping",
  "Fix chain mappings on an existing IK Retargeter — for the pairs auto-mapping got wrong. Each mapping says which source chain drives which target chain.",
  {
    asset_path: z.string().describe("Content path of the IK Retargeter"),
    mappings: z.preprocess(jsonPreprocess, z.array(z.any())).describe(
      "[{ target_chain, source_chain }] — e.g. [{\"target_chain\":\"LeftArm\",\"source_chain\":\"arm_l\"}]"
    ),
    save: z.boolean().optional().describe("Save the asset afterwards (default true)"),
  },
  async (args) => rigging.mcpResponse(await rigging.ikRetargeterSetChainMapping(args))
);

server.tool(
  "ue5_controlrig_create",
  [
    "Create a Control Rig blueprint for a skeletal mesh, with the skeleton hierarchy imported into it.",
    "The factory chooses the location; pass asset_path to have it moved there afterwards. Set modular:true for a Modular Rig.",
    "Authoring the rig's node graph is not covered — do that in the Control Rig editor.",
  ].join(" "),
  {
    skeletal_mesh: z.string().describe("Skeletal mesh to build the rig from"),
    asset_path:    z.string().optional().describe("Where the rig should end up; defaults to beside the mesh"),
    modular:       z.boolean().optional().describe("Create a Modular Rig (default false)"),
  },
  async (args) => rigging.mcpResponse(await rigging.controlRigCreate(args))
);

server.tool(
  "ue5_nanite_configure",
  [
    "Turn Nanite on or off for static meshes and tune how it simplifies them (precision, fallback mesh, shape preservation).",
    "The mesh is rebuilt as part of the call, so the result's before/after blocks show what actually changed rather than what was requested.",
    "UE 5.8 note: the old bPreserveArea boolean is now the shape_preservation enum — 'none', 'preserve_area' or 'voxelize'.",
  ].join(" "),
  {
    static_meshes: z.preprocess(jsonPreprocess, z.array(z.string())).describe(
      "Static mesh content paths, e.g. [\"/Game/Meshes/SM_Rock\"]"
    ),
    enabled:                    z.boolean().optional().describe("Enable Nanite on these meshes"),
    position_precision:         z.number().int().optional().describe("Position quantisation; leave unset for Auto"),
    normal_precision:           z.number().int().optional().describe("Normal quantisation; -1 is Auto"),
    tangent_precision:          z.number().int().optional().describe("Tangent quantisation; -1 is Auto"),
    keep_percent_triangles:     z.number().optional().describe("Fraction of triangles kept when building (0..1)"),
    trim_relative_error:        z.number().optional().describe("Error budget for trimming during the build"),
    fallback_target:            z.string().optional().describe("'auto', 'percent_triangles' or 'relative_error' — how the non-Nanite fallback mesh is chosen"),
    fallback_percent_triangles: z.number().optional().describe("Fallback mesh triangle fraction (0..1), with fallback_target 'percent_triangles'"),
    fallback_relative_error:    z.number().optional().describe("Fallback mesh error budget, with fallback_target 'relative_error'"),
    explicit_tangents:          z.boolean().optional().describe("Store tangents rather than deriving them"),
    shape_preservation:         z.string().optional().describe("'none', 'preserve_area' or 'voxelize'"),
    max_edge_length_factor:     z.number().optional().describe("Cap on edge length relative to the mesh size"),
    save:                       z.boolean().optional().describe("Save the assets afterwards (default true)"),
  },
  async (args) => rendering.mcpResponse(await rendering.naniteConfigure(args))
);

server.tool(
  "ue5_lumen_configure",
  [
    "Set Lumen global illumination and reflections for the open level, on an unbounded PostProcessVolume (created if the level has none).",
    "Every post-process property is inert until its override flag is set; this sets the flag for each value you pass and lists them in overrides_set, which is the usual reason 'I changed Lumen settings and nothing happened'.",
    "advanced{} takes any other PostProcessSettings property by its snake_case name and overrides it the same way.",
  ].join(" "),
  {
    volume_label:      z.string().optional().describe("Target a specific PostProcessVolume by actor label; default is the level's unbounded one"),
    create_if_missing: z.boolean().optional().describe("Spawn an unbounded volume if there isn't one (default true)"),
    global_illumination: z.string().optional().describe("'lumen', 'screen_space', 'none' or 'plugin'"),
    reflections:         z.string().optional().describe("'lumen', 'screen_space' or 'none'"),
    ray_lighting_mode:   z.string().optional().describe("'default', 'surface_cache', 'hit_lighting' or 'hit_lighting_for_reflections'"),
    lumen_scene_lighting_quality: z.number().optional().describe("Lumen Scene lighting quality (1 = default)"),
    lumen_scene_detail:           z.number().optional().describe("Lumen Scene detail (1 = default)"),
    lumen_scene_view_distance:    z.number().optional().describe("Lumen Scene view distance in cm"),
    lumen_final_gather_quality:   z.number().optional().describe("Final gather quality (1 = default)"),
    lumen_reflection_quality:     z.number().optional().describe("Reflection quality (1 = default)"),
    lumen_max_reflection_bounces: z.number().int().optional().describe("Reflection bounce count"),
    lumen_max_trace_distance:     z.number().optional().describe("Max trace distance in cm"),
    advanced: z.preprocess(jsonPreprocess, z.record(z.any())).optional().describe(
      "Any other PostProcessSettings property, e.g. {\"lumen_surface_cache_resolution\": 0.75}"
    ),
    save: z.boolean().optional().describe("Save the level afterwards (default true)"),
  },
  async (args) => rendering.mcpResponse(await rendering.lumenConfigure(args))
);

server.tool(
  "ue5_render_read",
  [
    "Report what actually drives the look of the open level: every PostProcessVolume with the settings it overrides, the live rendering console variables, and the project's [/Script/Engine.RendererSettings] ini section.",
    "Pass static_meshes to include their Nanite state (enabled, precision, fallback, triangle counts) in the same answer.",
  ].join(" "),
  {
    static_meshes: z.preprocess(jsonPreprocess, z.array(z.string())).optional().describe(
      "Static mesh paths to report Nanite settings for"
    ),
  },
  async (args) => rendering.mcpResponse(await rendering.renderRead(args))
);

server.tool(
  "ue5_render_project_settings",
  [
    "Change project-wide renderer settings — Nanite, Lumen, virtual shadow maps, Substrate — by writing [/Script/Engine.RendererSettings] in Config/DefaultEngine.ini. A null value removes a key.",
    "UE 5.8 exposes no RendererSettings object to Python, so this is ini text: the running editor keeps its own copy and only picks the file up on the next start. apply_live pushes each key as a console variable so the viewport updates now, but some settings are read-only at runtime.",
    "Use ue5_render_read to see the current values first.",
  ].join(" "),
  {
    settings: z.preprocess(jsonPreprocess, z.record(z.any())).describe(
      "Map of setting to value, e.g. {\"r.DynamicGlobalIlluminationMethod\": 1, \"r.Shadow.Virtual.Enable\": 1}"
    ),
    apply_live: z.boolean().optional().describe("Also run each key as a console command (default true)"),
  },
  async (args) => rendering.mcpResponse(await rendering.renderProjectSettings(args))
);

server.tool(
  "ue5_collision_configure",
  [
    "Set up collision on static meshes: simple primitives, convex decomposition, the trace flag, and the physical material.",
    "Order inside the call is remove -> add -> flags, so remove_existing:true with add_shape replaces the collision rather than clearing what you just added.",
    "trace_flag, physical_material and double_sided_geometry live on the mesh's BodySetup, which only exists once the mesh has collision.",
  ].join(" "),
  {
    static_meshes: z.preprocess(jsonPreprocess, z.array(z.string())).describe(
      "Static mesh content paths"
    ),
    remove_existing:  z.boolean().optional().describe("Clear existing collision first"),
    add_shape:        z.string().optional().describe("'box', 'sphere', 'capsule', 'ndop10_x', 'ndop10_y', 'ndop10_z', 'ndop18' or 'ndop26'"),
    shape_count:      z.number().int().optional().describe("How many of that shape to add (default 1)"),
    convex_hulls:     z.number().int().optional().describe("Run convex decomposition, producing up to this many hulls"),
    convex_max_verts: z.number().int().optional().describe("Max vertices per hull (default 16)"),
    convex_precision: z.number().int().optional().describe("Decomposition precision (default 100000)"),
    trace_flag:       z.string().optional().describe("'use_default', 'use_simple_and_complex', 'use_simple_as_complex' or 'use_complex_as_simple'"),
    physical_material: z.string().optional().describe("Content path of a PhysicalMaterial to bind"),
    double_sided_geometry: z.boolean().optional().describe("Treat complex collision as double sided"),
    save:             z.boolean().optional().describe("Save the assets afterwards (default true)"),
  },
  async (args) => assetconfig.mcpResponse(await assetconfig.collisionConfigure(args))
);

server.tool(
  "ue5_physical_material_create",
  [
    "Create or update a PhysicalMaterial: friction, restitution, density, the combine modes, and the surface type that drives footstep sounds and impact decals.",
    "The combine modes only take effect when their override flag is set, which this does for you.",
    "surface_type values come from [/Script/Engine.PhysicsSettings] in DefaultEngine.ini — a project that declares none has only 'default'.",
  ].join(" "),
  {
    asset_path:      z.string().describe("Destination path, e.g. '/Game/Physics/PM_Ice'"),
    friction:        z.number().optional().describe("Kinetic friction (0..1 typical)"),
    static_friction: z.number().optional().describe("Static friction"),
    restitution:     z.number().optional().describe("Bounciness (0..1)"),
    density:         z.number().optional().describe("Density in g/cm^3"),
    friction_combine_mode:    z.string().optional().describe("'average', 'min', 'multiply' or 'max'"),
    restitution_combine_mode: z.string().optional().describe("'average', 'min', 'multiply' or 'max'"),
    surface_type:    z.string().optional().describe("Surface type name, e.g. 'default'"),
    overwrite:       z.boolean().optional().describe("Update the asset if it already exists (default false)"),
  },
  async (args) => assetconfig.mcpResponse(await assetconfig.physicalMaterialCreate(args))
);

server.tool(
  "ue5_foliage_type_create",
  [
    "Create or update a FoliageType asset for a static mesh — the settings both the foliage paint brush and procedural foliage read: density, radius, scale range, slope limits, cull distance.",
    "Ranges are [min, max] pairs. A FoliageType without a mesh scatters nothing, so static_mesh is required.",
  ].join(" "),
  {
    asset_path:  z.string().describe("Destination path, e.g. '/Game/Foliage/FT_Grass'"),
    static_mesh: z.string().describe("Static mesh this foliage scatters"),
    density:     z.number().optional().describe("Instances per 1000x1000 unit area"),
    radius:      z.number().optional().describe("Minimum distance between instances"),
    scaling:     z.string().optional().describe("'uniform', 'free', 'lock_xy', 'lock_xz' or 'lock_yz'"),
    scale_x:     z.preprocess(jsonPreprocess, z.array(z.number()).length(2)).optional().describe("[min, max] scale"),
    scale_y:     z.preprocess(jsonPreprocess, z.array(z.number()).length(2)).optional().describe("[min, max] scale (non-uniform scaling only)"),
    scale_z:     z.preprocess(jsonPreprocess, z.array(z.number()).length(2)).optional().describe("[min, max] scale (non-uniform scaling only)"),
    align_to_normal:     z.boolean().optional().describe("Align instances to the surface normal"),
    random_yaw:          z.boolean().optional().describe("Randomise yaw"),
    random_pitch_angle:  z.number().optional().describe("Max random pitch in degrees"),
    ground_slope_angle:  z.preprocess(jsonPreprocess, z.array(z.number()).length(2)).optional().describe("[min, max] slope in degrees the mesh may sit on"),
    z_offset:            z.preprocess(jsonPreprocess, z.array(z.number()).length(2)).optional().describe("[min, max] vertical offset"),
    cull_distance:       z.preprocess(jsonPreprocess, z.array(z.number()).length(2)).optional().describe("[start, end] cull distance"),
    collision_with_world: z.boolean().optional().describe("Test against world collision when placing"),
    cast_shadow:         z.boolean().optional().describe("Instances cast shadows"),
    overwrite:           z.boolean().optional().describe("Update the asset if it already exists (default false)"),
  },
  async (args) => assetconfig.mcpResponse(await assetconfig.foliageTypeCreate(args))
);

server.tool(
  "ue5_texture_configure",
  [
    "Set compression and streaming settings on textures — the pass that makes a freshly imported normal map or mask render correctly.",
    "Enum values accept the short form as well as the engine spelling: 'normalmap' or 'TC_NORMALMAP', 'character' or 'TEXTUREGROUP_CHARACTER', 'no_mipmaps' or 'TMGS_NO_MIPMAPS'. A wrong value answers with the full list.",
  ].join(" "),
  {
    textures: z.preprocess(jsonPreprocess, z.array(z.string())).describe("Texture content paths"),
    compression_settings: z.string().optional().describe("'default', 'normalmap', 'masks', 'grayscale', 'hdr', 'bc7', ..."),
    lod_group:            z.string().optional().describe("'world', 'character', 'ui', 'character_normal_map', ..."),
    srgb:                 z.boolean().optional().describe("Treat the source as sRGB (false for normal maps and masks)"),
    max_texture_size:     z.number().int().optional().describe("Clamp the built texture to this size; 0 means no limit"),
    mip_gen_settings:     z.string().optional().describe("'from_texture_group', 'no_mipmaps', 'simple_average', 'sharpen0'..'sharpen10', ..."),
    lod_bias:             z.number().int().optional().describe("LOD bias"),
    never_stream:         z.boolean().optional().describe("Keep fully resident rather than streaming"),
    virtual_texture_streaming: z.boolean().optional().describe("Build as a virtual texture"),
    filter:               z.string().optional().describe("'default', 'nearest', 'bilinear' or 'trilinear'"),
    address_x:            z.string().optional().describe("'wrap', 'clamp' or 'mirror'"),
    address_y:            z.string().optional().describe("'wrap', 'clamp' or 'mirror'"),
    compression_no_alpha: z.boolean().optional().describe("Discard the alpha channel when compressing"),
    flip_green_channel:   z.boolean().optional().describe("Flip green — for normal maps authored in the other convention"),
    save:                 z.boolean().optional().describe("Save the assets afterwards (default true)"),
  },
  async (args) => assetconfig.mcpResponse(await assetconfig.textureConfigure(args))
);

server.tool(
  "ue5_interchange_import",
  [
    "Import a file through Interchange, UE's current import framework, with control over the import pipeline: combine meshes, build Nanite, which skeleton to bind to, whether to create materials, LODs, import offsets.",
    "This is the difference from ue5_import_asset, which uses the legacy importer and can only drop a file into a folder.",
    "source_file is read by the editor process, so it must be an absolute path on the editor's machine. destination_path is a content path like '/Game/Imported'.",
    "Beyond the named arguments, pipeline_settings reaches any pipeline property by dotted path ('mesh_pipeline.nanite_triangle_threshold'). Run ue5_interchange_inspect to list the real names.",
  ].join(" "),
  {
    source_file:      z.string().describe("Absolute path of the file to import, on the editor's machine"),
    destination_path: z.string().describe("Content folder for the result, e.g. '/Game/Imported'"),
    asset_name:       z.string().optional().describe("Name for the imported asset; renamed after import for the asset types Interchange names from the file"),
    pipeline_asset:   z.string().optional().describe("A pipeline saved with ue5_interchange_pipeline_create, used as the base"),
    pipeline_settings: z.preprocess(jsonPreprocess, z.record(z.any())).optional().describe(
      "Any pipeline property by dotted path, e.g. {\"mesh_pipeline.build_nanite\": true}"
    ),
    replace_existing: z.boolean().optional().describe("Overwrite assets already at that path (default true)"),
    import_static_meshes:   z.boolean().optional().describe("Import static meshes from the file"),
    import_skeletal_meshes: z.boolean().optional().describe("Import skeletal meshes from the file"),
    import_animations:      z.boolean().optional().describe("Import animation tracks"),
    import_materials:       z.boolean().optional().describe("Import materials"),
    import_morph_targets:   z.boolean().optional().describe("Import morph targets"),
    import_collision:       z.boolean().optional().describe("Import collision geometry"),
    import_lods:            z.boolean().optional().describe("Import LODs present in the file"),
    import_sockets:         z.boolean().optional().describe("Import sockets"),
    build_nanite:           z.boolean().optional().describe("Build imported static meshes as Nanite"),
    create_physics_asset:   z.boolean().optional().describe("Create a physics asset for skeletal meshes"),
    create_new_materials:   z.boolean().optional().describe("Create materials rather than only reusing existing ones"),
    reuse_existing_materials: z.boolean().optional().describe("Reuse materials already in the project when names match"),
    recompute_normals:      z.boolean().optional().describe("Recompute normals instead of using the file's"),
    recompute_tangents:     z.boolean().optional().describe("Recompute tangents"),
    combine_static_meshes_behavior:   z.string().optional().describe("'all', 'visible_only' or 'do_not_combine'"),
    combine_skeletal_meshes_behavior: z.string().optional().describe("'by_skeleton', 'by_skeleton_visible_only' or 'do_not_combine'"),
    skeleton:         z.string().optional().describe("Existing Skeleton asset to bind a skeletal mesh or animation to"),
    parent_material:  z.string().optional().describe("Parent material for created material instances"),
    import_offset_translation:   z.preprocess(jsonPreprocess, z.array(z.number()).length(3)).optional().describe("[x, y, z] offset applied on import"),
    import_offset_rotation:      z.preprocess(jsonPreprocess, z.array(z.number()).length(3)).optional().describe("[pitch, yaw, roll] applied on import"),
    import_offset_uniform_scale: z.number().optional().describe("Uniform scale applied on import"),
  },
  async (args) => interchange.mcpResponse(await interchange.interchangeImport(args))
);

server.tool(
  "ue5_interchange_inspect",
  [
    "Report whether Interchange can import a given file and which translator handles it, and list the pipeline property names available to ue5_interchange_import's pipeline_settings.",
    "Run this before an unfamiliar import: the property list is the ground truth for this engine build, and it already excludes names the engine has deprecated.",
  ].join(" "),
  {
    source_file: z.string().optional().describe("Absolute path of a file to test; omit to only list pipeline properties"),
    list_pipeline_properties: z.boolean().optional().describe("Include the pipeline property listing (default true)"),
  },
  async (args) => interchange.mcpResponse(await interchange.interchangeInspect(args))
);

server.tool(
  "ue5_interchange_pipeline_create",
  [
    "Save a configured Interchange pipeline as an asset so the same import settings can be reused and version-controlled, then pass it to ue5_interchange_import as pipeline_asset.",
    "Takes the same named settings as ue5_interchange_import plus pipeline_settings for anything else. Per-import overrides are applied to a throwaway copy, so a saved pipeline is never modified behind your back.",
  ].join(" "),
  {
    asset_path: z.string().describe("Destination path, e.g. '/Game/Import/PL_CharacterFBX'"),
    pipeline_settings: z.preprocess(jsonPreprocess, z.record(z.any())).optional().describe(
      "Any pipeline property by dotted path, e.g. {\"mesh_pipeline.build_nanite\": true}"
    ),
    overwrite: z.boolean().optional().describe("Update the asset if it already exists (default false)"),
    import_static_meshes:   z.boolean().optional().describe("Import static meshes from the file"),
    import_skeletal_meshes: z.boolean().optional().describe("Import skeletal meshes from the file"),
    import_animations:      z.boolean().optional().describe("Import animation tracks"),
    import_materials:       z.boolean().optional().describe("Import materials"),
    import_lods:            z.boolean().optional().describe("Import LODs present in the file"),
    build_nanite:           z.boolean().optional().describe("Build imported static meshes as Nanite"),
    create_physics_asset:   z.boolean().optional().describe("Create a physics asset for skeletal meshes"),
    combine_static_meshes_behavior: z.string().optional().describe("'all', 'visible_only' or 'do_not_combine'"),
    skeleton:               z.string().optional().describe("Existing Skeleton asset to bind to"),
    import_offset_uniform_scale: z.number().optional().describe("Uniform scale applied on import"),
  },
  async (args) => interchange.mcpResponse(await interchange.interchangePipelineCreate(args))
);

server.tool(
  "ue5_substrate_material_create",
  [
    "Build a native Substrate material — a BSDF wired into the material's FrontMaterial output — instead of the legacy BaseColor/Metallic/Roughness graph ue5_create_material makes.",
    "Substrate's Slab BSDF has no BaseColor or Metallic pin: it takes Diffuse Albedo and F0. Passing base_color/metallic/specular inserts the SubstrateMetalnessToDiffuseAlbedoF0 node that bridges them, so the familiar three values still work.",
    "use_parameters exposes named scalar/vector/texture parameters so material instances can drive the result.",
    "Requires Substrate to be on in the project; if it is off the tool says so rather than building a graph that compiles to nothing.",
  ].join(" "),
  {
    asset_path: z.string().describe("Destination path, e.g. '/Game/Materials/M_Metal'"),
    shading:    z.string().optional().describe("'slab' (default), 'clearcoat', 'unlit', 'toon', 'hair', 'eye' or 'water'"),
    base_color: z.preprocess(jsonPreprocess, z.array(z.number()).min(3).max(4)).optional().describe("[r, g, b] linear colour"),
    metallic:   z.number().optional().describe("0 = dielectric, 1 = metal"),
    specular:   z.number().optional().describe("Specular level (0.5 is the neutral default)"),
    roughness:  z.number().optional().describe("0 = mirror, 1 = fully rough"),
    emissive_color: z.preprocess(jsonPreprocess, z.array(z.number()).min(3).max(4)).optional().describe("[r, g, b] emissive"),
    base_color_texture: z.string().optional().describe("Texture asset driving base colour instead of a constant"),
    normal_texture:     z.string().optional().describe("Normal map texture asset"),
    use_parameters:     z.boolean().optional().describe("Expose the values as material parameters (default false)"),
    blend_mode: z.string().optional().describe("'opaque' (default), 'masked', 'translucent', 'additive', 'modulate', 'alpha_composite', 'alpha_holdout'"),
    two_sided:  z.boolean().optional().describe("Render both faces"),
    overwrite:  z.boolean().optional().describe("Replace an existing asset at that path"),
  },
  async (args) => substrate.mcpResponse(await substrate.substrateMaterialCreate(args))
);

server.tool(
  "ue5_substrate_read",
  [
    "Report whether Substrate is enabled in the project, and — given a Material — which Substrate nodes its graph contains, their input pins, and the parameters it exposes.",
    "Call it with no asset_path to answer just 'is this a Substrate project'.",
  ].join(" "),
  {
    asset_path: z.string().optional().describe("Material to inspect; omit to report only the project state"),
  },
  async (args) => substrate.mcpResponse(await substrate.substrateRead(args))
);

server.tool(
  "ue5_landscape_read",
  [
    "Report the landscapes in the open level: material, hole material, paint layers with the LayerInfo asset bound to each, edit layers, and which actors are streaming proxies rather than the parent landscape.",
    "If the level has none, the answer says so — UE 5.8 cannot create a landscape from script (ALandscape::Import is not exposed to Python, and spawning the class gives a LandscapePlaceholder), so that first step happens in Landscape mode.",
  ].join(" "),
  {
    include_proxies: z.boolean().optional().describe("Also list World Partition streaming proxies (default false)"),
  },
  async (args) => landscape.mcpResponse(await landscape.landscapeRead(args))
);

server.tool(
  "ue5_landscape_setup",
  [
    "Assign a landscape's material and bind its paint layers, creating the LandscapeLayerInfoObject asset each named layer needs.",
    "A paint layer declared in the landscape material does nothing until a LayerInfo asset is bound to it on that landscape — this is the '+' next to a layer in Landscape mode, done in bulk.",
    "Needs a landscape that already exists; see ue5_landscape_read.",
  ].join(" "),
  {
    landscape_label: z.string().optional().describe("Which landscape, by actor label; required only when the level has more than one"),
    material:        z.string().optional().describe("Landscape material to assign"),
    hole_material:   z.string().optional().describe("Landscape hole material to assign"),
    layers: z.preprocess(jsonPreprocess, z.array(z.any())).optional().describe(
      "Paint layers: [{ name, layer_info_path?, hardness?, phys_material?, debug_color? }] — name must match the layer name in the material"
    ),
    layer_info_folder: z.string().optional().describe("Where new LayerInfo assets go; defaults to a LayerInfo folder beside the level"),
    save: z.boolean().optional().describe("Save the level afterwards (default true)"),
  },
  async (args) => landscape.mcpResponse(await landscape.landscapeSetup(args))
);

server.tool(
  "ue5_metasound_create",
  [
    "Build a MetaSound Source or Patch and save it as an asset. The whole graph goes in ONE call: nodes[], connections[] and graph_inputs[].",
    "Why one call: UE 5.8's MetaSound scripting API cannot find a node again in a later call (there is no node enumeration), so nodes are addressed by ids you choose inside this request — same shape as ue5_bp_add_logic.",
    "A node class is namespace/name/variant. Give just the name ('Sine', 'Saw', 'Noise', 'Stereo Delay', 'Trigger Repeat') and the variant is auto-detected: audio-rate nodes use 'Audio', trigger and maths nodes use none. Prefix a namespace with a dot if it is not UE.",
    "connections: {from: 'nodeId.OutputName', to: 'nodeId.InputName'}. `to` also accepts 'audio_out' (the source's audio output) and 'graph_output:Name'.",
    "The result lists every node's real input and output pin names — read it before wiring a second graph.",
  ].join(" "),
  {
    asset_path:    z.string().describe("Destination content path, e.g. '/Game/Audio/MS_Engine'"),
    type:          z.string().optional().describe("'source' (playable, default) or 'patch' (reusable graph)"),
    output_format: z.string().optional().describe("'mono' (default) or 'stereo' — sources only"),
    one_shot:      z.boolean().optional().describe("Source finishes itself rather than looping (default true)"),
    nodes: z.preprocess(jsonPreprocess, z.array(z.any())).optional().describe(
      "Array of { id, class, variant?, version?, inputs? } — e.g. [{\"id\":\"osc\",\"class\":\"Sine\",\"inputs\":{\"Frequency\":220.0}}]"
    ),
    connections: z.preprocess(jsonPreprocess, z.array(z.any())).optional().describe(
      "Array of { from, to } — e.g. [{\"from\":\"osc.Audio\",\"to\":\"audio_out\"}]"
    ),
    graph_inputs: z.preprocess(jsonPreprocess, z.array(z.any())).optional().describe(
      "Parameters the sound exposes: [{ name, type, default }], type is a MetaSound data type (Float, Bool, Int32, String, Trigger, Audio)"
    ),
    author: z.string().optional().describe("Author recorded on the asset (default 'NGG')"),
  },
  async (args) => metasound.mcpResponse(await metasound.metasoundCreate(args))
);

server.tool(
  "ue5_metasound_read",
  [
    "Report what an existing MetaSound exposes: type, output format, and its graph inputs and outputs.",
    "Note the limit, which is the API's and not this tool's: UE 5.8 cannot enumerate the nodes inside a MetaSound graph from scripting, so this shows the interface, not the node network.",
  ].join(" "),
  {
    asset_path: z.string().describe("Content path of the MetaSound Source or Patch"),
  },
  async (args) => metasound.mcpResponse(await metasound.metasoundRead(args))
);

server.tool(
  "ue5_metasound_set_graph_input_defaults",
  "Retune an existing MetaSound: change the default values of the graph inputs it exposes, without rebuilding the graph. Use ue5_metasound_read to see the input names.",
  {
    asset_path: z.string().describe("Content path of the MetaSound"),
    inputs: z.preprocess(jsonPreprocess, z.record(z.any())).describe(
      "Map of input name to value, e.g. {\"Frequency\": 440.0, \"Gain\": 0.5}"
    ),
    save: z.boolean().optional().describe("Save the asset afterwards (default true)"),
  },
  async (args) => metasound.mcpResponse(await metasound.metasoundSetGraphInputDefaults(args))
);

server.tool(
  "ue5_create_sound_cue",
  "Create a SoundCue asset from an imported SoundWave (WavePlayer wired automatically). Import the .wav first with ue5_import_asset. Optional volume/pitch/looping.",
  {
    asset_path: z.string().describe("Destination content path, e.g. '/Game/Audio/SC_Explosion'"),
    sound_wave: z.string().describe("Content path of the source SoundWave"),
    volume:     z.number().optional().describe("Volume multiplier (default 1.0)"),
    pitch:      z.number().optional().describe("Pitch multiplier (default 1.0)"),
    looping:    z.boolean().optional().describe("Loop the wave"),
    overwrite:  z.boolean().optional().describe("Replace an existing asset at that path"),
  },
  async (args) => gamedev.mcpResponse(await gamedev.createSoundCue(args))
);

server.tool(
  "ue5_add_gameplay_tags",
  "Register new gameplay tags in Config/DefaultGameplayTags.ini (skips tags that already exist) and attempt a live tag-tree refresh. If the live refresh isn't supported, the tags load on next editor start.",
  {
    tags: z.array(z.object({
      tag:     z.string().describe("Tag name, e.g. 'Ability.Dash.Cooldown'"),
      comment: z.string().optional().describe("DevComment shown in the tag picker"),
    })).describe("Tags to add"),
  },
  async (args) => gamedev.mcpResponse(await gamedev.addGameplayTags(args))
);

server.tool(
  "ue5_add_component_to_actor",
  "Add a component to a level actor INSTANCE (not a Blueprint class — use ue5_add_component_to_blueprint for that) via AddComponentByClass, with optional initial properties.",
  {
    actor_label:     z.string().describe("Label of the level actor"),
    component_class: z.string().describe("Component class name, e.g. 'PointLightComponent', 'AudioComponent'"),
    component_label: z.string().optional().describe("Name for the new component"),
    properties:      z.record(z.any()).optional().describe("Editor properties to set; object refs as asset paths"),
  },
  async (args) => gamedev.mcpResponse(await gamedev.addComponentToActor(args))
);

// ===========================================================================
// Audio (round 2)
// ===========================================================================

server.tool(
  "ue5_create_sound_attenuation",
  "Create a SoundAttenuation asset with the common distance-falloff settings — assign it to sounds/components so audio falls off with distance.",
  {
    asset_path:       z.string().describe("Destination content path, e.g. '/Game/Audio/ATT_Footsteps'"),
    falloff_distance: z.number().optional().describe("Distance over which volume fades to silence (uu)"),
    inner_radius:     z.number().optional().describe("Full-volume inner radius (uu)"),
    shape:            z.string().optional().describe("Attenuation shape: Sphere, Capsule, Box, Cone"),
    spatialize:       z.boolean().optional().describe("3D-position the sound (default true)"),
    overwrite:        z.boolean().optional().describe("Replace an existing asset at that path"),
  },
  async (args) => gamedev.mcpResponse(await gamedev.createSoundAttenuation(args))
);

server.tool(
  "ue5_spawn_ambient_sound",
  "Spawn an AmbientSound actor in the level playing a SoundWave/SoundCue, with optional volume, pitch, and attenuation asset.",
  {
    sound:         z.string().describe("Content path of the SoundWave or SoundCue"),
    location:      z.array(z.number()).length(3).optional().describe("[X, Y, Z] (default [0,0,100])"),
    actor_label:   z.string().optional().describe("Label for the spawned actor"),
    volume:        z.number().optional().describe("Volume multiplier"),
    pitch:         z.number().optional().describe("Pitch multiplier"),
    attenuation:   z.string().optional().describe("Content path of a SoundAttenuation asset"),
    auto_activate: z.boolean().optional().describe("Play automatically on level start (default true)"),
  },
  async (args) => gamedev.mcpResponse(await gamedev.spawnAmbientSound(args))
);

server.tool(
  "ue5_play_sound_preview",
  "Play a sound once through the editor's audio device so the user can hear it (audible on the editor machine only).",
  {
    sound:  z.string().describe("Content path of the SoundWave or SoundCue"),
    volume: z.number().optional().describe("Volume multiplier (default 1.0)"),
  },
  async (args) => gamedev.mcpResponse(await gamedev.playSoundPreview(args))
);

// ===========================================================================
// UMG widgets (round 2)
// ===========================================================================

server.tool(
  "ue5_set_widget_properties",
  "Set arbitrary editor properties on a named widget inside a Widget Blueprint's tree (beyond ue5_style_widgets' whitelist). Colors as [r,g,b,a], enums by name, object refs as paths, UImage textures via key 'brush_texture'. Compile with ue5_compile_widget_blueprint afterwards.",
  {
    widget_blueprint: z.string().describe("Content path of the Widget Blueprint"),
    widget_name:      z.string().describe("Widget name in the tree"),
    properties:       z.record(z.any()).describe("Editor properties to set"),
  },
  async (args) => gamedev.mcpResponse(await gamedev.setWidgetProperties(args))
);

server.tool(
  "ue5_widget_bind_event",
  "Bind a widget's delegate (e.g. a Button's OnClicked) to a new event node in the Widget Blueprint's graph. Marks the widget as a variable, adds the K2Node_ComponentBoundEvent, and returns its node id — wire the handler body with ue5_bp_add_logic.",
  {
    widget_blueprint: z.string().describe("Content path of the Widget Blueprint"),
    widget_name:      z.string().describe("Widget name in the tree, e.g. 'StartButton'"),
    delegate:         z.string().describe("Delegate to bind, e.g. 'OnClicked', 'OnValueChanged', 'OnTextCommitted'"),
    node_id:          z.string().optional().describe("Id for the new event node (default '<widget>_<delegate>')"),
  },
  async ({ widget_blueprint, widget_name, delegate, node_id }) => {
    try {
      // Flag the widget as a BP variable via the C++ style route (UWidget's
      // bIsVariable isn't Python-exposed), then compile so the graph sees it.
      await ue5.styleWidgets(widget_blueprint, [{ widget_name, is_variable: true }]);
      await ue5.compileWidgetBlueprint(widget_blueprint);
      const id = node_id || `${widget_name}_${delegate}`;
      const result = await ue5.bpAddLogic({
        blueprint: widget_blueprint,
        nodes: [{ id, type: "component_event", component: widget_name, delegate }],
        connections: [],
        compile: true,
      });
      return { content: [{ type: "text", text: JSON.stringify({ ...result, node_id: id }, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ===========================================================================
// Gameplay Ability System (round 2)
// ===========================================================================

server.tool(
  "ue5_gas_add_attribute",
  "Append a GameplayAttributeData attribute to an existing AttributeSet Blueprint (use ue5_gas_create_attribute_set's attributes param for initial creation).",
  {
    blueprint: z.string().describe("Content path of the AttributeSet Blueprint"),
    name:      z.string().describe("Attribute name, e.g. 'Stamina'"),
    save:      z.boolean().optional().describe("Save after compiling (default true)"),
  },
  async ({ blueprint, name, save }) => {
    try {
      const result = await ue5.bpCreateVariable({
        blueprint,
        name,
        type: "struct:GameplayAttributeData",
        compile: true,
        save: save !== false,
      });
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

server.tool(
  "ue5_gas_list_attributes",
  "List the GameplayAttributeData attributes (name + base value) of an AttributeSet Blueprint or native class.",
  {
    asset_path: z.string().describe("Content path of the AttributeSet Blueprint (or /Script class path)"),
  },
  async (args) => gamedev.mcpResponse(await gamedev.gasListAttributes(args))
);

server.tool(
  "ue5_gas_grant_on_beginplay",
  "Wire the standard GAS startup into an actor Blueprint's event graph: BeginPlay → GiveAbility for each ability class → ApplyGameplayEffectToSelf for each effect class, targeting the BP's AbilitySystemComponent. The BP must already have an ASC (ue5_gas_setup_actor).",
  {
    blueprint_path: z.string().describe("Content path of the actor Blueprint with an ASC"),
    abilities:      z.array(z.string()).optional().describe("GameplayAbility BP paths to grant, e.g. ['/Game/GAS/GA_Dash']"),
    effects:        z.array(z.string()).optional().describe("GameplayEffect BP paths to apply to self on startup"),
  },
  async ({ blueprint_path, abilities = [], effects = [] }) => {
    try {
      if (!abilities.length && !effects.length) {
        return { content: [{ type: "text", text: "provide at least one ability or effect" }], isError: true };
      }
      const setup = await ue5.gasReadSetup({ blueprint_path });
      const ascName =
        setup?.ability_system_component?.component_name ??
        setup?.asc?.component_name ??
        setup?.component_name;
      if (!ascName) {
        return {
          content: [{ type: "text", text: `no AbilitySystemComponent found on ${blueprint_path} — run ue5_gas_setup_actor first (read_setup: ${JSON.stringify(setup)})` }],
          isError: true,
        };
      }
      const asClass = (p) => (p.includes(".") ? p : `${p}.${p.split("/").pop()}_C`);
      const nodes = [
        { id: "ngg_gas_begin", type: "event", event: "ReceiveBeginPlay" },
        { id: "ngg_gas_asc", type: "variable_get", variable: ascName },
      ];
      const connections = [];
      let prev = "ngg_gas_begin.then";
      abilities.forEach((a, i) => {
        const id = `ngg_gas_give_${i}`;
        nodes.push({
          id, type: "call_function", function: "K2_GiveAbility",
          class: "/Script/GameplayAbilities.AbilitySystemComponent",
          defaults: { AbilityClass: asClass(a) },
        });
        connections.push({ from: prev, to: `${id}.execute` });
        connections.push({ from: `ngg_gas_asc.${ascName}`, to: `${id}.target` });
        prev = `${id}.then`;
      });
      effects.forEach((e, i) => {
        const id = `ngg_gas_apply_${i}`;
        nodes.push({
          id, type: "call_function", function: "BP_ApplyGameplayEffectToSelf",
          class: "/Script/GameplayAbilities.AbilitySystemComponent",
          defaults: { GameplayEffectClass: asClass(e), Level: "1" },
        });
        connections.push({ from: prev, to: `${id}.execute` });
        connections.push({ from: `ngg_gas_asc.${ascName}`, to: `${id}.target` });
        prev = `${id}.then`;
      });
      const result = await ue5.bpAddLogic({
        blueprint: blueprint_path, nodes, connections, auto_layout: true, compile: true,
      });
      return { content: [{ type: "text", text: JSON.stringify({ ...result, asc_component: ascName }, null, 2) }] };
    } catch (err) {
      return { content: [{ type: "text", text: err.message }], isError: true };
    }
  }
);

// ---------------------------------------------------------------------------
// Toolset summary
// ---------------------------------------------------------------------------
// Printed after every registration so the user can see what a NGG_TOOLSETS
// setting actually cost them. Silent when nothing was filtered.
if (skippedToolsets.size > 0) {
  const skippedTotal = [...skippedToolsets.values()].reduce((a, b) => a + b, 0);
  const detail = [...skippedToolsets.entries()]
    .sort((a, b) => b[1] - a[1])
    .map(([set, n]) => `${set}(${n})`)
    .join(" ");
  console.error(
    `[ue5-ngg] toolsets ${TOOLSETS.mode}: registered ${[...TOOLSETS.enabled].sort().join(",")} — ` +
    `skipped ${skippedTotal} tool(s): ${detail}`
  );
}

// ---------------------------------------------------------------------------
// Start the server
// ---------------------------------------------------------------------------
// Default: stdio transport (Claude Code spawns this as a child process via .mcp.json).
// Alternate: WebSocket transport for persistent connections.
//   NGG_MCP_TRANSPORT=ws NGG_MCP_WS_PORT=6777 node index.js
// ---------------------------------------------------------------------------

if (TRANSPORT_MODE === "ws") {
  // Lazy-import the WebSocket server transport to avoid requiring the package
  // when running in default stdio mode (ws is an optional peer dependency).
  let WebSocketServerTransport;
  try {
    ({ WebSocketServerTransport } = await import("@modelcontextprotocol/sdk/server/websocket.js"));
  } catch {
    process.stderr.write(
      "[unrealngg-mcp] ERROR: WebSocket transport requires @modelcontextprotocol/sdk >= 1.1.0 " +
      "with WebSocket support. Run: npm install @modelcontextprotocol/sdk@latest\n"
    );
    process.exit(1);
  }

  const wsTransport = new WebSocketServerTransport({ port: WS_PORT });
  await server.connect(wsTransport);
  process.stderr.write(`[unrealngg-mcp] WebSocket MCP server listening on ws://localhost:${WS_PORT}\n`);
} else {
  const transport = new StdioServerTransport();
  await server.connect(transport);
  // Process exits when parent closes stdin — no explicit keep-alive needed.
}
