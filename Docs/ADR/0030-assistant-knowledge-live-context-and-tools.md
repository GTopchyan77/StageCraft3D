# ADR 0030: Assistant v2: knowledge, live context, diagnostics and tools

- **Status:** drafted 2026-11-05 (STATE.md #67). Gevor's Phase H brief (2026-11-06) asks for H0–H4 "fully aligned with ADR 0030", which is taken as approval of this design. Still open and his to decide: the default online model (§8) and the key policy for a shipped product (§9).
  - H0 spike (STATE.md #72, §13): streaming confirmed on the engine's HTTP module against a local mock; the paid measurement per model is a dev command for Gevor (no key on the build PC).
  - H1 implemented (STATE.md #72, §14): knowledge index, anchors on seven panels, 18 help articles in English and Armenian, offline search, SHOW ME, assistant docked on the right.
  - H2 implemented (STATE.md #73, §15): show diagnostics and the Problems panel, "why is it dark", assistant intents explain_fixture and list_problems.
  - H3 implemented (STATE.md #74, §16): tool catalogue, patch / update cue / delete cue tools, plans as proposal cards, one undo step with rollback, several online tool calls as one plan. The bounded online read loop, history replay and the vault key are H3b.
  - H4 implemented (STATE.md #75, §17): several changes typed in one prompt as one plan (English and Armenian), conversation memory ("them", "do it again on group 2"), fixture targets by ID and group, tools for the selection, palettes, groups and effects.
- **Amends:**
  - ADR 0016 §2.1 (one command per prompt → a bounded tool loop; the model still never acts);
  - ADR 0016 §2.5 (history replay);
  - ADR 0023 §7 (the assistant's key moves into the vault).
- **Absorbs:** Phase 26 in `tasks/todo.md` (offline knowledge base, multi-step, memory; Gevor 2026-10-30). That phase pointed at "ADR 0022", which became the DCC bridge; this ADR is its home.
- **Related:** ADR 0017 (Armenian and voice), ADR 0019 (DMX), ADR 0025–0028 (console), ADR 0029 (audio).
- **Rules.md:** request bridge, §20 / §55 / §66 (the model's output is untrusted input), §29 (async lifetime), §60 (one prompt in flight), §65 (Shipping).
- **Roadmap:** `Docs/Roadmap/2026-11-05-realism-audio-assistant.md`, Phase H.

## 1. Problem

Gevor's brief (2026-11-05) asks for an assistant inside the console that:
- knows every button, command, fixture parameter, cue store rule, DMX value and panel control;
- answers in real time and explains operations step by step;
- diagnoses patch and show errors;
- makes live fixture and preset adjustments on command.

What exists (ADR 0016, #43–#45, #49):
- A chat panel, an offline English grammar with Armenian mapped onto it, and voice prompts.
- About 30 intents, each run through the controller's requests with undo.
- An optional Claude backend with one `stage_command` tool, **one command per prompt**, a short scene summary and no knowledge of the UI.
- **Gevor's standing decision (2026-10-30):** the free offline engine is the full default, and Claude only extends it.

## 2. Decision: the model never acts, commands do (kept)

ADR 0016 §2.1 stands:
- Every effect on the stage is a validated command run through `AModularPlayerController`'s public requests, judged by the GameMode and recorded in the history.
- The model, local or online, only proposes.
- Nothing in this ADR gives the assistant a path the user's buttons do not have.

**Never exposed to the assistant:** purchases, the wallet, profile or entitlements; credentials; file deletion; scene open, save or overwrite; Clear Stage; dev console commands; DMX OUT on or off (a live rig).

## 3. Decision: knowledge generated from the code, not written twice

**`FStageKnowledgeIndex`** is read-only and built once at startup by `StageKnowledge::Build`. Each entry has an id, a kind, a title, a body, keywords, a language, and an optional UI anchor and command.

| Kind | Source of truth | Example entry |
|---|---|---|
| Command | `StageKeyBindings` command table plus the user's current chords | "Blackout: Ctrl+Shift+B, or BLACKOUT in the Playback panel" |
| Control | **UI anchor registry** (new, below) | "Patch › DMX OUT › PRI: sACN priority 0–200, default 100" |
| Parameter | `StageParameterTypes` tags, ranges and units; which fixture models offer each | "Zoom: 4–60° on Beam 280" |
| Rule | Help articles `Content/StageCraft/Help/<lang>/*.md`, written from the ADRs (tracking, store modes, HTP/LTP, palettes, patch, DMX in/out, effects, audio) | "Cue Only: the change applies to this cue and the next cue restores…" |
| Panel | `DA_Panel_*` definitions and menu paths | "Effects panel: Window › Effects…" |

**UI anchors** (`FStageUIAnchor {PanelTag, ControlId, Label, Tooltip, HelpTopic}`):
- Each panel declares its anchors in one static table next to its build code, and the control's tooltip is read from the same entry, so help and tooltip cannot drift.
- Slate widget addresses are never used: they are runtime-only and unstable.
- An anchor lets a reply carry a **SHOW ME** button: it opens the panel and pulses that control (§7).

**Search.** `StageKnowledgeSearch` is pure and offline: BM25 over keyword tokens, English and Armenian, with the Armenian lexicon of ADR 0017. It answers "how do I…" and "where is…" with no network. It is Phase 26's `StageAIFeatureSearch`.

**Shipping.** Help articles are staged as UFS. Dev console commands are not indexed, because they do not exist in Shipping.

## 4. Decision: live context through read tools, never a dump

Nothing about the live show is put into the system prompt. The model asks through **read tools**, each bounded to about 4 KB of output and run on the game thread from current state:

| Tool | Returns |
|---|---|
| `get_stage_summary` | today's summary (items, fixtures, cues, totals, environment) |
| `find_fixtures {query, limit}` | ids, names, models, patch, selection state |
| `get_fixture {id}` | parameters with values and ranges; patch and options; **why it looks like this** (§5) |
| `get_patch {universe}` | addresses, footprints, conflicts |
| `get_dmx {universe, from, count≤64}` | the rendered output values the show would send |
| `get_cues`, `get_playbacks`, `get_effects`, `get_palettes`, `get_groups` | lists with ids and the settings that matter |
| `get_io_status` | DMX IN and OUT state and counters; audio source, BPM and signal (ADR 0029) |
| `run_diagnostics {scope}` | the findings of §5 |
| `search_help {query}` | the top entries of §3 |

Read tools are also what the offline engine uses to answer, so local and online answers agree.

## 5. Decision: diagnostics and "why" are pure code, useful without AI

**`StageShowDiagnostics::Run(const FStageShowSnapshot&)`** returns `FStageDiagnostic {Severity, Code, Message, Subjects, SuggestedCommand?}`.

Codes:
- PatchConflict, FootprintOverflow, Unpatched (a fixture used by a cue or playback but not patched);
- DMXInNoSource, DMXInTimedOut, DMXOutFailed, OutputPausedUnderExternal, EffectsHeldUnderExternal;
- PaletteReferenceMissing, CueNamesMissingFixture;
- BlackoutOn, GrandMasterLow, AudioNoSignal;
- ShadowBudgetFull, and more as found.

They are deterministic and tested. They also feed a **Problems** list in the Patch and Cues panels, which works without the assistant.

**`StageFixtureExplain::Explain(Fixture, Attribute)`** walks the output mix layers for one fixture and attribute and says which layer set the final value:
1. home look;
2. cue (which cue, tracked from which cue);
3. playback (HTP or LTP winner);
4. programmer;
5. effect (which one, audio mode);
6. highlight;
7. masters.

"Why is fixture 12 dark?" is answered exactly from this, for example "Grand Master is at 0 %" or "Cue 4 tracks dimmer 0 from cue 2". The model only phrases it.

## 6. Decision: action tools and a bounded loop

**Action tools.** There is one action tool per request family, each a strict JSON schema validated by `StageAICommandSchema` before anything runs:
- `set_fixture_values {fixtures, values, fan}` → programmer (existing `set_fixture`);
- `apply_palette`, `store_cue {number, name, mode}`, `update_cue`, `go / back / go_to_cue`;
- `start_effect {quick or settings, audio}`, `stop_effect`;
- `select {fixtures | group}`, `record_group`, `record_palette`;
- `patch_fixtures {assignments}` (through `RequestPatchFixtures`), `set_environment`, `apply_atmosphere`;
- `add_fixtures`, `move / replace / delete_selected` (existing);
- `undo / redo`, `export_report`.

Each maps onto existing controller requests. New requests appear only where a button already exists without one.

**The loop:**

1. Read tools run at once.
2. Action tools are **collected**, not run, until the model ends its turn.
3. The plan is shown as a **proposal card** in the chat, listing the steps in words with APPLY and CANCEL (the IMPORT CSV pattern).
4. Plans that only touch the programmer of selected fixtures, playback (GO and BACK) or the camera apply at once. The user can switch that off. Every other plan waits for APPLY.
5. APPLY runs the steps in order through the requests inside **one history transaction**, so the plan is one undo step: "Assistant: <summary>". It stops at the first refusal, reports which step failed and why, and rolls back the earlier steps of that plan (the `RequestPatchFixtures` rollback pattern).

**Limits:**
- at most 8 model turns and 24 tool calls per prompt;
- at most 32 steps per plan;
- one prompt in flight (§60);
- a prompt can be cancelled from the panel.

**Offline:**
- The local grammar gains multi-step prompts ("add 4 pars and store cue 1"), split into the same plan type.
- Conversation memory: "them" means the last fixtures affected, and "do it again on group 2" works.
- The same proposal card is used.

## 7. Decision: UI

- **Reply buttons:** SHOW ME (anchor), APPLY / CANCEL (plan), OPEN PANEL. Answers that come from help articles cite the article title.
- **Pulse:** a soft, 1.5 s highlight on the anchored control (a Slate overlay on the panel, no tick after it ends).
- **Footer:** names the interpreter (local or the online model) and what is sent (§8).
- **Panel host:** it can dock in the Lighting Desk layout as well as float.

## 8. Decision: the online model (amends ADR 0016 §2.5)

The wire facts below were checked against the current API reference on 2026-11-05.

**Transport:** raw HTTP to the Messages API (C++ has no official SDK), with the headers and `fallbacks: "default"` as today.

**Model:**
- Default `claude-opus-5-5` at `output_config.effort: "low"` (chat).
- A setting may pick `claude-sonnet-5-5` or `claude-haiku-5-5` for latency or cost. That is Gevor's product call. H0 measures time to first token and cost per prompt for each.

**Tools on the wire:**
- `strict: true` on every tool.
- `tool_choice` stays `auto`: forced tool choice is a 400 on Opus 5.5 and Sonnet 5.5.
- Parallel read calls are allowed; their results return together in one user message.

**Prompt caching:**
- The request order is tools, then system, then messages.
- The tool list (sorted, deterministic) and the system prompt (role, rules of §2, a compact digest of the knowledge index: panel and command names, not articles) are byte-stable, with a `cache_control` breakpoint after the system prompt.
- Nothing volatile goes before it: no time, no scene state.
- H0 checks `usage.cache_read_input_tokens` on the second prompt.

**History:**
- Append-only. The assistant's content blocks, including thinking and `tool_use` blocks, are kept and sent back unchanged, both inside a loop and across turns.
- This replaces ADR 0016's "tool calls and thinking blocks are never replayed". Current models check that earlier turns were not edited (preserved thinking), and an edited history can be refused.
- A conversation past about 40 turns starts fresh with a short local summary, rather than being trimmed.

**Streaming (H0):** SSE streaming of the reply text, if the engine's HTTP module delivers the response body incrementally (`SetResponseBodyReceiveStreamDelegate` or equivalent; to be confirmed). Otherwise it stays non-streaming with "thinking…".

**Validation:** with eager tool-input streaming, tool inputs are validated against the schema before use, and a truncated input is a failed tool result, never a partial command.

**Stop reasons** are checked before content: `refusal` reports a decline, `max_tokens` and `pause_turn` are handled, and any transport failure falls back to the local engine and says so.

**Tool search** (`defer_loading`) is not needed for about 20 tools. It is reconsidered past 40.

**Sent to the service:** the prompt, the stable system prefix and tool results. Never file paths, user or machine names, keys or the scene file. The footer and the Preferences page say so.

## 9. Decision: credentials

- The Anthropic key moves into the credential vault (ADR 0023): a "Claude assistant" row in Services & Keys. `ANTHROPIC_API_KEY` stays as a developer fallback in non-Shipping builds only.
- **A shipped product cannot carry a StageCraft-owned key in the client** (§66). For paying users the choice is a bring-your-own key (as now) or a StageCraft proxy server that holds the key and meters use. That decision is Gevor's, and it is out of scope here.

## 10. Tests (planned; no network in any test)

| Test | Proves |
|---|---|
| `StageCraft.Assistant.Knowledge` | the index is built from the key map, anchors, parameters and articles; every anchor has a label and tooltip; no dev command is indexed in a Shipping-configured build |
| `StageCraft.Assistant.Search` | an EN/HY corpus of about 120 questions gets its expected top-3 article; ranking is stable |
| `StageCraft.Show.Diagnostics` | each code fires on a built world and not on a clean one |
| `StageCraft.Show.Explain` | each layer is reported as the winner in a built mix (home, tracked cue, HTP playback, programmer, effect, highlight, GM, blackout) |
| `StageCraft.Assistant.ToolSchema` | every tool rejects unknown fields, out-of-range values, wrong types, huge arrays and NUL bytes |
| `StageCraft.Assistant.Loop` | a scripted mock backend: read then act; a plan is one undo step; a refusal stops and rolls back; auto-apply versus confirm; turn and tool limits; cancel; `refusal`, `max_tokens` and transport-failure fallbacks |
| `StageCraft.Assistant.MultiStep` | the offline splitter and the memory ("them", "again") |

## 11. Alternatives considered

- **Injecting the whole UI and show state into every prompt.** Rejected: it costs tokens on every prompt, breaks caching on every change, and goes stale between tool calls. Read tools fetch exactly what is asked, when it is asked.
- **Letting the model run console commands.** Rejected: dev commands are compiled out of Shipping and bypass the request bridge.
- **Embeddings and a vector store** for help search. Rejected for now: the corpus is a few hundred entries, BM25 works offline in two languages, and embeddings would need a model download or a network call.
- **Executing each action as the model emits it.** Rejected: there would be no single undo step, and no chance to confirm a large change.

## 12. Consequences and rollback

- New Private `AI/Knowledge`, `Show/Diagnostics` and `UI` anchor tables; help articles as content; a Problems list.
- The Claude backend changes from one tool to a loop. The offline engine stays the default and grows with Phase 26.
- **Rollback:** setting the online model off returns the panel to the local engine. Diagnostics and help search stand alone.

## 13. H0 spike results (STATE.md #72)

**Streaming works on the engine's HTTP module.** UE 5.8 has `IHttpRequest::SetResponseBodyReceiveStreamDelegateV2`, called on the HTTP thread as body bytes arrive (the body is then not kept in the response). Measured against `Scripts/Assistant/MockSseServer.ps1`, which sends one SSE event every 150 ms:
- 9 chunks over 1.32 s and 1.29 s in two requests, one per event, so the body is handed over as it arrives and not at the end;
- time to the first byte 255 ms and 165 ms, first text 737 ms and 650 ms (mostly the mock's own delays);
- the parser read `message_start` usage (cache write 1800, then cache read 1800), the text deltas, `message_delta` (stop reason, output tokens) and skipped the keep-alive comment.

**What H3 uses from it:**
- `FStageSseParser` (Private/AI/StageSseParser): a pure SSE splitter (LF, CRLF, CR, comments, multi-line data, an event closed without a blank line, UTF-8 cut anywhere). The HTTP thread feeds it; events go to the game thread.
- The request shape: tools, then one system block (the role and rules, then the digest of §3) with `cache_control` ephemeral, then the messages. The digest is 2.7 K characters and byte-stable (a test builds it twice).

**Not measured here: latency, cache hits and cost per model.** The build PC has no `ANTHROPIC_API_KEY`, and the measurement spends money. `StageCraft.Assistant.Spike Model=all Runs=2` (development builds only) sends the real request twice to each of `claude-opus-5-5`, `claude-sonnet-5-5` and `claude-haiku-5-5`. It logs time to first byte, first block and first text, the chunks, cache write and read tokens and the cost, and appends a row per request to `Saved/Profiling/AssistantSpike.csv`.
- Expected cost: well under $0.10 for all six requests (the request is 13 KB of JSON, roughly 3 K tokens of prefix).
- Prices used: per million tokens, input / output / cache read. Opus 5.5 is 4 / 20 / 0.20, Sonnet 5.5 is 2 / 10 / 0.20, Haiku 5.5 is 0.10 / 0.50 / 0.01. A cache write is 1.25 × input. Haiku's cache-read price is the usual 0.1 × input, because the table used does not list it.
- The minimum cacheable prefix on these models is 512 tokens, so the prefix should cache. The second request's `cache_read_input_tokens` shows it.
- `NoFallbacks` drops `fallbacks: "default"`. Haiku 5.5 has no server-side fallback, so the first run will show whether the field is refused for it.

## 14. H1 implementation notes (STATE.md #72)

- **Index** (`AI/StageKnowledge`, `AI/StageKnowledgeSubsystem`, a game-instance subsystem):
  - 175 entries: 34 commands, 68 controls, 21 panels, 16 fixture attributes (with the catalog models that offer each and their ranges), and 18 articles × 2 languages.
  - Built in about 7 ms on the first question, and again after the item or panel catalog changes (delegates; nothing polls).
  - A command entry's chord is filled in when it is shown, from the asking player's key map, so a rebound key is answered correctly.
- **Anchors** (`UI/StageUIAnchors`):
  - Playback, Cues, Effects, Audio, Patch, Groups and Palettes each declare a static anchor table, and their build code takes the label and tooltip of those controls from it.
  - `IStageAnchoredPanel::FindAnchorWidget` resolves an id to the live widget. All 68 resolve in a running game (`StageCraft.Assistant.CheckAnchors`).
  - Anchors per playback point at playback 1.
- **Search** (`AI/StageKnowledgeSearch`):
  - BM25 (k1 1.2, b 0.75); title and keywords weigh 3, the body 1.
  - English: light stemming. Armenian: the endings are stripped until none is left, and `-ության` and `-ությամբ` fold into `-ություն`. An Armenian prompt is also read through the ADR 0017 lexicon.
  - Each id is reported once, in the asker's language when an article has a translation. Below a score of 2 nothing is answered.
  - 93 questions (65 English, 28 Armenian) find their answer in the top three.
- **Routing** (`StageKnowledgeSearch::Route`):
  - The knowledge answers what the grammar does not understand, and "help" with a topic.
  - A how-to question the grammar reads as an action ("how do I store cue 5") is **KnowledgeOnly**: it is answered from the knowledge, or with "I changed nothing", and is never run. With the online model on, how-to questions the knowledge can answer are still answered offline.
  - Stage questions ("how many fixtures", "what is the total weight") keep their live answers.
- **UI:**
  - A knowledge reply carries an `FStageAIReference` (panel, control, source title). The chat shows SHOW ME (or OPEN PANEL) and "From: <title>".
  - `UStageWorkspaceSubsystem::ShowControl` opens the panel, looks for the control for up to 0.5 s, scrolls it into view, and pulses it. A control in a collapsed section (TAP without a selected effect) is not pulsed, and that is logged.
  - The pulse blinks render opacity three times over 1.5 s, and swells compact controls by 12 %. It is not the Slate overlay §7 describes: a dimmed blink needs no change to the panels' trees. It shows in Slate window captures (the engine's screenshot does not show it).
  - The Default and Lighting Desk layouts dock the assistant on the right: below the Inspector column, and between Groups/Palettes and the Inspector. It still floats when dragged out.
- **Not indexed:** development console commands. Tests check that no entry or digest mentions one.
- **Articles:** `Content/StageCraft/Help/en|hy/<id>.md`, staged as UFS in the pak (`DirectoriesToAlwaysStageAsUFS`). No packaged build has been made yet to confirm they load there.

## 15. H2 implementation notes (STATE.md #73)

- **Mix explanation** (`Show/StageShowMix`): `ExplainAttribute(Attribute, Layers)` mirrors `ResolveAttribute` step by step and reports the winning layer (home, cue list, playback with its index, programmer, highlight), whether an effect changed the value and whether the masters scaled it. `StageCraft.Show.Explain` checks that both give the same value in every case. `UShowControlSubsystem::BuildLayers` now builds the layers for both writing and explaining, so the explanation cannot drift from the output; `FindSettingCue` names the cue a value tracks from (`StageCueTracking::FindSettingCue`).
- **Diagnostics** (`Show/StageShowDiagnostics`, pure): `Run(FStageShowSnapshot)` returns `FStageDiagnostic {Severity, Code, Place, FixtureIds, Cues, Playback, Universes, Error, Level}`, worst first, then in code order. Codes as §5, plus `DMXInFailed`, `UniverseMuted`, `AudioSourceFailed`, `AudioNoSource`, `AudioKilled` and `ProgrammerHolds`; `FootprintOverflow` is part of `PatchConflict` (the patch subsystem already reports a fixture that runs past channel 512 as a conflict), and `ShadowBudgetFull` was left out (a performance setting, not a fault). `Unpatched` fires only while DMX goes in or out, so a previs-only stage is not nagged. Missing fixtures and palettes are one finding per cue list (main, or each playback) listing the cues.
- **Place, not panel:** a finding names an `EStageDiagnosticPlace` (Patch, DMX IN, DMX OUT, Cues, control source, Playback, GM, Blackout, Audio, KILL, programmer, Effects); `StageUIAnchors::GetPlaceTarget` maps it to a panel and anchor, so the show code stays free of UI. Every place maps to a real anchor (tested).
- **Why is it dark** (`Show/StageFixtureExplain`, pure): `Explain(FStageFixtureFacts, Language)` walks DMX IN (under External), Blackout, the Grand Master, the layer that set the dimmer (with the tracked-from cue, or the playback's name and fader), an effect, colour (the mix's luminous fraction) and iris, and names the first cause (`EStageDarkCause`). Separately it names what keeps a real fixture from following (`EStageOutputCause`: DMX OUT off, failed or paused, unpatched, muted universe, conflict). The text gives the verdict first, then one line per step, in English or Armenian.
- **Subsystem** (`UStageShowDiagnosticsSubsystem`, world, Game and PIE): gathers the snapshot and one fixture's facts from the show, patch, programmer, library, DMX network and audio subsystems. It listens to their delegates; while someone listens to `OnProblemsChanged` it runs once on the next frame after any change (an FTSTicker one-shot, removed in Deinitialize) and broadcasts only when the findings differ. With no listener nothing runs until `GetProblems`. The music's signal is not pushed; REFRESH and the assistant re-run on demand.
- **Problems panel** (`UI/StageProblemsPanel`, `StageCraft.Panel.Problems`, `DA_Panel_Problems`): one row per finding (severity, sentence); a click selects the fixtures concerned through `RequestSelectItems` and calls `ShowControl` on the place's anchor. A tab in the Lighting Desk layout (next to Audio) and Window › Problems. §5 put the list in the Patch and Cues panels; one panel that covers every place replaced that.
- **Assistant:** intents `explain_fixture {fixture_id}` and `list_problems` (questions; read-only). "why is fixture 12 dark", "why is light 3 not responding", "explain fixture 7", "why is it dark" (the selected fixture), "any problems", "what's wrong", "check the show"; in Armenian "ինչու է 12 սարքը մութ", "ինչ խնդիրներ կան", "ինչն է սխալ". A why-question about no fixture in particular still goes to the help article. The reply carries SHOW ME at the cause's control (or OPEN PANEL on Problems for a list).

## 16. H3 implementation notes (STATE.md #74)

- **Tool catalogue** (`AI/StageAIToolCatalogue`, pure): one entry per intent, in enum order, with its wire name, what it touches (`EStageAIToolEffect`: none, fixtures, playback, cues, patch, stage, environment, files, history, marketplace), whether the history records it, and whether it may be one step of a plan. `StageAICommandSchema` takes the wire names from it (the schema's own table is gone), the subsystem the plan rules, the card its lines (`DescribeStep`, English and Armenian). `GetNeverExposed` names what §2 forbids; a test checks no tool matches one.
- **New tools** where a button had no assistant path: `patch_fixtures {fixture_ids, universe, address}` (the selected fixtures when no IDs; packed one after another from U.AAA by `UStageDMXPatchSubsystem::PlanConsecutive`, applied through `RequestPatchFixtures`; already patched there is not a refusal), `update_cue {cue_number, label, fade_seconds}` (`RequestUpdateCue`, what is left out keeps its value) and `delete_cue {cue_number}` (`RequestDeleteCue`). Grammar in English and Armenian: "patch fixture 3 to universe 2 address 101", "patch fixtures 1 to 4 at 2.001" (also "1-4"), "patch the selected fixtures to universe 2", "delete cue 3", "rename cue 2 to Finale", "set cue 4 fade to 2.5 seconds"; «պատչիր 1-4 սարքերը 2 յունիվերսում», «ջնջիր կյու 3». The other §6 tools (`apply_palette`, `start_effect`, `select`, `record_group`, ...) are H4 with the multi-step grammar.
- **Plans** (`FStageAIPlan`): the actions of one answer. `CheckPlan` allows 1 to 32 changes; undo, redo, export and the marketplace only ever run alone (an undo inside a plan's group would be refused; a paid listing keeps its own yes turn).
- **When a plan asks** (§6 as built): every step that may apply without asking (cue playback; controls of the *selected* fixtures) applies at once. Otherwise a plan from the online model, or of several steps, is shown as a **proposal card**. A single change typed to the local grammar runs at once, like its button, unless the player switched **ASK FIRST** on in the assistant's header (per PC, `UStageCraftUserSettings`, off by default); then every change waits, playback included.
- **The card** is an assistant line with a `ProposalId`: the numbered steps, a note when cue playback is in it (not undone), and APPLY / CANCEL while it is the open proposal. A typed yes / apply / կիրառիր or no / cancel answers it (`ParseConfirmation`); any other prompt drops it unapplied. APPLY and CANCEL go through `AModularPlayerController::RequestAssistantProposal`.
- **One undo step** (`UStageEditHistorySubsystem::BeginGroup / EndGroup / AbortGroup`): while a group is open every recorded step is collected, whichever request recorded it (cue list, patch parameters, programmer, environment, placements), and undo / redo are refused (the controller says the assistant is still applying). Kept: one step, named "Assistant: <first step> (+N more)", or the step itself when there was only one. The component's edit transaction is ended first, so a fixture-control step still merges per item inside it. A plan still open when play ends is recorded; a scene load drops it.
- **Rollback:** the first refused step stops the plan; `AbortGroup` undoes what the group collected, newest first, all or nothing (`FStageBatchCommand`), and records nothing. If the rules refuse the rollback, what was done is recorded as one step so Ctrl+Z can still revert it, and the reply says so. Cue playback a plan started is not undone, and the reply says that too. A step that waits (fixtures streaming their assets) holds the plan, with the group open, until `FinishPendingAction`; a step that finishes inside its own call is handled too.
- **Online:** several `stage_command` calls in one answer are one plan (`disable_parallel_tool_use` is no longer sent; the tool description and system prompt say how). One invalid call, or an answer cut off by `max_tokens`, runs nothing. Questions among the calls are answered; the changes go to the card.
- **Bug fixed on the way:** `HandleBackendFinished` called `SetState(Idle)` before `Respond`, which drops the asking player's executor, so every action of the online model was answered "There is no player to make that change for". The online path was never run with a key, so it was not seen; `StageCraft.Assistant.OnlinePlan` now drives it with a scripted backend.
- **Not in H3 (moved to H3b in `tasks/todo.md`):** the bounded read/act loop with read tools, append-only history replay with thinking blocks, the cached prefix in the live backend and the key in the credential vault (§4, §8, §9). The panel cannot cancel a running plan yet.

## 17. H4 implementation notes (STATE.md #75)

- **Offline multi-step** (`StageAICommandParser::ParsePlan`, pure): a typed prompt is split at "and", "then", commas, semicolons, full stops and line breaks (Armenian և, ու, հետո, ապա, իսկ and ։), never inside quotes. Each part is read by the same grammar (`Parse`, Armenian through `StageAIArmenian` per part); a part that is not a change on its own joins its neighbour ("delete the spots and the washes" stays one delete), and a fixture control without a target of its own joins the control before it ("set the spots to red and zoom wide" stays one step). Two or more changes that may share a plan become its steps, in the prompt's language; anything else (a question, undo, export, the marketplace) leaves the prompt to `Parse` as before. A later fixture step that names no fixtures continues on the fixtures the step before acted on ("select group 2 and set dimmer to 50%"). At most 48 parts are read (`MaxPlanClauses`); `CheckPlan` still caps a plan at 32 steps. The plan is the H3 plan: a card unless every step may apply without asking, one undo step, rollback on the first refusal.
- **Conversation memory** (`UStageAICommandSubsystem`, tool state like the conversation, never saved, emptied with it and with the world): every change that succeeds is remembered with the fixtures it acted on (`FStageAIActionOutcome::AffectedFixtureIds`, filled by the runner for add, set, select, patch, palettes, groups and effects; added fixtures are the placed items that were not there before the build) and the player's selected fixtures right after it (`FStageAIActionHost::SelectedFixtures`).
  - **"Them"** (`EStageAITarget::Recent`: them, they, those, it; Armenian դրանք, դրանց, նրանք, նրանց) is resolved when the step runs, so the second step of "add 4 pars and make them blue" colours the pars just placed. It means the remembered fixtures unless there are none or the player has selected different fixtures since; then the selection. "This", "these" and "the selected" stay the selection. "Them" never targets a delete, move or replace (the schema refuses it): a remembered fixture is never removed by a pronoun.
  - **"Again"** (`EStageAIIntent::Repeat`, local only: the online model names the change itself and cannot send it): the last change as it ran, on a new target when one is named ("do it again on group 2", «նորից արա 2 խմբի համար»). Only fixture tools take a new target; "again" alone repeats any plannable change. Resolved before validation, so the repeated command is validated like any other.
  - A plan that is rolled back restores the memory it found, so "them" never names fixtures its rollback removed.
- **Targets of the fixture tools** (`bTargetsFixtures` in the catalogue): the selection, placed fixtures by name, fixture IDs (`Fixtures`: "fixture 3", "fixtures 1 to 4", "lights 1, 2 and 5", Armenian «1-4 սարքերը»; a range whose end carries a unit is a value, so "fixture 3 to 50%" is fixture 3 at 50 %), a group by name or number (`Group`), or "them". Tools that work on the selection (palettes, groups, effects) select their target first; a selection is tool state and not an undo step, so a rollback leaves it.
- **New tools** (`select_fixtures`, `apply_palette`, `record_palette`, `delete_palette`, `record_group`, `delete_group`, `start_effect`, `update_effect`, `stop_effect`), each through the request its panel button uses (`RequestSelectItems`, `RequestApplyPalette`, `RequestRecordPalette`, `RequestDeletePalette`, `RequestRecordGroup`, `RequestDeleteGroup`, `RequestStartEffect`, `RequestUpdateEffect`, `RequestStopEffect`), so the GameMode judges them and the history records them; inside a plan they are part of its one undo step. Pools are named by name or number (`StageAICommandParser::ResolveName`: whole name, else the one name holding every word, else the place in the pool; several matches are listed back, never guessed). Effects are the quick effects of the Effects panel by name ("chase" is the Dimmer Chase before the Beat Chase), with an optional rate (Hz or BPM) and size; `update_effect` changes the programmer's running effects (one a cue runs is changed after EDIT in the Effects panel), `stop_effect` stops by name or all (STOP ALL, one step). New catalogue effects: `Selection` (applies without asking, never undone, no card warning) and `Library` (palette and group pools, asks).
- **What applies without asking** (§6 as built in H3) now also covers a selection, and palettes and effects on the selected fixtures or on "them". Palettes, groups or effects on fixtures named by words, IDs or a group wait for APPLY when they are part of a card.
- **Online schema:** the new intents, `target` fixtures / group / recent, and the fields `group`, `palette`, `palette_kind`, `effect`, `rate_hz`, `effect_size`; validation refuses an empty pool name, an unknown palette kind, rates past 25 Hz, sizes past 270, IDs below 1, a group target without a group, and `recent` on deletes, moves and replaces.
- **Not in H4:** fixtures added to or taken out of an existing group ("remove the spots from group 2" is not read as deleting the group), renaming palettes and groups, per-fixture palettes, effect settings beyond rate and size, and the online read loop (H3b).
