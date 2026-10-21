# ADR 0010: Edit rules on undo/redo, one-step drags, autosave and exit prompt, scene format v2

- **Status:** accepted, implemented. See STATE.md #37.
- **Related:**
  - ADR 0003 (undo/redo), ADR 0004 §3.2 (group transform) and §4 (scene files), ADR 0009 (outdoor environment).
  - `Docs/PROJECT_AUDIT.md`: R2, R3, R4, R5 and R7.
  - Rules.md: "Roles of the framework classes" and "Safe data binding"; Engineering Standard §7, §17, §26, §55 and §60.

## 1. Problem

The project audit (STATE.md #36) and Gevor's Phase 2 request (2026-10-20) asked for five fixes:

1. **Undo/redo bypassed the rules (audit R3).** A transform step was written straight to the actor, so Ctrl+Z could move a locked item. Parameter undo already went through the GameMode; transform undo did not.
2. **Group edits needed per-item checks.** Every item of a group must pass the lock check and a sanity check, on undo and redo too.
3. **One drag made many undo steps (audit R4).** An inspector spin-box drag commits on every change, and each commit recorded its own step. A single drag could flush the 100-step history.
4. **Unsaved work could be lost on close (audit R5).** Closing the main window never asked, and there was no autosave.
5. **The environment was not saved (audit R7, ADR 0009 §7).** The environment needed to go into scene files, with a migration from the version 1 format.

## 2. Decisions: edit rules

### 2.1 Undo/redo asks the rules authority, item by item

- **A new GameMode check.** `AStageCraftGameModeBase::EvaluateTransformChange(Player, Item, NewTransform)` judges a whole-transform write, as undo/redo makes one.
  - It first requires a sane transform (`StageTransformRules::IsSaneTransform`).
  - Then it calls `EvaluateParameterChange` for each changed part (Location, Rotation, Scale). Lock, a span's refusal of Scale, ownership and Blueprint overrides therefore apply exactly as they do for an inspector edit.
  - Unchanged parts are not judged.
  - It is not a BlueprintNativeEvent. It composes one, so there is a single extension point.
- **The history component asks first.** `UStageEditHistoryComponent::TransformValidator` is a delegate, like `ParameterApplier`.
  - The controller binds it to `ValidateHistoryTransform`, which calls `EvaluateTransformChange` and reports a refusal.
  - When it is unbound (tests, tools), only the sanity check applies.
- **What `FLiveItemEditor::SetItemTransform` does:**
  1. Clamps scale.
  2. Returns **Invalid** for an insane transform, so the step is dropped and never offered again.
  3. Returns **Refused** when the validator says no, so the step stays and the user can retry after unlocking.
  4. Only then writes the transform.
- **Groups are all-or-nothing.** A group step is an `FStageBatchCommand`, which applies each item in turn and rolls back on the first failure. So one locked item refuses the whole group step and the items already moved are put back. The `StageCraft.History.Rules` test checks this, with the lock on an item undone after another one.

### 2.2 Live group edits check every item

- **At the start of an edit.** A gizmo drag start and a numeric group edit already call `EvaluateSelectionTransform`, which asks the GameMode about every selected item (lock, scale support) before anything moves.
- **New per-follower check.** `UStageGroupTransformComponent::FollowLeader` now also sanity-checks each follower's computed transform. A follower that orbiting a far pivot would carry out of range stays where it last was valid. Its start transform is still recorded, so undo restores the whole group.

### 2.3 Transactions: one continuous edit is one step

- **The API.** `UStageEditHistoryComponent::BeginTransaction` / `EndTransaction`.
- **While a transaction is open:**
  - `RecordTransformChange(s)` keeps the first "before" per item.
  - `RecordParameterChange` keeps the first "before" per item and parameter.
- **When it ends:**
  - Each item's first "before" is compared with its current state.
  - Items that ended where they started are left out.
  - Everything is recorded with the existing `RecordAsOneStep`: one command, or one batch.
- **Every edit is still applied live.** Each value change still goes through `RequestParameterChange`, with the GameMode check, the session and the read-back. Only the recording is deferred.

Who opens and closes it:

| Event | Effect |
|---|---|
| `USpinBox::OnBeginSliderMovement` on an inspector row | `UStageParameterControlWidget::BeginInteraction`. The view then calls `AModularPlayerController::BeginEditTransaction`. |
| End of the drag (`OnEndSliderMovement` / commit) | `FinishInteraction`, then `EndEditTransaction`. Fired at most once per interaction. |
| The row goes away mid-drag (re-inspect, rebuild, panel closed) | `UStageParameterViewWidget::UnregisterControls` ends the transaction the view opened (`bOwnsEditTransaction`). |
| Undo or redo while a transaction is open | The transaction ends first, so undo reverts the drag rather than an older step. |
| `EndPlay` | Ends any open transaction. |

Typing a value and pressing Enter was already one commit, and so one step. Mouse-wheel steps on a spin box stay one step each, because they are discrete edits.

Fixture attributes (faders, encoders) are still not recorded. That is a separate open item; see STATE.md #31.

### 2.4 Shared sanity rule

`StageTransformRules::IsSaneTransform` requires all three:
- every component is finite;
- `|location| <= MaxCoordinate` (10 km);
- the rotation is not degenerate.

`FStageSceneStore::MaxCoordinate` now refers to the same constant, so files and edits share one limit.

## 3. Decisions: autosave and the exit prompt

### 3.1 Autosave (`UStageSceneComponent`)

- **When it runs.** A looping timer (`AutosaveIntervalSeconds`, default 120 s, minimum 10 s, 0 = off) runs **only while the session is dirty**. `OnDirtyChanged` starts and stops it, so a clean stage costs nothing.
- **What it writes.** It writes only when the edit serial changed since the last autosave, and never while a user save or load is running. The file goes to `Saved/StageCraft/Scenes/Autosave/<scene name>.json`, or `(Untitled).json` for a stage that was never saved.
  - It uses a second `FStageSceneStore` (the same format, atomic write, background task).
  - It never touches the user's scene file and never marks the stage clean.
- **Cleanup:**
  - A successful Save deletes the stage's autosave, and the one under its previous name. An untitled stage saved as "Show" removes `(Untitled)`.
  - An autosave that finishes after the stage was saved or discarded deletes itself.
  - "Don't Save" on exit deletes it.
- **Recovery.** After a crash the file stays. File › Recover Autosave (`RequestRecoverAutosave`) opens it through the normal load path, with the same placement rules and catalog checks. The stage keeps its scene name and **stays marked unsaved**, because the scene file still lacks those changes.

### 3.2 Exit prompt ("Save changes before closing?")

- **The shell asks first.** `FStageWorkspaceShell` asks `Callbacks.ConfirmMainWindowClose` before closing the main window (close button, Alt+F4). `UStageWorkspaceSubsystem::ConfirmMainWindowClose` returns:
  - true (close now) when there is no stage or no unsaved change;
  - otherwise false, and it shows the prompt.
- **The dialog.** `SStageUnsavedChangesDialog` offers **Save / Don't Save / Cancel**.
  - It is shown by `FStageWorkspaceShell::ShowDialog` on a dimmed layer over the whole workspace, which takes every click. Keyboard focus moves to the dialog, so Delete or Ctrl+Z cannot edit the stage behind it.
  - A stage that was never saved gets a name field. As in Save Scene As, the first Save on an existing name only warns.
- **Save waits for the file.** The subsystem binds `OnSceneOperationFinished`. On success it calls `FStageWorkspaceShell::CloseMainWindow`. A failed save leaves the prompt open with the reason.
- **Why not a modal window.** `FSlateApplication::AddModalWindow` runs a nested Slate loop. The background save task's game-thread continuation would not run inside it, so Save could never finish.
- **Not covered:**
  - PIE, where the workspace does not exist and the editor owns closing.
  - `exit` / `RequestExit`, which is programmatic. It is also what leaves an autosave behind to recover.
- **For scripted verification:** `StageCraft.Workspace.ExitPrompt Save [Name] | Discard | Cancel`, `StageCraft.Scene.AutosaveNow` and `StageCraft.Scene.Recover <Name>`. All development-only.

## 4. Decisions: scene format version 2

- **`FStageSceneStore::FormatVersion = 2` and `MinReadableVersion = 1`.** Saves always write version 2. Parse reads versions 1 and 2 and refuses anything else as `VersionMismatch`: a newer file could hold data this build would silently drop on the next save.
- **What version 2 adds.** An `environment` object: `skyEnabled`, `timeOfDay`, `sunElevation`, `sunAzimuth`, `cloudCoverage`, `rain`, `snow`, `fog`.
  - A missing object or field keeps the default.
  - The result goes through `StageEnvironment::Sanitize`, so a file can never put the sky out of range.
- **Migration from version 1.** The default environment: sky off, the indoor stage. A version 1 file never had an environment, so a stray one in it is ignored. Re-saving a version 1 scene writes version 2.
- **Capture and apply:**
  - `UStageSceneComponent::CaptureStage` reads `UStageEnvironmentSubsystem::GetSettings`.
  - `ApplyDocument` calls `SetSettings` **before** `MarkClean`.
- **Environment edits mark the scene unsaved.** `UStageSessionSubsystem` binds `OnEnvironmentChanged` (`InitializeDependency<UStageEnvironmentSubsystem>`) and marks the session dirty after play has begun, by the same rule as items.

## 5. Alternatives considered

| Alternative | Why not |
|---|---|
| Re-validate inside each `IStageEditCommand` | The commands are pure and world-free (tested with fakes). The rules belong to the GameMode, reached through the editor interface's validator. |
| Coalesce consecutive steps in `FStageCommandHistory` by time or item | Implicit merging would also join two separate quick edits. An explicit transaction tied to the drag's start and end is exact. |
| Native `FMessageDialog` Yes/No/Cancel for the exit prompt | It blocks the game thread (the save could not complete), and its buttons cannot say Save / Don't Save. |
| Autosave into the Scenes folder as "<Name> (Autosave)" | It would mix with user files in File › Open, and a Save there would overwrite the autosave name. |
| Environment in the item list as a pseudo-item | It is stage state with one instance, not an item. A top-level object keeps the item contract unchanged. |
| Keep accepting only version 1 and add an optional field | Older builds would silently drop the environment on save. A version bump makes them refuse instead. |

## 6. Consequences

- **Older builds cannot open new scenes.** Scene files saved by this build are version 2, which older builds refuse (`VersionMismatch`) instead of silently dropping the environment.
- **Environment edits now count as unsaved changes.** They show in the File menu heading and trigger the exit prompt.
- **Disk use.** One autosave file per scene name with unsaved changes, deleted when the stage is saved or discarded.
- **Two new world tests:** `StageCraft.History.Rules` and `StageCraft.History.Transaction` run against a real game world.
  - The GameMode's protected `SessionRules` is set through reflection in the test only, because the test world has no game instance and so no economy.
- **Shipping unchanged.** The new development console commands are compiled out of Shipping.

## 7. Packaging and cost of the sky (Phase 3 verification)

- **Builds.** UE 5.8, all clean:
  - Editor Development
  - Game Development
  - Game Shipping (`-DisableAdaptiveUnity`)
  - packaged Development
  - packaged Shipping (`BuildCookRun -cook -stage -pak -archive`)

  None of them shows a warning or error in the build or cook log.
- **Sky Creator in the package.** Every Sky Creator content folder the cook rules name (ADR 0009 E5) is in the IoStore container: 259 assets.
- **One missing dependency, fixed.** The packaged game logged "Failed to find `/Engine/EngineSky/VolumetricClouds/m_SimpleVolumetricCloud_Inst`" when the sky turned on.
  - `UVolumetricCloudComponent`'s constructor points its material at that engine asset (`VolumetricCloudComponent.cpp:72`), and the component loads it when Sky Creator creates it. Sky Creator then replaces it, so nothing rendered wrong, but the package lacked it.
  - Fix: `+DirectoriesToAlwaysCook=(Path="/Engine/EngineSky/VolumetricClouds")` in `DefaultGame.ini`. That is 19 more packages; the next packaged run was warning-free.
- **A dead reference in Sky Creator.** `MF_SC_BlueNoiseDithering` references `/Engine/EngineMaterials/STBlueNoise_scalar_128x128x64`, which UE 5.8 no longer ships.
  - Its only user is `MF_SC_Triplanar_SingleSample`, and no asset uses that function, so nothing rendered depends on it.
  - The marketplace content is left unchanged (ADR 0009 §3.1).
- **Measured cost** (packaged Development):
  - **Setup.** Rendering was at 1184×657 inside a 1600×900 window. CSV profiler with GPU stats, 300 frames per state, two runs.
  - **Caveat.** A second Unreal editor (Kmidden) was open on the same GPU, so absolute times are only indicative. The differences between states are consistent across both runs.

| State | GPU time (ms) | Frame time (ms) | Local video memory in use (MB) |
|---|---|---|---|
| Sky off (indoor stage) | 4.52 / 4.39 | 8.24 / 5.36 | 1896 / 1907 |
| Sky on, clear | 6.85 / 6.62 | 10.45 / 7.60 | 2087 / 2086 |
| Sky on, Rain preset | 9.61 / 8.99 | 12.68 / 10.07 | 2182 / 2191 |

Where the extra GPU time goes:
- Sky on: about 2.3 ms more, mostly volumetric clouds (0.25 ms) plus extra volumetric fog, lights and translucency.
- Rain: a further 2.4–2.8 ms (volumetric fog 0.8–0.9 ms, clouds 0.5 ms, translucent rain 0.4–0.45 ms).
- Video memory: +180 MB with the sky on, +285 MB with rain.

The sky is a per-stage opt-in and is free while off (§3 of ADR 0009), so these costs only apply to outdoor stages.

## 8. Open items

- **Mouse checks (Gevor).** One undo step per spin-box drag in the inspector; the exit prompt's buttons and name field.
- **Undo for fixture attributes, patch, visibility, lock and label.** Unchanged from ADR 0003 §6.
- **Saving cues with scenes.** Needs scene format version 3, and the cue UI first (audit R10).
