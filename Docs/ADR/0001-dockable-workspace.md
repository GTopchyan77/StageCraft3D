# ADR 0001 — Dockable multi-window workspace, detachable viewport, preferences

- **Status:** Accepted by Gevor on 2026-10-02.
  - Decisions: (1) the workspace runs in Standalone Game and packaged builds only, and PIE keeps the current HUD; (2) panels use the existing view-binding pattern, not the MVVM plugin; (3) start the Phase 0 spike.
  - **Phase 0 spike: GO** (§8). The spike found three engine behaviours (F12–F14) that changed parts of §3; §3.1 and §3.5 were updated.
- **Date:** 2026-10-02
- **Engine:** UE 5.8 (`C:/Program Files/Epic Games/UE_5.8`). Every engine fact below was checked in that source; paths are relative to `Engine/Source`.
- **Related:** Rules.md (Architecture; Engineering Standard §5, §9, §16, §18, §21–22, §26–29, §36–37), STATE.md #16 and #18.

---

## 1. Context

StageCraft is a packaged desktop app with an editor-style workflow. Today:
- The whole UI is one UMG widget, `WBP_StageCraftHUD`. `AModularPlayerController::BeginPlay` creates it and calls `AddToViewport` (`ModularPlayerController.cpp:104-111`).
- `WBP_StageCraftHUD` is a CanvasPanel with the inspector at the top right and the fader bank at the bottom left.
- Panels can't be moved, docked, floated or persisted. There is no settings system, and no key remapping: Enhanced Input mappings are built in code (`ModularPlayerController.cpp:144-254`).

Goal: an Unreal-Editor-like workspace.
- Panels can be dragged, docked, split, tabbed, floated and resized.
- The 3D viewport can be detached to another window or monitor.
- Layouts are named and saved to disk.
- A Preferences window covers UI scale, layouts, viewport, graphics and key bindings.

## 2. Verified engine facts that shape the design

