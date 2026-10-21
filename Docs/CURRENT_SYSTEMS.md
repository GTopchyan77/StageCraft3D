# StageCraft 3D: current systems

Updated 2026-10-20 after STATE.md #37. This is a quick reference to what exists, how well it works, and where it lives. It was audited on 2026-10-20 at commit `960f5da`. Evidence, file:line references and the risk list are in `Docs/PROJECT_AUDIT.md`.

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
| Panels: Library, Inspector, Fader Bank, Outliner, Patch, BOM, Render, Environment | 🟢 | `UI/*Panel*`, `Content/StageCraft/UI/Panels/DA_Panel_*` |
| Status bar, viewport toolbar (5 sections) | 🟢 | `UI/StageStatusBarWidget`, `UI/StageViewportToolbar` |
| Input. Built in code; there are no IMC or IA assets. | 🟢 | `Player/ModularPlayerController` (`BuildDefaultInputMapping`) |
| Preferences panel, key-binding editor, modal dialogs, colour picker, hover highlight, box select | ⚪ | — |

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
| Undo/redo (100 steps) | ✅ core | `History/*` |
| Undo coverage | 🟡 transforms, place, delete and span shape only | `ModularPlayerController.cpp:1018-1052` |
| Undo respects locks and rules (every item of a group step; insane steps dropped) | ✅ (#37) | `Game/StageCraftGameModeBase::EvaluateTransformChange`, `StageEditHistoryComponent` (TransformValidator) |
| Orbit, pan and frame-selected camera | ⚪ the fly camera only | `Player/StageCameraPawn` |

## Lighting, rendering, environment
| System | Status | Where |
|---|---|---|
| Moving-head fixture: dimmer, RGB(W), CTC, zoom, pan/tilt, gobo, iris, blades, strobe → real spot light | 🟢 | `Actors/LightingFixtureActor` |
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
| Cue engine: store, Go, crossfade | 🟡 exists, but no UI or C++ caller, and not saved | `Subsystems/ShowControlSubsystem` |
| Timeline / Sequencer | ⚪ | — |
| Patch: auto-ID, auto-patch, conflict detection, Patch panel | 🟢 no tests | `ShowControlSubsystem`, `UI/StagePatchPanel` |
| DMX encode/decode, simulated live values | ✅ data / 🟢 actor | `LightingFixtureActor::WriteDMX/ReadDMX` |
| DMX output or input to hardware | ⚪ `UStageDMXBridge` is abstract and empty | `Show/StageDMXBridge` |

## Data, persistence, reports
| System | Status | Where |
|---|---|---|
| Item catalog (Asset Manager `StageItem`) | 🟢 | `Subsystems/StageItemSubsystem`, `Data/BaseItemData` |
| Runtime model import (glb, gltf, obj; fbx in editor only) | ✅ rules / 🟢 import | `Import/StageMeshImportSubsystem` |
| Remove an import, release its memory | ⚪ | — |
| Scene save/load (JSON v2 with environment; v1 migrates; atomic, async) | ✅ store and migration / 🔴 partial load then save loses items (R2) | `Scene/StageSceneStore`, `Scene/StageSceneComponent` |
| Saved in scenes | Transforms, label, hide, lock, fixture state, patch, spans, loudspeakers, environment (v2) | — |
| Not saved in scenes | Cues, camera | — |
| Autosave (while unsaved, every 120 s), File > Recover Autosave, exit prompt Save / Don't Save / Cancel | 🟢 scripted Standalone runs (#37); buttons need a mouse check | `Scene/StageSceneComponent`, `Workspace/SStageUnsavedChangesDialog`, `Workspace/StageWorkspaceSubsystem` |
| Bill of materials and CSV export | ✅ | `Report/StageBillOfMaterials*`, `UI/StageBillOfMaterialsPanel` |
| PDF/XLSX, cable schedule, amps/phase, rigging loads, drawings | ⚪ | — |
| Profile, wallet, entitlements, purchases (local backend). No tests. | 🟢 | `Subsystems/StageProfileSubsystem`, `StageEconomySubsystem`, `Economy/*` |
| Shop UI, server commerce | ⚪ | — |
| UI audio feedback | ✅ | `Audio/StageAudioSubsystem` |

## State owners
| Scope | Owner |
|---|---|
| GameInstance | `UStageProfileSubsystem`, `UStageEconomySubsystem`, `UStageItemSubsystem`, `UStageMeshImportSubsystem`, `UStageWorkspaceSubsystem`, `UStageAudioSubsystem` |
| World (Game and PIE) | `UStageSessionSubsystem`, `UStageEditHistorySubsystem`, `UShowControlSubsystem` (ticks only while busy), `UStageRenderSubsystem`, `UStageBillOfMaterialsSubsystem`, `UStageEnvironmentSubsystem` |
| Controller components | Selection, SpawnSystem, PlacementTool, SpanTool, Snapping, GroupTransform, EditHistory, Scene, Viewport, EditorAudioFeedback |
| Per-user preferences | `UStageCraftUserSettings` (`GameUserSettings`) |
| Rules authority | `AStageCraftGameModeBase` |
| UI-to-game bridge | `AModularPlayerController` |

## Build and tests
| Item | Detail |
|---|---|
| Modules | `ModularSceneBuilder` (core) and `StageCraftSkyCreator` (adapter) |
| Engine | UE 5.8 |
| Plugins enabled | Interchange, SkyCreatorPlugin (plus Python and ModelingTools in the editor only) |
| Plugins tracked but disabled | 19 |
| Tests | 49 `StageCraft.*` automation tests: pure logic plus world tests for Sky Creator and undo/redo rules and transactions |
| Areas with no tests | GameMode rules, request bridge, selection, session, economy/profile, show control |
| Console commands | About 84, all inside `#if !UE_BUILD_SHIPPING` |
