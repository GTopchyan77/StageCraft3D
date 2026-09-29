// Copyright 2025-2026 NGG. All Rights Reserved.
// profile.js — unrealngg-mcp
// Unreal Insights profiling: capture (Trace.* console commands via execPython)
// + headless analysis (UnrealInsights.exe -OpenTraceFile + ExportTimerStatistics)
// + Python TraceServices fallback.
//
// Trace command surface verified against UE 5.7 source
// (Engine/Source/Runtime/Core/Private/ProfilingDebugging/TraceAuxiliary.cpp):
//   Trace.File [Path] [ChannelSet]   — start tracing to file (Trace.Start is deprecated)
//   Trace.Stop                       — stop tracing
//   Trace.Pause / Trace.Resume       — pause/resume channels
//   Trace.Bookmark [Name]            — emit a TRACE_BOOKMARK event
//   Trace.RegionBegin / RegionEnd    — emit named region boundaries (UE 5.3+)
//   Trace.Status                     — print status / channel list
//   Trace.SnapshotFile [Path]        — write current in-memory buffer to a file
//
// Headless analysis CLI (verified against
// Engine/Source/Developer/TraceInsights/Private/Insights/Tests/FunctionalTests/ExportCommandsTests.cpp):
//   UnrealInsights.exe -OpenTraceFile="<utrace>" -ABSLOG="<log>" -AutoQuit -NoUI
//                      -ExecOnAnalysisCompleteCmd="<task>" -log
//
// Export tasks (TimingProfilerManager.cpp):
//   TimingInsights.ExportTimers <csv>            — list of timers (id, name, ...)
//   TimingInsights.ExportTimerStatistics <csv> [-sortBy=TotalInclusiveTime]
//                                                [-sortOrder=Descending|Ascending]
//                                                [-timers=<glob>]   — wildcard supported
//                                                [-threads=<glob>]
//                                                [-region=<glob>]
//                                                [-startTime=N -endTime=N]
//                                                [-maxTimerCount=N]
//                                                [-columns=...]
//   TimingInsights.ExportThreads <csv>
//   TimingInsights.ExportTimingEvents <csv> [filters...]
//
// Authority: capture commands run in-editor via /editor/exec_python (already on the
// bridge). Analysis happens out-of-process so it works on saved .utrace files
// even if the editor is closed.

import { spawn } from "child_process";
import fs from "fs";
import os from "os";
import path from "path";
import * as ue5 from "./ue5client.js";

// ---------------------------------------------------------------------------
// Defaults
// ---------------------------------------------------------------------------

// Lean default: covers >90% of "what's slow?" investigations.
// Add memalloc/loadtime/task/net only on demand — they bloat trace files fast.
export const DEFAULT_CHANNELS = "cpu,gpu,frame,bookmark,log";

// UE's Trace.File parser tokenizes by whitespace and treats the resulting
// string as a path RELATIVE to engine working dir if it isn't a valid
// absolute path (it does NOT honor wrapping quotes — the leading `"` becomes
// part of the path, which then resolves under <EngineDir>/Engine/Binaries/Win64/).
// So the destination must be:
//   - absolute, with NO surrounding quotes
//   - free of spaces (otherwise the parser splits on them)
//
// We therefore default to %LOCALAPPDATA%\UnrealEngine\Common\UnrealTrace\Store\
// — which is also where UnrealTraceServer puts traces from network-mode
// connections, so Insights' Session Browser picks them up automatically.
export const DEFAULT_TRACE_SUBDIR = path.join("Saved", "Profiling", "UnrealInsights");

function defaultStoreDir() {
  const localAppData = process.env.LOCALAPPDATA;
  if (!localAppData) {
    throw new Error("LOCALAPPDATA env var is not set — cannot resolve UnrealTrace store dir");
  }
  return path.join(localAppData, "UnrealEngine", "Common", "UnrealTrace", "Store");
}

// Default top-N when ranking timers.
export const DEFAULT_TOP_N = 30;

