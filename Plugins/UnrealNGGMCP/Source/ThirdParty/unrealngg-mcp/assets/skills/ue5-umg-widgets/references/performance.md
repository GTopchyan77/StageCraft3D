# UE5 UMG — Performance Reference

## Table of Contents
1. The Golden Rule: Event-Driven UI
2. Invalidation Box
3. Retainer Box
4. Visibility Costs
5. FormatText & Text Performance
6. Profiling Commands
7. Anti-Patterns to Avoid

---

## 1. The Golden Rule: Event-Driven UI

Slate/UMG use **invalidation and caching** — widgets only re-render when explicitly invalidated.

**Property Bindings = called every frame** (per Slate render cycle). This is like having
a Tick event on every bound property. For > 5–10 bindings on complex screens, this tanks performance.

**Rule:** Use Property Bindings only for:
- Very simple screens (< 5 bindings)
- Debug/editor tools
- Values that genuinely update every frame (countdown timers as a last resort)

**For everything else, use Events:**

```
Character takes damage
  → Broadcast OnHealthChanged delegate
    → Widget receives event
      → Calls UpdateHealth() once
```

This fires only when health actually changes — not 60+ times per second.

---

## 2. Invalidation Box

Wrapping widgets in an **Invalidation Box** caches their geometry. Cached widgets are:
- Not pre-passed (layout not recalculated)
- Not ticked
- Not repainted

Unless they are **invalidated** (something inside changes).

### When to Use
- Large sections of HUD that rarely change (minimap border, static frame art)
- Lists of items where each item rarely updates
- Any widget subtree that is mostly static

### How to Use (Designer)
Drag an **Invalidation Box** from the Palette and wrap your static widget subtree inside it.

### Manually Invalidating (Blueprint)
```
Child Widget → Invalidate Layout and Volatility
```

### Volatile Widgets
Inside an Invalidation Box, mark a specific child as **Volatile** (Details panel → Is Volatile).
Volatile widgets redraw every frame but don't force the whole Invalidation Box to invalidate.
Use for the one dynamic element (e.g., health bar fill) inside an otherwise static frame.

### Cache Relative Transforms
For Invalidation Boxes inside scrolling containers (Scroll Box), enable
**Cache Relative Transforms** — this avoids updating all vertex buffers on scroll,
instead updating only the shader transform matrix.

### Debug Invalidation
```
Widget Reflector: Ctrl+Shift+W → Toggle Invalidation Debugging
Console: SlateDebugger.Invalidate.Enable 1
Console: SlateDebugger.InvalidationRoot.Enable 1
```

---

## 3. Retainer Box

The **Retainer Box** renders its children to a render target at a **reduced tick rate**.
Useful for complex sub-UIs that don't need to update at full frame rate.

Configure:
- **Render on Invalidation** — re-render only when a child invalidates
- **Render on Phase** — re-render every N frames (e.g., Phase=2 = 30fps on a 60fps game)

Use for: inventory screens, map overlays, anything decorative that doesn't need 60fps updates.

---

## 4. Visibility Costs

Calling `SetVisibility` has a CPU cost even when the state doesn't change. Avoid calling it
every tick. Cache current visibility and only call when it needs to change.

| Transition | Cost Notes |
|---|---|
| Visible → Hidden | Removes from render; layout still computed |
| Visible → Collapsed | Removes from render AND layout recalculation |
| Hidden → Visible | Layout already exists; cheap |
| Collapsed → Visible | Must recalculate layout; more expensive |

**For frequent show/hide** (e.g., crosshair): prefer **Hidden** (avoids layout recalc cost on show).
**For panels that are gone a long time** (e.g., inventory): prefer **Collapsed**.

---

## 5. FormatText & Text Performance

`FormatText` (localizable string formatting) is not free — it can cost ~0.04ms per call on console.

**Don't call FormatText every tick.** Instead:
```
Cache the integer/float value
On value change event → call FormatText → update TextBlock
```

Example (C++):
```cpp
void UMyWidget::UpdateScore(int32 NewScore)
{
    if (NewScore == CachedScore) return;  // Early out if unchanged
    CachedScore = NewScore;
    
    ScoreLabel->SetText(FText::Format(
        NSLOCTEXT("UI", "Score", "Score: {0}"),
        FText::AsNumber(NewScore)
    ));
}
```

For non-localized debug text, `FText::FromString(FString::Printf(...))` is faster.

---

## 6. Profiling Commands

```
stat slate              — Overall Slate tick and render times
stat slateverbose       — Detailed per-widget breakdown
Ctrl+Shift+W            — Widget Reflector (live widget tree inspector)
SlateDebugger.Invalidate.Enable 1  — Show invalidation events
r.DumpRenderTargets     — Dump render targets (useful for Retainer Box debug)
```

Key stats to watch:
- `STAT_SlateTickTime` — CPU time for Slate tick (game thread)
- `STAT_SlateRenderingRTTime` — Render thread time for Slate

Targets (desktop): Slate tick < 0.5ms, Slate render < 1ms.

---

## 7. Anti-Patterns to Avoid

| Anti-Pattern | Why | Fix |
|---|---|---|
| Property Bindings on complex screens | Called every frame | Use Event Dispatchers / Delegates |
| Tick event in Widget Blueprint with heavy logic | Runs 60x/sec | Use Timers or events |
| Deep Canvas Panel nesting | Expensive layout recalc | Use Vertical/Horizontal Box + Overlay |
| Creating new child widgets every frame | GC pressure, no pooling | Use ListView/TileView |
| Calling SetVisibility every tick | Unnecessary invalidation | Cache state, call only on change |
| FormatText every frame | 0.04ms+ per call | Cache value, update on change |
| Huge texture atlases with lots of padding | Wastes memory, wrong hit area | Trim padding; use UMG padding instead |
| Building all screens in one giant Widget Blueprint | Slow iteration, merge conflicts | Split into composable WBP_ components |
| Having game systems reference widgets directly | Tight coupling, crashes on widget removal | Widgets observe game; game doesn't reference widgets |
