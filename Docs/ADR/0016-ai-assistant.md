# ADR 0016: AI Assistant (chat panel and natural-language commands)

- **Status:** accepted. See STATE.md #43.
- **Related:**
  - ADR 0015 §2.5 (sketch plans placed all or nothing, one undo step): the assistant places fixtures through the same plan and builder.
  - ADR 0011 §2.2 (cue requests), ADR 0012 / 0014 (report exports), ADR 0009 (outdoor environment).
  - ADR 0003 and ADR 0010 (undo history; the rules decide undo).
  - Rules.md: Architecture (state by lifetime, request bridge, push not poll), Engineering Standard §20 and §66 (never trust a client value; here the model's output is untrusted), §21 (no tick), §29 (async lifetime), §55 (validate at the trust boundary), §65 (Shipping safety).

## 1. Problem

Gevor's milestone (2026-10-21), "Step 3: AI Conversational Assistant & Natural Language Command Subsystem":

1. A dockable or pop-up **chat panel** (`UStageAIAssistantWidget`) where the user types requests or questions about the stage: equipment counts, power loads, cue lists.
2. **`UStageAICommandSubsystem`** parses requests and maps them to **safe actions** (spawn fixtures, set up cues, change the weather, export reports) **through the existing transaction and rule checks**, and answers scene statistics (weight, power, fixture counts) clearly.
3. `TObjectPtr`, encapsulation, no per-frame tick, tests, zero warnings, STATE.md.

What exists: every stage change already has a validated request on `AModularPlayerController` (cues, exports, undo/redo, sketch plans placed through `StageSketchBuilder`). The outdoor environment is written straight to `UStageEnvironmentSubsystem::SetSettings` by its panel and is **not undoable**.

## 2. Decisions

### 2.1 The model never acts; commands do

A language model is a translator, never an authority. Everything the assistant can do is a closed set of **commands** (`FStageAICommand`: an `EStageAIIntent` plus bounded parameters). Text becomes a command in one of two ways:

- **Local parser** (`StageAICommandParser::Parse`), always available. It is deterministic, offline and pure, and it understands a fixed English grammar (§2.3). It is the default, and it is what the tests cover.
- **Online model** (`UStageClaudeAssistantBackend`), optional and **off by default**. It sends the prompt, a short scene summary and the recent conversation to the Claude Messages API with **one tool**, `stage_command`, whose JSON schema mirrors `FStageAICommand`. The model either calls the tool or answers in plain text.

Whichever way a command arrives, it goes through `StageAICommandSchema::Validate` before it runs: the intent must be known, counts and numbers must be in range, strings are trimmed and capped, and unknown fields are rejected. A tool call that fails validation is reported as "I could not do that" and **never** partially applied. The model's output is treated like untrusted client input (§20, §66).

### 2.2 Layers and ownership

| Piece | Kind | Owns |
|---|---|---|
| `FStageAICommand`, `FStageAIMessage`, `FStageAIInterpretation` (`AI/StageAITypes.h`) | value types | — |
| `StageAICommandParser` (`AI/StageAICommandParser.h`) | pure functions | text → command or "not understood"; fixture item matching; the row layout of added fixtures |
| `StageAICommandSchema` (`AI/StageAICommandSchema.h`) | pure functions | command validation, the tool's JSON schema, the API request body and response parsing |
| `UStageAIAssistantBackend` / `UStageClaudeAssistantBackend` | `UObject` | one online request in flight; nothing else |
| `UStageAIAssistantSettings` | `UDeveloperSettings` (Project Settings › StageCraft AI Assistant) | online model on/off, model id, effort, timeout, history length |
| `UStageAICommandSubsystem` | `UWorldSubsystem` (Game, PIE) | **the conversation** of this stage session (capped transcript), interpretation in flight, read-only answers to questions, streaming the assets of a fixture plan |
| `StageAICommandRunner::Run` | function | runs one action command through the controller's **public request API** |
| `AModularPlayerController::RequestAssistantPrompt` | request bridge | the panel's single entry point |
| `UStageAIAssistantWidget` | view | the text being typed (transient UI state only) |

- The transcript lives in the world subsystem, not in the widget, so closing, floating or re-docking the panel keeps the conversation. It is per stage session and not saved in scene files.
- The subsystem **never changes the stage**. Questions are answered read-only from `UStageSessionSubsystem`, `UShowControlSubsystem` and `UStageEnvironmentSubsystem`. Actions are handed to the executor that `RequestAssistantPrompt` passes in, which calls the runner.
- The runner only calls requests that already exist (or are added here as requests: §2.4), so every action is checked by the GameMode and recorded in the history exactly as when the user clicks the matching button. The runner holds no rules.
- One prompt is in flight at a time (§60). A second one is refused with a message rather than queued.

### 2.3 What the assistant understands

| Intent | Example | Runs |
|---|---|---|
| Help | "help", "what can you do" | — |
| Stage summary | "status", "summary" | read-only |
| Item count, weight, power | "how many items", "total weight", "power draw" | read-only (session totals, against the session limits when there are any) |
| Fixture count | "how many fixtures" | read-only (by fixture type) |
| Count of an item | "how many truss" | read-only (placed items whose name matches) |
| Cue list | "list cues", "how many cues" | read-only |
| Environment | "what's the weather" | read-only |
| Add fixtures | "add 6 moving heads", "place a led par" | a sketch plan of floor fixtures → `RequestBuildPlan` |
| Store a cue | "store cue 5 called Intro fade 3" | `RequestStoreCue` (the next free whole number when none is given) |
| Play cues | "go", "back", "go to cue 2" | `RequestCueGo` / `RequestCueBack` / `RequestGoToCue` |
| Weather | "make it rain", "fog", "clear sky" | `RequestSetEnvironment` (turns the sky on when a sky is installed) |
| Time of day | "set time to 18:30", "sunset" | `RequestSetEnvironment` |
| Sky on / off | "go outdoors", "indoor stage" | `RequestSetEnvironment` |
| Export | "export bill of materials", "export patch sheet", "export cable schedule", "export all reports" | `RequestExport*` |
| Undo / redo | "undo", "redo" | `RequestUndo` / `RequestRedo` |

Destructive whole-stage actions (clear stage, delete, open or save a scene) are deliberately **not** commands: they have confirmation dialogs in the menus, and the assistant answers with where to find them.

**Added fixtures.** "Add N <name>" picks the catalog's lighting fixture whose name or fixture type matches the words (ignoring generic words such as "lights" and "fixtures"). With no specific words it picks the first fixture. N is 1–24. The fixtures are laid out in a row along Y, 1 m apart, centred on the stage centre line. The row is 2 m downstage of the items already placed (or at the origin on an empty stage) and snapped to the 50 cm grid. They become fixture strokes for `StageSketchGenerator::BuildPlan`, so placement, snapping and duplicate handling are the sketch's, and they are placed all or nothing as **one undo step** ("Assistant: add 6 fixtures").

### 2.4 New requests on the controller

- **`RequestBuildPlan(const FStageSketchBuildPlan&, const FText& StepName)`.** The body of the sketch's `HandleSketchAssetsReady` moves here. It runs the builder, records one undo step and reports a refusal. The sketch calls it too, so both paths share one implementation.
- **`RequestSetEnvironment(const FStageEnvironmentSettings&, const FText& StepName)`.** It writes through the environment subsystem and records **`FStageEnvironmentCommand`** (before/after settings) as one undo step. `IStageItemEditor` gains `SetEnvironment` (default `Invalid`, like `SetCueList`). The Environment panel keeps writing directly: a slider drag would otherwise flood the history (follow-up: a panel transaction like the inspector's). Undoing an assistant weather change restores the whole environment as it was before it, including any panel edits made since. This is the same whole-value semantics as cue list steps.

### 2.5 The online model

- **Opt-in.** Off by default (`UStageAIAssistantSettings::bUseOnlineModel`). When it is on, the prompt, the scene summary (item, fixture and cue counts, totals and limits, environment) and the last `MaxHistoryMessages` lines of the conversation are sent to Anthropic. The panel says so in its footer.
- **Credentials.** The API key is read from the `ANTHROPIC_API_KEY` environment variable at request time. It is never stored in config, saves or logs. Without a key the local parser is used, and the panel says why.
- **Request.** Raw HTTP `POST https://api.anthropic.com/v1/messages` (C++ has no official SDK). The headers are `x-api-key`, `anthropic-version: 2023-06-01` and `anthropic-beta: server-side-fallback-2026-07-01`. The body has:
  - `model` (default `claude-opus-5-5`), `max_tokens` 16000, and `output_config.effort` (default `low`: a chat-like translation task);
  - no `thinking` field, so the model runs its default adaptive thinking;
  - `fallbacks: "default"`, so a safety decline is retried server-side on a fallback model;
  - `tools` with the one `strict` `stage_command` tool, and `tool_choice` `auto` with `disable_parallel_tool_use` (forced tool choice is not supported on this model).
- **History.** It is sent as alternating plain-text turns from the subsystem's own transcript. Tool calls and thinking blocks are never replayed, so no earlier turn is edited.
- **Response.** The client checks `stop_reason` before reading `content`. A `refusal` is reported as a decline; `max_tokens` as a failure. One `tool_use` block becomes a command and goes through `StageAICommandSchema::Validate`; otherwise the text blocks become the reply. A transport error, timeout, non-200 status or malformed body **falls back to the local parser** and says so in the reply.
- **Lifetime.** The HTTP callback runs on the game thread and captures the backend weakly. The subsystem cancels the request in `Deinitialize`. No tick: a timeout is the request's own (`FHttpModule` request timeout).
- **Shipping.** The online backend is a product feature and ships, but it is inert unless the project setting is on and a key is present.

### 2.6 Panel

`StageCraft.Panel.Assistant` (`DA_Panel_Assistant`) opens as a floating pop-up window (560 × 720, like the Sketch window). It is reached from Edit › Assistant › AI Assistant..., the Window menu and the viewport toolbar's ASSIST button (next to SKETCH). The panel shows:
- the transcript, as user and assistant bubbles in a scroll box that follows new messages;
- quick-prompt chips ("Stage summary", "Power draw", "Fixtures", "Cues", "Help");
- a text field and SEND (Enter sends);
- a footer naming the interpreter in use (local, or the online model).

It binds `OnConversationChanged` in `NativeConstruct`, unbinds it in `NativeDestruct` and never ticks. While a prompt is in flight, the field is read-only and a "thinking…" line is shown.

### 2.7 Development commands

`StageCraft.Assistant.Ask <text>` (spaces allowed; `-ExecCmds` splits on commas, so prompts with commas cannot be scripted) and `StageCraft.Assistant.Screenshot`. Both are `#if !UE_BUILD_SHIPPING`.

## 3. Alternatives considered

- **Model-only interpretation.** Rejected: it would not work offline or without a key, could not be tested deterministically, and would make an external service a single point of failure for basic commands.
- **Letting the model call many fine-grained tools in an agent loop.** Rejected for this step: one validated command per prompt keeps every action a single, reviewable, undoable request. Multi-step plans ("build a festival stage") are a follow-up that would chain commands through the same validator.
- **Executing commands inside the subsystem.** Rejected: subsystems own state and never take the UI's write path. The controller remains the only bridge from the UI to the game.
- **A Blueprint-authored chat widget.** Rejected for parity with the other panels, which are built in code from the theme.

## 4. Consequences

- Assistant actions cannot do anything the user could not do with the buttons, and they are refused by exactly the same rules.
- Assistant weather and time changes are undoable; Environment panel changes still are not (follow-up).
- The module gains the `HTTP` dependency.
- **Follow-ups:**
  - report export completion back into the chat;
  - multi-command prompts ("add 4 pars and store cue 1");
  - fixture mounting ("hang them on the truss");
  - more languages in the local grammar;
  - saving the conversation with the scene.

## 5. Rollback

Remove `Source/ModularSceneBuilder/*/AI/`, the panel definition and its menu entries, and the two controller requests (re-inline `RequestBuildPlan` into `HandleSketchAssetsReady`). `FStageEnvironmentCommand` can stay; it is unused without the assistant.
