# ADR 0037: MegaLights for fixture lighting

- **Status:** proposed, **blocked as written** (2026-11-26, STATE.md #110). Not applied: the project still renders with MegaLights off.
  - Enabling MegaLights for the project drops every fixture's light function: gobos, iris, blades, prism and frost disappear, and shaped beams light their whole cone (§7).
  - Gevor chooses one of the ways forward in §7.3 before anything is applied.
- **Related:**
  - ADR 0012 §3 (fixture benchmark and the light-rebuild workaround for moving heads).
  - ADR 0018 §4 and §4.1 (the shadow budget and its moving-caster limit, STATE.md #108).
  - STATE.md #106–#108 (where the 500-fixture crossfade spends its time).
  - Rules.md §22 (measure before optimizing), §49 (rule precedence; a project-wide rendering change is an architecture decision), §57 (packaged builds).

## 1. Problem

With the shadow budget (64 still / 16 moving), a 500-fixture rig on this machine (STATE.md #109, one trace each):

| | Frame | Bound by |
|---|---|---|
| Idle | 12.7–13.2 ms | Render thread (12.7 ms busy); the game thread waits 7.4 ms of each frame |
| Moving crossfade | 17.3–18.3 ms | Render thread (about 18 ms busy); the game thread does about 14.4 ms of its own work and waits the rest |

- The largest single render-thread cost, idle or not, is drawing the beams' lights: `RenderDeferredLighting` 3.7–4.2 ms, of which `BatchedLights` alone is 3.0 ms.
- Each of the ~490 unshadowed spot lights is a deferred light pass set up one by one.
- UI (Slate) and fixture game-thread work cannot shorten a frame the render thread sets.

## 2. Options measured

All runs use `StageCraft.Bench.Fixtures 500 Seconds=10 Fade=2` in a `-game` window at 1600×900, D3D12, on the development PC.

| Option | Idle frame | Moving crossfade frame | Notes |
|---|---|---|---|
| Current (MegaLights off, budget 64/16) | 13.2 / 13.6 / 13.2 ms (GPU 9.6–9.8) | 17.3 / 18.6 / 17.7 ms | three runs in one session |
| `r.UseClusteredDeferredShading_ToBeRemoved 1` | — | — | **The game crashed** on the first frame ("Shader compilation failures are Fatal", Foreground Worker). The path is deprecated in 5.7+ and marked for removal. Rejected. |
| `r.MegaLights.Enable 1` at runtime only | 13.2 ms | 17.7 ms | no effect: MegaLights must be enabled for the project at startup |
| **MegaLights for the project** (`r.MegaLights.EnableForProject=1`, `r.MegaLights.Enable=1`), budget 64/16 | **8.0 / 8.0 ms** (GPU 6.3) | **14.0 / 14.7 ms** | the crossfade becomes game-thread bound (game 14.0 = frame) |
| MegaLights, shadow budget off (all 502 cast) | 8.1 ms (GPU 6.7) | 23.2 ms | moving shadowed lights still cost render-thread time |
| MegaLights off, shadow budget off | 16.5 ms (GPU 11.3) | 46.9 ms | for reference |

- **Screenshots of the default view** (`Window_191237/191244` without, `Window_191356/191403` with, in `%TEMP%/StageCraftRuns/Shot*`) differ by 40–42 dB PSNR, which is noise-level.
- That view shows the rig from above and mostly the floor. It is **not** a close look at beams, pools, gobos or shadows, so it does not prove the look is unchanged.

## 3. Proposed decision

Enable MegaLights for the project and keep the shadow budget (64 still / 16 moving):
- `DefaultEngine.ini` `[/Script/Engine.RendererSettings]`: `r.MegaLights.EnableForProject=True`.

**Why:**
- It removes the largest render-thread and GPU cost of a lit rig without touching any fixture or show code: 5 ms off the idle frame and 3–4 ms off the moving crossfade at 500 fixtures.
- The budget still matters: with every fixture shadowed, moving crossfades cost 23 ms even under MegaLights.

## 4. Before accepting, check

1. **The look:**
   - close-ups of beams, pools and gobos (light functions);
   - shadows from the 64 still casters;
   - the haze and volumetric look;
   - noise or smearing of MegaLights' stochastic sampling on moving beams.
   This needs Gevor's eye, not a PSNR number.
2. **Hardware:** MegaLights needs SM6. Decide what lower-end GPUs get, for example a scalability fallback to MegaLights off, and verify that fallback.
3. **Packaged build:** shaders cook with MegaLights; run the bench in a packaged Shipping or Development build (Rules §57).
4. **Engine status** of MegaLights in UE 5.8 (experimental or beta) and its known limitations for many small spot lights with light functions.
5. **Tests:** the suite runs `-NullRHI`, so it is unaffected. Add a bench-based check (frame and GPU) to the A/B record.

## 5. Consequences

- **If accepted:**
  - The idle rig and moving crossfades run 3–5 ms faster on this machine.
  - The moving crossfade becomes game-thread bound, so the game-thread items from STATE.md #106 (pivot propagation about 3.9 ms, deferred render updates about 2.5 ms, moving-part bodies 1–2 ms) become the next measured costs.
  - Slate (about 3 ms of game thread) then matters too.
- **If rejected:**
  - The render thread stays the bound. The game-thread and UI items cannot shorten frames on this class of machine and are not worth changing yet.

## 6. Rollback

Remove `r.MegaLights.EnableForProject` (or set it False) in `DefaultEngine.ini`; nothing else depends on it.

## 7. Pre-accept checks (2026-11-26, STATE.md #110)

### 7.1 The look: light functions are lost (blocking)

- **A/B:** the starter stage's two Spot fixtures, tilt 115°, dimmer full; Spot 101 on gobo 2 (star), Spot 102 iris 0.6; top-down camera; run with and without the two MegaLights switches.
  - **Without MegaLights:** the star gobo and the small iris pool project as expected.
  - **With MegaLights:** Spot 101 lights a full, much brighter open cone with no star, and Spot 102 lights its full 30° cone instead of the iris pool.
  - Screenshots: `%TEMP%/StageCraftRuns/LfBase2/Window_101304.png` (without) and `%TEMP%/StageCraftRuns/LfMega2/Window_101346.png` (with).
- **Cause (UE 5.8 source):**
  - MegaLights samples a light function only through the light function atlas.
  - The atlas takes only materials the material compiler marks compatible: no vertex position, scene depth or scene textures, and no texture coordinates fed into the graph (`HLSLMaterialTranslator.cpp`, `bIsLightFunctionAtlasCompatible`).
  - `M_StageBeamLightFunction` feeds a TextureCoordinate into its Custom node (the cone coordinates), so it is marked incompatible. The light then gets atlas index 0, which means no light function (`LightFunctionAtlas.cpp`).
  - The light function also narrows the light's cone to the beam angle (`ConeToBeam`), so even open-but-zoomed beams change.
- **Also limiting even with a compatible material:**
  - The atlas keys slots by material instance (`FLightFunctionSlotKey::LFMaterialUniqueID`).
  - Every fixture with beam shaping has its own dynamic instance, so each needs its own slot.
  - The atlas holds 4×4 = 16 slots by default (`r.LightFunctionAtlas.Size`), and at most 16×16 = 256.
  - Each slot is 128×128 texels by default (`r.LightFunctionAtlas.SlotResolution`): a gobo is rasterised once into its slot instead of evaluated per pixel, which is softer close up.
  - Lights over the slot count silently lose their light function.
- **The #109 performance numbers** come from `StageCraft.Bench.Fixtures`, whose fixtures project no gobo. They do not include light functions.

### 7.2 The other checks

- **Hardware:**
  - The project already targets SM6 only (`DefaultEngine.ini`: D3D12 `PCD3D_SM6`, Vulkan `SF_VULKAN_SM6`; SM5 removed), so "GPUs without SM6" are not a new MegaLights constraint.
  - MegaLights compiles only where the shader platform has SM6, wave operations and ray tracing (`ShouldCompileMegaLightsShaders`).
  - At runtime it needs hardware ray tracing or distance-field software tracing (`HasRequiredTracingData`). Without either, `MegaLights::IsEnabled` is false and the lights fall back to the normal deferred path, with an on-screen warning in non-Shipping builds.
  - **The scalability fallback already exists:** `r.MegaLights.Allowed` is an `ECVF_Scalability` variable. The engine's MegaLights settings live under `[ShadowQuality@N]` (`BaseScalability.ini`), so a project `DefaultScalability.ini` can set `r.MegaLights.Allowed=0` for low shadow quality. Not added (nothing is enabled).
- **Engine status:** UE 5.8 still calls MegaLights an "Experimental feature" (help text of `r.MegaLights.EnableForProject`).
- **Packaged build:** not run. Benching a packaged build is pointless until §7.1 is solved, because the bench does not exercise light functions.

### 7.3 Ways forward (Gevor decides)

1. **Keep MegaLights off (no change).** The render thread stays the bound, about 13 ms idle and 17–19 ms moving crossfade at 500 fixtures on this machine.
2. **Hybrid: only light-function-free beams use MegaLights.**
   - A fixture turns `bAllowMegaLights` off on its spot light while its light function is attached, and back on when it detaches. The engine reads `ULightComponent::bAllowMegaLights` per light. It has no runtime setter: set the member and rebuild the light's render state, as the fixture already does after head moves.
   - Gobos and shaping then look exactly as today; only open beams take the MegaLights path.
   - The gain shrinks as more fixtures project gobos, so measure with a gobo-heavy bench.
   - This is a small code change (fixture actor plus a test) and needs the project setting as well.
3. **Make the beam material atlas-compatible.**
   - Set the material's "Compatible With Light Function Atlas" flag, then check the look.
   - Raise `r.LightFunctionAtlas.Size` to 16 and possibly the slot resolution.
   - Share instances between fixtures with identical shaping, or accept that more than 256 shaped fixtures lose their gobos.
   - This is the largest change, with a visible quality risk (128-texel gobos).

- **Recommendation:** option 1 now. Option 2 if Gevor wants the open-beam gain, measured first with gobos on part of the rig; it keeps today's look by construction.
