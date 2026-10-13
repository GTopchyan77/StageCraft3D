// Copyright DamenTools internal. Damen File Manager plugin.

#pragma once

#include "CoreMinimal.h"
#include "DlFmMain_Logic.h"
#include "DamenFileManagerWidget.generated.h"

class UDlFmButton;
class UFileDrop;
class UPanelWidget;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FDamenFileManagerCloseRequestedSignature);
DECLARE_DYNAMIC_DELEGATE(FDamenFileManagerCloseRequestedDelegate);

/**
 * Damen-specific file manager widget: the vendor DlFileManager browse/copy/
 * rename/delete UI (UDlFmMain_Logic, used completely unmodified) plus native
 * drag-and-drop FBX import for the DamenTool import pipeline. Backs
 * WBP_DamenFileManager (/Game/DamenTool/Blueprints/) - GitHub issue #28.
 *
 * ALL logic lives here, in C++. WBP_DamenFileManager is Designer-only: its
 * widget tree exists purely to satisfy UDlFmMain_Logic's BindWidget members
 * (Container_DirectoryContainer, Container_FileContainer, the ~20 optional
 * Button_, Toggle_ and Dialog_ members) plus this class's own FileDropAnchor, and its
 * EventGraph is deliberately EMPTY. If new behaviour is needed, add it to this
 * class - do not wire nodes into that Blueprint.
 *
 * Inheriting UDlFmMain_Logic (rather than wrapping it) is what keeps the
 * vendor widget tree reusable: WBP_DamenFileManager was duplicated from the
 * vendor demo BP_DlFmMain_D, so every pre-existing BindWidget name/type is
 * already satisfied and only FileDropAnchor is newly required.
 *
 * The drop zone itself is NOT a Designer widget: NativeConstruct builds a
 * UFileDrop (FileDropUMG plugin) in C++ and parents it into the Designer-placed
 * FileDropAnchor panel. Note that SFileDrop::ComputeDesiredSize() returns
 * zero, so the UFileDrop only ever has clickable/droppable area if its slot
 * stretches it - NativeConstruct therefore forces its slot to fill the anchor,
 * and FileDropAnchor must itself be given real screen area by the Designer
 * (in WBP_DamenFileManager it is a VerticalBox with a Fill slot) or nothing
 * can be dropped on it.
 */
UCLASS()
class DAMENFILEMANAGER_API UDamenFileManagerWidget : public UDlFmMain_Logic
{
	GENERATED_BODY()

public:
	UDamenFileManagerWidget(const FObjectInitializer& ObjectInitializer);

	/**
	 * File extensions accepted by the drop zone, WITHOUT a leading dot
	 * ("fbx", not ".fbx") - that is the format UFileDrop compares against
	 * FPaths::GetExtension(), and the comparison is case-insensitive
	 * (FString::operator==), so "FBX" and "fbx" both match one entry.
	 * Defaults to { "fbx" } to match the existing DamenTool FBX import
	 * pipeline (DamenImporterBP / WBP_FBXdrop).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DamenFileManager|Import")
	TArray<FString> ImportFileExtensions;

	/**
	 * Absolute roots of the drives that can be browsed right now ("C:/", "D:/", ...),
	 * via the Windows GetLogicalDrives bitmask. Drives with nothing mounted (empty
	 * card reader, optical drive without a disc) are skipped. Non-Windows: { "/" }.
	 */
	UFUNCTION(BlueprintPure, Category = "DamenFileManager|Browse")
	static TArray<FString> GetAvailableDrives();

	/** Navigates the browser to a drive root (as returned by GetAvailableDrives). */
	UFUNCTION(BlueprintCallable, Category = "DamenFileManager|Browse")
	void BrowseToDrive(const FString& DriveRoot);

	/** Re-lists the drives whenever the panel is shown, so plugged-in drives appear. */
	virtual void SetVisibility(ESlateVisibility InVisibility) override;

