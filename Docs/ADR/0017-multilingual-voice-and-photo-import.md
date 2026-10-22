# ADR 0017: Armenian & English assistant, voice prompts, and photo / scan import for Sketch to Stage

- **Status:** accepted. See STATE.md #44.
- **Related:**
  - ADR 0016 (the AI assistant: commands, validation, the request bridge, the optional Claude model). This ADR extends it; nothing in its trust model changes.
  - ADR 0015 (Sketch to Stage: strokes, the generator, generation through the rules as one undo step). Imported photos only add strokes.
  - Rules.md: Architecture (state by lifetime, the request bridge, push not poll), Engineering Standard §20 and §66 (model and transcript output are untrusted), §21 (no tick), §29 (async lifetime and thread ownership), §55 (validate at the trust boundary), §57 (cooked and staged content), §65 (Shipping safety).

## 1. Problem

Gevor's milestone (2026-10-22), "Multilingual (Armenian & English) Voice/Text Assistant Support and Real-World Photo/Sketch Import for Sketch-to-3D":

1. The assistant (`UStageAICommandSubsystem`, `UStageAIAssistantWidget`) fully processes queries and commands in **Armenian**, using the Claude backend when a key is present.
2. A **microphone / speech-to-text** hook in the assistant panel for spoken prompts.
3. Sketch to Stage accepts **PNG / JPEG** photos of hand-drawn stage diagrams or floor plans; image processing extracts the structure and passes it to `StageSketchGenerator` to rebuild decks and truss in 3D.
4. `TObjectPtr`, encapsulation, no per-frame tick, all tests, zero warnings, STATE.md.

Gevor's choices (2026-10-22, asked before design): voice through **OpenAI transcription**; without a key, an **offline Armenian grammar** with Armenian replies; photos read by a **local edge pipeline with an optional Claude reading**, landing as **editable strokes** before GENERATE 3D.

Facts found first:

