// Copyright 2025-2026 NGG. All Rights Reserved.
// sequencer.js — unrealngg-mcp
//
// Level Sequence authoring: reading a sequence's structure, adding bindings and
// tracks, cutting sections, and keying channels.
//
// Runs through POST /editor/exec_python, like pcg.js and the three original
// sequencer helpers in gamedev.js. Kept in its own module rather than growing
// gamedev.js further — this is the largest single subsystem added to the tool
// catalog and it has its own vocabulary (bindings, tracks, sections, channels).
//
// Requires an AuthToken, because exec_python does. The plugin provisions one on
// first launch (see FNGGHttpServer::ProvisionAuthToken), so this is normally
// automatic; ue5_health_check reports the state if it is not.
//
// Design notes, all confirmed against a live UE 5.8.1 editor:
//
//   * Channels are addressed BY NAME ("Location.X", "Rotation.Z", "Scale.Y"),
//     not by index. Every scripting channel exposes `channel_name`. The older
//     gamedev.js helper hard-codes "channels 0..2 = Location" with a comment,
//     which silently keys the wrong property the moment a track's channel
//     layout differs. Names are self-describing and survive layout changes.
//   * A 3D transform track yields 9 MovieSceneScriptingDoubleChannel entries —
//     Double, not Float. Passing a float channel type would find nothing.
//   * Times are given in SECONDS at the API boundary and converted with the
//     sequence's own display rate. Frames are an implementation detail the
//     caller should not have to track, but they are echoed back in results.

import * as ue5 from "./ue5client.js";

// ---------------------------------------------------------------------------
// Python plumbing — same sentinel pattern as gamedev.js / pcg.js, own markers
// so a stray line from another module can't be mistaken for our result.
// ---------------------------------------------------------------------------

const RESULT_BEGIN = "__NGG_SEQ_RESULT_BEGIN__";
const RESULT_END   = "__NGG_SEQ_RESULT_END__";

const PY_PRELUDE = `
import unreal, json, base64, traceback

def ngg_result(obj):
    print("${RESULT_BEGIN}" + json.dumps(obj) + "${RESULT_END}")

def ngg_error(msg):
    ngg_result({"ok": False, "error": msg})

def s(v):
    """unreal.Text / FName / structs are not JSON-serialisable."""
    return None if v is None else str(v)

def load_sequence(path):
    seq = unreal.load_asset(path)
    if seq is None:
        return None, "LevelSequence not found: " + path
    if not isinstance(seq, unreal.LevelSequence):
        return None, path + " is a " + type(seq).__name__ + ", not a LevelSequence"
    return seq, None

def fps_of(seq):
    r = seq.get_display_rate()
    den = r.denominator if r.denominator else 1
    return float(r.numerator) / float(den)

def frame_int(seq, seconds):
    """Seconds -> plain int frame in display-rate space."""
    return int(round(float(seconds) * fps_of(seq)))

def to_frame(seq, seconds):
    """Seconds -> FrameNumber. For add_key, which nativizes a FrameNumber.

    The scripting API is asymmetric here, and it is not obvious: channel
    add_key() takes a FrameNumber, while MovieSceneSection.set_range() and
    MovieSceneSequence.set_playback_start/end() take int32 and raise
    "Cannot nativize 'FrameNumber' as 'int32'" if handed one. Use frame_int()
    for those. (get_playback_start() likewise returns a plain int.)
    """
    return unreal.FrameNumber(frame_int(seq, seconds))

def frame_value(t):
    """FrameNumber or FrameTime -> int, whichever the API handed back."""
    if hasattr(t, "frame_number"):
        return int(t.frame_number.value)
    if hasattr(t, "value"):
        return int(t.value)
    return int(t)

def find_binding(seq, name):
    for b in seq.get_bindings():
        if str(b.get_name()) == name or str(b.get_display_name()) == name:
            return b
    return None

def resolve_track_class(name):
    """Accept 'Transform' or the full 'MovieScene3DTransformTrack'."""
    alias = {
        "transform":        "MovieScene3DTransformTrack",
        "visibility":       "MovieSceneVisibilityTrack",
        "audio":            "MovieSceneAudioTrack",
        "skeletalanimation":"MovieSceneSkeletalAnimationTrack",
        "animation":        "MovieSceneSkeletalAnimationTrack",
        "cameracut":        "MovieSceneCameraCutTrack",
        "event":            "MovieSceneEventTrack",
        "float":            "MovieSceneFloatTrack",
        "double":           "MovieSceneDoubleTrack",
        "bool":             "MovieSceneBoolTrack",
        "integer":          "MovieSceneIntegerTrack",
        "color":            "MovieSceneColorTrack",
        "vector":           "MovieSceneVectorTrack",
        "particle":         "MovieSceneParticleTrack",
        "cinematicshot":    "MovieSceneCinematicShotTrack",
    }
    key = name.replace("_", "").replace(" ", "").lower()
    cls_name = alias.get(key, name)
    cls = getattr(unreal, cls_name, None)
    if cls is None:
        return None, ("unknown track type '" + name + "'. Use a short alias (" +
                      ", ".join(sorted(alias.keys())) + ") or an exact UClass name.")
    return cls, None
`;