// Built-in symbol filters that the user can pick by name. Applied in JS after
// the CSV is read, NOT via Insights' -timers= flag — that flag only filters
// per-event exports (ExportTimingEvents), not per-timer aggregates
// (ExportTimerStatistics). Patterns are case-insensitive substrings; the
// user can override with `timersFilter` for explicit regex.
export const TIMER_FILTERS = {
  collision:  ["Collision", "Sweep", "Overlap", "LineTrace", "PrimitiveComponent", "BodyInstance"],
  physics:    ["Physics", "Chaos", "PxScene", "BodyInstance", "TickPhysics", "Solver"],
  rendering:  ["RenderThread", "BasePass", "Shadow", "PostProcess", "Slate", "RHIThread", "RenderGraph"],
  gameplay:   ["Tick", "Actor", "ProcessEvent", "Blueprint", "BehaviorTree", " AI "],
  loading:    ["Load", "Stream", "Package", "AsyncLoad", "Cook"],
  ui:         ["Slate", "UMG", "Widget", "Paint"],
};

function buildNameMatcher(modeOrPatterns, override) {
  if (override) {
    // Treat override as a regex if it looks like one, else as a comma-separated
    // list of substrings.
    if (/[()|\\^$+]/.test(override)) {
      try { return new RegExp(override, "i"); } catch (_) { /* fall through */ }
    }
    const parts = override.split(",").map((s) => s.trim()).filter(Boolean);
    if (parts.length === 0) return null;
    return new RegExp(parts.map(escapeRegex).join("|"), "i");
  }
  const patterns = TIMER_FILTERS[modeOrPatterns];
  if (!patterns) return null;
  return new RegExp(patterns.map(escapeRegex).join("|"), "i");
}

function escapeRegex(s) {
  return s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}

// ---------------------------------------------------------------------------
// Caller-value sanitizers
// ---------------------------------------------------------------------------
// Several caller-supplied strings are interpolated raw into Unreal Insights
// task / console command strings (passed on the command line and re-tokenized
// by UE's FParse). Anything with quotes, semicolons, newlines, backticks or $
// could smuggle extra command tokens, so each value is restricted to a tight
// charset up-front — same approach as the `channels` check in traceStart().
//
// region/bookmark name — letters, digits, _ . - and spaces. Region/bookmark
// names can be human-readable, so spaces are allowed; everything that could
// break out of the token is rejected.
function sanitizeRegion(region, label = "region") {
  if (region === undefined || region === null || region === "") return region;
  if (!/^[A-Za-z0-9_.\- ]+$/.test(region)) {
    throw new Error(
      `Invalid ${label} "${region}": only letters, digits, spaces and _ . - are allowed ` +
      `(no quotes, semicolons, newlines, backticks or $).`
    );
  }
  return region;
}

// threadsFilter — comma-separated thread names/ids. Letters, digits, _ - ,
// and spaces; no glob metacharacters that could break the token.
function sanitizeThreadsFilter(threadsFilter) {
  if (threadsFilter === undefined || threadsFilter === null || threadsFilter === "") return threadsFilter;
  if (!/^[A-Za-z0-9_,\- ]+$/.test(threadsFilter)) {
    throw new Error(
      `Invalid threadsFilter "${threadsFilter}": only letters, digits, spaces, commas and _ - are allowed ` +
      `(no quotes, semicolons, newlines, backticks or $).`
    );
  }
  return threadsFilter;
}

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------

/**
 * Build the per-project profiling directory and ensure it exists.
 * @param {string} uprojectPath absolute path to .uproject
 * @returns {string} absolute path to <Project>/Saved/Profiling/UnrealInsights
 */
export function profilingDir(uprojectPath) {
  const projectRoot = path.dirname(uprojectPath);
  const dir = path.join(projectRoot, DEFAULT_TRACE_SUBDIR);
  fs.mkdirSync(dir, { recursive: true });
  return dir;
}

function defaultTraceFileName() {
  const d = new Date();
  const pad = (n) => String(n).padStart(2, "0");
  return `trace_${d.getFullYear()}${pad(d.getMonth()+1)}${pad(d.getDate())}_${pad(d.getHours())}${pad(d.getMinutes())}${pad(d.getSeconds())}.utrace`;
}

