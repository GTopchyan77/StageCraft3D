---
type: home
tags: [moc]
---

# Game Animation Sample — Blueprint → C++ Conversion

Working vault for porting the **GameAnimationSample** (UE 5.7) Blueprint project to the
`GameAnimationSample` C++ module. It is both a **conversion tracker** (per-asset status) and
a **design knowledge base** (how each system works + porting strategy).

## Progress

```dataview
TABLE WITHOUT ID status AS Status, length(rows) AS Assets
FROM "Assets"
WHERE type = "asset"
GROUP BY status
SORT status ASC
```

> Full breakdown in [[Conversion Tracker]].

## Methodology
- [[Capabilities]] — what can be done & how to ask (the menu).
- [[Commands (RU)]] — шпаргалка команд на русском.
- [[BP_TO_CPP_PLAYBOOK]] — the canonical method (full playbook + this project's overrides).
- [[Conversion Guide]] — quick start: the loop + tool index.
- [[Conversion Order]] — the generated, dependency-sorted worklist (run `py Tools\bp-extract order`).

## Systems
- [[Overview]] — scope and architecture
- [[Character Pawns]] — the CMC character (Mover variant removed), GameMode, PlayerController
- [[Components]] — PreCMCTick, TraversalLogic, VisualOverrideManager
- [[Movement Modes]] — Walking / Falling / Slide + transitions
- [[Traversal System]] — mantle / vault / hurdle / climb
- [[Animation and Retargeting]] — ABPs, retarget chain, modifiers, notifies, control rigs
- [[Smart Objects]] — StateTree-driven interactions
- [[AI and StateTree]] — NPC controller and behaviour trees
- [[Camera System]] — camera asset, director, rigs
- [[Data Types]] — enums, structs, curves, function libraries

## How to use
1. Open an asset note under `Assets/` and set its `status` (`todo` → `in-progress` → `done`).
2. Fill `cpp_target` with the class you create, and capture gotchas under **C++ Conversion Notes**.
3. New notes: use the **Templater** templates in `_Templates/`.
