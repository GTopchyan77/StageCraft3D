# StageCraft 3D — Task List

See `STATE.md` for detailed history of completed work.

## Phase 3 — Player Controller & Spawning/Deletion
- [x] `StageItem` trace channel (DefaultEngine.ini) + `StageCraftCollision::StageItemChannel` constant; `AModularBaseActor` blocks it
- [x] `USpawnSystemComponent`: caches the selected item via `UStageItemSubsystem::OnSelectedItemChanged` (no polling); single / continuous grid placement strokes; delete of `IInteractableInterface` actors; `OnItemSpawned` / `OnItemDeleted` delegates
- [x] `AModularPlayerController`: Enhanced Input (designer-assignable IMC/actions, code-built LMB/RMB fallback), cursor traces (placement = Visibility, delete = StageItem), forwards hits to the spawn component
- [x] `AStageCraftGameModeBase` wired as `GlobalDefaultGameMode`
- [x] Clean UE 5.8 build + headless -game smoke test (startup only)
- [ ] Manual PIE verification (Gevor): place props by holding LMB, lights by single click, RMB deletes

## Backlog
- [ ] Camera navigation (orbit/pan/zoom) — mouse look is disabled in Phase 3 so LMB/RMB don't fight the camera
- [ ] Hover highlight (per-frame cursor trace only while nothing is held) — Phase 4 alongside selection
- [ ] Overlay materials for hover/selected (content)
- [ ] Phase 4: Gizmo & transform modes
- [ ] Phase 5: UMG catalog + light color picker
