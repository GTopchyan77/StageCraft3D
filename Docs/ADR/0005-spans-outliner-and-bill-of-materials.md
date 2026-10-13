# ADR 0005: Spans, outliner (visibility, lock, groups) and bill of materials

- **Status:** accepted, implemented. See STATE.md #27.
- **Related:** ADR 0004 (selection set, scene files), ADR 0003 (command history, snapshots), ADR 0002 (placement tool), ADR 0001 (workspace panels). Rules.md "Where state lives", "Safe data binding", Engineering Standard §8, §16, §21, §36, §37, §55, §60.

## 1. Problem

Gevor asked for three features:
1. **Parametric spans.** Linear elements (truss, railings, cables) placed by start and end point, built from repeated segments rather than one mesh scaled to length.
2. **Outliner.** A dockable panel listing placed items by discipline (Truss, Lighting, Audio, Props) with group selection, visibility and transform lock.
3. **Bill of materials.** Item counts, combined weight and power, in a professional panel for production planning.

What existed: catalog specs (`FStageEquipmentSpecs`: weight, power) per item; session totals counting every item once; per-item SaveGame state persisted by undo snapshots and scene files; the GameMode → controller → subsystem request path.

## 2. Verified engine facts (UE 5.8 source)

| # | Fact | Source | Consequence |
|---|---|---|---|
| S1 | Components register in `AActor::PostSpawnInitialize` even for deferred spawns, before `FinishSpawning`. | `Actor.cpp` ~4336 | A span builds its instances when configured before validation; it rebuilds at construction. |
| S2 | `UWorld::DestroyActor` only checks validity; deferred-spawn transforms are cached by weak pointer and stale entries are swept. | `LevelActor.cpp` ~844, `Actor.cpp` ~4260 | A deferred actor the rules refuse is destroyed before it ever finishes spawning or begins play. |
| S3 | Instanced static meshes draw `OverlayMaterial`. | `InstancedStaticMesh.cpp` ~1154 | Span highlight works like other items once overlay materials exist. |
| S4 | Adding a `UUserWidget` to a live panel builds its Slate widget from its current tree; setting `RootWidget` later does not rebuild it. | Found at runtime (STATE.md #27, Run1) | Outliner rows are built before they join the list. |
| S5 | `Icons.Visible/Hidden/Lock/Unlock/ChevronDown/ChevronRight` exist in the Starship core style. | `StarshipCoreStyle.cpp` 361-396 | Outliner icons need no content. |
| S6 | `UE::ComparisonUtility::CompareNaturalOrder` exists in Core. | `Misc/ComparisonUtility.h` | "Spot 2" sorts before "Spot 10". |

## 3. Spans

### 3.1 Data and geometry
- `UStageSpanData` (catalog item): `Fit` (WholeSegments / Stretch), `SegmentLength`, `MaxSegments` (asset cap; `AbsoluteMaxSegments` = 500 caps every asset), `AngleSnapDegrees`, `SegmentParts` and `JointParts` (`FStageSpanPart`: mesh, optional material, transform authored for a segment along +X). **Specs are per segment.**
- Parts rather than one mesh: a real segment mesh is one part; a convincing truss is chords and lacing from simple meshes (the test truss is 20 cylinder parts + a coupler).
- `StageSpanMath` (pure): `ComputeLayout` (whole: nearest count of exact segments; stretch: nearest count, each stretched to the exact length, min half a segment), clamps untrusted lengths (negative, NaN, huge), `ConstrainEndPoint` (level with the start, AutoCAD-style polar snap with projected length), `MakeRunTransform`, segment and joint instance transforms. Stretch is applied in the run's frame, so a part lying along the run gets longer, not thicker.
- `UStageSpanGeometryComponent` builds the run as instances, **one instanced component per distinct mesh + material** (the test truss: 2 components for any length). Used by the placed actor and by the ghost.
- `AStageSpanActor`: one SaveGame value, `SpanLength`, stored normalised; everything else is derived. Undo snapshots and scene files therefore carry spans with no format change.

### 3.2 Quantity
- `AModularBaseActor::GetQuantity` (1) / `PredictQuantity` / `SupportsScaling` (true); spans return their segment count, predict it for a new length, and refuse scale.
- Session totals and the bill of materials count specs × quantity. `StageCraftTags::IsQuantityParameter` (span length) recomputes totals.
- **Limits see the whole run:** `FStagePlacementValidator` and `AStageCraftGameModeBase::EvaluatePlacement` take a quantity. `USpawnSystemComponent` now validates the *prepared* deferred actor (restore or `SpawnConfiguredItem`), so restoring a 10-segment run by undo or scene load is checked for 10 segments; refused actors are destroyed unfinished (S2). Lengthening checks `EvaluateQuantityIncrease`; shrinking is always allowed.

### 3.3 Tool
`UStageSpanToolComponent` (controller), an explicit state machine `Inactive → PickStart → PickEnd`: click the start, the ghost (`AStageSpanPreview`) shows the exact run, its length and segments (status bar), click the end. The run's real end becomes the next start (chained runs: a truss rectangle in five clicks); Esc drops the start, a second Esc leaves Place mode. The controller routes Place-mode input to it while a span is armed. The ghost's verdict is asked with the run's segment count and cached by count.

### 3.4 Editing
- Inspector "Span" section: Length (editable, range = rules), Segments, Segment, Fit (read-only).
- Length edits are **undoable** (`FStageParameterItemCommand`, `IStageItemEditor::SetItemParameter`); undo / redo go back through the GameMode via `UStageEditHistoryComponent::ParameterApplier`, so a lock or limit can refuse them.
- **Rejected:** scaling a mesh to length (the request), and binding length to the gizmo's scale (magic; a 2x scale would not mean 2x segments in the bill).

## 4. Outliner, visibility, lock

- **State lives on the item** (`AModularBaseActor`): `bHiddenOnStage`, `bTransformLocked`, SaveGame, exposed as the Bool parameters `Visible` and `Locked`. Writes go through `RequestParameterChange` (GameMode, session dirty), and the inspector shows them for free. Undo snapshots and scene files carry them. Two independent flags, not a mode (§8).
- **Hidden:** not drawn, no collision (cannot be clicked), not a snap neighbour, left out of Select All, deselected when hidden; still saved and billed.
- **Locked (the rule is in the GameMode, `EvaluateItemEdit`):** refuses Location, Rotation, Scale and span length; deleting is still allowed (Unreal Editor behaviour). The refusal code is `SessionRuleViolation`, not `Locked` (which means "not owned" and offers a purchase).
- **Group edits are all or nothing:** a gizmo drag or numeric transform of a selection asks the GameMode for every selected item first (`EvaluateSelectionTransform`); one locked item, or a span under Scale, refuses the whole edit rather than leaving it behind.
- **Groups are derived, not stored:** `StageOutliner::GetGroupCategory` maps a catalog category to its discipline (direct child of `StageCraft.Category`). Order is the build order of a show (Stage, Truss, Rigging, Lighting, Audio, Screen, Cable, Prop, others, Other); items in natural order, ties by id (deterministic).
- `UStageOutlinerPanel`: groups with eye / lock tri-state toggles (any visible → hide all; partly locked → lock all), item rows, filter, group selection (Ctrl adds / removes the group) through `RequestSelectItems`. It rebuilds on the next tick after `UStageSessionSubsystem::OnItemListChanged` (one rebuild per burst) and restyles on selection changes.
- **Rejected:** user-defined layers stored in the session. Disciplines from catalog categories cover the request with no second source of truth; custom layers can be added later as a SaveGame tag on items.
- Visibility and lock toggles are **not undo steps**, like the Unreal Editor's eye; they do mark the scene unsaved.

## 5. Bill of materials

- `StageBillOfMaterials::Build` (pure): merges placements per catalog item (quantity = units, placements = items/runs), groups by discipline in the outliner's order, subtotals and totals computed from sorted lines (order-independent), defensive against bad specs (negative / NaN → 0, quantity < 1 → 1).
- `UStageBillOfMaterialsSubsystem` (world): a **derived cache** of the session's items, marked stale on `OnItemListChanged` and rebuilt lazily by `GetReport` (a scene load costs one rebuild). No tick, no polling.
- **CSV export** (`Saved/StageCraft/Reports/BOM_<Scene>_<yyyyMMdd-HHmmss>.csv`): RFC 4180 escaping, "." decimals, UTF-8 BOM for spreadsheets, CSV-injection guard (cells starting with = + - @ get an apostrophe), subtotal and total rows. Text built on the game thread, written atomically on a `UE::Tasks` worker (`FStageLayoutStore::WriteFileAtomic`), result returned to a weakly held subsystem; one export at a time; late results from a finished world are dropped. Requested through `AModularPlayerController::RequestExportBillOfMaterials`.
- `UStageBillOfMaterialsPanel`: tiles (items, units, weight, power), table by discipline, Export CSV and Open Folder. File menu: Production section (Bill of Materials..., Export, Open Reports Folder).

## 6. Consequences and limitations
- Runs of the same item share a label (the catalog name) until renamed, as for every item.
- Spans are drawn level; tilt them afterwards with the gizmo's rotation.
- Stretched runs approximate rotated parts (diagonal lacing) because `FTransform` holds no shear; whole-segment runs are exact.
- Changing a span's catalog item with the Type dropdown is not checked against the quantity limits (same as other type swaps).
- A scene load pre-checks each item with quantity 1 for its silent summary; a span refused for its full quantity is refused (and reported) by the spawn validator during the restore.
- The bill counts specs per catalog unit only; rigging loads and per-phase power distribution are not modelled yet.

## 7. Rollback
Each part is separable: the span data/actor/tool/geometry and the quantity parameter of the validator; the Visible/Locked parameters and GameMode rules; the outliner model and panel; the report subsystem and panel. Removing the two tabs from `MakeDefaultLayout` restores the previous default layout; `DA_Panel_Outliner` / `DA_Panel_BillOfMaterials` and the span items are content only.
