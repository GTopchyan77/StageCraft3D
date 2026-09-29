#!/usr/bin/env node
// Copyright 2025-2026 NGG. All Rights Reserved.
// sequencer.live.test.mjs — end-to-end exercise of the sequencer tools against
// a RUNNING UE5 editor with the UnrealNGGMCP plugin loaded.
//
// Builds a small cinematic (spawnable camera, transform track, keys, a second
// section, a camera-cut master track, a retime), reads it back, asserts the
// numbers actually landed, then removes everything it made.
//
// Run:  node sequencer.live.test.mjs
// Needs an AuthToken, because it goes through exec_python — the plugin
// provisions one on first launch, so this is normally automatic.
//
// Two things here are regression guards for bugs found the first time this ran
// against 5.8.1, both worth keeping:
//   * frames: set_range / set_playback_start take int32 and reject FrameNumber,
//     while channel add_key takes FrameNumber. The frame assertions catch a
//     regression back to the wrong one.
//   * a failed set_range used to leave a half-built section on the track.
import * as seqmod from "./sequencer.js";
import * as gamedev from "./gamedev.js";
import * as ue5 from "./ue5client.js";

const SEQ = "/Game/__NGGSeqTest__/LS_Test";
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

console.log("\n=== setup ===");
await step("create sequence", () => gamedev.sequencerCreate({
  asset_path: SEQ, frame_rate: 30, length_seconds: 5, overwrite: true,
}));

console.log("\n=== add_spawnable ===");
const sp = await step("add spawnable CineCameraActor", () => seqmod.sequencerAddSpawnable({
  sequence_path: SEQ, actor_class: "CineCameraActor", display_name: "HeroCam",
}));
check("spawnable reports its binding name", sp && !!sp.binding_name, JSON.stringify(sp));

const badClass = await seqmod.sequencerAddSpawnable({ sequence_path: SEQ, actor_class: "NoSuchActorClass" });
check("unknown actor_class is rejected with a useful message",
  badClass.ok === false && /unknown actor_class/i.test(badClass.error), badClass.error);

const noArgs = await seqmod.sequencerAddSpawnable({ sequence_path: SEQ });
check("missing actor_class/asset_path is rejected",
  noArgs.ok === false && /either actor_class or asset_path/.test(noArgs.error), noArgs.error);

console.log("\n=== add_track ===");
const tr = await step("add transform track on the spawnable", () => seqmod.sequencerAddTrack({
  sequence_path: SEQ, track_type: "transform", binding: sp?.binding_name,
}));
check("track reports channel names, not just indices",
  tr && Array.isArray(tr.channels) && tr.channels.some(c => c.name === "Location.X"),
  JSON.stringify(tr?.channels?.slice(0, 3)));
check("transform track exposes 9 channels", tr && tr.channels?.length === 9, `got ${tr?.channels?.length}`);

const badTrack = await seqmod.sequencerAddTrack({ sequence_path: SEQ, track_type: "nonsense_track" });
check("unknown track_type lists the valid aliases",
  badTrack.ok === false && /unknown track type/i.test(badTrack.error) && /transform/.test(badTrack.error),
  badTrack.error);

const badBinding = await seqmod.sequencerAddTrack({
  sequence_path: SEQ, track_type: "transform", binding: "NoSuchBinding",
});
check("unknown binding names the existing ones",
  badBinding.ok === false && /binding not found/.test(badBinding.error) && /Existing bindings/.test(badBinding.error),
  badBinding.error);

console.log("\n=== set_keys ===");
const keyed = await step("key Location.X and Location.Z", () => seqmod.sequencerSetKeys({
  sequence_path: SEQ, track_type: "transform", binding: sp?.binding_name,
  keys: [
    { channel: "Location.X", time: 0, value: 0 },
    { channel: "Location.X", time: 4, value: 500, interpolation: "linear" },
    { channel: "Location.Z", time: 0, value: 100 },
    { channel: "Location.Z", time: 4, value: 300 },
  ],
}));
check("4 keys applied", keyed && keyed.keys_applied === 4, JSON.stringify(keyed?.keys_applied));
check("seconds converted to frames at 30fps (4s -> 120)",
  keyed && keyed.keys?.some(k => k.frame === 120), JSON.stringify(keyed?.keys));

const badChan = await seqmod.sequencerSetKeys({
  sequence_path: SEQ, track_type: "transform", binding: sp?.binding_name,
  keys: [{ channel: "Location.Q", time: 0, value: 1 }],
});
check("unknown channel lists what is available",
  badChan.ok === false && /Available:/.test(badChan.error) && /Location\.X/.test(badChan.error),
  badChan.error);

