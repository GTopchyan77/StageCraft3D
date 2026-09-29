---
type: system
status: doc
tags: [methodology, conversion]
---

# Conversion Guide — quick start

The full, canonical method is **[[BP_TO_CPP_PLAYBOOK]]** (with this project's two overrides at the
top of it). This page is the short loop and the tool index.

## The loop

1. **Inventory** — `py Plugins\UnrealNGGMCP\Tools\bp-extract --from-vault` (editor open). One JSON per asset in `_extracted/`.
2. **Order** — `py Plugins\UnrealNGGMCP\Tools\bp-extract order`. Builds the dependency graph → tiers, cycles, per-function
   engine-only split. Output: [[Conversion Order]].
3. **Brief** — `py Plugins\UnrealNGGMCP\Tools\bp-extract brief <Name>`. Per-BP pre-flight matrix (blockers, migration order,
   don't-port flags) in `_briefs/<System>/`.
4. **Codegen** — `py Plugins\UnrealNGGMCP\Tools\bp-extract codegen --system Data` (then other tiers). Emits UENUM/USTRUCT/
   UINTERFACE + empty typed parents into `Source/GameAnimationSample/<System>/`. Review, then build.
5. **Readiness gate** (per target, before any C++) — pull the target's full dependency closure from
   [[Conversion Order]] (deps + "Unresolved external") and each dep's `status:` frontmatter; classify
   done / in-progress / external and write a **GO / GO-with-seams / BLOCKED** verdict. Playbook
   **Phase 3.4**. This is the cheap step that stops you starting a blocked target.
6. **Migrate** per the playbook: **duplicate** `BP_Foo → BP_Foo_CPP`, reparent the **copy** to the C++
   parent, port functions engine-only-first, keep the original untouched, swap external refs last.

## Core rules (see the playbook for detail)

- **Dependency order, bottom-up.** Convert tier 0 (enums/structs/curves) first; never convert before
  the BPs a thing depends on. [[Conversion Order]] is the live order.
- **`in-progress` = a seam boundary, not a blocker.** A dep with `status: in-progress` has a C++ mirror
  but the BP still uses the BP type — *different reflection types* ("struct dichotomy"). Type unification
  (CoreRedirects) is a **confirmed dead end** and abandoned (playbook **Override 5**). So an `in-progress`
  shared type means: convert the target **whole** with C++ mirror types internally and **bridge the BP edge
  with a seam** (reflection filler / `BlueprintNativeEvent` / `UINTERFACE`). It's **GO-with-seams**, not
  blocked. Only `done` deps need no seam.
- **Don't port toolkit/external roots.** A dep in "Unresolved external" (engine type, or a BP rooted in
  the level-prototyping toolkit) is *not* converted — expose the one method you need via a UInterface
  (playbook Phase 3.7) instead of porting its class chain.
- **Originals untouched** — work on the `*_CPP` duplicate.
- **Empty parents have zero UPROPERTY** that shadows a BP variable (the #1 reparent foot-gun).
- **Engine-only functions first**, then project-dependent (the brief splits them).
- **Cycles** (e.g. `AC_PreCMCTick ↔ SandboxCharacter_CMC`): break the seam with an interface /
  forward-decl / soft ref, or convert the cluster together.
- **AnimBP port = playbook Phase 3.11.** A `UAnimInstance` port keeps the AnimGraph in BP, reparents the
  **original** ABP (the mesh's `AnimClass` points at it), mirrors only Category-A (engine-typed) vars, keeps
  Category-B (`S_*`/`E_*`) vars BP-side (read by reflection-on-self), and ports getters as `BlueprintPure,
  meta=(BlueprintThreadSafe)`. Watch the reparent-rename-`_0` trap.

## Editor-cycle discipline (the real speed lever)

The dominant cost is editor restarts (~45-100s), not compilation. **Batch C++ edits, run one editor
cycle, verify at the end** — never restart per commit. Live Coding handles existing-function bodies
without a restart; new `UCLASS`/`UFUNCTION`/`UPROPERTY`/`USTRUCT`/`UENUM` need a full
kill→build→launch. Keep a per-BP `lint-baseline` of known struct-node false positives (playbook Phase 4).

## Links
- Method: [[BP_TO_CPP_PLAYBOOK]]
- Order: [[Conversion Order]]
- Tracker: [[Conversion Tracker]]
- Tool: `Plugins/UnrealNGGMCP/Content/Python/bp-extract/README.md` (Python ≥3.10, stdlib only; per-project config via `BlueprintConvert.json`)
