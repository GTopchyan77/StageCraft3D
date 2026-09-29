#!/usr/bin/env node
// Copyright 2025-2026 NGG. All Rights Reserved.
// interchange.live.test.mjs — end-to-end exercise of the Interchange import
// tools against a RUNNING UE5 editor.
//
// Generates its own source file (a small PNG built here, so the test does not
// depend on any art existing in the project), imports it, and cleans up both the
// imported assets and the temporary file.
//
// Run:  node interchange.live.test.mjs

import { writeFileSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { deflateSync } from "node:zlib";

import * as ich from "./interchange.js";
import * as ue5 from "./ue5client.js";

const ROOT = "/Game/__NGGInterchangeTest__";
const PIPELINE = `${ROOT}/PL_Test`;

let pass = 0, fail = 0;

function check(name, cond, detail) {
  if (cond) { pass++; console.log(`  PASS  ${name}`); }
  else { fail++; console.log(`  FAIL  ${name}${detail ? "\n        " + detail : ""}`); }
}

async function step(name, fn) {
  try {
    const r = await fn();
    if (r && r.ok === false) { fail++; console.log(`  FAIL  ${name}\n        ${r.error}`); return null; }
    pass++; console.log(`  PASS  ${name}`);
    return r?.data ?? r;
  } catch (e) {
    fail++; console.log(`  FAIL  ${name}\n        ${e.message}`);
    return null;
  }
}

async function py(body) {
  const r = await ue5.execPython(`import unreal\n${body}\n`, "execute_file");
  return (r.log ?? []).map(e => e.output ?? "").join("");
}

// --- build a source file ----------------------------------------------------
//
// A 4x4 RGBA PNG, written by hand so this test needs nothing from the project.

function crc32(buf) {
  let c, crc = 0xffffffff;
  for (let n = 0; n < buf.length; n++) {
    c = (crc ^ buf[n]) & 0xff;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    crc = c ^ (crc >>> 8);
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function chunk(type, data) {
  const len = Buffer.alloc(4);
  len.writeUInt32BE(data.length);
  const body = Buffer.concat([Buffer.from(type, "ascii"), data]);
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(body));
  return Buffer.concat([len, body, crc]);
}

function makePng(size = 4) {
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(size, 0);
  ihdr.writeUInt32BE(size, 4);
  ihdr[8] = 8;    // bit depth
  ihdr[9] = 6;    // colour type RGBA
  const raw = Buffer.alloc(size * (size * 4 + 1));
  for (let y = 0; y < size; y++) {
    const row = y * (size * 4 + 1);
    raw[row] = 0; // filter: none
    for (let x = 0; x < size; x++) {
      const p = row + 1 + x * 4;
      raw[p] = (x * 60) & 0xff;
      raw[p + 1] = (y * 60) & 0xff;
      raw[p + 2] = 128;
      raw[p + 3] = 255;
    }
  }
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk("IHDR", ihdr),
    chunk("IDAT", deflateSync(raw)),
    chunk("IEND", Buffer.alloc(0)),
  ]);
}

const workDir = mkdtempSync(join(tmpdir(), "ngg-interchange-"));
const PNG = join(workDir, "NGGProbe.png");
writeFileSync(PNG, makePng());
console.log(`\nsource file: ${PNG}`);

// --- inspect ----------------------------------------------------------------

console.log("\n=== inspect ===");
const insp = await step("inspect the generated PNG", () => ich.interchangeInspect({ source_file: PNG }));
check("the file reports as importable", insp?.can_import === true, JSON.stringify(insp?.can_import));
check("a translator is named", typeof insp?.translator === "string" && insp.translator.length > 0,
  String(insp?.translator));
check("pipeline property groups are listed",
  insp?.pipeline_properties && insp.pipeline_properties.mesh_pipeline?.includes("build_nanite"),
  JSON.stringify(Object.keys(insp?.pipeline_properties || {})));
check("root-level properties are separated from the groups",
  insp?.pipeline_properties?.["(root)"]?.includes("import_offset_uniform_scale"),
  JSON.stringify(insp?.pipeline_properties?.["(root)"]));
check("the answer says how to address them", /group\.property/.test(String(insp?.pipeline_note)),
  String(insp?.pipeline_note));

const inspMissing = await ich.interchangeInspect({ source_file: join(workDir, "nope.png") });
check("a missing file is rejected and says whose filesystem is read",
  inspMissing.ok === false && /machine the editor runs on/.test(inspMissing.error), inspMissing.error);

const inspNoFile = await step("inspect with no file still lists the pipeline",
  () => ich.interchangeInspect({}));
