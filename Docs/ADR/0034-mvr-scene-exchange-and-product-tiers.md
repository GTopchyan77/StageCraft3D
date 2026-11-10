# ADR 0034: MVR scene exchange, and the Free / Pro product tiers (D1–D8)

- **Status:** accepted for the tiers (§2, Gevor locked D1–D8 on 2026-11-10); **proposed** for the MVR design (§3–§10). M1–M4 in §3 are recommendations waiting for Gevor; MV0 can start on the recommended answers because none of them changes the reader.
- **Related:**
  - ADR 0033 (GDTF import, runtime fixture types, the device library). MVR builds directly on it: an `.mvr` carries its fixtures as `.gdtf` files.
  - ADR 0019 (patch, DMX IN/OUT), ADR 0006 / 0013 §3.1 (runtime model import), ADR 0022 (DCC bridge, `.glb` export), ADR 0021 / 0032 (packages, cloud).
  - STATE.md #84 (D1–D8 proposed), #86 (this ADR).
  - Rules.md: Architecture (request bridge, one writer), Economy security §8 (entitlements, not objects), Engineering Standard §26 (versioned serialization), §55 (trust boundary), §65–66 (Shipping and security boundaries).
- **Roadmap change:** MVR moves to **Step 2**, ahead of MIDI/OSC (now Step 3), because it builds directly on GDTF (Gevor, 2026-11-10).

## 1. Problem

Real productions exchange whole rigs, not single fixtures. Vectorworks Spotlight, Capture, WYSIWYG, grandMA3, Depence and others read and write **MVR** (My Virtual Rig, DIN SPEC 15801, current version 1.6). An MVR file holds the venue's geometry, the trusses, every fixture with its GDTF type, DMX mode, patch and position, and focus points. Without MVR a StageCraft user has to rebuild a designer's plot by hand, and cannot hand a StageCraft design to a console or a CAD tool.

## 2. Product tiers (D1–D8, locked 2026-11-10)

