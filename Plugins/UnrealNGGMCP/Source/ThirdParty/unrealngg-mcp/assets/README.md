# Bundled Claude skills & agents

The `ue5-ngg` MCP server installs everything in this folder into the project's
`.claude/` directory on startup (see `../setup-assets.js`). Drop your content
here:

```
assets/
├── manifest.json          # bump `version` whenever you change content below
├── skills/
│   └── <skill-name>/
│       └── SKILL.md        # frontmatter: name, description (the trigger)
└── agents/
    └── <agent-name>.md     # frontmatter: name, description (when to use)
```

The tree under `skills/` and `agents/` is copied verbatim to
`<project>/.claude/skills/...` and `<project>/.claude/agents/...`.

## Install behaviour

- **Version-tracked, never clobbers edits.** A manifest at
  `.claude/.ngg-assets.json` records the hash of every file the server wrote.
  On later runs it installs missing files, refreshes files the user hasn't
  touched when `manifest.json`'s `version` bumps, and leaves user-modified files
  alone. **Bump `version` after editing content here** so unedited installs
  pick it up.
- **Picked up on next restart.** Claude Code loads skills/agents at session
  start, so new files appear after the next Claude Code restart.
- **Force a refresh** with the `ue5_setup_skills` MCP tool.
- **Disable** with `NGG_SKIP_SKILL_INSTALL=1`.