function unrealInsightsExe(engineDir) {
  return path.join(engineDir, "Engine", "Binaries", "Win64", "UnrealInsights.exe");
}

// ---------------------------------------------------------------------------
// Python helper — every Trace.* console command goes through this
// ---------------------------------------------------------------------------

/**
 * Build a Python snippet that runs a console command in-editor.
 * Tries multiple ways to obtain a world so it works in PIE and editor-only.
 * @param {string} consoleCmd the full console command, e.g. 'Trace.File "C:/x.utrace" cpu,gpu'
 */
function pyExecConsole(consoleCmd) {
  // Triple-double-quoted string is escaped by JSON.stringify when it crosses
  // the bridge — execPython sends `command` as JSON, so the inner Python code
  // is a normal string here.
  return [
    "import unreal",
    `_cmd = ${JSON.stringify(consoleCmd)}`,
    "_world = None",
    "try:",
    "    _world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()",
    "except Exception:",
    "    pass",
    "if _world is None:",
    "    try:",
    "        _world = unreal.EditorLevelLibrary.get_editor_world()",
    "    except Exception:",
    "        pass",
    "if _world is None:",
    "    raise RuntimeError('No editor world available — open a level first')",
    "unreal.SystemLibrary.execute_console_command(_world, _cmd)",
    "print('OK:', _cmd)",
  ].join("\n");
}

async function runConsoleCommand(consoleCmd) {
  const py = pyExecConsole(consoleCmd);
  const res = await ue5.execPython(py, "execute_file");
  if (!res?.success) {
    const log = (res?.log ?? []).map((l) => `[${l.type}] ${l.output}`).join("\n");
    throw new Error(`Console command failed: ${consoleCmd}\n${log || res?.result || "(no output)"}`);
  }
  return res;
}

// ---------------------------------------------------------------------------
// Capture API — thin wrappers over Trace.* commands
// ---------------------------------------------------------------------------

/**
 * Start a trace.
 *
 * The trace is written to %LOCALAPPDATA%\UnrealEngine\Common\UnrealTrace\Store\
 * by default — that path is space-free on standard Windows installs and is
 * also where UnrealTraceServer puts traces, so they show up in Insights'
 * Session Browser. The project's Saved/Profiling/ directory is NOT used as
 * the default because typical UE project paths contain spaces (e.g.
 * "Unreal Projects"), and `Trace.File` does not respect quoted paths —
 * see DEFAULT_TRACE_SUBDIR comment.
 *
 * @param {object} opts
 * @param {string} [opts.channels]    comma-separated channel set (default DEFAULT_CHANNELS)
 * @param {string} [opts.fileName]    just the basename, e.g. "tick.utrace"
 * @param {string} opts.uprojectPath  absolute project path (kept for symmetry; not used for the
 *                                    write path unless an env override is added later)
 * @returns {Promise<{file: string, channels: string}>}
 */
export async function traceStart({ channels = DEFAULT_CHANNELS, fileName, uprojectPath }) {
  if (!uprojectPath) throw new Error("uprojectPath is required");
  // channels is interpolated raw into a `Trace.File <path> <channels>` console
  // command — restrict it to a comma-separated channel list so nothing can be
  // smuggled into the command string.
  if (!/^[a-z0-9,]+$/i.test(channels)) {
    throw new Error(`Invalid trace channels "${channels}": expected a comma-separated list of channel names (letters/digits only).`);
  }
  const dir = defaultStoreDir();
  fs.mkdirSync(dir, { recursive: true });
  const file = path.join(dir, fileName ?? defaultTraceFileName());
  if (file.includes(" ")) {
    throw new Error(
      `Trace path contains spaces, which UE's Trace.File parser rejects: ${file}. ` +
      `Either rename your Windows user profile to remove spaces, or set fileName to avoid them.`
    );
  }
  // Path must be absolute, forward-slashed, and UNQUOTED — UE's parser includes
  // any wrapping quotes as part of the path string and then resolves it
  // relative to the engine working directory.
  const filePosix = file.replace(/\\/g, "/");
  await runConsoleCommand(`Trace.File ${filePosix} ${channels}`);
  return { file, channels };
}