check("no can_import key when no file was given", inspNoFile?.can_import === undefined,
  String(inspNoFile?.can_import));

// --- import -----------------------------------------------------------------

console.log("\n=== import ===");
const imp = await step("import the PNG through Interchange", () => ich.interchangeImport({
  source_file: PNG,
  destination_path: ROOT,
}));
check("an asset came back", imp?.imported?.length >= 1, JSON.stringify(imp?.imported));
check("it is a texture", imp?.imported?.some(o => /Texture/.test(o.class)), JSON.stringify(imp?.imported));
check("with no settings, the project's default stack is used",
  imp?.pipeline === "project default stack", String(imp?.pipeline));

const importedPath = imp?.imported?.[0]?.asset;
const exists = await py(`print('EXISTS=%s' % unreal.EditorAssetLibrary.does_asset_exist('${importedPath}'))`);
check("the asset really exists in the content browser", /EXISTS=True/.test(exists), exists.trim());

const impNamed = await step("import again under a chosen name with settings", () => ich.interchangeImport({
  source_file: PNG,
  destination_path: ROOT,
  asset_name: "T_NGGRenamed",
  pipeline_settings: { "material_pipeline.import_materials": false },
}));
// Interchange ignores the pipeline's asset_name for a texture import, so this
// only lands if the tool finishes the rename itself.
check("the chosen name was used",
  impNamed?.imported?.some(o => /T_NGGRenamed$/.test(o.asset)), JSON.stringify(impNamed?.imported));
check("the rename is reported as a post-import step, not passed off as the importer's doing",
  impNamed?.imported?.[0]?.renamed_after_import === true, JSON.stringify(impNamed?.imported?.[0]));
check("the setting is echoed back as applied",
  impNamed?.settings_applied?.["material_pipeline.import_materials"] === "False",
  JSON.stringify(impNamed?.settings_applied));
check("the pipeline is reported as configured in memory",
  impNamed?.pipeline === "configured in memory", String(impNamed?.pipeline));

console.log("\n=== import validation ===");
const impNoSource = await ich.interchangeImport({ destination_path: ROOT });
check("no source_file is refused", impNoSource.ok === false && /source_file is required/.test(impNoSource.error), impNoSource.error);

const impNoDest = await ich.interchangeImport({ source_file: PNG });
check("no destination_path is refused", impNoDest.ok === false && /destination_path is required/.test(impNoDest.error), impNoDest.error);

const impFsDest = await ich.interchangeImport({ source_file: PNG, destination_path: "C:/Temp" });
check("a filesystem destination is caught before the import runs",
  impFsDest.ok === false && /content path starting with/.test(impFsDest.error), impFsDest.error);

const impMissing = await ich.interchangeImport({ source_file: join(workDir, "gone.png"), destination_path: ROOT });
check("a missing source file is rejected", impMissing.ok === false && /no file at/.test(impMissing.error), impMissing.error);

const impBadSetting = await ich.interchangeImport({
  source_file: PNG, destination_path: ROOT,
  pipeline_settings: { "mesh_pipeline.make_it_shiny": true },
});
check("an unknown pipeline property lists the real ones at that level",
  impBadSetting.ok === false && /is not a property of/.test(impBadSetting.error) &&
  /build_nanite/.test(impBadSetting.error), impBadSetting.error);

const impBadGroup = await ich.interchangeImport({
  source_file: PNG, destination_path: ROOT,
  pipeline_settings: { "no_such_group.thing": true },
});
check("an unknown group is caught at the group, not the leaf",
  impBadGroup.ok === false && /'no_such_group' is not a property of/.test(impBadGroup.error), impBadGroup.error);

const impBadPipeline = await ich.interchangeImport({
  source_file: PNG, destination_path: ROOT, pipeline_asset: `${ROOT}/NoSuchPipeline`,
});
check("a missing pipeline asset is rejected",
  impBadPipeline.ok === false && /pipeline asset not found/.test(impBadPipeline.error), impBadPipeline.error);

// --- pipeline assets --------------------------------------------------------

console.log("\n=== pipeline asset ===");
const pl = await step("save a reusable pipeline", () => ich.interchangePipelineCreate({
  asset_path: PIPELINE,
  build_nanite: true,
  import_lods: false,
  pipeline_settings: { "mesh_pipeline.combine_static_meshes_behavior": "all" },
}));
check("it was created", pl?.created === true, String(pl?.created));
check("a shortcut argument mapped onto its real path",
  pl?.settings_applied?.["mesh_pipeline.build_nanite"] === "True",
  JSON.stringify(pl?.settings_applied));
