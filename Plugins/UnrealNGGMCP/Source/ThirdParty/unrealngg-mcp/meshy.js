// Copyright 2025-2026 NGG. All Rights Reserved.
// meshy.js — unrealngg-mcp
// Meshy.ai v2 text-to-3D REST flow: submit preview, poll, (optionally) refine,
// and download the generated mesh file. Uses built-in fetch (Node 18+).

import fs from "fs";
import path from "path";

const MESHY_BASE = "https://api.meshy.ai/openapi/v2/text-to-3d";

const POLL_INTERVAL_MS = 10_000;
const MAX_WAIT_MS      = 15 * 60 * 1000; // 15 min per phase
const REQUEST_TIMEOUT_MS = 60_000;       // per-fetch abort guard
const MAX_UNKNOWN_STATUSES = 5;          // fail fast on repeated unrecognized poll statuses

// Strip any Meshy API key (msy-...) that may have leaked into a response body
// before it ends up in a thrown error message.
function scrubMeshyKeys(text) {
  return String(text).replace(/msy-[A-Za-z0-9_-]+/g, "msy-***");
}

/**
 * Resolve the Meshy API key from env var first, then optional config file.
 * Returns { key, source } or throws an Error whose message explains both options.
 * @param {string} projectRoot - Absolute path to the project root (for .meshy.json lookup).
 */
export function resolveApiKey(projectRoot) {
  if (process.env.MESHY_API_KEY && process.env.MESHY_API_KEY.trim()) {
    return { key: process.env.MESHY_API_KEY.trim(), source: "env:MESHY_API_KEY" };
  }
  const configPath = path.join(projectRoot, ".meshy.json");
  if (fs.existsSync(configPath)) {
    try {
      const cfg = JSON.parse(fs.readFileSync(configPath, "utf8"));
      if (cfg && typeof cfg.api_key === "string" && cfg.api_key.trim()) {
        return { key: cfg.api_key.trim(), source: `file:${configPath}` };
      }
    } catch (err) {
      throw new Error(`Failed to read ${configPath}: ${err.message}`);
    }
  }
  throw new Error(
    "Meshy API key not found. Provide it via one of:\n" +
    "  1) Set environment variable MESHY_API_KEY=msy-...\n" +
    `  2) Create ${configPath} with content: {\"api_key\":\"msy-...\"}\n` +
    "Get a key from https://www.meshy.ai/settings/api"
  );
}

async function meshyFetch(url, apiKey, opts = {}) {
  const res = await fetch(url, {
    ...opts,
    signal: opts.signal ?? AbortSignal.timeout(REQUEST_TIMEOUT_MS),
    headers: {
      "Authorization": `Bearer ${apiKey}`,
      "Content-Type":  "application/json",
      "Accept":        "application/json",
      ...(opts.headers ?? {}),
    },
  });
  if (!res.ok) {
    const text = await res.text().catch(() => "");
    throw new Error(scrubMeshyKeys(`Meshy API ${res.status} ${res.statusText}: ${text.slice(0, 500)}`));
  }
  return res.json();
}

/**
 * Submit a preview or refine task. Returns the task id.
 * @param {string} apiKey
 * @param {object} body - POST body (mode, prompt, etc.)
 */
export async function submitTask(apiKey, body) {
  const data = await meshyFetch(MESHY_BASE, apiKey, {
    method: "POST",
    body: JSON.stringify(body),
  });
  // v2: response is { result: <task_id> }
  const id = data.result ?? data.task_id ?? data.id;
  if (!id || typeof id !== "string") {
    throw new Error(`Meshy submit response missing task id: ${JSON.stringify(data)}`);
  }
  return id;
}

/**
 * Poll a task id every POLL_INTERVAL_MS until status is SUCCEEDED.
 * Throws on FAILED/EXPIRED or when MAX_WAIT_MS is exceeded.
 * Returns the final task object (which includes model_urls).
 */
