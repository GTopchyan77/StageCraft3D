# StageCraft 3D: current systems

Updated 2026-10-26 after STATE.md #46. This is a quick reference to what exists, how well it works, and where it lives. It was audited on 2026-10-20 at commit `960f5da`. Evidence, file:line references and the risk list are in `Docs/PROJECT_AUDIT.md`.

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
| Duplicate / copy-paste | ⚪ | — |
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
| Undo coverage | 🟡 transforms, place, delete, span shape, deck height, stairs and curved truss shape, cue edits, a whole sketch generation or assistant build (one step), an assistant move or replacement (one step) | `ModularPlayerController.cpp:1018-1052` |
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

## Show control and DMX
| System | Status | Where |
|---|---|---|
| Cue list: store / re-record, update, delete, Go, Back, Go To, crossfade (writes only the attributes that change, #39), Internal/External; GameMode rules, undo, saved in scenes (v3) | ✅ rules, history, file, fade / 🟢 panel (mouse check pending, #38) | `Subsystems/ShowControlSubsystem`, `UI/StageCuePanel`, `History/StageCueCommands`, `Game/StageCraftGameModeBase` (EvaluateCueRequest) |
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
| Scene save/load (JSON v3 with environment and cues; v1 and v2 migrate; atomic, async). #40 (R2): a partial load keeps skipped items and stays unsaved, damaged entries are backed up before a save, and loads wait for imports. #40 (R6): restored values are sanitized. | ✅ store, migration, kept items, sanitizing / 🟢 apply (scripted run R40_RunA) | `Scene/StageSceneStore`, `Scene/StageSceneComponent` |
| Saved in scenes | Transforms, label, hide, lock, fixture state, the DMX patch (only since #40; it was silently dropped before), spans, loudspeakers, environment (v2), cues (v3) | — |
| Not saved in scenes | Camera | — |
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
| GameInstance | `UStageProfileSubsystem`, `UStageEconomySubsystem`, `UStageItemSubsystem`, `UStageMeshImportSubsystem`, `UStageWorkspaceSubsystem`, `UStageAudioSubsystem` |
| World (Game and PIE) | `UStageSketchSubsystem` (the sketch, #42), `UStageAICommandSubsystem` (the assistant conversation, #43), `UStageSessionSubsystem`, `UStageEditHistorySubsystem`, `UShowControlSubsystem` (ticks only while busy), `UStageRenderSubsystem`, `UStageBillOfMaterialsSubsystem`, `UStagePatchSheetSubsystem`, `UStageCableScheduleSubsystem`, `UStageEnvironmentSubsystem` |
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
| Tests | 117 `StageCraft.*` automation tests (#49): pure logic (colour science, gel library, fixture capabilities and cells, assistant fixture commands, camera rules and the mouse input config, DMX patching and protocol, shadow budget, stage shape geometry, assistant edits and shapes, assistant grammar in English and Armenian, command schema, voice audio and upload format, photo tracing and the online photo reading, sketch strokes, deck geometry, sketch plans, reports, PDF composer, cable schedule, shopping list, frame statistics, key bindings, crash-safe files, import list file, kept scene items) plus world tests for Sky Creator, sketch generation (all or nothing, one undo step), stairs / curved truss / round riser placement and a replacement step (#45), undo/redo rules and transactions, cues, the fixture output path and restored-state sanitizing |
| Areas with no tests | Request bridge, selection, session, economy/profile, patch conflicts, fixture playback (scripted runs only) |
| Console commands | About 88, all inside `#if !UE_BUILD_SHIPPING` |
