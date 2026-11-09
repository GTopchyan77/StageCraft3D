# ADR 0033: GDTF fixture import, fixture modes and the device library

- **Status:** accepted (2026-11-09).
  - Gevor chose the recommended option for G1–G4 (§2): user import only, one item per mode, sized primitives, 64 cells.
  - G5 (the local corpus of real GDTF files) is still needed for GD0.
- **Related:**
  - ADR 0020 (professional fixture suite: `ULightingFixtureData`, attributes from capabilities).
  - ADR 0019 (DMX input, patch, DMX OUT §9–§10).
  - ADR 0006 and 0013 §3.1 (runtime model import, scene loads wait for imports).
  - ADR 0021 (`.stage` packages, `NotPortable`), ADR 0032 (StageCraft Cloud).
  - tasks/todo.md Phase 34 ("GDTF / MVR import" follow-up) and Phase 37 (this work).
  - Rules.md: Architecture (one writer, request bridge, push not poll), Engineering Standard §26 (versioned serialization), §55 (trust boundary), §57 (assets in packaged builds), §65 (Shipping safety).
- **Scope:** GDTF import and the fixture-model changes it needs. MVR (scene exchange) gets its own ADR (0034), built on this one. Packages carrying GDTF fixtures are specified here (§9) but built with the marketplace download manager (step 4 of the plan, STATE.md #79), so the package format changes once.

## 1. Problem

StageCraft ships 13 fixture types: 11 `DA_Fixture_*` and 2 `DA_MovingHead_*`, all generic and all made in the editor. A user whose real rig uses any other fixture cannot model it. The patch footprint, DMX OUT and the paperwork are then wrong for the real show. Every professional tool solves this with a fixture library:
- Daslight 5 lists 20,000+ profiles with cloud updates and an online Profile Builder.
- grandMA3 supports GDTF natively and downloads fixtures over the internet.
- Avolites ships a downloadable personality library.

GDTF (General Device Type Format, DIN SPEC 15800, current `DataVersion="1.2"`) is the open, manufacturer-supported standard for this. Its spec is public (github.com/mvrdevelopment/spec).

Four things in today's model stop us from simply reading GDTF into `ULightingFixtureData`:

1. **Fixture types are editor assets.** The catalog (`UStageItemSubsystem`) is built from Asset Manager-scanned `/Game/` assets. Only `UStageMeshImportSubsystem` adds items at runtime (`AddRuntimeItem`, friend-only), and those are props.
2. **One DMX mode per type.** `ULightingFixtureData` has one `DMXModeName` and one `DMXChannels` list. Real fixtures have 2–10 modes, and the user must patch the one the fixture is set to.
3. **DMX encoding is linear.** `NormalizeForDMX` maps an attribute's range linearly across the channel, or splits it into equal shares for wheel slots. Real channels have **channel functions**: DMX ranges with their own meaning. For example, a shutter channel has closed, open, strobe slow to fast and random strobe ranges; a gobo channel has slot ranges, then shake, then wheel spin. Linear encoding would send wrong values to a real fixture.
4. **Only attribute channels exist.** A channel with no StageCraft attribute (control or reset, lamp on, fan mode, focus, macros) has no representation. Today it would be output as 0, and 0 can mean "lamp off", "closed" or "reset".

## 2. Decisions only Gevor can make

G1–G4 were decided on 2026-11-09, each as recommended below. G5 is still open.

| # | Question | Recommendation |
|---|---|---|
| G1 | Where GDTF files come from. (a) The user imports a `.gdtf` they downloaded (GDTF Share needs a free account). (b) A GDTF Share API integration, which needs an agreement with the GDTF Share operators and their terms. (c) A StageCraft cloud fixture library that redistributes files. | **(a) now.** Check the GDTF Share terms before (b) or (c). We never bundle or redistribute manufacturer files until their licence allows it. |
| G2 | One catalog item per mode (§4.2), or one item with a mode parameter. | **One item per mode,** grouped by fixture type in the Library and switchable in place. Details in §4.2. |
| G3 | 3D look in v1: primitives sized from the GDTF `Model` dimensions (Base, Yoke, Head), or the GDTF glTF meshes. | **Primitives in v1** (§6). glTF meshes are an optional later phase (GD5); `.3ds` is not supported by our importer. |
| G4 | The cell limit (`CellCount` ClampMax 32, with native cell tags `StageCraft.Attribute.Cell.01..32.*` in `StageFixtureCells.h`). LED bars and matrices often have 60–200 pixels. | **Raise it to 64 in GD2,** with 32 more native cell tag sets and a Bench.Fixtures A/B. Larger pixel counts wait for pixel mapping. |
| G5 | Test files. Real manufacturer GDTFs are needed to prove the importer, but their licence may not allow committing them. | Gevor downloads about 20 varied files (spot, wash, beam, hybrid, LED par, bar, blinder, strobe, profile; several manufacturers) into a **local, uncommitted** corpus folder. Committed tests use small GDTF files we author ourselves. |

## 3. Decision overview

- A `.gdtf` file is read **once**, on a worker thread, into a pure description: `FStageGdtfFixtureType`, with modes, channels, functions, wheels, emitters, geometry and properties.
- That description is turned into **one transient `ULightingFixtureData` per DMX mode**. They are registered as runtime catalog items under a stable path, so scenes keep working across runs.
- The original `.gdtf` is kept in the user's **device library** (`Saved/StageCraft/Fixtures/`) and read again at every start, in the same way as imported models.
- The DMX channel model gains **channel functions** and **unmapped channels with a default value**. Built-in fixtures have neither, so they keep today's encoding byte for byte.
- Nothing downstream needs fixture-specific code. Cues, palettes, effects, the programmer, DMX IN and OUT, the patch, the assistant and the reports already follow `ULightingFixtureData`.

## 4. The fixture model

### 4.1 Runtime fixture types

- `ULightingFixtureData` stays the one fixture class. GDTF types are `NewObject<ULightingFixtureData>` instances:
  - in a transient package under `/Temp/StageCraftFixtures/` (never saved);
  - filled by a builder, `FStageGdtfFixtureBuilder`, that is a friend of the class. The class is `Const` for Blueprints, and its properties stay `EditDefaultsOnly`.
- They are added through `UStageItemSubsystem::AddRuntimeItem`. The friend list gains `UStageFixtureLibrarySubsystem` (§7); there is no public mutator.
- **Item path (stable):** `/Temp/StageCraftFixtures/<FixtureTypeID digits>/<ModeSlug>.<ModeSlug>`.
  - `ModeSlug` is the mode name reduced to `[A-Za-z0-9_]`, with a numeric suffix when two modes reduce to the same slug.
  - The same file therefore gives the same paths in every run, and saved scenes resolve.
- `StageMeshImport::IsImportedItemPath` remains for models. A new `StageFixtureLibrary::IsFixtureItemPath` is accepted by the scene loader in the same place (`StageSceneStore.cpp`, item path check).
- `UBaseItemData` fields come from the file:
  - `DisplayName`: `"<Manufacturer> <Name>"`;
  - `Description`: the GDTF `Description`, cut to the catalog limit;
  - the existing `Specs` (`FStageEquipmentSpecs`), so the bill of materials, the power and weight totals and the rules work unchanged:
    - `Manufacturer`: `Manufacturer`;
    - `WeightKg`: `Properties/Weight`;
    - `PowerDrawWatts`: the sum of `PowerConsumption`, falling back to `Beam.PowerConsumption`;
    - `DimensionsCm`: the bounds of the models;
  - `CategoryTag`: by kind, §5.6.

### 4.2 Modes: one catalog item per mode

- Each `DMXMode` becomes its own `ULightingFixtureData`, with `DMXModeName` set and its own `DMXChannels`. Capabilities (wheels, zoom, cells and so on) are the **fixture's**, the same for every mode. A mode that lacks a channel for an attribute simply cannot drive it from DMX, which is today's rule ("capabilities, not the DMX layout").
- **Why per mode.**
  - Footprint, DMX OUT, DMX IN, the patch conflict checks and the schedule CSV all read `GetDMXFootprint()` and `DMXChannels` from the item's data.
  - A per-instance mode parameter would mean every one of those reads the mode from the actor instead. That touches about a dozen call sites and breaks the "data describes the fixture" rule.
  - Per-mode items change none of them.
- **Switching modes** uses the existing item swap: the inspector's Type dropdown and `GetSwappableItems`. It is undoable and goes through `GameMode::EvaluateParameterChange`. `GetSwappableItems` gains one rule: for a fixture with a `FixtureTypeId`, only the modes of the same type are offered.
- The patch keeps the start address. If the new footprint no longer fits, the existing conflict reporting shows it, as with any overlap.
- New fields on `ULightingFixtureData`:
  - `FGuid FixtureTypeId`: invalid for built-ins;
  - `FName SourceFormat`: `None` or `GDTF`.
  - The manufacturer stays in `Specs.Manufacturer`.
  - These are shown in the inspector's Info rows and used by the Library to group modes under one card.

### 4.3 Channel functions (DMX encoding)

`FStageFixtureDMXChannel` gains:

```cpp
/** One DMX range of a channel and what it does (a GDTF ChannelFunction or ChannelSet). */
USTRUCT() struct FStageDMXFunction
{
    int32 DMXFrom = 0;          // In the channel's resolution (0..255 or 0..65535). The range ends at the next entry's DMXFrom - 1.
    FGameplayTag Attribute;     // The StageCraft attribute this range drives; invalid = a range StageCraft never selects.
    float PhysicalFrom = 0.f;   // Attribute value at DMXFrom, in StageCraft units (degrees, 0..1, slot index, Hz).
    float PhysicalTo = 0.f;     // Attribute value at the end of the range.
    int32 Slot = INDEX_NONE;    // A wheel or prism slot (from WheelSlotIndex); continuous when INDEX_NONE.
};

TArray<FStageDMXFunction> Functions;  // Sorted by DMXFrom. Empty = today's linear / equal-slot encoding.
int32 DefaultDMX = 0;                 // Sent when no attribute drives this channel (the channel's GDTF default).
```

- **Output** (`NormalizeForDMX`, called by DMX OUT):
  - With `Functions`, it picks the **first range of the channel's attribute** that contains the value, then interpolates inside it, or uses the range centre for a slot.
  - A value no range covers clamps to the nearest range of that attribute.
- **Input** (`DenormalizeFromDMX`, DMX IN): the received value's range gives the attribute value. A range whose attribute is invalid or belongs to another attribute leaves the current value unchanged.
- **Unmapped channels** (`Attribute` invalid, for example Control, Focus, Fan or Macro) occupy their slots in the footprint and are always sent as `DefaultDMX`.
  - `ALightingFixtureActor::WriteDMX` writes them when it writes the fixture.
  - Today it skips any channel whose attribute the fixture does not support, which leaves those slots at 0.
  - The CMY and pan/tilt resolution (`ResolveDMXAttribute`, `GetChannelSource`) stays as it is, and runs before the function lookup.
- **Several attributes on one channel** happen in GDTF; the usual case is `Shutter1`, which carries open, closed and strobe. One StageCraft attribute owns the channel, and the others become ranges with an invalid attribute. The importer reports this (§8).
- **Built-in fixtures have empty `Functions` and `DefaultDMX` 0.** Their output stays byte for byte as today; this is proved by a test that compares all 13 built-ins before and after.
- The ranges are computed once at import. Encoding a value is a short linear scan over at most 32 ranges per channel, with no allocation, so the DMX OUT per-frame path is unchanged in cost class. A Bench.Fixtures `DMXOut=1` A/B checks it.

### 4.4 Cells (multi-instance geometry)

- GDTF repeats a geometry with `GeometryReference` elements. Channels on the referenced geometry, marked `DMXBreak="Overwrite"`, repeat once per reference at that reference's offset. The exact rules are verified against the spec text and real files in GD0.
- The importer turns N references of a beam geometry into `CellCount = N` and `CellControl` from the repeated attributes: dimmer, colour, or both. Each cell's channels go into `DMXChannels` with the existing per-cell attribute tags.
- More cells than the limit (G4) is refused, with a reason.

## 5. GDTF to StageCraft mapping

### 5.1 Attributes

| GDTF attribute(s) | StageCraft | Notes |
|---|---|---|
| `Dimmer` | `Attribute.Dimmer` | |
| `Pan`, `Tilt` | `Pan`, `Tilt` | `PhysicalFrom/To` give `PanLimits` / `TiltLimits` (degrees). |
| `Shutter1` (ranges), `Shutter1Strobe`, `StrobeFrequency` | `Shutter` | Open becomes 0 and strobe becomes the Hz range. The strobe ranges' `PhysicalFrom/To` give `StrobeRateHz`. Closed and random ranges are kept as unselected ranges. |
| `ColorAdd_R/G/B/W` | `ColorR/G/B/W` | Emitter colours come from the linked `Emitter` (CIE xyY, converted to linear Rec.709). |
| `ColorAdd_RY` (amber) | `ColorA` | GDTF has no `ColorAdd_A`. |
| `ColorSub_C/M/Y` | `ColorC/M/Y` | CMY system. |
| `CTO` | `CTO` | `CTC` maps to `ColorTemp` when the fixture mixes colour. `CTB` is unmapped. |
| `Color1` (wheel) | `ColorWheel` | Slots from the linked `Wheel`: slot colour (CIE xyY) becomes the filter. `Color1WheelSpin` is an unselected range. |
| `Gobo1`, `Gobo1Pos`, `Gobo1PosRotate`, `Gobo1SelectSpin` | `Gobo`, `GoboRotation`, `GoboSpin` | Slot images (`./wheels/*.png`) become gobo textures (§6.3). `bGoboSpin` is set when a spin range exists. |
| `Gobo2` | `Gobo2` | Static wheel. |
| `Prism1`, `Prism1PosRotate` | `Prism`, `PrismRotation` | Facets from the wheel slot's `PrismFacet` count, defaulting to 3. |
| `Zoom` | `Zoom` | `PhysicalFrom/To` override `BeamAngleMin/Max`. |
| `Iris` | `Iris` | |
| `Frost1` | `Frost` | |
| `Blade(n)A` (n = 1..4), `ShaperRot` | `BladeA..D`, `BladeRotation` | **Approximation:** our blades have one insertion each. `Blade(n)B` and `Blade(n)Rot` are unmapped. `BeamShaper = FramingShutters`. |
| Anything else (`Focus(n)`, `Control*`, `Fan*`, `Macro*`, `Function*`, `Effects*`, `Video*`, …) | unmapped | Kept as a channel with `DefaultDMX` (§4.3) and listed in the import report. |

The mapping table lives in one pure function, `StageGdtfMapping::MapAttribute(Name) → {Tag, Role}`, and is covered by a table test.

### 5.2 Optics and photometry (from the first `Beam` geometry)
- `LuminousFlux` maps to `LuminousFlux`, and `BeamAngle` to `BeamAngleMin/Max` when there is no zoom.
- `FieldAngle / BeamAngle` maps to `FieldAngleRatio`, clamped to 1..3.
- `ColorTemperature` maps to `ColorTemperatureK`, clamped to 1700–12000.
- `BeamRadius × 2` maps to `Body.LensDiameterCm`, converted from metres to centimetres.
- `BeamType` sets defaults: `Spot` or `Rectangle` mean a hard edge and `FieldAngleRatio` 1.1; `Wash`, `Fresnel` and `PC` mean a soft one, 1.6.

### 5.3 Colour system
Chosen from the mapped colour attributes:
- R, G and B with W and A give RGBWA; with W only, RGBW; with A only, RGBA; alone, RGB.
- C, M and Y give CMY.
- Otherwise None, with `bAcceptsGel` for conventionals (`BeamType` Fresnel, PC or Spot without pan and tilt).

### 5.4 Motion
- `bHasPanTilt` is set when the mode tree has `Axis` geometries driven by Pan and Tilt.
- Pivot offsets come from the axis geometries' positions (GDTF `Position` matrices, in metres, converted to centimetres and Unreal axes in one tested function).

### 5.5 Units and axes
- GDTF uses metres and a right-handed frame; StageCraft uses centimetres and Unreal's left-handed Z-up frame.
- All conversion is in `StageGdtfUnits`, a pure, tested namespace. No other code converts.

### 5.6 Kind and category
- `FixtureKind` is inferred:
  - pan and tilt with gobos and zoom give Spot (or Hybrid when the beam angle is under 5°);
  - pan and tilt without gobos give Wash;
  - Beam when the beam angle is under 5°;
  - cells over 1 give PixelBar or Blinder;
  - Strobe when only a dimmer and a fast shutter exist;
  - Profile when there are blades;
  - Fresnel, or LEDPar otherwise.
- The user can correct the kind in the import report. It is stored in the library index, never in the `.gdtf`.

## 6. Geometry and look

### 6.1 Body from GDTF models (v1, G3)
- The `Model` elements linked from the geometry tree give sizes in metres. `PrimitiveType` `Base`, `Yoke` and `Head`, or the geometry roles, map to `Body.BaseSizeCm`, `YokeSizeCm` and `HeadSizeCm`.
- The existing engine basic-shape body (ADR 0020 §1) draws them. Only fixtures with no models get kind defaults.

### 6.2 Meshes (GD5, optional)
The `models/gltf/*.glb` files are imported through the existing runtime Interchange path, one mesh per part, and replace the primitives. `.3ds` is not supported, and those fixtures keep primitives.

### 6.3 Gobo and wheel images
- `./wheels/*.png` are decoded on a worker (ImageWrapper, module loaded first on the game thread) into transient `UTexture2D`, at most 256 × 256 and greyscale.
- They are kept by the fixture library for the run, and referenced by `FStageGoboSlot::Texture` through the in-memory object.

## 7. The device library: `UStageFixtureLibrarySubsystem`

A new `UGameInstanceSubsystem`; its lifetime is the whole run.

- **Storage.**
  - Each imported file is copied to `Saved/StageCraft/Fixtures/<FixtureTypeID>.gdtf`.
  - `Fixtures.json` (format version 1) records the user's kind correction, the import time and the source file name.
  - The data root follows `StageCraftPaths`, so tests redirect it and never touch user data (Rules: tests must not change user data).
- **Start-up.** Every library file is parsed again on workers, then built and registered on the game thread.
  - `HasLoadsInProgress()` joins `UStageMeshImportSubsystem::HasImportsInProgress()` in the scene-load wait (ADR 0013 §3.1). A scene that names a GDTF fixture loads after the library is ready.
- **Import.** `AModularPlayerController::RequestImportFixtureFiles(Files)` asks `GameMode` (a new `EvaluateFixtureImport`; deny by default without a GameMode), then calls the subsystem.
  - The subsystem validates (§8), parses on a worker, shows the report, and registers on the game thread.
  - Importing the same `FixtureTypeID` again is **Replace**: the user confirms, the file is swapped, and placed fixtures keep their item paths because the slugs come from the same mode names. A mode that no longer exists makes those fixtures missing (§7.1).
- **Remove.** `RequestRemoveFixtureType(FixtureTypeId)` is refused with `InUse` while any placed fixture uses one of its modes.
- **Events.** `OnLibraryChanged` is pushed to views. The catalog also broadcasts `OnCatalogChanged`.
- **Thread rules.** Parsing and image decoding run on `UE::Tasks`, and UObject creation and registration on the game thread (Rules §29). A serial number drops stale results.

### 7.1 Scenes that name a fixture type this PC does not have
- **Scene format v12:** an item entry whose path is a fixture-library path also writes `"fixtureType": {"id", "manufacturer", "name", "mode"}`.
- On load, a missing type is reported in plain words: "Robe MegaPointe (Mode 1) is missing. Import its GDTF file to see these 12 fixtures." It is reported like other missing items, and the fixture's patch and programming are kept in the scene file.
- v1–v11 scenes load unchanged. Builds before this one refuse v12, as with every bump.

## 8. Trust boundary and validation (Rules §55, §66)

A `.gdtf` is untrusted input.

- **Container.** ZIP with a central directory. The reader is self-written and small, using the engine's zlib (`FCompression`) for deflate. Whether `FCompression` decodes raw deflate is verified in GD0; the fallback is the zlib that ships with the engine.
  - Size caps:
    - file ≤ 50 MB;
    - entries ≤ 2,000;
    - `description.xml` ≤ 8 MB uncompressed;
    - each image ≤ 4 MB;
    - total uncompressed ≤ 200 MB;
    - compression ratio ≤ 100:1 (a "zip bomb" check).
  - Entry names are checked: no absolute paths, no `..`, no drive letters. Nothing from the archive is ever written to disk except the whole original file, copied into the library.
- **XML.** Read with `StageXml`, a small strict reader of our own (GD0 found both engine parsers unsuitable, §14).
  - It reads elements and attributes, both quote styles, the five predefined entities and numeric character references.
  - It refuses any DOCTYPE, so no entity is ever defined or expanded.
  - Caps: depth ≤ 32, elements ≤ 100,000, attributes per element ≤ 64, values ≤ 4,096 characters.
  - Every error carries a line number.
  - Every number is parsed with range checks. A bad value is an error with its XML path, and nothing is clamped silently.
  - Model caps: modes ≤ 64, channels per mode ≤ 512, functions per channel ≤ 64, wheels ≤ 16, slots per wheel ≤ 64, geometry depth ≤ 32, geometry nodes ≤ 4,096, strings ≤ 256 characters.
- **Model checks.** A file-level error (structure, a bad number, a missing link, a missing or invalid `FixtureTypeID`) refuses the whole file. The stable item paths need the GUID.
  - **Mode-level problems leave that mode out,** with the reason in the report, and the other modes still import. The file is refused only when no mode is left. Mode-level problems are:
    - a footprint over 512;
    - a DMX break other than 1 (multi-break modes are not supported in v1);
    - overlapping channels;
    - `DMXFrom` values that do not rise;
    - more cells than StageCraft supports.
- **Import report** (`FStageGdtfImportReport`), shown before the fixture is added:
  - manufacturer, name, modes and footprints;
  - per mode, how many channels are mapped, approximated or kept at their default, with names;
  - warnings such as the blade approximation or a `CTB` channel;
  - errors that refuse the file.
  - The same text goes to the log under `LogStageCraft`. Nothing from the file is logged beyond names and counts.
- **Licensing.** GDTF is an open specification; this code is clean-room from the public spec, with no third-party GDTF libraries. Manufacturer files are the user's own downloads (G1).

## 9. Packages and portability (built in step 4)

- **Until step 4,** scenes with GDTF fixtures are `NotPortable` for publishing, like scenes with imported models. The reason text names the fixtures.
- **Step 4 design.**
  - `.stage` package manifest v3 adds a `FIXT` section: the `.gdtf` files the scene uses, each ≤ 16 MB and ≤ 32 MB in total, hash-checked like every section.
  - The client imports missing types from the package into the library on acquire, after the same §8 validation.
  - The server's package reader (`Services/StageCraftCloud/src/package/stagePackage.ts`) gains the section and the same caps.
  - Publishing requires the publisher to confirm they may share the included files. This is a decision for G1 and for terms.

## 10. UI

- **Library panel.**
  - IMPORT FIXTURE (GDTF)... opens the file picker (`StageFileDialog`, `.gdtf`). Dropping `.gdtf` files on the window is routed as for models.
  - One card per fixture type, with manufacturer, name and a "N modes" chip. Placing it uses the first mode, or the last mode used for that type.
- **Import report dialog.** It shows the report, an editable kind, ADD or CANCEL, and REPLACE for a known type.
- **Inspector.**
  - The Type dropdown lists the modes, for example "Mode 1 (16 ch)".
  - Info rows show the manufacturer, mode, footprint and source (GDTF).
- **Patch panel.** A Mode column; changing the mode is the same swap request, as one undo step.
- **Assistant.**
  - Catalog names include the manufacturer, so "add four MegaPointes" resolves through the existing name matching.
  - A help article "Importing fixtures (GDTF)" is added in English and Armenian.

## 11. Alternatives considered

- **One item with a mode parameter on the actor.** It is closer to how consoles store the mode. It was rejected for now because every footprint and channel read would have to move from data to actor (§4.2). It can be revisited if per-instance modes are ever needed beyond swapping.
- **Converting GDTF into `/Game/` assets offline (editor tool).** Users would need the editor, and packaged builds could not add fixtures. Rejected.
- **A third-party GDTF parser library.** Licensing and Shipping-dependency risk, for a format we only read. Rejected in favour of a small clean-room reader with tests (§8).
- **A full GDTF channel model (relations, mode masters, macros).** Not needed to place, program and output a fixture correctly. Relations and mode masters are read only to validate; channels a mode master gates take their default. Revisit with real-file evidence from GD0.

## 12. Consequences

- **Benefits.**
  - Any fixture with a GDTF file can be placed, patched with the right footprint, programmed and sent to a real rig with correct DMX values.
  - This is the base for MVR (ADR 0034), portable packages (§9) and universe counting for licences.
- **Costs and risks.**
  - **Fidelity** varies across manufacturers' files, which is why the GD0 corpus and the import report exist.
  - **Approximations** are visible to the user: blades, frost behaviour, pixel limits.
  - **Start-up time** grows by parsing the library. Measure it in GD2; the target is under 20 ms per file on a worker, with nothing on the game thread beyond registration.
- **Formats.**
  - Scene format v12.
  - `Fixtures.json` v1.
  - Package manifest v3 (step 4).
- **Rollback.** Disable the library import request in the GameMode. Scenes v12 still load in this build. Built-in fixtures are untouched.

## 13. Phases and verification

| Phase | Content | Proof |
|---|---|---|
| **GD0 spike** | The ZIP reader and deflate path; the XML reader choice; the GeometryReference and Break rules; parse time; a list of the attributes and quirks in the corpus | A report in this ADR (§14). The probe command `StageCraft.Fixtures.GdtfProbe` is Development only. |
| **GD1 parser and mapping** (pure) | `StageGdtf::Parse` (bytes to `FStageGdtfFixtureType` plus errors), `StageGdtfMapping`, `StageGdtfUnits`, the report | Tests on self-authored files: the minimum valid file, every mapped attribute, 16-bit offsets, functions and sets, wheels, cells, and every §8 refusal. A corpus run gives 100 % parsed or refused with a reason, and no crash. |
| **GD2 runtime types and library** | Builder, `UStageFixtureLibrarySubsystem`, stable paths, catalog registration, scene v12, missing-type reporting, `CellCount` 64 | Tests: build from a description; stable paths across two runs; v12 round trip; v11 loads; missing type reported; user data unchanged. Bench A/B with 64-cell fixtures. |
| **GD3 DMX functions** | `Functions` and `DefaultDMX`, encode and decode, unmapped channels in DMX OUT and IN | Tests: the 13 built-ins byte-identical; shutter, gobo and wheel ranges both ways; defaults sent. Bench `DMXOut=1` A/B. Gevor checks a real fixture or a visualiser (Capture, or the grandMA3 onPC 3D view) receiving DMX OUT. |
| **GD4 UI** | Import button and drop, the report dialog, the mode swap in the inspector and patch, Library cards, the help article | Scripted `-game` run with window screenshots; UI tests through the controller; Gevor's mouse checks. |
| **GD5 (optional)** | glTF meshes; real gobo images in projection | A/B screenshots and a benchmark. |

Each phase: Editor, Game and Shipping builds with 0 warnings; the full suite run without a renderer; STATE.md entry; Gevor commits.

## 14. Spike results

GD0 and GD1 were done together on 2026-11-09, on self-authored files (STATE.md #80). The real-file corpus (G5) is still to come.

- **ZIP and deflate.**
  - `FCompression::UncompressMemory(NAME_Zlib, …, CompressionData = -15)` decodes ZIP's raw deflate: the engine passes the bit window straight to zlib's `inflateInit2` (Core, `Misc/Compression.cpp`).
  - The ZIP reader is self-written (`Private/Fixtures/StageZipReader`). It handles stored and deflate entries, refuses ZIP64, encryption and multi-disk archives, and checks names, sizes, the ratio and CRC-32.
  - CRC-32 is our own table, checked against the standard check value `0xCBF43926`.
- **XML: neither engine parser is suitable for untrusted files.**
  - `FXmlFile` is line-based. Its comments say it assumes one element per line, and it reads double quotes only, which is fragile across GDTF writers.
  - `FFastXml` streams correctly, but it `check()`s on a malformed declaration (`<?xml … ?` without `>`) and on deep nesting. A hostile file could crash a Development build that way.
  - Decision: `StageXml`, a strict reader of our own (§8). It returns an error with a line number for every malformed input that the tests cover.
- **GeometryReference and Break.** Implemented as: a `DMXBreak="Overwrite"` channel on a geometry inside a referenced template is repeated once per reference, at `offset + Break.DMXOffset − 1`. Each reference is one cell, in document order. **To confirm against real files** with `StageCraft.Fixtures.GdtfProbe`.
- **Geometry Position.** The translation is read from the fourth column (metres), and Y is mirrored for Unreal. **To confirm** with real files: a wrong reading would show as pivots in the wrong place.
- **Parse time and corpus quirks.** **Pending the corpus.**
  - Put the files in `Saved/GDTFCorpus` and run `StageCraft.Fixtures.GdtfProbe` (Development builds).
  - It logs every file's report and time, the slowest file, and the GDTF attributes most often kept at their default or approximated.
