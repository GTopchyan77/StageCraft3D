# SplineRouteTool

Runtime spline-route authoring plugin for Unreal Engine 5.7. Lets an operator place a spline route directly in PIE/runtime by clicking in the viewport, drag waypoints to reshape it, spawn vessels or ground vehicles that follow it at configurable per-segment speed, and save/load the whole layout. Built for shipyard/harbor traffic-simulation demos but the follower component works on any actor.

Module: `SplineRouteTool` (Runtime, `LoadingPhase: Default`). Depends on `EnhancedInput`.

## 1. Overview

| Class | Base | Purpose |
|---|---|---|
| `ASplineRouteActor` | `AActor` | A spline route: control points, per-point handle meshes, per-segment visual meshes, per-point speed/reverse metadata. |
| `USplineFollowerComponent` | `UActorComponent` | Drives its owner along a route's spline at configurable speed/turn-rate, scaled by global time. |
| `ASplineFollowerVessel` | `AActor` | Spawnable actor (mesh + follower) used by the vessel picker. |
| `USplineRouteAuthoringSubsystem` | `UWorldSubsystem` | Per-world authoring brain: active route, selection, place/drag/delete, vessel/obstruction spawning, playback, save/load. |
| `ASplineRoutePlayerController` | `APlayerController` | Wires Enhanced Input actions to the authoring subsystem. |
| `ASplineRouteSession` | `AActor` | Drop-in alternative to the GameMode/PC wiring — pushes its own input + HUD onto whatever PC/pawn already exists in a level. |
| `ASplineRouteGameModeBase` | `AGameModeBase` | Minimal demo GameMode: PC class + spawns the HUD. |
| `USplineRouteHUDWidget` | `UUserWidget` | Runtime HUD: sliders, buttons, pickers — see §5 for the binding contract. |
| `UVesselCatalog` | `UPrimaryDataAsset` | Data-driven list of spawnable vessel/ground-vehicle types and obstruction types, plus HUD slider ranges. |
| `USplineRouteTimeSubsystem` | `UGameInstanceSubsystem` | Single global time-scale multiplier shared by every follower. |
| `ASplineRouteZone` | `AActor` | Box volume that constrains where control points may be placed (`Water` or `Ground`). |
| `ASplineRouteObstruction` | `AActor` | Placed yard-layout prop. Visual only — no collision-avoidance or pathing. |
| `USplineRouteSaveGame` | `USaveGame` | The live save format — see §2. |
| `ESplineFollowerEndBehavior`, `ERouteVehicleType`, `ESplineRouteZoneType` | enums | Loop/Clamp/PingPong; Vessel/Ground; Water/Ground. |

> **Dead code:** `Public/SplineRouteScenario.h` defines an older, unused save-file schema (`FSplineRouteScenarioFile` etc.). It predates `USplineRouteSaveGame`, is not `#include`d anywhere, and is not tracked in git. Ignore it — the real save/load path is described in §2.

## 2. Data model

### `ASplineRouteActor`

Control points are plain `USplineComponent` spline points. Each point gets a dynamically-created `UStaticMeshComponent` handle (`HandleMeshes`, index-parallel to spline points, `QueryOnly` collision on `ECC_Visibility` so screen-space hit-testing works). Segments are rendered with one `USplineMeshComponent` per pair of adjacent points, rebuilt from the engine's cylinder mesh whenever geometry changes. Per-point speed overrides and reverse flags live in parallel `TArray<float> PointSpeedOverrides` (`-1` = no override) and `TArray<bool> PointReverseFlags`. If no material is assigned, runtime `UMaterialInstanceDynamic`s are generated from `WorldGridMaterial` (blue = normal, orange = selected, red = reverse, tan = ground-vehicle route).

