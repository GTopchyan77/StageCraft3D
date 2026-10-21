# StageCraft 3D: current systems

Updated 2026-10-21 after STATE.md #42. This is a quick reference to what exists, how well it works, and where it lives. It was audited on 2026-10-20 at commit `960f5da`. Evidence, file:line references and the risk list are in `Docs/PROJECT_AUDIT.md`.

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
| Status bar, viewport toolbar (6 sections, the last opens Sketch to Stage and the AI Assistant) | 🟢 | `UI/StageStatusBarWidget`, `UI/StageViewportToolbar` |
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
| Reference grid | ✅ | `Viewport/StageGridActor` |
| Outliner: discipline groups, hide, lock. No user groups. | ✅ model | `Outliner/StageOutlinerModel`, `UI/StageOutlinerPanel` |
| Spans: path, closed loop, sag. Points cannot be edited after placement. | ✅ math | `Actors/StageSpanActor`, `Placement/StageSpanMath`, `StageSpanToolComponent` |
| Decks / risers of any simple outline (procedural prism, Height parameter, panels as quantity; saved through SaveGame, sanitized on restore) (#42). Outline not editable after placing. | ✅ geometry, restore repair / 🟢 visuals | `Actors/StageDeckActor`, `Data/StageDeckData`, `Placement/StageDeckMath` |
| Sketch to Stage (#42): 2D window (freehand, stage shape, truss line, fixture, erase; stabiliser, grid snap), plan (decks, truss on whole sections, fixtures hung / on decks / on the floor), GENERATE 3D through the rules, all or nothing, one undo step. Sketch not saved in scenes; no 3D preview. | ✅ plan and generation / 🟢 window (mouse check pending) | `Sketch/*`, `UI/StageSketchCanvasWidget`, `UI/StageSketchSurfaceWidget`, `ModularPlayerController::RequestGenerateFromSketch` |
| AI Assistant (#43): chat panel; local English grammar (questions on items, weight, power, fixtures, cues, environment; add fixtures, store / play cues, weather, time, sky, exports, undo / redo), optional Claude model (off by default, key from ANTHROPIC_API_KEY); every command validated and run through the controller's requests (rules, undo). Weather / time / sky changes from the assistant are undoable. | ✅ commands and answers / 🟢 panel (manual check pending) / ⚪ online model untested live | `AI/*`, `UI/StageAIAssistantWidget`, `ModularPlayerController::RequestAssistantPrompt` |
| Undo/redo (100 steps) | ✅ core | `History/*` |
| Undo coverage | 🟡 transforms, place, delete, span shape, deck height, cue edits, a whole sketch generation (one step) | `ModularPlayerController.cpp:1018-1052` |
| Undo respects locks and rules (every item of a group step; insane steps dropped) | ✅ (#37) | `Game/StageCraftGameModeBase::EvaluateTransformChange`, `StageEditHistoryComponent` (TransformValidator) |
| Orbit, pan and frame-selected camera | ⚪ the fly camera only | `Player/StageCameraPawn` |

## Lighting, rendering, environment
| System | Status | Where |
|---|---|---|
| Moving-head fixture: dimmer, RGB(W), CTC, zoom, pan/tilt, gobo, iris, blades, strobe → real spot light. Applies only the outputs a change drives; the gobo/iris/blade light function only while the beam is shaped (#39) | ✅ output path (Lighting.BeamShapingOnDemand, PanTiltTransform, FadeChangedOnly) / 🟢 visuals | `Actors/LightingFixtureActor` |
| Fixture data and DMX layout | ✅ | `Data/LightingFixtureData` |
| Photometrics (display only) | ✅ | `Show/StagePhotometry` |
| Beam visualisation | 🟡 relies on the level's volumetric fog; no cone mesh | `Content/StageCraft/Maps/L_StageTest` |
| `FixtureKind` (wash, spot, laser, strobe), IES, pan/tilt speed, DMX modes | ⚪ | — |
| Still renders | ✅ rules and framing / 🟢 capture | `Render/StageRenderSubsystem`, `UI/StageRenderPanel` |
| Render cancel, view-mode match, path tracing, scalability | ⚪ | — |
| Outdoor sky and weather (Sky Creator backend); packaged and measured (ADR 0010 §7) | ✅ | `Environment/*`, `Source/StageCraftSkyCreator/*` |

## Show control and DMX
| System | Status | Where |
|---|---|---|
| Cue list: store / re-record, update, delete, Go, Back, Go To, crossfade (writes only the attributes that change, #39), Internal/External; GameMode rules, undo, saved in scenes (v3) | ✅ rules, history, file, fade / 🟢 panel (mouse check pending, #38) | `Subsystems/ShowControlSubsystem`, `UI/StageCuePanel`, `History/StageCueCommands`, `Game/StageCraftGameModeBase` (EvaluateCueRequest) |
| Timeline / Sequencer | ⚪ | — |
| Patch: auto-ID, auto-patch, conflict detection, Patch panel | 🟢 no tests | `ShowControlSubsystem`, `UI/StagePatchPanel` |
| DMX encode/decode, simulated live values | ✅ data / 🟢 actor | `LightingFixtureActor::WriteDMX/ReadDMX` |
| DMX output or input to hardware | ⚪ `UStageDMXBridge` is abstract and empty | `Show/StageDMXBridge` |

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
| Controller components | Selection, SpawnSystem, PlacementTool, SpanTool, Snapping, GroupTransform, EditHistory, Scene, Viewport, EditorAudioFeedback |
| Per-user preferences | `UStageCraftUserSettings` (`GameUserSettings`, storage); `UStageInputPreferencesSubsystem` (local player: key map, look) |
| Rules authority | `AStageCraftGameModeBase` |
| UI-to-game bridge | `AModularPlayerController` |

## Build and tests
| Item | Detail |
|---|---|
| Modules | `ModularSceneBuilder` (core) and `StageCraftSkyCreator` (adapter) |
| Engine | UE 5.8 |
| Plugins enabled | Interchange, SkyCreatorPlugin (plus Python and ModelingTools in the editor only) |
| Plugins tracked but disabled | 19 |
| Tests | 75 `StageCraft.*` automation tests: pure logic (sketch strokes, deck geometry, sketch plans, reports, PDF composer, cable schedule, shopping list, frame statistics, key bindings, crash-safe files, import list file, kept scene items) plus world tests for Sky Creator, sketch generation (all or nothing, one undo step), undo/redo rules and transactions, cues, the fixture output path and restored-state sanitizing |
| Areas with no tests | Request bridge, selection, session, economy/profile, patch conflicts, fixture playback (scripted runs only) |
| Console commands | About 88, all inside `#if !UE_BUILD_SHIPPING` |