	/** Label in front of the drive buttons. Empty = no label. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DamenFileManager|Browse")
	FText DriveBarLabel;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DamenFileManager|Browse")
	FSlateColor DriveButtonTextColor;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DamenFileManager|Browse", meta = (ClampMin = "6"))
	int32 DriveBarFontSize;

	/** Space around each drive button / the label inside DriveBarAnchor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DamenFileManager|Browse")
	FMargin DriveBarItemPadding;

	/**
	 * Fired by RequestClose (the X button), AFTER this widget has collapsed itself.
	 * The host decides what comes back on screen - this widget deliberately knows
	 * nothing about who opened it (WB_Demo_UI binds this to restore itself).
	 */
	UPROPERTY(BlueprintAssignable, Category = "DamenFileManager|Close")
	FDamenFileManagerCloseRequestedSignature OnCloseRequested;

	/**
	 * Shows this widget as a standalone viewport panel: anchored to the viewport
	 * centre, offset by Alignment, Size in Slate units. Adds it to the viewport the
	 * first time, then only updates the slot, and sets it Visible.
	 *
	 * The whole viewport slot is written at once on purpose. The per-field
	 * UUserWidget setters depend on call order: in UE 5.7, SetDesiredSizeInViewport
	 * resets Anchors to (0,0) (UGameViewportSubsystem::SetWidgetSlotDesiredSize).
	 * A Blueprint chain of SetAnchors -> SetDesiredSize therefore put this panel
	 * off-screen at the top-left (STATE.md #209).
	 */
	UFUNCTION(BlueprintCallable, Category = "DamenFileManager|Show")
	void ShowStandalone(int32 ZOrder = 10, FVector2D Size = FVector2D(900.0, 600.0), FVector2D Alignment = FVector2D(0.5, 0.8));

	/** Collapses this widget (it stays in the viewport, cached for reuse) and broadcasts OnCloseRequested. */
	UFUNCTION(BlueprintCallable, Category = "DamenFileManager|Close")
	void RequestClose();

	/**
	 * Adds Event to OnCloseRequested unless it is already bound, so a host may call
	 * this every time it opens the panel without stacking duplicate handlers.
	 * Exists alongside the dispatcher because the ue5-ngg MCP bridge cannot author
	 * "Bind Event to" nodes; a hand-wired Bind Event node in the editor works too.
	 */
	UFUNCTION(BlueprintCallable, Category = "DamenFileManager|Close")
	void BindCloseRequested(FDamenFileManagerCloseRequestedDelegate Event);

protected:
	virtual void NativeConstruct() override;

	/**
	 * Designer-placed panel (a HorizontalBox in WBP_DamenFileManager, above the
	 * path row) that RebuildDriveBar fills with one UDamenDriveButton per drive.
	 * Optional: without it the browser works, just without drive buttons.
	 */
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UPanelWidget> DriveBarAnchor;

	/**
	 * Designer-placed panel that hosts the natively-constructed drop zone. The
	 * paired Widget Blueprint must contain a panel widget named EXACTLY
	 * "FileDropAnchor" (an Overlay, VerticalBox, HorizontalBox or Border -
	 * WBP_DamenFileManager currently uses a VerticalBox, see the
	 * DamenFileManagerWidget.cpp note on the MCP bridge).
	 *
	 * BindWidgetOptional (not BindWidget) on purpose: a WBP subclass that omits
	 * it must still compile and still work as a plain file browser - it just
	 * loses drag-and-drop import, and NativeConstruct logs a loud warning
	 * saying so rather than failing silently.
	 */
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UPanelWidget> FileDropAnchor;

	/**
	 * Designer-placed "X" button at the right end of WBP_DamenFileManager's header
	 * row, bound to RequestClose in NativeConstruct. A vendor UDlFmButton (not a
	 * UButton) so it renders exactly like the toolbar's Button_SelectConfirm & co.
	 * Optional so a WBP without it still compiles; it just has no way to close itself.
	 */
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UDlFmButton> Button_Close;

	/** The drop zone. Constructed natively in NativeConstruct - NOT a BindWidget. */
	UPROPERTY()
	TObjectPtr<UFileDrop> FileDropWidget;