| # | Fact | Source | Consequence |
|---|---|---|---|
| F1 | The docking framework (`FTabManager`, `FGlobalTabmanager`, `SDockTab`, `FLayout`/`FArea`/`FSplitter`/`FStack`, `RestoreFrom`, `TryInvokeTab`) is in the **runtime Slate** module. It has no editor dependency; the only `WITH_EDITOR` blocks are cosmetic (`TabManager.cpp:2056, 3883`). | `Runtime/Slate/Public/Framework/Docking/TabManager.h` | Usable in a packaged game. `InvokeTab` no longer exists in 5.8, so use `TryInvokeTab` (`TabManager.h:1087`). |
| F2 | `FLayout::ToString` / `NewFromString` round-trip JSON. The layout name is stored in the JSON and acts as the version key. | `TabManager.cpp:391-456` | We own the file format and versioning. |
| F3 | `GEditorLayoutIni` / `GEditorPerProjectIni` are only loaded under `WITH_EDITOR`, so they are empty in a game. `FLayoutSaveRestore::SaveToConfig` also writes a JSON copy to `UserSettingsDir/<product>/Editor/` (`LayoutService.cpp:170-256`). | `ConfigCacheIni.cpp:6638-6656` | Don't use `FLayoutSaveRestore` or editor configs. Write our own files under `Saved/`. |
| F4 | The game viewport is created with `RenderDirectlyToWindow(true)` (`GameEngine.cpp:213-225`). `FSceneViewport` reads this once, in its constructor (`SceneViewport.cpp:76`). `UGameEngine::CreateGameViewportWidget()` is **virtual** (`GameEngine.h:52`). | | A `UGameEngine` subclass that creates the viewport with `RenderDirectlyToWindow(false)` lets the viewport render into a dock tab, as editor viewports do (`SLevelViewport.cpp:3900`). This costs one extra composite per frame and **must be measured** (§22). |
| F5 | The UMG layer (`SGameLayerManager` and `AddViewportWidgetContent`) is a child of the `SViewport` (`GameViewportClient.cpp:1285-1327, 3376`). | | Viewport overlays (toasts, fly speed) move with the viewport. Dock panels are separate. The UMG DPI curve scales by **viewport** size (`SGameLayerManager.cpp:467-489`). |
| F6 | The engine finds the viewport's window dynamically (`FSceneViewport::FindWindow`, `SceneViewport.cpp:1915`; `UGameViewportClient::GetWindow`, refreshed each `Draw`). Mouse position uses the viewport's own geometry (`SceneViewport.cpp:213-224`). | | Picking, the gizmo and deprojection keep working after re-parenting. After a move, call `FSlateApplication::RegisterGameViewport` again (`SlateApplication.cpp:2524-2551`). |
| F7 | **Closing the main game window quits the app** (`GameEngine.cpp:247, 768-780`). | | The main window stays the docking root and is never just a viewport host. |
| F8 | `UGameEngine::OnViewportResized` (not virtual; bound at `GameEngine.cpp:273`) writes the viewport size into `GSystemResolution` and `UGameUserSettings` whenever the main viewport resizes in windowed mode. `ResizeFrame` resizes **whatever window holds the viewport** (`SceneViewport.cpp:1657-1867`). | | A docked viewport would save the tab's size as the "screen resolution". The workspace must own window sizing and must never call `ApplyResolutionSettings` while the viewport is docked. The Phase 0 spike verifies the mitigation. |
| F9 | `UGameEngine::SwitchGameWindowToUseGameViewport` (not virtual) sets the main window's content back to `UGameEngine::GameViewportWidget` whenever the content is anything else. | `GameEngine.cpp:729-760` | Superseded by F12, which shows it runs **every frame**. |
| F12 | *(Found in the Phase 0 spike.)* With the movie player enabled, `FEngineLoop::Tick` calls `GetMoviePlayer()->WaitForMovieToFinish(true)` **every frame** (`LaunchEngineLoop.cpp:5902`). Its no-movie branch calls `SwitchGameWindowToUseGameViewport()` (`DefaultGameMoviePlayer.cpp:620-628`). `IsMoviePlayerEnabled()` cannot be turned off in Shipping (`MoviePlayer.cpp:123-133`). So every packaged game resets the main window's content to `GameViewportWidget` every frame; normally that is a no-op. | | The dock root cannot simply be set as the window content. `UStageCraftGameEngine::SetMainWindowContent` puts it inside a host `SViewport` (no viewport interface) and points the public member `GameViewportWidget` at that host, so the per-frame switch keeps *our* content. The virtual `GetGameViewportWidget()` is overridden to keep returning the real scene viewport. It is used by `UWidgetComponent` hit testing and nDisplay; input, cursor and mouse lock use `UGameViewportClient::GetGameViewportWidget()`, which reads the `FSceneViewport` (`GameViewportClient.cpp:507-514`). The first `UGameEngine::Tick` registers the member with Slate once (`GameEngine.cpp:2022`), so for that one tick the member is the real viewport. |
| F13 | *(Spike.)* `FTabManager::SpawnTab` only spawns when the spawner's previous tab is gone (`!Spawner->SpawnedTabPtr.IsValid()`, `TabManager.cpp` `SpawnTab`). `CloseAllAreas` only *requests* window destruction (deferred). For an embedded area, `SDockingArea::GetParentWindow()` returns the window even when the area does not manage it, so `CloseAllAreas` would request destroying the main window. | | Never use `CloseAllAreas` for panels. To apply a layout, release the Workspace tab's content, destroy floating panel windows immediately (`DestroyWindowImmediately`), drop the panel tab manager, and restore into a fresh one. |
| F14 | *(Spike.)* Nomad panel tabs restored directly into `FGlobalTabmanager`'s primary area ended up outside the main window's dock area ("This asset editor has no docked tabs"). | | Use the structure Slate's standalone apps use: one **major tab** (the Workspace, tab well hidden) in the global primary area, owning a panel `FTabManager` (`NewTabManager`) whose panel tabs hold the viewport and widgets. `SDockTab::GetParentWindow()` is null for tabs inside it, so the host window is found with `FSlateApplication::FindWidgetWindow`. |
| F10 | `FSlateApplication::SetApplicationScale` scales every Slate window (`SWindow.cpp:825-830`). `UUserInterfaceSettings::ApplicationScale` scales only the game layer. | | The "UI scale" preference uses `SetApplicationScale`. |
| F11 | `FGlobalTabmanager::SetRootWindow` makes torn-off tabs native children of the root window (`FDockingDragOperation.cpp:351-414`). The Trace Insights standalone tool uses this exact setup (`TraceInsightsModule.cpp:307-358`). | | Reference pattern for our shell. |

