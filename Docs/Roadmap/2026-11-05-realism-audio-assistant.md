# Roadmap: audio-reactive lighting, assistant v2, realistic stages and crowds

- **Requested by:** Gevor, 2026-11-05.
- **Written:** 2026-11-05 at commit `1594875` (Phase F complete). STATE.md #67.
- **Status:** proposed. Nothing here is approved or built.
- **Design records:**
  - ADR 0029 (audio analysis and audio-reactive effects, proposed);
  - ADR 0030 (assistant v2, proposed);
  - ADR 0031 (stage realism and crowds, to be written in I0 once the asset decision below is made).

## 1. Where the project stands (scanned 2026-11-05)

| Area | State today | What the brief adds |
|---|---|---|
| Console | F1–F5 done: programmer, palettes, cue timing and tracking, 10 playbacks, effects engine (cycle clocks, TAP), Art-Net / sACN in and out. 189 tests. | Music as a clock and a modulation source for the existing effects |
| Audio | Microphone capture for voice prompts (`AudioCaptureCore`), UI feedback sounds. No analysis, no music playback. | File, input and system-audio sources; bands; beat and BPM |
| Assistant | Chat panel, offline EN grammar with Armenian, voice, ~30 intents, optional Claude backend with **one tool, one command per prompt**. Phase 26 (knowledge base, multi-step, memory) planned, **not built**. Gevor 2026-10-30: offline is the full default; Claude extends it. | Knowledge of every control, live context, diagnostics, multi-step actions |
| Stages | Four starter stages built through the app by script (one fixture type, Beam 280), four atmospheres, studio rig, volumetric fog for beams. | Full truss rigs with mixed fixtures, haze, crowd, band |
| Characters | None. The **OverCrowd** plugin (VAT crowds, third-party, beta, built for 5.7) is in `Plugins/` but disabled. UE 5.8 ships AnimToTexture, MassCrowd and MetaHuman plugins. | Crowd and band meshes with idle animation |

### Performance baselines that exist

The brief says "120+ FPS baselines". **No 120 FPS baseline is recorded for StageCraft.** The measured numbers are:

| Measurement | Result | Where |
|---|---|---|
| Camera flight on `L_StageTest` | about 110–120 fps | #47 |
| 200 crossfading fixtures | 14.2 ms a frame | #39 |
| 500 crossfading fixtures | 27–33 ms a frame (render thread: light proxy updates of 500 shadowed movers) | #39, #48 |
| Atmosphere blend | 8.4 ms average | ADR 0024 §5 |
| F5 show tick, idle with DMX OUT | one tick in 10 s | #65 |

**The gate for every phase is "no regression against the commit before it"**, using `Bench.Fixtures` A/B in the `E:/SC_Baseline` worktree (with the workspace copied in), plus a new bench per feature with its own budget (below).

A fixed 120 fps target on a 500-fixture rig is not reachable with today's light count. If Gevor wants a 120 fps target, it needs to name a reference scene and GPU, and it becomes its own performance phase (beam LOD, shadow budget below 64, per-cell lights).

## 2. Recommended order

1. **Phase G: audio-reactive lighting** comes first.
   - It is engineering we fully control, it builds directly on F4, and it is testable with synthetic signals.
   - The crowd and the assistant both want its output: crowd energy, and "what BPM is it?".
2. **Phase H: assistant v2** comes second.
   - It needs every console feature to exist before its knowledge base and tools describe them, audio included.
   - It absorbs the unbuilt Phase 26.
3. **Phase I: stage realism** comes third for code. Its **I0 asset decision should start now**, because sourcing art has the longest lead time and I cannot produce realistic character meshes myself.

G and the start of I0 can overlap. H and I are independent of each other.

## 3. Phase G: audio-reactive lighting (ADR 0029)

| Step | Content | Exit criteria |
|---|---|---|
| **G0 spike** (throwaway branch) | WmfMedia MP3/WAV/FLAC from a file URL in a **Shipping** build, routed to a submix and tapped; a WASAPI loopback prototype; latency of each source; AudioSynesthesia real-time onset compared with our own | A short note in ADR 0029 §2 (A6 confirmed or the fallback chosen); latency numbers |
| **G1 sources and analyser** | `FStageAudioTap` (SPSC ring), `FStageAudioAnalyzer` thread, pure `StageAudioAnalysis` (bands, flux onsets, tempo, PLL phase), `UStageAudioReactiveSubsystem`, File / Input / SystemAudio / TestSignal, per-PC settings, controller requests, dev commands | Tests Audio.Bands, Audio.Tempo (±1 BPM at 90–174), Audio.RingBuffer, Audio.Analyzer; analyser under 2 % of a core; Editor, Game and Shipping 0 warnings |
| **G2 effects integration** | `FStageEffectAudio` (Sync, Modulate, Trigger), runner reads one snapshot per advance, scene format v11, Effects panel AUDIO row, six audio quick effects, Audio panel (meters, BPM, beat lamp, transport, latency) | Tests Show.AudioEffects and Scene.EffectAudioData; `Bench.Fixtures Effects=1 Audio=1` within 5 % of `Effects=1`; scripted run with TestSignal at 128 BPM shows a beat chase on the beat |
| **G3 safety and polish** | Flash limiter (3 Hz default), AUDIO master and KILL, the "Listening" indicator, latency offset, status bar BPM | Limiter test at 3 Hz; Shipping UTF-16 scan (dev commands absent); manual: real music through each source (Gevor) |

