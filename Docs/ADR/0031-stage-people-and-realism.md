# ADR 0031: Stage people (audience, band, crew) and stage realism

- **Status:** implemented by Claude in auto mode on Gevor's brief of 2026-11-06 (STATE.md #76), awaiting Gevor's approval.
  - The roadmap's I0 asked Gevor to choose an art source (MetaHuman, Fab or commissioned) first. The brief asked for "visual proxies" now, so this ADR builds procedural stand-ins on a contract that real art can fill later (§3.4).
- **Related:**
  - ADR 0005 (spans: the barricade is one, and props reuse the span's instanced parts).
  - ADR 0013 §3.2 (restored state is untrusted).
  - ADR 0022 (glTF export: people export in their rest pose).
  - ADR 0024 (starter stages, the welcome screen).
  - ADR 0029 (audio features: crowd energy is a follow-up, §8).
  - Rules.md: Architecture (state by lifetime, no idle tick), §21 (no polling), §54 (determinism), §55 (saved data), §65 (Shipping).
- **Roadmap:** `Docs/Roadmap/2026-11-05-realism-audio-assistant.md`, Phase I.

## 1. Problem

Gevor's brief asks for:
- realistic stage elements (trusses, decks, barricades, sound and lighting desks, background structures) drawn with instancing and LODs;
- a scalable animated crowd (vertex animation or GPU-instanced meshes, aiming at 120+ FPS);
- animated band members (drummer, guitarist, vocalist, keyboardist);
- animated crew at the desks and in the wings;
- no frame-time regression;
- two UI changes, recorded in §7.

The project has no character art. OverCrowd (disabled plugin) holds tooling only, no people. A skeletal mesh per person would cost an actor, an animation instance and a tick each.

## 2. Decision in one paragraph

**The figure.** One procedural humanoid made of 11 rigid segments, built in code in three LODs. Ten looping clips are evaluated by forward kinematics and baked into a small float **bone animation texture** (two texels per bone per frame).

**Drawing.** Every audience, band and crew item draws instances of that one mesh. `M_StageFigure` plays each instance's clip in the vertex shader (world position offset and turned normals) from per-instance custom data (clip, phase, rate, colours).

**Cost.** No actor, animation or tick per person. A crowd is one hierarchical instanced component, and a band member is a few draw batches.

**Items.** Props and set pieces are new catalog items assembled from instanced parts. The barricade is a span.

## 3. The figure (`StageFigureRig`, `UStageFigureAssetSubsystem`, `M_StageFigure`)

1. **Rig.** The 11 segments are pelvis, chest, head, upper and lower arms, and thighs and shins.
   - The pose is solved by forward kinematics from local joint rotations and a root offset.
   - The figure is 175 cm tall, faces +X, and stands on Z = 0.
   - Pure code, tested in `StageCraft.People.FigureRig`: rest pose, winding, loops, feet on the floor, cheering hands above the head, a seated drummer, clapping hands that meet.
2. **Clips** (`EStageFigureClip`, whose order is the texture layout):
   - audience: idle, bounce (on the beat at 128 BPM), cheer, clap;
   - band: drummer (seated), guitarist (also the bassist at half rate), vocalist, keyboardist;
   - crew: desk operator, stagehand.

   Each clip has 32 frames. The bounce lowers the hips by exactly what the bent legs lose, so the feet stay planted. The clap aims the arms at hand targets.
3. **Mesh.** Three LODs, at 348, 168 and 72 triangles. They switch at screen sizes 0.16 and 0.05, so most of a large crowd is LOD1 or LOD2.
   - UV channel 1 holds the bone (U) and the body region (V: skin, shirt, trousers, dark).
   - The region is in a UV, not a vertex colour, because a mesh build stores colours as sRGB bytes.
   - The bounds are extended for raised arms.
   - Collision is a simple box, used as complex too: a fast build keeps no CPU copy to cook a triangle mesh from.
4. **Contract.** The texture has 22 x 320 RGBA32F texels.
   - Texel 2b holds bone b's rotation quaternion; texel 2b + 1 holds its translation, so that `posed = Rotate(q, v) + t`.
   - Each row is one frame of one clip, in clip order.
   - Neighbouring frames are kept in one quaternion hemisphere, so the material can blend them.

   Art baked later in the same layout (for example with the engine's AnimToTexture in bone mode, from MetaHuman or Fab characters) replaces the mesh and the texture as data, with no code change.
5. **Built at runtime, once per process,** by an **engine** subsystem.
   - The assets come from code, so they cannot drift from it.
   - They exist in every world, including the glTF export world and editor worlds, which have no game instance.
   - The material is content (`Scripts/Content/create_people_content.py`).
     - It reads the texture with point samples in the vertex shader.
     - It blends two frames.
     - It turns the normal in the vertex shader and hands it to the pixel shader through a vertex interpolator, as a world-space normal.
     - It colours by region from the instance's shirt colour, skin tone and trouser shade.
6. **Per-instance custom data:** 8 floats (clip, phase, rate in loops per second, shirt RGB, skin tone, trouser shade).

## 4. Audience area (`AStageAudienceActor`, `UStageAudienceData`, `StageAudienceMath`)

- **Shape.** A rectangle of floor whose origin is the middle of its front edge. It runs back along +X, everyone faces -X, and the user turns it to face the stage.
- **State.** Four SaveGame values (width, depth, density in people per m², mood) and a seed, so a saved area shows the same crowd when it is opened again.
  - Restored values are sanitized against the item's ranges, and each repair is counted.
  - The size and density parameters are clamped. A changed parameter refills the crowd.
  - The four parameters are undoable shape edits, refused while the item is locked. People is read-only.
- **Fill** (pure and deterministic): count = floor(area x density), capped by the item (20,000) and at 5 per m².
  - People stand on a jittered grid, one per cell, so nobody overlaps (tested: closest pair over 18 cm at 2.5 per m²).
  - Each person gets a clip from the mood's mix, a random phase, ±15 % speed, ±7 % height, a turn of up to ±22°, a shirt from the item's palette, a skin tone and a trouser shade.
- **Moods:**
  - Calm: idle 80 %, clap 20 %.
  - Dancing: bounce 70 %, idle 15 %, cheer 10 %, clap 5 %.
  - Cheering: cheer 60 %, bounce 25 %, clap 15 %.
- **Drawing.** One `UHierarchicalInstancedStaticMeshComponent` (cluster culling and LOD), with no shadows, no distance fields and no ray tracing, and a cull distance of 300 m.
- **Picking.** A 2 cm box over the area's floor answers only the stage item channel.
  - Clicking the crowd selects it: the ray reaches the floor under a person.
  - Placement, camera pivots (which use Visibility) and fixtures above or beyond the crowd are never blocked.

## 5. Props, band, crew and set (`AStagePropActor`, `UStagePropData`)

- **Parts.** `FStageSpanPart` meshes at fixed transforms, drawn through `UStageSpanGeometryComponent::BuildParts`: one instanced component per mesh and material (the batching spans already use).
- **Figures.** One instanced component of the figure mesh. Figures answer only the stage item channel: clicking a performer selects the item, while placement and the camera look through them.
  - The hover and selection overlay is drawn on the parts only; on the figures it would show the rest pose.
  - Performers cast shadows; a crowd does not.
- **Items** (all made of engine cubes and cylinders, `create_people_content.py`):
  - band: drummer with a 5-piece kit, cymbals and throne; guitarist and bassist with their instruments and a wedge; lead vocalist with a stand and a wedge; keyboard player on an X-stand;
  - crew: FOH lighting position (desk, console, two screens, operator), FOH sound position, monitor position, stagehand with a road case;
  - backline: guitar amp stack;
  - set: 8 x 4.5 m LED wall on a ground-support frame (emissive, about 1,100 kg and 7.2 kW);
  - barricade: a span of whole 1 m sections, with the base plate on the crowd side.
- **No state of their own:** undo, scenes and duplicates need nothing new.

## 6. Performance

- **Gate.** The show tick and frame/GPU times of `Bench.Fixtures` with no people are compared against the commit before (A/B worktree, STATE.md #76).
- **Crowd budget.** `StageCraft.Bench.Fixtures ... Crowd=N` stands N people under the bench fixtures. The measured costs are in STATE.md #76.
  - The audience item's cap is 20,000 per area.
  - A packed crowd far away is mostly LOD2.
- **Per frame, a person costs no CPU.** Vertex work is 3 texture reads per vertex for the pose (rotation pair, translation pair) on up to 348 vertices.

## 7. UI changes in the same brief

- **No interface sounds.** `UStageEditorAudioFeedbackComponent` (selection, placement, snap and refusal cues) was removed with its cue table (`UStageAudioDeveloperSettings`), the four `SFX_*` assets, the `StageCraft.Sound.*` tags, the Effects volume preference and `StageCraft.Audio.Effects` / `.Play`.
  - Master volume and Mute stay; they govern the music the show follows.
  - Regression test: `StageCraft.People.NoUISounds`.
- **A starter stage shows on its own.** Opening a starter stage by any path (welcome screen, File > New from Template, the assistant, a console command) now calls `UStageWorkspaceSubsystem::PresentStarterStage`.
  - It closes the welcome screen, including one still pending at startup, and every panel in a floating window.
  - Docked panels stay, and nothing opens.
  - Before, a restored session brought back its floating windows (seven in Gevor's layout) over the new stage.

## 8. Not done here (follow-ups)

- Crowd energy from the music (ADR 0029 features driving the rate, through one material parameter at no more than 30 Hz).
- Band tempo locked to the detected BPM.
- Real character art through the §3.4 contract; haze machine and starter stages v2 (I3); packaged validation per template (I4).
- Audience shapes other than rectangles (a polygon like decks). Capacity lines in the reports and the bill of materials.
- A placement ghost for prop and audience items (like decks, they have none).
