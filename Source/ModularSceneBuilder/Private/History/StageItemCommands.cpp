// Copyright Epic Games, Inc. All Rights Reserved.

#include "History/StageItemCommands.h"

#define LOCTEXT_NAMESPACE "StageItemCommands"

namespace StageItemCommands
{
	// Below these differences a transform edit is noise (float round trips through the inspector).
	constexpr double LocationTolerance = 0.01;		// cm
	constexpr double RotationTolerance = 1.e-4;		// quaternion components
	constexpr double ScaleTolerance = 1.e-4;

	/** Removes the item, refreshing Snapshot first so the latest state comes back on restore. */
	EStageCommandResult RemoveCapturing(IStageItemEditor& Editor, FStageItemSnapshot& Snapshot)
	{
		FStageItemSnapshot Latest;
		if (!Editor.CaptureItem(Snapshot.InstanceId, Latest))
		{
			return EStageCommandResult::Invalid;
		}

		const EStageCommandResult Result = Editor.RemoveItem(Snapshot.InstanceId);
		if (Result == EStageCommandResult::Succeeded)
		{
			Snapshot = MoveTemp(Latest);
		}
		return Result;
	}

	EStageCommandResult Restore(IStageItemEditor& Editor, const FStageItemSnapshot& Snapshot)
	{
		return Snapshot.IsValid() ? Editor.RestoreItem(Snapshot) : EStageCommandResult::Invalid;
	}
}

// --- Place ---

FStagePlaceItemCommand::FStagePlaceItemCommand(FStageItemSnapshot InPlacedItem)
	: Item(MoveTemp(InPlacedItem))
{
}

FText FStagePlaceItemCommand::GetDescription() const
{
	return FText::Format(LOCTEXT("Place", "Place {0}"), Item.DisplayName);
}

EStageCommandResult FStagePlaceItemCommand::Undo(IStageItemEditor& Editor)
{
	return StageItemCommands::RemoveCapturing(Editor, Item);
}

EStageCommandResult FStagePlaceItemCommand::Redo(IStageItemEditor& Editor)
{
	return StageItemCommands::Restore(Editor, Item);
}

// --- Delete ---

FStageDeleteItemCommand::FStageDeleteItemCommand(FStageItemSnapshot InDeletedItem)
	: Item(MoveTemp(InDeletedItem))
{
}

FText FStageDeleteItemCommand::GetDescription() const
{
	return FText::Format(LOCTEXT("Delete", "Delete {0}"), Item.DisplayName);
}

EStageCommandResult FStageDeleteItemCommand::Undo(IStageItemEditor& Editor)
{
	return StageItemCommands::Restore(Editor, Item);
}

EStageCommandResult FStageDeleteItemCommand::Redo(IStageItemEditor& Editor)
{
	return StageItemCommands::RemoveCapturing(Editor, Item);
}

// --- Transform ---

FStageTransformItemCommand::FStageTransformItemCommand(const FGuid& InInstanceId, const FText& InItemName, const FTransform& InBefore, const FTransform& InAfter)
	: InstanceId(InInstanceId)
	, ItemName(InItemName)
	, Before(InBefore)
	, After(InAfter)
{
}

FText FStageTransformItemCommand::GetDescription() const
{
	using namespace StageItemCommands;

	const bool bMoved = !Before.GetLocation().Equals(After.GetLocation(), LocationTolerance);
	const bool bRotated = !Before.GetRotation().Equals(After.GetRotation(), RotationTolerance);
	const bool bScaled = !Before.GetScale3D().Equals(After.GetScale3D(), ScaleTolerance);
	const int32 PartsChanged = int32(bMoved) + int32(bRotated) + int32(bScaled);

	if (PartsChanged == 1)
	{
		const FText Verb = bMoved ? LOCTEXT("Move", "Move") : (bRotated ? LOCTEXT("Rotate", "Rotate") : LOCTEXT("Scale", "Scale"));
		return FText::Format(LOCTEXT("TransformVerb", "{0} {1}"), Verb, ItemName);
	}
	return FText::Format(LOCTEXT("Transform", "Transform {0}"), ItemName);
}

EStageCommandResult FStageTransformItemCommand::Undo(IStageItemEditor& Editor)
{
	return Editor.SetItemTransform(InstanceId, Before);
}

EStageCommandResult FStageTransformItemCommand::Redo(IStageItemEditor& Editor)
{
	return Editor.SetItemTransform(InstanceId, After);
}

bool FStageTransformItemCommand::IsMeaningfulChange(const FTransform& InBefore, const FTransform& InAfter)
{
	using namespace StageItemCommands;

	return !InBefore.GetLocation().Equals(InAfter.GetLocation(), LocationTolerance)
		|| !InBefore.GetRotation().Equals(InAfter.GetRotation(), RotationTolerance)
		|| !InBefore.GetScale3D().Equals(InAfter.GetScale3D(), ScaleTolerance);
}

#undef LOCTEXT_NAMESPACE