/** Stop the active trace. */
export async function traceStop() {
  await runConsoleCommand("Trace.Stop");
  return { stopped: true };
}

export async function tracePause()  { await runConsoleCommand("Trace.Pause");  return { paused:  true }; }
export async function traceResume() { await runConsoleCommand("Trace.Resume"); return { resumed: true }; }

export async function traceBookmark(name) {
  if (!name) throw new Error("bookmark name is required");
  // name is interpolated raw into a Trace.Bookmark console command — sanitize
  // it like the other caller-controlled region/bookmark values.
  sanitizeRegion(name, "bookmark name");
  await runConsoleCommand(`Trace.Bookmark ${name}`);
  return { bookmark: name };
}

export async function traceRegionBegin(name) {
  if (!name) throw new Error("region name is required");
  sanitizeRegion(name, "region name");
  await runConsoleCommand(`Trace.RegionBegin ${name}`);
  return { region: name, state: "begin" };
}

export async function traceRegionEnd(name) {
  if (!name) throw new Error("region name is required");
  sanitizeRegion(name, "region name");
  await runConsoleCommand(`Trace.RegionEnd ${name}`);
  return { region: name, state: "end" };
}

/** Return Trace.Status output as captured Python log lines. */
export async function traceStatus() {
  const res = await ue5.execPython(pyExecConsole("Trace.Status"), "execute_file");
  return {
    success: !!res?.success,
    log: (res?.log ?? []).map((l) => `[${l.type}] ${l.output}`),
  };
}

// ---------------------------------------------------------------------------
// Trace listing — pure Node, works regardless of editor state
// ---------------------------------------------------------------------------

// Locations to scan for .utrace files. The editor often forwards traces to
// UnrealTraceServer's store rather than the path passed to Trace.File when the
// store process is running, so we always check both.
function utraceSearchRoots(uprojectPath) {
  const roots = [profilingDir(uprojectPath)];
  const localAppData = process.env.LOCALAPPDATA;
  if (localAppData) {
    roots.push(path.join(localAppData, "UnrealEngine", "Common", "UnrealTrace", "Store"));
  }
  return roots.filter((r) => fs.existsSync(r));
}

function scanUtraceTree(root) {
  const out = [];
  const stack = [root];
  while (stack.length > 0) {
    const cur = stack.pop();
    let entries;
    try { entries = fs.readdirSync(cur, { withFileTypes: true }); } catch (_) { continue; }
    for (const e of entries) {
      const full = path.join(cur, e.name);
      if (e.isDirectory()) { stack.push(full); continue; }
      if (e.isFile() && e.name.toLowerCase().endsWith(".utrace")) {
        try {
          const st = fs.statSync(full);
          out.push({ name: e.name, path: full, size_bytes: st.size, mtime: st.mtime.toISOString() });
        } catch (_) { /* skip files we can't stat */ }
      }
    }
  }
  return out;
}

export function listTraces(uprojectPath) {
  const roots = utraceSearchRoots(uprojectPath);
  const all = roots.flatMap(scanUtraceTree).sort((a, b) => b.mtime.localeCompare(a.mtime));
  return { roots, traces: all };
}

// ---------------------------------------------------------------------------
// Headless analysis via UnrealInsights.exe
// ---------------------------------------------------------------------------

