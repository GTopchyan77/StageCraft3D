#!/usr/bin/env node
// Copyright 2025-2026 NGG. All Rights Reserved.
// metasound.live.test.mjs — end-to-end exercise of the MetaSound tools against
// a RUNNING UE5 editor with the UnrealNGGMCP plugin loaded.
//
// Builds a playable source (sine -> audio out) with an exposed Frequency
// parameter, a patch, reads them back, retunes an input, then deletes it all.
//
// Run:  node metasound.live.test.mjs
//
// The variant assertions are a regression guard: a MetaSound node class is
// namespace/name/variant, and audio-rate nodes ('Sine') need variant "Audio"
// while trigger nodes need none. Passing the wrong one returns a bare FAILED,
// which is what the auto-detection in metasound.js exists to absorb.

import * as ms from "./metasound.js";
import * as ue5 from "./ue5client.js";

const ROOT = "/Game/__NGGMSTest__";
const SOURCE = `${ROOT}/MS_TestSource`;
const PATCH = `${ROOT}/MS_TestPatch`;

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

console.log("\n=== create a source ===");
const src = await step("build sine -> audio_out with a Frequency parameter", () => ms.metasoundCreate({
  asset_path: SOURCE,
  type: "source",
  output_format: "mono",
  nodes: [{ id: "osc", class: "Sine", inputs: { Frequency: 220.0 } }],
  connections: [{ from: "osc.Audio", to: "audio_out" }],
  graph_inputs: [{ name: "Freq", type: "Float", default: 220.0 }],
}));
check("asset path is reported", src && /MS_TestSource/.test(String(src.asset)), String(src?.asset));
check("variant was auto-detected as Audio", src?.nodes?.[0]?.variant === "Audio", JSON.stringify(src?.nodes?.[0]?.variant));
check("node pin names are reported back",
  src?.nodes?.[0]?.inputs?.includes("Frequency") && src?.nodes?.[0]?.outputs?.includes("Audio"),
  JSON.stringify(src?.nodes?.[0]));
check("connection recorded", src?.connections?.length === 1, JSON.stringify(src?.connections));
check("graph input declared", src?.graph_inputs?.[0]?.name === "Freq", JSON.stringify(src?.graph_inputs));

console.log("\n=== input validation ===");
const dupe = await ms.metasoundCreate({
  asset_path: SOURCE, nodes: [{ id: "osc", class: "Sine" }],
});
check("creating over an existing asset is refused",
  dupe.ok === false && /already exists/.test(dupe.error), dupe.error);

const badClass = await ms.metasoundCreate({
  asset_path: `${ROOT}/MS_Bad`, nodes: [{ id: "x", class: "NoSuchNodeAtAll" }],
});
check("unknown node class explains the namespace/name/variant shape",
  badClass.ok === false && /Tried/.test(badClass.error) && /variant/.test(badClass.error), badClass.error);

const badPin = await ms.metasoundCreate({
  asset_path: `${ROOT}/MS_Bad2`,
  nodes: [{ id: "osc", class: "Sine" }],
  connections: [{ from: "osc.NoSuchPin", to: "audio_out" }],
});
check("unknown output pin lists the real ones",
  badPin.ok === false && /has no output/.test(badPin.error) && /Audio/.test(badPin.error), badPin.error);

const badInputName = await ms.metasoundCreate({
  asset_path: `${ROOT}/MS_Bad3`,
  nodes: [{ id: "osc", class: "Sine", inputs: { NotAPin: 1.0 } }],
});
check("unknown node input lists the real ones",
  badInputName.ok === false && /has no input/.test(badInputName.error) && /Frequency/.test(badInputName.error),
  badInputName.error);

const badRef = await ms.metasoundCreate({
  asset_path: `${ROOT}/MS_Bad4`,
  nodes: [{ id: "osc", class: "Sine" }],
  connections: [{ from: "ghost.Audio", to: "audio_out" }],
});
check("connection from an unknown node id names the known ones",
  badRef.ok === false && /Known nodes/.test(badRef.error), badRef.error);

