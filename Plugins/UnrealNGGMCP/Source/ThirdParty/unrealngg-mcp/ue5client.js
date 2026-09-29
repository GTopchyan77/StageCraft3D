// Copyright 2025-2026 NGG. All Rights Reserved.
// ue5client.js — unrealngg-mcp
// HTTP client wrapper for the UnrealNGGMCP UE5 plugin.
// All functions are async and throw on non-2xx responses.

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

// Env vars are resolved per-call (not at module load) so tests can override
// them after import and callers get the current value at request time.
export const DEFAULT_BRIDGE_URL = "http://localhost:6776";
export const DEFAULT_TIMEOUT_MS = 15000;

// Sidecar lives at <Project>/Plugins/UnrealNGGMCP/Source/ThirdParty/unrealngg-mcp/ue5client.js.
// Five levels up — unrealngg-mcp, ThirdParty, Source, UnrealNGGMCP, Plugins —
// lands on the project root that owns this copy of the plugin, the same project
// whose editor wrote bridge.json under Saved/.
//
// That walk assumes the standard <Project>/Plugins/<PluginFolder> layout (the
// folder name itself is irrelevant — only the depth counts). NGG_PROJECT_ROOT
// covers the layouts where it does not hold: a plugin grouped one level deeper
// under Plugins/, or one left in the engine. The editor writes that variable
// into the generated .mcp.json only for those cases, so the usual install keeps
// a config with no machine-specific absolute paths in it.
const __dirname = path.dirname(fileURLToPath(import.meta.url));
const WALKED_PROJECT_ROOT = path.resolve(__dirname, "..", "..", "..", "..", "..");
function projectRoot() {
  return process.env.NGG_PROJECT_ROOT
    ? path.resolve(process.env.NGG_PROJECT_ROOT)
    : WALKED_PROJECT_ROOT;
}
let discoveryPathOverride = null;
function getDiscoveryPath() {
  return discoveryPathOverride ?? path.join(projectRoot(), "Saved", "UnrealNGGMCP", "bridge.json");
}

// The plugin reads its auth token from GConfig under [UnrealNGGMCP] AuthToken=…
// so we read the same ini layers, in GConfig's own precedence order:
//
//   Config/UserEngine.ini    — `GameDirUser`, the LAST entry in GConfigLayers
//                              (Engine/Source/Runtime/Core/Public/Misc/
//                              ConfigHierarchy.h), so it overrides everything
//                              below and the editor never rewrites it.
//   Config/DefaultEngine.ini — `ProjectDefault`; tracked in git.
//
// The secret belongs in the UserEngine.ini layer: it is gitignored, while
// DefaultEngine.ini is tracked and would publish the token into history.
// Reading both means the sidecar and the live test suites authenticate against
// this project's editor out of the box, with no NGG_BRIDGE_TOKEN in the
// environment, and installs that still keep their token in DefaultEngine.ini
// keep working.
//
// Built per call rather than at import, for the same reason getDiscoveryPath is:
// NGG_PROJECT_ROOT may be set after this module loads.
let iniPathOverride = null;   // null = use the real layers; otherwise an array
function getIniPaths() {
  return iniPathOverride ?? [
    path.join(projectRoot(), "Config", "UserEngine.ini"),
    path.join(projectRoot(), "Config", "DefaultEngine.ini"),
  ];
}

// Cache the discovered URL briefly so back-to-back calls don't hammer the
// filesystem, but stay short enough to pick up a port change after an editor
// restart without forcing a sidecar restart.
const DISCOVERY_CACHE_MS = 2000;
let discoveryCache = null;
let discoveryCacheAt = 0;

function readDiscoveryFile() {
  const now = Date.now();
  if (discoveryCache !== null && (now - discoveryCacheAt) < DISCOVERY_CACHE_MS) {
    return discoveryCache;
  }
  discoveryCacheAt = now;
  try {
    const text = fs.readFileSync(getDiscoveryPath(), "utf8");
    const parsed = JSON.parse(text);
    if (parsed && Number.isFinite(parsed.port) && parsed.port > 0 && parsed.port < 65536) {
      discoveryCache = `http://localhost:${parsed.port}`;
      return discoveryCache;
    }
  } catch {
    // File missing or unparseable — silently fall through. Editor may not be
    // running yet, or this is an older plugin version without discovery.
  }
  discoveryCache = null;
  return null;
}

function bridgeUrl() {
  // Resolution order:
  //   1. NGG_BRIDGE_URL env (explicit override — used by tests and power users)
  //   2. <Project>/Saved/UnrealNGGMCP/bridge.json (written by the plugin)
  //   3. DEFAULT_BRIDGE_URL fallback (editor not yet started, or pre-discovery plugin)
  if (process.env.NGG_BRIDGE_URL) return process.env.NGG_BRIDGE_URL;
  const discovered = readDiscoveryFile();
  if (discovered) return discovered;
  return DEFAULT_BRIDGE_URL;
}

function timeoutMs()  {
  const parsed = parseInt(process.env.NGG_BRIDGE_TIMEOUT_MS || String(DEFAULT_TIMEOUT_MS), 10);
  // A non-numeric env value yields NaN; setTimeout(.., NaN) fires immediately
  // and aborts every request. Fall back to the default in that case.
  return Number.isNaN(parsed) ? DEFAULT_TIMEOUT_MS : parsed;
}
// Token from DefaultEngine.ini, cached like the discovery file so per-request
// resolution doesn't re-read the ini, but an edit is picked up within seconds.
let iniTokenCache = null;
let iniTokenCacheAt = 0;

// Pull [UnrealNGGMCP] AuthToken out of one ini file. Returns "" when the file
// is missing, unreadable, or has no token — the caller then tries the next
// layer.
function parseIniToken(iniPath) {
  try {
    const text = fs.readFileSync(iniPath, "utf8");
    let inSection = false;
    for (const rawLine of text.split(/\r?\n/)) {
      const line = rawLine.trim();
      if (line.startsWith("[")) {
        inSection = line === "[UnrealNGGMCP]";
        continue;
      }
      if (!inSection) continue;
      const m = line.match(/^AuthToken\s*=\s*(\S+)/);
      if (m) return m[1];
    }
  } catch {
    // Ini missing or unreadable — no token from this layer.
  }
  return "";
}

function readIniToken() {
  const now = Date.now();
  if (iniTokenCache !== null && (now - iniTokenCacheAt) < DISCOVERY_CACHE_MS) {
    return iniTokenCache;
  }
  iniTokenCacheAt = now;
  iniTokenCache = "";
  // First layer that carries a token wins, matching GConfig's precedence
  // (UserEngine.ini overrides DefaultEngine.ini). If none do, requests go out
  // unauthenticated — which still works against a bridge that has no AuthToken
  // configured, minus exec_python and the lifecycle routes.
  for (const iniPath of getIniPaths()) {
    const token = parseIniToken(iniPath);
    if (token) {
      iniTokenCache = token;
      break;
    }
  }
  return iniTokenCache;
}

function authToken()  { return process.env.NGG_BRIDGE_TOKEN      || readIniToken(); }

/**
 * Explains a 401 in terms of where this process got its token, so the failure
 * points at the file to fix instead of just saying "unauthorized".
 *
 * The editor writes an AuthToken into <Project>/Config/UserEngine.ini the first
 * time it runs in a project that has none, so the usual causes of a mismatch
 * are a stale NGG_BRIDGE_TOKEN overriding that file, or a sidecar pointed at a
 * different project's ini than the editor it is talking to.
 */
function authMismatchHint() {
  // Name the layer the token actually came from; fall back to the one the
  // editor provisions into when no layer carries a token at all.
  const paths   = getIniPaths();
  const iniPath = paths.find(p => parseIniToken(p)) ?? paths[0];
  if (process.env.NGG_BRIDGE_TOKEN) {
    return (
      `NGG_BRIDGE_TOKEN is set in this process's environment and takes precedence over ` +
      `${iniPath}, where the editor keeps the token it actually expects ([UnrealNGGMCP] AuthToken). ` +
      `Copy that value into NGG_BRIDGE_TOKEN, or drop the env var (e.g. from .mcp.json) so the ini is used.`
    );
  }
  if (readIniToken()) {
    return (
      `The token sent came from ${iniPath} ([UnrealNGGMCP] AuthToken) and the editor rejected it — ` +
      `this sidecar is most likely pointed at a different project than the editor on the bridge port. ` +
      `Check that ini against the editor's project, then restart the editor.`
    );
  }
  return (
    `No token was sent: ${iniPath} has no [UnrealNGGMCP] AuthToken and NGG_BRIDGE_TOKEN is unset. ` +
    `Restart the Unreal Editor — it writes a token there on first run — or set NGG_BRIDGE_TOKEN to the editor's token.`
  );
}

// Read at import for anything that needs a stable banner/error line. Will
// reflect whatever was set when the server started; callers wanting the
// live value should call bridgeUrl() directly.
export function getBridgeUrl() { return bridgeUrl(); }

// Exposed for tests — clears the discovery file cache so an updated bridge.json
// is picked up on the next call without waiting for DISCOVERY_CACHE_MS.
export function _resetDiscoveryCacheForTests() {
  discoveryCache = null;
  discoveryCacheAt = 0;
}

// Exposed for tests — lets the test harness see where the sidecar looks for
// the discovery file by default.
export function _getDiscoveryPathForTests() {
  return getDiscoveryPath();
}

// Exposed for tests — lets the test harness see every ini layer the sidecar
// consults for the token fallback, in precedence order.
export function _getIniPathsForTests() {
  return getIniPaths();
}

// Exposed for tests — point discovery at a temp file instead of the real
// <Project>/Saved/UnrealNGGMCP/bridge.json so the test suite doesn't collide
// with a live editor's discovery file. Pass null to restore the default.
export function _setDiscoveryPathForTests(p) {
  discoveryPathOverride = p;
  discoveryCache = null;
  discoveryCacheAt = 0;
}

// Exposed for tests — collapse the ini token fallback to a single temp file
// instead of the real UserEngine/DefaultEngine layers. Pass null to restore.
export function _setIniPathForTests(p) {
  _setIniPathsForTests(p === null ? null : [p]);
}

// Exposed for tests — override the whole layer list, so precedence between
// Saved/ and Config/ can be exercised. Pass null to restore the real layers.
export function _setIniPathsForTests(paths) {
  iniPathOverride = paths;
  iniTokenCache = null;
  iniTokenCacheAt = 0;
}

// Exposed for tests — the resolved token (env var or ini fallback).
export function _getAuthTokenForTests() {
  return authToken();
}

/**
 * Where the auth token is coming from, for diagnostics. Never returns the token
 * itself — callers surface this to the model and to logs.
 *
 * Without a token the bridge still serves every ordinary route, but exec_python
 * and the lifecycle routes stay disabled, which takes every pcg_* tool with
 * them. `disables` names that blast radius so the caller doesn't have to.
 *
 * @returns {{configured: boolean, source: string|null, disables: string[]}}
 */
