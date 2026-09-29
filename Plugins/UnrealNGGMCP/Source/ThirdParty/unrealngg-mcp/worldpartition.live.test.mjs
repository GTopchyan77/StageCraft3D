#!/usr/bin/env node
// Copyright 2025-2026 NGG. All Rights Reserved.
// worldpartition.live.test.mjs — end-to-end exercise of the World Partition
// tools against a RUNNING UE5 editor with the UnrealNGGMCP plugin loaded.
//
// Creates a scratch World Partition map, authors Data Layers on it, moves
// actors between them, streams a region, places a Level Instance, then puts the
// editor back on the level it started on and deletes everything it made.
//
// Run:  node worldpartition.live.test.mjs
//
// Note it deliberately opens a different map: Data Layers only exist on a
// partitioned world, so the tools cannot be exercised on a classic level.

import * as wp from "./worldpartition.js";
import * as ue5 from "./ue5client.js";

const ROOT = "/Game/__NGGWPTest__";
const MAP = `${ROOT}/L_Partitioned`;
const ROOM = `${ROOT}/L_Room`;
const LAYER_ASSET = `${ROOT}/DL_Props`;
const CHILD_ASSET = `${ROOT}/DL_PropsDetail`;

let pass = 0, fail = 0;
let startingLevel = null;

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

console.log("\n=== setup ===");
const before = await py(`
ues = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
print('CURRENT=' + ues.get_editor_world().get_path_name())
`);
startingLevel = (before.match(/CURRENT=(\S+)/) || [])[1]?.split(".")[0] ?? null;
console.log(`  (will return the editor to ${startingLevel} afterwards)`);

await step("create a scratch room level to instance later", () => ue5.createLevel(ROOM));
await step("create a World Partition map", () => ue5.createLevel(MAP, { partitioned: true }));

console.log("\n=== wp_read on a partitioned map ===");
const read = await step("read the partitioned world", () => wp.wpRead());
check("world is reported as partitioned", read && read.is_partitioned === true, JSON.stringify(read?.is_partitioned));
check("world bounds are reported", read && Array.isArray(read.editor_world_bounds?.min), JSON.stringify(read?.editor_world_bounds));
check("starts with no data layers", read && read.data_layers?.length === 0, `${read?.data_layers?.length}`);

console.log("\n=== create_data_layer ===");
const layer = await step("create a runtime data layer", () => wp.wpCreateDataLayer({
  asset_path: LAYER_ASSET, type: "runtime", initial_runtime_state: "loaded",
}));
check("layer reports its asset and instance", layer && layer.asset === LAYER_ASSET && !!layer.instance, JSON.stringify(layer)?.slice(0, 200));
check("layer type is runtime", layer?.instance?.type === "runtime", layer?.instance?.type);

const dupe = await wp.wpCreateDataLayer({ asset_path: LAYER_ASSET });
check("creating over an existing asset is refused",
  dupe.ok === false && /already exists/.test(dupe.error), dupe.error);

const badType = await wp.wpCreateDataLayer({ asset_path: `${ROOT}/DL_Bad`, type: "nonsense" });
check("unknown type is rejected", badType.ok === false && /runtime.*editor/.test(badType.error), badType.error);

const badState = await wp.wpCreateDataLayer({ asset_path: `${ROOT}/DL_Bad2`, initial_runtime_state: "nope" });
check("unknown initial_runtime_state lists the valid ones",
  badState.ok === false && /unloaded/.test(badState.error) && /activated/.test(badState.error), badState.error);

const child = await step("create a nested child layer", () => wp.wpCreateDataLayer({
  asset_path: CHILD_ASSET, type: "runtime", parent: layer?.instance?.short_name,
}));
check("child layer created", !!child?.instance, JSON.stringify(child)?.slice(0, 160));

const badParent = await wp.wpCreateDataLayer({ asset_path: `${ROOT}/DL_Orphan`, parent: "NoSuchLayer" });
check("unknown parent names the existing layers",
  badParent.ok === false && /parent layer not found/.test(badParent.error) && /Existing layers/.test(badParent.error),
  badParent.error);

console.log("\n=== actors on layers ===");
await py(`
eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
for i in range(2):
    a = eas.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector(i * 200.0, 0, 0))
    a.set_actor_label('WPTestActor_%d' % i)
print('spawned')
`);

const assigned = await step("add two actors to the layer", () => wp.wpSetActorDataLayers({
  layer: layer?.instance?.short_name, actors: ["WPTestActor_0", "WPTestActor_1"], mode: "add",
}));
check("layer membership is reported back",
  assigned && assigned.layer_actors_now?.length === 2, JSON.stringify(assigned?.layer_actors_now));

const removed = await step("remove one actor again", () => wp.wpSetActorDataLayers({
  layer: layer?.instance?.short_name, actors: ["WPTestActor_1"], mode: "remove",
}));
check("membership shrank to one", removed && removed.layer_actors_now?.length === 1, JSON.stringify(removed?.layer_actors_now));

const missingActor = await wp.wpSetActorDataLayers({
  layer: layer?.instance?.short_name, actors: ["NoSuchActorAtAll"],
});
check("unknown actor label is rejected",
  missingActor.ok === false && /not found in the level/.test(missingActor.error), missingActor.error);

const missingLayer = await wp.wpSetActorDataLayers({ layer: "NoSuchLayer", actors: ["WPTestActor_0"] });
check("unknown layer names the existing ones",
  missingLayer.ok === false && /Existing layers/.test(missingLayer.error), missingLayer.error);

const emptyActors = await wp.wpSetActorDataLayers({ layer: layer?.instance?.short_name, actors: [] });
check("empty actors[] is rejected rather than silently succeeding",
  emptyActors.ok === false && /empty/.test(emptyActors.error), emptyActors.error);

