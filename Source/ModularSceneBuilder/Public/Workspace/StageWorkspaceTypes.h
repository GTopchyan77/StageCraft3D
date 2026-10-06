// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "NativeGameplayTags.h"
#include "StageWorkspaceTypes.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogStageWorkspace, Log, All);

/** Lifecycle of the dockable workspace (Docs/ADR/0001-dockable-workspace.md §3.2). */
UENUM(BlueprintType)
enum class EStageWorkspaceState : uint8
{
	/** No local controller has registered yet, so support has not been decided. */
	Uninitialized,
	/** This run cannot host the workspace (not UStageCraftGameEngine, or the viewport renders directly to the window). The plain HUD is used. */
	Unsupported,
	/** The dock root is installed in the main window. */
	Ready,
	ShuttingDown
};

/** Where a panel currently lives. Derived from Slate; Slate's tab manager is the source of truth. */
UENUM(BlueprintType)
enum class EStagePanelHost : uint8
{
	Closed,
	/** Docked in the main application window. */
	MainWindow,
	/** Torn off into its own OS window (possibly on another monitor). */
	FloatingWindow
};

/** Outcome of a layout operation (apply, save, delete). Every failure is logged by the workspace with the layout name. */
UENUM(BlueprintType)
enum class EStageLayoutResult : uint8
{
	Success,
	/** The workspace is not installed (editor, PIE, -StageDirectViewport, or not started yet). */
	WorkspaceInactive,
	/** Empty, too long, or contains characters that are not allowed in a file name. */
	InvalidName,
	/** The name belongs to a built-in layout, which cannot be overwritten or deleted. */
	ReservedName,
	NotFound,
	/** The file could not be read or parsed. It was renamed to .bak and kept. */
	Corrupt,
	/** The file was written by an incompatible version. It was renamed to .bak and kept. */
	VersionMismatch,
	WriteFailed
};

/** Native tags so C++ identifies panels without string literals. A panel's tag name is also its Slate tab ID. */
namespace StageCraftTags
{
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Panel);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Panel_Viewport);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Panel_Inspector);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Panel_FaderBank);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Panel_Library);
}