export function getAuthTokenStatus() {
  const disables = ["exec_python", "all pcg_* tools", "shutdown", "build_and_run", "kill_and_restart"];
  if (process.env.NGG_BRIDGE_TOKEN) {
    return { configured: true, source: "NGG_BRIDGE_TOKEN env", disables: [] };
  }
  for (const iniPath of getIniPaths()) {
    if (parseIniToken(iniPath)) {
      return { configured: true, source: path.basename(iniPath), disables: [] };
    }
  }
  return { configured: false, source: null, disables };
}

// ---------------------------------------------------------------------------
// Internal fetch helper
// ---------------------------------------------------------------------------

/**
 * @param {string} path   - URL path, must start with /
 * @param {object} opts   - fetch options (method, body already stringified, etc.)
 * @returns {Promise<any>} - parsed JSON response body
 */
async function bridgeFetch(path, opts = {}) {
  const base  = bridgeUrl();
  const tmo   = timeoutMs();
  const token = authToken();
  const url   = `${base}${path}`;

  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), tmo);

  let response;
  try {
    response = await fetch(url, {
      ...opts,
      signal: controller.signal,
      headers: {
        "Content-Type":  "application/json",
        "Accept":        "application/json",
        ...(token ? { "Authorization": `Bearer ${token}` } : {}),
        ...(opts.headers ?? {}),
      },
    });
  } catch (err) {
    if (err.name === "AbortError") {
      throw new Error(
        `UE5 bridge request timed out after ${tmo}ms. ` +
        `Is the Unreal Editor open with the UnrealNGGMCP plugin active?`
      );
    }
    throw new Error(
      `UE5 bridge connection failed: ${err.message}. ` +
      `Ensure the Unreal Editor is running and the UnrealNGGMCP plugin is loaded (${base}).`
    );
  } finally {
    clearTimeout(timer);
  }

  // Parse body regardless of status — error bodies carry useful messages
  let body;
  const contentType = response.headers.get("content-type") ?? "";
  if (contentType.includes("application/json")) {
    body = await response.json();
  } else {
    body = await response.text();
  }

  if (!response.ok) {
    const message = (typeof body === "object" && body?.error)
      ? body.error
      : JSON.stringify(body);
    // 401 = a token was sent and rejected; 403 = the bridge has no token at
    // all. Different fixes, so they get different explanations.
    if (response.status === 401) {
      throw new Error(`UE5 bridge error 401: ${message}. ${authMismatchHint()}`);
    }
    throw new Error(`UE5 bridge error ${response.status}: ${describeAuthFailure(response.status, message, path)}`);
  }

  return body;
}