const badType = await ms.metasoundCreate({ asset_path: `${ROOT}/MS_Bad5`, type: "nonsense" });
check("unknown type is rejected", badType.ok === false && /source.*patch/.test(badType.error), badType.error);

const badGraphInput = await ms.metasoundCreate({
  asset_path: `${ROOT}/MS_Bad6`, graph_inputs: [{ name: "X", type: "NotAType", default: 1 }],
});
check("bad graph input type is rejected with guidance",
  badGraphInput.ok === false && /data type/.test(badGraphInput.error), badGraphInput.error);

console.log("\n=== create a patch ===");
const patch = await step("build a patch", () => ms.metasoundCreate({
  asset_path: PATCH,
  type: "patch",
  nodes: [{ id: "osc", class: "Saw" }],
  graph_inputs: [{ name: "Pitch", type: "Float", default: 110.0 }],
}));
check("patch reports type", patch?.type === "patch", String(patch?.type));

const audioOutOnPatch = await ms.metasoundCreate({
  asset_path: `${ROOT}/MS_Bad7`, type: "patch",
  nodes: [{ id: "osc", class: "Sine" }],
  connections: [{ from: "osc.Audio", to: "audio_out" }],
});
check("'audio_out' on a patch is refused with the reason",
  audioOutOnPatch.ok === false && /only available on a source/.test(audioOutOnPatch.error),
  audioOutOnPatch.error);

console.log("\n=== read ===");
const read = await step("read the source back", () => ms.metasoundRead({ asset_path: SOURCE }));
check("type is source", read?.type === "source", String(read?.type));
check("graph inputs include the declared parameter",
  read?.graph_inputs?.some(n => /Freq/.test(n)), JSON.stringify(read?.graph_inputs));
check("graph outputs include the audio output",
  read?.graph_outputs?.some(n => /Audio/.test(n)), JSON.stringify(read?.graph_outputs));
check("read is explicit that nodes cannot be enumerated",
  read && /cannot enumerate the nodes/.test(String(read.note)), String(read?.note));

const readMissing = await ms.metasoundRead({ asset_path: `${ROOT}/NoSuchAsset` });
check("reading a missing asset is rejected", readMissing.ok === false && /not found/.test(readMissing.error), readMissing.error);

const readWrongType = await ms.metasoundRead({ asset_path: "/Game/ThirdPerson/Lvl_ThirdPerson" });
check("reading a non-MetaSound says what it actually is",
  readWrongType.ok === false && /not a MetaSound/.test(readWrongType.error), readWrongType.error);

console.log("\n=== retune ===");
const retune = await step("change the Freq default", () => ms.metasoundSetGraphInputDefaults({
  asset_path: SOURCE, inputs: { Freq: 440.0 },
}));
check("applied value is reported", retune?.applied?.[0]?.value === 440.0, JSON.stringify(retune?.applied));

const badRetune = await ms.metasoundSetGraphInputDefaults({
  asset_path: SOURCE, inputs: { NoSuchInput: 1.0 },
});
check("unknown graph input lists what the sound exposes",
  badRetune.ok === false && /exposes/.test(badRetune.error), badRetune.error);

const emptyRetune = await ms.metasoundSetGraphInputDefaults({ asset_path: SOURCE, inputs: {} });
check("empty retune is rejected", emptyRetune.ok === false && /nothing to change/.test(emptyRetune.error), emptyRetune.error);

console.log("\n=== teardown ===");
const cleanup = await ue5.execPython(`
import unreal
lib = unreal.EditorAssetLibrary
if lib.does_directory_exist('${ROOT}'):
    for p in lib.list_assets('${ROOT}', recursive=True):
        lib.delete_asset(p.split('.')[0])
    lib.delete_directory('${ROOT}')
print('DIR_EXISTS=%s' % lib.does_directory_exist('${ROOT}'))
`, "execute_file");
const clOut = (cleanup.log ?? []).map(e => e.output ?? "").join("");
check("scratch assets removed", /DIR_EXISTS=False/.test(clOut), clOut.trim().slice(-160));

console.log(`\n━━━ metasound: ${pass} passed, ${fail} failed ━━━`);
process.exit(fail ? 1 : 0);