## 3. Decision

### 3.1 Layering (where each responsibility lives)

```
┌──────────── Main SWindow (UGameEngine::GameViewportWindow) ─────────────────────────────────┐
│ host SViewport  ← UGameEngine::GameViewportWidget points here (F12), via UStageCraftGameEngine │
│  └ SDockingArea (FGlobalTabmanager::RestoreFrom, layout "StageCraft_Root_v1")                │
│     └ Major tab "StageCraft.Workspace" (tab well hidden; owns the panel FTabManager)         │
│        └ panel SDockingArea (panel FTabManager::RestoreFrom, layout "StageCraft_Workspace_v1")│
│           ├─ Panel tab "StageCraft.Panel.Viewport"  → real game SViewport (+ UMG overlay)    │
│           ├─ Panel tab "StageCraft.Panel.Inspector" → UMG WBP_StageInspectorPanel (TakeWidget)│
│           └─ Panel tab "StageCraft.Panel.FaderBank" → UMG WBP_StageFaderBank                 │
└───────────────────────────────────────────────────────────────────────────────────────────────┘
   floating SWindows (children of the main window) hold torn-off panel tabs, incl. the viewport
```

| Class | Kind | One responsibility | Lifetime / owner |
|---|---|---|---|
| `UStageCraftGameEngine` | `UGameEngine` subclass (`DefaultEngine.ini` `GameEngine=`) | Make the game viewport dockable and keep the engine from fighting that. It creates the viewport with a separate render target (F4) and owns the main window's content (`SetMainWindowContent` / `RestoreMainWindowViewport`, F12). It also removes the engine's resize-to-resolution handling (F8). | The engine |
| `UStageWorkspaceSubsystem` | `UGameInstanceSubsystem` | The workspace's public API, state machine and delegates. It owns the shell and the panel registry and talks to the layout store and settings. | The whole run. Slate windows outlive level travel, so this is not a World subsystem. |
| `FStageWorkspaceShell` | Plain C++ class (`TSharedPtr` in the subsystem so Slate can bind it weakly with `CreateSP`; private header) | All Slate: the root layout and the Workspace major tab, the panel tab manager and its spawners, layout restore (F13), hosting the viewport tab, tab event hooks. It keeps Slate types out of the UObject header. | Created by the subsystem on the first local-controller registration (when supported), destroyed in `Deinitialize` |
| `FStageLayoutStore` | Plain C++ class (pure logic, no world) | Read, write, validate and version layout files. Lists user and built-in layouts. Clamps window rects to the current monitors. | Owned by the subsystem. Unit-testable. |
| `UStagePanelDefinition` | `UPrimaryDataAsset`, scanned by the Asset Manager (like `StageProduct`) | Describes a panel as data: `PanelTag` (`StageCraft.Panel.*`), display name, icon (soft), `TSoftClassPtr<UUserWidget>` widget class, `EStagePanelKind` (`EngineViewport` / `Widget`), single or multi instance, default stack. | Assets |
| `UStageCraftUserSettings` | `UGameUserSettings` subclass (`GameUserSettingsClassName`) | Per-machine preferences: UI scale, startup layout, restore-on-launch, viewport preferences (FOV, default fly speed). Validated setters and a settings version. Graphics settings are inherited (scalability). | Engine-owned singleton, `GameUserSettings.ini` |
| Key bindings | `UEnhancedInputUserSettings` (engine) | Player-mappable keys, saved by Enhanced Input. | Per local player |
| `UStagePreferencesPanel` | UMG view (C++ base + WBP) | Editor-Preferences-style categories (General, Layout, Viewport, Graphics, Keyboard). It reads settings, commits through validated setters and reads values back. | Dock tab |

