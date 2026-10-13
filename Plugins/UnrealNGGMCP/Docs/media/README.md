# Chat Window — Tutorial Media

Drop short tutorial clips for the Claude Chat window into this folder. They are referenced from `../ChatWindow.md` (the "Visual tutorials" section).

---

## File naming

Use the exact filenames the doc expects:

```
01-open-tab.gif
02-first-prompt.gif
03-slash-commands.gif
04-at-mentions.gif
05-image-paste.gif
06-permission-prompts.gif
07-bypass-toggle.gif
08-plan-mode.gif
09-selection-toggle.gif
10-bp-selection-banner.gif
11-model-selector.gif
12-effort-selector.gif
13-conversation-persistence.gif
14-cancel-clear.gif
```

For long-form clips (>20 s), use the same number/slug with `.mp4` instead of `.gif`. The doc will pick up either extension if you update the embed line.

---

## Specs

| Property | Target |
|---|---|
| Dimensions | ≤ 1280 × 720 (720p). 960 × 540 is fine for most. |
| Frame rate | 15–24 fps for GIF; 30–60 fps for MP4. |
| GIF file size | ≤ 5 MB. Anything bigger, switch to MP4. |
| MP4 codec | H.264 / AAC. Keep clips under ~5 MB so the repo stays cloneable. |
| Length | 5–15 s for GIF; up to ~30 s for MP4. |
| Audio | None. Captions belong in the doc, not the clip. |

---

## Capture tools

| Tool | OS | Why |
|---|---|---|
| **ScreenToGif** | Windows | Best free GIF recorder. Trim, crop, edit-frame-by-frame. |
| **ShareX** | Windows | One-key region capture, GIF or MP4 output. |
| **OBS Studio** | Windows / macOS / Linux | Best for MP4. Use "Replay Buffer" for retroactive clips. |
| **LICEcap** | Windows / macOS | Lightweight GIF-only. |

**Workflow tip:** record at 1× speed, then in ScreenToGif speed up idle frames (e.g. 2× during typing pauses) so the clip stays under 15 s.

---

## Embedding in the doc

Markdown image works for both GIF and (autoplay-disabled) MP4 in most viewers, but for MP4 prefer the HTML form so GitHub renders inline playback:

```markdown
<!-- GIF -->
![Open the tab](media/01-open-tab.gif)

<!-- MP4 -->
<video src="media/01-open-tab.mp4" controls width="720"></video>
```

If you replace a GIF with an MP4, update the embed line in `ChatWindow.md` to match.

---

## Privacy / hygiene

Before pushing:

- Crop or blur any window titles, file paths, or filenames you don't want public.
- Strip auth tokens or project IDs visible in tool outputs (re-record over them is fastest).
- Don't include CLAUDE.md / `.env` content in any frame.
- Confirm the project path in the title bar isn't sensitive — use a clean test project if it is.

---

## Updating a clip

1. Re-record over the existing file (keep the same name).
2. `git add` and commit — the doc reference doesn't change.
3. If you switch from GIF to MP4 (or vice versa), edit the embed line in `ChatWindow.md` to match.