export async function pollUntilDone(apiKey, taskId, { onTick } = {}) {
  const start = Date.now();
  const KNOWN_PENDING = new Set(["PENDING", "IN_PROGRESS", "QUEUED", "STARTED", ""]);
  let unknownCount = 0;
  while (true) {
    const task = await meshyFetch(`${MESHY_BASE}/${encodeURIComponent(taskId)}`, apiKey);
    const status = (task.status ?? "").toUpperCase();
    if (typeof onTick === "function") onTick(status, task.progress ?? null);
    if (status === "SUCCEEDED") return task;
    if (status === "FAILED" || status === "EXPIRED" || status === "CANCELED") {
      throw new Error(scrubMeshyKeys(`Meshy task ${taskId} ended with status=${status}: ${task.task_error?.message ?? "(no error message)"}`));
    }
    // An unrecognized status would otherwise spin until the 15-min cap. Track
    // consecutive unknowns and bail fast once they exceed the threshold.
    if (KNOWN_PENDING.has(status)) {
      unknownCount = 0;
    } else if (++unknownCount >= MAX_UNKNOWN_STATUSES) {
      throw new Error(`Meshy task ${taskId} returned unrecognized status="${status}" ${unknownCount} times in a row; aborting.`);
    }
    if (Date.now() - start > MAX_WAIT_MS) {
      throw new Error(`Meshy task ${taskId} timed out after ${MAX_WAIT_MS / 1000}s (last status=${status})`);
    }
    await new Promise(r => setTimeout(r, POLL_INTERVAL_MS));
  }
}

/**
 * Download the best-available model file from the task's model_urls.
 * Tries fbx → glb → obj. Writes binary to destFile, creating parent dirs.
 * Returns { ext, bytes }.
 */
export async function downloadModel(task, destFileNoExt) {
  const urls = task.model_urls ?? {};
  const preferred = ["fbx", "glb", "obj"];
  let pickedExt = null;
  let pickedUrl = null;
  for (const ext of preferred) {
    if (urls[ext] && typeof urls[ext] === "string") {
      pickedExt = ext;
      pickedUrl = urls[ext];
      break;
    }
  }
  if (!pickedExt) {
    throw new Error(`Meshy task returned no fbx/glb/obj URL. Available: ${JSON.stringify(Object.keys(urls))}`);
  }
  const res = await fetch(pickedUrl, { signal: AbortSignal.timeout(REQUEST_TIMEOUT_MS) });
  if (!res.ok) {
    throw new Error(scrubMeshyKeys(`Failed to download Meshy model (${pickedExt}): ${res.status} ${res.statusText}`));
  }
  const buf = Buffer.from(await res.arrayBuffer());
  const destFile = `${destFileNoExt}.${pickedExt}`;
  fs.mkdirSync(path.dirname(destFile), { recursive: true });
  fs.writeFileSync(destFile, buf);
  return { ext: pickedExt, bytes: buf.length, file: destFile };
}

/**
 * High-level orchestration.
 *
 * @param {object} opts
 * @param {string} opts.apiKey
 * @param {string} opts.prompt
 * @param {"realistic"|"sculpture"} [opts.art_style]
 * @param {string} [opts.negative_prompt]
 * @param {boolean} [opts.refine]        - If true, run refine phase after preview.
 * @param {string}  opts.destFileNoExt   - Absolute path (no extension) to write the model.
 * @param {(phase:string, status:string)=>void} [opts.onStatus]
 * @returns {Promise<{preview_task_id, refine_task_id?, final_task_id, ext, bytes, file}>}
 */
export async function generateTextTo3D(opts) {
  const {
    apiKey, prompt,
    art_style = "realistic",
    negative_prompt,
    refine = false,
    destFileNoExt,
    onStatus,
  } = opts;

  const previewBody = {
    mode: "preview",
    prompt,
    art_style,
    ai_model: "meshy-6",
    ...(negative_prompt ? { negative_prompt } : {}),
  };
  if (onStatus) onStatus("preview", "SUBMITTING");
  const previewId = await submitTask(apiKey, previewBody);
  if (onStatus) onStatus("preview", `SUBMITTED:${previewId}`);
  const previewTask = await pollUntilDone(apiKey, previewId, {
    onTick: (s) => onStatus && onStatus("preview", s),
  });

  let finalTask = previewTask;
  let refineId;
  if (refine) {
    if (onStatus) onStatus("refine", "SUBMITTING");
    refineId = await submitTask(apiKey, {
      mode: "refine",
      preview_task_id: previewId,
    });
    if (onStatus) onStatus("refine", `SUBMITTED:${refineId}`);
    finalTask = await pollUntilDone(apiKey, refineId, {
      onTick: (s) => onStatus && onStatus("refine", s),
    });
  }

  const dl = await downloadModel(finalTask, destFileNoExt);
  return {
    preview_task_id: previewId,
    ...(refineId ? { refine_task_id: refineId } : {}),
    final_task_id: refineId ?? previewId,
    ...dl,
  };
}