console.log("\n=== layer state ===");
const state = await step("hide the layer and set it to start activated", () => wp.wpSetDataLayerState({
  layer: layer?.instance?.short_name, visible: false, initial_runtime_state: "activated",
}));
check("visibility applied", state?.layer?.is_visible === false, JSON.stringify(state?.layer?.is_visible));
check("initial runtime state applied",
  /Activated/i.test(String(state?.layer?.initial_runtime_state)), String(state?.layer?.initial_runtime_state));

const noopState = await wp.wpSetDataLayerState({ layer: layer?.instance?.short_name });
check("a no-op state call is rejected", noopState.ok === false && /nothing to change/.test(noopState.error), noopState.error);

console.log("\n=== streaming region ===");
const region = await step("load a region by bounds", () => wp.wpLoadRegion({
  min: [-1000, -1000, -1000], max: [1000, 1000, 1000], mode: "load",
}));
check("region reports how many actors matched", region && typeof region.actors_matched === "number", JSON.stringify(region?.actors_matched));

const badBox = await wp.wpLoadRegion({ min: [0, 0, 0], max: [0, 100, 100] });
check("degenerate box is rejected", badBox.ok === false && /greater than min/.test(badBox.error), badBox.error);

const badMode = await wp.wpLoadRegion({ min: [0, 0, 0], max: [1, 1, 1], mode: "sideways" });
check("unknown mode is rejected", badMode.ok === false && /load, unload, pin or unpin/.test(badMode.error), badMode.error);

console.log("\n=== level instance ===");
const li = await step("place a level instance", () => wp.levelInstanceCreate({
  level_asset: ROOM, location: [500, 500, 0], actor_label: "WPTestRoomInstance",
}));
check("level instance reports its asset", li && li.level_asset === ROOM, JSON.stringify(li)?.slice(0, 160));

const badLI = await wp.levelInstanceCreate({ level_asset: `${ROOT}/NoSuchLevel` });
check("missing level asset is rejected", badLI.ok === false && /not found/.test(badLI.error), badLI.error);

console.log("\n=== delete layer ===");
const del = await step("delete the child layer", () => wp.wpDeleteDataLayer({ layer: child?.instance?.short_name }));
check("deleted layer is gone from the list",
  del && !del.layers_now?.includes(child?.instance?.short_name), JSON.stringify(del?.layers_now));
check("the shared asset is kept", del && /kept/.test(String(del.note)), String(del?.note));

const delMissing = await wp.wpDeleteDataLayer({ layer: "NoSuchLayer" });
check("deleting an unknown layer is rejected", delMissing.ok === false, delMissing.error);

console.log("\n=== non-partitioned world ===");
await py(`
les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
les.load_level('${ROOM}')
print('switched')
`);
const plainRead = await step("read a classic level", () => wp.wpRead());
check("classic level is reported as not partitioned", plainRead && plainRead.is_partitioned === false, JSON.stringify(plainRead?.is_partitioned));
check("and says what to do about it", plainRead && /partitioned=true/.test(String(plainRead.note)), String(plainRead?.note));

const layerOnPlain = await wp.wpCreateDataLayer({ asset_path: `${ROOT}/DL_ShouldFail` });
check("creating a data layer on a classic level is refused with guidance",
  layerOnPlain.ok === false && /not partitioned/.test(layerOnPlain.error), layerOnPlain.error);

console.log("\n=== teardown ===");
const cleanup = await py(`
les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

# On a World Partition map each actor lives in its own external package under
# /Game/__ExternalActors__/..., and deleting the map does NOT delete those. They
# only go away when the actors themselves are destroyed, which has to happen
# while their map is the open one. Do that first, then leave.
les.load_level('${MAP}')

# On a partitioned map, actors outside the loaded region are NOT returned by
# get_all_level_actors — they exist only as descriptors until something streams
# them in. Load every descriptor first, or the teardown silently misses most of
# what it is meant to delete.
descs = unreal.WorldPartitionBlueprintLibrary.get_actor_descs() or []
unreal.WorldPartitionBlueprintLibrary.load_actors([d.guid for d in descs])

killed = 0
for a in eas.get_all_level_actors():
    if str(a.get_actor_label()).startswith('WPTest'):
        eas.destroy_actor(a); killed += 1
les.save_current_level()
print('descs=%d destroyed=%d' % (len(descs), killed))

${startingLevel ? `les.load_level('${startingLevel}')` : "pass"}
lib = unreal.EditorAssetLibrary
if lib.does_directory_exist('${ROOT}'):
    for p in lib.list_assets('${ROOT}', recursive=True):
        try:
            lib.delete_asset(p.split('.')[0])
        except Exception as e:
            print('keep ' + p)
    lib.delete_directory('${ROOT}')

# A World Partition map keeps its actors in external packages under
# /Game/__ExternalActors__/<map folder>, which deleting the map folder does not
# touch. Without this the suite leaves a directory behind on every run.
external = '/Game/__ExternalActors__/${ROOT.replace("/Game/", "")}'
if lib.does_directory_exist(external):
    for p in lib.list_assets(external, recursive=True):
        try:
            lib.delete_asset(p.split('.')[0])
        except Exception:
            pass
    lib.delete_directory(external)

print('DIR_EXISTS=%s EXT_EXISTS=%s' % (lib.does_directory_exist('${ROOT}'),
                                       lib.does_directory_exist(external)))
`);
check("scratch assets removed", /DIR_EXISTS=False/.test(cleanup), cleanup.trim().slice(-200));
check("external-actor packages removed too", /EXT_EXISTS=False/.test(cleanup), cleanup.trim().slice(-200));

console.log(`\n━━━ world partition: ${pass} passed, ${fail} failed ━━━`);
process.exit(fail ? 1 : 0);
