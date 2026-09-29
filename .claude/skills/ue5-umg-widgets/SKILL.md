---
name: ue5-umg-widgets
description: >
  Expert guidance for creating, structuring, and optimizing UMG (Unreal Motion Graphics)
  widgets in Unreal Engine 5. Use this skill whenever the user asks about: Widget Blueprints,
  UMG, UUserWidget, Canvas Panels, anchors, HUD creation, UI layout, widget C++ binding,
  BindWidget, NativeConstruct, widget hierarchy, screen-size-safe UI, Invalidation Box,
  event-driven UI updates, widget performance, reusable UI components, pixel-accurate
  layouts, matching a Figma/reference design, FullHD to 4K scaling, DPI scaling, CommonUI /
  CommonButtonBase, or any UE5 UI/HUD work. Also trigger for questions about showing/hiding
  widgets, adding widgets to viewport, widget communication, data binding in UMG, naming
  conventions for widgets, or building a UI from a provided reference image/mockup. If the
  user is building any kind of game UI in UE5 — even if they don't explicitly say "UMG" —
  use this skill.
---

# UE5 UMG Widget Skill

Comprehensive guidance for building UMG widgets in Unreal Engine 5 — from layout and anchors
to C++ binding, reference-matching, and performance. Read this file fully before starting
any widget work.

**Reference files to read based on the task:**
- `references/reference-matching.md` — **Read this when the user provides a visual reference** (Figma, screenshot, mockup), mentions pixel-accurate spacing, 4K scaling, reusable components, or building to match a design spec.
- `references/cpp-patterns.md` — Advanced C++ patterns: BindWidget variations, delegates, list views.
- `references/performance.md` — Deep performance: Invalidation Box, Slate caching, profiling commands.

---

## 1. Core Concepts

**UMG (Unreal Motion Graphics)** is UE5's UI framework. All UI elements are **Widgets**.
A **Widget Blueprint (WBP_)** defines both layout (Designer tab) and logic (Graph tab).
The underlying engine layer is **Slate** — UMG is a Blueprint-friendly wrapper around Slate.

Widget lifecycle order:
1. `PreConstruct` — runs in editor preview; safe for setting defaults
2. `NativeConstruct` / `Construct` — runs at runtime when widget is added to viewport
3. `NativeTick` / `Tick` — runs every frame (avoid heavy logic here)
4. `NativeDestruct` / `Destruct` — runs when widget is removed

---

## 2. Core Principles (Read First)

These govern every widget decision in this skill:

1. **Match reference structure first, style second, assets last.** Structure is stable; art iterates.
2. **Repeated UI must become a reusable `WBP_`** if it appears 2+ times in the reference.
3. **Use UMG containers correctly** — do not force layout with random offsets.
4. **Separate behavior/state logic from visuals** — supports later art swap without touching code.
5. **Prefer scalable rules over one-off fixes.**
6. **Names describe purpose, not appearance.** `BTN_Confirm`, not `BTN_GreenButton`.
7. **Snap spacing to a scale** (see Section 6). Random values like `23` or `37` are a smell.

If the user provides a visual reference, also read `references/reference-matching.md`
before starting — it covers the full reference-matching workflow.

---

## 3. Creating a Widget Blueprint

**In Editor:**
1. Content Browser → Right-click → User Interface → Widget Blueprint
2. Name with prefix `WBP_` (e.g., `WBP_HUD`, `WBP_HealthBar`)
3. Open → Designer tab for layout, Graph tab for logic

**Adding to Viewport (Blueprint):**
```
Create Widget (Class: WBP_HUD, Owning Player: Get Player Controller 0)
→ Add to Viewport
→ Store reference in variable for later removal
```

**Adding to Viewport (C++):**
```cpp
// In PlayerController or HUD class
UPROPERTY(EditDefaultsOnly, Category="UI")
TSubclassOf<UUserWidget> HUDWidgetClass;

UPROPERTY()
TObjectPtr<UUserWidget> HUDWidget;

// In BeginPlay:
if (HUDWidgetClass)
{
    HUDWidget = CreateWidget<UUserWidget>(this, HUDWidgetClass);
    if (HUDWidget)
        HUDWidget->AddToViewport();
}
```