// base64 the payload so paths, labels and property names containing quotes,
// backslashes or newlines cannot corrupt or inject into the script.
function pyArgs(payload, varName = "args") {
  const jsonStr = typeof payload === "string" ? payload : JSON.stringify(payload);
  const b64 = Buffer.from(jsonStr, "utf8").toString("base64");
  return `${varName} = json.loads(base64.b64decode("${b64}").decode("utf-8"))`;
}

async function runPy(body) {
  const indented = body.split("\n").map((l) => "    " + l).join("\n");
  const script =
    PY_PRELUDE +
    "def _ngg_main():\n" + indented + "\n" +
    "try:\n    _ngg_main()\nexcept Exception as _e:\n" +
    "    ngg_error(str(_e) + '\\n' + traceback.format_exc())\n";

  let resp;
  try {
    resp = await ue5.execPython(script, "execute_file");
  } catch (err) {
    // execPython already rewrites the no-token 403 into actionable advice.
    return { ok: false, error: `bridge: ${err.message}` };
  }

  for (const entry of resp.log ?? []) {
    const out = entry.output ?? "";
    // lastIndexOf: if a previous run's sentinel is still in the log buffer, the
    // newest result is the one we want.
    const start = out.lastIndexOf(RESULT_BEGIN);
    const end = start >= 0 ? out.indexOf(RESULT_END, start + RESULT_BEGIN.length) : -1;
    if (start >= 0 && end > start) {
      const json = out.slice(start + RESULT_BEGIN.length, end);
      try {
        const parsed = JSON.parse(json);
        if (parsed && parsed.ok === false) return { ok: false, error: parsed.error, raw: resp };
        return { ok: true, data: parsed, raw: resp };
      } catch (err) {
        return { ok: false, error: `bad result json: ${err.message}`, raw: resp };
      }
    }
  }

  const logStr = (resp.log ?? []).map((e) => `${e.type}: ${e.output}`).join("\n");
  return { ok: false, error: `no result sentinel; log:\n${logStr}`, raw: resp };
}

/** Shape a runPy outcome as an MCP tool response. */
export function mcpResponse(result) {
  if (result.ok) {
    return { content: [{ type: "text", text: JSON.stringify(result.data, null, 2) }] };
  }
  return { content: [{ type: "text", text: result.error }], isError: true };
}

// ---------------------------------------------------------------------------
// Read
// ---------------------------------------------------------------------------

/**
 * Read a Level Sequence's whole structure: playback range, display rate,
 * bindings, their tracks, each track's sections, and each section's channels
 * with their keys.
 *
 * This is the counterpart to ue5_bp_read_graph. Without it every other
 * sequencer call is a blind write — you cannot know what channel names a track
 * exposes, or what is already keyed.
 *
 * @param {object}  opts
 * @param {string}  opts.sequence_path
 * @param {boolean} [opts.include_keys=true] - set false for a lighter outline
 */