Workspace rules:
- **No game state lives in the workspace.** Panels keep using the controller request bridge (Rules.md "Safe data binding"). The workspace never touches the session, profile or economy.
- **Source of truth (§36):**
  - Slate's tab managers are the only truth for the live layout. The subsystem does not mirror it. The shell keeps one derived map, the last reported host per panel, only to report `PreviousHost`. It is re-synchronised after every restore.
  - The layout store owns what is saved on disk.
  - `UStageCraftUserSettings` owns preferences.
- **No Tick and no polling (§21).** Everything is driven by events:
  - `SDockTab::OnTabRelocated` / `OnTabClosed` (per tab, bound in the spawner)
  - `FTabManager::SetOnPersistLayout` (Slate already defers these saves on a 5-second ticker, `TabManager.cpp:1103-1125`)
  - `FGlobalTabmanager::OnActiveTabChanged_Subscribe`
  - Loading movies need no hook: when one ends, the engine's window switch restores `GameViewportWidget`, which is our host (F12).

### 3.2 State model (§37)

```cpp
UENUM() enum class EStageWorkspaceState : uint8 { Uninitialized, Unsupported, Ready, ShuttingDown };
UENUM() enum class EStagePanelHost     : uint8 { Closed, MainWindow, FloatingWindow };
```
- `Unsupported` means there is no `UGameEngine` main window. This covers the editor and PIE, where the editor owns the viewport, and dedicated servers. The subsystem then does nothing, and the existing `AddToViewport` HUD path keeps working unchanged.
- While a layout is being restored, the shell ignores relocate and close callbacks (a scoped guard in `FStageWorkspaceShell`, not a subsystem state). Restoring a layout fires them for every tab, and they must not trigger a save or broadcasts.

### 3.3 Public API (sketch; contracts in the header per §30)

```cpp
UCLASS()
class MODULARSCENEBUILDER_API UStageWorkspaceSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()
public:
    // Game thread only. Valid in state Ready; otherwise returns false / no-op and logs LogStageWorkspace.
    UFUNCTION(BlueprintCallable, Category="StageCraft|Workspace") bool OpenPanel(FGameplayTag PanelTag);
    UFUNCTION(BlueprintCallable, Category="StageCraft|Workspace") bool ClosePanel(FGameplayTag PanelTag);
    UFUNCTION(BlueprintCallable, Category="StageCraft|Workspace") EStagePanelHost GetPanelHost(FGameplayTag PanelTag) const;

    UFUNCTION(BlueprintCallable, Category="StageCraft|Workspace") EStageLayoutResult ApplyLayout(FName LayoutName);
    UFUNCTION(BlueprintCallable, Category="StageCraft|Workspace") EStageLayoutResult SaveCurrentLayoutAs(FName LayoutName);
    UFUNCTION(BlueprintCallable, Category="StageCraft|Workspace") void ResetToDefaultLayout();
    UFUNCTION(BlueprintCallable, Category="StageCraft|Workspace") TArray<FName> GetAvailableLayouts() const;

    // AModularPlayerController calls these in BeginPlay / EndPlay (it is the local UI bridge). Panels that need
    // the controller are (re)built against it. On travel, tabs keep their place and show an empty state until rebind.
    void RegisterLocalController(class AModularPlayerController* Controller);
    void UnregisterLocalController(class AModularPlayerController* Controller);

    UPROPERTY(BlueprintAssignable) FOnStagePanelHostChanged OnPanelHostChanged; // (Tag, NewHost, OldHost): docked / detached / closed
    UPROPERTY(BlueprintAssignable) FOnStageLayoutChanged    OnLayoutChanged;    // (LayoutName)
    UPROPERTY(BlueprintAssignable) FOnStageViewportHostChanged OnViewportHostChanged; // viewport window changed (monitor move, float, dock)
private:
    TSharedPtr<class FStageWorkspaceShell> Shell;   // shared so Slate binds it weakly (CreateSP)
    TUniquePtr<class FStageLayoutStore>    LayoutStore;
    UPROPERTY() TMap<FGameplayTag, TObjectPtr<class UStagePanelDefinition>> PanelDefinitions;
    TWeakObjectPtr<class AModularPlayerController> LocalController;
    EStageWorkspaceState State = EStageWorkspaceState::Uninitialized;
};
```
- The requested `OnWindowDocked` / `OnWindowDetached` are one `OnPanelHostChanged` event with old and new host. It is the same transition, so a single delegate avoids listeners having to pair two events (§16).
- Ownership of UMG panels: the panel's `UUserWidget` is created with the local controller as owner (`GetOwningPlayer` is what the inspector binds through, `StageParameterViewWidget.cpp:21`). `TakeWidget()` gives the tab an `SObjectWidget`, which keeps the UObject alive while the tab exists. The subsystem holds only weak references. When the controller unregisters, tab content is replaced with an empty state, which releases the widget, and panels unbind in `NativeDestruct` as they do today.

