# ADR 0006: Inspector polish and runtime model import

- **Status:** accepted, implemented. See STATE.md #28.
- **Related:** ADR 0005 (outliner, spans), ADR 0004 (scene files), ADR 0002 (Library, placement), ADR 0001 (workspace shell). Rules.md "Where state lives", "Roles of the framework classes", Engineering Standard §9, §21, §28, §29, §55, §65.

## 1. Problem

Gevor asked for:
1. **Inspector polish.** The value editors were white, unaligned and unstyled; section headers should be amber, GrandMA3 / Unreal Editor style.
2. **OS drag and drop import** of `.obj`, `.fbx`, `.gltf`, `.glb` (and `.datasmith`) onto the viewport and the Library: parse, import, build a runtime static mesh, register it in the Library.
3. **An Import button** in the Library using the OS file dialog (`IDesktopPlatform::OpenFileDialog`).
4. **Imported models selectable for Place mode at once**, without a restart.

## 2. Verified engine facts (UE 5.8 source and runtime)

| # | Fact | Source | Consequence |
|---|---|---|---|
| E1 | Interchange imports static meshes at runtime: factories check `IsRuntimeOrPIE` and build render data from mesh descriptions (`BuildFromMeshDescriptions`). | `InterchangeStaticMeshFactory.cpp` ~936 | No editor needed; works in packaged builds. |
| E2 | The FBX translator registers and reports formats only `WITH_EDITOR`. | `InterchangeImportModule.cpp` 107, `InterchangeFbxTranslator.cpp` 260 | FBX imports in the editor and in Standalone Game from editor binaries, **not in packaged builds**. Supported formats are asked from Interchange, so FBX disappears from the dialog and the drop rule by itself. |
| E3 | Interchange translators register on `OnPostEngineInit`, after GameInstance subsystems initialize. | `InterchangeImportModule.cpp` 176 | Formats are asked when needed, never cached at `Initialize`. |
| E4 | `DesktopPlatform` is under `Engine/Source/Developer`; UBT refuses it in Shipping as non-redistributable. | `ModuleRules.cs` ~1882 | The file dialog exists in builds with developer tools only. Drag and drop works everywhere. |
| E5 | In a game run from editor binaries the Interchange editor utilities are not loaded and the default says "editor"; the generic pipeline elsewhere checks `IsRunningGame()`. The mix creates `UMaterialInstanceConstant` and trips `check(GIsEditor)` (OBJ materials). | `MaterialInstanceConstant.cpp` 175, `InterchangeGenericMaterialPipeline.cpp` 1049 vs 3780 | `UStageInterchangeGameUtilities` (answers "runtime") is installed only when `WITH_EDITOR && IsRunningGame() && !GIsEditor`. Standalone tests then use the same path as a packaged game. |
| E6 | `/Memory/` packages have no file root; the editor-side material code asks for one and crashes (`LongPackageNameToFilename`). `/Temp/` is mounted in every build (to Saved, read-only). | `PackageName.cpp` 904, runtime crash | Imported assets live under `/Temp/StageCraft/Imported/`. |
| E7 | The OBJ translator applies no unit or axis conversion (1 unit = 1 cm, Z-up). glTF and FBX carry units and axes and are converted. | `InterchangeOBJTranslator.cpp` | OBJ is assumed Y-up metres (Blender, Maya, C4D) and corrected with the pipeline's import offset. |
| E8 | OS file drops reach Slate as `FExternalDragOperation`; `SViewport` does not handle drops, so they bubble to parents. UMG's `NativeOnDrop` ignores external operations. | `DragAndDrop.h` 222, `SlateApplication.cpp` 7428 | A Slate drop target wraps each dock tab's content. |
| E9 | `FPreviewScene` defaults to an *editor* preview world, which a game does not render; `SetEditor(false)` makes a game preview world. | `PreviewScene.cpp` 39, runtime | Thumbnails render in a game preview world. |
| E10 | `UComboBoxString` takes its font and foreground only before the Slate widget is built (`InitFont` is protected); its default is Roboto Bold 16 on a light button. `UImage` with a rounded-box brush has no size of its own. | `ComboBoxString.h` 238, runtime | A tiny `UStageThemedComboBox` subclass; brushes get explicit `ImageSize`. |

## 3. Inspector

- **Code-created editors are themed** (`UStageParameterRowWidget::SetTheme`, set by the inspector from its `Theme`): numeric fields (`StageCraftWidgetStyle::MakeNumericField`: recessed, accent outline while active, faint fill showing a ranged value), checkboxes (outlined box, amber check), dropdowns (`MakeComboBox`, `MakeListRow`), text fields (`MakeInputField`), all in a 22 px box (`FieldHeight`). Vector, rotator and colour components carry an X/Y/Z (R/G/B) tag in the theme's new `AxisX/Y/ZColor`, the gizmo's colours. Widgets a Blueprint binds keep their designer style.
- **Layout fixes in the Widget Blueprints** (through the editor bridge): units get a fixed 18 px column so fields line up across rows, rows get right padding, section headers are amber (`#FFB300`).
- **Read-only rows:** the empty editor slot is collapsed so the value sits where the editors start, and the units column is left empty because the formatted value already carries them.