- **No engine font has Armenian glyphs.** Roboto, DroidSansFallback and NotoSansThai in `Engine/Content/Slate/Fonts` all lack U+0531..U+0587 (checked with the font files' character maps), so Armenian text drew as boxes.
- **The Claude API takes images, not audio.** Speech needs a separate service; Windows' own speech recognition has no Armenian.
- The engine's capture backend (`AudioCaptureCore`, WASAPI registered by the default-enabled AudioCapture plugin) and image codecs (`ImageWrapper`) work at runtime and in Shipping.

## 2. Decisions

### 2.1 The reply follows the prompt, not the culture

`EStageAILanguage` (English, Armenian) is a field of `FStageAICommand`. `StageAILanguage::Detect` counts Armenian against Latin letters; the online model may override it with `reply_language` (§2.3). Every reply (answers, action outcomes, validation errors, notes) is written in the command's language.

- English stays the localizable source text (`LOCTEXT`); the Armenian rendition is culture-invariant text beside it (`StageAILanguage::Pick(Language, LOCTEXT(...), INVTEXT("…"))`). The culture cannot decide, because one user may ask in Armenian and the next in English while the menus stay in the UI language.
- Not translated: undo step names (they are Edit-menu entries, in the UI language), catalog names, and refusals worded by other systems (GameMode, economy), which an Armenian reply quotes as they are.

### 2.2 Offline Armenian: the same grammar, not a second one

`StageAIArmenian::TranslateToGrammar` rewrites an Armenian prompt into the words the English rules already know, and `StageAICommandParser::Parse` runs them; one set of rules, limits and tests serves both languages.

- Words match by **stem, longest first** (`ավելացր` → add, `կյու` → cue, `ֆերմա` → truss), so Armenian endings (articles -ը/-ն, plurals, cases) need no rules. Short words match exactly (`մեկ` would otherwise read `մեկնարկ` as 1).
- Number words and ordinals become digits; `5-րդ` reads 5; an ordinal before `կյու` moves after it.
- `՞` (written inside a word) and question words make a question; «» and "" keep a label as typed.
- A number next to a part of the day becomes a clock time (`ժամը 8 երեկոյան` → 20:00).
- Unknown Armenian words are **kept**, so `ավելացրու 2 վիշապ` is refused ("no fixture matches") instead of silently adding the first catalog fixture.
- Every example of the Armenian help text is a test case.

### 2.3 Online Armenian

The `stage_command` tool gains an optional `reply_language` (`en` / `hy`, validated; anything else is refused). The system prompt says users write English or Armenian (also in Latin letters), that transcripts may contain recognition errors, and that `item` must use the catalog's English words. Without the field the prompt's detected language is used. The model's free-text replies are already in the user's language.

### 2.4 Armenian on screen

`StageCraftWidgetStyle::MakeFont` builds one composite font: a copy of the engine's Roboto composite plus an **Armenian sub-font** (U+0530..U+058F, U+FB13..U+FB17) from **Noto Sans Armenian** Regular / Bold (SIL Open Font License 1.1, `OFL.txt` beside the files), lazily loaded. Every code-built panel and the input fields go through `MakeFont`, so prompts, replies and item text render everywhere.

- The files live in `Source/ModularSceneBuilder/Resources/Fonts` and are staged by the module (`RuntimeDependencies`, UFS), the way plugins ship non-asset files. Under `Content/` the editor's auto-import (`bAutoCreateAssets`, on by default) would turn them into font assets.
- Missing files: the engine font is used and a warning is logged once.

### 2.5 Voice prompts

| Piece | Kind | Owns |
|---|---|---|
| `UStageVoiceInputComponent` | component on `AModularPlayerController` (one local player's tool) | the state Idle → Recording → Transcribing → Idle, the status line, the length-limit timer |
| `FStageVoiceRecorder` | plain class (private), owned by the component | the open microphone stream (`Audio::FAudioCapture`) and the samples under a lock |
| `UStageSpeechToTextBackend` / `UStageOpenAITranscriptionBackend` | `UObject`, owned by the component | one HTTP request in flight |
| `StageSpeechAudio` | pure functions | resampling (box filter down), loudness, 16-bit WAV, the multipart/form-data body, the response |

- **Off by default** (Project Settings › StageCraft AI Assistant › Voice Input); key from `OPENAI_API_KEY` at request time, never stored or logged; nothing is recorded before MIC is pressed; the microphone closes when recording stops, the window closes or play ends.
- The capture callback (device thread) only appends mono samples under a lock, up to the limit; Stop / Abort close the stream before returning. The limit is a timer with a handle, cleared on every way out of Recording. No tick.
- Recordings under 0.3 s or below an RMS of 0.003 are not sent. Each one goes out at 16 kHz mono (≈ 1 MB per 30 s), with a bilingual stage-vocabulary hint, and the language fixed or detected per the settings.
- **The transcript is untrusted text:** it goes into the prompt field to be checked (Enter sends it), or straight through `RequestAssistantPrompt` when the settings say so. It is interpreted and validated exactly like typed text.

### 2.6 Photo / scan import: strokes, not stage

`UStageSketchSubsystem::ImportImage(Path, Reader)` adds what a drawing shows to the **sketch** as ordinary strokes, using the canvas's own conventions. It never touches the stage: the user checks the strokes against the photo, corrects them with the usual tools, and generates through the rules as before (ADR 0015 §2.5).

- **Local reading** (`StageSketchImageTrace`, pure and deterministic):
  1. **Ink.** An adaptive threshold: each pixel is compared with its neighbourhood mean from an integral image, so shadows and uneven light do not become ink. A dark image is read as light lines on dark. A 3 × 3 closing bridges pencil gaps; specks are removed.
  2. **Decks.** Background regions enclosed by ink (4-connected) are traced along their edge (Moore neighbour tracing) and simplified (Ramer–Douglas–Peucker). A deck inside another stands one deck height higher (a riser).
  3. **Fixtures.** Small compact marks (filled dots, or small circles) become fixtures at their centre.
  4. **Truss.** Remaining ink, less a band one line width around each deck, is thinned (Zhang–Suen) and followed from end to end or junction. Spurs are dropped and runs simplified, so a truss leaving a deck's edge survives.
  5. **Scale.** The image is the chosen **photo width** (5–100 m, a sketch setting) wide, centred on the plan origin, image up = plan +X.
- **Text and smudges** that are neither lines nor dots are left out and counted. Limits: 32 decks, 64 runs, 96 fixtures; the sketch's own 256-stroke cap and `SanitizeStroke` apply again.
- **Trust boundary.** The file is untrusted. Only .png, .jpg and .jpeg are accepted, up to 64 MB. The format comes from the content, and the dimensions from the header before decoding (≤ 12000 px, ≤ 60 MP); alpha is flattened onto white.
- **Threads.** Reading, decoding and tracing run on a worker (`UE::Tasks`) that touches only a plain job struct. The result returns as a game-thread task and is applied only if its serial number is still current, so a cancelled, replaced or orphaned import is dropped. `Deinitialize` bumps the serial and cancels the online request.
- **Reference photo.** A transient `UTexture2D` owned by the subsystem, drawn at 40 % under the grid where the strokes were placed; SHOW / REMOVE.

### 2.7 Optional Claude reading of a photo

With the online model on, READ › CLAUDE sends the image (JPEG, ≤ 1568 px) with one strict tool, `sketch_plan`. The tool takes decks as corner lists with an optional height, truss runs as point lists with closed, and fixtures, all in image fractions. Effort is never below medium.

- The answer is untrusted. Unknown fields are refused. Non-finite points, and points more than 2 % outside the image, are dropped; nearer ones are clamped. Elements with too few points are dropped, counts are capped, and every drop is counted in the message.
- Placement is the same as the local reading, so both line up with the reference photo.
- **The local reading is always made too and is the fallback:** on a decline, a text answer ("not a stage plan"), an empty plan or a transport error, its strokes are added and the message says why.

### 2.8 Development commands (compiled out of Shipping)

- `StageCraft.Assistant.Voice start|stop|cancel|status|devices`: `devices` lists the microphones without opening one.
- `StageCraft.Sketch.Import <path> [claude]`, `StageCraft.Sketch.Set PhotoWidth <cm>`, and the photo and import state in `StageCraft.Sketch.Status`.

## 3. Alternatives considered

- **A second, Armenian grammar.** Rejected: two rule sets would drift apart and double the tests. A lexicon onto one grammar keeps one behaviour.
- **Switching the game's culture to Armenian per reply.** Rejected: the culture is global (menus, numbers), and replies must follow each prompt.
- **Windows dictation, or a local Whisper build.** Windows has no Armenian. Whisper means a native third-party library and hundreds of MB of model; it is possible later behind the same `UStageSpeechToTextBackend` interface.
- **Building 3D straight from the photo.** Rejected: reading drawings is uncertain, so the result must be checked and corrected first, and the sketch already offers that (and the same rules and undo).
- **Claude-only reading.** Rejected as the only path: it needs a key and the network, and it sends the image away. The offline pipeline always works and is the fallback.
- **Fonts as `UFontFace` assets under `Content/`.** It would work, but it needs an editor import step. Files under the module, staged with it, keep the font a code dependency.

## 4. Consequences

- New module dependencies: `AudioCaptureCore` and `ImageWrapper` (both engine runtime modules). The AudioCapture plugin is listed explicitly in the .uproject (it was on by default).
- The Noto files and their licence ship with every build.
- Settings in DefaultGame.ini, all off by default. Turning one on sends data out: voice recordings to OpenAI, images to Anthropic.
- Known limits:
  - The Armenian lexicon is finite: unusual wording falls back to "not understood", and the online model handles free Armenian.
  - Phone photos with EXIF rotation are read as stored (ImageWrapper does not rotate them).
  - Perspective (a photo taken at an angle) is not corrected.
  - A closed truss loop drawn on paper reads as a deck, the same convention as the canvas.
  - Text on the drawing can produce stray fixture marks (letters such as "o").
  - The microphone path and both online services are verified by hand (no key or recording on the build machine).

## 5. Rollback

Each part is separable:

- The language layer is a lexicon plus `Pick` calls; removing Armenian means removing the `INVTEXT` branches.
- Voice is one component, its backend and the MIC button.
- Photo import is an additive subsystem API, the PHOTO row and the reference drawing. No save format, asset or map changed.