export async function sequencerRead({ sequence_path, include_keys } = {}) {
  return runPy(`
${pyArgs({ sequence_path, include_keys: include_keys !== false })}
seq, err = load_sequence(args["sequence_path"])
if err:
    return ngg_error(err)

fps = fps_of(seq)

def read_section(sec):
    out = {
        "class": type(sec).__name__,
        "row_index": sec.get_row_index(),
        "active": sec.is_active(),
        "locked": sec.is_locked(),
        "has_start_frame": sec.has_start_frame(),
        "has_end_frame": sec.has_end_frame(),
    }
    if sec.has_start_frame():
        out["start_frame"] = frame_value(sec.get_start_frame())
        out["start_seconds"] = sec.get_start_frame_seconds()
    if sec.has_end_frame():
        out["end_frame"] = frame_value(sec.get_end_frame())
        out["end_seconds"] = sec.get_end_frame_seconds()

    channels = []
    for idx, ch in enumerate(sec.get_all_channels()):
        entry = {
            "index": idx,
            "name": s(ch.channel_name),
            "type": type(ch).__name__,
            "num_keys": ch.get_num_keys(),
        }
        if args["include_keys"] and ch.get_num_keys():
            keys = []
            for k in ch.get_keys():
                kf = frame_value(k.get_time())
                item = {"frame": kf, "seconds": (kf / fps) if fps else None}
                try:
                    item["value"] = k.get_value()
                except Exception:
                    item["value"] = None
                if hasattr(k, "get_interpolation_mode"):
                    item["interpolation"] = s(k.get_interpolation_mode())
                keys.append(item)
            entry["keys"] = keys
        channels.append(entry)
    out["channels"] = channels
    return out

def read_track(tr):
    return {
        "class": type(tr).__name__,
        "display_name": s(tr.get_display_name()),
        "sections": [read_section(x) for x in tr.get_sections()],
    }

spawnable_ids = set()
for sp in seq.get_spawnables():
    spawnable_ids.add(str(sp.get_name()))

bindings = []
for b in seq.get_bindings():
    name = str(b.get_name())
    bindings.append({
        "name": name,
        "display_name": s(b.get_display_name()),
        "kind": "spawnable" if name in spawnable_ids else "possessable",
        "bound_class": s(b.get_possessed_object_class().get_name()) if b.get_possessed_object_class() else None,
        "tracks": [read_track(t) for t in b.get_tracks()],
    })

ngg_result({
    "ok": True,
    "sequence": args["sequence_path"],
    "display_rate_fps": fps,
    "playback_start_frame": frame_value(seq.get_playback_start()),
    "playback_end_frame": frame_value(seq.get_playback_end()),
    "playback_start_seconds": seq.get_playback_start_seconds(),
    "playback_end_seconds": seq.get_playback_end_seconds(),
    "marked_frames": [
        {"frame": frame_value(m.frame_number), "label": s(m.label)}
        for m in seq.get_marked_frames()
    ],
    "master_tracks": [read_track(t) for t in seq.get_tracks()],
    "bindings": bindings,
})
`);
}

// ---------------------------------------------------------------------------
// Bindings
// ---------------------------------------------------------------------------

/**
 * Add a spawnable binding — an object the sequence itself creates, rather than
 * one it possesses from the level. Either from a class ("CineCameraActor",
 * "PointLight") or from an existing asset path (a Blueprint, a static mesh).
 *
 * @param {object} opts
 * @param {string} opts.sequence_path
 * @param {string} [opts.actor_class]  - class name, e.g. "CineCameraActor"
 * @param {string} [opts.asset_path]   - or an asset to spawn from
 * @param {string} [opts.display_name] - rename the binding in the outliner
 * @param {boolean}[opts.save=true]
 */
export async function sequencerAddSpawnable({
  sequence_path, actor_class, asset_path, display_name, save,
} = {}) {
  return runPy(`
${pyArgs({ sequence_path, actor_class, asset_path, display_name, save: save !== false })}
seq, err = load_sequence(args["sequence_path"])
if err:
    return ngg_error(err)

if not args.get("actor_class") and not args.get("asset_path"):
    return ngg_error("pass either actor_class or asset_path")

binding = None
if args.get("asset_path"):
    obj = unreal.load_asset(args["asset_path"])
    if obj is None:
        return ngg_error("asset not found: " + args["asset_path"])
    binding = seq.add_spawnable_from_instance(obj)
else:
    cls = getattr(unreal, args["actor_class"], None)
    if cls is None:
        return ngg_error("unknown actor_class '" + args["actor_class"] +
                         "'. Use an unreal.* class name such as CineCameraActor or PointLight.")
    binding = seq.add_spawnable_from_class(cls)

if binding is None or not binding.is_valid():
    return ngg_error("the editor refused to create the spawnable binding")

if args.get("display_name"):
    binding.set_display_name(args["display_name"])
    binding.set_name(args["display_name"])

saved = None
if args["save"]:
    saved = unreal.EditorAssetLibrary.save_asset(args["sequence_path"], only_if_is_dirty=False)

ngg_result({
    "ok": True,
    "sequence": args["sequence_path"],
    "binding_name": str(binding.get_name()),
    "display_name": s(binding.get_display_name()),
    "kind": "spawnable",
    "saved": saved,
})
`);
}

