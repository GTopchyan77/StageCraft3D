# ADR 0014: Shopping and assembly list (measured cables, distros, rigging hardware)

- **Status:** accepted. See STATE.md #41.
- **Related:**
  - ADR 0012 §2.3 (the cable schedule and power plan this extends; its "Power cables" and "DMX cables" length rules are replaced by §2.3 below).
  - ADR 0011 §3.2 (`StageReportFiles`, PDF + CSV written together), ADR 0005 §5 (bill of materials).
  - Rules.md Engineering Standard §21 (no polling), §27 and §55 (configuration validated at the trust boundary), §29 (worker threads read plain values only), §54 (determinism).

## 1. Problem

Gevor's milestone (2026-10-21), "Step 1: Detailed Shopping & Cable Assembly Export":

1. **Real 3D distances** between fixtures, power distros and control nodes.
2. **Stock cable lengths** (e.g. 2, 5, 10, 15, 20 m) with a **configurable +10 % safety margin**, grouped as "XLR 5-pin 10 m — 12x", "PowerCON TRUE1 15 m — 8x".
3. **Mounting and safety hardware** (clamps, safety bonds, power distro units) scaled to the fixture count and the calculated wattage and weight.
4. A **"Shopping & Assembly List"** section in the PDF and CSV exports under File › Production.

What exists (ADR 0012):
- Power home-runs have no length ("site"), because the distro is not part of the stage.
- DMX links are measured as the straight line × 1.2, rounded up to a fixed list (1–50 m). Over 50 m is flagged "split". The first link of each universe (from the console or node) is "site".
- There are no distros, clamps or bonds. Nothing counts cables by length.

## 2. Decisions

### 2.1 Where the list lives

The shopping list is part of the cable schedule report, not a fourth export.
- It is computed from the schedule's runs and circuits, so the two always agree.
- It is printed as the first section after the warnings, because it is what a crew packs from. The wiring tables that follow say which cable goes where.
- The menu entry becomes "Export Cable Schedule, Power & Shopping List (PDF + CSV)". The file stem stays `CableSchedule_<Scene>_<time>`.

Code:
- `Public/Report/StageShoppingList.h` holds the value types (`FStageCablingRules`, `FStageRiggedFixture`, `FStageCableAssembly`, `FStageShoppingLine`, `FStageShoppingList`) and the pure functions: `Sanitize`, `GetRouteMeters`, `AssembleRun`, `FormatAssembly`, `IsRigged`, `GetClampCount`, `PickSafetyBond` and `GetSectionName`.
- `StageShoppingList::Build(const FStageCableSchedule&, Fixtures)` is declared in `StageCableSchedule.h`, after the schedule type. Declaring it in `StageShoppingList.h` would need an elaborated `struct FStageCableSchedule` inside the namespace, which declares a new `StageShoppingList::FStageCableSchedule` (the Code Standards pitfall; the first build failed exactly so).
- `StageCableSchedule::Build` gains the rigged fixtures and the cabling rules and fills `Schedule.Shopping`.

No UObjects, no world access, any thread. The subsystem gathers plain values on the game thread, as before.

### 2.2 Positions: distros and the DMX control node

The stage has no distro or DMX node items, so their positions come from the rules (`EStageCableAnchor`):
- **Automatic** (default):
  - The distros stand at the centre of the powered equipment, on the floor (Z = 0).
  - The node stands at the centre of the patched fixtures, on the floor. With no patched fixture it stands with the distros.
  - The centre (mean) is the simplest deterministic choice, and it roughly minimises the total cable.
- **Fixed:** a position in the level, in metres, from Project Settings, for a venue with a known dimmer city or stage-left rack.

All distros stand together. That is how a rack or dimmer city is built, and it keeps the existing phase balancing unchanged. Per-distro placement near its loads would need spatial clustering before balancing, which would change every plan. That is left for when distros become placeable items (§4).

### 2.3 Measuring a run

- **Route** = horizontal distance + height difference (`GetRouteMeters`).
  - Cables run along the floor or a truss and then up or down a drop. They do not go through the air, so the straight line under-measures every rigged fixture.
  - Non-finite coordinates read 0. Coordinates are clamped to ±10 km, which bounds the cable count of one run.
- **Required** = route × (1 + safety margin). The default margin is 10 %, configurable from 0 to 100 %.
- **Stock** (`AssembleRun`; default 2/5/10/15/20 m, configurable):
  - A run up to the longest stock length takes the shortest stock length that covers it, with a 1 µm tolerance so an exact fit is not pushed up.
  - A longer run takes as many longest-stock cables as fit, plus the shortest stock that covers the rest. If the rest needs the longest stock again, it counts as one more full length.
  - It is stored as `FStageCableAssembly {Count, StockMeters, TailMeters}` (never a list of pieces) and printed as "15 m" or "2 x 20 m + 5 m".
  - A route of 0 takes the shortest cable.
- **Every run is measured now:**
  - Power: distro → load.
  - DMX: node → first fixture of each universe, then fixture → fixture.
