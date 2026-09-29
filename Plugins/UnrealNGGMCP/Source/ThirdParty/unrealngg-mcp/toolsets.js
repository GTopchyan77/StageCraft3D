// Copyright 2025-2026 NGG. All Rights Reserved.
// toolsets.js — unrealngg-mcp
//
// Groups the tool catalog so a project can register only the parts it uses.
//
// Why this is opt-in rather than auto-detected: the plugin's .uplugin declares
// PCG, GameplayAbilities, Niagara, GeometryScripting, StateTree, EnhancedInput
// and PythonScriptPlugin as dependencies, and UE auto-enables a plugin's
// dependencies. So "is PCG enabled in this project?" is always yes whenever this
// server can run at all — reading the .uproject would tell us nothing. (The
// project's own .uproject doesn't even list PCG for exactly that reason; see
// Docs/FAB_TRC_FIXES.md.) What the engine cannot know is whether the *user*
// intends to author PCG graphs, so that stays an explicit choice.
//
// The full catalog is ~187 KB of tool schemas on every handshake. Dropping the
// sets a project never touches is the cheapest context saving available.
//
// Configuration (both accept a comma/space separated list, case-insensitive):
//   NGG_TOOLSETS          allowlist — only these sets are registered
//   NGG_TOOLSETS_EXCLUDE  denylist  — everything except these
// Unset (the default) registers everything, so existing installs are unchanged.
// `core` is always registered and cannot be excluded.

/**
 * Ordered rules mapping a tool name to its set. First match wins, so the
 * specific prefixes must precede the generic ones.
 *
 * `core` holds the tools any session may need: reachability, project info,
 * saving, the editor lifecycle, generic asset read/write, and the batch runner.
 */
const RULES = [
  // --- core: lifecycle, discovery, generic asset access ---------------------
  [/^ue5_(health_check|project_info|get_log|save_all|setup_skills)$/,      "core"],
  [/^ue5_(launch_editor|kill_editor|build)$/,                             "core"],
  [/^ue5_batch$/,                                                         "core"],
  [/^ue5_(list_assets|get_asset|delete_asset|duplicate_asset)$/,          "core"],
  [/^ue5_(import_asset|reimport_asset)$/,                                 "core"],
  [/^ue5_set_asset_(property|map_entries)$/,                              "core"],
  [/^ue5_create_data_asset$/,                                             "core"],

  // --- distinct subsystems --------------------------------------------------
  [/^pcg_/,                                                               "pcg"],
  [/^ue5_gas_/,                                                           "gas"],
  [/^ue5_bt_/,                                                            "ai"],
  [/^ue5_st_/,                                                            "ai"],
  [/^ue5_profile_/,                                                       "profiling"],
  [/^ue5_mesh_(?!y_)/,                                                    "mesh"],
  [/^ue5_meshy_/,                                                         "meshy"],
  [/^ue5_(anim_|configure_anim_blueprint|create_anim_blueprint|skeleton_)/, "anim"],
  [/^ue5_(ikrig_|ikretargeter_|controlrig_)/,                             "rigging"],
  [/^ue5_(create_niagara_system|configure_niagara_system|set_niagara_emitter_params)$/, "niagara"],
  [/^ue5_(nanite_|lumen_|render_)/,                                       "rendering"],
  [/^ue5_(collision_configure|physical_material_|foliage_type_|texture_configure)/, "assetconfig"],
  [/^ue5_interchange_/,                                                   "interchange"],
  [/^ue5_sequencer_/,                                                     "sequencer"],
  [/^ue5_wp_/,                                                            "worldpartition"],
  [/^ue5_level_instance_/,                                                "worldpartition"],
  [/^ue5_datatable_/,                                                     "datatable"],
  [/^ue5_metasound_/,                                                     "audio"],
  [/^ue5_(create_sound_cue|create_sound_attenuation|spawn_ambient_sound|play_sound_preview)$/, "audio"],
  [/^ue5_substrate_/,                                                     "material"],
  [/^ue5_(create_material|create_material_instance|create_post_process_material|read_material|material_)/, "material"],
  [/^ue5_(create_float_curve|create_color_curve|read_curve)$/,            "material"],

  // --- UMG ------------------------------------------------------------------
  [/^ue5_(create_widget_blueprint|compile_widget_blueprint|get_widget_tree)$/, "umg"],
  [/^ue5_(add_widget_to_blueprint|remove_widget_from_blueprint)$/,        "umg"],
  [/^ue5_(rename_widget|reparent_widget|set_widget_properties|style_widgets|widget_bind_event)$/, "umg"],

  // --- Blueprints -----------------------------------------------------------
  [/^ue5_bp_/,                                                            "blueprint"],
  [/^ue5_create_blueprint/,                                               "blueprint"],
  [/^ue5_(reparent_blueprint|set_blueprint_defaults)$/,                   "blueprint"],
  [/^ue5_(add_component_to_blueprint|remove_component_from_blueprint)$/,  "blueprint"],
  [/^ue5_(get_component_defaults|set_component_defaults)$/,               "blueprint"],

  // --- levels, actors, viewport, PIE ---------------------------------------
  [/^ue5_landscape_/,                                                     "landscape"],
  [/^ue5_(create_level|open_level|set_level_environment|set_world_settings)$/, "level"],
  [/^ue5_(spawn_actor|spawn_post_process_volume|update_actor|delete_actor|list_actors|add_component_to_actor)$/, "level"],
  [/^ue5_(set_viewport_camera|viewport_screenshot)$/,                     "level"],
  [/^ue5_pie_/,                                                           "level"],

  // --- input, tags ----------------------------------------------------------
  [/^ue5_configure_imc$/,                                                 "input"],
  [/^ue5_(add_gameplay_tags|list_gameplay_tags)$/,                        "gameplaytags"],

  // --- ADL exercise authoring (project-specific content tooling) ------------
  [/^ue5_(create_exercise|get_exercise|add_phase_to_exercise|add_step_to_exercise)$/, "exercises"],

  // --- outside the editor ---------------------------------------------------
  [/^youtube_/,                                                           "youtube"],
];