// ---------------------------------------------------------------------------
// Tracks
// ---------------------------------------------------------------------------

/**
 * Add a track. With `binding` it goes on that object; without one it becomes a
 * master track on the sequence (that is where CameraCut and Event belong).
 *
 * @param {object} opts
 * @param {string} opts.sequence_path
 * @param {string} opts.track_type      - alias ("transform") or UClass name
 * @param {string} [opts.binding]       - binding name/display name
 * @param {string} [opts.property_path] - required for property tracks (float/double/bool/…)
 * @param {boolean}[opts.add_section=true] - also cut one section spanning the playback range
 * @param {boolean}[opts.save=true]
 */
export async function sequencerAddTrack({
  sequence_path, track_type, binding, property_path, add_section, save,
} = {}) {
  return runPy(`
${pyArgs({
  sequence_path, track_type, binding, property_path,
  add_section: add_section !== false, save: save !== false,
})}
seq, err = load_sequence(args["sequence_path"])
if err:
    return ngg_error(err)

cls, err = resolve_track_class(args["track_type"])
if err:
    return ngg_error(err)

owner_desc = "sequence (master track)"
if args.get("binding"):
    b = find_binding(seq, args["binding"])
    if b is None:
        available = [str(x.get_name()) for x in seq.get_bindings()]
        return ngg_error("binding not found: " + args["binding"] +
                         ". Existing bindings: " + (", ".join(available) if available else "(none)"))
    track = b.add_track(cls)
    owner_desc = "binding " + str(b.get_name())
else:
    track = seq.add_track(cls)

if track is None:
    return ngg_error("add_track returned nothing for " + args["track_type"])

# Property tracks are inert until told which property they drive, and the
# failure mode is a silent no-op at evaluation time rather than an error here.
if args.get("property_path"):
    if not hasattr(track, "set_property_name_and_path"):
        return ngg_error(type(track).__name__ + " does not take a property path")
    leaf = args["property_path"].split(".")[-1]
    track.set_property_name_and_path(leaf, args["property_path"])

section = None
if args["add_section"]:
    section = track.add_section()
    if section is not None:
        # get_playback_start/end already return plain ints, which is what
        # set_range wants — see the note on to_frame().
        try:
            section.set_start_frame_bounded(True)
            section.set_end_frame_bounded(True)
            section.set_range(seq.get_playback_start(), seq.get_playback_end())
        except Exception as _sec_err:
            track.remove_section(section)
            return ngg_error("track created but its section could not be bounded (section removed): " +
                             str(_sec_err))

result = {
    "ok": True,
    "sequence": args["sequence_path"],
    "track_class": type(track).__name__,
    "display_name": s(track.get_display_name()),
    "owner": owner_desc,
    "section_added": section is not None,
}
if section is not None:
    result["channels"] = [
        {"index": i, "name": s(c.channel_name), "type": type(c).__name__}
        for i, c in enumerate(section.get_all_channels())
    ]

if args["save"]:
    result["saved"] = unreal.EditorAssetLibrary.save_asset(args["sequence_path"], only_if_is_dirty=False)

ngg_result(result)
`);
}

// ---------------------------------------------------------------------------
// Sections
// ---------------------------------------------------------------------------

/**
 * Cut an extra section on an existing track, bounded in seconds. Use this for
 * clips that do not span the whole sequence (a second animation, a shot).
 *
 * @param {object} opts
 * @param {string} opts.sequence_path
 * @param {string} opts.track_type
 * @param {string} [opts.binding]
 * @param {number} opts.start_seconds
 * @param {number} opts.end_seconds
 * @param {number} [opts.row_index] - stack sections on separate rows
 * @param {boolean}[opts.save=true]
 */
