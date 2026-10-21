# ADR 0013: Preferences, key bindings, and the audit's scene and import list risks (R2, R6, R8)

- **Status:** accepted. See STATE.md #40.
- **Related:**
  - ADR 0001 §Preferences (`UStageCraftUserSettings`), ADR 0004 §4 (scene files), ADR 0006 (runtime model import), ADR 0010 §3–4 (autosave, scene format).
  - `Docs/PROJECT_AUDIT.md` risks R2, R6 and R8 and its fix plan items 1, 2, 6 and 7.
  - Rules.md: "Where state lives" (per-user settings → `ULocalPlayerSubsystem`), "Safe data binding", Engineering Standard §21 (no polling), §26 and §55 (validate saved and external data), §59 (resource handling).

## 1. Problem

Gevor's milestone (2026-10-21) asked for:

1. **Preferences and key bindings.** Users can remap the viewport, navigation and editing actions.
2. **The audit's open findings:**
   - **R2:** a partial scene load must not lose items.
   - **R6:** restored state must be sanitized.
   - **R8:** `Imports.json` must survive corruption and unexpected shutdowns.
3. Tests, a clean package, and STATE.md.

What exists today:
- **Shortcuts are hard-coded.** `AModularPlayerController` builds one Enhanced Input action per shortcut, with Ctrl and Shift as chord actions. The Ctrl+Shift+Z mapping must come before Ctrl+Z, so Enhanced Input's injected chord blockers stop Z from also undoing. The fly keys W/A/S/D/Q/E are a second, higher-priority context.
- **There is no input or navigation preference.** Look sensitivity is a pawn default.
- **Preferences are spread out.** They live in `UStageCraftUserSettings` (GameUserSettings.ini), and each has one owner that writes it: audio (`UStageAudioSubsystem`), snapping (`UStageSnappingComponent`), the grid (`UStageViewportComponent`) and the render panel.
- **R2:**
  - `ApplyDocument` skips items whose catalog entry is unknown, or that the rules refuse now, and then calls `MarkClean`. A later Save rewrites the file without them.
  - `LoadScene` does not wait for imported models that are still re-importing at start-up, so their items count as unknown.
- **R6:**
  - Fixture attributes and the patch are taken from the file as they are. Audio gain, delay and splay rely on editor-only `ClampMin` metadata.
  - The restore archive is created with `bLoadIfFindFails = true`, so an object path in saved state would load that package.
- **R8:**
  - A corrupt `Imports.json` reads as an empty list. The next import then saves only the new file.
  - Every "atomic" writer (scenes, layouts, the session, the import list, reports, renders) renames the temporary file without flushing it. After a power cut, NTFS can keep the rename but lose the data.

## 2. Decisions: key bindings and preferences

### 2.1 Pure model: `FStageKeyBindings` (`Public/Input/StageKeyBindings.h`)

- **Commands:** `EStageInputCommand` lists every bindable action. Its enumerator names are written to the ini, so they are never renamed.

  | Category | Commands (default keys) |
  |---|---|
  | Editing | Undo (Ctrl+Z), Redo (Ctrl+Y, Ctrl+Shift+Z), Delete Selection (Delete), Select All (Ctrl+A), Cancel / Deselect (Esc) |
  | Tools | Cycle Transform Tool (Space), Move Tool (W), Rotate Tool (E), Scale Tool (R), Place Mode (P), Finish Path (Enter), Remove Last Path Point (Backspace) |
  | View | Perspective (Alt+G), Top (Alt+J), Front (Alt+H), Back (Alt+Shift+H), Left (Alt+K), Right (Alt+Shift+K), Toggle Grid (G) |
  | Navigation (while the right mouse button is held) | Fly Forward (W), Back (S), Left (A), Right (D), Up (E), Down (Q) |

  The new defaults follow the Unreal Editor: W/E/R choose a tool while the right mouse button is up, and fly while it is held.
- **Bindings:** each command has a primary and a secondary chord. A chord is `FStageKeyChord` (key, Ctrl, Shift, Alt).
- **Rules,** enforced by the only mutators, `Assign`, `Clear`, `Reset` and `ResetAll`:
  - **Bindable keys:** keyboard keys only. Mouse buttons, axes, gamepad, touch, gestures, modifier keys on their own, `AnyKey` and the console key (Tilde) are refused.
  - **Navigation keys have no modifiers.** They are held while flying, often together with Shift or Ctrl by accident.
  - **No two editing/tool/view commands share a chord.** No two navigation commands share a key.
  - **A navigation key may share a chord with a command.** They are never live at the same time, because commands are suspended while the right mouse button is held. This is what makes W both "Move tool" and "fly forward", as in the Unreal Editor.
  - A refused assignment leaves the map unchanged and names the command that holds the chord, so the UI can say "Ctrl+Z is already Undo".