// The bridge disables exec_python and the lifecycle routes when no AuthToken is
// configured, and answers 403. Its message points at DefaultEngine.ini, which is
// the tracked file the token must NOT live in — and it says nothing about the
// blast radius, so a failing `pcg_*` call reads as an unrelated bug. Rewrite the
// tail into something the caller can act on.
//
// Matching is on the bridge's own wording; if it ever changes, the original
// message still comes through unmodified.
function describeAuthFailure(status, message, requestPath) {
  const text = String(message ?? "");
  if (status !== 403 || !/AuthToken/i.test(text)) return text;

  const head = text.split("Set one in")[0].trim();
  const viaPython = requestPath === "/editor/exec_python";
  return (
    `${head}\n\n` +
    `Fix: run \`npm run set-token\` in ` +
    `Plugins/UnrealNGGMCP/Source/ThirdParty/unrealngg-mcp/, then restart the editor. ` +
    `It writes [UnrealNGGMCP] AuthToken to <Project>/Config/UserEngine.ini — the ` +
    `gitignored config layer. Do not put the token in Config/DefaultEngine.ini: ` +
    `that file is tracked in git.` +
    (viaPython
      ? `\n\nNote: every pcg_* tool runs through exec_python, so all of them stay ` +
        `disabled until a token is set.`
      : "")
  );
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/**
 * The bridge wire-protocol version this sidecar was written against. The plugin
 * reports its own as `bridge_protocol` from GET /health (NGG_BRIDGE_PROTOCOL in
 * NGGHttpServer.h). A mismatch means the two halves shipped separately — the
 * usual cause is a stale plugin binary left in Binaries/ after an update.
 */
export const EXPECTED_BRIDGE_PROTOCOL = 1;

/**
 * Compare what /health reported against what this sidecar expects.
 *
 * A missing `bridge_protocol` is not an error: it means the plugin predates the
 * version surface, which is exactly the situation where a scolding message
 * would be least useful.
 *
 * @param {object} health - parsed GET /health body
 * @returns {{ok: boolean, warning: string|null}}
 */
export function checkBridgeProtocol(health) {
  const reported = health?.bridge_protocol;
  if (reported === undefined || reported === null) {
    return { ok: true, warning: null };
  }
  if (reported === EXPECTED_BRIDGE_PROTOCOL) {
    return { ok: true, warning: null };
  }
  const direction = reported > EXPECTED_BRIDGE_PROTOCOL
    ? "The editor plugin is newer than this MCP server"
    : "The editor plugin is older than this MCP server";
  return {
    ok: false,
    warning:
      `Bridge protocol mismatch: plugin reports ${reported}, this server expects ` +
      `${EXPECTED_BRIDGE_PROTOCOL}. ${direction} — some tools may fail in confusing ways. ` +
      `Rebuild the editor target so the plugin binary matches the shipped sidecar.`,
  };
}

/** GET /health */
export async function healthCheck() {
  return bridgeFetch("/health");
}

/**
 * GET /project_info
 * Ground-truth project + engine paths from the running editor. Lets the
 * sidecar follow whichever editor is currently up on the bridge port instead
 * of caching paths at startup.
 */
export async function getProjectInfo() {
  return bridgeFetch("/project_info");
}

/**
 * GET /assets/list?path=...
 * @param {string} path - content browser path, e.g. "/Game/Data/Exercises"
 */
export async function listAssets(path = "/Game") {
  const encoded = encodeURIComponent(path);
  return bridgeFetch(`/assets/list?path=${encoded}`);
}

/**
 * GET /assets/get?path=...
 * @param {string} assetPath - full content path, e.g. "/Game/Data/Exercises/DA_ADL01"
 */
export async function getAsset(assetPath) {
  const encoded = encodeURIComponent(assetPath);
  return bridgeFetch(`/assets/get?path=${encoded}`);
}

/**
 * POST /assets/create
 * @param {string} className  - UClass name without 'U' prefix, e.g. "AdlExerciseDefinition"
 * @param {string} assetPath  - destination content path, e.g. "/Game/Data/Exercises/DA_ADL01"
 */
export async function createAsset(className, assetPath) {
  return bridgeFetch("/assets/create", {
    method: "POST",
    body: JSON.stringify({ class: className, path: assetPath }),
  });
}

/**
 * POST /assets/set_property
 * @param {string} assetPath    - full content path
 * @param {string} propertyName - UPROPERTY name (exact match)
 * @param {*}      value        - new value (string, number, boolean, or JSON-serialisable object)
 */
export async function setAssetProperty(assetPath, propertyName, value) {
  return bridgeFetch("/assets/set_property", {
    method: "POST",
    body: JSON.stringify({ path: assetPath, property: propertyName, value }),
  });
}

/**
 * POST /adl/exercise/create_full
 * Creates (or overwrites) a complete UAdlExerciseDefinition asset.
 *
 * @param {object} exerciseDef - full exercise definition JSON (see handler docs)
 */
export async function createExerciseFull(exerciseDef) {
  return bridgeFetch("/adl/exercise/create_full", {
    method: "POST",
    body: JSON.stringify(exerciseDef),
  });
}

/**
 * POST /adl/exercise/add_phase
 * @param {string} assetPath - content path of the existing exercise asset
 * @param {object} phase     - { phase_display_name: string, steps: [] }
 */
export async function addPhaseToExercise(assetPath, phase) {
  return bridgeFetch("/adl/exercise/add_phase", {
    method: "POST",
    body: JSON.stringify({ asset_path: assetPath, phase }),
  });
}

/**
 * POST /adl/exercise/add_step
 * @param {string} assetPath  - content path of the existing exercise asset
 * @param {number} phaseIndex - zero-based index of the target phase
 * @param {object} step       - FAdlStep-shaped object
 */
export async function addStepToExercise(assetPath, phaseIndex, step) {
  return bridgeFetch("/adl/exercise/add_step", {
    method: "POST",
    body: JSON.stringify({ asset_path: assetPath, phase_index: phaseIndex, step }),
  });
}

/**
 * GET /adl/exercise/get?id=...
 * @param {string} exerciseId - e.g. "ADL_01_JamSandwich"
 */
export async function getExercise(exerciseId) {
  const encoded = encodeURIComponent(exerciseId);
  return bridgeFetch(`/adl/exercise/get?id=${encoded}`);
}

/**
 * POST /assets/delete
 * @param {string} assetPath - full content path of the asset to delete
 */
export async function deleteAsset(assetPath) {
  return bridgeFetch("/assets/delete", {
    method: "POST",
    body: JSON.stringify({ asset_path: assetPath }),
  });
}

/** POST /editor/save_all */
export async function saveAll() {
  return bridgeFetch("/editor/save_all", { method: "POST", body: "{}" });
}

/**
 * POST /editor/reimport
 * @param {string} assetPath - content path of the asset to reimport
 */
export async function reimportAsset(assetPath) {
  return bridgeFetch("/editor/reimport", {
    method: "POST",
    body: JSON.stringify({ path: assetPath }),
  });
}

/** GET /gameplay_tags/list */
export async function listGameplayTags() {
  return bridgeFetch("/gameplay_tags/list");
}

/**
 * POST /assets/duplicate
 * @param {string} source - content path of the asset to copy
 * @param {string} dest   - content path for the new copy
 */
export async function duplicateAsset(source, dest) {
  return bridgeFetch("/assets/duplicate", {
    method: "POST",
    body: JSON.stringify({ source, dest }),
  });
}

/**
 * POST /assets/set_map_entries
 * Replaces all entries of a TMap property on an asset. Values that are
 * object/class paths are resolved with LoadObject on the editor side.
 * @param {string} assetPath - full content path
 * @param {string} property  - TMap UPROPERTY name
 * @param {Array<{key: string, value: string}>} entries
 */
export async function setAssetMapEntries(assetPath, property, entries) {
  return bridgeFetch("/assets/set_map_entries", {
    method: "POST",
    body: JSON.stringify({ path: assetPath, property, entries }),
  });
}

/**
 * POST /bp/add_interface
 * @param {object} opts - { blueprint, interface, compile? }
 */
export async function bpAddInterface({ blueprint, interface: iface, compile } = {}) {
  const body = { blueprint, interface: iface };
  if (compile !== undefined) body.compile = compile;
  return bridgeFetch("/bp/add_interface", {
    method: "POST",
    body: JSON.stringify(body),
  });
}

/**
 * POST /bp/implement_interface_function
 * @param {object} opts - { blueprint, function, compile? }
 */
export async function bpImplementInterfaceFunction({ blueprint, function: func, compile } = {}) {
  const body = { blueprint, function: func };
  if (compile !== undefined) body.compile = compile;
  return bridgeFetch("/bp/implement_interface_function", {
    method: "POST",
    body: JSON.stringify(body),
  });
}

/**
 * POST /bp/refresh_all_nodes
 * @param {object} opts - { blueprint, compile?, save? }
 */
export async function bpRefreshAllNodes({ blueprint, compile, save } = {}) {
  const body = { blueprint };
  if (compile !== undefined) body.compile = compile;
  if (save !== undefined) body.save = save;
  return bridgeFetch("/bp/refresh_all_nodes", {
    method: "POST",
    body: JSON.stringify(body),
  });
}

// ---------------------------------------------------------------------------
// Blueprint / Widget Blueprint / World Settings / Actor Spawning
// ---------------------------------------------------------------------------

/**
 * POST /editor/create_blueprint
 * @param {string} parentClass - C++ class name (e.g. 'AAdlSequencerActor') or Blueprint path
 * @param {string} assetPath   - Destination content path, e.g. '/Game/Blueprints/BP_AdlSequencerActor'
 */
export async function createBlueprint(parentClass, assetPath) {
  return bridgeFetch("/editor/create_blueprint", {
    method: "POST",
    body: JSON.stringify({ parent_class: parentClass, asset_path: assetPath }),
  });
}

/**
 * POST /editor/create_widget_blueprint
 * @param {string} parentClass - C++ UserWidget subclass name (e.g. 'URehabMainMenuWidget')
 * @param {string} assetPath   - Destination content path, e.g. '/Game/UI/WBP_MainMenu'
 * @param {Array}  widgets     - Array of { type, name, parent?, user_widget_class? }
 * @param {string} [rootType]  - Root panel type: CanvasPanel|Overlay|VerticalBox|HorizontalBox|GridPanel|UniformGridPanel|ScrollBox|Border|SizeBox|ScaleBox (default CanvasPanel)
 */
export async function createWidgetBlueprint(parentClass, assetPath, widgets, rootType) {
  return bridgeFetch("/editor/create_widget_blueprint", {
    method: "POST",
    body: JSON.stringify({
      parent_class: parentClass,
      asset_path: assetPath,
      widgets,
      ...(rootType ? { root_type: rootType } : {}),
    }),
  });
}

/**
 * POST /editor/set_blueprint_defaults
 * @param {string} assetPath  - Content path of the Blueprint asset
 * @param {Array}  properties - Array of { name, value } objects to set on the CDO
 */
export async function setBlueprintDefaults(assetPath, properties) {
  return bridgeFetch("/editor/set_blueprint_defaults", {
    method: "POST",
    body: JSON.stringify({ asset_path: assetPath, properties }),
  });
}

/**
 * POST /editor/reparent_blueprint
 * @param {string} assetPath  - Content path to the Blueprint asset
 * @param {string} newParent  - New parent: C++ class name (e.g. "MyGameHUDWidget") or Blueprint content path
 */
export async function reparentBlueprint(assetPath, newParent) {
  return bridgeFetch("/editor/reparent_blueprint", {
    method: "POST",
    body: JSON.stringify({ asset_path: assetPath, new_parent: newParent }),
  });
}

/**
 * POST /editor/set_world_settings
 * @param {object} settings - { game_mode_class, player_controller_class, default_pawn_class }
 */
export async function setWorldSettings(settings) {
  return bridgeFetch("/editor/set_world_settings", {
    method: "POST",
    body: JSON.stringify(settings),
  });
}

/**
 * POST /editor/spawn_actor_in_level
 * @param {string} actorClass - C++ class name or Blueprint content path
 * @param {object} location   - { x, y, z } in world-space centimetres
 * @param {object} rotation   - { pitch, yaw, roll } in degrees (optional)
 * @param {string} label      - Actor label shown in the Outliner (optional)
 */
export async function spawnActorInLevel(actorClass, location, rotation, label, staticMesh) {
  return bridgeFetch("/editor/spawn_actor_in_level", {
    method: "POST",
    body: JSON.stringify({
      actor_class: actorClass,
      location,
      rotation: rotation ?? { pitch: 0, yaw: 0, roll: 0 },
      label: label ?? "",
      ...(staticMesh && { static_mesh: staticMesh }),
    }),
  });
}

/**
 * Set properties on a named subobject component of a Blueprint's CDO.
 * @param {string} assetPath     - Content path of the Blueprint
 * @param {string} componentName - Name of the component subobject (e.g. "HUDWidgetComponent")
 * @param {Array<{name:string, value:string}>} properties - Properties to set
 */
export async function setComponentDefaults(assetPath, componentName, properties) {
  return bridgeFetch("/editor/set_component_defaults", {
    method: "POST",
    body: JSON.stringify({
      asset_path: assetPath,
      component_name: componentName,
      properties,
    }),
  });
}

/**
 * GET /editor/get_component_defaults?blueprint=...&component=...&all_props=...
 * Read counterpart of setComponentDefaults. Reports a Blueprint component's
 * configured default values together with the component-class CDO value, so the
 * caller can see exactly what was overridden (and verify a prior set).
 * @param {string}  blueprint   - Content path of the Blueprint
 * @param {string}  [component] - Single component variable name; omit to dump all SCS components
 * @param {boolean} [allProps]  - true to emit every editable property; default emits only deltas vs the class CDO
 */
export async function getComponentDefaults(blueprint, component, allProps) {
  const params = new URLSearchParams({ blueprint });
  if (component) params.set("component", component);
  if (allProps) params.set("all_props", "true");
  return bridgeFetch(`/editor/get_component_defaults?${params.toString()}`);
}

/**
 * Add a new component to a Blueprint's SimpleConstructionScript.
 * @param {string} assetPath      - Content path of the Blueprint
 * @param {string} componentClass - Component class name (e.g. "UCesiumIonRasterOverlay")
 * @param {string} componentName  - Name for the new component
 * @param {Array<{name:string, value:string}>} properties - Optional initial properties
 * @param {string} [attachParent] - Name of an existing scene component to attach the new
 *                                  one under. Omit to attach scene components to the
 *                                  default scene root (legacy behavior).
 */
export async function addComponentToBlueprint(assetPath, componentClass, componentName, properties, attachParent) {
  return bridgeFetch("/editor/add_component_to_blueprint", {
    method: "POST",
    body: JSON.stringify({
      asset_path: assetPath,
      component_class: componentClass,
      component_name: componentName || "",
      properties: properties ?? [],
      attach_parent: attachParent || "",
    }),
  });
}

/**
 * Remove a component from a Blueprint's SimpleConstructionScript by name.
 * Children of the removed node are promoted to its parent so they aren't lost.
 * Refuses to delete the default scene root.
 * @param {string} assetPath     - Content path of the Blueprint
 * @param {string} componentName - Name of the component to remove
 */
export async function removeComponentFromBlueprint(assetPath, componentName) {
  return bridgeFetch("/editor/remove_component_from_blueprint", {
    method: "POST",
    body: JSON.stringify({
      asset_path: assetPath,
      component_name: componentName,
    }),
  });
}

/** Open an existing level in the editor. */
export async function openLevel(levelPath) {
  return bridgeFetch("/editor/open_level", {
    method: "POST",
    body: JSON.stringify({ level_path: levelPath }),
  });
}

/** Create a new empty level and open it in the editor. */
/**
 * POST /editor/create_level
 *
 * @param {string}  levelPath
 * @param {object}  [opts]
 * @param {boolean} [opts.partitioned] - create a World Partition map
 */
export async function createLevel(levelPath, { partitioned } = {}) {
  return bridgeFetch("/editor/create_level", {
    method: "POST",
    body: JSON.stringify({
      level_path: levelPath,
      ...(partitioned != null ? { partitioned } : {}),
    }),
  });
}

/** List actors in the current editor level. Optional class filter. */
export async function listActors(classFilter) {
  const params = classFilter ? `?class_filter=${encodeURIComponent(classFilter)}` : "";
  return bridgeFetch(`/editor/list_actors${params}`);
}

/** Update an actor's transform and/or label by its current label. */
export async function updateActor(actorLabel, { location, rotation, newLabel, componentProperties } = {}) {
  return bridgeFetch("/editor/update_actor", {
    method: "POST",
    body: JSON.stringify({
      actor_label: actorLabel,
      ...(location && { location }),
      ...(rotation && { rotation }),
      ...(newLabel && { new_label: newLabel }),
      ...(componentProperties?.length > 0 && { component_properties: componentProperties }),
    }),
  });
}

/** Delete an actor from the current level by label. */
export async function deleteActor(actorLabel) {
  return bridgeFetch("/editor/delete_actor", {
    method: "POST",
    body: JSON.stringify({ actor_label: actorLabel }),
  });
}

/**
 * POST /editor/batch
 * Execute multiple operations in a single HTTP round-trip to the UE5 editor.
 * @param {Array<{method:string, path:string, body:object}>} operations
 * @returns {Promise<{ results: Array<{ index:number, status:number, body:object }> }>}
 */
export async function batch(operations) {
  return bridgeFetch("/editor/batch", {
    method: "POST",
    body: JSON.stringify({ operations }),
  });
}

/**
 * POST /input/configure_imc
 *
 * Clears an InputMappingContext and writes fresh key mappings for one or more
 * InputActions.  Supports modifiers: "Negate", "SwizzleAxis", "DeadZone", "Scalar".
 *
 * @param {string} imcPath  - Content path of the IMC, e.g. "/Game/MyGame/Input/IMC_MyGame"
 * @param {Array}  mappings - Array of { action_path, key, modifiers[] } objects
 *   e.g. [
 *     { action_path: "/Game/MyGame/Input/IA_Move", key: "D", modifiers: [] },
 *     { action_path: "/Game/MyGame/Input/IA_Move", key: "A", modifiers: ["Negate"] },
 *     { action_path: "/Game/MyGame/Input/IA_Move", key: "W", modifiers: ["SwizzleAxis"] },
 *     { action_path: "/Game/MyGame/Input/IA_Move", key: "S", modifiers: ["SwizzleAxis","Negate"] },
 *   ]
 */
export async function configureIMC(imcPath, mappings) {
  return bridgeFetch("/input/configure_imc", {
    method: "POST",
    body: JSON.stringify({ imc_path: imcPath, mappings }),
  });
}

/**
 * POST /editor/style_widgets
 *
 * Apply visual styling operations to named widgets inside an existing Widget Blueprint.
 * Each style entry targets one widget by name and applies only the fields present.
 *
 * @param {string} widgetBlueprint - Content path of the WBP, e.g. '/Game/UI/WBP_MainMenu'
 * @param {Array}  styles          - Array of style operation objects. Each must include
 *                                   widget_name. All other fields are optional and skipped
 *                                   silently if not applicable to the widget type:
 *   {
 *     widget_name:          string,   // required — matches WidgetTree FName
 *     font_size:            number,   // UTextBlock, UEditableTextBox
 *     font_bold:            boolean,  // UTextBlock
 *     text:                 string,   // UTextBlock, UEditableTextBox
 *     auto_wrap:            boolean,  // UTextBlock
 *     color:                string,   // '#RRGGBB' — UTextBlock foreground; UProgressBar fill
 *     fill_color:           string,   // '#RRGGBB' — UProgressBar fill (alias for color)
 *     background_color:     string,   // '#RRGGBB' — UButton tint (normal/hovered/pressed)
 *     width_override:       number,   // UCanvasPanelSlot width in pixels
 *     height_override:      number,   // UCanvasPanelSlot height in pixels
 *     anchor:               string,   // 'top-left'|'top-center'|'top-right'|'center-left'|'center'|'center-right'|'bottom-left'|'bottom-center'|'bottom-right'
 *     position_x:           number,   // UCanvasPanelSlot position X offset from anchor
 *     position_y:           number,   // UCanvasPanelSlot position Y offset from anchor
 *     alignment_x:          number,   // 0.0–1.0 — UCanvasPanelSlot pivot X
 *     alignment_y:          number,   // 0.0–1.0 — UCanvasPanelSlot pivot Y
 *     visibility:           string,   // 'Visible'|'Hidden'|'Collapsed'|'HitTestInvisible'
 *     horizontal_alignment: string,   // 'Left'|'Center'|'Right' — UTextBlock justification
 *     padding:              { left, top, right, bottom }, // UCanvasPanelSlot margins
 *     percent:              number,   // 0.0–1.0 — UProgressBar fill amount
 *   }
 * @returns {Promise<{ success, widget_blueprint, total_applied, total_skipped, results[] }>}
 */
export async function styleWidgets(widgetBlueprint, styles) {
  return bridgeFetch("/editor/style_widgets", {
    method: "POST",
    body: JSON.stringify({ widget_blueprint: widgetBlueprint, styles }),
  });
}

/**
 * POST /editor/create_niagara_system
 *
 * Creates a blank UNiagaraSystem asset at the given content path.
 * If template_path is provided, the existing system is duplicated instead of
 * creating a blank one — useful for bootstrapping from a known working template.
 *
 * After creation open the asset in the Niagara editor to add emitter modules
 * (Spawn Burst Instantaneous, Initialize Particle, Add Velocity, Gravity Force,
 * Sprite Renderer).  Call ue5_save_all to persist.
 *
 * @param {string}  asset_path    - Destination content path, e.g. '/Game/MyGame/VFX/NS_BoxDestroy'
 * @param {string} [template_path]- Optional path of an existing UNiagaraSystem to clone from
 * @returns {Promise<{ success, already_existed, asset_path, next_step }>}
 */
export async function createNiagaraSystem({ asset_path, template_path } = {}) {
  return bridgeFetch("/editor/create_niagara_system", {
    method: "POST",
    body: JSON.stringify({
      asset_path,
      ...(template_path ? { template_path } : {}),
    }),
  });
}

/**
 * POST /editor/configure_niagara_system
 *
 * Configure an existing UNiagaraSystem with emitter modules and parameters.
 * If the system has no emitters, searches engine content for a sprite-burst
 * template and adds it automatically.  Then tries to set user-exposed
 * parameters (spawn count, lifetime, colour, sprite size, velocity, gravity).
 *
 * @param {string}  asset_path   - Content path to an existing UNiagaraSystem
 * @param {number}  [spawn_count]   - Particles per burst (default 10)
 * @param {number}  [lifetime_min]  - Min particle lifetime in seconds (default 0.6)
 * @param {number}  [lifetime_max]  - Max particle lifetime in seconds (default 0.8)
 * @param {number}  [color_r]       - Linear-space red   (default 0.212 = #8B5E3C)
 * @param {number}  [color_g]       - Linear-space green (default 0.139)
 * @param {number}  [color_b]       - Linear-space blue  (default 0.071)
 * @param {number}  [sprite_size]   - Sprite size in UU (default 3.0)
 * @param {number}  [velocity]      - Outward launch speed cm/s (default 200)
 * @param {number}  [gravity_z]     - Gravity Z cm/s² (default -980)
 * @param {string}  [loop_behavior] - "Once" | "Infinite" | "Multiple" (default "Once")
 * @returns {Promise<{ success, emitter_added, template_used, parameters, steps, manual_config_required? }>}
 */
export async function configureNiagaraSystem({
  asset_path,
  spawn_count,
  lifetime_min,
  lifetime_max,
  color_r,
  color_g,
  color_b,
  sprite_size,
  velocity,
  gravity_z,
  loop_behavior,
} = {}) {
  return bridgeFetch("/editor/configure_niagara_system", {
    method: "POST",
    body: JSON.stringify({
      asset_path,
      ...(spawn_count   != null && { spawn_count }),
      ...(lifetime_min  != null && { lifetime_min }),
      ...(lifetime_max  != null && { lifetime_max }),
      ...(color_r       != null && { color_r }),
      ...(color_g       != null && { color_g }),
      ...(color_b       != null && { color_b }),
      ...(sprite_size   != null && { sprite_size }),
      ...(velocity      != null && { velocity }),
      ...(gravity_z     != null && { gravity_z }),
      ...(loop_behavior != null && { loop_behavior }),
    }),
  });
}

/**
 * Set Niagara emitter Rapid Iteration Parameters (RIPs) directly — bypasses the
 * User.* parameter store so it works even on template emitters that don't expose
 * their module inputs as User params (e.g. SimpleSpriteBurst).
 *
 * Call with list_only=true first to discover available RIP names, then use
 * raw_params for precise targeting or the named shorthand params for common values.
 *
 * @param {string}  asset_path      - Content path to the UNiagaraSystem
 * @param {number}  [emitter_index] - Emitter index in the system (default 0)
 * @param {boolean} [list_only]     - If true, return all RIP names without setting (default false)
 * @param {number}  [spawn_count]
 * @param {number}  [lifetime_min]
 * @param {number}  [lifetime_max]
 * @param {number}  [color_r]
 * @param {number}  [color_g]
 * @param {number}  [color_b]
 * @param {number}  [sprite_size]
 * @param {number}  [velocity_min]
 * @param {number}  [velocity_max]
 * @param {number}  [gravity_z]
 * @param {Array}   [raw_params]    - [{ name: "Constants.Module.Param", value: 1.0 }]
 * @returns {Promise<{ success, rip_count, params_set, params_not_found, all_rips, hint? }>}
 */
export async function setNiagaraEmitterParams({
  asset_path,
  emitter_index,
  list_only,
  remove_emitter,
  spawn_count,
  lifetime_min,
  lifetime_max,
  color_r,
  color_g,
  color_b,
  sprite_size,
  velocity_min,
  velocity_max,
  gravity_z,
  raw_params,
} = {}) {
  return bridgeFetch("/editor/set_niagara_emitter_params", {
    method: "POST",
    body: JSON.stringify({
      asset_path,
      ...(emitter_index  != null && { emitter_index }),
      ...(list_only       != null && { list_only }),
      ...(remove_emitter  != null && { remove_emitter }),
      ...(spawn_count    != null && { spawn_count }),
      ...(lifetime_min   != null && { lifetime_min }),
      ...(lifetime_max   != null && { lifetime_max }),
      ...(color_r        != null && { color_r }),
      ...(color_g        != null && { color_g }),
      ...(color_b        != null && { color_b }),
      ...(sprite_size    != null && { sprite_size }),
      ...(velocity_min   != null && { velocity_min }),
      ...(velocity_max   != null && { velocity_max }),
      ...(gravity_z      != null && { gravity_z }),
      ...(raw_params     != null && { raw_params }),
    }),
  });
}

/**
 * POST /editor/add_widget_to_blueprint
 *
 * Append UMG widgets to an existing Widget Blueprint's widget tree.
 * Does NOT clear existing widgets — appends only.
 * Widgets added in the same call can reference each other by name as "parent".
 *
 * @param {string} widgetBlueprint - Content path of the WBP, e.g. '/Game/MyGame/UI/WBP_Settings'
 * @param {Array}  widgets         - Array of:
 *   {
 *     type:    string,  // Button|TextBlock|Image|ProgressBar|EditableTextBox|ComboBoxString|
 *                       // Slider|VerticalBox|HorizontalBox|CanvasPanel
 *     name:    string,  // unique FName for this widget (must not already exist in tree)
 *     parent?: string,  // optional: FName of an existing parent panel widget
 *   }
 * @returns {Promise<{ success, widget_blueprint, widgets_added, widgets_skipped }>}
 */
export async function addWidgetToBlueprint(widgetBlueprint, widgets) {
  return bridgeFetch("/editor/add_widget_to_blueprint", {
    method: "POST",
    body: JSON.stringify({ widget_blueprint: widgetBlueprint, widgets }),
  });
}

/**
 * POST /editor/get_widget_tree
 *
 * Inspect the full widget hierarchy of a Widget Blueprint as nested JSON.
 * Returns slot details (canvas/vbox/hbox/grid/overlay/etc.), render transform,
 * tooltip, is_enabled, is_variable, brush info, text content, etc.
 *
 * @param {string} widgetBlueprint - Content path of the WBP
 * @returns {Promise<{ widget_blueprint, root }>}
 */
export async function getWidgetTree(widgetBlueprint) {
  return bridgeFetch("/editor/get_widget_tree", {
    method: "POST",
    body: JSON.stringify({ widget_blueprint: widgetBlueprint }),
  });
}

/**
 * POST /editor/remove_widget_from_blueprint
 *
 * Delete a widget (and optionally its descendants) from the widget tree.
 * Root widget cannot be removed.
 *
 * @param {string}  widgetBlueprint
 * @param {string}  widgetName       - FName of the widget to remove
 * @param {boolean} [cascade=true]   - If false and widget has children, returns 400
 * @returns {Promise<{ success, widget_blueprint, removed: string[] }>}
 */
export async function removeWidgetFromBlueprint(widgetBlueprint, widgetName, cascade) {
  return bridgeFetch("/editor/remove_widget_from_blueprint", {
    method: "POST",
    body: JSON.stringify({
      widget_blueprint: widgetBlueprint,
      widget_name: widgetName,
      ...(cascade != null ? { cascade } : {}),
    }),
  });
}

/**
 * POST /editor/reparent_widget
 *
 * Move a widget under a different parent panel. Slot properties are not
 * preserved — call ue5_style_widgets with slot_* keys afterwards if needed.
 *
 * @param {string} widgetBlueprint
 * @param {string} widgetName
 * @param {string} newParent
 * @param {number} [childIndex]     - Insert position in new parent's children (default: append)
 * @returns {Promise<{ success, widget_blueprint, widget_name, new_parent, new_slot_class }>}
 */
export async function reparentWidget(widgetBlueprint, widgetName, newParent, childIndex) {
  return bridgeFetch("/editor/reparent_widget", {
    method: "POST",
    body: JSON.stringify({
      widget_blueprint: widgetBlueprint,
      widget_name: widgetName,
      new_parent: newParent,
      ...(childIndex != null ? { child_index: childIndex } : {}),
    }),
  });
}

/**
 * POST /editor/rename_widget
 *
 * Rename a widget while preserving its slot and children. The new name becomes
 * the new FName used by BindWidget / tree lookups.
 *
 * @param {string} widgetBlueprint
 * @param {string} oldName
 * @param {string} newName
 * @returns {Promise<{ success, widget_blueprint, old_name, new_name }>}
 */
export async function renameWidget(widgetBlueprint, oldName, newName) {
  return bridgeFetch("/editor/rename_widget", {
    method: "POST",
    body: JSON.stringify({
      widget_blueprint: widgetBlueprint,
      old_name: oldName,
      new_name: newName,
    }),
  });
}

/**
 * POST /editor/compile_widget_blueprint
 *
 * Compile a Widget Blueprint after structural edits. Required before BindWidget
 * lookups, runtime instantiation, or use in TSubclassOf<UUserWidget> properties.
 *
 * @param {string} widgetBlueprint
 * @returns {Promise<{ success, widget_blueprint, status, warnings: string[], errors: string[] }>}
 */
export async function compileWidgetBlueprint(widgetBlueprint) {
  return bridgeFetch("/editor/compile_widget_blueprint", {
    method: "POST",
    body: JSON.stringify({ widget_blueprint: widgetBlueprint }),
  });
}

/**
 * POST /editor/configure_anim_blueprint
 *
 * Wire an Animation Blueprint's state machine with animation sequences/blend
 * spaces.  Creates states, assigns sequence/blend-space players, and adds
 * transition rules based on variable conditions.
 *
 * @param {string} assetPath     - Content path to the AnimBlueprint
 * @param {Array}  states        - Array of { name, animation, loop? }
 * @param {Array}  transitions   - Array of { from, to, condition }
 * @returns {Promise<{ success, asset_path, states_created, transitions_created, total_states }>}
 */
export async function configureAnimBlueprint(assetPath, states, transitions) {
  return bridgeFetch("/editor/configure_anim_blueprint", {
    method: "POST",
    body: JSON.stringify({ asset_path: assetPath, states, transitions }),
  });
}

/**
 * POST /editor/set_level_environment
 *
 * Open a level, add/configure ExponentialHeightFog, DirectionalLight, and
 * SkyLight actors for themed environments, then mark dirty for save.
 *
 * @param {string} levelPath - Content path to the level
 * @param {object} options   - { fog, directional_light, sky_light }
 * @returns {Promise<{ success, level_path, actions[] }>}
 */
export async function setLevelEnvironment(levelPath, options) {
  return bridgeFetch("/editor/set_level_environment", {
    method: "POST",
    body: JSON.stringify({ level_path: levelPath, ...options }),
  });
}

/**
 * POST /assets/import
 *
 * Import an external file into the UE5 content browser.
 * Supports any format the editor can import (wav, ogg, png, jpg, fbx, etc.)
 *
 * @param {string}  sourcePath - Absolute disk path, e.g. 'C:/audio/SFX_Explosion.wav'
 * @param {string}  destPath   - Content browser folder, e.g. '/Game/MyGame/Audio'
 * @param {string} [assetName] - Optional: override the asset name (default: filename stem)
 * @returns {Promise<{ success, asset_path, source_path }>}
 */
export async function importAsset(sourcePath, destPath, assetName) {
  return bridgeFetch("/assets/import", {
    method: "POST",
    body: JSON.stringify({
      source_path: sourcePath,
      dest_path:   destPath,
      ...(assetName ? { asset_name: assetName } : {}),
    }),
  });
}

/**
 * POST /editor/create_material
 *
 * Create a parent UMaterial. With use_parameters:true (recommended), creates
 * a Material with VectorParameter "BaseColor" and ScalarParameter
 * "BrightnessMultiplier" wired to the BaseColor pin — DMI-friendly out of the
 * box. The default basic-shapes material has no parameters, so DMI parameter
 * swaps in Blueprint do nothing; use this tool to create a parent that DMI
 * can drive.
 *
 * @param {string}  asset_path           - Destination, e.g. '/Game/Materials/M_ParamColor'
 * @param {object}  [base_color]         - { r, g, b } in linear space (default warm brown)
 * @param {boolean} [use_parameters]     - Add BaseColor + BrightnessMultiplier params (default false; pass true for DMI workflows)
 * @param {boolean} [pixel_art]          - Add UV-border outline shader
 * @param {string}  [texture_path]       - Optional texture; implies use_parameters
 * @param {number}  [brightness_default] - Default for BrightnessMultiplier scalar (default 1.0)
 * @param {boolean} [unlit]              - Route color to Emissive + set Shading Model = Unlit (lighting-independent)
 * @param {string}  [blueprint_path]     - Optional: also assign to a BP component as override
 * @param {string}  [component_name]     - Component to receive the override material
 */
export async function createMaterial({ asset_path, base_color, use_parameters, pixel_art, texture_path, brightness_default, unlit, blueprint_path, component_name } = {}) {
  return bridgeFetch("/editor/create_material", {
    method: "POST",
    body: JSON.stringify({
      asset_path,
      ...(base_color         != null ? { base_color }         : {}),
      ...(use_parameters     != null ? { use_parameters }     : {}),
      ...(pixel_art          != null ? { pixel_art }          : {}),
      ...(texture_path       != null ? { texture_path }       : {}),
      ...(brightness_default != null ? { brightness_default } : {}),
      ...(unlit              != null ? { unlit }              : {}),
      ...(blueprint_path     ? { blueprint_path } : {}),
      ...(component_name     ? { component_name } : {}),
    }),
  });
}

/**
 * Create (or overwrite) a UCurveLinearColor asset with the given RGB keys.
 *
 * @param {string}  asset_path - Destination, e.g. '/Game/Materials/Curve_CableColor'
 * @param {Array}   keys       - [{ time, r, g, b }, ...] in ascending time order (alpha = 1)
 * @param {boolean} [linear]   - true (default) = linear interp; false = constant (stepped)
 * @returns {Promise<{ success, asset_path, already_existed, key_count }>}
 */
export async function createColorCurve({ asset_path, keys, linear } = {}) {
  return bridgeFetch("/editor/create_color_curve", {
    method: "POST",
    body: JSON.stringify({
      asset_path,
      ...(keys   != null ? { keys }   : {}),
      ...(linear != null ? { linear } : {}),
    }),
  });
}

/**
 * POST /editor/create_material_instance
 *
 * Create a UMaterialInstanceConstant from a parent material with scalar/vector
 * parameter overrides.  Optionally applies the MI to a Blueprint component.
 *
 * @param {string}  asset_path      - Content path for the new MI, e.g. '/Game/Materials/MI_BoxHover'
 * @param {string}  parent_material - Content path of the parent material
 * @param {Array}  [scalar_params]  - [{name, value}, ...] scalar parameter overrides
 * @param {Array}  [vector_params]  - [{name, value: {r,g,b,a}}, ...] vector parameter overrides
 * @param {Array}  [texture_params] - [{name, texture_path}, ...] texture parameter overrides
 * @param {string} [blueprint_path] - Optional: apply MI to this Blueprint's component
 * @param {string} [component_name] - Optional: which component to apply MI to
 * @returns {Promise<{ success, asset_path, parent_material, already_existed, scalar_params_set, vector_params_set, texture_params_set, applied_to_blueprint }>}
 */
export async function createMaterialInstance({ asset_path, parent_material, scalar_params, vector_params, texture_params, blueprint_path, component_name } = {}) {
  return bridgeFetch("/editor/create_material_instance", {
    method: "POST",
    body: JSON.stringify({
      asset_path,
      parent_material,
      ...(scalar_params  ? { scalar_params }  : {}),
      ...(vector_params  ? { vector_params }  : {}),
      ...(texture_params ? { texture_params } : {}),
      ...(blueprint_path ? { blueprint_path } : {}),
      ...(component_name ? { component_name } : {}),
    }),
  });
}

/**
 * POST /editor/read_material
 *
 * Inspect a UMaterial or UMaterialInstance: reports class, parent (instances),
 * domain, blend mode, shading model, and every scalar / vector / texture
 * parameter with its current value. Use before SetXParameterValue to confirm
 * a parameter actually exists on the material.
 *
 * @param {string} asset_path - Content path of the material or material instance
 * @returns {Promise<{ success, asset_path, class, parent?, domain, blend_mode, shading_model, scalar_params, vector_params, texture_params }>}
 */
export async function readMaterial({ asset_path } = {}) {
  return bridgeFetch("/editor/read_material", {
    method: "POST",
    body: JSON.stringify({ asset_path }),
  });
}

/**
 * POST /editor/create_float_curve
 *
 * Create (or overwrite) a UCurveFloat — the scalar counterpart of a color
 * curve (intensity / speed / alpha over time), sampled with GetFloatValue.
 *
 * @param {string}  asset_path - Destination, e.g. '/Game/Curves/Curve_Intensity'
 * @param {Array}   keys       - [{ time, value }, ...] in ascending time order
 * @param {boolean} [linear]   - true (default) = linear interp; false = constant (stepped)
 * @returns {Promise<{ success, asset_path, already_existed, key_count }>}
 */
export async function createFloatCurve({ asset_path, keys, linear } = {}) {
  return bridgeFetch("/editor/create_float_curve", {
    method: "POST",
    body: JSON.stringify({
      asset_path,
      ...(keys   != null ? { keys }   : {}),
      ...(linear != null ? { linear } : {}),
    }),
  });
}

/**
 * POST /editor/read_curve
 *
 * Read a UCurveFloat or UCurveLinearColor and return its keys. Float curves
 * return type:"float" with [{ time, value }]; color curves return type:"color"
 * with [{ time, r, g, b, a }].
 *
 * @param {string} asset_path - Content path of the curve asset
 * @returns {Promise<{ success, asset_path, type, keys }>}
 */
export async function readCurve({ asset_path } = {}) {
  return bridgeFetch("/editor/read_curve", {
    method: "POST",
    body: JSON.stringify({ asset_path }),
  });
}

/**
 * POST /editor/create_post_process_material
 *
 * Builds a UMaterial with MaterialDomain = MD_PostProcess that highlights
 * meshes with RenderCustomDepthPass enabled, but ONLY when those meshes are
 * occluded by another opaque pixel (i.e. behind a wall). Used for the
 * "enemy through walls" outline effect.
 *
 * @param {string} asset_path                   - Content path, e.g. '/Game/Materials/M_EnemyHighlight'
 * @param {string} [effect="occlusion_outline"] - Effect preset (only one supported today)
 * @param {{r:number,g:number,b:number}} [highlight_color] - Linear-space RGB (default red)
 * @returns {Promise<{ success, asset_path, already_existed, effect, material_domain }>}
 */
export async function createPostProcessMaterial({ asset_path, effect, highlight_color } = {}) {
  return bridgeFetch("/editor/create_post_process_material", {
    method: "POST",
    body: JSON.stringify({
      asset_path,
      ...(effect          ? { effect }          : {}),
      ...(highlight_color ? { highlight_color } : {}),
    }),
  });
}

/**
 * POST /editor/spawn_post_process_volume
 *
 * Spawns (or reuses) an APostProcessVolume in the current editor world,
 * configures it as unbounded, and adds the supplied material to the
 * PostProcessSettings.WeightedBlendables array.
 *
 * Idempotent: calling with the same actor_label twice updates the existing
 * volume instead of duplicating it.
 *
 * @param {string}  material_path            - Content path of a UMaterialInterface
 * @param {string}  [actor_label]            - Volume actor label (default 'PP_<MaterialName>')
 * @param {boolean} [unbounded=true]         - When true, post-process applies regardless of camera location
 * @param {number}  [priority=0]             - Volume priority (higher overrides lower in overlapping volumes)
 * @returns {Promise<{ success, actor_label, already_existed, material_path, material_added, unbounded, priority, blendables_count }>}
 */
export async function spawnPostProcessVolume({ material_path, actor_label, unbounded, priority } = {}) {
  return bridgeFetch("/editor/spawn_post_process_volume", {
    method: "POST",
    body: JSON.stringify({
      material_path,
      ...(actor_label != null ? { actor_label } : {}),
      ...(unbounded   != null ? { unbounded }   : {}),
      ...(priority    != null ? { priority }    : {}),
    }),
  });
}

// ---------------------------------------------------------------------------
// Mesh composition (DynamicMesh handles on the UE5 plugin side)
// ---------------------------------------------------------------------------

/** POST /mesh/create */
export async function meshCreate(handle) {
  return bridgeFetch("/mesh/create", {
    method: "POST",
    body: JSON.stringify({ handle }),
  });
}

/** POST /mesh/append_primitive */
export async function meshAppendPrimitive({ handle, shape, transform, params } = {}) {
  return bridgeFetch("/mesh/append_primitive", {
    method: "POST",
    body: JSON.stringify({
      handle,
      shape,
      ...(transform ? { transform } : {}),
      ...(params    ? { params }    : {}),
    }),
  });
}

/** POST /mesh/boolean */
export async function meshBoolean({ handle, other_handle, op, transform } = {}) {
  return bridgeFetch("/mesh/boolean", {
    method: "POST",
    body: JSON.stringify({
      handle,
      other_handle,
      op,
      ...(transform ? { transform } : {}),
    }),
  });
}

/** POST /mesh/transform */
export async function meshTransform({ handle, location, rotation, scale } = {}) {
  return bridgeFetch("/mesh/transform", {
    method: "POST",
    body: JSON.stringify({
      handle,
      ...(location ? { location } : {}),
      ...(rotation ? { rotation } : {}),
      ...(scale    ? { scale }    : {}),
    }),
  });
}

/** POST /mesh/deform */
export async function meshDeform({ handle, op, axis, amount, upper, lower } = {}) {
  return bridgeFetch("/mesh/deform", {
    method: "POST",
    body: JSON.stringify({
      handle,
      op,
      axis,
      amount,
      ...(upper != null ? { upper } : {}),
      ...(lower != null ? { lower } : {}),
    }),
  });
}

/** POST /mesh/remesh */
export async function meshRemesh({ handle, target_edge_length, iterations, smoothing } = {}) {
  return bridgeFetch("/mesh/remesh", {
    method: "POST",
    body: JSON.stringify({
      handle,
      ...(target_edge_length != null ? { target_edge_length } : {}),
      ...(iterations         != null ? { iterations }         : {}),
      ...(smoothing          != null ? { smoothing }          : {}),
    }),
  });
}

/** POST /mesh/bake_static */
export async function meshBakeStatic({ handle, asset_path } = {}) {
  return bridgeFetch("/mesh/bake_static", {
    method: "POST",
    body: JSON.stringify({ handle, asset_path }),
  });
}

/** POST /mesh/delete_handle */
export async function meshDeleteHandle(handle) {
  return bridgeFetch("/mesh/delete_handle", {
    method: "POST",
    body: JSON.stringify({ handle }),
  });
}

/** POST /editor/anim/add_copy_bone — splice a Copy Bone skeletal control node into an AnimBP's AnimGraph */
export async function animAddCopyBone({
  anim_bp_path,
  source_bone,
  target_bone,
  copy_translation,
  copy_rotation,
  copy_scale,
  control_space,
  node_offset_x,
  node_offset_y,
} = {}) {
  return bridgeFetch("/editor/anim/add_copy_bone", {
    method: "POST",
    body: JSON.stringify({
      anim_bp_path,
      source_bone,
      target_bone,
      ...(copy_translation != null ? { copy_translation } : {}),
      ...(copy_rotation    != null ? { copy_rotation }    : {}),
      ...(copy_scale       != null ? { copy_scale }       : {}),
      ...(control_space    ? { control_space } : {}),
      ...(node_offset_x    != null ? { node_offset_x } : {}),
      ...(node_offset_y    != null ? { node_offset_y } : {}),
    }),
  });
}

/** POST /editor/anim/add_hand_ik_retargeting — splice a Hand IK Retargeting node into an AnimBP's AnimGraph */
export async function animAddHandIKRetargeting({
  anim_bp_path,
  right_hand_fk,
  left_hand_fk,
  right_hand_ik,
  left_hand_ik,
  ik_bones_to_move,
  hand_fk_weight,
  per_axis_alpha,
  alpha_curve_name,
  alpha_scale,
  alpha_bias,
  node_offset_x,
  node_offset_y,
} = {}) {
  return bridgeFetch("/editor/anim/add_hand_ik_retargeting", {
    method: "POST",
    body: JSON.stringify({
      anim_bp_path,
      right_hand_fk,
      left_hand_fk,
      right_hand_ik,
      left_hand_ik,
      ...(ik_bones_to_move ? { ik_bones_to_move } : {}),
      ...(hand_fk_weight   != null ? { hand_fk_weight }   : {}),
      ...(per_axis_alpha   ? { per_axis_alpha } : {}),
      ...(alpha_curve_name ? { alpha_curve_name } : {}),
      ...(alpha_scale      != null ? { alpha_scale } : {}),
      ...(alpha_bias       != null ? { alpha_bias }  : {}),
      ...(node_offset_x    != null ? { node_offset_x } : {}),
      ...(node_offset_y    != null ? { node_offset_y } : {}),
    }),
  });
}

/** POST /editor/skeleton/add_virtual_bone — add a virtual bone to a USkeleton */
export async function skeletonAddVirtualBone({
  skeleton_path,
  source_bone,
  target_bone,
  vb_name,
} = {}) {
  return bridgeFetch("/editor/skeleton/add_virtual_bone", {
    method: "POST",
    body: JSON.stringify({
      skeleton_path,
      source_bone,
      target_bone,
      ...(vb_name ? { vb_name } : {}),
    }),
  });
}

/** POST /editor/anim/add_two_bone_ik — splice a Two Bone IK node into an AnimBP's AnimGraph */
export async function animAddTwoBoneIK({
  anim_bp_path,
  ik_bone,
  effector_location_space,
  joint_target_location_space,
  take_rotation_from_effector,
  effector_target_bone,
  joint_target_bone,
  joint_target_offset,
  node_offset_x,
  node_offset_y,
} = {}) {
  return bridgeFetch("/editor/anim/add_two_bone_ik", {
    method: "POST",
    body: JSON.stringify({
      anim_bp_path,
      ik_bone,
      ...(effector_location_space     ? { effector_location_space }     : {}),
      ...(joint_target_location_space ? { joint_target_location_space } : {}),
      ...(take_rotation_from_effector != null ? { take_rotation_from_effector } : {}),
      ...(effector_target_bone ? { effector_target_bone } : {}),
      ...(joint_target_bone    ? { joint_target_bone }    : {}),
      ...(joint_target_offset  ? { joint_target_offset }  : {}),
      ...(node_offset_x != null ? { node_offset_x } : {}),
      ...(node_offset_y != null ? { node_offset_y } : {}),
    }),
  });
}

/** POST /editor/anim/add_layered_bone_blend — splice a Layered Bone Blend node into an AnimBP's AnimGraph */
export async function animAddLayeredBoneBlend({
  anim_bp_path,
  graph,
  branch_filters,
  blend_weights,
  blend_mode,
  alpha,
  splice_before_output,
  node_offset_x,
  node_offset_y,
} = {}) {
  return bridgeFetch("/editor/anim/add_layered_bone_blend", {
    method: "POST",
    body: JSON.stringify({
      anim_bp_path,
      ...(graph                ? { graph } : {}),
      ...(branch_filters       ? { branch_filters } : {}),
      ...(blend_weights        ? { blend_weights }  : {}),
      ...(blend_mode           ? { blend_mode }     : {}),
      ...(alpha                != null ? { alpha } : {}),
      ...(splice_before_output != null ? { splice_before_output } : {}),
      ...(node_offset_x        != null ? { node_offset_x } : {}),
      ...(node_offset_y        != null ? { node_offset_y } : {}),
    }),
  });
}

/** POST /editor/anim/add_sequence_player — spawn a SequencePlayer node in an AnimBP's AnimGraph */
export async function animAddSequencePlayer({
  anim_bp_path,
  graph,
  sequence,
  loop,
  play_rate,
  start_position,
  node_offset_x,
  node_offset_y,
} = {}) {
  return bridgeFetch("/editor/anim/add_sequence_player", {
    method: "POST",
    body: JSON.stringify({
      anim_bp_path,
      sequence,
      ...(graph          ? { graph } : {}),
      ...(loop           != null ? { loop }           : {}),
      ...(play_rate      != null ? { play_rate }      : {}),
      ...(start_position != null ? { start_position } : {}),
      ...(node_offset_x  != null ? { node_offset_x }  : {}),
      ...(node_offset_y  != null ? { node_offset_y }  : {}),
    }),
  });
}

/** POST /editor/anim/add_modify_bone — splice a Transform (Modify) Bone node into an AnimBP's AnimGraph */
export async function animAddModifyBone({
  anim_bp_path,
  graph,
  bone_name,
  translation_mode,
  rotation_mode,
  scale_mode,
  translation_space,
  rotation_space,
  scale_space,
  translation,
  rotation,
  scale,
  alpha,
  splice_before_output,
  node_offset_x,
  node_offset_y,
} = {}) {
  return bridgeFetch("/editor/anim/add_modify_bone", {
    method: "POST",
    body: JSON.stringify({
      anim_bp_path,
      bone_name,
      ...(graph             ? { graph }             : {}),
      ...(translation_mode  ? { translation_mode }  : {}),
      ...(rotation_mode     ? { rotation_mode }     : {}),
      ...(scale_mode        ? { scale_mode }        : {}),
      ...(translation_space ? { translation_space } : {}),
      ...(rotation_space    ? { rotation_space }    : {}),
      ...(scale_space       ? { scale_space }       : {}),
      ...(translation       ? { translation }       : {}),
      ...(rotation          ? { rotation }          : {}),
      ...(scale             ? { scale }             : {}),
      ...(alpha             != null ? { alpha } : {}),
      ...(splice_before_output != null ? { splice_before_output } : {}),
      ...(node_offset_x     != null ? { node_offset_x } : {}),
      ...(node_offset_y     != null ? { node_offset_y } : {}),
    }),
  });
}

/** POST /editor/anim/add_look_at — splice a LookAt skeletal control node into an AnimBP's AnimGraph */
export async function animAddLookAt({
  anim_bp_path,
  graph,
  bone_to_modify,
  look_at_axis,
  look_at_axis_local,
  look_at_target_bone,
  look_at_socket,
  look_at_location,
  look_at_location_space,
  look_at_clamp,
  interpolation_type,
  interpolation_time,
  alpha,
  splice_before_output,
  node_offset_x,
  node_offset_y,
} = {}) {
  return bridgeFetch("/editor/anim/add_look_at", {
    method: "POST",
    body: JSON.stringify({
      anim_bp_path,
      bone_to_modify,
      ...(graph                  ? { graph } : {}),
      ...(look_at_axis           ? { look_at_axis } : {}),
      ...(look_at_axis_local     != null ? { look_at_axis_local } : {}),
      ...(look_at_target_bone    ? { look_at_target_bone } : {}),
      ...(look_at_socket         ? { look_at_socket } : {}),
      ...(look_at_location       ? { look_at_location } : {}),
      ...(look_at_location_space ? { look_at_location_space } : {}),
      ...(look_at_clamp          != null ? { look_at_clamp } : {}),
      ...(interpolation_type     ? { interpolation_type } : {}),
      ...(interpolation_time     != null ? { interpolation_time } : {}),
      ...(alpha                  != null ? { alpha } : {}),
      ...(splice_before_output   != null ? { splice_before_output } : {}),
      ...(node_offset_x          != null ? { node_offset_x } : {}),
      ...(node_offset_y          != null ? { node_offset_y } : {}),
    }),
  });
}

/** POST /editor/anim/add_aim_offset_blend_space — splice an AimOffset (RotationOffsetBlendSpace) node into an AnimBP's AnimGraph */
export async function animAddAimOffsetBlendSpace({
  anim_bp_path,
  graph,
  blend_space,
  alpha,
  splice_before_output,
  node_offset_x,
  node_offset_y,
} = {}) {
  return bridgeFetch("/editor/anim/add_aim_offset_blend_space", {
    method: "POST",
    body: JSON.stringify({
      anim_bp_path,
      blend_space,
      ...(graph                ? { graph } : {}),
      ...(alpha                != null ? { alpha } : {}),
      ...(splice_before_output != null ? { splice_before_output } : {}),
      ...(node_offset_x        != null ? { node_offset_x } : {}),
      ...(node_offset_y        != null ? { node_offset_y } : {}),
    }),
  });
}

/** POST /editor/anim/delete_node — remove an AnimGraph node by GUID, reconnecting the pose chain */
export async function animDeleteNode({ anim_bp_path, node_id, reconnect_pose } = {}) {
  return bridgeFetch("/editor/anim/delete_node", {
    method: "POST",
    body: JSON.stringify({
      anim_bp_path,
      node_id,
      ...(reconnect_pose != null ? { reconnect_pose } : {}),
    }),
  });
}

/** POST /mesh/add_socket — add a socket to a UStaticMesh, USkeletalMesh, or USkeleton asset */
export async function meshAddSocket({
  mesh_path,
  socket_name,
  bone_name,
  location,
  rotation,
  scale,
  replace,
  target,
} = {}) {
  return bridgeFetch("/mesh/add_socket", {
    method: "POST",
    body: JSON.stringify({
      mesh_path,
      socket_name,
      ...(bone_name ? { bone_name } : {}),
      ...(location  ? { location }  : {}),
      ...(rotation  ? { rotation }  : {}),
      ...(scale     ? { scale }     : {}),
      ...(replace != null ? { replace } : {}),
      ...(target   ? { target }   : {}),
    }),
  });
}

// ---------------------------------------------------------------------------
// Blueprint graph authoring
// ---------------------------------------------------------------------------

/** POST /bp/add_node — create a K2 node by class path. Advanced escape hatch. */
export async function bpAddNode({ blueprint, graph, node_class, node_id, config, pin_defaults, position } = {}) {
  return bridgeFetch("/bp/add_node", {
    method: "POST",
    body: JSON.stringify({
      blueprint,
      ...(graph       ? { graph }       : {}),
      node_class,
      ...(node_id     ? { node_id }     : {}),
      ...(config      ? { config }      : {}),
      ...(pin_defaults ? { pin_defaults } : {}),
      ...(position    ? { position }    : {}),
    }),
  });
}

/** POST /bp/connect_pins — wire two pins by node_id + pin name. */
export async function bpConnectPins({ blueprint, graph, from_node, from_pin, to_node, to_pin } = {}) {
  return bridgeFetch("/bp/connect_pins", {
    method: "POST",
    body: JSON.stringify({
      blueprint,
      ...(graph ? { graph } : {}),
      from_node, from_pin, to_node, to_pin,
    }),
  });
}

/** POST /bp/compile — compile a blueprint, optionally save on success. */
export async function bpCompile({ blueprint, save } = {}) {
  return bridgeFetch("/bp/compile", {
    method: "POST",
    body: JSON.stringify({
      blueprint,
      ...(save != null ? { save } : {}),
    }),
  });
}

/** POST /asset/create_blueprint_struct — create a UUserDefinedStruct (S_Foo). */
export async function createBlueprintStruct({ asset_path, fields, save } = {}) {
  return bridgeFetch("/asset/create_blueprint_struct", {
    method: "POST",
    body: JSON.stringify({
      asset_path,
      fields: fields ?? [],
      ...(save != null ? { save } : {}),
    }),
  });
}

/** POST /asset/create_blueprint_enum — create a UUserDefinedEnum (E_Foo). */
export async function createBlueprintEnum({ asset_path, entries, save } = {}) {
  return bridgeFetch("/asset/create_blueprint_enum", {
    method: "POST",
    body: JSON.stringify({
      asset_path,
      entries: entries ?? [],
      ...(save != null ? { save } : {}),
    }),
  });
}

/** POST /asset/create_blueprint_interface — create a Blueprint Interface (BPI_Foo). */
export async function createBlueprintInterface({ asset_path, functions, save } = {}) {
  return bridgeFetch("/asset/create_blueprint_interface", {
    method: "POST",
    body: JSON.stringify({
      asset_path,
      functions: functions ?? [],
      ...(save != null ? { save } : {}),
    }),
  });
}

/** POST /asset/create_anim_blueprint — create a UAnimBlueprint (ABP_Foo). */
export async function createAnimBlueprint({ asset_path, target_skeleton, parent_class, save } = {}) {
  return bridgeFetch("/asset/create_anim_blueprint", {
    method: "POST",
    body: JSON.stringify({
      asset_path,
      target_skeleton,
      ...(parent_class ? { parent_class } : {}),
      ...(save != null ? { save } : {}),
    }),
  });
}

/** POST /bp/create_function — add a custom function graph to a Blueprint. */
export async function bpCreateFunction({ blueprint, name, inputs, outputs, is_pure, is_const, category, compile, save } = {}) {
  return bridgeFetch("/bp/create_function", {
    method: "POST",
    body: JSON.stringify({
      blueprint,
      name,
      ...(inputs   ? { inputs }   : {}),
      ...(outputs  ? { outputs }  : {}),
      ...(is_pure  != null ? { is_pure }  : {}),
      ...(is_const != null ? { is_const } : {}),
      ...(category ? { category } : {}),
      ...(compile  != null ? { compile }  : {}),
      ...(save     != null ? { save }     : {}),
    }),
  });
}

/** POST /bp/create_macro — add a macro graph to a Blueprint. */
export async function bpCreateMacro({ blueprint, name, inputs, outputs, category, compile, save } = {}) {
  return bridgeFetch("/bp/create_macro", {
    method: "POST",
    body: JSON.stringify({
      blueprint,
      name,
      ...(inputs   ? { inputs }   : {}),
      ...(outputs  ? { outputs }  : {}),
      ...(category ? { category } : {}),
      ...(compile  != null ? { compile }  : {}),
      ...(save     != null ? { save }     : {}),
    }),
  });
}

/** POST /bp/create_variable — add a member variable to a Blueprint. */
export async function bpCreateVariable({
  blueprint,
  name,
  type,
  default_value,
  category,
  instance_editable,
  blueprint_read_only,
  expose_on_spawn,
  private: isPrivate,
  compile,
  save,
} = {}) {
  return bridgeFetch("/bp/create_variable", {
    method: "POST",
    body: JSON.stringify({
      blueprint,
      name,
      type,
      ...(default_value       != null ? { default_value }       : {}),
      ...(category            != null ? { category }            : {}),
      ...(instance_editable   != null ? { instance_editable }   : {}),
      ...(blueprint_read_only != null ? { blueprint_read_only } : {}),
      ...(expose_on_spawn     != null ? { expose_on_spawn }     : {}),
      ...(isPrivate           != null ? { private: isPrivate }  : {}),
      ...(compile             != null ? { compile }             : {}),
      ...(save                != null ? { save }                : {}),
    }),
  });
}

/** POST /bp/delete_function — remove a user-created function graph from a Blueprint. */
export async function bpDeleteFunction({ blueprint, name, compile, save } = {}) {
  return bridgeFetch("/bp/delete_function", {
    method: "POST",
    body: JSON.stringify({
      blueprint,
      name,
      ...(compile != null ? { compile } : {}),
      ...(save    != null ? { save }    : {}),
    }),
  });
}

/** POST /bp/delete_variable — remove a member variable from a Blueprint. */
export async function bpDeleteVariable({ blueprint, name, compile, save } = {}) {
  return bridgeFetch("/bp/delete_variable", {
    method: "POST",
    body: JSON.stringify({
      blueprint,
      name,
      ...(compile != null ? { compile } : {}),
      ...(save    != null ? { save }    : {}),
    }),
  });
}

/**
 * POST /bp/heal_world_context — repair the __WorldContext static-function cook
 * corruption on one or more Blueprints. Accepts a single `blueprint` or a
 * `blueprints` array; `dry_run` reports per-function diagnostics with no mutation.
 */
export async function bpHealWorldContext({ blueprint, blueprints, compile, save, dry_run } = {}) {
  return bridgeFetch("/bp/heal_world_context", {
    method: "POST",
    body: JSON.stringify({
      ...(blueprint  != null ? { blueprint }  : {}),
      ...(blueprints != null ? { blueprints } : {}),
      ...(compile    != null ? { compile }    : {}),
      ...(save       != null ? { save }       : {}),
      ...(dry_run    != null ? { dry_run }    : {}),
    }),
  });
}

/** GET /bp/list_variables — read all member variables on a Blueprint. */
export async function bpListVariables({ blueprint } = {}) {
  const params = new URLSearchParams();
  params.set("blueprint", blueprint);
  return bridgeFetch(`/bp/list_variables?${params.toString()}`);
}

/** POST /bp/add_logic — high-level: nodes[] + connections[] in one call. */
export async function bpAddLogic({ blueprint, graph, nodes, connections, auto_layout, compile, save } = {}) {
  return bridgeFetch("/bp/add_logic", {
    method: "POST",
    body: JSON.stringify({
      blueprint,
      ...(graph       ? { graph } : {}),
      nodes,
      ...(connections ? { connections } : {}),
      ...(auto_layout != null ? { auto_layout } : {}),
      ...(compile     != null ? { compile }     : {}),
      ...(save        != null ? { save }        : {}),
    }),
  });
}

/** POST /bp/delete_node — delete a node by id or guid; auto compile+save. */
export async function bpDeleteNode({ blueprint, graph, node_id, node_guid, compile, save } = {}) {
  return bridgeFetch("/bp/delete_node", {
    method: "POST",
    body: JSON.stringify({
      blueprint,
      ...(graph     ? { graph }     : {}),
      ...(node_id   ? { node_id }   : {}),
      ...(node_guid ? { node_guid } : {}),
      ...(compile != null ? { compile } : {}),
      ...(save    != null ? { save }    : {}),
    }),
  });
}

/** GET /bp/read_graph — return a graph (or all graphs) as JSON. */
export async function bpReadGraph({ blueprint, graph } = {}) {
  const params = new URLSearchParams();
  params.set("blueprint", blueprint);
  if (graph) params.set("graph", graph);
  return bridgeFetch(`/bp/read_graph?${params.toString()}`);
}

/**
 * GET /bp/get_selection — return the nodes the developer currently has
 * selected in an open Blueprint editor, plus a "boundary" classification of
 * pins that cross the selection set.
 *
 * If `blueprint` is omitted, the server picks the most recently-active BP
 * editor that has a non-empty selection.
 */
export async function bpGetSelection({ blueprint, include_pins, include_boundary } = {}) {
  const params = new URLSearchParams();
  if (blueprint) params.set("blueprint", blueprint);
  if (include_pins     != null) params.set("include_pins",     include_pins     ? "1" : "0");
  if (include_boundary != null) params.set("include_boundary", include_boundary ? "1" : "0");
  const qs = params.toString();
  return bridgeFetch(`/bp/get_selection${qs ? `?${qs}` : ""}`);
}

/** POST /bp/lint — analyze a BP for graph issues; optionally auto-fix + compile + save. */
export async function bpLint({ blueprint, auto_fix, compile, save } = {}) {
  return bridgeFetch("/bp/lint", {
    method: "POST",
    body: JSON.stringify({
      blueprint,
      ...(auto_fix != null ? { auto_fix } : {}),
      ...(compile  != null ? { compile }  : {}),
      ...(save     != null ? { save }     : {}),
    }),
  });
}

/** POST /bp/lint_project — lint every BP_* under path_prefix (default "/Game"). */
export async function bpLintProject({ path_prefix, auto_fix, compile, save } = {}) {
  return bridgeFetch("/bp/lint_project", {
    method: "POST",
    body: JSON.stringify({
      ...(path_prefix ? { path_prefix } : {}),
      ...(auto_fix != null ? { auto_fix } : {}),
      ...(compile  != null ? { compile }  : {}),
      ...(save     != null ? { save }     : {}),
    }),
  });
}

/**
 * POST /editor/exec_python — run a Python string inside the editor.
 * @param {string} command - Python source.
 * @param {"execute_file"|"execute_statement"|"evaluate_statement"} mode
 */
export async function execPython(command, mode = "execute_file") {
  return bridgeFetch("/editor/exec_python", {
    method: "POST",
    body: JSON.stringify({ command, mode }),
  });
}

// ---- Behavior Tree authoring (Phase 1: asset creation) -------------------

/** POST /bt/create_tree — create a UBehaviorTree, optionally linking a Blackboard. */
export async function btCreateTree({ asset_path, blackboard_path, save } = {}) {
  return bridgeFetch("/bt/create_tree", {
    method: "POST",
    body: JSON.stringify({
      asset_path,
      ...(blackboard_path ? { blackboard_path } : {}),
      ...(save != null ? { save } : {}),
    }),
  });
}

/** POST /bt/create_blackboard — create a UBlackboardData, optionally inheriting from a parent. */
export async function btCreateBlackboard({ asset_path, parent_blackboard_path, save } = {}) {
  return bridgeFetch("/bt/create_blackboard", {
    method: "POST",
    body: JSON.stringify({
      asset_path,
      ...(parent_blackboard_path ? { parent_blackboard_path } : {}),
      ...(save != null ? { save } : {}),
    }),
  });
}

/** POST /bt/add_blackboard_keys — append typed entries to a UBlackboardData. */
export async function btAddBlackboardKeys({ asset_path, keys, save } = {}) {
  return bridgeFetch("/bt/add_blackboard_keys", {
    method: "POST",
    body: JSON.stringify({
      asset_path,
      keys: keys ?? [],
      ...(save != null ? { save } : {}),
    }),
  });
}

/** POST /bt/add_logic — author the BT graph (composites + tasks + decorators + services + wiring). */
export async function btAddLogic({
  behavior_tree, nodes, decorators, services, connections, clear, compile, save,
} = {}) {
  return bridgeFetch("/bt/add_logic", {
    method: "POST",
    body: JSON.stringify({
      behavior_tree,
      nodes:       nodes       ?? [],
      ...(decorators  ? { decorators }  : {}),
      ...(services    ? { services }    : {}),
      ...(connections ? { connections } : {}),
      ...(clear   != null ? { clear }   : {}),
      ...(compile != null ? { compile } : {}),
      ...(save    != null ? { save }    : {}),
    }),
  });
}

/** GET /bt/read_tree?behavior_tree=... — dump the BT EdGraph as JSON. */
export async function btReadTree({ behavior_tree } = {}) {
  const qs = new URLSearchParams({ behavior_tree: behavior_tree ?? "" }).toString();
  return bridgeFetch(`/bt/read_tree?${qs}`, { method: "GET" });
}

/** GET /statetree/read_tree?state_tree=... — dump the StateTree authoring hierarchy as JSON. */
export async function stateTreeReadTree({ state_tree } = {}) {
  const qs = new URLSearchParams({ state_tree: state_tree ?? "" }).toString();
  return bridgeFetch(`/statetree/read_tree?${qs}`, { method: "GET" });
}

/** POST /statetree/repoint_node — rewrite every node whose wrapped class is from_class to to_class (migrating instance props by name). */
export async function stateTreeRepointNode({ state_tree, from_class, to_class, compile, save, dry_run } = {}) {
  return bridgeFetch("/statetree/repoint_node", {
    method: "POST",
    body: JSON.stringify({
      state_tree,
      from_class,
      ...(to_class != null ? { to_class } : {}),
      ...(compile != null ? { compile } : {}),
      ...(save != null ? { save } : {}),
      ...(dry_run != null ? { dry_run } : {}),
    }),
  });
}

// ---------------------------------------------------------------------------
// Gameplay Ability System (GAS) endpoints
// ---------------------------------------------------------------------------

/** POST /gas/setup_actor — add UAbilitySystemComponent to a Blueprint actor. */
export async function gasSetupActor({ blueprint_path, replication_mode, save } = {}) {
  return bridgeFetch("/gas/setup_actor", {
    method: "POST",
    body: JSON.stringify({
      blueprint_path,
      ...(replication_mode != null ? { replication_mode } : {}),
      ...(save != null ? { save } : {}),
    }),
  });
}

/** POST /gas/create_attribute_set — create a Blueprint subclass of UAttributeSet. */
export async function gasCreateAttributeSet({ asset_path, parent_class, attributes, save } = {}) {
  return bridgeFetch("/gas/create_attribute_set", {
    method: "POST",
    body: JSON.stringify({
      asset_path,
      ...(parent_class != null ? { parent_class } : {}),
      ...(attributes != null ? { attributes } : {}),
      ...(save != null ? { save } : {}),
    }),
  });
}

/** POST /gas/create_ability — create a Blueprint subclass of UGameplayAbility. */
export async function gasCreateAbility({
  asset_path, parent_class, net_execution_policy, instancing_policy,
  ability_tags, block_ability_tags, cancel_abilities_tags,
  cost_effect, cooldown_effect, activation_group, save
} = {}) {
  return bridgeFetch("/gas/create_ability", {
    method: "POST",
    body: JSON.stringify({
      asset_path,
      ...(parent_class != null ? { parent_class } : {}),
      ...(net_execution_policy != null ? { net_execution_policy } : {}),
      ...(instancing_policy != null ? { instancing_policy } : {}),
      ...(ability_tags != null ? { ability_tags } : {}),
      ...(block_ability_tags != null ? { block_ability_tags } : {}),
      ...(cancel_abilities_tags != null ? { cancel_abilities_tags } : {}),
      ...(cost_effect != null ? { cost_effect } : {}),
      ...(cooldown_effect != null ? { cooldown_effect } : {}),
      ...(activation_group != null ? { activation_group } : {}),
      ...(save != null ? { save } : {}),
    }),
  });
}

/** POST /gas/create_effect — create a Blueprint subclass of UGameplayEffect. */
export async function gasCreateEffect({
  asset_path, parent_class, duration_policy, duration, period,
  stacking_type, stack_limit, modifiers,
  granted_tags, application_required_tags, ongoing_required_tags, immunity_tags, save
} = {}) {
  return bridgeFetch("/gas/create_effect", {
    method: "POST",
    body: JSON.stringify({
      asset_path,
      ...(parent_class != null ? { parent_class } : {}),
      ...(duration_policy != null ? { duration_policy } : {}),
      ...(duration != null ? { duration } : {}),
      ...(period != null ? { period } : {}),
      ...(stacking_type != null ? { stacking_type } : {}),
      ...(stack_limit != null ? { stack_limit } : {}),
      ...(modifiers != null ? { modifiers } : {}),
      ...(granted_tags != null ? { granted_tags } : {}),
      ...(application_required_tags != null ? { application_required_tags } : {}),
      ...(ongoing_required_tags != null ? { ongoing_required_tags } : {}),
      ...(immunity_tags != null ? { immunity_tags } : {}),
      ...(save != null ? { save } : {}),
    }),
  });
}

/** POST /gas/configure_asc — update an existing ASC's replication mode on a Blueprint. */
export async function gasConfigureAsc({ blueprint_path, replication_mode, save } = {}) {
  return bridgeFetch("/gas/configure_asc", {
    method: "POST",
    body: JSON.stringify({
      blueprint_path,
      ...(replication_mode != null ? { replication_mode } : {}),
      ...(save != null ? { save } : {}),
    }),
  });
}

/** GET /gas/read_setup?blueprint_path=... — inspect GAS configuration of a Blueprint. */
export async function gasReadSetup({ blueprint_path } = {}) {
  const qs = new URLSearchParams({ blueprint_path: blueprint_path ?? "" }).toString();
  return bridgeFetch(`/gas/read_setup?${qs}`, { method: "GET" });
}