check("a shortcut set to false is applied, not dropped",
  pl?.settings_applied?.["common_meshes_properties.import_lods"] === "False",
  JSON.stringify(pl?.settings_applied));
check("an explicit dotted setting resolved a short enum name",
  /ALL/.test(String(pl?.settings_applied?.["mesh_pipeline.combine_static_meshes_behavior"])),
  JSON.stringify(pl?.settings_applied));
check("the asset was saved", pl?.saved === true, String(pl?.saved));

// dir() lists properties UE has deprecated, and reading one throws. The tool
// must say which replacement to use instead of "no such property".
const plDeprecated = await ich.interchangePipelineCreate({
  asset_path: `${ROOT}/PL_Dep`, pipeline_settings: { "mesh_pipeline.combine_static_meshes": true },
});
check("a deprecated property answers with the engine's own replacement note",
  plDeprecated.ok === false && /deprecated in this engine version/.test(plDeprecated.error) &&
  /CombineStaticMeshesBehavior/.test(plDeprecated.error), plDeprecated.error);

check("deprecated names are kept out of the reported property list",
  !insp?.pipeline_properties?.mesh_pipeline?.includes("combine_static_meshes"),
  JSON.stringify(insp?.pipeline_properties?.mesh_pipeline?.filter(n => /combine/.test(n))));

const plDupe = await ich.interchangePipelineCreate({ asset_path: PIPELINE, build_nanite: true });
check("creating over an existing pipeline is refused",
  plDupe.ok === false && /already exists/.test(plDupe.error), plDupe.error);

const plEmpty = await ich.interchangePipelineCreate({ asset_path: `${ROOT}/PL_Empty` });
check("a pipeline asset with no settings is refused",
  plEmpty.ok === false && /project\s+default/.test(plEmpty.error), plEmpty.error);

const plBad = await ich.interchangePipelineCreate({
  asset_path: `${ROOT}/PL_Bad`, pipeline_settings: { "mesh_pipeline.nope": 1 },
});
check("a bad setting refuses and removes the half-made asset",
  plBad.ok === false && /removed again/.test(plBad.error), plBad.error);
const badGone = await py(`print('GONE=%s' % (not unreal.EditorAssetLibrary.does_asset_exist('${ROOT}/PL_Bad')))`);
check("the half-made pipeline asset really is gone", /GONE=True/.test(badGone), badGone.trim());

const impWithPipeline = await step("import using the saved pipeline", () => ich.interchangeImport({
  source_file: PNG, destination_path: ROOT, asset_name: "T_ViaPipeline",
  pipeline_asset: PIPELINE,
}));
check("the answer names the pipeline asset used",
  String(impWithPipeline?.pipeline).includes(PIPELINE), String(impWithPipeline?.pipeline));

const impOverride = await step("layer settings on top of a saved pipeline", () => ich.interchangeImport({
  source_file: PNG, destination_path: ROOT, asset_name: "T_ViaOverride",
  pipeline_asset: PIPELINE,
  pipeline_settings: { "mesh_pipeline.build_nanite": false },
}));
check("the override was applied",
  impOverride?.settings_applied?.["mesh_pipeline.build_nanite"] === "False",
  JSON.stringify(impOverride?.settings_applied));

const savedStill = await py(`
p = unreal.load_asset('${PIPELINE}')
print('SAVED_NANITE=%s' % p.get_editor_property('mesh_pipeline').get_editor_property('build_nanite'))
print('SCRATCH=%s' % unreal.EditorAssetLibrary.does_directory_exist('/Game/__NGGInterchangeTmp__'))
`);
check("the saved pipeline was NOT modified by the per-import override",
  /SAVED_NANITE=True/.test(savedStill), savedStill.trim());
check("the throwaway copy was cleaned up", /SCRATCH=False/.test(savedStill), savedStill.trim());

// --- teardown ---------------------------------------------------------------

console.log("\n=== teardown ===");
const cleanup = await py(`
lib = unreal.EditorAssetLibrary
for d in ['${ROOT}', '/Game/__NGGInterchangeTmp__']:
    if lib.does_directory_exist(d):
        for p in lib.list_assets(d, recursive=True):
            try:
                lib.delete_asset(p.split('.')[0])
            except Exception:
                pass
        lib.delete_directory(d)
print('DIR_EXISTS=%s' % lib.does_directory_exist('${ROOT}'))
`);
check("scratch assets removed", /DIR_EXISTS=False/.test(cleanup), cleanup.trim().slice(-300));

rmSync(workDir, { recursive: true, force: true });

console.log(`\n━━━ interchange: ${pass} passed, ${fail} failed ━━━`);
process.exit(fail ? 1 : 0);