export async function sequencerAddSection({
  sequence_path, track_type, binding, start_seconds, end_seconds, row_index, save,
} = {}) {
  return runPy(`
${pyArgs({
  sequence_path, track_type, binding,
  start_seconds, end_seconds, row_index, save: save !== false,
})}
seq, err = load_sequence(args["sequence_path"])
if err:
    return ngg_error(err)

if args.get("start_seconds") is None or args.get("end_seconds") is None:
    return ngg_error("start_seconds and end_seconds are required")
if float(args["end_seconds"]) <= float(args["start_seconds"]):
    return ngg_error("end_seconds must be greater than start_seconds")

cls, err = resolve_track_class(args["track_type"])
if err:
    return ngg_error(err)

if args.get("binding"):
    b = find_binding(seq, args["binding"])
    if b is None:
        return ngg_error("binding not found: " + args["binding"])
    tracks = b.find_tracks_by_type(cls)
else:
    tracks = seq.find_tracks_by_type(cls)

if not tracks:
    return ngg_error("no " + args["track_type"] + " track exists yet — add one first")

track = tracks[0]
section = track.add_section()
if section is None:
    return ngg_error("add_section returned nothing")

# add_section() has already mutated the track, so anything that fails from here
# has to take the half-built section with it — otherwise a rejected range leaves
# an unbounded section behind for the caller to trip over.
try:
    section.set_start_frame_bounded(True)
    section.set_end_frame_bounded(True)
    section.set_range(frame_int(seq, args["start_seconds"]), frame_int(seq, args["end_seconds"]))
    if args.get("row_index") is not None:
        section.set_row_index(int(args["row_index"]))
except Exception as _range_err:
    track.remove_section(section)
    return ngg_error("failed to bound the new section (it was removed again): " + str(_range_err))

result = {
    "ok": True,
    "sequence": args["sequence_path"],
    "track_class": type(track).__name__,
    "section_index": len(track.get_sections()) - 1,
    "start_frame": frame_value(section.get_start_frame()),
    "end_frame": frame_value(section.get_end_frame()),
    "row_index": section.get_row_index(),
    "channels": [
        {"index": i, "name": s(c.channel_name), "type": type(c).__name__}
        for i, c in enumerate(section.get_all_channels())
    ],
}
if args["save"]:
    result["saved"] = unreal.EditorAssetLibrary.save_asset(args["sequence_path"], only_if_is_dirty=False)

ngg_result(result)
`);
}

// ---------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------

/**
 * Key channels by name.
 *
 * `keys` is a list of {channel, time, value, interpolation?} where `channel` is
 * a channel_name as reported by sequencerRead / sequencerAddTrack — e.g.
 * "Location.X", "Rotation.Z". Names, not indices: a track's channel layout is
 * not a stable contract, and an off-by-one silently keys the wrong property.
 *
 * @param {object} opts
 * @param {string} opts.sequence_path
 * @param {string} opts.track_type
 * @param {string} [opts.binding]
 * @param {number} [opts.section_index=0]
 * @param {Array}  opts.keys
 * @param {boolean}[opts.save=true]
 */
