// Copyright 2025-2026 NGG. All Rights Reserved.
// pcg.test.util.mjs — shared helpers for pcg.test.mjs and pcg.live.test.mjs.
//
// safeDeleteAsset is the deletion sequence we have to use whenever a PCG test
// graph has been loaded into memory — UE 5.5/5.7's Python bindings root the
// asset via the PCG runtime subsystem + AssetEditorSubsystem, so a plain
// EditorAssetLibrary.delete_asset is silently refused (and the user later sees
// the "PCGGraph is in use" error from the Content Browser).
//
// Order (each step is skipped if the binding isn't exposed in this build):
//   1. close_all_editors_for_asset — drops the asset-editor native ref
//   2. SystemLibrary.collect_garbage / gc.collect — flushes weak refs
//   3. delete_loaded_assets (plural) — most aggressive in-place deletion path
//   4. delete_loaded_asset (singular) — per-asset fallback
//   5. rename_asset to /Game/PCG/_TRASH_<stamp>_<name> — UE allows renames
//      when delete is refused; the original path is freed for reuse and the
//      trashed copy can be removed on the next editor restart.

import * as ue5 from "./ue5client.js";

const PY_SAFE_DELETE = `
import unreal, gc, datetime
paths = json.loads(SAFE_DEL_PAYLOAD)
eal = unreal.EditorAssetLibrary
aes = unreal.get_editor_subsystem(unreal.AssetEditorSubsystem)
stamp = datetime.datetime.now().strftime("%H%M%S")
result = {"deleted": [], "trashed": [], "missing": [], "failed": []}

# Pass 1: close every editor that owns one of our targets.
for p in paths:
    if not eal.does_asset_exist(p):
        result["missing"].append(p)
        continue
    try:
        a = eal.load_asset(p)
        if a is not None and hasattr(aes, "close_all_editors_for_asset"):
            aes.close_all_editors_for_asset(a)
    except Exception:
        pass

# Pass 2: GC twice to clear WeakObjectPtrs picked up during the load above.
for _ in range(2):
    try:
        unreal.SystemLibrary.collect_garbage()
    except Exception:
        try: unreal.collect_garbage(True)
        except Exception: pass
    gc.collect()

# Pass 3: try the plural delete (it gets further past PCG native refs).
to_del = []
for p in paths:
    if eal.does_asset_exist(p):
        a = eal.load_asset(p)
        if a is not None:
            to_del.append((p, a))
if to_del and hasattr(eal, "delete_loaded_assets"):
    try:
        eal.delete_loaded_assets([a for _, a in to_del])
    except Exception:
        pass

# Pass 4: per-asset delete_loaded_asset for anything left.
for p in paths:
    if not eal.does_asset_exist(p):
        if p not in result["missing"] and p not in result["deleted"]:
            result["deleted"].append(p)
        continue
    try:
        a = eal.load_asset(p)
        ok = eal.delete_loaded_asset(a) if hasattr(eal, "delete_loaded_asset") else eal.delete_asset(p)
        if ok:
            result["deleted"].append(p)
            continue
    except Exception:
        pass

    # Pass 5: rename to trash. This is the only path that actually frees the
    # original content path when native refs are wedged.
    try:
        dest = "/Game/PCG/_TRASH_" + p.rsplit("/", 1)[1] + "_" + stamp
        if eal.rename_asset(p, dest):
            result["trashed"].append({"from": p, "to": dest})
        else:
            result["failed"].append(p)
    except Exception:
        result["failed"].append(p)

print("__SAFE_DEL_RESULT__" + json.dumps(result) + "__END__")
`;

/**
 * Robustly delete one or more PCG-related asset paths even when the editor
 * has native references keeping them rooted.
 *
 * @param {string[]} paths - content-browser paths, e.g. ['/Game/PCG/Foo']
 * @returns {Promise<{deleted: string[], trashed: {from:string,to:string}[],
 *                   missing: string[], failed: string[]}>}
 */
export async function safeDeleteAssets(paths) {
  const payload = JSON.stringify(paths);
  // Embed paths via a triple-quoted Python string so escaping stays simple.
  const script =
    `import json\nSAFE_DEL_PAYLOAD = '''${payload}'''\n` + PY_SAFE_DELETE;
  const resp = await ue5.execPython(script, "execute_file");
  const out  = (resp.log ?? []).map((e) => e.output ?? "").join("");
  const m = out.match(/__SAFE_DEL_RESULT__([\s\S]*?)__END__/);
  if (!m) {
    return { deleted: [], trashed: [], missing: [], failed: paths.slice(),
             raw: out };
  }
  try {
    return JSON.parse(m[1]);
  } catch {
    return { deleted: [], trashed: [], missing: [], failed: paths.slice(),
             raw: m[1] };
  }
}
