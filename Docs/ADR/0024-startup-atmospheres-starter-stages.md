# ADR 0024: Startup screen, atmospheres and starter stages

- **Status:** accepted. Implemented with STATE.md #58 (Phase E2).
- **Related:**
  - ADR 0009 (outdoor environment), amended by §4–5 here.
  - ADR 0001 (dockable workspace, F12: the engine's per-frame window switch).
  - ADR 0010 / 0013 (scene format, Ctrl+S on an untitled stage).
  - ADR 0018 §5 (the camera's focus glide), ADR 0022 §4 (scene format v4, origin).
  - Rules.md: Architecture (state by lifetime, request bridge, push not poll), Engineering Standard §17 (timers), §21–22 (tick and measurement), §55 (trust boundaries), §61 (deferred structural changes).

## 1. Problem

Gevor's brief (2026-11-02) asked for an app that looks finished the moment it opens:
- a branded loader instead of a blank screen, with asynchronous loading;
- studio-grade lighting and post-processing;
- an atmosphere switcher: Clear Daytime, Dramatic Sunset, Cyberpunk Night, Minimalist Studio;
- several default stage presets;
- smooth transitions, graceful fallbacks, no frame drops, clean teardown.

The brief was written in web-app terms ("fetch HDRI", "unmount", "render loop"); this ADR maps it onto the app.

What existed (three read-only audits):
- **Startup:** no loading screen at all. The window showed the bare game viewport while the map loaded in a blocking call; the dock layout then appeared in the controller's BeginPlay with three catalogs still streaming, so panels said "Loading…". Readiness was separate flags; there was no combined signal and no percentage anywhere.
- **Environment:** `UStageEnvironmentSubsystem` (ADR 0009) with the sky on or off, time, sun and weather, saved in scenes. Every change snapped. There was no camera look (exposure, bloom, grading) on the live view and no studio lighting: "sky off" meant the level's own dark lights.
- **Presets:** no starter stages or templates.

Gevor's decisions: the studio is StageCraft's own rig with an HDRI and a flat fallback; the camera stays put on an atmosphere switch; starter stages ship, with a start screen and a camera glide; the loader covers both engine start and the catalogs.

## 2. Decision: the startup loader

**One widget for both stages.** `SStageBrandedLoader` (Private/Startup): the wordmark, a thin bar and a status line, colours from the theme's class defaults (no asset, so it paints before content loads). The bar animates from the paint time, so it moves on the MoviePlayer's loading thread too, which paints but runs no active timers.

**Stage 1, engine start and the blocking map load.** The game module binds `IGameMoviePlayer::OnPrepareLoadingScreen` (not in the editor, commandlets or servers) and hands the movie player the loader with no fraction: a blocking `LoadMap` has none, so the bar honestly sweeps.

- The F12 interaction (ADR 0001): when a loading screen ends, `UGameEngine::SwitchGameWindowToUseGameViewport` registers `GameViewportWidget` as Slate's game viewport. Had the workspace installed during the loading screen, that would be our host (no viewport interface): `RegisterViewport` ensures (seen in the first run). So `UStageWorkspaceSubsystem` waits in the new state `WaitingForLoadingScreen` and installs on `OnMoviePlaybackFinished`; the controller's `WillHostPanels` keeps the fixed HUD out meanwhile.

**Stage 2, the in-app overlay.** `UStageStartupSubsystem` (game instance) combines named sources into one weighted progress, one status line and one state (`Loading` → `Ready`, `Failed` or `TimedOut`, once):
- the item catalog (weight 6), the panel definitions (2), the product catalog (1), the atmosphere presets (1);
- each loader keeps its `FStreamableHandle` and pushes progress through `BindUpdateDelegate`; nothing polls;
- a failed source names itself; after `StartupTimeoutSeconds` (30) the loader offers Continue. Neither ever traps the user: what is not loaded keeps refusing its own requests, as before.

The shell shows the loader on its own overlay layer above dialogs; it fades out (0.35 s) when ready and is removed, and is never shown when everything was ready at install. `StageCraft.Startup.PreviewLoader` (dev) shows it on demand, because a real start is usually too quick to see.

## 3. Decision: atmospheres as content

`UStageAtmospherePreset` (primary asset type `StageAtmosphere`, `/Game/StageCraft/Atmospheres`, AlwaysCook) holds a display name, description, order, swatch colour and a complete `FStageEnvironmentSettings`. `UStageAtmosphereSubsystem` (game instance) loads them asynchronously at startup (a startup source), lists them in order and finds the one matching the stored settings (the panel lights that card). Four ship, built by `Scripts/Content/create_atmosphere_content.py`.

Applying one goes through the controller (`RequestApplyAtmosphere`) into the existing environment request: one undo step, blended in. The catalog owns no environment state; the environment subsystem stays the single writer.

## 4. Decision: lighting source, camera look, studio rig (amends ADR 0009)

- **`EStageLightingSource { Level, OutdoorSky, Studio }`** replaces `bool bSkyEnabled`: one explicit state instead of a second boolean. `IsOutdoorSky()` serves the old callers.
- **`FStageEnvironmentLook`:** exposure (EV), bloom, AO, white balance, saturation, contrast, vignette, and the studio key light. Its defaults are the engine's own post-process defaults, so a stage that never set a look renders as before. It is applied to one unbound `UPostProcessComponent` (priority -1, so a level's own volumes still win where they set something) on a transient actor owned by the environment subsystem. Offline renders see it too, so a render matches the viewport.
- **`AStageStudioRig`:** a movable sky light from an HDRI cubemap (config `StudioCubemap`, `/Engine/MapTemplates/Sky/DaylightAmbientCubemap`, loaded asynchronously the first time the studio is shown, cooked via `DirectoriesToAlwaysCook`), a soft key light, and a light-grey height fog as a seamless backdrop that starts beyond the stage (volumetric, so beams still read). Without the cubemap it lights from a flat captured sky with a light lower hemisphere and the panel says so.
- **The level's own lighting** is hidden and restored by one shared helper, `FStageHiddenLevelLighting`, used by both the Sky Creator backend and the rig (moved out of the backend, no duplicate).
- **Scene format v5** writes `"lighting"` by name and a `"look"` object. v2–v4 files read `skyEnabled` as OutdoorSky or Level with the default look; an unknown name is Level; every look value is clamped.

## 5. Decision: blended transitions

`SetSettings(Settings, EStageEnvironmentTransition)`: `Instant` (scene load, undo and redo, scripts, sliders) or `Blend` (atmosphere cards). The stored settings, and so undo, saving and the broadcast, are the new ones at once; the blend is presentation only. The subsystem is a `UTickableWorldSubsystem` that ticks only while blending.

`StageEnvironment::BlendForDisplay` (pure, tested): numbers ease in and out over `BlendSeconds` (2 s); the time of day runs forward when the target is less than 12 h ahead (Daytime → Sunset sets the sun) and backward otherwise; the sun azimuth takes the short way. A change of lighting source cannot blend (lights appear and disappear), so the source switches at the midpoint inside a 6 EV exposure dip. A new change during a blend continues from what is on screen.

**Measured** (`StageCraft.Bench.AtmosphereBlend`, -game, 1600×900, all four atmospheres, Development editor binaries): frame 8.37 ms average / 20.0 ms worst while blending against 7.64 / 11.9 ms at rest; game thread 4.64 / 8.95 ms against 4.34 / 7.44 ms. So sending the interpolated settings to Sky Creator every frame is affordable; the planned fallback (weather only at the end) was not needed.

## 6. Decision: starter stages

- **Files:** four scene files in `Content/StageCraft/Templates` (Empty Stage, Concert, Theatre, Conference), staged as UFS (`DirectoriesToAlwaysStageAsUFS`) and read through the normal scene parser, so the trust boundary is unchanged.
- **Built through the app, not by hand:** `Scripts/Content/build_starter_stages.ps1` draws each stage as Sketch to Stage strokes (decks, truss runs, fixtures on the truss), generates it through the controller's build-plan request, applies an atmosphere and saves it, in a scripted -game run that restores the user's data. Every item's saved state is what the real spawn path writes. Only free catalog items are used. (Truss points snap to the 2 m section; truss lines are drawn on even metres so the fixtures hang on them.)
- **Opening one:** `UStageSceneComponent::LoadTemplate` loads like a saved scene, then gives every item a fresh id, clears the scene name (so Ctrl+S asks for one and the starter stage is never written) and the marketplace origin, and starts clean. It reports `EStageSceneOperation::OpenTemplate`; the controller then glides the camera (`UStageCameraNavigationComponent::FrameStage`: a three-quarter view from the audience over 1.2 s, the pawn's focus glide extended with rotation and a duration). The camera never moves on an atmosphere switch.
- **Where:** File > New from Template (with the same discard confirmation as Open Scene), `StageCraft.Scene.NewFromTemplate` (dev), and the welcome screen after startup: four cards, Continue (or Escape) and "Show at startup" (`UStageCraftUserSettings`). It is opened a quarter-second after the loader, never from inside the install (which runs in the engine's loading-screen callback), and while it is up the shortcut router stands aside (`IsModalOverlayOpen`). `-StageNoWelcome` (non-Shipping) skips it for scripted runs.

## 7. Alternatives rejected

- **Atmospheres as hard-coded tables** like the weather presets: content people want to tune belongs in assets.
- **Engine HDRI Backdrop plugin** for the studio: a plugin dependency and a visible dome mesh; the rig does the same with three components.
- **Hand-written template JSON:** item state is binary SaveGame data; only the real spawn path writes it correctly.
- **A per-atmosphere camera pose:** moves the user's view while they edit (Gevor's decision).

## 8. Consequences and open items

- **Saved before #58** files keep working; files saved now (v5) are refused by older builds (VersionMismatch), as with every format bump.
- **Not verified by hand:** the MoviePlayer screen at a real cold start (a packaged build would show it longest); the welcome cards and atmosphere cards by mouse; fixture beams under Cyberpunk Night with real fixtures (in the run, beams read in the night haze).
- **Not done:** the panel's sliders still write the environment directly rather than through the controller (pre-existing, PROJECT_AUDIT); quality/scalability settings; the main window title is still the project name.
- **Starter stages use one fixture type** (the Sketch tool's fixture item, Beam 280).
