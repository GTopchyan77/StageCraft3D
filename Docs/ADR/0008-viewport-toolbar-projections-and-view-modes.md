# ADR 0008: Viewport toolbar, orthographic projections, view modes, global grid snap

- **Status:** accepted, implemented. See STATE.md #32.
- **Related:** ADR 0001 (dockable workspace), ADR 0002 (placement), ADR 0003 (object snapping), ADR 0004 (render subsystem), STATE.md #11–#15 (fly camera). Rules.md "Where state lives", "Safe data binding", Engineering Standard §8, §16, §17, §21, §28, §57, §65.

## 1. Problem

Gevor asked (2026-10-19) for a CAD-style viewport header toolbar:

1. **Projection.** Perspective, plus orthographic Top (XY), Front (XZ), Back, Left / Right (YZ), with orthographic zoom.
2. **View modes.** Lit (photometric), Unlit (flat, no lights) and a wireframe / line view for CAD alignment. The stage grid must read clearly in the orthographic and unlit modes.
3. **Toolbar.** Projection selector, view-mode selector, gizmo Move / Rotate / Scale, grid snap (10 / 50 / 100 cm) and angle snap (5° / 15° / 45°).
4. Event-driven updates, no per-frame polling.

Decisions by Gevor (2026-10-19):
- **Wireframe = a CAD line view** (post-process outlines), because the engine's wireframe cannot ship (F2).
- **The toolbar grid is global.** It replaces the per-item placement grids, and it drives placement, gizmo moves and the drawn grid. Off means free placement and free moves.

## 2. Verified engine facts (UE 5.8 source)

| # | Fact | Source | Consequence |
|---|---|---|---|
| F1 | `UGameViewportClient::SetViewMode` forces `VMI_Lit` in Test/Shipping, and `VMI_Lit` without `AllowDebugViewmodes()` in cooked builds. `Draw` itself only reads the public `ViewModeIndex` and runs `EngineShowFlagOverride` with it. | `GameViewportClient.cpp` 1619-1620, 4706-4750 | StageCraft sets `ViewModeIndex` and `EngineShowFlags` itself, in `UStageCraftGameViewportClient::ApplyStageViewMode`, instead of calling `SetViewMode`. |
| F2 | The `Wireframe` show flag is `SHOWFLAG_FIXED_IN_SHIPPING(0, …)`, a compile-time constant 0 in optimised builds. | `ShowFlagsValues.inl` 283, `ShowFlags.cpp` 185 | Engine wireframe cannot exist in Shipping. Hence the CAD line view (§4.3). |
| F3 | `Lighting`, `UnlitViewmode`, `PostProcessing`, `Fog`, `EyeAdaptation` and `Bloom` are `SHOWFLAG_ALWAYS_ACCESSIBLE`. `EngineShowFlagOverride` turns lighting, atmosphere and fog off for `VMI_Unlit` (and back on for `VMI_Lit`) every frame. | `ShowFlagsValues.inl`, `ShowFlags.cpp` 560-620 | Unlit works in every configuration through `ViewModeIndex = VMI_Unlit`. |
| F4 | Unlit colour (base colour written as emissive) is `USE_DEVELOPMENT_SHADERS` code, gated by `View.UnlitViewmodeMask`, which `SceneView.cpp` sets from show flags with no Shipping guard. Development shaders are compiled when `r.CompileShadersForDevelopment` is 1 (engine default; this project does not override it). | `BasePassPixelShader.usf` 1623-1628, `SceneView.cpp` 2873, `ShaderCompiler.cpp` 3887 | Unlit in a cooked build depends on development shaders being cooked. The project pins `r.CompileShadersForDevelopment=1` explicitly, so the dependency is visible and cannot be lost silently. |
| F5 | Without a camera component, `APlayerCameraManager::UpdateViewTarget` builds the view from `bIsOrthographic` and `DefaultOrthoWidth` (public), plus the auto ortho-plane flags. | `PlayerCameraManager.cpp` 370-382, 468 | Orthographic projection needs no camera component; the viewport component sets those members. Auto planes are turned off and the camera stands far back, so near/far are explicit (0 / world max). |
| F6 | `ApplyViewMode(VMI_Unlit)` turns `PostProcessing` off. Batched lines (`ULineBatchComponent`) draw into scene colour, not the GBuffer. | `ShowFlags.cpp` 304, line batcher scene proxy | The line view re-enables post-processing after `ApplyViewMode`, and its material adds scene colour back, so the grid survives (§4.3). |
| F7 | Camera modifiers' `ModifyPostProcess` blends go to the camera manager's post-process blend cache. That cache is applied to the player's view, not to `GetCameraCacheView().PostProcessSettings`, which renders copy. | `PlayerCameraManager.cpp` (`ApplyCameraModifiers`, `AddCachedPPBlend`), `LocalPlayer.cpp` | The line-view material reaches the viewport through a camera modifier and never leaks into renders. |
| F8 | UE5 ortho viewports: Top = pitch −90 (+X up, +Y right); Front (`LVT_OrthoXZ`) = yaw −90; Back = yaw 90; Left (`LVT_OrthoYZ`) = yaw 0; Right = yaw 180. | `EditorViewportClient.cpp` 5024-5052 | StageCraft uses the same orientations and names as the Unreal Editor. |
| F9 | The engine default `AspectRatioAxisConstraint` is `AspectRatio_MaintainYFOV`. In that case the ortho width spans the viewport's **height** (no aspect compensation). The perspective FOV stays horizontal at the camera's own `AspectRatio`, which defaults to **4:3** in `APlayerCameraManager`, and the viewport keeps the resulting vertical angle. A scene capture reads both horizontally at its own aspect. | `BaseEngine.ini` 2900, `CameraStackTypes.cpp` 284-335, `PlayerCameraManager.cpp` 47 | Orthographic pan and zoom measure the pixel size by deprojection, so they do not assume an axis. Renders convert FOV and ortho width (`StageRender::GetCaptureFOV`, `GetCaptureOrthoWidth`), so a render frames like the viewport. This also fixes perspective renders, which since #26 had been cropped by 4:3 → 16:9 (verified by measurement, STATE.md #32). |
| F10 | `APlayerController::UpdateRotation` passes the control rotation through `ProcessViewRotation`, which limits pitch to `ViewPitchMin/Max` (±89.9 by default). | `PlayerCameraManager.cpp` (`LimitViewPitch`) | While an ortho view is active, the viewport component widens the limits to ±90 so Top looks straight down, and restores them in Perspective. |
| F11 | Eye adaptation forces its target exposure on a camera cut (`ForceTarget = View.bCameraCut || …`). | `PostProcessEyeAdaptation.cpp` 723 | Every projection or view-mode switch requests a camera cut. Without it, Lit came back dark after Lines (eye adaptation off there) and took about 10 s to recover (measured: floor 22 → 144). The cut also drops TAA history across the switch. |