- **Persistence:** `ToOverrides` / `FromOverrides`.
  - Only commands that differ from the defaults are saved, so a later version's new defaults reach existing users.
  - `FromOverrides` is the trust boundary for the ini. Unknown command names, duplicates and unbindable keys are dropped. Every overridden command is cleared first and then assigned, so a swap (Undo ↔ Redo) loads in any order. An override that still conflicts is dropped and counted.

### 2.2 Owner: `UStageInputPreferencesSubsystem` (`ULocalPlayerSubsystem`)

- **Where it lives:** Rules.md puts per-user, non-gameplay preferences in a `ULocalPlayerSubsystem`; this is the first one.
- **What it owns:** the key bindings and two navigation preferences: look sensitivity (×0.1 to ×5) and invert vertical look.
- **Single writer:**
  - It is the only caller of the new `UStageCraftUserSettings` input setters.
  - Every change is validated by the model, stored, saved to GameUserSettings.ini and announced on `OnInputPreferencesChanged`.
  - A refused change stores nothing.
- **Machine-wide values:** GameUserSettings is per machine, so all local players share one key map. StageCraft has one local player.

### 2.3 Dispatch: one lookup on key press instead of one Enhanced Input action per shortcut

- `AModularPlayerController::InputKey` passes every key to Enhanced Input first, unchanged. On a key press (not a repeat) that is bindable, and while the camera is not being flown, it builds the chord and runs the matching command.
  - The modifiers come from the viewport's key state (`FViewport::KeyState`), not from the OS. The viewport records a key the moment Slate delivers it and forgets every key when it loses focus, and the simulated chords of the scripted runs (`StageCraft.Edit.Key`) go through it like real ones.
  - The player input's own state (`IsInputKeyDown`) is used only when an event has no viewport. It updates on the next input tick, so Ctrl pressed in the same frame as Z would read as up. The first scripted run showed this: every chord failed during a slow import frame.
- **Why not Enhanced Input per shortcut?**
  - Exact chords would depend on mapping order and injected chord blockers. A user map can order them any way, and a plain "Z" binding still fires during Ctrl+Z unless a blocker is injected for that exact pair.
  - A rebind would rebuild and re-add a mapping context while keys may be held.
  - With a lookup, matching is exact (Ctrl+Z never runs a plain-Z command) and a rebind is just a value change.
- **What stays in Enhanced Input:**
  - The left mouse button (select/place) and the held right mouse button (navigate). They are fixed, because the viewport client's cursor capture and the gizmo depend on them. The panel lists them as not remappable.
  - Look, fly and wheel. The fly context is rebuilt from the navigation bindings only when they change (`bIgnoreAllPressedKeysUntilRelease`).
- **Removed:** the per-shortcut `UInputAction` properties and the Ctrl/Shift chord machinery. No asset referenced them; the project has no IMC or IA assets.
- **Changed while flying:** keyboard commands no longer run while the right mouse button is held. Before, Delete and Ctrl+Z did. This matches the Unreal Editor.
- **Navigation preferences** are applied where look input is forwarded to the pawn. Sensitivity scales the look delta and invert flips pitch. Orthographic panning is unaffected.

### 2.4 Text that names keys follows the bindings

The Edit menu labels and the status bar hints read the current bindings: "Undo (Ctrl+Z)", "Esc or P: finish", "Right mouse + WASD: fly". An unbound command is left out of a hint. The status bar refreshes on `OnInputPreferencesChanged`.

### 2.5 Preferences panel (`UStagePreferencesPanel`, `StageCraft.Panel.Preferences`)

- **Where it opens:** a dock panel like the others, created headlessly as `DA_Panel_Preferences`. It opens floating from Edit › Preferences… or the Window menu. It is not in the default layout.
- **Sections:**
  - **Navigation:** look sensitivity, invert look.
  - **Viewport:** snap to items, grid.
  - **Audio:** master, effects, mute.
  - **Keyboard shortcuts:** per category, a row per command with Primary and Secondary buttons and Reset; Reset All; and the fixed mouse controls.
- **Writes** go to each value's owner (the input preferences subsystem, `UStageSnappingComponent`, `UStageViewportComponent`, `UStageAudioSubsystem`). The panel never writes `UStageCraftUserSettings`, and it re-reads every value from its owner after an event.
- **Capturing a shortcut:**
  - Clicking a binding button puts the panel in capture mode and gives it keyboard focus, so the key never reaches the viewport.
  - The next non-modifier key, with the modifiers held, is the new chord.
  - CLEAR unbinds the slot. Clicking elsewhere (focus lost) cancels.
  - A refusal shows why ("Ctrl+Z is already used by Undo", "Navigation keys cannot use Ctrl, Shift or Alt").
