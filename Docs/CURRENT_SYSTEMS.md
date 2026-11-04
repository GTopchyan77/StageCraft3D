# StageCraft 3D: current systems

Updated 2026-11-04 after STATE.md #63. This is a quick reference to what exists, how well it works, and where it lives. It was audited on 2026-10-20 at commit `960f5da`. Evidence, file:line references and the risk list are in `Docs/PROJECT_AUDIT.md`.

**Legend**

| Status | Meaning |
|---|---|
| ✅ Verified | An automation test passed (46/46 on 2026-10-20). |
| 🟢 Works (static) | The code path is complete but has not been run in this audit. |
| 🟡 Partial | Some of the feature exists. |
| 🔴 Broken | The code has a defect. |
| ⚪ Absent | Not implemented. |

No feature has a person-confirmed run recorded in STATE.md.

Paths are relative to `Source/ModularSceneBuilder/` (`Public/` and `Private/` share the same folder names).

## Application shell
| System | Status | Where |
|---|---|---|
| Docked workspace: tabs, floating windows, detachable viewport. Standalone and packaged only; PIE uses `WBP_StageCraftHUD`. | 🟢 | `Workspace/StageWorkspaceSubsystem`, `StageWorkspaceShell`, `SStageWorkspaceMenuBar`; `Player/StageCraftGameEngine` |
| Layouts and `Session.json` | ✅ store / 🟢 shell | `Workspace/StageLayoutStore` |
| Startup (#58, ADR 0024): StageCraft's loader as the engine's loading screen during the map load, then an in-app overlay with real progress from the catalogs' streamable handles (Ready, Failed or Timed out with Continue); welcome screen with the starter stages | ✅ progress rules / 🟢 screens (scripted runs; cold start needs a hand check) | `Startup/*`, `Workspace/SStageWelcomeDialog` |
| Panels: Library, Inspector, Fader Bank, Outliner, Patch, BOM, Render, Environment, Cues, Preferences, Sketch to Stage, AI Assistant | 🟢 | `UI/*Panel*`, `Content/StageCraft/UI/Panels/DA_Panel_*` |
| Status bar, viewport toolbar (6 sections, the last opens Sketch to Stage and the AI Assistant; collapses to a strip with « / », Ctrl+Shift+T, remembered, #45) | 🟢 | `UI/StageStatusBarWidget`, `UI/StageViewportToolbar` |
| Input. Mouse buttons and fly axes use Enhanced Input, built in code (no IMC or IA assets). Keyboard shortcuts come from the user's key map: exact chords, looked up on key press (#40). | ✅ key map rules / 🟢 dispatch (scripted run R40_RunC) | `Player/ModularPlayerController` (`InputKey`, `ExecuteCommand`, `RebuildCameraMapping`), `Input/StageKeyBindings` |
| Preferences (#40): key bindings (24 commands, two slots each, set by pressing the keys in the Preferences panel), look speed and invert, and viewport and audio preferences in one panel | ✅ model and storage / 🟢 panel (mouse check pending) | `Input/StageInputPreferencesSubsystem`, `UI/StagePreferencesPanel`, `Settings/StageCraftUserSettings` |
| Modal dialogs, colour picker, hover highlight, box select, remappable mouse buttons | ⚪ | — |

## Scene and CAD
| System | Status | Where |
|---|---|---|
| Placement: ghost, stamping, rules-checked spawn | ✅ math / 🟢 tool | `Placement/StagePlacementToolComponent`, `Components/SpawnSystemComponent` |
| Selection: click, Ctrl, all, from Outliner or Patch. No tests. | 🟢 | `Components/SelectionComponent` |
| Delete, Clear Stage | 🟢 | `History/StageEditHistoryComponent` |
| Duplicate / copy-paste | 🟢 Duplicate (Ctrl+D, STATE.md #57); copy-paste ⚪ | ADR 0013 §5 |
| Gizmo move, rotate, scale. World space only. | ✅ scale rules / 🟢 drag | `Actors/ModularTransformGizmo` |
| Group transform | ✅ math | `Interaction/StageGroupTransform`, `StageGroupTransformComponent` |
| Numeric transform in the Inspector (a spin-box drag is one undo step, STATE.md #37) | 🟢 drag needs a mouse check; transaction ✅ | `UI/StageParameterRowWidget`, `UI/StageParameterViewWidget`, `History/StageEditHistoryComponent` (BeginTransaction) |
| Grid and angle snap, object snap, guides | ✅ | `Placement/StageSnappingComponent`, `StageSnapMath`, `Viewport/StageViewMath` |
| Projections (Perspective, Top, Front, Back, Left, Right), ortho pan and zoom | ✅ | `Viewport/StageViewportComponent` |
| View modes: Lit, Unlit, CAD Lines, Lighting Only, Detail Lighting, Reflections | 🟢 | `Player/StageCraftGameViewportClient`, `Viewport/StageLineViewCameraModifier` |
| Reference grid: one translucent quad with `M_StageGrid`, analytic anti-aliased lines that fade with density, stable under TSR (#48, ADR 0008 §4.4) | ✅ surface math (StageCraft.Viewport.Grid) / 🟢 measured: 7.0 % → 0.14–0.30 % unstable pixels (`StageCraft.View.MeasureStability`, R48) | `Viewport/StageGridActor`, `/Game/StageCraft/Viewport/M_StageGrid` |
| Outliner: discipline groups, hide, lock. No user groups. | ✅ model | `Outliner/StageOutlinerModel`, `UI/StageOutlinerPanel` |
| Spans: path, closed loop, sag. Points cannot be edited after placement. | ✅ math | `Actors/StageSpanActor`, `Placement/StageSpanMath`, `StageSpanToolComponent` |
| Decks / risers of any simple outline (procedural prism, Height parameter, panels as quantity; saved through SaveGame, sanitized on restore) (#42). Outline not editable after placing. | ✅ geometry, restore repair / 🟢 visuals | `Actors/StageDeckActor`, `Data/StageDeckData`, `Placement/StageDeckMath` |
| Sketch to Stage (#42): 2D window (freehand, stage shape, truss line, fixture, erase; stabiliser, grid snap), plan (decks, truss on whole sections, fixtures hung / on decks / on the floor), GENERATE 3D through the rules, all or nothing, one undo step. Sketch not saved in scenes; no 3D preview. Photo / scan import (#44): PNG or JPEG read into editable strokes (local edge pipeline, or Claude with the local reading as fallback), the photo shown under the canvas, a chosen real width. | ✅ plan and generation / 🟢 window (mouse check pending) | `Sketch/*`, `UI/StageSketchCanvasWidget`, `UI/StageSketchSurfaceWidget`, `ModularPlayerController::RequestGenerateFromSketch` |
| AI Assistant (#43, #44): chat panel; local English grammar plus an offline Armenian lexicon onto it, replies in the prompt's language, Armenian glyphs through a Noto Sans Armenian sub-font (#44); MIC voice prompts through OpenAI transcription (off by default, key from OPENAI_API_KEY; #44); (questions on items, weight, power, fixtures, cues, environment; add fixtures, store / play cues, weather, time, sky, exports, undo / redo; delete, move and replace the selected or named items, and build round / curved risers, stairs and curved truss (#45, ADR 0018 §6); fixture controls: dimmer, colour, gel, zoom, frost, iris, CTO, prism, gobo, spin, strobe, pan, tilt (#49, ADR 0020 §2.8)), optional Claude model (off by default, key from ANTHROPIC_API_KEY); every command validated and run through the controller's requests (rules, undo). Weather / time / sky changes from the assistant are undoable. | ✅ commands and answers / 🟢 panel (manual check pending) / ⚪ online model untested live | `AI/*`, `UI/StageAIAssistantWidget`, `ModularPlayerController::RequestAssistantPrompt` |
| Undo/redo (100 steps) | ✅ core | `History/*` |
| Undo coverage | 🟡 transforms, place, delete, span shape, deck height, stairs and curved truss shape, cue edits, a whole sketch generation or assistant build (one step), an assistant move or replacement (one step), fixture look edits through the programmer, Clear and group edits (#59), palette apply, record, update, rename and delete (#60), cue timing and block (#61), playback record, clear and rename (#62), effect start, edit (one step per drag), stop and Clear (#63), effect stop over a cue, take-over (EDIT) and stores that move effects into cues and playbacks (#64) | `ModularPlayerController.cpp:1018-1052` |
| Undo respects locks and rules (every item of a group step; insane steps dropped) | ✅ (#37) | `Game/StageCraftGameModeBase::EvaluateTransformChange`, `StageEditHistoryComponent` (TransformValidator) |
| Camera as in the Unreal Editor 5.8 (#45, #47, ADR 0018 §2, §2.6): LMB fly-and-turn, RMB look + WASDQE, MMB pan, LMB+RMB vertical pan, Alt+LMB orbit, Alt+RMB dolly (up = away, as in UE), Alt+MMB orbit pan, wheel dolly 96 cm, RMB+wheel speed ±10 % (saved), F frames the selection. Raw mouse counts (Mouse2D sensitivity 1), applied at once; flight is the editor's integrator; ticks only while flying or gliding to a focus | ✅ rules + input config (StageCraft.Camera) / 🟢 real Slate drags (`StageCraft.View.MouseDrag`, R47) / mouse feel by hand pending | `Player/StageCameraNavigation`, `Player/StageCameraNavigationComponent`, `Player/StageCameraPawn` |
| Stairs (#45): procedural steps up to a deck, rise / width / handrails as undoable parameters | ✅ geometry, restore repair, undo / 🟢 visuals | `Actors/StageStairsActor`, `Data/StageStairsData`, `Placement/StageProceduralShapes` |
| Curved truss (#45): arcs and circles with chords and bracing on the true curve, counted in 2 m curved sections, radius / angle as quantity parameters | ✅ geometry, quantity prediction, undo / 🟢 visuals | `Actors/StageArcTrussActor`, `Data/StageArcTrussData`, `Placement/StageProceduralShapes` |

## Lighting, rendering, environment
| System | Status | Where |
|---|---|---|
| Lighting fixture (#49, ADR 0020): dimmer, colour (RGB/RGBW/RGBA/RGBWA emitters, CMY + CTO, colour wheel, gel frame), CTC, zoom with beam/field angle, frost, iris, rotating (index + spin) and static gobo wheels, prisms (spinning), framing shutters or barndoors, pan/tilt, strobe, per-cell colour or dimmer (pixel bars, blinders) → one spot light + light function + lens / cell glow. Applies only the outputs a change drives; the light function only while something is projected (#39); spins on the material's clock (no tick) | ✅ output path, mix, cones, prism, gels, cells, cues (Lighting.FixtureSuiteActor, BeamShapingOnDemand, PanTiltTransform, FadeChangedOnly) / 🟢 visuals (scripted run) | `Actors/LightingFixtureActor` |
| Fixture data and DMX layout: capabilities → attributes, console-order layouts with cell blocks, validation, body and dimensions (#49) | ✅ (Lighting.FixtureSuiteData, FixtureData) | `Data/LightingFixtureData`, `Show/StageFixtureCells` |
| Colour science: additive emitters, subtractive filters, CTO mired shift, brightest colour match (#49) | ✅ (Lighting.ColorScience) | `Show/StageColorScience` |
| Gel library: 118 Lee / Rosco gels (sRGB approximations), lookup by code, number or name, gel picker in the inspector (#49) | ✅ (Lighting.GelLibrary) | `Show/StageGelLibrary`, `Show/StageLightingSettings`, `Scripts/Content/Data/StageGels.csv`, `/Game/StageCraft/Lighting/DT_StageGels` |
| Fixture library: 11 generic models (spot, wash, beam, hybrid, moving profile, RGBWA par, strobe, 4-cell blinder, 16-pixel bar, ellipsoidal, fresnel) with dimensions, weight, power, flux and DMX modes (#49) | ✅ (Lighting.FixtureLibraryAssets) / 🟢 look | `/Game/StageCraft/Fixtures/DA_Fixture_*`, `Scripts/Content/create_fixture_library_content.py` |
| Photometrics (display only): beam and field angle, candela, throw, illuminance, beam size; frost, prism, gobos, colour mix included (#49) | ✅ | `Show/StagePhotometry` |
| Beam visualisation | 🟡 relies on the level's volumetric fog; no cone mesh | `Content/StageCraft/Maps/L_StageTest` |
| Fixture shadow budget (#45, ADR 0018 §4): the 64 most important fixtures (flux at dimmer ÷ distance²) cast beam shadows, re-ranked on change (0.25 s, hysteresis 25 %), a fixture keeps its shadow state at least 1 s after a switch (#48), Project Settings › StageCraft Performance | ✅ selection and hold rules / ⚪ frame-time gain not measured yet | `Show/StageShadowBudget`, `Subsystems/ShowControlSubsystem` |
| IES profiles, pan/tilt speed, multiple DMX modes per model, per-cell beams (a multi-cell fixture casts one light) | ⚪ | — |
| Still renders | ✅ rules and framing / 🟢 capture | `Render/StageRenderSubsystem`, `UI/StageRenderPanel` |
| Render cancel, view-mode match, path tracing, scalability | ⚪ | — |
| Outdoor sky and weather (Sky Creator backend); packaged and measured (ADR 0010 §7) | ✅ | `Environment/*`, `Source/StageCraftSkyCreator/*` |
| Atmospheres (#58, ADR 0024): four presets (Clear Daytime, Dramatic Sunset, Cyberpunk Night, Minimalist Studio) as data assets; lighting source Venue / Outdoor / Studio; studio rig (HDRI sky light, key light, backdrop haze, flat fallback); camera look (exposure, bloom, AO, white balance, saturation, contrast, vignette) on the live view and renders; 2 s blends, source switch inside an exposure dip; measured | ✅ blend rules / 🟢 world | `Environment/*`, `UI/StageEnvironmentPanel` |

## Show control and DMX
| System | Status | Where |
|---|---|---|
| Cue list: store / re-record, update, delete, Go, Back, Go To, crossfade (writes only the attributes that change, #39), Internal/External; GameMode rules, undo, saved in scenes (v3) | ✅ rules, history, file, fade / 🟢 panel (mouse check pending, #38) | `Subsystems/ShowControlSubsystem`, `UI/StageCuePanel`, `History/StageCueCommands`, `Game/StageCraftGameModeBase` (EvaluateCueRequest) |
| Programmer and output mix (#59, ADR 0025): each fixture's output is mixed from its home look, the playing cue, the programmer and the highlight by UShowControlSubsystem (the only writer); look edits from the inspector, faders and assistant go into the programmer for every selected fixture, spread by the fan (Linear, V, Inverted V); Clear, Highlight, undoable programmer edits; a cue store takes the programmer in; saved in scenes (v6) | ✅ mix and fan rules, layers on fixtures, undo, file (Show.OutputMix, Show.Programmer, Scene.ShowData) / 🟢 selection spread and panel (scripted run R59a) | `Show/StageShowMix`, `Subsystems/StageProgrammerSubsystem`, `Subsystems/ShowControlSubsystem`, `Player/ModularPlayerControllerProgrammer.cpp`, `History/StageShowCommands` |
| Fixture groups (#59, ADR 0025): record from the selection (order kept), select, update, rename, delete, one undo step each, saved in scenes (v6); Groups panel with the programmer bar (CLEAR, HIGHLIGHT, FAN) | ✅ rules, pool, undo (Show.FixtureGroups) / 🟢 panel (mouse check pending) | `Subsystems/StageShowLibrarySubsystem`, `UI/StageGroupsPanel` |
| Palettes (#60, ADR 0028): Intensity, Position, Color, Beam, Focus and All pools; recorded from the selection globally (a colour palette keeps one colour each fixture mixes its own way, a wheel fixture by its nearest slot) or per fixture; applied as references, so cues stored with them follow a re-recorded palette; delete turns references into values; ten starter palettes; saved in scenes (v7); Palettes panel | ✅ rules, references through programmer and cues, undo, file (Show.PaletteRules, Show.PaletteReferences, Scene.PaletteData) / 🟢 apply, record, update, delete, undo, save and reload on the Concert stage (scripted runs R60a, R60b); mouse check pending | `Show/ShowControlTypes` (StagePaletteRules), `Subsystems/StageShowLibrarySubsystem`, `Subsystems/ShowControlSubsystem`, `Player/ModularPlayerControllerProgrammer.cpp`, `UI/StagePalettesPanel` |
| Cue timing and tracking (#61, ADR 0026): delay, In/Out, feature times, Follow/Wait chains, S-curve; cues hold only their changes and track to the next block; Track, Cue Only and Look stores; Block; scene v8; Cues panel columns and detail line | ✅ tracking, timing, chains, store modes, file (Show.CueTracking, Show.CueTiming, Scene.CueTimingData) / 🟢 Concert stage run R61a; mouse check pending | `Show/StageCueTracking`, `Subsystems/ShowControlSubsystem`, `UI/StageCuePanel` |
| Playbacks and masters (#62, ADR 0026 §6): ten playback faders, each with its own tracking cue list (REC from the programmer, GO, FLASH, REL, CLR); intensity HTP, the rest LTP by activation; Grand Master and Blackout on every light (Ctrl+Shift+B); scene v8; Playback panel; Lighting Desk layout | ✅ mix, playbacks, masters, file (Show.PlaybackMix, Show.Playbacks, Scene.PlaybackData) / 🟢 Concert stage run R62a; mouse check pending | `Show/StageShowMix`, `Subsystems/ShowControlSubsystem`, `UI/StagePlaybackPanel` |
| Effects engine (#63, ADR 0027): sine, cosine, square, ramps, triangle, random, circle and figure 8 on intensity, position, colour and beam; phase, spread, wings, blocks, direction and size fan; effect layer above the programmer; effects in the programmer (undo, Clear, scene v9); Effects panel with quick effects, live preview and TAP; Bench.Fixtures Effects=1 or 2 | ✅ maths, rules, mix, programmer effects, file (Show.EffectWaves, Show.EffectSpread, Show.EffectRules, Show.EffectMix, Show.Effects, Scene.EffectData) / 🟢 Concert stage run R63a; mouse check pending; effects in cues and playbacks: next row (#64) | `Show/StageEffects`, `Show/StageEffectRunner`, `Subsystems/StageProgrammerSubsystem`, `UI/StageEffectsPanel` |
| Effects in cues and playbacks (#64, ADR 0027 §5): cues and playback cues hold effects and stops, tracked (Track, Cue Only, Look, Block); effects fade in and out with the cue timing and morph between cue settings; the programmer stops or takes over (EDIT) a cue effect; playback effects scaled by the fader; FX column, sources and EDIT in the panels; scene v10; Bench.Fixtures Effects=3 | ✅ tracking, fades, morph, sources, file (Show.EffectTracking, Show.CueEffects, Scene.CueEffectData) / 🟢 Concert stage runs R64a, R64b; benchmark A/B against 065d0cf; mouse check pending | `Show/StageCueTracking`, `Show/StageEffectRunner`, `Subsystems/ShowControlSubsystem`, `UI/StageEffectsPanel`, `UI/StageCuePanel` |
| DMX output | ⚪ planned as Phase F5 (tasks/todo.md Phase 34, STATE.md #65) | — |
| Timeline / Sequencer | ⚪ | — |
| Patch: auto-ID, auto-patch, per-universe index, conflicts (sweep, past 512), AUTO PATCH / FIX CONFLICTS / PATCH SELECTION AT as one undo step, address must fit the footprint, universe usage (#46, ADR 0019) | ✅ allocation, index, decode / 🟢 panel (mouse check pending) | `Show/StageDMXPatching`, `Subsystems/StageDMXPatchSubsystem`, `Player/ModularPlayerController` (RequestPatchFixtures), `UI/StagePatchPanel` |
| DMX encode/decode, simulated live values | ✅ data / 🟢 actor | `LightingFixtureActor::WriteDMX/ReadDMX` |
| DMX input over the network: Art-Net 4 (ArtDmx, ArtPollReply) and sACN E1.31 (multicast joins, priority, timeout, sequence, termination), socket threads with a coalesced game-thread hand-off, only changed fixtures decoded; DMX IN in the Patch panel, remembered per PC (#46, ADR 0019) | ✅ parser, sources, loopback receiver / 🟢 real console on the LAN (not tried) | `Show/StageDMXNetProtocol`, `Show/StageDMXNetworkReceiver`, `Show/StageNetworkDMXBridge`, `Subsystems/StageDMXNetworkSubsystem` |
| DMX output to hardware | ⚪ (the bridge boundary exists; no sender) | `Show/StageDMXBridge` |

## Data, persistence, reports
| System | Status | Where |
|---|---|---|
| Item catalog (Asset Manager `StageItem`) | 🟢 | `Subsystems/StageItemSubsystem`, `Data/BaseItemData` |
| Runtime model import (glb, gltf, obj; fbx in editor only). A damaged import list is moved to .bak; an unreadable one is never overwritten (#40). | ✅ rules and list file / 🟢 import | `Import/StageMeshImportSubsystem` |
| Crash-safe saves: every scene, layout, import list, report and render is flushed to disk before the rename (#40) | ✅ | `IO/StageAtomicFile` |
| Remove an import, release its memory | ⚪ | — |
| Scene save/load (JSON v5: environment v2, cues v3, origin v4, lighting source and camera look v5 (#58); older versions migrate; atomic, async). #40 (R2): a partial load keeps skipped items and stays unsaved, damaged entries are backed up before a save, and loads wait for imports. #40 (R6): restored values are sanitized. | ✅ store, migration, kept items, sanitizing / 🟢 apply (scripted run R40_RunA) | `Scene/StageSceneStore`, `Scene/StageSceneComponent` |
| Saved in scenes | Transforms, label, hide, lock, fixture state, the DMX patch (only since #40; it was silently dropped before), spans, loudspeakers, environment (v2), cues (v3) | — |
| Not saved in scenes | Camera | — |
| Starter stages (#58): Empty Stage, Concert, Theatre, Conference in `Content/StageCraft/Templates`, built through the app by `Scripts/Content/build_starter_stages.ps1`; open untitled with fresh ids and a camera glide (File > New from Template, welcome screen) | ✅ files / 🟢 open | `Scene/StageSceneComponent::LoadTemplate` |
| Autosave (while unsaved, every 120 s), File > Recover Autosave, exit prompt Save / Don't Save / Cancel | 🟢 scripted Standalone runs (#37); buttons need a mouse check | `Scene/StageSceneComponent`, `Workspace/SStageUnsavedChangesDialog`, `Workspace/StageWorkspaceSubsystem` |
| Bill of materials as PDF + CSV (File > Production, BOM panel EXPORT PDF) | ✅ model and PDF / 🟢 export (scripted run, #39) | `Report/StageBillOfMaterials*`, `Report/StageReportPdfComposer`, `UI/StageBillOfMaterialsPanel` |
| Cable schedule and power plan as PDF + CSV: loads balanced over phases, circuits and distros, power home-runs and DMX chains measured along floor and truss with a safety margin, warnings (File > Production) | ✅ model and PDF / 🟢 export (scripted run, #39, #41) | `Report/StageCableSchedule*`, `Report/StagePowerDistributionSettings` (Project Settings) |
| Shopping & assembly list in the cable schedule PDF + CSV: cables by stock length (configurable 2/5/10/15/20 m, +10 %), distros, DMX node outputs and terminators, clamps and safety bonds scaled to fixture weight | ✅ model and PDF / 🟢 export (scripted run, #41) | `Report/StageShoppingList`, `Report/StageCablingSettings` (Project Settings › StageCraft Cabling & Rigging) |
| Fixture benchmark (`StageCraft.Bench.Fixtures`, `Scripts/Bench/RunFixtureBench.ps1`): 200 crossfading fixtures 14.2 ms a frame (was 26.0), 500 crossfading 29 ms (was 56; render-thread bound) (#39) | ✅ stats / 🟢 harness (Development only) | `Diagnostics/StageFixtureBenchmark`, `Diagnostics/StageFrameStats` |
| DMX patch sheet as PDF + CSV (File > Production, Patch panel) | ✅ model and PDF / 🟢 export (scripted run) | `Report/StagePatchSheet`, `Report/StagePdfWriter`, `Report/StagePatchSheetSubsystem` |
| XLSX, saved circuit assignment, power cable lengths, rigging loads, light plot drawings | ⚪ | — |
| Profile, wallet, entitlements, purchases (local backend). No tests. | 🟢 | `Subsystems/StageProfileSubsystem`, `StageEconomySubsystem`, `Economy/*` |
| Shop UI, server commerce | ⚪ | — |
| UI audio feedback | ✅ | `Audio/StageAudioSubsystem` |

## State owners
| Scope | Owner |
|---|---|
| GameInstance | `UStageProfileSubsystem`, `UStageEconomySubsystem`, `UStageItemSubsystem`, `UStageMeshImportSubsystem`, `UStageWorkspaceSubsystem`, `UStageAudioSubsystem`, `UStageStartupSubsystem` (startup progress, #58), `UStageAtmosphereSubsystem` (atmosphere presets, #58) |
| World (Game and PIE) | `UStageSketchSubsystem` (the sketch, #42), `UStageAICommandSubsystem` (the assistant conversation, #43), `UStageSessionSubsystem`, `UStageEditHistorySubsystem`, `UShowControlSubsystem` (ticks only while busy; the only writer of fixture output), `UStageProgrammerSubsystem` (programmer and highlight, #59), `UStageShowLibrarySubsystem` (fixture groups #59, palettes #60), `UStageRenderSubsystem`, `UStageBillOfMaterialsSubsystem`, `UStagePatchSheetSubsystem`, `UStageCableScheduleSubsystem`, `UStageEnvironmentSubsystem` |
| Controller components | Selection, SpawnSystem, PlacementTool, SpanTool, Snapping, GroupTransform, EditHistory, Scene, Viewport, EditorAudioFeedback, VoiceInput (#44) |
| Per-user preferences | `UStageCraftUserSettings` (`GameUserSettings`, storage); `UStageInputPreferencesSubsystem` (local player: key map, look) |
| Rules authority | `AStageCraftGameModeBase` |
| UI-to-game bridge | `AModularPlayerController` |

## Build and tests
| Item | Detail |
|---|---|
| Modules | `ModularSceneBuilder` (core) and `StageCraftSkyCreator` (adapter) |
| Engine | UE 5.8 |
| Plugins enabled | Interchange, SkyCreatorPlugin, AudioCapture (plus Python and ModelingTools in the editor only) |
| Plugins tracked but disabled | 19 |
| Tests | 182 `StageCraft.*` automation tests (#64; `Scripts/Tests/RunAutomation.ps1`); at #49 there were 117: pure logic (colour science, gel library, fixture capabilities and cells, assistant fixture commands, camera rules and the mouse input config, DMX patching and protocol, shadow budget, stage shape geometry, assistant edits and shapes, assistant grammar in English and Armenian, command schema, voice audio and upload format, photo tracing and the online photo reading, sketch strokes, deck geometry, sketch plans, reports, PDF composer, cable schedule, shopping list, frame statistics, key bindings, crash-safe files, import list file, kept scene items) plus world tests for Sky Creator, sketch generation (all or nothing, one undo step), stairs / curved truss / round riser placement and a replacement step (#45), undo/redo rules and transactions, cues, the fixture output path and restored-state sanitizing |
| Areas with no tests | Request bridge, selection, session, economy/profile, patch conflicts, fixture playback (scripted runs only) |
| Console commands | About 88, all inside `#if !UE_BUILD_SHIPPING` |
