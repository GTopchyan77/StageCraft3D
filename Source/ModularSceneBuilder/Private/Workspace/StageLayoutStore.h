// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Math/Box2D.h"
#include "Workspace/StageWorkspaceTypes.h"

/** An OS window's restored rectangle in screen pixels, plus whether it is maximized. */
struct FStageWindowPlacement
{
	FVector2D Position = FVector2D::ZeroVector;
	FVector2D Size = FVector2D::ZeroVector;
	bool bMaximized = false;
};

/** One workspace layout as stored on disk: what the user named it, Slate's panel arrangement, and the main window's rectangle. */
struct FStageSavedLayout
{
	/** User-facing name. For a user layout this is also the file name. */
	FString Name;

	/** FTabManager::FLayout::ToString() of the panel tab manager. Floating panel windows carry their own rectangles in here. */
	FString SlateLayout;

	/** Unset for built-in layouts: those leave the main window where it is. */
	TOptional<FStageWindowPlacement> MainWindow;
};

/**
 * Reads, writes, validates and versions workspace layout files (Docs/ADR/0001-dockable-workspace.md §3.4).
 *
 * Pure logic: no world, no UObjects, no Slate. Slate's own layout JSON is only checked for shape and version here.
 * The shell turns it into an FLayout. Every function is synchronous and safe on any thread, because the store holds only immutable paths.
 * The workspace runs writes during play on a background task (WriteFileAtomic on copied strings).
 *
 * Files:
 *   <Directory>/Layouts/<Name>.json   named user layouts, written only by an explicit "Save Layout As"
 *   <Directory>/Session.json          the live arrangement, autosaved and restored on the next launch
 *
 * A file that fails validation is renamed to <file>.bak, never deleted, and the caller falls back to a built-in layout.
 */
class FStageLayoutStore
{
public:
	/** Bump when the wrapper format changes incompatibly. */
	static constexpr int32 FormatVersion = 1;
	static constexpr int32 MaxNameLength = 64;

	/**
	 * @param InDirectory           Root folder for layout files. It is created on first write.
	 * @param InSlateLayoutVersion  The only FLayout name accepted on load. The shell builds every panel layout with this name, so it versions the panel structure.
	 */
	FStageLayoutStore(FString InDirectory, FName InSlateLayoutVersion);

	/** 1 to MaxNameLength characters: letters, digits, space, '-', '_', '(' and ')'. No leading or trailing space, and no reserved device names. */
	static bool IsValidLayoutName(const FString& Name);

	/** Names of the saved user layouts, sorted case-insensitively. */
	TArray<FString> ListUserLayouts() const;

	EStageLayoutResult LoadUserLayout(const FString& Name, FStageSavedLayout& OutLayout) const;

	/** Writes Layout under Layout.Name, replacing a user layout of the same name. Synchronous. */
	EStageLayoutResult SaveUserLayout(const FStageSavedLayout& Layout) const;

	EStageLayoutResult DeleteUserLayout(const FString& Name) const;

	EStageLayoutResult LoadSession(FStageSavedLayout& OutLayout) const;

	/** The session file path and contents, so the caller can write them on a background task. */
	FString GetSessionPath() const;
	FString Serialize(const FStageSavedLayout& Layout) const;

	/** Writes to a temporary file, then moves it over Path, so a crash mid-write never leaves a truncated layout. Any thread. */
	static bool WriteFileAtomic(const FString& Path, const FString& Contents);

	/**
	 * Moves and shrinks the main window and every floating panel window so each lies fully inside one monitor's work area.
	 * - Each window goes to the work area it overlaps most.
	 * - A window that overlaps none (its monitor was unplugged) goes to index 0, which callers pass as the primary monitor.
	 *
	 * The two lists describe the same monitors in the same order, in two unit systems:
	 * - ScreenWorkAreas are physical pixels. MainWindow is captured and applied in those units.
	 * - SlateWorkAreas are each work area divided by its monitor's DPI scale. Slate saves floating window
	 *   rectangles that way (SDockingArea.cpp, SetAreaNodeWindowPlacementValues) and scales them back on restore.
	 * Either list may be empty, which skips that kind of window.
	 */
	static void ClampToWorkAreas(FStageSavedLayout& Layout, TConstArrayView<FBox2D> ScreenWorkAreas, TConstArrayView<FBox2D> SlateWorkAreas);

	/** Exposed for tests. */
	static FStageWindowPlacement ClampPlacement(const FStageWindowPlacement& Placement, TConstArrayView<FBox2D> WorkAreas);

	FString GetUserLayoutPath(const FString& Name) const;

private:
	EStageLayoutResult LoadFile(const FString& Path, FStageSavedLayout& OutLayout) const;
	EStageLayoutResult Parse(const FString& Contents, FStageSavedLayout& OutLayout) const;
	static void MoveAside(const FString& Path);

	FString Directory;
	FName SlateLayoutVersion;
};