- **"Too long, add a splitter" is dropped.** DMX and TRUE1 cables are joined in line, so a long run is several cables, flagged "Joined cables" and bold in the PDF. A DMX line can run several hundred metres; ADR 0012's 50 m limit described its stock list, not a DMX limit.

### 2.4 Distros

- After the circuits are planned (unchanged), circuits become outputs of distros in circuit order.
- A distro is full when its outputs are used (default 12) or when one more circuit would take a phase of its feed past `DistroFeedAmps` (default 63 A per phase). Then the next distro opens.
- A circuit that alone exceeds the feed still gets a distro. It is already reported as overloaded or needing a larger feed.
- The count therefore scales with both the number of circuits (fixtures) and the wattage.
- Circuits and power cables name their distro: "PD2 C07 / L1". A "Distros" table lists the circuits and the highest phase current per distro.
- The mains feed cable to each distro is not counted ("measured on site"); the distance to the venue's supply is unknown.

### 2.5 Rigging hardware

- **Rigged** (`IsRigged`) means either:
  - the fixture's origin is at least `RiggedAboveMeters` high (world Z, default 2 m: above any stage deck), or
  - it is tilted more than 30° from upright (hanging or side-mounted), from `GetActorUpVector().Z`.
  - Standing fixtures get nothing.
- **Clamps:** each rigged fixture gets max(1, ⌈weight / `WeightPerClampKg`⌉), default 20 kg per clamp. Heavy moving heads get two or more.
- **Safety bonds:** one per rigged fixture, the lightest stocked rating at least its weight (default 10/25/50/100 kg).
  - A fixture heavier than every bond is listed as "Secondary suspension, rigger to specify" (bold) and warned about.
- **Weights** are the catalog `Specs.WeightKg` × quantity.
  - Missing weight data gets one clamp and the lightest bond, with a warning.
  - The rigged fixture weight (fixtures only) is totalled for the truss load check a rigger does anyway.
- **DMX hardware:** one node or splitter output and one terminator per universe.

### 2.6 Configuration: `UStageCablingSettings`

- It is shown in Project Settings › Game › StageCraft Cabling & Rigging and saved to DefaultGame.ini.
- It holds the margin, stock lengths, cable names, the two anchors, distro outputs and feed, the rigging height, kg per clamp, and the bond ratings.
- `ToRules()` sanitizes every value (`StageShoppingList::Sanitize`):
  - values are clamped to their ranges, and non-finite values take the default;
  - list entries outside their range are dropped, and the rest sorted and de-duplicated;
  - an empty list or a blank name takes the default.
- A hand-edited ini cannot divide by zero, loop without end, or print an empty name.
- It is kept separate from `UStagePowerDistributionSettings`, which describes the electrical supply, not the cable stock or the rigging.

### 2.7 Alternatives considered

- **A separate "Export Shopping List" file.**
  - Rejected: it would duplicate the run computation, and the two could drift apart.
  - The CSV's shopping block is first and self-contained, so it can still be pasted into a rental quote alone.
- **Placeable distro and DMX node catalog items.**
  - This is the better long-term answer, but it needs content (data assets, a category tag, meshes) and a rule for several distros.
  - Fixed and automatic anchors deliver measured lengths now; placeable items can feed the same anchor later.
- **Straight-line distance × routing factor (ADR 0012).**
  - Rejected: for a fixture 6 m up and 1 m across, the straight line is 6.1 m. The real route is 7 m, and the factor plus the margin would double-count.
- **Splitting the margin per kind (power vs DMX).** Not asked for; one margin keeps the list explainable.

## 3. Verification

- `StageCraft.Report.ShoppingList.Rules` covers:
  - sanitizing;
  - route measuring;
  - the assembly edge cases: an exact fit, an exact multiple, the rest needing the longest stock again, NaN, no margin, and half-metre stock;
  - the rigging classification, clamps and bonds.
- `StageCraft.Report.ShoppingList.Build` covers:
  - cable lines by length, totals, the distro line and rigging counts;
  - the overweight and missing-weight warnings;
  - determinism;
  - distros limited by outputs and by the feed, and the automatic distro position;
  - the CSV and PDF content, and the empty stage.
- `StageCraft.Report.CableSchedule.Dmx` and `.Export` are updated:
  - the node is fixed at the origin;
  - joined runs "5 x 20 m + 10 m";
  - the automatic node position and the route up a truss;
  - the new CSV headers.
- A scripted run loaded "R38 Cues" and exported. The PDF and CSV were checked by eye (STATE.md #41).

## 4. Consequences

- Plans now show a length for every run, so the "site" entries are gone. Old exports are not regenerated.
- The cable totals are only as good as the anchor positions. The PDF and CSV print where the distros and the node were assumed to stand, and how they were placed (automatic or fixed).
- **Not modelled:**
  - per-distro placement near its loads;
  - TRUE1 daisy-chaining (one home-run per load is still the plan);
  - truss and speaker rigging (only lighting fixtures get hardware);
  - mains feed cables;
  - spares.
- **Rollback:** delete `StageShoppingList.*` and `StageCablingSettings.*`, restore ADR 0012's measuring in `StageCableSchedule.cpp` (git history), and drop the shopping tests. The cable schedule's public API returns to ADR 0012's signature.