const badInterp = await seqmod.sequencerSetKeys({
  sequence_path: SEQ, track_type: "transform", binding: sp?.binding_name,
  keys: [{ channel: "Location.X", time: 1, value: 1, interpolation: "bogus" }],
});
check("unknown interpolation is rejected",
  badInterp.ok === false && /unknown interpolation/i.test(badInterp.error), badInterp.error);

console.log("\n=== add_section ===");
const sec = await step("cut a second transform section", () => seqmod.sequencerAddSection({
  sequence_path: SEQ, track_type: "transform", binding: sp?.binding_name,
  start_seconds: 1, end_seconds: 3, row_index: 1,
}));
check("section bounds converted to frames (1s..3s -> 30..90)",
  sec && sec.start_frame === 30 && sec.end_frame === 90,
  `${sec?.start_frame}..${sec?.end_frame}`);

const badRange = await seqmod.sequencerAddSection({
  sequence_path: SEQ, track_type: "transform", binding: sp?.binding_name,
  start_seconds: 3, end_seconds: 1,
});
check("inverted range is rejected",
  badRange.ok === false && /greater than start/.test(badRange.error), badRange.error);

console.log("\n=== master track ===");
const cut = await step("add a CameraCut master track", () => seqmod.sequencerAddTrack({
  sequence_path: SEQ, track_type: "cameracut",
}));
check("master track owner is the sequence", cut && /master track/.test(cut.owner), cut?.owner);

console.log("\n=== playback range ===");
const range = await step("retime to 8s at 60fps", () => seqmod.sequencerSetPlaybackRange({
  sequence_path: SEQ, start_seconds: 0, end_seconds: 8, frame_rate: 60,
}));
check("display rate applied", range && range.display_rate_fps === 60, JSON.stringify(range?.display_rate_fps));
check("end frame recomputed at the new rate (8s * 60 = 480)",
  range && range.playback_end_frame === 480, JSON.stringify(range?.playback_end_frame));

const noop = await seqmod.sequencerSetPlaybackRange({ sequence_path: SEQ });
check("a no-op call is rejected rather than silently doing nothing",
  noop.ok === false && /nothing to change/.test(noop.error), noop.error);

console.log("\n=== read ===");
const read = await step("read the sequence back", () => seqmod.sequencerRead({ sequence_path: SEQ }));
check("read reports the binding", read && read.bindings?.length >= 1, `${read?.bindings?.length} bindings`);
check("read reports the master CameraCut track",
  read && read.master_tracks?.some(t => t.class === "MovieSceneCameraCutTrack"),
  JSON.stringify(read?.master_tracks?.map(t => t.class)));
const b0 = read?.bindings?.[0];
check("binding is marked spawnable", b0 && b0.kind === "spawnable", b0?.kind);
const xform = b0?.tracks?.find(t => t.class === "MovieScene3DTransformTrack");
check("transform track has both sections", xform && xform.sections?.length === 2, `${xform?.sections?.length}`);
const locX = xform?.sections?.[0]?.channels?.find(c => c.name === "Location.X");
check("Location.X keys are read back", locX && locX.num_keys >= 2, JSON.stringify(locX));
check("key values survived the round trip",
  locX?.keys?.some(k => k.value === 500), JSON.stringify(locX?.keys));

const missing = await seqmod.sequencerRead({ sequence_path: "/Game/__NGGSeqTest__/NoSuchSequence" });
check("reading a missing sequence gives a clear error",
  missing.ok === false && /not found/i.test(missing.error), missing.error);

const notASequence = await seqmod.sequencerRead({ sequence_path: SEQ + "_NOPE" });
check("a bad path does not throw", notASequence.ok === false, notASequence.error);

console.log("\n=== teardown ===");
const cleanup = `
import unreal
lib = unreal.EditorAssetLibrary
if lib.does_directory_exist('/Game/__NGGSeqTest__'):
    for p in lib.list_assets('/Game/__NGGSeqTest__', recursive=True):
        lib.delete_asset(p.split('.')[0])
    lib.delete_directory('/Game/__NGGSeqTest__')
print('dir_exists=%s' % lib.does_directory_exist('/Game/__NGGSeqTest__'))
`;
const cl = await ue5.execPython(cleanup, "execute_file");
const clOut = (cl.log ?? []).map(e => e.output).join("");
check("scratch assets removed", /dir_exists=False/.test(clOut), clOut.trim());

console.log(`\n━━━ sequencer: ${pass} passed, ${fail} failed ━━━`);
process.exit(fail ? 1 : 0);
