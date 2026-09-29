# StageCraft 3D — Task List

See `STATE.md` for detailed history of completed work.

## Phase 3 — Player Controller & Spawning/Deletion
- [x] Implemented, compiled, headless smoke-tested (see STATE.md #3)
- [ ] Manual PIE verification (Gevor): place props by holding LMB, lights by single click, RMB deletes

## Phase 4 — Selection & Transform Gizmo
- [x] `Gizmo` trace channel (ECC_GameTraceChannel2) + `StageCraftCollision::GizmoChannel`
- [x] `USelectionComponent`: interface-based selection, OnSelect/OnDeselect highlight swap, auto-release on OnDestroyed, `OnSelectionChanged` delegate
- [x] `AModularTransformGizmo`: world-space translate arrows / rotate rings, drag via cursor ray vs. drag plane, snapping props, Space toggle, constant screen size (ticks only while attached)
- [x] Controller click arbitration (gizmo > item select > empty deselect+place; RMB item delete > empty deselect+disarm), Space binding with legacy-key consumption
- [x] Game target compiles clean (editor target blocked by Live Coding while the editor was open)
- [x] Editor target rebuilt and up to date
- [ ] Manual PIE verification (Gevor): see STATE.md #4 checklist

## Backlog
- [ ] Camera navigation (orbit/pan/zoom); mouse look is disabled so LMB/RMB don't fight the camera
- [ ] Hover highlight via IInteractableInterface::OnHoverBegin/End (needs a cheap throttled cursor trace)
- [ ] Gizmo handle hover highlight
- [ ] Place onto an existing item with a modifier (plain LMB on an item now selects it)
- [ ] Local-space gizmo option; gizmo drawn on top of geometry (needs a custom no-depth-test material)
- [ ] Undo/redo, hooking OnItemSpawned / OnItemDeleted / gizmo drag end
- [ ] Overlay materials for hover/selected (content)
- [ ] Phase 5: UMG catalog + light color picker