export async function sequencerSetKeys({
  sequence_path, track_type, binding, section_index, keys, save,
} = {}) {
  return runPy(`
${pyArgs({
  sequence_path, track_type, binding,
  section_index: section_index ?? 0, keys: keys ?? [], save: save !== false,
})}
seq, err = load_sequence(args["sequence_path"])
if err:
    return ngg_error(err)

if not args["keys"]:
    return ngg_error("keys[] is empty — nothing to do")

cls, err = resolve_track_class(args["track_type"])
if err:
    return ngg_error(err)

if args.get("binding"):
    b = find_binding(seq, args["binding"])
    if b is None:
        return ngg_error("binding not found: " + args["binding"])
    tracks = b.find_tracks_by_type(cls)
else:
    tracks = seq.find_tracks_by_type(cls)

if not tracks:
    return ngg_error("no " + args["track_type"] + " track found — add one first")

sections = tracks[0].get_sections()
idx = int(args["section_index"])
if idx < 0 or idx >= len(sections):
    return ngg_error("section_index %d out of range (track has %d section(s))" % (idx, len(sections)))
section = sections[idx]

by_name = {}
for ch in section.get_all_channels():
    by_name[str(ch.channel_name)] = ch

INTERP = {
    "auto":     unreal.RichCurveInterpMode.RCIM_CUBIC,
    "cubic":    unreal.RichCurveInterpMode.RCIM_CUBIC,
    "linear":   unreal.RichCurveInterpMode.RCIM_LINEAR,
    "constant": unreal.RichCurveInterpMode.RCIM_CONSTANT,
}

applied = []
for spec in args["keys"]:
    cname = spec.get("channel")
    if cname is None:
        return ngg_error("every key needs a 'channel' name; available: " + ", ".join(sorted(by_name.keys())))
    ch = by_name.get(cname)
    if ch is None:
        return ngg_error("channel '" + str(cname) + "' not on this section. Available: " +
                         ", ".join(sorted(by_name.keys())))
    if spec.get("time") is None:
        return ngg_error("key for channel '" + str(cname) + "' has no 'time' (seconds)")
    if "value" not in spec:
        return ngg_error("key for channel '" + str(cname) + "' has no 'value'")

    frame = to_frame(seq, spec["time"])
    value = spec["value"]
    # Bool channels reject a numeric 0/1 and integer channels reject a float,
    # so coerce to what the channel class actually stores.
    cls_name = type(ch).__name__
    if "Bool" in cls_name:
        value = bool(value)
    elif "Integer" in cls_name or "Byte" in cls_name:
        value = int(value)
    elif "String" in cls_name or "Text" in cls_name:
        value = str(value)
    else:
        value = float(value)

    key = ch.add_key(frame, value)
    mode = spec.get("interpolation")
    if mode and hasattr(key, "set_interpolation_mode"):
        m = INTERP.get(str(mode).lower())
        if m is None:
            return ngg_error("unknown interpolation '" + str(mode) + "' (use auto, cubic, linear or constant)")
        key.set_interpolation_mode(m)

    applied.append({"channel": cname, "frame": frame_value(frame), "value": value})

result = {
    "ok": True,
    "sequence": args["sequence_path"],
    "track_class": type(tracks[0]).__name__,
    "section_index": idx,
    "keys_applied": len(applied),
    "keys": applied,
}
if args["save"]:
    result["saved"] = unreal.EditorAssetLibrary.save_asset(args["sequence_path"], only_if_is_dirty=False)

ngg_result(result)
`);
}

// ---------------------------------------------------------------------------
// Sequence-level settings
// ---------------------------------------------------------------------------

/**
 * Set the playback range and/or display rate of an existing sequence.
 *
 * Changing the display rate is deliberately separate from the length: frames
 * already keyed are stored in tick resolution, so a rate change re-times the
 * ruler, not the content.
 *
 * @param {object} opts
 * @param {string} opts.sequence_path
 * @param {number} [opts.start_seconds]
 * @param {number} [opts.end_seconds]
 * @param {number} [opts.frame_rate]
 * @param {boolean}[opts.save=true]
 */
export async function sequencerSetPlaybackRange({
  sequence_path, start_seconds, end_seconds, frame_rate, save,
} = {}) {
  return runPy(`
${pyArgs({ sequence_path, start_seconds, end_seconds, frame_rate, save: save !== false })}
seq, err = load_sequence(args["sequence_path"])
if err:
    return ngg_error(err)

if (args.get("start_seconds") is None and args.get("end_seconds") is None
        and args.get("frame_rate") is None):
    return ngg_error("nothing to change — pass start_seconds, end_seconds or frame_rate")

if args.get("frame_rate") is not None:
    fr = int(args["frame_rate"])
    if fr <= 0:
        return ngg_error("frame_rate must be positive")
    seq.set_display_rate(unreal.FrameRate(fr, 1))

start_s = args.get("start_seconds")
end_s = args.get("end_seconds")
if start_s is not None and end_s is not None and float(end_s) <= float(start_s):
    return ngg_error("end_seconds must be greater than start_seconds")
if start_s is not None:
    seq.set_playback_start(frame_int(seq, start_s))
if end_s is not None:
    seq.set_playback_end(frame_int(seq, end_s))

result = {
    "ok": True,
    "sequence": args["sequence_path"],
    "display_rate_fps": fps_of(seq),
    "playback_start_frame": frame_value(seq.get_playback_start()),
    "playback_end_frame": frame_value(seq.get_playback_end()),
    "playback_start_seconds": seq.get_playback_start_seconds(),
    "playback_end_seconds": seq.get_playback_end_seconds(),
}
if args["save"]:
    result["saved"] = unreal.EditorAssetLibrary.save_asset(args["sequence_path"], only_if_is_dirty=False)

ngg_result(result)
`);
}
