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

The other decisions cover pricing (D3), licence enforcement and offline use (D4), the payment provider and legal entity (D5), online fixture sources (D6), the scope and security of the control protocols (D7), and the step order (D8: GDTF → **MVR** → MIDI/OSC → Download Manager → Web Remote → Stripe licensing). **Their exact wording is not recorded here and must be restated by Gevor before Step 6 (licensing) depends on it.** This ADR does not invent it. *Gevor restated D3–D5 on 2026-11-16; they are recorded with the licensing design in §17.*

### 2.1 Consequences of the tiers for what exists today
- **DMX IN (#47) and multi-universe DMX OUT (#65) are free today.** They become Pro. Nothing is removed until licensing exists (Step 6). Until then the build behaves as Pro, and the gates below are written but open. *Closed by Step 6 (§17).*
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

The spec asks importers to apply only intended changes and to preserve what they do not support. v1 keeps each imported item's `MvrId` and the source file's path in the scene (scene format v13). A re-import of the same file matches by `MvrId`, moves, re-patches, adds and removes items, and shows the differences in the review first. Preserving unsupported nodes for re-export (keeping the original XML of unknown nodes) is a follow-up (MV5) and is listed as a known limitation until then. Built in MV5 as §18 describes.

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
| **MV5** | Re-import differences, preserved unknown nodes | Tests on edited files (built: §18) |

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

## 16. Amendment: MV4 (export) as built (STATE.md #90, 2026-11-11)

Built after Step 3 (MIDI and OSC), before Step 4 (GDTF-Share downloads), because §7 had decided it and it needs no outside service or account.
- **Request:** `AModularPlayerController::RequestExportMvr(Path)` / `RequestBrowseMvrExport()` (File › 3D Apps › Export MVR Scene…) → GameMode `EvaluateMvrExport` → `UStageMvrExportSubsystem::StartExport`. `SessionRules.bAllowMvrExport` (default on) switches it off. **The Pro gate (§2.1, `StageCraft.Entitlement.Pro.MvrExport`) is not enforced yet:** no product grants Pro tags until Step 6, so enforcing it now would make the feature unusable. It goes into `EvaluateMvrExport` with Step 6, as for remote control (ADR 0035).
- **Subsystem lifetime:** `UStageMvrExportSubsystem` is a `UWorldSubsystem` (Game and PIE): an export writes the stage of one world, like the bill of materials. The stage is read on the game thread into plain values (`StageMvrExportGather`); a worker copies the GDTF library files, builds the archive (`StageMvrExport::Write`, pure) and writes it with `StageAtomicFile`. The result reaches the status bar through `OnExportFinished` (the reports' `FStageReportExportResult`). One export at a time.
- **ZIP writer:** `StageZip::FWriter` (next to the reader): stored or raw-deflate entries through the engine's zlib, UTF-8 names, no ZIP64 or extra fields, entries dated 1980-01-01 so the same stage gives the same bytes. `.gdtf` files are stored unchanged, as the spec asks.
- **Ids:** an item imported from MVR writes its `MvrId`; every other item writes its StageCraft instance id, which scene files keep, so exporting the same stage again writes the same UUIDs. A truss's later legs get ids derived from the item's (last 32 bits mixed with the leg number).
- **Layers:** one per fixture group that has a fixture on the stage, named after the group and using its id; a fixture goes to its first group. Everything else is on a "StageCraft" layer. This is §7's "one Layer per Outliner group": the Outliner's groups are catalog categories, so the fixture groups are what a user names (as on import, §15).
- **Fixtures:** the inverse of the import's rest pose (`StageMvrExport::FixtureMatrix`, tested against `StageMvrImport::FixtureTransform` for every pose); patch as an absolute `break="0"` address; `FixtureID` and `FixtureIDNumeric`; for a library GDTF type the library file under the name it was imported as (made safe and unique) and the mode's name. Built-in fixtures and session-only types are written without `GDTFSpec`. A library file that cannot be read is noted, and the fixture is written without a type. Pan, tilt and focus points are not written.
- **M3 deviation (geometry):** trusses and other items are written with **box** `.glb` geometry (`StageMvrGeometry::WriteBoxGlb`, glTF 2.0, the inverse of the import's axis mapping), not with meshes from the DCC bridge exporter. That exporter writes a whole saved scene through a temporary world over many frames (ADR 0022 §12); one mesh per item is a follow-up. A truss is one `Truss` per straight leg, its box the leg's length, as wide as it is high, from the bottom of the item's bounds (30 cm without bounds); the import plans the same run back. Other items are `SceneObject`s the size of their local bounds times their scale; items without bounds are left out and counted. Equal boxes share one file.
- **Round trip (§7):** tested: the pure writer read back by `StageMvrArchive::Read` and `StageMvrImport::Build` gives the same layers, UUIDs, GDTF files, modes, patch, IDs, fixture poses and truss runs; a stage exported through the subsystem reads back item by item. The third-party check (Vectorworks, grandMA3, an MVR viewer) is Gevor's (§10).
- Dev console (Development only): `StageCraft.Mvr.Export <absolute path>`. Help article `mvr-export` (EN/HY).

## 17. Amendment: Step 6, Pro subscription and licence (D3–D5 restated 2026-11-16, STATE.md #93)

### 17.1 Decisions (Gevor, 2026-11-16)
| # | Decision |
|---|---|
| D3 | **Pro is a subscription**, sold through the existing StageCraft Cloud and Stripe infrastructure (ADR 0032). Monthly and yearly plans; each Stripe Price is configured on the server (`PRO_PRICE_MONTHLY`, `PRO_PRICE_YEARLY`), so prices never ship in the client. |
| D4 | **The server signs an Ed25519 licence token; it works offline for 14 days.** When it expires, Pro features lock and **saved scenes stay fully usable**: nothing about a scene, its patch or its universes depends on the licence, only what the app *does* with them (send, receive, export, accept remote commands). |
| D5 | **Same Stripe account and the same cloud server as the marketplace.** No second provider, no second legal entity. |

### 17.2 Server (Services/StageCraftCloud)
- **Checkout:** `POST /v1/subscriptions/checkout {plan, idempotencyKey}` opens a Stripe Checkout session in `mode=subscription` for the plan's configured Price. The account id and plan go into the session's and the subscription's metadata. Idempotent per account and key; refused when the account already has an entitled subscription (`AlreadySubscribed`) or the plan has no Price (`PlanNotAvailable`).
- **Settlement only by verified webhooks**, as for orders (ADR 0032 §3.5): `customer.subscription.created/updated/deleted` write the subscription row (status, plan, period end, cancel-at-period-end) on the same signed, stored-once, one-transaction path. Stripe does not order its events, so each row keeps the `created` time of the event that last wrote it and **an older event never overwrites a newer one** (Rules.md §52). `created` is whole seconds, so on a tie the later lifecycle stage wins (incomplete < active/trialing/past_due < unpaid < canceled), and `canceled` is final. The plan comes from the Price first (a Billing Portal plan switch keeps the old metadata). `checkout.session.completed` / `.expired` in subscription mode only close the checkout record. Subscription revenue is platform revenue with no seller share, so it is not written to the marketplace ledger; Stripe is its record.
- **Entitled** means Stripe status `active`, `trialing` or `past_due` (Stripe is still retrying a failed renewal; a paying customer is not locked out for a declined card). `unpaid`, `canceled` and `incomplete` are not. Online, the next refresh then removes the licence; offline, the token runs out within 14 days.
- **Licence:** `POST /v1/licences {machineId}` issues a token for the signed-in account on that machine when it is entitled. `machineId` is a 64-hex digest the client derives (§17.3). At most `LICENCE_MAX_MACHINES` (default 3) machines per account; `POST /v1/licences/release {machineId}` frees one. Refusals: `NoActiveSubscription` (403), `MachineLimitReached` (409).
- **Token:** `SC1.<base64url(payload)>.<base64url(signature)>`, Ed25519 over the ASCII bytes `SC1.<base64url(payload)>`. Payload: `v` (1), `kid`, `sub` (account id), `mid` (machine id), `plan`, `ent` (the exact Pro tags), `iat`, `exp` (Unix seconds; `exp = iat + LICENCE_OFFLINE_DAYS × 86400`, default 14 days). The private key (`LICENCE_SIGNING_KEY`, PKCS#8) never leaves the server; production refuses to start without one, and refuses the development key.
- **Manage:** `POST /v1/subscriptions/portal` returns a Stripe Billing Portal link (cancel, change card, invoices). `GET /v1/me/subscription?waitSeconds=0..25` reports the subscription and long-polls while a checkout is open.
- **Development:** the fake provider's `/dev/fake-subscription/:session` page settles a subscription through the same signed-webhook path. A fixed **development** key pair (`kid` `dev-1`) signs tokens outside production; Shipping clients do not trust it.

### 17.3 Client
- **`StageLicenceToken` (pure):** parses and verifies a token with the engine's OpenSSL 1.1.1 (`EVP_PKEY_ED25519`, a proven implementation; not hand-written crypto), against a list of trusted `(kid, public key)` pairs. The production keys are **compiled into the binary**, not read from config: a user-editable `.ini` must never be able to add a key. Development and test builds also trust the development key. Caps every field (Rules.md §55). Evaluation is a separate pure step with an explicit clock: wrong machine, expired, and the clock set back more than a day before the newest time this install has seen are refused.
- **Machine id:** SHA-256 of a fixed salt and the platform's per-user install id (`FPlatformMisc::GetLoginId`). A token copied to another PC does not validate. This deters casual copying only (Rules.md §66).
- **`UStageLicenceSubsystem`** (game instance; depends on `UStageCloudSubsystem`): the only owner of the licence on this install. It keeps the token in `<user data>/StageCraft/Licence.json` (written with `StageAtomicFile`), verifies it at start, and answers `HasProEntitlement(Tag)` with an exact match. It refreshes it while signed in (at sign-in, at start, every 12 h; on a timer, never a tick), and re-evaluates when the token expires (one timer at `exp`), broadcasting `OnLicenceChanged`. A server answer of `NoActiveSubscription` removes the token. A transport failure keeps it: that is the offline case.
- **Not an economy purchase.** The subscription uses the same server, session, Stripe account, browser checkout and long-poll as `UStageCloudCommerceBackend`, but it does not go through `UStageEconomySubsystem` and is not stored in the profile. Pro is time-limited and server-signed. A profile entitlement is permanent and locally editable (Economy security 1, 7, 8), so recording Pro there would make it forgeable and never expire.
- **Gates (GameMode, deny by default):** `EvaluateMvrExport` (`Pro.MvrExport`), `EvaluateRemoteControl` (`Pro.MidiOsc`), `EvaluateWebRemote` (`Pro.WebRemote`), the new `EvaluateDmxInput` (`Pro.DmxIn`) and `GetDmxOutputUniverseLimit` (`Pro.DmxUniverses`; Free = 1). The session rule is judged first, then the licence. Refusals are `Locked` with a reason that names StageCraft Pro. Without a game instance or licence subsystem, everything Pro is refused.
- **DMX enforcement:** `UStageDMXNetworkSubsystem` takes capability limits (`FStageDMXCapabilityLimits`: input allowed, maximum universes sent). The controller pushes them at `BeginPlay` and on every `OnLicenceChanged`. What the user left on (input, output) reopens only with the first push, not at `OnWorldBeginPlay`, so a Free licence never receives or sends for even one frame. When the limits forbid input, input stops without changing the saved choice, and the show returns to Internal. It opens again when Pro returns. Output sends the lowest patched, unmuted universes up to the limit; the Patch panel lists the held ones. The subsystem itself stays rule-agnostic: unbound tools and tests see no limit.
- **MIDI/OSC and Web Remote:** switching on is judged, and so is every command (unchanged since ADR 0035/0036), so a lapsed licence refuses commands at once, reported once per reason.
- **Development override:** the `StageCraft.Licence.DevAssumePro` console variable exists only outside Shipping. `DefaultEngine.ini` sets it to 1, so editor and Development builds behave as before. Tests and Gevor set it to 0 to see Free. **Shipping has only the real licence.**
- **UI:** a Preferences section, STAGECRAFT PRO, shows the status (Free / Pro until date / expired / a refusal), with SUBSCRIBE MONTHLY / YEARLY, MANAGE, REFRESH and RELEASE THIS PC. Each goes through `AModularPlayerController` (`RequestProCheckout`, `RequestManageSubscription`, `RequestRefreshLicence`, `RequestReleaseLicence`).

### 17.4 Honest limits (Rules.md §66)
The client decides what it does from a token it verifies itself. A patched binary can skip the check; no client-side scheme prevents that. The signature stops forged and edited tokens, the machine binding stops copied ones, and the expiry bounds how long a cancelled subscription keeps working offline. The clock-rollback high-water mark is the larger of the stored time and the licence file's write time; both are on the user's disk. The machine id comes from a per-user registry value, so copying it with the file defeats the binding. Releasing frees a place while the released machine's token runs out, so the practical cap on live tokens is about twice `LICENCE_MAX_MACHINES` for 14 days. Development and Test packaged builds trust the development key and assume Pro: never hand them to users. Anything multi-user that needs Pro on a server must check entitlement there.

### 17.5 Rollback
Set `StageCraft.Licence.DevAssumePro=1` (non-Shipping), or remove the five Pro checks from the GameMode. The server tables are additive (migration `0002`) and can be ignored.

### 17.6 Open
- **The production signing key pair:** Gevor generates it with `node scripts/make-licence-key.ts`, keeps the private key in the secret store (`LICENCE_SIGNING_KEY`), and pastes the public key into `StageLicenceToken.cpp`'s production list. Until then, Shipping trusts no key and stays Free.
- The Stripe Prices (amounts, currencies, tax) and the Billing Portal configuration are set in the Stripe dashboard.

## 18. Amendment: MV5 (re-import and preserved data) as built (STATE.md #99, 2026-11-17)

Supersedes the "skips items already on the stage" rule of §15 and settles §8.

### 18.1 What an imported item remembers
- **`FStageMvrItemOrigin`** (`Public/Mvr/StageMvrItemOrigin.h`), a `SaveGame` struct on `AModularBaseActor` next to `MvrId`. It holds the MVR layer id, the file name, one fingerprint (CRC-32) per field of what the file said at the last import (placement: the world matrix, and a truss's geometry bounds; name; patch; GDTF file and mode; aim: the fixture's matrix with its focus point's), and the preserved XML. It is tagged data, so undo snapshots and scene files carry it. **No scene format bump:** older scenes load with an empty origin, and older builds skip the tag.
- **Why fingerprints and not the file's values:** a re-import must apply only what the *file* changed. Comparing the file with the stage would undo every edit made in StageCraft (a rename, a nudged truss) on every re-import. Comparing the file with its own last version tells which fields the designer changed. 20 bytes per item.
- A duplicate (`PrepareAsDuplicate`) drops the MVR UUID and origin: a copy is the user's own item, so a re-import neither moves it in place of the original nor removes it with it.

### 18.2 Re-import rules (`StageMvrImport::Build` / `Diff`, pure)
- **Matching** is by MVR UUID (`StageMvrStageReader` reads the stage into `FStageMvrExistingItem`s on the game thread, at read time for the review and again at confirm for the plan).
- **Per field:** only fields whose fingerprint changed are written. Fields the file did not change keep the stage's value: the user's name, a move, a home look.
- **Items imported before MV5** (no fingerprints): placement, name and patch are compared with the stage; type and aim are not known and left alone. Every such item is updated once (`EChange::Origin`) so it records its fingerprints, and from then on only the file's own changes apply.
- **What an update keeps:** an updated item keeps its instance id (earlier undo steps still find it), its fixture ID (cues, groups and palettes find fixtures by it), and its SaveGame state: home look, hidden, locked and the rest. If the file asks for another ID, the note says it was kept. A fixture is re-aimed only when its aim fingerprint changed (its own placement or its focus point moved) or its type changed.
- **Type changes:** a new GDTF mode or file gives the mode's catalog item, or the generic fixture. A UUID that changed kind (truss to fixture, or back) is replaced by a new item of the new kind, without the old state.
- **Trusses** keep their own truss type when updated. Only a new run, a changed path or a kind change uses the Sketch window's truss; a rename needs no truss in the Sketch window at all. A truss whose new placement cannot be built as a run stays as it was, with a note.
- **Locked items** (the Outliner's lock forbids moving and resizing) keep their placement and their aim; the file's other changes still apply. Their origin keeps the last import's placement and aim fingerprints, so the first re-import after unlocking moves them where the file has them.
- **Missing items:** an item is "of this plot" when its recorded layer is one of the file's layers. A file name match counts only for items recorded without a layer, because two venues' plots are easily both `Plot.mvr`. Items of a layer the new file dropped entirely are therefore kept. Missing items are removed or kept as the review says (`EStageMvrMissingItems`); the dialog offers Remove first because the file is the designer's current plot. A review that showed no missing items never removes any at confirm, even if the stage changed meanwhile. A removed fixture's ID is free for the file's new fixtures, and they take on whatever show data still refers to that ID, as with any delete and place.
- **New fixtures** join their layer's existing group, by name, instead of a second group of the same name.

### 18.3 Applying (`FStageMvrPlacementJob`)
Removals first, then trusses, then fixtures, a slice per frame as in MV2. An update takes the old item off (its snapshot is kept), then spawns the new one with `AssignInstanceId` and `RestoreSnapshotState` of the old item (same kind of catalog item only), then the file's changes on top (`PrepareImported` with an optional label, `PrepareImportedPatch`). It is all or nothing: a refusal deletes what was placed and restores what was taken off, newest first. The controller records the result with `RecordReplacement` (removed snapshots, then placed items) plus the group edit, as **one undo step** named "Update from <file>".

### 18.4 Review and UI
`FStageMvrImportReview` gains the counts new, changed, unchanged, missing and preserved, computed with the same pure `Diff` the plan uses. It is an estimate: the confirm plans again against the stage as it is then, and can skip what cannot be built. The dialog shows a CHANGES line, a REMOVE / KEEP ... NO LONGER IN THE FILE toggle when items are missing, UPDATE instead of IMPORT, and "nothing to update" when the file changes nothing. Development console: `StageCraft.Mvr.Confirm [KeepMissing]`; `.Status` prints the review counts and each item's origin.

### 18.5 Preserved data (`StageMvrPreserved`, pure)
- **Kept:** for each Fixture and Truss, every attribute except `uuid`, `name` and `multipatch`, and every child element the export does not write itself, in canonical XML `<Keep ...>...</Keep>`. Names are checked, text and values are escaped, and comments and mixed text are dropped. The cap is 16 KB per object; over it, nothing is kept and the file is reported. `<UnitNumber>0</UnitNumber>`, which the export writes by default, is not kept.
- **Not kept**, because the export does not carry what they point at: Focus, Classing, Position, Mappings, Gobo, Connections, and a non-fixture's GDTFSpec/GDTFMode. They are counted in the review's notes (Focus excepted: it aims the fixture).
- **Export:** `StageMvrExport::Write` re-reads each fragment as untrusted input (scene files are user-editable): it is parsed again with caps, names are allow-listed and every element and value is rebuilt. Only then are its attributes added to the object's start tag and its elements written after the object's own. A fragment that does not read back is left out and named in the notes. A preserved UnitNumber replaces the default 0.
- **Round trip:** tested. A file's kept data, exported and imported again, gives the same fragment.
- **Not preserved in MV5:** objects StageCraft does not place (scene objects, supports, video screens, projectors, focus points, groups), AUXData (classes, positions, symbols, mapping definitions) and their files. Keeping them needs scene-level storage (a scene format bump) and the geometry files' bytes; that is a follow-up.

### 18.6 Verification
- Tests: `StageCraft.Mvr.Preserved`, `.Diff` and `.Reimport` (a world test: update with user edits kept, removal, a new fixture taking a freed ID, one undo step, redo, a refusal half-way rolled back, duplicates, export gather). `.Plan` and `.Review` were extended.
- `Scripts/Tests/RunMvrReimportCheck.ps1`: a scripted `-game` run. The plot is re-imported under another file name, the review counts are checked, then the update, undo, redo and the exported XML, with a screenshot of the review.
- Not verified: real Vectorworks or grandMA3 re-exports of a plot (the UUID and layer stability this relies on comes from the spec), and the dialog clicked with a mouse.
