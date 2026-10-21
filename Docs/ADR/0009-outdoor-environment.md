# ADR 0009: Outdoor environment (sky, time of day, weather) and viewport toolbar sections

- **Status:** accepted, implemented. See STATE.md #35.
- **Related:** ADR 0001 (dockable workspace, panel definitions), ADR 0004 (render subsystem, the panel pattern this follows), ADR 0008 §4.6 (viewport toolbar). Rules.md "Where state lives", "Safe data binding", Engineering Standard §9, §16, §21, §24, §28, §38, §57, §65.

## 1. Problem

Gevor asked (2026-10-20) for:

1. **Toolbar sections.** The viewport toolbar split into five clearly separated sections with subtle vertical dividers and uniform padding: projection | view modes | grid snap | angle snap | gizmo.
2. **Weather and environment.** Integrate the available weather plugin for outdoor stages: sun angle, time of day, cloud density, rain and snow particles, atmospheric fog. The settings are edited in a dedicated Environment panel, with no per-frame polling.

## 2. Verified facts

| # | Fact | Source | Consequence |
|---|---|---|---|
| E1 | Two weather plugins are in `Plugins/`, both disabled since #30. **SkyWeatherControls** only drives *Ultra Dynamic Sky / Weather* through their Blueprint libraries, and UDS is not in the project. **Sky Creator** (`Plugins/SkyCreat`, marketplace, C++ source included) is a complete sky: sun, moon, sky atmosphere, volumetric clouds, sky light, height fog, Niagara rain / snow / lightning, all on one actor (`ASkyCreator`). | Plugin descriptors and sources | Sky Creator is the weather system. SkyWeatherControls stays disabled. |
| E2 | Sky Creator's source compiles on UE 5.8 without edits. Its own header raises 13 MSVC C4305 warnings (double literals in float default arguments). | Build logs, STATE.md #35 | No plugin source is edited. Our module silences C4305 around its includes only. |
| E3 | The descriptor says `"Installed": true, "EngineVersion": "5.7.0"`. For an installed plugin whose engine version does not match, 5.8 logs "requires engine version '5.7.0'… Skipping load". The plugin's **content is not mounted**, yet its module still loads when another module links to it: the CDO then fails to find every texture, material and Niagara system. | `LogPluginManager` in the first headless run | The descriptor's `EngineVersion` is set to `5.8.0`, the only change to the plugin. |
| E4 | `ASkyCreator` ticks: cloud wind, occlusion captures, Niagara simulation stepping and the camera-position MPC. Its `Set*Settings` functions apply immediately, and `GetWeatherSettings` returns what was applied. | `SkyCreatorActor.cpp` | Our code never ticks. The actor exists only while the outdoor sky is on, so an indoor stage pays nothing. |
| E5 | The sky actor finds its assets by path in its constructor and at runtime (`ConstructorHelpers`, `LoadObject`). Nothing in `/Game` references them. | `SkyCreatorActor.cpp` | Its asset folders are added to `DirectoriesToAlwaysCook`. Demo, Maps and Sounds are left out. |
| E6 | Exponential height fog density is applied as density / 1000 per cm. 0.02 (the engine default) is a distant haze; about 1.0 gives tens of metres of visibility. The engine clamps it to [0, 10]. | `ExponentialHeightFogComponent.cpp`; measured in screenshots | `StageSkyCreator::MaxFogDensity = 1`, applied with Fog squared. A first value of 0.08 was invisible at stage distance (ScreenShot00018). |
| E7 | `UImage::SetDesiredSizeOverride` forwards to the Slate image only if it already exists. It is not stored. Code-built trees run before Slate is built (`NativeOnInitialized`), so the call is dropped and the image has the brush's size, which is zero for a box brush. | `UMG/Private/Components/Image.cpp` 122-127 | The toolbar dividers had never drawn (0 px wide, measured). Divider size now goes into the brush (`StageCraftWidgetStyle::MakeLine`). Other panels' 1 px header lines have the same bug: follow-up. |

## 3. Decision: a backend boundary, with Sky Creator in its own module

### 3.1 Layers

| Piece | Module | Role |
|---|---|---|
| `FStageEnvironmentSettings`, `EStageWeatherPreset`, `StageEnvironment::Sanitize / ApplyWeatherPreset / FindWeatherPreset` | ModularSceneBuilder | Backend-neutral, normalized state and its pure rules: wrap time and azimuth, clamp the rest, non-finite → default. |
| `UStageEnvironmentBackend` (abstract `UObject`) | ModularSceneBuilder | The contract: `Apply(World, Settings)` (sanitized, only on change; sky off = remove and restore everything), `Shutdown()` (once, at world end, idempotent), `GetDisplayName()`. Game thread only. |
| `UStageEnvironmentSubsystem` (`UWorldSubsystem`, Game + PIE, `Config=Game`) | ModularSceneBuilder | Source of truth. `SetSettings` is the single write path: sanitize → refuse sky-on without a backend → skip if unchanged → store → `Backend->Apply` → broadcast `OnEnvironmentChanged`. Creates the backend from `BackendClass` in `DefaultGame.ini`. Calls the backend only after `OnWorldBeginPlay`. |
| `UStageSkyCreatorBackend` + `StageSkyCreator::MakeWeather` | **StageCraftSkyCreator** (new game module) | Spawns one transient `ASkyCreator` on sky-on, hides the level's own sky components, maps the settings, destroys the actor and restores the level on sky-off. |
| `UStageEnvironmentPanel`, `UStageValueSlider` | ModularSceneBuilder | The Environment panel (§4). |