### 3.4 Layout files (§26)

- **Location:** `Saved/StageCraft/Layouts/<Name>.json`. Built-in layouts ("Stage Design", "Show Control", "Dual Monitor") are defined in C++ with the `FTabManager::NewLayout` builders. They are never written to disk, so **Reset** always works.
- **File format:**
  ```json
  { "FormatVersion": 1, "Name": "My Layout", "SlateLayoutVersion": "StageCraft_Workspace_v1",
    "MainWindow": { "X":0,"Y":0,"W":1920,"H":1080,"Maximized":true },
    "SlateLayout": "<FLayout::ToString() JSON>" }
  ```
- **Validation on load:**
  - Unknown `FormatVersion`, or a `SlateLayoutVersion` that doesn't match: fall back to the default layout, log a warning, and rename the file to `.bak` (it is never deleted).
  - `NewFromString` returns null: same fallback.
  - Rects are clamped to the current monitors' work areas (`FSlateApplication::GetCachedDisplayMetrics`), so a window from an unplugged second monitor comes back on screen.
  - Unknown panel IDs (a removed panel) are dropped. The Phase 0 spike verifies how `RestoreFrom` treats a tab with no spawner.
- **When it saves:**
  - On `OnPersistLayout` (already deferred by Slate) into the current user layout, if there is one.
  - On `Deinitialize`.
  - On "Save Layout As".
  - Files are a few KB. A synchronous write is acceptable on shutdown. Other saves go through `AsyncTask` on a copied `FString`, with no UObject capture (§29).

### 3.5 Viewport detach rules

- The viewport tab can't be closed (`OnCanCloseTab` returns false). It can be floated, moved to another monitor and dragged back. A floating window that contains it can't be closed. The spike confirms closing behaviour for floating windows. The fallback is to re-dock the viewport into the main window when its floating window closes.
- After any relocation of the viewport tab:
  - Call `FSlateApplication::RegisterGameViewport(viewportWidget)` again.
  - Broadcast `OnViewportHostChanged`.
- Fullscreen (Alt+Enter / F11, `DefaultInput.ini:61-62`) acts on the viewport's current window (F8). That gives a deliberate "fullscreen viewport on monitor 2" feature. It goes through the workspace so the main window is never resized by accident.
- Resolution settings (F8): while the workspace is active, the main window's rect comes from the layout file, not `UGameUserSettings` resolution. The Preferences Graphics page hides the resolution/window-mode controls. The engine's `OnViewportResized` writes into user settings; `UStageCraftGameEngine::Start` removes that binding (`FViewport::ViewportResizedEvent.RemoveAll(this)`). The spike verified that `GSystemResolution` and the saved resolution stay untouched while layout changes dock, float and resize the viewport (§8). Resizing the main window by hand is still to be tested.