## 3. Where the state lives (Rules.md table)

| State | Owner | Why |
|---|---|---|
| Projection, view mode, grid visibility, ortho width, saved perspective camera | `UStageViewportComponent` (new, on `AModularPlayerController`) | One local player's view, like the other tool components. |
| Grid snap and angle snap | `UStageSnappingComponent` (extended), stored in `UStageCraftUserSettings` | Snapping already owns the object-snap preference and is its only writer. |
| Show flags and `ViewModeIndex` | `UStageCraftGameViewportClient::ApplyStageViewMode` | The viewport client owns them (F1). |
| Ortho projection | The player camera manager (F5), written only by the viewport component | — |
| Fixed ortho rotation, planar motion | `AStageCameraPawn` "fixed view" | The pawn already owns rotation and motion. |
| Grid lines | `AStageGridActor` (new), owned by the viewport component | A world primitive, registered as a render helper. |

Persisted per machine: grid snap, angle snap and grid visibility. Projection and view mode are not persisted: every launch starts in Lit Perspective, so a presentation never opens in a line view.

## 4. Design

### 4.1 Projections
- `EStageViewProjection { Perspective, Top, Front, Back, Left, Right }` and `StageViewMath::GetProjectionRotation` (F8).
- **Perspective → ortho:**
  - The focus point is the placement surface under the viewport centre (fallback: the Z=0 plane, or 20 m ahead).
  - The width fits the perspective view at that distance (`2·d·tan(FOV/2)`), so the scale carries over.
  - The perspective camera transform is saved.
- **Ortho → ortho:** keeps the focus (the view centre) and the width.
- **Ortho → perspective:** restores the saved camera.
- The camera stands `OrthoStandoff` (200 m) back from the focus along the view axis, so the whole layout is in front of the near plane.
- The switch is instant. A tween would only animate a camera the user cannot steer meanwhile.
- **Ortho navigation** (UE editor convention):
  - The right button drags to pan, 1:1 with the cursor. The size of one pixel is measured by deprojection (F9).
  - The wheel zooms around the cursor: `StageViewMath::ComputeOrthoZoomShift` keeps the world point under the cursor fixed, with the cursor offset measured in world units. The width is clamped to 1–500 m.
  - The camera manager's pitch limits are ±90 while in ortho (F10). Each switch is a camera cut (F11).
  - WASD pans in the view plane at fly speed; looking around is ignored.
- **Screen-size helpers.** The gizmo and the snap guides used camera distance, which is meaningless in ortho. `StageViewMath::GetScreenScaleDistance` returns the distance, or in ortho `OrthoWidth / (2·tan(FOV/2))`.
- **Renders follow the projection.** The capture copies `ProjectionMode`, and its FOV and ortho width are converted for the output aspect (F9), so a top view renders as a plan framed like the viewport. View modes never reach renders: these are always photometric.

### 4.2 Unlit
`ViewModeIndex = VMI_Unlit` + `ApplyViewMode(VMI_Unlit)` on a copy of the startup flags, so returning to Lit restores everything (F1, F3, F4).

