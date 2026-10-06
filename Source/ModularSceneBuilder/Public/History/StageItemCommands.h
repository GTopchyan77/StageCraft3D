// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "History/StageEditCommand.h"
#include "History/StageItemSnapshot.h"

/**
 * History steps for the three structural edits: placing, deleting and transforming an item.
 * Pure logic against IStageItemEditor; covered by the StageCraft.History.ItemCommands tests.
 */

/**
 * An item was placed. Undo removes it, redo brings it back with the same id. The snapshot is re-taken
 * just before every removal, so edits made after placing (label, patch, attributes) survive undo + redo.
 */
class MODULARSCENEBUILDER_API FStagePlaceItemCommand final : public IStageEditCommand
{
public:
	explicit FStagePlaceItemCommand(FStageItemSnapshot InPlacedItem);

	virtual FText GetDescription() const override;
	virtual EStageCommandResult Undo(IStageItemEditor& Editor) override;
	virtual EStageCommandResult Redo(IStageItemEditor& Editor) override;

private:
	FStageItemSnapshot Item;
};

/** An item was deleted. The exact inverse of FStagePlaceItemCommand. */
class MODULARSCENEBUILDER_API FStageDeleteItemCommand final : public IStageEditCommand
{
public:
	/** InDeletedItem must be captured before the deletion. */
	explicit FStageDeleteItemCommand(FStageItemSnapshot InDeletedItem);

	virtual FText GetDescription() const override;
	virtual EStageCommandResult Undo(IStageItemEditor& Editor) override;
	virtual EStageCommandResult Redo(IStageItemEditor& Editor) override;

private:
	FStageItemSnapshot Item;
};

/** An item's world transform changed (gizmo drag or numeric Location / Rotation / Scale edit). */
class MODULARSCENEBUILDER_API FStageTransformItemCommand final : public IStageEditCommand
{
public:
	FStageTransformItemCommand(const FGuid& InInstanceId, const FText& InItemName, const FTransform& InBefore, const FTransform& InAfter);

	/** "Move", "Rotate", "Scale" or "Transform" (several parts changed) plus the item name. */
	virtual FText GetDescription() const override;
	virtual EStageCommandResult Undo(IStageItemEditor& Editor) override;
	virtual EStageCommandResult Redo(IStageItemEditor& Editor) override;

	/** True when Before and After differ enough to be worth a history step. */
	static bool IsMeaningfulChange(const FTransform& Before, const FTransform& After);

private:
	FGuid InstanceId;
	FText ItemName;
	FTransform Before;
	FTransform After;
};