**Not in G:** songs saved with a show, beat-triggered cue GOs, verse and chorus detection, MIDI clock and Ableton Link. These are follow-ups in ADR 0029 §11.

## 4. Phase H: assistant v2 (ADR 0030; absorbs Phase 26)

| Step | Content | Exit criteria |
|---|---|---|
| **H0 spike** | HTTP response streaming in UE 5.8; prompt-cache hit on the second prompt; time to first token and cost per prompt for Opus 5.5 low effort against Sonnet 5.5 and Haiku 5.5 on 20 scripted prompts | Numbers for Gevor's model choice; streaming confirmed or the non-streaming path kept |
| **H1 knowledge** (offline) | UI anchor tables in every panel (tooltips read from them), `FStageKnowledgeIndex` from the key map, parameters and `DA_Panel_*`, help articles EN/HY written from the ADRs, `StageKnowledgeSearch` (BM25), SHOW ME pulse | Tests Assistant.Knowledge and Assistant.Search (~120-question EN/HY corpus, top-3) |
| **H2 diagnostics and "why"** | `StageShowDiagnostics`, `StageFixtureExplain`, Problems list in the Patch and Cues panels, offline answers to "why is X dark / not moving / not patched" | Tests Show.Diagnostics and Show.Explain; useful with the assistant off |
| **H3 tools and loop** | Read and action tool catalogue, strict schemas, proposal card, one-undo-step plans with rollback, limits, cancel; Claude backend as a bounded loop with append-only history and a cached prefix; key in the vault (Services & Keys) | Tests Assistant.ToolSchema and Assistant.Loop (mock backend, no network); a manual online session (Gevor's key) |
| **H4 offline multi-step and memory** | The rest of Phase 26: the splitter, conversation memory, new actions (selection, cue update and delete, BOM answers, export completion in chat) | Test Assistant.MultiStep; EN/HY corpus extended |

**Open for Gevor:** the default online model, and how a shipped product gets a key (bring-your-own, as now, or a StageCraft proxy server). ADR 0030 §8–9.

## 5. Phase I: realistic stages, crowd and band (ADR 0031 in I0)

| Step | Content | Exit criteria |
|---|---|---|
| **I0 decisions and spike** | **Asset source** (§6). **Crowd technique**: vertex-animation textures on instanced static meshes, baked editor-side with the engine's AnimToTexture, played by our own material. Count, LOD, shadow and GPU cost measured at 1k, 5k and 20k instances on Gevor's GPU. OverCrowd evaluated, and dropped unless it clearly wins (third-party beta, built for 5.7). | ADR 0031 written and approved; crowd GPU budget set from measurements |
| **I1 audience area** | A placeable **Audience** item: a floor outline like decks, density per m², filled with crowd instances (no actors, no tick, per-instance random clip, offset and tint), capacity counted in the BOM and reports; crowd energy from ADR 0029 features through one material parameter at ≤ 30 Hz (jumping on the beat) | `Bench.Crowd` at the budget; tests for fill maths (density, outline, capacity) and save and restore |
| **I2 band and backline** | Band members as catalog items: skeletal mesh, one looping performance clip per instrument, no Animation Blueprint, rate optionally locked to the BPM; backline props (drum riser, amps, keyboard, mic stands) | Visual check; frame cost of 5 performers measured |
| **I3 haze and starter stages v2** | Haze machine item (local fog volume, density and spread as undoable parameters, environment subsystem stays the fog writer); starter stages rebuilt by script with mixed fixtures (spots, washes, beams, blinders, pixel bars), multiple truss layouts and atmospheres: Arena, Festival, Club, Theatre, Conference | Templates open from the welcome screen; benchmark per template recorded |
| **I4 packaged validation** | Cooked Shipping build: templates, crowd, band and audio together | Frame times recorded per template; Shipping 0 warnings |

## 6. Decisions needed from Gevor before the code phases

1. **Approve ADR 0029** (G).
   - System audio instead of YouTube URLs.
   - The 3 Hz flash limit on audio-driven intensity.
   - Scene format v11.
2. **Approve ADR 0030** (H).
   - The bounded loop with proposal cards.
   - Diagnostics as pure code.
   - The model and key policy.
3. **Crowd and band art source** (I0). Options:
   - **(a) MetaHuman Creator** characters: free in UE projects, very realistic, heavy. Fine for 3–6 band members, and baked to VAT for a crowd.
   - **(b) Fab marketplace** crowd and character packs: cost and licence per pack.
   - **(c) A commissioned low-poly set.**

   Realistic results depend on this choice. Code alone cannot make a convincing human.
4. **Performance target.** Keep "no regression plus a per-feature budget", or name a reference scene, resolution and GPU for a hard frame-rate target.

## 7. Risks

| Risk | Mitigation |
|---|---|
| WmfMedia cannot play MP3 in Shipping | G0 checks it first; WAV and FLAC by our own reader; MP3 as a follow-up |
| System audio latency makes lights lag | latency offset setting; beat phase prediction from the PLL |
| Beat tracking fails on rubato or ambient music | confidence shown; Sync falls back to the last tempo or TAP; Modulate still works without a beat |
| Assistant cost and latency online | the offline engine answers most questions; cached prefix; effort low; model choice in H0 |
| The model proposes harmful or confusing plans | closed tool set, strict schemas, GameMode rules, confirmation, one undo step |
| Crowd GPU cost on large audiences | VAT instancing, LOD and culling, no crowd shadows past near range, density cap from the I0 budget |
| Third-party plugin drift (OverCrowd 5.7 beta) | not a dependency unless chosen in ADR 0031 |