**Removing from Viewport:**
```
Widget Reference → Remove from Parent
```
Set the variable to null/invalid after removing.

---

## 4. Layout Containers — Which to Use

| Container | Use Case |
|---|---|
| **Canvas Panel** | Absolute positioning; HUD overlays; use anchors for screen safety. Use only at the top level or for intentional absolute overlays. |
| **Vertical Box** | Stack items top-to-bottom (menus, lists). Default for linear flow. |
| **Horizontal Box** | Stack items left-to-right (icon + label, stat rows). Default for linear flow. |
| **Overlay** | Layer widgets on top of each other (bg + border + content + effects). Prefer over stacked Canvas Panels. |
| **Uniform Grid Panel** | Uniform-sized grids (inventory, ability bars). |
| **Grid Panel** | Non-uniform row/column layouts (mixed-width settings rows). |
| **Scale Box** | Force a child to fit/fill a space while maintaining aspect ratio. Use sparingly — complicates hit-testing and DPI math. |
| **Size Box** | Enforce min/max/fixed size constraints on a child. Required for locking critical UI elements (see Section 7). |
| **Scroll Box** | Scrollable list. Pair with `ListView`/`TileView` for recycling at scale. |
| **Widget Switcher** | Explicit named states (`Loading`, `Empty`, `Content`) — use instead of manual `SetVisibility` chains. |

**Key rule:** A container is only as large as the largest thing inside it (except Vertical/Horizontal Box, which sum their children). Do NOT nest multiple Canvas Panels or Size Boxes without good reason — it creates fragile, hard-to-maintain layouts.

**Workflow tip:** Use right-click → "Wrap With" / "Replace With" in the Hierarchy to swap containers quickly without losing slot settings.

For deeper container selection guidance (especially for reference-matching), see
`references/reference-matching.md` Section 6.

---

## 5. Anchors — The Most Important UMG Concept

Anchors keep widgets positioned correctly across all screen sizes and aspect ratios.

### How Anchors Work

Anchors are **normalized coordinates** where:
- `(0, 0)` = top-left of the Canvas Panel
- `(1, 1)` = bottom-right of the Canvas Panel

The **Anchor Medallion** (yellow diamond in Designer) shows where the widget's reference point is on the canvas.

When the screen resizes, the widget stays at a **fixed offset from its anchor point**.

### Preset Anchors (Use These First)

Access via: select widget → Details panel → **Anchors** drop-down.

| Preset | Min | Max | Use For |
|---|---|---|---|
| Top-Left | (0,0) | (0,0) | Fixed top-left elements |
| Top-Center | (0.5,0) | (0.5,0) | Centered top bar |
| Top-Right | (1,0) | (1,0) | Fixed top-right elements |
| Center | (0.5,0.5) | (0.5,0.5) | Centered HUD element |
| Bottom-Left | (0,1) | (0,1) | Fixed bottom-left |
| Bottom-Center | (0.5,1) | (0.5,1) | Bottom-center UI |
| Bottom-Right | (1,1) | (1,1) | Fixed bottom-right |

**Tip:** Hold **Ctrl** while clicking a preset anchor to move both the anchor AND the widget simultaneously.

### Stretching Anchors

When **Min ≠ Max**, the widget **stretches** with the canvas:

| Pattern | Min | Max | Effect |
|---|---|---|---|
| Full horizontal stretch | (0, 0) | (1, 0) | Widget fills full width |
| Full vertical stretch | (0, 0) | (0, 1) | Widget fills full height |
| Full screen fill | (0, 0) | (1, 1) | Widget fills entire screen |
| Bottom bar stretch | (0, 1) | (1, 1) | Full-width bottom bar |

When stretching, the position fields change to **Left/Right/Top/Bottom offsets** (margins from the edges), not position + size.

### Split Anchor Medallion

Drag one pin of the medallion to split it — this enables stretching on that axis. Split on both axes = full stretch.

### Alignment (Pivot Point)

