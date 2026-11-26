# ADR 0016: AI Assistant (chat panel and natural-language commands)

- **Status:** accepted. See STATE.md #43; §6 amendment (STATE.md #114: bill of materials answers); §7 amendment (STATE.md #115: export completion in the chat); §8 amendment (STATE.md #117: fixtures hung on the truss).
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
- Assistant weather and time changes are undoable; so are Environment panel changes since STATE.md #116 (ADR 0009 §9).
- The module gains the `HTTP` dependency.
- **Follow-ups:**
  - report export completion back into the chat (done, §7);
  - multi-command prompts ("add 4 pars and store cue 1"; done in H4, STATE.md #75, ADR 0030 §17);
  - fixture mounting ("hang them on the truss"; done, §8);
  - more languages in the local grammar;
  - saving the conversation with the scene.

## 5. Rollback

Remove `Source/ModularSceneBuilder/*/AI/`, the panel definition and its menu entries, and the two controller requests (re-inline `RequestBuildPlan` into `HandleSketchAssetsReady`). `FStageEnvironmentCommand` can stay; it is unused without the assistant.

## 6. Amendment: bill of materials answers (STATE.md #114, 2026-11-26)

**Measured before** (`-game` run `Bom114Before`, Concert starter stage, local interpreter): the BOM panel's report had 15 lines, 92 units, 3,519 kg and 16.4 kW, but the assistant answered none of these questions from it.
- "what is on the bill of materials" and "նյութերի ցուցակը" got the help article that names the panel.
- "what do I need to rent" and "show me the BOM" were not understood.
- "list the equipment" got the cue list article.
- "how many truss sections do I need" answered "None of the 26 placed items match "truss need"", although the stage holds 16 sections in 2 runs.

**Decision:**
- **New question `bill_of_materials`** (`EStageAIIntent::BillOfMaterials`, read-only, answered from the live stage like `total_weight`). The online model gets the same tool name, so it reaches the same answer.
  - **Words that trigger it:** bom, materials, inventory, rent / rental / hire, pick list, packing list, equipment list, gear list, "list the equipment", and Armenian նյութ… (→ materials).
  - **Words that don't:** "where / open / panel / window / find" are left to the help articles, which name the panel. "export …" stays an export, and "how many …" stays a count.
- **The answer gives** the totals (units, lines, weight, power) and each discipline's lines in rental units ("16 × F34 Truss Run (2 m sections)"). It shows at most four lines per discipline ("and N more") and ends by pointing to the export for the full PDF and CSV. It uses English and Armenian frames; group labels and catalog names stay as the report has them.
- **Single source of truth:** `FStageAISceneView::BillOfMaterials` is a copy of `UStageBillOfMaterialsSubsystem::GetReport()`, so the chat, the panel and the export give the same numbers.
- **Counts give rental units.** A count answer adds "In rental units: …" for matched items whose units differ from their placements (spans in sections, decks in panels). Count prompts drop need / rent / hire / to / order / panel(s) / segment(s), so "how many truss sections do I need" counts "truss".

**Not in this amendment:** export completion in the chat (§4 follow-up; done in §7).

## 7. Amendment: export completion in the chat (STATE.md #115, 2026-11-26)

**Measured before** (`-game` run `Export115Before`, Concert starter stage): "export the bill of materials" and "export all reports" wrote their PDF + CSV pairs 157–375 ms after the request. The conversation's only word on them was "the status bar shows when each file is written", and the status bar message is gone a few seconds later.

**Decision:**
- **The runner says what started.** `FStageAIActionOutcome::StartedExports` lists the reports whose `RequestExport*` returned true. A refused report is already explained in the reply.
- **The subsystem waits for those, and only those.**
  - `UStageAICommandSubsystem` keeps `AwaitedExports` (report → the prompt's language) and binds the three report subsystems' `OnExportFinished` for its lifetime (`InitializeDependency`, unbound in `Deinitialize`).
  - Each report subsystem exports one file pair at a time, so its next result after a started export is that export.
  - An awaited result adds one line: "The bill of materials is written: X.pdf and X.csv in Saved/StageCraft/Reports." A failed write names the path and why it may have failed.
  - Results the assistant did not start (the panels, the menu) add nothing; the status bar reports them as before.
- **The conversation is not blocked.** The export step is done when it starts (not Pending), so the player can keep asking while files are written. The line arrives whenever the worker finishes.
- **Bubbles break long words.** File names are single words wider than a docked panel, so assistant bubbles use `AllowPerCharacterWrapping`, which breaks a word only when it does not fit on a line.

**Alternatives considered:**
- **Making the export step Pending until the files exist.** Rejected: it would hold the assistant busy for no gain, and "export all reports" would need to wait for three results inside one step.
- **Copying the report subsystem's own `Message`.** Rejected: it is English only and phrased for the status bar.

## 8. Amendment: fixtures hung on the truss (STATE.md #117, 2026-11-26)

**Measured before** (`-game` run `Hang117Before`, Concert starter stage: two F34 truss runs at 7 m, front at X = −600 and back at X = 400, with nine Beams already hung):
- "hang 4 pars on the truss" and "add 2 moving head spots to the truss" were read as plain adds; "truss" was dropped as a word.
- The fixtures were placed on the floor at X = −2300 / −2500, behind the FOH desks. The reply said "in a row on the floor".
- Only Sketch plans hung fixtures, and only from truss drawn in the same sketch.

**Decision:**
- `FStageAICommand::Mount` (`EStageAIFixtureMount`): Floor (default), Truss, FrontTruss or BackTruss.
  - Local grammar: "truss" / "trusses", or hang / hung / fly / flown without floor / ground / deck, means a truss.
  - back / rear / upstage picks the back run; front / downstage the front one.
  - Those words are kept out of the fixture query.
  - Armenian reaches it through the lexicon (կախ → hang, ֆերմա → truss; since STATE.md #118 առջև- / առաջի → front and հետևի / հետևից / հետին → back, whole words only for back because հետևեն / հետևում also mean "follow").
- Online tool: a `mount` field (floor, truss, front_truss, back_truss), refused when unknown. Proposal cards say where they hang ("Add 2 spots on the back truss").
- Runner: every truss run on the stage, most downstage (lowest X, towards the audience) first.
  - Straight truss spans of category `StageCraft.Category.Truss` give their path (the bottom chord, as the Sketch window hangs fixtures at a span's height).
  - Curved truss is sampled along its arc half a profile below its origin.
- Pure layout (`StageAICommandParser::FindFreeTrussSpots`, `SpreadOverSpots`):
  - a spot every 50 cm along the run, measured over the whole run so corners get one spot;
  - spots within 60 cm (and 150 cm vertically) of a fixture already there are left out;
  - the fixtures go at the centres of equal shares of the free spots.
- Which run:
  - "the truss" takes the first run, from the front, with room for them all;
  - front and back name one run;
  - not enough room is refused with the number that fits.
- The fixtures hang upside down (roll 180°) like the Sketch window's. They are placed as one plan through `RequestBuildPlan`: every fixture is judged by the rules, and Ctrl+Z removes them all.
- The reply names the run ("hung from the front truss, spread along it"). The floor reply is unchanged.

**Alternatives considered:**
- Snapping to the nearest truss from the floor row. Rejected: the floor row lies in front of everything, far from any truss.
- Asking which run every time. Rejected: "the truss" has an obvious default, and front / back pick another.
- Moving `FindTrussMount` from the Sketch generator. Rejected: it projects a drawn marker, not an even spread over the free length.

**Verified** (`Hang117After`, same stage):
- **Pars:** 4 hung at Y −700, −200, 200, 700 on the front truss, between the Beams.
- **Back truss:** 2 Profile 600 at Y ±400.
- **Armenian:** «կախիր 2 լույս ֆերմայից» hung 2 Beams at ±450 on the front truss, with an Armenian reply.
- **Refusal:** "hang 20 pars on the front truss" answered "room for 4 more".
- **Undo:** Ctrl+Z removed the last add as one step.
- **Tests:** `StageCraft.AI.TrussHang`.