## 4. Alternatives considered

| Alternative | Why not |
|---|---|
| Render the 3D view a second time into a `SceneCapture`/render target for the detached window | Doubles the scene rendering cost. Input and picking would need a second path, and there would be two views of the truth. |
| UMG-only fake docking (draggable `UUserWidget` frames in one canvas) | No OS windows, so it can't go to a second monitor. It would reimplement Slate docking badly. |
| A custom `SWindow` per panel without `FTabManager` | It would reimplement splitting, tabs, drag previews and serialization that Slate already ships and tests. |
| A `UWorldSubsystem` as the window manager | Windows and layouts must survive level travel, so a world-lifetime owner would have to tear the UI down and rebuild it on every travel. |
| UE's `ModelViewViewModel` plugin for MVVM | It adds a plugin and a second binding style. The project already has a working view pattern: bind subsystem/actor delegates, commit through a validated path, read back (STATE.md #16, #18). Rules §40 says to respect existing conventions. It can be reconsidered if panels multiply. |
| A separate `StageCraftWorkspace` module | Panels depend on game classes (controller, selection), so a split now would only add a dependency edge. Revisit if the shell becomes reusable. |

## 5. Consequences

- The workspace only works in **Standalone Game or packaged builds.** In PIE the editor owns the viewport (F4/F7), so the subsystem reports `Unsupported` and the current HUD keeps working. To test the workspace from the editor, use **Play ▸ Standalone Game**.
- `WBP_StageCraftHUD` loses the inspector and the fader bank (they become panels) and keeps only viewport overlays.
- Build.cs gains private `Json` (layout file wrapper) and possibly `ApplicationCore`. The Phase 0 spike confirms which are needed.
- Rendering the viewport to a separate target costs something. The spike measures it before anything is committed (§22).
- UMG viewport overlays scale with the viewport size (F5). A small docked viewport shrinks overlays unless the DPI rule is adjusted. That is a follow-up.

## 6. Implementation plan

| Phase | Scope | Exit criteria |
|---|---|---|
| **0. Spike** (branch, nothing merged) | `UStageCraftGameEngine` + a minimal shell: viewport tab + one UMG panel. Float the viewport to monitor 2, dock it back, resize. Test picking, the gizmo drag, RMB fly, Delete. Test `OnViewportResized` removal, closing a floating window, a restored layout with an unknown tab ID, and level travel. Baseline `stat unit` / `stat gpu` with direct and separate rendering. | Each F8/F9/§3.5 question answered with evidence, the cost measured, go/no-go recorded in this ADR. |
| **1. Shell** | `UStageCraftGameEngine`, `UStageWorkspaceSubsystem`, `FStageWorkspaceShell`, `UStagePanelDefinition` (+ `StageCraft.Panel.*` tags), Viewport / Inspector / Fader panels, the default layout, controller register/unregister, the Window menu. | Standalone Game: dock, split, tab, float and resize all work. Panels keep working after level travel. PIE is unchanged. |
| **2. Layouts** | `FStageLayoutStore`, Save As, Load, Reset, the Layouts menu, restore on launch, monitor clamping. | Automation tests: round-trip, corrupt file, version mismatch, off-screen clamp. A manual restart restores the layout. |
| **3. Preferences** | `UStageCraftUserSettings` (UI scale, startup layout, viewport preferences), the Preferences panel (General, Layout, Viewport, Graphics). | Settings persist, invalid values are clamped (tested), and UI scale applies live. |
| **4. Key bindings** | Move the code-built IMCs into IA/IMC assets with player-mappable keys. Add `UEnhancedInputUserSettings` and a Keyboard page with conflict display. | Remap, then restart, then the remap is still active. Conflicts are shown. Defaults can be restored. |
| **5. Polish** | An `FSlateStyleSet` from `UStageCraftUITheme` for the dock chrome, the overlay DPI rule, a per-monitor fullscreen viewport, more built-in layouts. | Visual review by Gevor. |

