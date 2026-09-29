# Reference Matching & Production Workflow

Deep guidance for building UMG widgets that match a provided visual reference (Figma, screenshot, mockup) with pixel-accurate fidelity, then scale cleanly across resolutions.

Read this when the user provides any visual reference (image, Figma link, mockup) or mentions pixel-accurate, 4K scaling, component extraction, or matching a design spec.

---

## 1. Core Philosophy

Build in this priority order, never out of order:

1. **Structure first** — identify zones, anchors, containers, hierarchy.
2. **Components second** — extract anything repeated into reusable `WBP_`s.
3. **Style third** — color, opacity, corner radius, typography.
4. **Assets last** — textures, materials, icons. Always use placeholders during structure pass.

Reason: art swaps often, structure rarely. If structure depends on specific art, every art iteration breaks layout. Placeholders keep the skeleton stable.

---

## 2. Component Extraction Rule

**If a visual element appears 2+ times in the reference, it becomes its own `WBP_`.** No exceptions.

Common extractables for game UI:

- `WBP_PanelShell` — layered background shell (bg + border + optional title slot + content slot)
- `WBP_HeaderBar` — title on left, action icons (close/search/back) on right
- `WBP_ActionButton` — primary/secondary variants, icon + label
- `WBP_StatusPill` — small highlighted label chip (e.g. "NEW", "EQUIPPED")
- `WBP_InfoPair` — label on left, value on right (stat rows, settings rows)
- `WBP_GridItemBase` — icon/image + quantity badge + selection state
- `WBP_IconSlot` — small category icon holder (tabs, filters)

Each reusable component must expose parameters via `UPROPERTY(EditAnywhere, BlueprintReadWrite)` or editor-time Blueprint variables:
- text values (title, label, value)
- state (selected, disabled, highlighted)
- colors / tint
- visibility toggles for optional sub-elements

Use `PreConstruct` to reflect these in the Designer preview.

---

## 3. Pixel-Accurate Spacing

### 3.1 The Rule
Preserve all paddings, margins, and edge offsets with pixel-level precision. Do not "approximate" distances by eye.

Respect exact spacing for:
- panel-to-screen offsets
- element-to-element gaps
- internal container padding
- header/footer edge distance
- corner radius and border thickness

### 3.2 Spacing Scale
Snap every spacing value to this scale unless the reference explicitly demands otherwise:

| Token | Range | Use |
|---|---|---|
| `XS` | 4–8 | Tight icon-to-label, chip internals |
| `S` | 10–16 | Button padding, card internal gap |
| `M` | 20–32 | Panel padding, section gaps |
| `L` | 36–48 | Zone separation, large panel offsets |

No random values like `23` or `37`. If the reference shows `23`, either snap to `24` (`M`) or document it as a deliberate exception.

### 3.3 How to measure
- From Figma: use the inspector panel — copy values directly.
- From a screenshot at known resolution: use a pixel ruler tool; measure twice.
- From a mockup without spec: measure canonical elements (button height, panel padding) and derive scale tokens.

---

## 4. FullHD → 4K Scaling

Common scenario: reference is delivered at `1920x1080` but the game's UI target is `3840x2160` (scale factor **2.0**).

### 4.1 What to scale
Multiply every spatial value by the scale factor:
- widths and heights
- paddings and margins
- offsets from edges
- corner radius
- border / outline thickness
- icon and image placeholder sizes
- font sizes (unless typography defines its own scaling system)

