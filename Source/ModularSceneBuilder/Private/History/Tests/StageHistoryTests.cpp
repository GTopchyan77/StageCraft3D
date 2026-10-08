// Copyright Epic Games, Inc. All Rights Reserved.

// Unit tests for the undo/redo history (Docs/ADR/0003-undo-redo-and-object-snapping.md).
// No world: the stack and the item commands run against an in-memory IStageItemEditor.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.History; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Data/BaseItemData.h"
#include "History/StageBatchCommand.h"
#include "History/StageCommandHistory.h"
#include "History/StageItemCommands.h"
#include "History/StageItemSnapshot.h"
#include "Misc/AutomationTest.h"

namespace StageHistoryTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	/** The stage as a map of id -> snapshot. Restores can be refused to model the placement rules saying no. */
	class FFakeItemEditor final : public IStageItemEditor
	{
	public:
		TMap<FGuid, FStageItemSnapshot> Items;
		bool bRefuseRestores = false;
		/** When set, restores succeed this many more times and are then refused (a session limit reached part-way). */
		TOptional<int32> RestoresLeft;

		virtual bool CaptureItem(const FGuid& InstanceId, FStageItemSnapshot& OutSnapshot) const override
		{
			const FStageItemSnapshot* Found = Items.Find(InstanceId);
			if (Found)
			{
				OutSnapshot = *Found;
			}
			return Found != nullptr;
		}

		virtual EStageCommandResult RestoreItem(const FStageItemSnapshot& Snapshot) override
		{
			if (Items.Contains(Snapshot.InstanceId))
			{
				return EStageCommandResult::Invalid;
			}
			if (bRefuseRestores || (RestoresLeft.IsSet() && RestoresLeft.GetValue() <= 0))
			{
				return EStageCommandResult::Refused;
			}
			if (RestoresLeft.IsSet())
			{
				RestoresLeft = RestoresLeft.GetValue() - 1;
			}
			Items.Add(Snapshot.InstanceId, Snapshot);
			return EStageCommandResult::Succeeded;
		}

		virtual EStageCommandResult RemoveItem(const FGuid& InstanceId) override
		{
			return Items.Remove(InstanceId) > 0 ? EStageCommandResult::Succeeded : EStageCommandResult::Invalid;
		}

		virtual EStageCommandResult SetItemTransform(const FGuid& InstanceId, const FTransform& Transform) override
		{
			FStageItemSnapshot* Found = Items.Find(InstanceId);
			if (!Found)
			{
				return EStageCommandResult::Invalid;
			}
			Found->Transform = Transform;
			return EStageCommandResult::Succeeded;
		}
	};

	/** Adds Delta to a shared counter on redo, subtracts on undo; a scripted result can override the outcome. */
	class FCounterCommand final : public IStageEditCommand
	{
	public:
		FCounterCommand(int32& InCounter, int32 InDelta) : Counter(InCounter), Delta(InDelta) {}

		TOptional<EStageCommandResult> ForcedResult;

		virtual FText GetDescription() const override { return FText::AsNumber(Delta); }

		virtual EStageCommandResult Undo(IStageItemEditor& Editor) override
		{
			if (ForcedResult.IsSet())
			{
				return ForcedResult.GetValue();
			}
			Counter -= Delta;
			return EStageCommandResult::Succeeded;
		}

		virtual EStageCommandResult Redo(IStageItemEditor& Editor) override
		{
			if (ForcedResult.IsSet())
			{
				return ForcedResult.GetValue();
			}
			Counter += Delta;
			return EStageCommandResult::Succeeded;
		}

	private:
		int32& Counter;
		int32 Delta;
	};

	FStageItemSnapshot MakeItem(const TCHAR* Name, const FVector& Location)
	{
		FStageItemSnapshot Snapshot;
		Snapshot.InstanceId = FGuid::NewGuid();
		Snapshot.Item = TSoftObjectPtr<UBaseItemData>(FSoftObjectPath(TEXT("/Game/StageCraft/Items/DA_Test_Crate.DA_Test_Crate")));
		Snapshot.Transform = FTransform(Location);
		Snapshot.DisplayName = FText::FromString(Name);
		return Snapshot;
	}

	FString Str(EStageCommandResult Result) { return UEnum::GetValueAsString(Result); }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageHistoryStackTest, "StageCraft.History.Stack", StageHistoryTests::Flags)