## 7. Verification strategy (§31–32)

- **Unit (automation, no world):**
  - `FStageLayoutStore`: round-trip, null or corrupt JSON, unknown version, rect clamping to fake monitor sets, unknown panel IDs.
  - `UStageCraftUserSettings`: setters clamp and normalize.
- **Integration (Standalone Game, manual checklist plus logs):**
  - Every dock operation.
  - Viewport float, monitor change and fullscreen.
  - Gizmo and picking inside a floated viewport.
  - Level travel with panels open.
  - Restart restore.
  - Deleting a layout file while it is active.
  - Unplugging the second monitor (clamp).
- **Performance:** frame time and GPU, before and after separate-target rendering, at 1080p and 4K. Recorded in STATE.md.
- **Packaged:** a Shipping build smoke test. Editor-only APIs must not be used (§28).

## 8. Phase 0 spike results (2026-10-02, branch `spike/dockable-workspace`)

**Verdict: GO.** The design works in Standalone Game. Three engine behaviours (F12–F14) required design changes, now reflected in §3. The spike code is shaped as the start of Phase 1 rather than throwaway.

**How it was tested.** The `StageCraft.Workspace.*` development commands were scripted with `StageCraft.Workspace.Delay` in `-ExecCmds`. Each run launched Standalone Game (`UnrealEditor.exe <project> -game -windowed -ResX=1600 -ResY=900`) on a three-monitor desktop. Logs and per-window captures (`PrintWindow`) were checked after every step.

| Check | Result |
|---|---|
| Viewport in a dock tab, panels around it (default layout) | Pass. The viewport renders at its tab size (1212×637). Inspector and Faders are the real UMG panels, bound to the controller. |
| Cursor picking in the docked viewport | Pass. The cursor is moved through the controller to the viewport centre; `GetHitResultUnderCursor` equals a centre deprojection (MATCH). |
| Float the viewport to monitor 3 | Pass. A window at (3890, 50) shows the viewport at 1280×688, and the panels stay in the main window. |
| Cursor picking in the floated viewport | Pass. MATCH with the OS cursor on monitor 3 (4533, 429). |
| Layout that names a removed panel | Pass. The unknown tab is dropped silently and the other panels restore. |
| Reset from floated to default | Pass. The floating window is destroyed and the viewport is re-docked. |
| Level travel (`open L_StageTest`) | Pass. The layout is kept, panels rebuild against the new controller, the main window does not move, and picking still matches. |
| Saved resolution (F8) | Pass. `GSystemResolution` stays 1600×900 and the saved `UGameUserSettings` resolution is unchanged while layouts resize the viewport. |
| Ensures, warnings, crashes | None, after the fixes for F12 (the first-tick registration ensure) and F13. |
| Development and Shipping Game targets | Development compiles clean. Shipping result is recorded in STATE.md #21. |

**Frame cost (§22).** RTX 3060 Ti, 1600×900, `r.VSync 0`, `t.MaxFPS 0`, 20 s FPS chart after a 6 s settle, one run each.

| Mode | GPU | Render thread | Game thread |
|---|---|---|---|
| Engine default (`-StageDirectViewport`, renders directly to the window) | 5.65 ms | 6.23 ms | 1.78 ms |
| Workspace (separate render target, viewport-only layout) | 5.97 ms | 6.58 ms | 1.86 ms |
| Difference | +0.32 ms (+5.7 %) | +0.35 ms | +0.08 ms |

Caveats:
- This is a single sample per mode.
- The direct run also drew the UMG HUD overlay and the workspace run had none, so the true difference may be slightly higher.
- Repeat with several runs and at 4K before the ship decision.

**Not verified yet (needs Gevor, by mouse):**
- Dragging tabs to split or re-dock, tearing off by mouse, dragging a floating viewport window between monitors.
- RMB fly, gizmo drag and Delete inside a floated viewport.
- Alt+Enter / F11 fullscreen with a floated viewport.
- Closing a floating window that holds the viewport with its OS close button.
- Resizing the main window by hand (F8).