This is the same split as DMX (`UStageDMXBridge`, STATE.md #9): the core never links to a third-party plugin. To remove Sky Creator: delete `BackendClass` from `DefaultGame.ini`, remove the module from the `.uproject` and the targets, and disable the plugin. The panel then says no sky system is installed.

### 3.2 Mapping (StageSkyCreator::MakeWeather)

The base is the sky actor's own default preset (`Weather_Default`), read once at spawn. Only these values are overwritten; Sky Creator's tuning (cloud lighting, particle looks, wind, atmosphere) stays.

| Setting | Sky Creator |
|---|---|
| CloudCoverage *c* | Cumulus = Stratocumulus = *c*; Stratus = 0 up to 60 %, rising to 1 at 100 % (overcast); Cumulonimbus 0; background cloud layers A–C = *c* |
| Rain *r* | `WeatherFXSettings.RainAmount` = *r*; `MaterialFXSettings.WetnessAmount` = *r* |
| Snow *s* | `WeatherFXSettings.SnowAmount` = *s* |
| Fog *f* | `ExponentialHeightFogSettings.FogDensity` = 1.0 × *f*² (E6) |
| TimeOfDay, SunElevation, SunAzimuth | `SetSunSimplePositionSettings(06:30, 19:30, elevation, azimuth)`, then `SetTime` |

### 3.3 The level's own sky

With the sky on, the level's directional lights, sky lights, sky atmospheres, volumetric clouds and height fogs are hidden in one pass over the world, done when the sky turns on. This is not a poll. They are made visible again when it turns off. Otherwise the stage would be lit and fogged twice. During world teardown the backend leaves both the actor and the components to the world (it checks `bIsTearingDown`).

### 3.4 Alternatives considered

- **SkyWeatherControls.** It requires Ultra Dynamic Sky, which is not in the project (E1).
- **A StageCraft-built sky** from engine components (sky atmosphere, volumetric clouds, height fog, own Niagara rain and snow). That is a larger job and would duplicate a system the project already owns. Rejected.
- **Linking the core module to SkyCreatorPlugin directly.** Smaller, but it couples the core to a marketplace plugin that needed a descriptor fix to load on 5.8. Rejected for the backend boundary, which has a precedent in DMX.
- **Placing the sky actor in the map.** That would replace the indoor night look every stage starts with. Rejected for the opt-in switch, default off.

## 4. Environment panel

`StageCraft.Panel.Environment` (`DA_Panel_Environment`, made headless by `Scripts/Content/create_environment_panel_content.py`). It is a tab after Render in the Default layout.

- **OUTDOOR SKY**: Off / On.
- **WEATHER** presets: Clear, Cloudy, Rain, Snow, Fog. They set clouds, rain, snow and fog only. The lit button is the preset the values match; none is lit after a manual edit (Custom).
- **SUN**: Time of day (shown as HH:MM, 00:00–23:59), Sun height (5–90°), Sun direction (0–359°).
- **CONDITIONS**: Clouds, Rain, Snow, Fog (%).

Sliders write on every move through `SetSettings`, and the panel redraws only from `OnEnvironmentChanged`. Slider tops stop one step short of the wrap point (23:59, 359°). Otherwise a value at the very end would read back as 0 and the handle would jump to the left end mid-drag. Without a backend every control is disabled and the panel says so. With the sky off, only Off / On is live.

`UStageValueSlider` is a never-focusable `USlider` that reports its index (like `UStageChoiceButton`). `SetValue` does not fire `OnValueChanged`, so the read-back cannot loop.

Development console commands (compiled out of Shipping): `StageCraft.Environment.Status`, `StageCraft.Environment.Set <Sky|Time|SunHeight|SunDirection|Clouds|Rain|Snow|Fog> <value>` and `StageCraft.Environment.Preset <name>`. They all go through `SetSettings`.

## 5. Toolbar sections

`UStageViewportToolbar::BuildTree` adds one divider between each pair of sections, with `SectionGap` (8 px) on both sides. Each section's first widget is added with the same gap. The divider is a 1 × 14 px line at half-strength `TextDisabled`, one step brighter than the button outlines. Its size is in the brush (E7).

## 6. Consequences

- New game module `StageCraftSkyCreator`, listed in the `.uproject` and in both targets' `ExtraModuleNames`.
- The packaged build grows by Sky Creator's materials, effects, meshes, curves, textures and presets (E5).
- Sky Creator adds GPU memory and per-frame cost while the sky is on (volumetric clouds, captures, particles). Not measured yet (§7).
- Environment settings live for the session only. They are not saved in scene files.

## 7. Open items

- Fix the dropped `SetDesiredSizeOverride` in the other code-built panels (E7).

Closed by STATE.md #37 (ADR 0010):
- Environment settings are saved with scenes (scene format version 2).
- The sky's cost was measured in a packaged Development build. Rendering was at 1184×657 in a 1600×900 window, with a second editor sharing the GPU. Means are over 300 frames, from two runs.
  - Sky off: GPU 4.4–4.5 ms, about 1.9 GB local video memory in use.
  - Sky on, clear: GPU 6.6–6.9 ms (+2.3 ms), +180 MB.
  - Sky on, Rain preset: GPU 9.0–9.6 ms (+4.8 ms), +285 MB.
- The packaged build contains all six Sky Creator content folders (259 assets). One engine cook dependency was added: `/Engine/EngineSky/VolumetricClouds`, which the cloud component loads as its default.
- Sky Creator's `MF_SC_BlueNoiseDithering` references `/Engine/EngineMaterials/STBlueNoise_scalar_128x128x64`, which no longer exists in 5.8. Only `MF_SC_Triplanar_SingleSample` uses that function, and no asset uses that one, so nothing that renders is affected. The plugin content is left unchanged.