bool FStageHistoryStackTest::RunTest(const FString& Parameters)
{
	using namespace StageHistoryTests;

	FFakeItemEditor Editor;
	int32 Counter = 0;
	FStageCommandHistory History(/*MaxDepth*/ 3);

	TestEqual(TEXT("Empty history has nothing to undo"), Str(History.Undo(Editor)), Str(EStageCommandResult::NothingToDo));
	TestEqual(TEXT("Empty history has nothing to redo"), Str(History.Redo(Editor)), Str(EStageCommandResult::NothingToDo));
	TestTrue(TEXT("Empty descriptions"), History.GetUndoDescription().IsEmpty() && History.GetRedoDescription().IsEmpty());

	// Commands are recorded after they ran, so the counter is advanced by hand like a tool would.
	Counter += 1; History.Record(MakeShared<FCounterCommand>(Counter, 1));
	Counter += 10; History.Record(MakeShared<FCounterCommand>(Counter, 10));
	TestEqual(TEXT("Two steps to undo"), History.GetUndoCount(), 2);
	TestEqual(TEXT("Undo names the latest step"), History.GetUndoDescription().ToString(), FString(TEXT("10")));

	TestEqual(TEXT("Undo succeeds"), Str(History.Undo(Editor)), Str(EStageCommandResult::Succeeded));
	TestEqual(TEXT("Undo reverts the latest step only (LIFO)"), Counter, 1);
	TestEqual(TEXT("Undone step can be redone"), History.GetRedoCount(), 1);
	TestEqual(TEXT("Redo names it"), History.GetRedoDescription().ToString(), FString(TEXT("10")));

	TestEqual(TEXT("Redo succeeds"), Str(History.Redo(Editor)), Str(EStageCommandResult::Succeeded));
	TestEqual(TEXT("Redo re-applies it"), Counter, 11);
	TestEqual(TEXT("Back on the undo stack"), History.GetUndoCount(), 2);

	// A new action forks history: the redo stack is discarded.
	History.Undo(Editor);
	Counter += 100; History.Record(MakeShared<FCounterCommand>(Counter, 100));
	TestFalse(TEXT("Recording clears redo"), History.CanRedo());
	TestEqual(TEXT("Counter after fork"), Counter, 101);

	// Depth limit: the oldest steps are dropped.
	Counter += 1000; History.Record(MakeShared<FCounterCommand>(Counter, 1000));
	Counter += 10000; History.Record(MakeShared<FCounterCommand>(Counter, 10000));
	TestEqual(TEXT("Undo stack capped at MaxDepth"), History.GetUndoCount(), 3);
	while (History.Undo(Editor) == EStageCommandResult::Succeeded) {}
	TestEqual(TEXT("Only the newest three steps were kept; the first (+1) can no longer be undone"), Counter, 1);

	// Refused: stays where it is, nothing changes, and it can be retried.
	History.Clear();
	TSharedRef<FCounterCommand> Refusing = MakeShared<FCounterCommand>(Counter, 5);
	Refusing->ForcedResult = EStageCommandResult::Refused;
	History.Record(Refusing);
	TestEqual(TEXT("Refused undo reports Refused"), Str(History.Undo(Editor)), Str(EStageCommandResult::Refused));
	TestEqual(TEXT("Refused step stays on the undo stack"), History.GetUndoCount(), 1);
	TestFalse(TEXT("Refused step does not move to redo"), History.CanRedo());
	Refusing->ForcedResult.Reset();
	TestEqual(TEXT("Retry succeeds once allowed"), Str(History.Undo(Editor)), Str(EStageCommandResult::Succeeded));

	// Invalid: dropped for good, the step below becomes next.
	History.Clear();
	History.Record(MakeShared<FCounterCommand>(Counter, 7));
	TSharedRef<FCounterCommand> Broken = MakeShared<FCounterCommand>(Counter, 9);
	Broken->ForcedResult = EStageCommandResult::Invalid;
	History.Record(Broken);
	TestEqual(TEXT("Invalid undo reports Invalid"), Str(History.Undo(Editor)), Str(EStageCommandResult::Invalid));
	TestEqual(TEXT("Invalid step is dropped"), History.GetUndoCount(), 1);
	TestFalse(TEXT("Invalid step never reaches redo"), History.CanRedo());
	TestEqual(TEXT("Next undo is the step below"), History.GetUndoDescription().ToString(), FString(TEXT("7")));

	TestEqual(TEXT("Depth below 1 is raised to 1"), FStageCommandHistory(0).GetMaxDepth(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageHistoryItemCommandsTest, "StageCraft.History.ItemCommands", StageHistoryTests::Flags)

bool FStageHistoryItemCommandsTest::RunTest(const FString& Parameters)
{
	using namespace StageHistoryTests;

	FFakeItemEditor Editor;
	FStageCommandHistory History;

	// Place: the tool already spawned it, the command only knows how to revert it.
	const FStageItemSnapshot Crate = MakeItem(TEXT("Crate A"), FVector(100.0, 0.0, 0.0));
	Editor.Items.Add(Crate.InstanceId, Crate);
	History.Record(MakeShared<FStagePlaceItemCommand>(Crate));
	TestEqual(TEXT("Place description"), History.GetUndoDescription().ToString(), FString(TEXT("Place Crate A")));

	// An edit made after placing must survive undo + redo (the snapshot is refreshed before removal).
	Editor.Items[Crate.InstanceId].DisplayName = FText::FromString(TEXT("Crate A (renamed)"));
	TestEqual(TEXT("Undo place"), Str(History.Undo(Editor)), Str(EStageCommandResult::Succeeded));
	TestFalse(TEXT("Undo place removes the item"), Editor.Items.Contains(Crate.InstanceId));
	TestEqual(TEXT("Redo place"), Str(History.Redo(Editor)), Str(EStageCommandResult::Succeeded));
	TestTrue(TEXT("Redo restores the same id"), Editor.Items.Contains(Crate.InstanceId));
	TestEqual(TEXT("Redo restores the latest state"), Editor.Items[Crate.InstanceId].DisplayName.ToString(), FString(TEXT("Crate A (renamed)")));

	// Transform: undo and redo set the exact recorded transforms.
	const FTransform Before = Editor.Items[Crate.InstanceId].Transform;
	const FTransform After(FRotator(0.0, 90.0, 0.0), FVector(250.0, 40.0, 0.0), FVector(2.0));
	Editor.Items[Crate.InstanceId].Transform = After;
	TestTrue(TEXT("A real change is meaningful"), FStageTransformItemCommand::IsMeaningfulChange(Before, After));
	TestFalse(TEXT("Float noise is not"), FStageTransformItemCommand::IsMeaningfulChange(Before, FTransform(Before.GetLocation() + FVector(0.001))));
	History.Record(MakeShared<FStageTransformItemCommand>(Crate.InstanceId, Crate.DisplayName, Before, After));
	TestEqual(TEXT("Several parts changed -> Transform"), History.GetUndoDescription().ToString(), FString(TEXT("Transform Crate A")));
	TestEqual(TEXT("Move-only description"), FStageTransformItemCommand(Crate.InstanceId, Crate.DisplayName, Before, FTransform(FVector(1.0, 2.0, 3.0))).GetDescription().ToString(), FString(TEXT("Move Crate A")));

	History.Undo(Editor);
	TestTrue(TEXT("Undo transform restores Before"), Editor.Items[Crate.InstanceId].Transform.Equals(Before));
	History.Redo(Editor);
	TestTrue(TEXT("Redo transform restores After"), Editor.Items[Crate.InstanceId].Transform.Equals(After));

	// Delete, then undo the delete: the transform step recorded earlier must still find the item by id.
	FStageItemSnapshot Deleted;
	Editor.CaptureItem(Crate.InstanceId, Deleted);
	Editor.RemoveItem(Crate.InstanceId);
	History.Record(MakeShared<FStageDeleteItemCommand>(Deleted));
	TestEqual(TEXT("Delete description"), History.GetUndoDescription().ToString(), FString(TEXT("Delete Crate A (renamed)")));

	TestEqual(TEXT("Undo delete"), Str(History.Undo(Editor)), Str(EStageCommandResult::Succeeded));
	TestTrue(TEXT("Undo delete restores the item at its last transform"), Editor.Items.Contains(Crate.InstanceId) && Editor.Items[Crate.InstanceId].Transform.Equals(After));
	TestEqual(TEXT("Older transform step still applies to the restored item"), Str(History.Undo(Editor)), Str(EStageCommandResult::Succeeded));
	TestTrue(TEXT("... and moved it back"), Editor.Items[Crate.InstanceId].Transform.Equals(Before));

	// Redo everything forward again: transform, then the delete.
	History.Redo(Editor);
	TestEqual(TEXT("Redo delete"), Str(History.Redo(Editor)), Str(EStageCommandResult::Succeeded));
	TestFalse(TEXT("Redo delete removes it again"), Editor.Items.Contains(Crate.InstanceId));

	// Rules refuse a restore (e.g. session item limit): stacks unchanged, nothing restored.
	Editor.bRefuseRestores = true;
	const int32 UndoCount = History.GetUndoCount();
	TestEqual(TEXT("Refused restore"), Str(History.Undo(Editor)), Str(EStageCommandResult::Refused));
	TestEqual(TEXT("Refused restore keeps the step"), History.GetUndoCount(), UndoCount);
	TestFalse(TEXT("Refused restore spawns nothing"), Editor.Items.Contains(Crate.InstanceId));
	Editor.bRefuseRestores = false;
	TestEqual(TEXT("Allowed again: restore works"), Str(History.Undo(Editor)), Str(EStageCommandResult::Succeeded));

	// A step whose item vanished outside the history is invalid and dropped.
	FStageTransformItemCommand Orphan(FGuid::NewGuid(), FText::FromString(TEXT("Ghost")), Before, After);
	TestEqual(TEXT("Transform of a missing item is Invalid"), Str(Orphan.Undo(Editor)), Str(EStageCommandResult::Invalid));
	FStagePlaceItemCommand OrphanPlace(MakeItem(TEXT("Ghost"), FVector::ZeroVector));
	TestEqual(TEXT("Undo place of a missing item is Invalid"), Str(OrphanPlace.Undo(Editor)), Str(EStageCommandResult::Invalid));
	FStageItemSnapshot NoAsset = MakeItem(TEXT("No asset"), FVector::ZeroVector);
	NoAsset.Item.Reset();
	FStageDeleteItemCommand OrphanDelete(NoAsset);
	TestEqual(TEXT("Restore without a catalog item is Invalid"), Str(OrphanDelete.Undo(Editor)), Str(EStageCommandResult::Invalid));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageHistoryBatchTest, "StageCraft.History.Batch", StageHistoryTests::Flags)

bool FStageHistoryBatchTest::RunTest(const FString& Parameters)
{
	using namespace StageHistoryTests;

	FFakeItemEditor Editor;
	FStageCommandHistory History;
	const FStageItemSnapshot A = MakeItem(TEXT("A"), FVector(0, 0, 0));
	const FStageItemSnapshot B = MakeItem(TEXT("B"), FVector(100, 0, 0));
	const FStageItemSnapshot C = MakeItem(TEXT("C"), FVector(200, 0, 0));
	for (const FStageItemSnapshot& Item : { A, B, C })
	{
		Editor.Items.Add(Item.InstanceId, Item);
	}

	// A multi-delete as the history component records it: remove all, then one batch of delete commands.
	TArray<TSharedRef<IStageEditCommand>> Deletes;
	for (const FStageItemSnapshot& Item : { A, B, C })
	{
		Editor.RemoveItem(Item.InstanceId);
		Deletes.Add(MakeShared<FStageDeleteItemCommand>(Item));
	}
	History.Record(MakeShared<FStageBatchCommand>(FText::FromString(TEXT("Delete 3 items")), MoveTemp(Deletes)));
	TestEqual(TEXT("One step for the whole selection"), History.GetUndoCount(), 1);
	TestEqual(TEXT("Batch description"), History.GetUndoDescription().ToString(), FString(TEXT("Delete 3 items")));

	TestEqual(TEXT("Undo restores the group"), Str(History.Undo(Editor)), Str(EStageCommandResult::Succeeded));
	TestEqual(TEXT("All three are back"), Editor.Items.Num(), 3);
	TestEqual(TEXT("Redo deletes the group"), Str(History.Redo(Editor)), Str(EStageCommandResult::Succeeded));
	TestEqual(TEXT("All three are gone"), Editor.Items.Num(), 0);

	// All or nothing: the rules allow only two restores (a session item limit reached part-way).
	Editor.RestoresLeft = 2;
	TestEqual(TEXT("Partial restore is refused"), Str(History.Undo(Editor)), Str(EStageCommandResult::Refused));
	TestEqual(TEXT("The items restored before the refusal are removed again"), Editor.Items.Num(), 0);
	TestEqual(TEXT("The refused step stays undoable"), History.GetUndoCount(), 1);
	Editor.RestoresLeft.Reset();
	TestEqual(TEXT("Retry once allowed"), Str(History.Undo(Editor)), Str(EStageCommandResult::Succeeded));
	TestEqual(TEXT("Everything restored"), Editor.Items.Num(), 3);

	// A group move: one step that moves every item back and forth together.
	const FTransform MovedA(FVector(0, 50, 0));
	const FTransform MovedB(FVector(100, 50, 0));
	Editor.SetItemTransform(A.InstanceId, MovedA);
	Editor.SetItemTransform(B.InstanceId, MovedB);
	TArray<TSharedRef<IStageEditCommand>> Moves;
	Moves.Add(MakeShared<FStageTransformItemCommand>(A.InstanceId, FText::FromString(TEXT("A")), A.Transform, MovedA));
	Moves.Add(MakeShared<FStageTransformItemCommand>(B.InstanceId, FText::FromString(TEXT("B")), B.Transform, MovedB));
	History.Record(MakeShared<FStageBatchCommand>(FText::FromString(TEXT("Transform 2 items")), MoveTemp(Moves)));
	TestEqual(TEXT("Undo group move"), Str(History.Undo(Editor)), Str(EStageCommandResult::Succeeded));
	TestTrue(TEXT("A back"), Editor.Items[A.InstanceId].Transform.Equals(A.Transform));
	TestTrue(TEXT("B back"), Editor.Items[B.InstanceId].Transform.Equals(B.Transform));
	TestEqual(TEXT("Redo group move"), Str(History.Redo(Editor)), Str(EStageCommandResult::Succeeded));
	TestTrue(TEXT("A moved again"), Editor.Items[A.InstanceId].Transform.Equals(MovedA));
	TestTrue(TEXT("B moved again"), Editor.Items[B.InstanceId].Transform.Equals(MovedB));

	// One member vanished outside the history: the whole step is Invalid and the others are left as they were.
	Editor.Items.Remove(B.InstanceId);
	TestEqual(TEXT("Group step with a missing item is Invalid"), Str(History.Undo(Editor)), Str(EStageCommandResult::Invalid));
	TestTrue(TEXT("The surviving item is left exactly as the step found it"), Editor.Items[A.InstanceId].Transform.Equals(MovedA));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