Key API:
- `int32 AddControlPoint(FVector WorldLocation)` / `MoveControlPoint(int32, FVector)` / `RemoveControlPoint(int32)` / `ClearControlPoints()`
- `void SetSelectedControlPoint(int32)` / `int32 GetSelectedControlPoint() const`
- `void SetPointSpeedOverride(int32, float)` (negative clears) / `float GetSpeedAtDistance(float Distance, float BaseSpeed) const` — **linearly interpolates** the effective speed between the two points bounding `Distance`, so speed changes ease across a segment rather than stepping at the waypoint.
- `void SetPointReverse(int32, bool)` / `bool IsReverseAtDistance(float Distance) const` — true if the segment leaving that point is reverse-flagged.
- `void SetVehicleType(ERouteVehicleType)` — retints all handles; also driven automatically when a vessel is spawned onto the route.
- `void SetHandleSize(float)` — rescales every handle mesh (selected handle gets an extra ×1.4).
- `OnSplineGeometryChanged` / `OnSelectedControlPointChanged` — BlueprintAssignable delegates for reacting to authoring edits.

### `USplineFollowerComponent`

| Property | Default | Effect |
|---|---|---|
| `Route` | `nullptr` | Spline to follow. |
| `Speed` | `200.0` | Base cruise speed; overridden per-segment via `Route->GetSpeedAtDistance`. |
| `StartDelay` / `InitialDistance` | `0` | Delayed start / staggered spawn offset. |
| `EndBehavior` | `Loop` | `Loop` / `Clamp` / `PingPong` at spline ends. |
| `bAlignToTangent` / `bYawOnly` | `true` / `true` | Rotate to tangent; yaw-only zeroes pitch/roll. |
| `TurnRateDegPerSec` | `90.0` | Max turn rate easing toward tangent heading; `0` = instant snap. |
| `HeightOffset` | `0.0` | Z offset applied after the spline-plane position query. |

Each tick: reads the global time scale (`USplineRouteTimeSubsystem`), scales `DeltaTime`, advances `DistanceAlongSpline` by `EffectiveSpeed * ScaledDelta * Direction`, applies `EndBehavior` wrap/clamp/reflect, samples location + tangent at the (clamped) distance, flips the tangent's heading if `Route->IsReverseAtDistance()` says the segment is reverse-flagged (the vessel keeps advancing forward along the spline but faces backward — this is the "reverse motion / docking" behavior, not a literal back-up-and-retrace), then eases rotation toward that heading via `RInterpConstantTo` at `TurnRateDegPerSec` (or snaps instantly if `0`).

### `UVesselCatalog`

Data-driven list consumed by the HUD's vessel picker and by `SpawnVesselOnActiveRoute`. Default asset path: `/Game/SplineRouteTool/Data/DA_VesselCatalog`.

- `Entries: TArray<FVesselCatalogEntry>` — `DisplayName`, `VehicleType` (`Vessel`/`Ground`), `Mesh` (soft ref), `RelativeScale`, `DefaultSpeed`, `HeightOffset`, `TurnRateDegPerSec`.
- `Obstructions: TArray<FObstructionCatalogEntry>` — `DisplayName`, `Mesh`, `Scale`.
- Five `FVesselSliderRange {Min, Max, Default}` fields — `VesselSizeRange`, `SegmentSpeedRange`, `HandleSizeRange`, `TimeScaleRange`, `TurnRateRange`. Whenever `Max > Min`, these **override** the HUD widget's own hardcoded slider bounds (see §5) — the catalog is the actual source of truth at runtime.

### Save format — `USplineRouteSaveGame`

- `Routes: TArray<FSavedRoute>` — `VehicleType` + `Points: TArray<FSavedRoutePoint>` (`Location`, `SpeedOverride`, `bReverse`).
- `Vehicles: TArray<FSavedVehicle>` — `CatalogIndex`, `RouteIndex` (index into `Routes`), `SizeMultiplier`.
- `Obstructions: TArray<FSavedObstruction>` — `CatalogIndex`, `TypeId`, `Transform` (full transform).
- `TimeScale`, `VesselSizeMultiplier`, `VesselTurnRate`.

`SaveScenario()` walks every `ASplineRouteActor` (order = `RouteIndex`), every `ASplineFollowerVessel`, every `ASplineRouteObstruction` in the world and serializes them as above, plus the current global sliders. `LoadScenario()` picks a route class (prefers whatever `ASplineRouteActor` subclass is already in the level so BP visuals survive), calls `ClearScenario()` to destroy all existing routes/vessels/obstructions, then rebuilds routes point-by-point (`AddControlPoint` + per-point overrides), rebuilds vehicles by resolving each saved `RouteIndex` and calling `SpawnVesselOnActiveRoute`, rebuilds obstructions from the catalog, and finally **forces time scale to 0** — a loaded scenario always starts paused; press Play to resume.