Gevor locked D1–D8 as proposed in the 2026-11-09 conversation (STATE.md #84). The proposal text itself was not saved in the repository; what was restated on 2026-11-10 is recorded here word for word in substance:

| | **Free** | **Pro** |
|---|---|---|
| DMX OUT | **1 universe** | **Unlimited universes** |
| DMX IN | – | ✓ |
| GDTF import | ✓ | ✓ |
| Design (stage, rigging, lighting, cues, paperwork) | ✓ | ✓ |
| MIDI / OSC | – | ✓ |
| Web Remote | – | ✓ |
| MVR export | – | ✓ |

The other decisions cover pricing (D3), licence enforcement and offline use (D4), the payment provider and legal entity (D5), online fixture sources (D6), the scope and security of the control protocols (D7), and the step order (D8: GDTF → **MVR** → MIDI/OSC → Download Manager → Web Remote → Stripe licensing). **Their exact wording is not recorded here and must be restated by Gevor before Step 6 (licensing) depends on it.** This ADR does not invent it.

### 2.1 Consequences of the tiers for what exists today
- **DMX IN (#47) and multi-universe DMX OUT (#65) are free today.** They become Pro. Nothing is removed until licensing exists (Step 6). Until then the build behaves as Pro, and the gates below are written but open.
- **Gates are entitlements, not checks scattered in code** (Rules.md Economy security §8). One exact tag per Pro capability, under `StageCraft.Entitlement.Pro.*`: `DmxUniverses`, `DmxIn`, `MidiOsc`, `WebRemote`, `MvrExport`. The GameMode's evaluators (`EvaluateParameterChange` for patch, the DMX output/input requests, the MVR export request) refuse with a clear reason when the tag is missing. Display may hide or mark a Pro control, but display is not permission.
- **The client is not the authority.** A local entitlement deters casual use only. Step 6 must validate the licence on the server (`UStageCommerceBackend`), as for any paid content (Rules.md Economy security §7, §66).
- **The Free universe limit counts universes in use by DMX OUT**, not patched fixtures, so a Free user can design a 10-universe rig and output one universe of it. The Patch panel shows which universe is live.

## 3. MVR decisions only Gevor can make

| # | Question | Recommendation |
|---|---|---|
| M1 | Is MVR **import** Free or Pro? D1–D8 name only export. | **Free.** Import is how designers bring work in, and GDTF import is Free. Export stays Pro. |
| M2 | Geometry in v1: trusses and scene objects as their `.glb` meshes (runtime model import, ADR 0006), or as primitives sized from their bounds? | **`.glb`/`.gltf` meshes through the existing runtime importer**, primitives when the file is `.3ds` (deprecated in MVR 1.6, and our importer does not read it), with the import report saying so. |
| M3 | What a StageCraft truss or deck becomes on export: an MVR `Truss` / `SceneObject` with a `.glb` from the DCC bridge exporter (ADR 0022), or a `SceneObject` with no geometry? | **`Truss` / `SceneObject` with `.glb` geometry** from the DCC bridge exporter, so CAD tools see the set. |
| M4 | MVR-xchange (network sharing over mDNS / WebSockets). | **Not in v1.** It is a network protocol with its own security questions (D7). Revisit after Web Remote. |

## 4. Format facts (verified 2026-11-10 against github.com/mvrdevelopment/spec `mvr-spec.md`)

- **Container:** ZIP, STORE or DEFLATE only, no encryption, every file at the archive root (no folders), file names unique ignoring case. The mandatory root file is `GeneralSceneDescription.xml` (UTF-8).
- **Root:** `GeneralSceneDescription` with required `verMajor`, `verMinor` (1.6 current), `provider`, `providerVersion`; children `UserData` (0..1) and `Scene` (1).
- **Scene:** `AUXData` (Symdef, Position, MappingDefinition, Class) and `Layers` → `Layer` (`uuid`, `name`; a Layer `Matrix` may only raise or lower it) → `ChildList` of `SceneObject`, `GroupObject`, `FocusPoint`, `Fixture`, `Support`, `Truss`, `VideoScreen`, `Projector`. ChildLists nest.
- **Fixture:** `uuid`, `name`, `multipatch`; `Matrix` `{u1,u2,u3}{v1,v2,v3}{w1,w2,w3}{o1,o2,o3}` in **mm** (identity when missing); `GDTFSpec` (file name) and `GDTFMode` (a DMXMode name, required with GDTFSpec); `Focus` (FocusPoint uuid); `Addresses` / `Address` with `break` (default 0), value an absolute DMX address or `Universe.Address` (universe from 1, address 1–512); `FixtureID` (string), `FixtureIDNumeric`, `UnitNumber`, `CustomId`; `Color` as CIE xyY; `Gobo` (file, `rotation`); `CastShadow`.
- **Geometry:** `Geometry3D fileName` (no extension means `.3ds`) with an optional `Matrix`; `Symbol symdef=<uuid>` instances of `AUXData/Symdef`. glTF 2.0 (`.glb` recommended, `extensionsRequired` must be empty) or 3DS (deprecated, 1 unit = 1 mm). All references are relative to the archive root; absolute paths and URIs are not allowed.
- **Coordinates:** right-handed, Z up, millimetres.
- **UUIDs:** RFC 4122, never nil, persistent across applications. **Data an application does not support should be preserved** on re-export (§8).
- **Known spec inconsistencies** to tolerate on read: `Classing` vs `Class`; `ipv4` vs `IPv4`; UUIDs without dashes in some examples; empty `FixtureID`.

## 5. Decision: reading (MV1)

- **Reuse the GDTF readers.** `StageGdtfZip` (stored and deflate, CRC-32, refuses ZIP64, encryption, unsafe names and zip bombs) and the strict XML reader (no DOCTYPE, caps, line-numbered errors) from GD1 read the container and the root file. MVR adds: refuse folders in entry names, and refuse two names equal ignoring case.
- **A pure parser** `StageMvrParser::Parse(Xml) → FStageMvrScene` (plain structs, no engine state, any thread): layers, the object tree with resolved world matrices, fixtures, focus points, symdefs, geometry references and a report. Like GD1, it is tested on self-authored files and probed on a real-file corpus (`StageCraft.Mvr.Probe`, Development only).
- **Units and axes:** MVR is right-handed Z-up mm; Unreal is left-handed Z-up cm. A point maps as `(x, −y, z) / 10`. A rotation matrix maps by flipping the Y row and column. The pure function `StageMvrUnits::ToUnreal(Matrix)` returns an `FTransform` and is tested on the spec's examples and on round trips.
- **Addresses:** both forms are accepted; `Universe.Address` is validated (universe ≥ 1, address 1–512) and the absolute form is split as `((a − 1) / 512) + 1, ((a − 1) % 512) + 1`. Only `break="0"` is patched in v1. Other breaks are named in the report.
- **Caps (Rules.md §55):** 64 MB archive, 512 entries, 20,000 objects, 4,096 fixtures, nesting depth 32, 256 distinct GDTF files. Over a cap the import is refused, never truncated.

## 6. Decision: importing into a stage (MV2)

- **Request:** `AModularPlayerController::RequestImportMvr(Path)` → GameMode `EvaluateMvrImport` (deny by default; under M1 = Free it allows everyone) → `UStageMvrImportSubsystem` (`UWorldSubsystem`, Game and PIE) reads on a worker and builds a review, the same two-step pattern as GD4: **nothing changes until the user confirms the review.**
- **Fixture types:** every distinct `GDTFSpec` goes through `UStageFixtureLibrarySubsystem` (ADR 0033 §7) as if the user had imported it. A type already in the library (same GDTF `FixtureTypeID` and revision) is reused. The `GDTFMode` picks the catalog item (one per mode, ADR 0033 §4.2). A fixture without `GDTFSpec`, or whose mode StageCraft cannot represent, is placed as the closest built-in generic of its kind and named in the report.
- **Placement:** fixtures are placed at their converted transform with their patch (universe, address) and `FixtureIDNumeric` as the StageCraft fixture ID when it is free. A clash takes the next free ID and is reported. Every write goes through the session's validated path. **The whole import is ONE undo step**, all or nothing.
- **Focus points** set pan and tilt for fixtures with pan and tilt. The pure aim maths is shared with the existing aim tools; out-of-range aims are clamped and reported.
- **Trusses, supports and scene objects:** `.glb` / `.gltf` geometry goes through the runtime model import (ADR 0006); `.3ds` and missing files become sized boxes when bounds are known and are skipped otherwise. Everything is named in the report (M2).
- **Layers** become Outliner groups named after the layer. MVR UUIDs are kept on each placed item as an `MvrId` tag, so a re-import updates instead of duplicating (§8) and an export writes the same UUIDs back.
- **Not imported in v1:** VideoScreen, Projector, Mappings, Connections, CustomCommands, Overwrites and Protocols. They are counted in the report.

## 7. Decision: exporting (MV4, Pro)

- **Request:** `RequestExportMvr(Path)` → GameMode `EvaluateMvrExport`, which requires `StageCraft.Entitlement.Pro.MvrExport` (§2.1).
- **Content:** one Layer per Outliner group (or one "StageCraft" layer); fixtures with their matrix, patch, IDs and, for GDTF-library types, the original `.gdtf` file (unchanged files are copied as they are, as the spec requires). Built-in generic fixtures have no GDTF and are written without `GDTFSpec` (allowed by the spec), with their name and patch. Trusses and decks are written as `Truss` / `SceneObject` with `.glb` geometry from the DCC bridge exporter (M3).
- **Round trip:** UUIDs kept from an import are written back. A file written by StageCraft and read back must give the same fixtures, patch and transforms (a test).

## 8. Re-import and preserved data

The spec asks importers to apply only intended changes and to preserve what they do not support. v1 keeps each imported item's `MvrId` and the source file's path in the scene (scene format v13). A re-import of the same file matches by `MvrId`, moves, re-patches, adds and removes items, and shows the differences in the review first. Preserving unsupported nodes for re-export (keeping the original XML of unknown nodes) is a follow-up (MV5) and is listed as a known limitation until then.

## 9. Scene format

**v13:** each placed item may carry `"mvrId"` (a UUID string). v1–v12 files load unchanged. A v13 file is refused by older builds, as with every bump.

## 10. Phases and verification

| Phase | Content | Verification |
|---|---|---|
| **MV0** | Spike: the GD1 ZIP and XML readers on real `.mvr` files (G5-style corpus from Vectorworks, grandMA3 and the GDTF-Share samples); measure parse time for a 2,000-fixture file | Probe log, no crashes on malformed or capped files |
| **MV1** | Pure parser, units, addresses, caps, report | Unit tests on self-authored files, the spec's examples and malformed input |
| **MV2** | Review and import as one undo step (fixtures through the GDTF library, focus, geometry, layers as groups) | World tests, undo and redo, a scripted `-game` run with screenshots |
| **MV3** | UI: File › Import MVR…, the review dialog (the GD4 dialog pattern), report, help article EN/HY | Scripted `-game` run, Gevor's manual check |
| **MV4** | Export (Pro gate), round-trip test | Round-trip test; the file opens in a third-party viewer (Gevor's manual check) |
| **MV5** | Re-import differences, preserved unknown nodes | Tests on edited files |

Every phase ends with Editor, Game and Shipping builds at 0 warnings and the full test suite green, as for GD0–GD4.

## 11. Alternatives considered

- **Unreal's DatasmithMVR / DMXGDTF plugins** (UE 5.8 `Engine/Plugins/VirtualProduction/DMX`). Rejected for the same reasons as in ADR 0033: they are editor-time importers that create assets, they depend on the DMX Engine plugin stack the core module deliberately avoids (ADR 0019), and they do not fit runtime, user-driven import with our trust-boundary checks.
- **Import only fixtures and patch (no geometry).** Simpler, but a designer's plot without its trusses is not useful to place in a venue. Kept as the fallback when geometry cannot be read.
- **One ADR per topic (tiers separately from MVR).** Gevor asked for one ADR; the tiers section is short and MVR export is their first concrete gate.

## 12. Consequences

- MVR import makes StageCraft a reader of real production plots; export makes it a source for consoles and CAD (Pro).
- The entitlement gates of §2.1 create the seams licensing (Step 6) plugs into; until then nothing is locked.
- Scene format v13 and the `MvrId` tag are new persistent contracts.
- Risks: real-world MVR files vary (the spec itself has inconsistencies, §4), so the MV0 corpus matters as much as G5 did for GDTF; `.3ds`-only exporters will import without set geometry.

## 13. Rollback

MVR is additive: the import and export requests, the subsystem and the menu entries can be removed without touching existing scenes, except v13 files, which keep loading in a build that ignores `mvrId`.

## 14. Amendment: MV1 as built (STATE.md #87, 2026-11-10)

- **The XML reader keeps element text** when asked (`StageXml::FLimits::bKeepText`): MVR carries its data in the text of leaf elements (`Matrix`, `GDTFSpec`, `GDTFMode`, `Address`, `Focus`, `FixtureID`). Text around child elements is never kept, references are decoded, CDATA is kept as written, and the text is capped like an attribute value. GDTF leaves it off, so its parsing is unchanged.
- **`StageMvr::Parse` / `ParseSceneDescription`** (`Private/Mvr/StageMvrParser.h`), pure, any thread. Caps: 64 MB archive, 512 entries, 32 MB scene file, 20,000 objects, 4,096 fixtures, object nesting 32, 1,024 layers, 256 GDTF files, 64 addresses per fixture. An entry in a folder refuses the file.
- **Errors vs warnings.** Errors refuse the whole file and name the line: a bad `Matrix`, address, UUID or number, a duplicate UUID, a missing `Scene`, a major version other than 1, a cap. Warnings go to the report (at most 50, then a count): a fixture without GDTF or mode, a DMX break other than 0, a focus point, multipatch or symbol that is not in the file (the reference is cleared), a GDTF or geometry file missing from the archive, 3DS geometry, unknown elements.
- **Tolerated:** UUIDs with or without dashes or braces; a `GDTFSpec` without `.gdtf` and a `Geometry3D` without extension (`.3ds`, as the spec says).
- **World matrices** are resolved in the parser: object, then each parent object, then the layer (row vectors, mm). `StageMvrUnits::ToUnreal` / `FromUnreal` mirror Y and convert mm to cm; tested on offsets, a 90 degree turn and a round trip.
- **MV0 probe:** `StageCraft.Mvr.Probe [folder or file]` (Development only, default `Saved/MVRCorpus`) logs each file's scene, report and parse time. It waits for a real corpus.
- Not yet: MV2–MV5. M1–M4 are still open.

## 15. Amendment: MV2 and MV3 as built (STATE.md #88, 2026-11-10)

Gevor asked for MV2–MV3 on 2026-11-10 while M1–M4 were still open, so the build follows the recommendations where it has to:
- **M1 (import Free):** applied. `EvaluateMvrImport` allows every player unless `SessionRules.bAllowMvrImport` or `bAllowFixtureImport` is off (the GDTF types go into the library).
- **M2 (geometry):** only partly. Trusses do **not** import their meshes: each MVR `Truss` becomes a StageCraft truss run (the Sketch window's truss item) of whole sections along the longest side of its geometry's bounds, at the bottom of the bounds. The bounds come from the numbers the files state about themselves: the glTF `POSITION` accessors' required `min`/`max` moved by the node transforms (no vertex buffers read), or the vertex lists of a 3DS file. Upright towers (more than 45° from level) and trusses without readable geometry are reported, not placed. SceneObject, Support, VideoScreen and Projector are counted in the review, not placed. Importing their `.glb` meshes through the runtime model import is the M2 follow-up.
- **M3, M4:** not needed by MV2–MV3.

Decisions made while building (each reversible):
- **glTF axes:** MVR 1.6 states no unit or axis for glTF geometry. glTF itself is metres, Y up; bounds are converted with the usual glTF-to-Z-up mapping `(x, y, z) -> (x, -z, y) × 1000` mm. To be checked against real exports (MV0 corpus).
- **Rest pose:** MVR's `Alignment` default is a beam along −Z (hanging, as GDTF). A StageCraft fixture at rest shines along its local +Z, so every imported fixture gets a half turn about X under its converted MVR transform. Scale is dropped.
- **Focus:** pan and tilt aiming at the FocusPoint are solved from the fixture's pivots (`StageMvrImport::SolvePanTilt`, the inverse of how the pan and tilt pivots turn the beam), inside the mode's ranges with the least movement, clamped and reported when out of reach. They become the fixture's home look.
- **IDs and patch:** `FixtureIDNumeric`, else a whole-number `FixtureID`, when free on the stage and in the file; otherwise the next free ID, reported. The first `break="0"` address is the patch. Multipatch fixtures (second patches of one fixture) are not placed.
- **Types:** a GDTF type the library holds (same FixtureTypeID) is reused as it is; revisions are not compared yet. A new one is added through `UStageFixtureLibrarySubsystem::AddEmbeddedFile` (copied into the library, outside the stage's undo, as any GDTF import). A fixture whose type or mode cannot be used becomes the Sketch window's generic fixture, named in the notes.
- **MvrId** is a `SaveGame` FGuid on `AModularBaseActor`, so it lives in each item's tagged state (undo snapshots and scene files) instead of a new top-level `"mvrId"` field: **no scene format bump** (§9 is superseded), older scenes load with none, older builds skip the unknown tag. A second import of the same file skips items whose UUID is on the stage; updating them is MV5.
- **Layers** become fixture groups (the show library's groups) named after the layer, recorded in the same undo step. The Outliner's groups are derived from catalog categories, so there is nothing else to map a layer to.
- **Subsystem lifetime:** `UStageMvrImportSubsystem` is a `UGameInstanceSubsystem`, not the `UWorldSubsystem` of §6: the review is not tied to a level and the workspace that shows it lives as long. Placement uses the requesting controller's spawn system and ends (rolled back) if that goes away.

Performance and memory (Rules.md §21–§22, §29):
- The read runs on a worker: the whole `.mvr` (≤ 64 MB) is read once; the scene XML's UTF-8 bytes are freed once converted, before the XML tree is built; each GDTF and each truss geometry file is extracted, used and released in turn; only truss geometry is extracted at all. The archive bytes never reach the game thread. The review keeps the parsed scene and the GDTF bytes until IMPORT or CANCEL; IMPORT moves the bytes into the library and drops the rest before placing. The log names the archive size and the largest extracted entry.
- Placement is frame-sliced (`FStageMvrPlacementJob`, 8 ms per frame on the game instance's timer), all or nothing: a refusal or a closed stage deletes everything the job placed. The log gives frames, total placing time and time per item.

UI (MV3): File › Import MVR Scene… and drag and drop; `UStageMvrImportDialog` (types new / in the Library / unusable, counts, generic fixtures, notes, progress), hosted in the workspace's dialog layer one at a time with the GDTF report; the status bar shows the outcome; help article `mvr-import` (EN/HY). Development console: `StageCraft.Mvr.Import`, `.Confirm`, `.Cancel`, `.Status`.
