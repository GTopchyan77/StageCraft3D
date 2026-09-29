// Copyright 2025-2026 NGG. All Rights Reserved.
// paths.js — unrealngg-mcp
// Pure, side-effect-free helpers for discovering the .uproject, Unreal Engine
// install directory, and build targets. Extracted from index.js so tests can
// import them without booting the MCP server.

import fs from "fs";
import path from "path";
import { execSync } from "child_process";

/**
 * Walk up from `startDir` (default process.cwd()) looking for a `.uproject`
 * file. Returns the absolute path or null.
 *
 * @param {string} [startDir]
 * @returns {string|null}
 */
export function findUProject(startDir) {
  let dir = startDir ?? process.cwd();
  for (let i = 0; i < 10; i++) {
    let entries;
    try { entries = fs.readdirSync(dir); } catch { return null; }
    const match = entries.find(f => f.endsWith(".uproject"));
    if (match) return path.join(dir, match);
    const parent = path.dirname(dir);
    if (parent === dir) break;
    dir = parent;
  }
  return null;
}

/**
 * Read the EngineAssociation field from a .uproject file. Returns null on any
 * read/parse failure or if the field is missing.
 *
 * @param {string} uprojectPath
 * @returns {string|null}
 */
export function readEngineAssociation(uprojectPath) {
  try {
    const data = JSON.parse(fs.readFileSync(uprojectPath, "utf8"));
    return data.EngineAssociation ?? null;
  } catch { return null; }
}

// An EngineAssociation is either a version ("5.8") or a source-build GUID
// ("{A1B2...}"). It comes from a file on disk and is interpolated into a shell
// command below, so anything outside that alphabet is refused rather than
// escaped — there is no legitimate association that needs quotes or separators.
const ENGINE_ASSOCIATION_RE = /^[A-Za-z0-9._{}-]+$/;

/**
 * Read InstalledDirectory for a launcher-installed engine out of HKLM. This is
 * where the Epic Games Launcher records every installed engine version, keyed
 * by "5.8"-style version. HKCU\...\Builds (queried separately) only ever holds
 * *custom* source builds the user registered by hand, so a machine with only
 * launcher installs has no HKCU key at all.
 *
 * @returns {string|null} the install root, or null when absent/unusable
 */
function readHklmInstallDir(engineAssociation, execSyncFn, existsSyncFn) {
  try {
    const regOut = execSyncFn(
      `reg query "HKLM\\SOFTWARE\\EpicGames\\Unreal Engine\\${engineAssociation}" /v InstalledDirectory 2>nul`,
      { encoding: "utf8", timeout: 5000 }
    );
    const match = regOut.match(/^\s*InstalledDirectory\s+REG_SZ\s+(.+)$/m);
    if (match) {
      const dir = match[1].trim();
      if (existsSyncFn(path.join(dir, "Engine", "Build", "BatchFiles", "Build.bat"))) {
        return dir;
      }
    }
  } catch { /* key missing, or reg unavailable — caller falls through */ }
  return null;
}

/**
 * Read the launcher's own manifest. Covers the case where the registry is
 * missing or stale but the launcher still knows where the engine lives.
 *
 * The manifest lists plugins (FabPlugin_5.8, QuixelBridge_5.8) alongside the
 * engines themselves, and those share the engine's InstallLocation — so match
 * on AppName/ArtifactId `UE_<version>` rather than taking the first hit.
 *
 * @returns {string|null} the install root, or null when absent/unusable
 */
function readLauncherManifest(engineAssociation, existsSyncFn, readFileSyncFn, env) {
  const programData = env.PROGRAMDATA || "C:\\ProgramData";
  const manifest = path.join(
    programData, "Epic", "UnrealEngineLauncher", "LauncherInstalled.dat"
  );
  try {
    if (!existsSyncFn(manifest)) return null;
    const parsed = JSON.parse(readFileSyncFn(manifest, "utf8"));
    const list = Array.isArray(parsed?.InstallationList) ? parsed.InstallationList : [];
    const wanted = `UE_${engineAssociation}`;
    for (const entry of list) {
      if (entry?.AppName !== wanted && entry?.ArtifactId !== wanted) continue;
      const dir = entry?.InstallLocation;
      if (typeof dir !== "string" || !dir) continue;
      if (existsSyncFn(path.join(dir, "Engine", "Build", "BatchFiles", "Build.bat"))) {
        return dir;
      }
    }
  } catch { /* missing, unreadable or malformed — caller falls through */ }
  return null;
}

/**
 * Discover the UE install directory.
 * Priority: NGG_UE_INSTALL_DIR env > HKCU custom builds > HKLM launcher
 *           installs > the launcher's LauncherInstalled.dat manifest >
 *           well-known install paths on every available drive letter.
 *
 * Windows only — the drive-letter scan and `reg query` have no meaning
 * elsewhere. On other platforms set NGG_UE_INSTALL_DIR.
 *
 * Throws if nothing is found.
 *
 * @param {string|null} engineAssociation - value from .uproject EngineAssociation
 * @param {object}      [opts]
 * @param {typeof fs.existsSync}   [opts.existsSync]   - injectable for tests
 * @param {typeof execSync}        [opts.execSync]     - injectable for tests
 * @param {typeof fs.readFileSync} [opts.readFileSync] - injectable for tests
 * @param {NodeJS.ProcessEnv}      [opts.env]          - injectable for tests
 * @returns {string}
 */