### Zones and obstructions

`ASplineRouteZone` is a `UBoxComponent` volume tagged `Water` or `Ground`. `CanPlaceAt()` in the authoring subsystem checks the active route's `VehicleType` against overlapping zones (Ground routes require Ground zones, Vessel routes require Water zones) and silently blocks placement outside the right zone. `ASplineRouteObstruction` is a static-mesh prop with a `CatalogIndex`/`TypeId` for save/load round-tripping — purely visual, no collision-avoidance or pathing is implemented against it.

## 3. Authoring flow

Comment in `ASplineRoutePlayerController::BeginPlay` summarizes it: *"F to place, LMB to select, hold Left Shift + drag to move, Delete/RMB to remove, HUD arrows to cycle."*

1. **Place (F)** → `HandlePlaceAtCursor`. First does a 32px screen-space proximity check against every existing handle — if the cursor is near one, it **selects** that point instead of placing (F never both selects and places). Otherwise: ray/plane-intersects the mouse against the route's placement plane, checks `CanPlaceAt()` (zone rules), claims or spawns the active route, and calls `Route->AddControlPoint()`.
2. **Select (LMB)** → `HandleSelectAtCursor`, a 60px screen-space nearest-handle search. This is deliberately select-**only** — it never arms a drag, which is what fixes the "clicking to select moved my point" bug from the Milestone 3 feedback pass.
3. **Drag (hold Left Shift + move mouse while LMB is down)** → `HandleDrag`. Grabs the currently-selected point (or nearest on-screen handle), and on every subsequent call projects the mouse to the plane, re-checks `CanPlaceAt()`, and calls `Route->MoveControlPoint()`. Releasing Shift or LMB calls `HandleReleaseDrag()`, which clears the drag state and freezes the point in place.
   > **Known doc/code mismatch:** several comments in the header/subsystem still say "Spacebar move-modifier," but the actual bound key in `OnDragTriggered` is `EKeys::LeftShift` (Spacebar was rejected because `DefaultPawn` already uses it for camera-up). Trust the Shift-key behavior; the comments need a cleanup pass.
4. **Delete (Delete key / RMB)** → `HandleDeleteSelected` removes the currently-selected control point; no-ops if nothing is selected.
5. **Spawn a vessel** → the HUD's `VesselPicker` combo box is populated from `UVesselCatalog::Entries` display names; clicking `SpawnVesselButton` calls `SpawnVesselOnActiveRoute(SelectedVesselIndex)`, which loads the entry's mesh, spawns an `ASplineFollowerVessel`, configures its follower (speed, height offset, turn rate), and reclassifies the active route's `VehicleType` to match the spawned entry.
6. **Cycle selection** → HUD's prev/next buttons call `SelectPrevPoint()` / `SelectNextPoint()`, wrapping across the active route's control points.
7. **Playback** → HUD Play/Pause/Stop buttons drive `PlayAll()` / `PauseAll()` / `StopAll()` on the authoring subsystem (global time-scale / per-follower reset), and the `TimeSlider` drives `USplineRouteTimeSubsystem::SetTimeScale` directly, affecting every follower in the world at once.

`ASplineRouteSession` is an alternative entry point to the same subsystem calls above — drop it into any level with an existing pawn/PC and it pushes its own Enhanced Input mapping + HUD without requiring the custom GameMode.

## 4. How to extend

**Add a new vessel type** — no C++ required. Add a row to the `DA_VesselCatalog` DataAsset's `Entries` array with `VehicleType = Vessel`, a mesh reference, scale, default speed, height offset, turn rate. If the mesh has a socket named `zPosition`, `ZPosition`, or `WaterLine`, the follower auto-derives its height offset from that socket instead of the catalog value — use this to get a hull's waterline to sit exactly on the spline plane regardless of what's in the catalog field.

**Add a new ground vehicle type** — identical mechanism to a vessel: add a catalog entry with `VehicleType = Ground`. There is no separate ground-vehicle actor class — `ASplineFollowerVessel` / `USplineFollowerComponent` are shared across both mediums. Setting `VehicleType = Ground` only changes: (a) the route's handle tint when spawned onto it, and (b) which zone type (`Ground` vs `Water`) `CanPlaceAt()` requires for that route.

