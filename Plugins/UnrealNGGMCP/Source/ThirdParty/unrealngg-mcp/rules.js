// Copyright 2025-2026 NGG. All Rights Reserved.
// rules.js — unrealngg-mcp
//
// Splits UE5_NGG_RULES.md into a small always-on core and a set of on-demand
// MCP resources.
//
// Why: the whole file (~63 KB) used to be sent as the server's `instructions`
// on every handshake, landing in the model's system context before any work
// happened. Together with the tool schemas that is ~60k tokens per session.
// Most of the bulk — GAS, Behavior Trees, profiling, the Blueprint schema
// reference — only matters once the user actually works on that subsystem, so
// it moves to resources the model reads when relevant.
//
// The file on disk stays a single canonical document (rule 9 calls it the
// contract, and the docs link to it); the split happens at load time.

// Section boundaries, in the order they appear in the document. Everything
// before the first anchor is the core. Each entry runs until the next anchor.
//
// `anchor` is matched as a line prefix, so renaming a heading's tail is safe
// but changing its number or opening words is not — see splitRules()'s
// all-or-nothing fallback for what happens then.
const SECTION_SPECS = [
  {
    anchor: "### 6. Custom events vs override events",
    slug: "blueprint-authoring",
    title: "Blueprint authoring rules",
    description:
      "Rules 6-11: custom vs override events, _DoubleDouble math operators, function-name lookups, splitting graphs into small functions, and Blueprint code style. Read before authoring or editing Blueprint graphs.",
  },
  {
    anchor: "### 12. AI logic",
    slug: "ai-behavior-trees",
    title: "AI: Behavior Trees and Blackboards",
    description:
      "Rule 12: authoring Behavior Trees, Blackboard keys, decorators and services. Read before using any ue5_bt_* tool.",
  },
  {
    anchor: "### 13. Gameplay Ability System",
    slug: "gas",
    title: "Gameplay Ability System (GAS)",
    description:
      "Rule 13: attribute sets, abilities, effects and ASC wiring for single player, multiplayer and AI. Read before using any ue5_gas_* tool.",
  },
  {
    anchor: "### 14. Performance profiling",
    slug: "profiling",
    title: "Performance profiling with Unreal Insights",
    description:
      "Rule 14: capturing and analysing traces. Read before using any ue5_profile_* tool.",
  },
  {
    anchor: "## Asset creation tools (cheat sheet)",
    slug: "asset-cheatsheet",
    title: "Asset creation cheat sheet and verification loop",
    description:
      "Which tool creates which asset type, and how to verify a change actually landed in the editor.",
  },
  {
    anchor: "## Blueprint authoring schema reference",
    slug: "blueprint-schema",
    title: "Blueprint authoring schema reference",
    description:
      "Node types, the `defaults` field, connection format, the 3-step component idiom, pin-name patterns, and common pitfalls. The precise payload schema for ue5_bp_add_logic / ue5_bp_add_node.",
  },
];

export const RESOURCE_URI_PREFIX = "ue5-ngg://rules/";

/** Find the line index a section starts at, or -1. */
function findAnchorLine(lines, anchor) {
  for (let i = 0; i < lines.length; i++) {
    if (lines[i].startsWith(anchor)) return i;
  }
  return -1;
}

/**
 * Split the rules document.
 *
 * On any structural surprise — a heading that moved or was renamed — this
 * returns the whole document as the core with no sections, i.e. exactly the
 * old behaviour. Losing a rule silently would be far worse than paying the
 * tokens, so the degradation is deliberate and reported via `complete`.
 *
 * @param {string} markdown - contents of UE5_NGG_RULES.md
 * @returns {{core: string, sections: Array<{slug,title,description,uri,text}>, complete: boolean, missing: string[]}}
 */
export function splitRules(markdown) {
  const lines = markdown.split(/\r?\n/);

  const found = SECTION_SPECS.map(spec => ({
    ...spec,
    uri: `${RESOURCE_URI_PREFIX}${spec.slug}`,
    line: findAnchorLine(lines, spec.anchor),
  }));

  const missing = found.filter(s => s.line < 0).map(s => s.anchor);
  // Anchors must also appear in the documented order; out-of-order means the
  // document was restructured and our slicing would mix unrelated rules.
  const ordered = found.every((s, i) => i === 0 || s.line > found[i - 1].line);

  if (missing.length > 0 || !ordered) {
    return { core: markdown, sections: [], complete: false, missing };
  }

  const sections = found.map((spec, i) => {
    const end = i + 1 < found.length ? found[i + 1].line : lines.length;
    return {
      slug: spec.slug,
      title: spec.title,
      description: spec.description,
      uri: spec.uri,
      text: lines.slice(spec.line, end).join("\n").trim() + "\n",
    };
  });

  const coreBody = lines.slice(0, found[0].line).join("\n").trim();
  return {
    core: `${coreBody}\n\n${buildIndex(sections)}`,
    sections,
    complete: true,
    missing: [],
  };
}

/**
 * The pointer list appended to the core. Without it the model has no way to
 * know the detailed rules exist at all — the resources would just never be read.
 */
function buildIndex(sections) {
  const rows = sections
    .map(s => `- \`${s.uri}\` — ${s.description}`)
    .join("\n");

  return `---

## Detailed rules (read on demand)

The rules above always apply. The rest of this contract is served as MCP
resources so it does not occupy context until it is relevant. **Read the
matching resource before working on that subsystem** — these are rules, not
background reading, and the tools assume you followed them.

${rows}

Fetch one with the MCP \`resources/read\` request for the URI shown.
`;
}