### 4.3 CAD line view ("Lines")
- **Flags:** Unlit flags, then `PostProcessing` back on (F6), with `UnlitViewmode`, temporal AA / AA, eye adaptation, bloom and motion blur off. Scene colour then holds only emissive surfaces and the grid lines, at a fixed exposure and without jitter.
- **Material:** `M_StageLineView` (post-process, after tonemapping). Silhouettes and creases are found in screen space:
  - Depth discontinuities use the Laplacian of an affine depth: device-linear depth in ortho, inverse depth in perspective, so a flat floor at a grazing angle is not an edge.
  - Normal discontinuities use the GBuffer world normal.
  - Lines are drawn over a flat background, and scene colour is added back (grid, emissive LEDs).
- **Delivery:** `UStageLineViewCameraModifier`, enabled only in Lines mode (F7).
- **Content:** made headless by `Scripts/Content/create_viewport_content.py`, and cooked through `DirectoriesToAlwaysCook` `/Game/StageCraft/Viewport`.

### 4.4 Grid
- `AStageGridActor` draws 1-pixel lines (thickness 0) with a `ULineBatchComponent` whose tick is disabled. Lines are rebuilt only when the plane, cell size or visibility changes.
- **Plane:** the floor (XY, 1 cm above Z=0, so it does not z-fight with a floor whose top is at 0) in Perspective and Top. Front/Back and Left/Right use a vertical plane behind the layout, as seen from the camera.
- **Lines:** minor lines at the grid-snap size (100 cm when snap is Off), major lines every 10 cells, and X/Y axis lines tinted red/green. The extent is ±50 m, with a cap of 1 001 lines per direction.
- **Visibility:** the Grid toggle on the toolbar. It is drawn in every mode and hidden from renders (`RegisterEditorHelper`).

### 4.5 Global grid and angle snap
- **Presets:** grid Off / 10 / 50 / 100 cm, angle Off / 5° / 15° / 45°. The defaults are 50 cm (the old truss/span grid) and 15°. Values loaded from disk that are not presets fall back to the defaults.
- **Placement and spans:** `StagePlacementMath::WithGlobalGrid(Rules, Cm)` replaces X/Y of the item's `GridSize` (Z is kept). Off means free.
- **Gizmo moves** snap the **absolute** world position on the dragged axis (CAD alignment), instead of the drag distance; object snapping still wins.
- **Gizmo rotations** snap the angle as before.
- **Wiring:** the controller pushes the values on `OnSnapSettingsChanged`; the tools stay preference-agnostic.

### 4.6 Toolbar
- `UStageViewportToolbar`: a code-built UMG strip in the viewport tab, above the viewport. It is created through a new shell callback, like the status bar.
- **Controls:** a projection dropdown, a Lit / Unlit / Lines segment, a Grid toggle, a Move / Rotate / Scale segment, and Grid and Angle snap dropdowns.
- **Binding:** it writes through the components' setters and redraws only from their delegates (`OnViewChanged`, `OnSnapSettingsChanged`, `OnModeChanged`). It reads values back, binds in `NativeConstruct`, unbinds in `NativeDestruct`, and has no tick.
- PIE has no workspace and no toolbar; the dev console commands work there.

## 5. Alternatives considered

- **Engine wireframe:** impossible in Shipping (F2). Rejected by Gevor in favour of the line view.
- **Unlit as a post-process of the GBuffer base colour:** works without development shaders. However, it runs after TAA (jittered, aliased) or before the tonemapper (subject to the level's exposure). The engine's own Unlit is anti-aliased and matches the editor. Kept the engine path and pinned F4's dependency.
- **A camera component on the pawn for ortho:** more invasive (FOV and rotation would move to the component, and every existing camera assumption changes). F5 makes it unnecessary.
- **A grid as a world-aligned material on the floor:** invisible in Lines mode (no lighting, outlines only), tied to one floor mesh, and blind in front/side views. Rejected for line primitives.
- **Per-item grids kept for placement:** Gevor chose the global grid.

## 6. Consequences

- Per-item `GridSize` X/Y no longer affects placement. Only Z, offset and normal alignment remain. Catalog data is unchanged.
- Gizmo moves now land on absolute grid positions. An item that was off-grid jumps onto the grid at the first snapped move (the Unreal Editor behaves the same).
- Ortho views use the same pawn. Selection, placement and gizmo picking go through deprojection, which UE supports in ortho.
- **Lumen in ortho:** lighting may differ from perspective. This is the engine's known limitation; Lit ortho is a working view, and renders use the same view.

## 7. Rollback

- Every new piece is additive: the component, actor, modifier, toolbar, material and settings fields.
- To revert the global grid, restore `ComputeLandingTransform` / `ComputeSnappedPoint` to the item rules, and restore the gizmo's distance snap.
- Removing the toolbar callback restores the old viewport tab.