## 4. Runtime model import

### 4.1 Ownership
| Piece | Lifetime / home | Role |
|---|---|---|
| `UStageMeshImportSubsystem` | GameInstance | Validates requests, one Interchange import at a time (queue), builds items, registers them, remembers files, renders thumbnails. |
| `UStageImportedItemData` | in memory, `/Temp/StageCraft/Imported/IMP_<hash>` | Catalog entry: `AModularBaseActor` + the imported mesh, category `StageCraft.Category.Imported`, free, no specs. |
| `UStageItemSubsystem::AddRuntimeItem` | friend of the importer only | Adds to the catalog, kept across catalog (re)loads, broadcasts `OnCatalogChanged`. |
| `AModularPlayerController::RequestImportModelFiles` / `RequestBrowseModelFiles` | request bridge | GameMode `EvaluateModelImport` (session rule `bAllowModelImport`), refusals on `OnRequestRejected`. |
| `SStageFileDropTarget` | wraps every dock tab's content (`FStageWorkspaceShell`) | Hint overlay while files hover ("Drop to import…" in amber, or a refusal in red); drop → controller. |

### 4.2 Import
- Interchange `ImportAssetAsync` with a code-built `UInterchangeGenericAssetsPipeline`: every mesh forced static and combined into one, no skeletal meshes, animations or geometry caches, no Nanite (cannot be built in a packaged game), materials as instances of Interchange's parents. OBJ gets the Y-up metres correction (E7). The result is collected on the game thread (`OnDone` → `AsyncTask(GameThread)`), matched by serial so a late result after shutdown is dropped.
- **Validation before anything starts** (untrusted input): supported extension, file exists, ≤ 512 MB, not already imported or queued. One status message per drop names the first reason and how many others were refused.
- **Identity:** the item name is `IMP_` + 16 hex digits of the SHA-1 of the lower-cased normalized path, so the same file always yields the same object path. Scene files may name these paths (`StageMeshImport::IsImportedItemPath`); catalog membership is still required to spawn.
- **Persistence:** successful imports are listed in `Saved/StageCraft/Imports.json` (versioned, validated on read, at most 256 entries) and imported again at startup, so scenes that use them load. Writes run in order on a `UE::Tasks::FPipe` with the atomic layout writer; the list is not written before last run's list was read.
- **Thumbnails:** each model is captured once, drawn with a uniform clay material (`M_ThumbnailClay`, set in `DefaultGame.ini`), in a private game preview world (no Lumen, no temporal effects), into a 128 px render target the Library tile draws directly. Clay rather than the model's own materials: those may still be compiling right after import and vary too much in brightness to read on a small dark tile.

### 4.3 Rejected / deferred
- **Datasmith (`.udatasmith`).** The only runtime path is the beta *DatasmithRuntime* plugin, which loads whole scenes as actors (not one Library item), needs DatasmithImporter + UdpMessaging, and DirectLink. Not a fit for "one file → one placeable item"; deferred until a use case needs scene import.
- **A Windows-native dialog in Shipping.** Possible (`IFileOpenDialog`), but a second implementation that only Shipping would exercise; drag and drop already covers Shipping. Follow-up if packaged users need the button.
- **Placing on drop.** A drop imports only; placing stays an explicit Library click (no surprise geometry where the cursor happened to be).

## 5. Consequences and limitations
- Packaged builds import glTF/GLB and OBJ, not FBX (E2). The Interchange parent materials (`/InterchangeAssets/Materials`, `/InterchangeAssets/gltf`) are added to `DirectoriesToAlwaysCook`; a packaged import has not been run yet.
- **This machine's engine install:** `/InterchangeAssets/Materials/PBRSurfaceMaterial_MR` references `/Game/KMidden/Items/Props/Textura_full_pg` and fails to compile, so OBJ materials fall back to the default material here (glTF materials were unaffected in testing). The engine plugin content was modified, apparently from the Kmidden project; verify the engine install.
- Imported items have no weight or power (shown as 0 in the bill of materials) and cannot be removed from the Library yet (delete `Saved/StageCraft/Imports.json`).
- A model whose file moved is reported at startup and kept in the list; scenes using it load without those items (counted as unknown).
- OBJ files written Z-up or in other units come in rotated or scaled; there is no per-import option yet.
- The OBJ translator raises an engine ensure for OBJ files without texture coordinates (the import still succeeds).

## 6. Rollback
The import system is separable: remove the `Import/` folder, `UStageImportedItemData`, the importer friend and `AddRuntimeItem` in `UStageItemSubsystem`, the drop target wrapping in `FStageWorkspaceShell`, the controller requests, the Library button and the `Interchange` plugin entry. The inspector theming is local to `UStageParameterRowWidget` and `StageCraftWidgetStyle`; the Widget Blueprint changes are three property edits.
