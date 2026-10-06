// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "StageEditCommand.generated.h"

/** Outcome of undoing or redoing one history step (Docs/ADR/0003-undo-redo-and-object-snapping.md). */
UENUM(BlueprintType)
enum class EStageCommandResult : uint8
{
	/** The step was applied; it moved to the other stack. */
	Succeeded,
	/** The rules refused it right now (e.g. a session item limit). The stacks are unchanged and the refusal was reported. */
	Refused,
	/** It can never apply again (its item or catalog asset is gone). The history drops it. */
	Invalid,
	/** Returned by the history only: there is nothing to undo or redo. */
	NothingToDo,
};

/** What changed in an edit history, for views (status bar, menus). */
UENUM(BlueprintType)
enum class EStageHistoryChange : uint8
{
	Recorded,
	Undone,
	Redone,
	/** Stacks emptied or a step was dropped as invalid. */
	Cleared,
};

/**
 * The stage operations history steps may perform. Implemented against the live world by
 * UStageEditHistoryComponent (spawning still goes through the placement rules), and by fakes in tests,
 * so every command is testable without a world.
 *
 * Items are addressed by instance id, never by pointer: undoing a delete creates a new actor with the
 * old id, and every older step that names that id still finds it.
 */
class IStageItemEditor
{
public:
	virtual ~IStageItemEditor() = default;

	/** Fills OutSnapshot with the item's current state. False if no live item has InstanceId. */
	virtual bool CaptureItem(const FGuid& InstanceId, struct FStageItemSnapshot& OutSnapshot) const = 0;

	/** Recreates the item from Snapshot, keeping its id. Refused when the rules do not allow it now. */
	virtual EStageCommandResult RestoreItem(const struct FStageItemSnapshot& Snapshot) = 0;

	/** Removes the item (its selection is released first). Invalid if it does not exist. */
	virtual EStageCommandResult RemoveItem(const FGuid& InstanceId) = 0;

	/** Sets the item's world transform (scale is clamped like every other write). Invalid if it does not exist. */
	virtual EStageCommandResult SetItemTransform(const FGuid& InstanceId, const FTransform& Transform) = 0;
};

/**
 * One undoable user action. Commands are recorded after the action already happened (the tool did
 * the work through its normal validated path), so a command only knows how to revert and re-apply
 * it. Undo and Redo must be exact inverses and must leave the stage unchanged when they fail.
 */
class IStageEditCommand
{
public:
	virtual ~IStageEditCommand() = default;

	/** Short user-facing name, e.g. "Move Test Crate". */
	virtual FText GetDescription() const = 0;

	virtual EStageCommandResult Undo(IStageItemEditor& Editor) = 0;
	virtual EStageCommandResult Redo(IStageItemEditor& Editor) = 0;
};