function spawnInsightsHeadless({ exe, traceFile, execTask, logFile, timeoutMs }) {
  return new Promise((resolve, reject) => {
    // UE parses its command line via FParse::Value(Cmd, "Foo=", Value) which
    // reads the literal command-line string (GetCommandLineW), not argv. The
    // canonical form Epic uses (ExportCommandsTests.cpp:117) is:
    //   -OpenTraceFile="<path>" -ABSLOG="<log>" -AutoQuit -NoUI
    //   -ExecOnAnalysisCompleteCmd="<task>" -log
    // We need windowsVerbatimArguments so Node won't re-escape our quotes.
    const args = [
      `-OpenTraceFile="${traceFile}"`,
      `-ABSLOG="${logFile}"`,
      "-AutoQuit",
      "-NoUI",
      `-ExecOnAnalysisCompleteCmd="${execTask}"`,
      "-log",
    ];
    const child = spawn(exe, args, {
      windowsHide: true,
      windowsVerbatimArguments: true,
      stdio: ["ignore", "pipe", "pipe"],
    });
    // Retain only a bounded tail of each stream — a long Insights run can emit
    // megabytes of -log output we'd otherwise accumulate in full.
    const MAX_BUFFER = 256 * 1024;
    let stdout = "";
    let stderr = "";
    const appendBounded = (cur, chunk) => {
      cur += chunk;
      return cur.length > MAX_BUFFER ? cur.slice(cur.length - MAX_BUFFER) : cur;
    };
    child.stdout?.on("data", (b) => { stdout = appendBounded(stdout, b.toString()); });
    child.stderr?.on("data", (b) => { stderr = appendBounded(stderr, b.toString()); });

    const killTimer = setTimeout(() => {
      try { child.kill("SIGKILL"); } catch (_) { /* ignore */ }
      reject(new Error(`UnrealInsights.exe timed out after ${timeoutMs}ms`));
    }, timeoutMs);

    child.on("error", (err) => {
      clearTimeout(killTimer);
      reject(new Error(`Failed to spawn UnrealInsights.exe: ${err.message}`));
    });
    child.on("exit", (code) => {
      clearTimeout(killTimer);
      // UnrealInsights / commandlets routinely exit non-zero even after a
      // successful export, so a non-zero code is NOT treated as failure here.
      // The CSV-existence check in analyze() is the real success signal; we
      // just surface the exit code + captured stderr for diagnostics.
      resolve({ stdout, stderr, code });
    });
  });
}

/**
 * Parse the CSV produced by TimingInsights.ExportTimerStatistics.
 * Header is well-defined; values are simple comma-separated numbers / quoted strings.
 * Resilient to extra columns or quoted commas.
 */
export function parseTimerStatisticsCsv(content) {
  const lines = content.split(/\r?\n/).filter((l) => l.trim().length > 0);
  if (lines.length === 0) return { header: [], rows: [] };
  const header = parseCsvLine(lines[0]);
  const rows = [];
  for (let i = 1; i < lines.length; i++) {
    const cells = parseCsvLine(lines[i]);
    if (cells.length !== header.length) continue;
    const row = {};
    for (let j = 0; j < header.length; j++) {
      const v = cells[j];
      // Numeric columns (anything that looks like a number) get coerced.
      const n = Number(v);
      row[header[j]] = (v !== "" && !Number.isNaN(n)) ? n : v;
    }
    rows.push(row);
  }
  return { header, rows };
}

function parseCsvLine(line) {
  const out = [];
  let buf = "";
  let inQ = false;
  for (let i = 0; i < line.length; i++) {
    const c = line[i];
    if (inQ) {
      if (c === '"' && line[i + 1] === '"') { buf += '"'; i++; }
      else if (c === '"') { inQ = false; }
      else { buf += c; }
    } else {
      if (c === '"') { inQ = true; }
      else if (c === ',') { out.push(buf); buf = ""; }
      else { buf += c; }
    }
  }
  out.push(buf);
  return out.map((s) => s.trim());
}

/**
 * Run UnrealInsights headless on a .utrace and return ranked timer statistics.
 *
 * @param {object} opts
 * @param {string} opts.utracePath      absolute path to .utrace
 * @param {string} opts.engineDir       absolute path to engine root (e.g. D:\UE_5.7)
 * @param {string} [opts.mode]          "top_functions" | "collision" | "physics" | "rendering" | "gameplay" | "loading" | "ui" | "raw"
 * @param {number} [opts.topN]          rows to keep after sort (default 30)
 * @param {string} [opts.timersFilter]  override -timers= glob (overrides mode preset)
 * @param {string} [opts.threadsFilter] -threads= glob (e.g. "GameThread", "Render*")
 * @param {string} [opts.region]        -region= glob
 * @param {number} [opts.startTime]
 * @param {number} [opts.endTime]
 * @param {number} [opts.timeoutMs]     default 5 minutes
 * @returns {Promise<object>}
 */