**Add a new zone type** — this one *does* require C++, since `ESplineRouteZoneType` is a hardcoded two-value enum (`Water` / `Ground`) and `CanPlaceAt()`'s logic is a binary check against it, not data-driven. To add a third type: extend the enum, add a tint case in `ASplineRouteZone::ApplyShapeColor()`, and rewrite `CanPlaceAt()`'s Vessel/Ground → Water/Ground mapping to cover the new case.

**Add a new obstruction type** — same as vessels: add a row to `UVesselCatalog::Obstructions` (`DisplayName`, `Mesh`, `Scale`). No C++ needed; `ASplineRouteObstruction` is generic.

## 5. HUD widget binding guide

`USplineRouteHUDWidget` expects a UMG Blueprint (e.g. `WBP_SplineRouteHUD`) with widgets bound **by exact variable name and class**. Only `TimeSlider` is `BindWidget` (hard requirement — the widget fails to construct without it). Everything else is `BindWidgetOptional`: if missing or misnamed, that feature silently does nothing (no crash, no error) rather than failing loudly, so a missing widget is easy to miss during a Blueprint review.

| Widget name | UMG class | Required | Wired to |
|---|---|---|---|
| `TimeSlider` | `USlider` | **Yes** | `USplineRouteTimeSubsystem::SetTimeScale` (global) |
| `TimeScaleLabel` | `UTextBlock` | No | Shows current time scale |
| `VesselPicker` | `UComboBoxString` | No | Selects vessel/ground-vehicle catalog entry to spawn |
| `SpawnVesselButton` | `UButton` | No | `SpawnVesselOnActiveRoute()` |
| `SegmentSpeedSlider` | `USlider` | No | Per-waypoint speed override |
| `SelectedSegmentLabel` | `UTextBlock` | No | Shows selected waypoint's speed state |
| `CyclePrevButton` / `CycleNextButton` | `UButton` | No | `SelectPrevPoint()` / `SelectNextPoint()` |
| `SelectedPointLabel` | `UTextBlock` | No | Shows "Waypoint: i / N" |
| `TurnRateSlider` | `USlider` | No | `SetTurnRateForAllVessels()` (global) |
| `TurnRateLabel` | `UTextBlock` | No | Shows current turn rate |
| `ReverseToggleButton` | `UButton` | No | `ToggleReverseForSelectedPoint()` |
| `ReverseLabel` | `UTextBlock` | No | Shows reverse-node state |
| `PlayButton` / `PauseButton` / `StopButton` | `UButton` | No | `PlayAll()` / `PauseAll()` / `StopAll()` |
| `SaveButton` / `LoadButton` | `UButton` | No | `SaveScenario()` / `LoadScenario()` |
| `RouteTypeButton` | `UButton` | No | `ToggleActiveRouteVehicleType()` |
| `RouteTypeLabel` | `UTextBlock` | No | Shows Water/Ground for the active route |
| `ObstructionModeButton` | `UButton` | No | Toggles obstruction-placement mode |
| `ObsPrevButton` / `ObsNextButton` | `UButton` | No | Cycles selected obstruction type |
| `ObstructionLabel` | `UTextBlock` | No | Shows current obstruction mode/type |
| `HandleSizeSlider` | `USlider` | No | `ActiveRoute->SetHandleSize()` |
| `HandleSizeLabel` | `UTextBlock` | No | Shows waypoint size |
| `VesselSizeSlider` | `USlider` | No | `SetVesselSizeMultiplier()` (global) |
| `VesselSizeLabel` | `UTextBlock` | No | Shows ship size multiplier |
| `Panel` | `UWidget` | No | Registered as the "cursor over HUD" hit-test region so viewport clicks under the panel don't place/select |

Slider ranges (`Min`/`Max`/`Default`) are configured on the widget class itself but get overridden at runtime by `UVesselCatalog`'s matching `FVesselSliderRange` fields whenever the catalog defines one (`Max > Min`) — treat the catalog as authoritative when tuning ranges, not the widget defaults.