**Alignment** controls which point of the widget maps to the anchor position:
- `(0, 0)` = widget's top-left aligns to anchor
- `(0.5, 0.5)` = widget's center aligns to anchor (use for centered elements)
- `(1, 0)` = widget's top-right aligns to anchor (use for right-aligned elements)

**Do not mix conflicting anchors and alignment** in the same widget unless intentional.
`Anchor (1,0)` paired with `Alignment (1,0)` = correct right-inset pattern.
`Anchor (1,0)` paired with `Alignment (0,0)` = almost always a bug.

**Tip:** Hold **Shift** when clicking a preset anchor to also set the alignment automatically.

### Common Anchor Patterns

**HUD Health Bar (Top-Left, fixed)**
```
Anchor: (0, 0) / (0, 0)   Position: X=20, Y=20   Alignment: (0, 0)
```

**Minimap (Top-Right, fixed)**
```
Anchor: (1, 0) / (1, 0)   Position: X=-20, Y=20  Alignment: (1, 0)
```

**Crosshair (Center screen)**
```
Anchor: (0.5, 0.5) / (0.5, 0.5)   Position: X=0, Y=0   Alignment: (0.5, 0.5)
```

**Bottom Action Bar (Full-width stretch)**
```
Anchor: (0, 1) / (1, 1)   Left=20, Right=20, Top=-80, Bottom=20
```

**Full-screen overlay**
```
Anchor: (0, 0) / (1, 1)   Left=0, Right=0, Top=0, Bottom=0
```

### Safe Zone Widget

For console/mobile, wrap your root Canvas Panel with a **Safe Zone** widget. This automatically insets content to avoid screen edges and notches.

```
Safe Zone
  └── Canvas Panel (your actual layout)
```

---

## 6. Spacing Scale & Pixel Accuracy

Preserve all paddings, margins, and edge offsets with pixel-level precision. Do not approximate distances by eye.

Respect exact spacing from the reference for:
- panel-to-screen offsets
- element-to-element gaps
- internal container padding
- header/footer edge distance
- corner radius and border thickness

### Spacing Scale

Snap every spacing value to one of these tokens unless the reference explicitly demands otherwise:

| Token | Range | Use |
|---|---|---|
| `XS` | 4–8 | Tight icon-to-label, chip internals |
| `S` | 10–16 | Button padding, card internal gap |
| `M` | 20–32 | Panel padding, section gaps |
| `L` | 36–48 | Zone separation, large panel offsets |

Random values like `23` or `37` are a smell — snap to the nearest token or document the exception.

### FullHD → 4K Scaling

If the reference is at `1920x1080` but the UI targets `3840x2160`, scale factor = **2.0**.

Multiply by the scale factor:
- widths, heights, paddings, margins
- edge offsets, corner radius, border thickness
- icon/image sizes, font sizes

Do **not** scale:
- Anchor coordinates (already normalized 0–1)
- Alignment values (normalized)
- Colors, opacity

**Preferred approach:** Set **DPI Scaling** (Project Settings → User Interface) with a curve based on screen height. Author at `1920x1080` with DPI Scale 1.0 and let UMG scale automatically. Manual 2x multiplication is only for native-4K authoring.

Full scaling details in `references/reference-matching.md` Section 4.

---

## 7. Size Stability

Critical UI elements must have locked dimensions. Wrap these in a `SizeBox` (`SBX_`):
- buttons and all clickable controls
- cards / tiles
- inventory/grid slot items
- icon containers
- key headers and footers