export async function analyze(opts) {
  const {
    utracePath,
    engineDir,
    mode = "top_functions",
    topN = DEFAULT_TOP_N,
    timersFilter,
    threadsFilter,
    region,
    startTime,
    endTime,
    timeoutMs = 300000,
  } = opts;

  if (!utracePath) throw new Error("utracePath is required");
  if (!engineDir)  throw new Error("engineDir is required");
  if (!fs.existsSync(utracePath)) throw new Error(`utrace not found: ${utracePath}`);

  const exe = unrealInsightsExe(engineDir);
  if (!fs.existsSync(exe)) {
    throw new Error(`UnrealInsights.exe not found at ${exe}. Engine path may be wrong.`);
  }

  // Write output to a no-space temp dir, NOT next to the .utrace. UE's
  // FParse::Token re-splits the inside of -ExecOnAnalysisCompleteCmd="..." on
  // whitespace, so a path containing spaces (e.g. "Unreal Projects") breaks
  // the file argument even with quoting. os.tmpdir() respects TMP/TEMP env
  // vars on Windows and is space-free on a sane install — fails loudly via
  // the assertion below if it isn't.
  const stamp = new Date().toISOString().replace(/[:.]/g, "-");
  const outDir = path.join(os.tmpdir(), `ue5ngg_insights_${stamp}`);
  fs.mkdirSync(outDir, { recursive: true });
  const csvPath = path.join(outDir, "timer_stats.csv");
  const logPath = path.join(outDir, "insights.log");
  if (csvPath.includes(" ") || logPath.includes(" ")) {
    throw new Error(`Output path contains spaces, which breaks UnrealInsights CLI parsing: ${csvPath}`);
  }

  // Pass only flags Insights' ExportTimerStatistics actually honors:
  //   -sortBy / -sortOrder / -threads / -region / -startTime / -endTime.
  // -timers= is documented but only takes effect on ExportTimingEvents, not
  // on the aggregate ExportTimerStatistics — verified by testing on UE 5.7.
  // We export everything (no -maxTimerCount) and filter+truncate in JS.
  // threadsFilter and region are interpolated raw into the export task string
  // (re-tokenized by UE's FParse), so sanitize them before use — same guard the
  // `channels` value gets in traceStart().
  const safeThreadsFilter = sanitizeThreadsFilter(threadsFilter);
  const safeRegion = sanitizeRegion(region);

  const flags = [];
  flags.push(`-sortBy=TotalInclusiveTime`);
  flags.push(`-sortOrder=Descending`);
  if (safeThreadsFilter) flags.push(`-threads=${safeThreadsFilter}`);
  if (safeRegion)        flags.push(`-region=${safeRegion}`);
  if (Number.isFinite(startTime)) flags.push(`-startTime=${startTime}`);
  if (Number.isFinite(endTime))   flags.push(`-endTime=${endTime}`);

  // The CSV path is space-free (we just enforced that), so we don't need to
  // wrap it in inner quotes — which would conflict with the outer quotes
  // around the whole task in the verbatim command line.
  const task = `TimingInsights.ExportTimerStatistics ${csvPath} ${flags.join(" ")}`;

  await spawnInsightsHeadless({
    exe,
    traceFile: utracePath,
    execTask: task,
    logFile: logPath,
    timeoutMs,
  });

  if (!fs.existsSync(csvPath)) {
    throw new Error(`Insights ran but produced no CSV at ${csvPath}. See ${logPath}.`);
  }

  const csv = fs.readFileSync(csvPath, "utf8");
  const { header, rows } = parseTimerStatisticsCsv(csv);

  // Filter rows by name in JS (Insights' -timers= is ignored for this export).
  const matcher = buildNameMatcher(mode, timersFilter);
  const filtered = matcher ? rows.filter((r) => matcher.test(String(r.Name ?? ""))) : rows;

  return {
    utrace: utracePath,
    mode,
    csv_path: csvPath,
    log_path: logPath,
    out_dir: outDir,
    timers_filter: matcher ? matcher.source : "(none)",
    threads_filter: threadsFilter ?? "*",
    region: region ?? null,
    columns: header,
    total_rows: rows.length,
    matched_rows: filtered.length,
    top: filtered.slice(0, topN),
  };
}