- **Not a replacement:** the panel adds a place for the existing audio and snapping preferences; the menus and toolbar keep theirs.

### 2.6 Alternatives considered

- **`UEnhancedInputUserSettings` (player-mappable keys).**
  - It needs mapping-context assets with per-mapping `PlayerMappableKeySettings`, and its own save slot next to GameUserSettings.ini.
  - Exact chords would still rely on chord actions and blockers.
  - It suits gamepad and game remapping better than an editor's command table, and it would split StageCraft's preferences across two stores.
- **Keeping one Enhanced Input action per command and rebuilding the editor context on change.** Rejected for the ordering and exactness problems in §2.3.
- **A Slate modal preferences window.** Rejected for consistency: every other StageCraft tool is a dockable panel, and a panel can stay open next to the viewport while trying new keys.

## 3. Decisions: audit findings

### 3.1 R2: a partial load keeps what it could not place

- **Unplaced items are kept.**
  - Items skipped because their catalog entry is unknown, or because the rules refuse them now, are kept as snapshots by `UStageSceneComponent`. `CaptureStage` appends them, so a re-save writes them back unchanged.
  - Placed items take precedence: a carried item whose instance id is on the stage is not written twice.
  - Loading another scene, or recovering an autosave, replaces the carried list.
- **The stage stays unsaved after a partial load.**
  - It stays dirty if anything was skipped or repaired, or if damaged entries were dropped.
  - Autosave and the exit prompt then protect it. The message gives the counts.
- **Damaged entries are backed up, not carried.**
  - They cannot be parsed, so they cannot be carried as snapshots.
  - The first save over that scene's own file first copies the original to `<Name>.json.bak`, and the message says so.
- **Imports first.**
  - `LoadScene` and `RecoverAutosave` refuse with the new `ImportsPending` result while `UStageMeshImportSubsystem::GetPendingCount() > 0`: "Imported models are still loading. Try again in a moment."
  - Without this, a load during start-up would skip the imported models' items.

### 3.2 R6: restored state is sanitized where it is applied

- **No package loads from saved state.** `RestoreSnapshotState` reads with `bLoadIfFindFails = false`, so a saved object reference can only resolve to an object that is already loaded. No StageCraft SaveGame property is an object reference today, so this is a guard, not a behaviour change.
- **Every class repairs its own values,** in the same place it already reconciles state with its item data (`ApplyItemData`), so a restored item and a re-applied item get the same rules:
  - **Base item:** the label is capped at 256 characters, like scene file labels.
  - **Fixture:**
    - A non-finite attribute takes the fixture's default.
    - Out-of-range values are clamped, and stepped attributes are rounded, exactly as `StoreAttribute` does.
    - The patch is clamped as `SetPatch` does, and a negative fixture ID becomes 0 (auto-assign).
  - **Audio:** gain −60…12 dB, delay 0…1000 ms and splay 0…the cabinet's maximum. NaN takes the default.
  - **Span:** a damaged path already falls back to the shortest run; that now counts as a repair.
- **Each repair is logged** with the item and the value (`ReportStateRepair`) and counted on the actor (`GetStateRepairCount`). The scene load adds the counts to its message and keeps the stage dirty, because the stage no longer matches the file.

### 3.3 R8: the import list is never overwritten by a bad read, and writes are durable

- **A list that cannot be parsed** is moved to `Imports.json.bak` before anything else, and a warning is logged. The session then starts with an empty list, and new imports are saved normally.
- **A list that exists but cannot be read** (locked, permissions) is never overwritten during that session. New imports work, but they are not remembered, and a warning says so.
- **Durable writes.** New helper `StageAtomicFile` (`Private/IO`): it writes the temporary file, flushes it to disk (`IFileHandle::Flush(true)`), then renames it over the target.
  - The scene store, the layout store, the session, the import list, the CSV/PDF reports and the render PNGs all use it. Three near-identical copies are removed.
  - After a crash or power cut the target holds either the old content or the new content, never a torn file.
  - The flush costs one disk sync per save, and saves are user actions or 120 s autosaves.

## 4. Consequences

- **Remappable keys:** every keyboard shortcut can be remapped, and the menus and hints show the user's keys. The mouse buttons stay fixed.
- **Removed controller properties:** the `UInputAction` properties for keyboard shortcuts are gone. A Blueprint subclass that assigned them would lose them; none exists.
- **Partial loads:** a partial load no longer loses items. It leaves the stage unsaved and says why.
- **Scenes that depend on imports** now load only after the imports have finished.
- **Damaged saved values** are repaired with a log line per value, instead of reaching the lights and the mixer.
- **A damaged import list** is set aside instead of being overwritten.
- **Rollback:** the input change is self-contained, in the controller dispatch, the subsystem and the settings fields. The audit fixes are independent of it and of each other.