/** Sets that are always registered regardless of configuration. */
export const ALWAYS_ON = new Set(["core"]);

/** Every set this module knows about, in a stable order. */
export const ALL_TOOLSETS = (() => {
  const seen = [];
  for (const [, set] of RULES) if (!seen.includes(set)) seen.push(set);
  return seen;
})();

/**
 * Which set a tool belongs to. Unknown names fall into `core` so a newly added
 * tool is never silently dropped from every configuration — the docs test and
 * `assertToolsetCoverage` are what catch a missing rule.
 *
 * @param {string} toolName
 * @returns {string}
 */
export function toolsetFor(toolName) {
  for (const [pattern, set] of RULES) {
    if (pattern.test(toolName)) return set;
  }
  return "core";
}

/** Split a comma/space separated env value into a lower-cased list. */
function parseList(value) {
  if (!value) return [];
  return value.split(/[,\s]+/).map(s => s.trim().toLowerCase()).filter(Boolean);
}

/**
 * Resolve which sets to register.
 *
 * @param {NodeJS.ProcessEnv} env
 * @returns {{enabled: Set<string>, mode: "all"|"allowlist"|"denylist", unknown: string[]}}
 */
export function resolveToolsets(env = process.env) {
  const allow = parseList(env.NGG_TOOLSETS);
  const deny  = parseList(env.NGG_TOOLSETS_EXCLUDE);
  const known = new Set(ALL_TOOLSETS);
  const unknown = [...allow, ...deny].filter(s => !known.has(s));

  if (allow.length > 0) {
    const enabled = new Set(allow.filter(s => known.has(s)));
    for (const s of ALWAYS_ON) enabled.add(s);
    return { enabled, mode: "allowlist", unknown };
  }
  if (deny.length > 0) {
    const enabled = new Set(ALL_TOOLSETS.filter(s => !deny.includes(s)));
    for (const s of ALWAYS_ON) enabled.add(s);
    return { enabled, mode: "denylist", unknown };
  }
  return { enabled: new Set(ALL_TOOLSETS), mode: "all", unknown };
}

/**
 * Report tool names that only match the catch-all. Used by the test suite to
 * keep the rule table honest as tools are added.
 *
 * @param {string[]} toolNames
 * @returns {string[]} names with no explicit rule
 */
export function assertToolsetCoverage(toolNames) {
  const uncovered = [];
  for (const name of toolNames) {
    const matched = RULES.some(([pattern]) => pattern.test(name));
    if (!matched) uncovered.push(name);
  }
  return uncovered;
}