export function discoverEngineDir(engineAssociation, opts = {}) {
  const existsSyncFn   = opts.existsSync   ?? fs.existsSync;
  const execSyncFn     = opts.execSync     ?? execSync;
  const readFileSyncFn = opts.readFileSync ?? fs.readFileSync;
  const env            = opts.env          ?? process.env;

  // Refuse an association that can't be safely interpolated into `reg query`.
  // Discovery continues without the registry/manifest steps rather than failing
  // outright — the drive scan may still find the engine.
  const safeAssociation =
    engineAssociation && ENGINE_ASSOCIATION_RE.test(engineAssociation)
      ? engineAssociation
      : null;

  // 1. Explicit env var wins — but only if it actually points at an engine
  //    root (otherwise fall through to discovery rather than returning a path
  //    that has no Build.bat).
  if (env.NGG_UE_INSTALL_DIR &&
      existsSyncFn(path.join(env.NGG_UE_INSTALL_DIR, "Engine", "Build", "BatchFiles", "Build.bat"))) {
    return env.NGG_UE_INSTALL_DIR;
  }

  // 2. Windows registry — custom engine builds registered under HKCU.
  if (safeAssociation) {
    try {
      const regOut = execSyncFn(
        `reg query "HKCU\\Software\\Epic Games\\Unreal Engine\\Builds" /v "${safeAssociation}" 2>nul`,
        { encoding: "utf8", timeout: 5000 }
      );
      // Anchor to the queried value name so we can't accidentally grab the
      // REG_SZ value of a different registered build. `reg query` prints each
      // value as: `    <ValueName>    REG_SZ    <data>`.
      const escapedName = safeAssociation.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
      const match = regOut.match(new RegExp(`^\\s*${escapedName}\\s+REG_SZ\\s+(.+)$`, "m"));
      if (match) {
        const regPath = match[1].trim();
        if (existsSyncFn(path.join(regPath, "Engine", "Build", "BatchFiles", "Build.bat"))) {
          return regPath;
        }
      }
    } catch { /* not registered — fall through */ }
  }

  // 3. HKLM — where the Epic Games Launcher records installed engine versions.
  //    This is the common case: a launcher-only machine has no HKCU\Builds key.
  if (safeAssociation) {
    const hklmDir = readHklmInstallDir(safeAssociation, execSyncFn, existsSyncFn);
    if (hklmDir) return hklmDir;
  }

  // 4. The launcher's own manifest — covers a missing or stale registry.
  if (safeAssociation) {
    const manifestDir = readLauncherManifest(
      safeAssociation, existsSyncFn, readFileSyncFn, env
    );
    if (manifestDir) return manifestDir;
  }

  // 5. Last resort: scan well-known install locations on every drive letter.
  //    Only finds engines that happen to sit in a conventional directory —
  //    D:\Epic\UE_5.8 would be missed, which is why the steps above exist.
  const drives = [];
  for (let c = 67; c <= 90; c++) {
    const d = String.fromCharCode(c) + ":\\";
    try { if (existsSyncFn(d)) drives.push(d); } catch {}
  }

  const candidates = [];
  for (const drive of drives) {
    if (engineAssociation) {
      candidates.push(path.join(drive, `UE_${engineAssociation}`));
      candidates.push(path.join(drive, `UE${engineAssociation}`));
      candidates.push(path.join(drive, `UnrealEngine-${engineAssociation}`));
    }
    candidates.push(path.join(drive, "Program Files", "Epic Games", `UE_${engineAssociation}`));
    candidates.push(path.join(drive, "Epic Games", `UE_${engineAssociation}`));
  }

  for (const candidate of candidates) {
    if (existsSyncFn(path.join(candidate, "Engine", "Build", "BatchFiles", "Build.bat"))) {
      return candidate;
    }
  }

  throw new Error(
    `Unreal Engine install not found (association=${engineAssociation ?? "unknown"}). ` +
    `Set NGG_UE_INSTALL_DIR to the engine root.`
  );
}

/**
 * Discover build target names from Source/*.Target.cs files.
 * Returns { editor, game, server?, client? }.
 *
 * @param {string} uprojectPath
 * @param {object} [opts]
 * @param {typeof fs.readdirSync} [opts.readdirSync] - injectable for tests
 * @returns {{editor:string, game:string, server?:string, client?:string}}
 */
export function discoverBuildTargets(uprojectPath, opts = {}) {
  const readdirSyncFn = opts.readdirSync ?? fs.readdirSync;
  const projectStem = path.basename(uprojectPath, path.extname(uprojectPath));
  const sourceDir = path.join(path.dirname(uprojectPath), "Source");
  const defaults = { editor: `${projectStem}Editor`, game: projectStem };
  try {
    const files = readdirSyncFn(sourceDir).filter(f => f.endsWith(".Target.cs"));
    const targets = {};
    for (const f of files) {
      const stem = f.replace(".Target.cs", "");
      if      (stem.toLowerCase().endsWith("editor")) targets.editor = stem;
      else if (stem.toLowerCase().endsWith("server")) targets.server = stem;
      else if (stem.toLowerCase().endsWith("client")) targets.client = stem;
      else                                            targets.game   = stem;
    }
    return { ...defaults, ...targets };
  } catch { return defaults; }
}

/**
 * Derive the project's display name (the stem of the .uproject file).
 * @param {string} uprojectPath
 * @returns {string}
 */
export function projectNameFrom(uprojectPath) {
  return path.basename(uprojectPath, path.extname(uprojectPath));
}