### 4.2 What NOT to scale
- Anchor coordinates (they're normalized 0–1, resolution-agnostic)
- Alignment values (normalized)
- Opacity, tint, color values

### 4.3 Preferred implementation
Set **DPI Scaling** (Project Settings → User Interface) with a curve based on screen height. This lets you author at `1920x1080` with scale `1.0` and have UMG scale everything automatically at higher resolutions. Manual 2x multiplication is only needed when authoring natively at 4K.

---

## 5. Size Stability Rule

Critical UI elements must have locked dimensions. Use `SizeBox` wrappers on:
- buttons (and all clickable controls)
- cards / tiles
- inventory/grid slot items
- icon containers
- key headers and footers

### When to use each SizeBox field

| Field | Use |
|---|---|
| `Width Override` / `Height Override` | Exact fixed size |
| `Min Desired Width/Height` | Lower bound (content can grow beyond) |
| `Max Desired Width/Height` | Upper bound (content can't exceed) |
| `MinAspectRatio` / `MaxAspectRatio` | Constrain proportions |

Lock with both `MinDesired` + `MaxDesired` at the same value when the element must not shift size based on content.

**Do not rely on auto-size** for critical components — text changes and localization will break the layout.

---

## 6. Container Selection Guide

Extended guidance beyond the main SKILL.md table — when to reach for which container in a reference-matching context:

### Canvas Panel
- Screen-level anchoring only. The root of each `WBP_Screen`.
- Fixed-position HUD zones.
- **Avoid deep nesting** of multiple Canvas Panels. Flatten to one per screen where possible.

### Overlay
- Layered visuals at the same position: background + border + content + effects.
- Prefer this over stacking Canvas Panels for layered looks.

### Vertical Box / Horizontal Box
- Default choice for linear flow with clean spacing.
- Use `Padding` on the slot and `Spacing` on the box itself (UE 5.3+) for consistent gaps.

### UniformGridPanel
- Inventory grids, ability bars, any fixed-size repeated grid.
- All cells forced to the same size.

### GridPanel
- Non-uniform row/column layouts (settings menus with mixed-width controls).

### ScaleBox
- Only when content must auto-fit an area while preserving aspect ratio.
- Do not use as a default — it complicates hit-testing and DPI math.

### ScrollBox
- Long dynamic lists or grids. Pair with `ListView`/`TileView` for recycling at scale.

### WidgetSwitcher
- Explicit named states: `Loading`, `Empty`, `Content`, `Error`; or `Collapsed` / `Expanded`.
- **Use this instead of manual `SetVisibility` chains** for screen states — it's readable and exhaustive.

---

## 7. Anchor Strategy by Zone

Root-level macro zones should be anchored by purpose, not by appearance:

| Zone | Anchor | Alignment |
|---|---|---|
| Left panel | Left stretch `(0,0)/(0,1)` | `(0, 0)` |
| Right panel | Right stretch `(1,0)/(1,1)` | `(1, 0)` |
| Top bar (full width) | Top stretch `(0,0)/(1,0)` | `(0, 0)` |
| Top bar (centered) | Top center `(0.5,0)/(0.5,0)` | `(0.5, 0)` |
| Footer actions (right) | Bottom-right `(1,1)/(1,1)` | `(1, 1)` |
| Footer actions (centered) | Bottom-center `(0.5,1)/(0.5,1)` | `(0.5, 1)` |
| Full-screen modal | Stretch `(0,0)/(1,1)` | `(0, 0)` |

**Do not mix conflicting anchors and alignment** in the same widget unless intentional (e.g., anchor right `(1,0)` with alignment `(1,0)` for right-inset elements — correct; anchor right with alignment `(0,0)` — almost always wrong).

---

## 8. Visual Placeholders Without Textures

When building structure before art is ready (or when texture assignment is manual):

1. Add `Image` widgets at every visual slot.
2. Name each placeholder by **purpose**, not appearance:
   - `IMG_PanelBg` — panel background
   - `IMG_PanelBorder` — panel border
   - `IMG_ItemThumb` — item thumbnail
   - `IMG_Icon_Category` — category icon
   - `IMG_DecorativeLine` — decorative separator
3. Configure non-texture styling while leaving the resource empty:
   - `ColorAndOpacity` (tint)
   - `Brush → DrawAs → RoundedBox` (corner radius, no texture needed)
   - `Brush → OutlineSettings` (width, radius, color)
4. Leave `Brush → Image` (texture/material) empty or at default.

This produces a fully laid-out screen that clearly shows every art slot for later fill-in.

---

## 9. Interaction State Rules

### 9.1 Prefer CommonButtonBase
Use `CommonButtonBase` (from Common UI plugin) over regular `Button` for interactive controls. It gives you gamepad navigation, sound hooks, and state handling for free.

Use regular `Button` only for:
- Very simple local interactions
- Debug/editor UI
- When Common UI plugin is not enabled

### 9.2 All clickable controls must define four states
- `Normal`
- `Hovered`
- `Pressed`
- `Disabled`

### 9.3 State feedback guidelines
- Use color / opacity changes — not size changes — for state.
- Keep padding stable across states to avoid layout jump.
- Keep sounds and events separate from visual style (Common UI style assets).
- Use `WidgetSwitcher` for mode/state screens, not manual `SetVisibility` chains.

---

## 10. Naming Conventions (UI_Rules Standard)

All widget-tree elements get a purpose-prefix. Purpose before appearance.

### Widgets
- `WBP_*` — Widget Blueprints: `WBP_HUD`, `WBP_InventoryScreen`, `WBP_PanelShell`

### Controls
- `BTN_*` — Buttons / `CommonButtonBase`: `BTN_Confirm`, `BTN_Close`
- `TXT_*` — Text blocks: `TXT_Title`, `TXT_ItemCount`
- `IMG_*` — Images: `IMG_PanelBg`, `IMG_Icon_Sword`

### Containers
- `CNV_*` — Canvas Panel: `CNV_Root`, `CNV_HUDLayer`
- `OVR_*` — Overlay: `OVR_ItemSlot`
- `VBX_*` — Vertical Box: `VBX_StatsList`
- `HBX_*` — Horizontal Box: `HBX_IconRow`
- `SBX_*` — Size Box: `SBX_ButtonFrame`
- `GRID_*` — Grid / Uniform Grid Panel: `GRID_Inventory`
- `SCR_*` — Scroll Box: `SCR_ItemList`
- `SWT_*` — Widget Switcher: `SWT_ScreenState`

### Rule
Names describe **purpose**, not appearance. `BTN_Confirm` not `BTN_GreenButton`. `IMG_PanelBg` not `IMG_BlueTexture`.

---

## 11. Data-Driven Content

Lists and grids populate dynamically from data. Never hand-place dynamic items in the Designer.

- One item prefab (`WBP_GridItemBase` or similar).
- One generation loop (Blueprint `For Each Loop` or C++ `CreateWidget` loop).
- Bind data via a struct passed into the item's `InitializeFromData` function.

### Explicit state widgets
Every data-driven list/grid needs three distinct state presentations, wrapped in a `WidgetSwitcher`:
- **Loading** — spinner or skeleton placeholder
- **Empty** — icon + "No items" message + optional call-to-action
- **Content** — the populated list/grid

Never fake empty/loading states by hiding the content widget — make them first-class.

---

## 12. Recommended Build Order

When starting a new screen from a reference, follow this order strictly:

1. **Root anchors and macro zones** — place empty Canvas Panel, establish left/right/top/bottom regions with correct anchors.
2. **Panel shells and major containers** — drop `WBP_PanelShell` or raw Overlay/VBox/HBox stubs into each zone.
3. **Reusable components** — create the `WBP_`s identified in the extraction pass. Author them standalone first.
4. **Content blocks and state switchers** — populate panels with components; wrap dynamic areas in `WidgetSwitcher`.
5. **Interactions / navigation** — wire buttons, hover states, input focus, tab order.
6. **Typography / colors / radius** — apply the design-token pass consistently across all widgets.
7. **Image placeholders** — add `IMG_*` widgets at every art slot with rounded-box brushes.
8. **Final visual pass** — compare side-by-side with reference, fix misalignments, verify spacing.

Skipping step 3 (reusable components) and jumping to step 4 is the most common mistake — it produces duplicated widgets that drift apart over time.

---

## 13. Build-Order Anti-Patterns

- **Designing to specific art assets** before structure is locked. Art changes; structure shouldn't have to.
- **Copy-pasting a panel** three times instead of making a `WBP_`. Each copy will diverge.
- **Hand-tweaked offsets** on Canvas Panel children to fake a grid. Use `UniformGridPanel` or `HBox`/`VBox`.
- **Per-widget `SetVisibility` chains** to manage screen state. Use `WidgetSwitcher`.
- **Random spacing values** (`23`, `37`, `41`). Snap to the scale tokens or document the exception.
- **Authoring at 4K natively** instead of using DPI Scaling with a `1920x1080` base.