// ---------------------------------------------------------------------------
// Python TraceServices fallback
// ---------------------------------------------------------------------------
// When the headless CLI fails (older engine, missing binary, container env),
// drive analysis from inside the running editor via the Python TraceServices
// bindings. Less reliable across versions but works on any open editor.

/**
 * Run analysis via in-editor Python. Uses unreal.TraceServices if exposed;
 * otherwise falls back to spawning a child UnrealEditor-Cmd that runs the
 * same export tasks via -ExecCmds.
 *
 * NOTE: as of UE 5.7, the Python TraceServices surface is small and not
 * documented. This function shells out the Insights CLI from inside Python
 * (so capture and analysis can happen on the same machine the editor owns)
 * — effectively the same as analyze() but driven from the editor, useful
 * when the Node process can't reach the engine paths directly.
 */
export async function analyzeViaPython(opts) {
  const { utracePath, engineDir, mode = "top_functions", topN = DEFAULT_TOP_N } = opts;
  if (!utracePath) throw new Error("utracePath is required");
  if (!engineDir)  throw new Error("engineDir is required");

  const exe = unrealInsightsExe(engineDir).replace(/\\/g, "/");
  const utrace = utracePath.replace(/\\/g, "/");
  const outDir = path.join(path.dirname(utracePath), `_analyze_py_${Date.now()}`).replace(/\\/g, "/");
  const csv = `${outDir}/timer_stats.csv`;
  const log = `${outDir}/insights.log`;

  // Same constraint analyze() enforces: UE's FParse re-splits the inside of
  // -ExecOnAnalysisCompleteCmd="..." on whitespace, so a space in the output
  // path breaks the file argument even when quoted.
  if (csv.includes(" ") || log.includes(" ")) {
    throw new Error(`Output path contains spaces, which breaks UnrealInsights CLI parsing: ${csv}`);
  }

  // TIMER_FILTERS entries are arrays of substrings; -timers= expects a single
  // comma-joined glob, so join rather than interpolating the array raw.
  const presetTimers = mode in TIMER_FILTERS ? TIMER_FILTERS[mode].join(",") : "*";
  const task = `TimingInsights.ExportTimerStatistics "${csv}" -sortBy=TotalInclusiveTime -sortOrder=Descending -maxTimerCount=${topN} -timers=${presetTimers}`;

  const py = [
    "import unreal, subprocess, os, json",
    `_out = ${JSON.stringify(outDir)}`,
    "os.makedirs(_out, exist_ok=True)",
    `_args = [${JSON.stringify(exe)},`,
    `         '-OpenTraceFile=' + ${JSON.stringify(utrace)},`,
    `         '-ABSLOG=' + ${JSON.stringify(log)},`,
    "         '-AutoQuit', '-NoUI',",
    `         '-ExecOnAnalysisCompleteCmd=' + ${JSON.stringify(task)},`,
    "         '-log']",
    "_r = subprocess.run(_args, capture_output=True, text=True, timeout=300)",
    `print('INSIGHTS_EXIT', _r.returncode)`,
    `print('CSV', ${JSON.stringify(csv)})`,
    `print('LOG', ${JSON.stringify(log)})`,
  ].join("\n");

  const pyRes = await ue5.execPython(py, "execute_file");
  if (!pyRes?.success) {
    throw new Error("Python fallback failed: " + (pyRes?.result ?? "(no message)"));
  }
  if (!fs.existsSync(csv)) {
    throw new Error(`Python fallback ran but produced no CSV at ${csv}.`);
  }
  const { header, rows } = parseTimerStatisticsCsv(fs.readFileSync(csv, "utf8"));
  return {
    utrace: utracePath,
    mode,
    csv_path: csv,
    log_path: log,
    out_dir: outDir,
    columns: header,
    row_count: rows.length,
    top: rows.slice(0, topN),
    via: "python",
  };
}