| Field | Use |
|---|---|
| `Width Override` / `Height Override` | Exact fixed size |
| `Min Desired Width/Height` | Lower bound (content can grow) |
| `Max Desired Width/Height` | Upper bound (content can't exceed) |

Lock with both `MinDesired` + `MaxDesired` at the same value when the element must not shift based on content.

**Do not rely on auto-size** for critical components — text changes and localization will break the layout.

---

## 8. Reusable Components

**If a UI element appears 2+ times, it becomes its own `WBP_`.** No exceptions.

Common extractables:
- `WBP_PanelShell` — layered bg + optional title slot + content slot
- `WBP_HeaderBar` — title + right action icons (close/search/back)
- `WBP_ActionButton` — primary/secondary variants
- `WBP_StatusPill` — small highlighted label chip
- `WBP_InfoPair` — label/value row
- `WBP_GridItemBase` — icon + quantity + selection state
- `WBP_IconSlot` — small category icon holder

Expose parameters on each component:
- text values (title, label, value)
- state (selected, disabled, highlighted)
- colors / tint
- visibility toggles for optional sub-elements

Use `PreConstruct` to reflect parameters in the Designer preview.

For the full component catalog and extraction workflow, see
`references/reference-matching.md` Section 2.

---

## 9. Visual Placeholders Without Textures

When building structure before art is ready:

1. Add `Image` widgets at every visual slot.
2. Name each placeholder by purpose: `IMG_PanelBg`, `IMG_PanelBorder`, `IMG_ItemThumb`, `IMG_Icon_Category`, `IMG_DecorativeLine`.
3. Configure non-texture styling while leaving the resource empty:
   - `ColorAndOpacity` (tint)
   - `Brush → DrawAs → RoundedBox` (corner radius without a texture)
   - `Brush → OutlineSettings` (width, radius, color)
4. Leave `Brush → Image` empty or default.

This produces a fully laid-out screen showing every art slot for later fill-in without blocking structure work.

---

## 10. C++ + Widget Blueprint Integration

### Setup: Build.cs

Ensure these are in your `PublicDependencyModuleNames`:
```csharp
"UMG", "Slate", "SlateCore"
```
Add `"CommonUI"` if using `CommonButtonBase` (recommended — see Section 11).

### C++ Base Class Pattern

```cpp
// WBP_HealthBar inherits from this in Blueprint
UCLASS(Abstract)
class MYGAME_API UHealthBarWidget : public UUserWidget
{
    GENERATED_BODY()

protected:
    // Blueprint widget with EXACT same name must exist in WBP
    UPROPERTY(BlueprintReadOnly, meta=(BindWidget))
    TObjectPtr<UProgressBar> HealthBar;

    UPROPERTY(BlueprintReadOnly, meta=(BindWidget))
    TObjectPtr<UTextBlock> TXT_Health;

    // Optional — no compile error if missing in Blueprint
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional))
    TObjectPtr<UTextBlock> TXT_Shield;

    virtual void NativeConstruct() override;

public:
    UFUNCTION(BlueprintCallable)
    void SetHealth(float Current, float Max);
};
```

```cpp
// .cpp
void UHealthBarWidget::NativeConstruct()
{
    Super::NativeConstruct();
    // BindWidget pointers are valid here, not in constructor
}

void UHealthBarWidget::SetHealth(float Current, float Max)
{
    if (HealthBar)
        HealthBar->SetPercent(Current / Max);

    if (TXT_Health)
        TXT_Health->SetText(FText::AsNumber(FMath::RoundToInt(Current)));
}
```

---

## 11. Naming Conventions

All widget-tree elements use a **purpose-prefix**. Describe what it *is for*, not what it *looks like*.

### Widgets
- `WBP_*` — Widget Blueprints: `WBP_HUD`, `WBP_InventoryScreen`, `WBP_PanelShell`

### Controls
- `BTN_*` — Buttons / `CommonButtonBase`: `BTN_Confirm`, `BTN_Close`
- `TXT_*` — Text blocks: `TXT_Title`, `TXT_ItemCount`
- `IMG_*` — Images: `IMG_PanelBg`, `IMG_Icon_Sword`

### Containers
| Prefix | Type |
|---|---|
| `CNV_*` | Canvas Panel |
| `OVR_*` | Overlay |
| `VBX_*` | Vertical Box |
| `HBX_*` | Horizontal Box |
| `SBX_*` | Size Box |
| `GRID_*` | Grid / Uniform Grid Panel |
| `SCR_*` | Scroll Box |
| `SWT_*` | Widget Switcher |

### C++ classes
- `U` + descriptive name for `UUserWidget` subclasses: `UHUDWidget`, `UInventoryWidget`

### Rule
`BTN_Confirm` — good. `BTN_GreenButton` — bad. `IMG_PanelBg` — good. `IMG_BlueTexture` — bad.

---

## 12. Interaction & State

### Prefer CommonButtonBase
Use `CommonButtonBase` (Common UI plugin) over regular `Button` for interactive controls. Gives gamepad navigation, sound hooks, and state handling for free. Use regular `Button` only for simple local interactions or when Common UI is not enabled.

### Four required states for every clickable control
- `Normal`
- `Hovered`
- `Pressed`
- `Disabled`

### State feedback guidelines
- Use color / opacity changes — not size changes — for state feedback.
- Keep padding stable across states to avoid layout jump.
- Keep sounds and events separate from visual style.
- Use `WidgetSwitcher` for mode/state screens, not manual hide chains.

---

## 13. Updating Widget Data — The Right Way

### ❌ DO NOT: Property Bindings for frequently-updating values
Property Bindings run **every frame** (called per Slate render cycle). For simple UIs
this is acceptable, but for complex widgets or mobile, it kills performance.

### ✅ DO: Event-Driven Updates

**Blueprint approach — Event Dispatcher:**
```
In Character BP:
  OnHealthChanged (Event Dispatcher)
    → Bind in widget's Construct event
    → Call UpdateHealth function on event fire
```

**C++ approach — Delegates:**
```cpp
// In Character.h
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnHealthChanged, float, Current, float, Max);

UPROPERTY(BlueprintAssignable)
FOnHealthChanged OnHealthChanged;

// In widget's NativeConstruct:
if (AMyCharacter* Char = Cast<AMyCharacter>(GetOwningPlayerPawn()))
{
    Char->OnHealthChanged.AddDynamic(this, &UHealthBarWidget::SetHealth);
}
```

### When Property Bindings ARE OK
- Very simple UIs with < 5 bindings
- Debug/editor widgets
- Values that truly change every frame (timers, velocity)

---

## 14. Widget Communication

### Widget → Game World
```
Get Owning Player → Get Player Controller → Cast → Call function
Get Owning Player Pawn → Cast → Access character data
```

### Game World → Widget
Use **Event Dispatchers** (Blueprint) or **Delegates** (C++). Never have game logic
directly reference a widget — the widget should observe the game, not the reverse.

### Widget → Widget (child to parent)
Store a reference to parent widget or use an event dispatcher. Avoid direct
cross-widget references when possible — use a centralized manager or game instance.

### Widget Manager Pattern (recommended for complex UIs)
```cpp
// In GameInstance or PlayerController
UPROPERTY()
TObjectPtr<UMainHUDWidget> HUDWidget;

UPROPERTY()
TObjectPtr<UInventoryWidget> InventoryWidget;

void ShowInventory();
void HideInventory();
void UpdateHUD(FPlayerStats Stats);
```

---

## 15. Data-Driven Lists & Grids

Lists and grids populate dynamically from data. Never hand-place dynamic items in the Designer.

- One item prefab (`WBP_GridItemBase` or similar).
- One generation loop (Blueprint `For Each` or C++ `CreateWidget` loop).
- Bind data via a struct passed into the item's `InitializeFromData` function.

### Explicit state widgets
Every data-driven list/grid needs three distinct states in a `WidgetSwitcher`:
- **Loading** — spinner or skeleton
- **Empty** — icon + "No items" + optional CTA
- **Content** — populated list/grid

Never fake empty/loading by hiding the content widget — make them first-class states.

---

## 16. Showing / Hiding Widgets

Prefer **Collapsed** or **Hidden** over removing/re-adding from viewport:

| Visibility | Renders? | Takes Space? | Receives Input? |
|---|---|---|---|
| Visible | Yes | Yes | Yes |
| Hidden | No | Yes | No |
| Collapsed | No | No | No |
| Not Hit-Testable | Yes | Yes | No |
| Hit-Test Invisible | Yes | Yes | Children only |

```cpp
Widget->SetVisibility(ESlateVisibility::Visible);
Widget->SetVisibility(ESlateVisibility::Hidden);     // keeps layout space
Widget->SetVisibility(ESlateVisibility::Collapsed);  // removes from layout
```

**Avoid:** calling `SetVisibility` on an already-hidden widget every tick — still costs CPU.

---

## 17. Input and Focus

To allow keyboard/gamepad input to a widget:
```
Set Input Mode UI Only (Player Controller, In Widget to Focus: self)
Set Show Mouse Cursor: true

// On close:
Set Input Mode Game Only
Set Show Mouse Cursor: false
```

For menus that need mouse + game input simultaneously:
```
Set Input Mode Game and UI
```

---

## 18. Performance Checklist

See `references/performance.md` for deep dive. Quick rules:

- ✅ Use **Events/Delegates** instead of Property Bindings
- ✅ Wrap static UI sections in **Invalidation Box** (caches geometry)
- ✅ Use **Collapsed** not **Hidden** for widgets that free up layout recalculations
- ✅ Avoid **Tick** in widgets — use timers or delegates
- ✅ Use **Textures** not Materials for static art where possible
- ✅ Build at **DPI Scale 1.0** and target resolution (e.g. 1920x1080)
- ✅ Minimize **built-in padding** in imported textures — use UMG padding instead
- ✅ Reuse widgets with **ListView/TileView** for scrollable lists
- ❌ Don't nest Canvas Panels inside Canvas Panels deeply
- ❌ Don't use Size Box inside Size Box inside Canvas (fragile and slow)
- ❌ Don't call `FormatText` every frame — cache and update only on change

---

## 19. Recommended Build Order

When starting a new screen (especially from a reference):

1. **Root anchors and macro zones** — empty Canvas Panel, correct anchors on each region.
2. **Panel shells and major containers** — `WBP_PanelShell` or raw Overlay/VBox/HBox stubs.
3. **Reusable components** — create the `WBP_`s identified in the extraction pass. Author standalone first.
4. **Content blocks and state switchers** — populate panels; wrap dynamic areas in `WidgetSwitcher`.
5. **Interactions / navigation** — wire buttons, hover states, input focus, tab order.
6. **Typography / colors / radius** — design-token pass across all widgets.
7. **Image placeholders** — `IMG_*` widgets at every art slot with rounded-box brushes.
8. **Final visual pass** — compare side-by-side with reference, fix misalignments, verify spacing.

**Common mistake:** skipping step 3 and jumping to step 4. Produces duplicated widgets that drift apart over time.

For the full reference-matching workflow with extraction and scaling, see
`references/reference-matching.md`.

---

## 20. Work Style & General Best Practices

### Structure
- One C++ base class per logical widget type; Blueprint subclass for layout only.
- Keep logic in C++, keep visual layout in Blueprint.
- Small, composable widgets — build from reusable User Widgets, not monolithic screens.

### Resolution & Scale
- Set target resolution before starting (e.g. 1920×1080).
- Author at **DPI Scale 1.0** — use the Scale drop-down in Designer tab.
- Use the **Screen Size** drop-down to test other resolutions — do not author at those sizes.
- Art assets: use 96 DPI in Photoshop for pixel-perfect match to UMG measurements.

### Hierarchy Discipline
- Prefer **Vertical/Horizontal Box + Overlay** over Canvas Panel for flow layouts.
- Use Canvas Panel only at the top level or for intentional absolute overlays.
- Wrap reusable sub-elements as their own `WBP_` and drop into parent.

### Designer Tips
- **Preview background:** select Root → Details → Preview Image to design over a game screenshot.
- Hold **Ctrl** + drag anchor to move anchor AND widget together.
- Right-click hierarchy → **Wrap With** to quickly test container changes.
- **Widget Reflector** (Ctrl+Shift+W) — inspect live widget tree and invalidation state.

---

## References

- `references/reference-matching.md` — Reference-matching workflow, component extraction, pixel-accurate spacing, FullHD→4K scaling, size stability, build order anti-patterns.
- `references/cpp-patterns.md` — Full C++ patterns: BindWidget, NativeConstruct, delegates, list views.
- `references/performance.md` — Deep performance: Invalidation Box, Slate caching, profiling commands.