	/**
	 * Bound to FileDropWidget->OnFilesDropped. Re-checks each path's extension
	 * against ImportFileExtensions (defence in depth - UFileDrop already
	 * filters, but its filter is a runtime-settable property on a vendor
	 * widget) and hands each accepted path to ImportFileViaDamenImporter.
	 * Logs per-file accept/reject/success/failure: those log lines are the only
	 * observable signal that an import actually fired, so they are deliberately
	 * verbose.
	 */
	UFUNCTION()
	void HandleFilesDropped(const TArray<FString>& FilePaths);

	/**
	 * Bound to the vendor UDlFmMain_Logic::OnFileListSelectConfirmed - fires on
	 * double-clicking a file row, or on clicking Button_SelectConfirm after
	 * selecting one (both already implemented by the vendor browser; see
	 * DlFmMain_Logic.h/.cpp - this is not new click-handling, just a new
	 * listener). FDlUI_FileExplorer_ItemInfo::Name is already a full absolute
	 * path (confirmed against FIterator_DirItemReader::Visit in DlFmLib.cpp),
	 * so each entry is handed straight to ImportFileViaDamenImporter, the same
	 * import path HandleFilesDropped already uses - this is a second trigger
	 * for that one path, not a separate import mechanism.
	 */
	UFUNCTION()
	void HandleFileListSelectConfirmed(const TArray<FDlUI_FileExplorer_ItemInfo>& FileNodeList);

private:
	/**
	 * Fires the DamenTool FBX import for one absolute file path.
	 *
	 * DamenImporterBP is a Blueprint-only Actor class with NO native base
	 * (parent is /Script/Engine.Actor) - exactly like BP_BuildManager - so
	 * there is no header to include and no type to cast to. Calling into it
	 * therefore requires this project's deliberate reflection bridge
	 * (FindFunctionByName + ProcessEvent), the same pattern already proven by
	 * UPreciseTransformLibrary/UWarehousePlannerSaveLibrary, and not an
	 * oversight. It expects EXACTLY this (confirmed against DamenImporterBP as
	 * of this writing):
	 *   - Blueprint class asset: /Game/DamenTool/Blueprints/DamenImporterBP
	 *     (generated class /Game/DamenTool/Blueprints/DamenImporterBP.DamenImporterBP_C)
	 *   - ONE actor of that class placed in the currently loaded level
	 *     (resolved with UGameplayStatics::GetActorOfClass, the same way
	 *     WBP_FBXdrop's existing Blueprint drop flow resolves it)
	 *   - a callable UFUNCTION named "ImportMesh" on that actor's class,
	 *     exec-only (void return), taking exactly one parameter:
	 *       FileLocation : FString  (absolute path of the FBX file to import)
	 *
	 * The parameter is written BY NAME via UFunction::FindPropertyByName and
	 * type-checked as an FStrProperty before being set, rather than through a
	 * hand-rolled positional params struct - so a rename or retype of
	 * ImportMesh's parameter in the Blueprint is reported as an error instead
	 * of silently corrupting the call.
	 *
	 * Fails LOUDLY at every step (UE_LOG(LogDamenFileManager, Error, ...) then
	 * return false): no world, Blueprint class not loadable, no actor of that
	 * class in the level, ImportMesh not found, FileLocation missing/retyped.
	 * Nothing is defaulted or guessed - a silently skipped import would look
	 * identical to a working one from the UI.
	 *
	 * If DamenImporterBP is ever refactored (renamed/moved asset, renamed
	 * event, changed parameter), update BOTH this comment and the lookups in
	 * the .cpp.
	 *
	 * @param FilePath  Absolute path of the dropped file.
	 * @return true if ImportMesh was actually invoked on a live DamenImporterBP actor.
	 */
	bool ImportFileViaDamenImporter(const FString& FilePath) const;

	/**
	 * Shared extension check used by both HandleFilesDropped and
	 * HandleFileListSelectConfirmed, so the allow-list logic (and its
	 * dot-tolerant comparison) lives in exactly one place.
	 */
	bool IsExtensionAllowed(const FString& FilePath) const;

	/** Clears DriveBarAnchor and fills it with the label + one button per available drive. */
	void RebuildDriveBar();
};
