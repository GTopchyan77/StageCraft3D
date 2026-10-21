// Copyright Epic Games, Inc. All Rights Reserved.

// Unit tests for the pure rules behind the Library panel and the workspace status bar (STATE #24).
// No world and no widgets: search matching, category labels and status hints only.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.UI; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Data/StageItemTypes.h"
#include "Misc/AutomationTest.h"
#include "UI/StageItemLibraryPanel.h"
#include "UI/StageStatusBarWidget.h"

namespace StageUITests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageLibrarySearchTest, "StageCraft.UI.LibrarySearch", StageUITests::Flags)

bool FStageLibrarySearchTest::RunTest(const FString& Parameters)
{
	using StageItemLibrary::MatchesSearch;

	TestTrue(TEXT("Empty query matches"), MatchesSearch(TEXT("Moving Head 300"), TEXT("Lighting"), TEXT("")));
	TestTrue(TEXT("Blank query matches"), MatchesSearch(TEXT("Moving Head 300"), TEXT("Lighting"), TEXT("   \t")));
	TestTrue(TEXT("Name substring, any case"), MatchesSearch(TEXT("Moving Head 300"), TEXT("Lighting"), TEXT("HEAD")));
	TestTrue(TEXT("Category substring"), MatchesSearch(TEXT("Moving Head 300"), TEXT("Lighting"), TEXT("light")));
	TestTrue(TEXT("Every word must match, name or category"), MatchesSearch(TEXT("Moving Head 300"), TEXT("Lighting"), TEXT("light 300")));
	TestFalse(TEXT("One unmatched word fails"), MatchesSearch(TEXT("Moving Head 300"), TEXT("Lighting"), TEXT("light truss")));
	TestFalse(TEXT("No match"), MatchesSearch(TEXT("Crate"), TEXT("Stage"), TEXT("speaker")));

	TestEqual(TEXT("Category prefix stripped"), StageItemLibrary::MakeCategoryLabel(StageCraftTags::Category_Truss).ToString(), FString(TEXT("Truss")));
	TestEqual(TEXT("No category reads Other"), StageItemLibrary::MakeCategoryLabel(FGameplayTag()).ToString(), FString(TEXT("Other")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageStatusHintTest, "StageCraft.UI.StatusHint", StageUITests::Flags)

bool FStageStatusHintTest::RunTest(const FString& Parameters)
{
	using StageStatusBar::MakeHint;
	const FText Crate = FText::FromString(TEXT("Crate"));

	const FString Placing = MakeHint(EStageEditMode::Place, Crate).ToString();
	TestTrue(TEXT("Place hint names the armed item"), Placing.Contains(TEXT("Crate")));
	TestTrue(TEXT("Place hint says how to finish"), Placing.Contains(TEXT("Esc")));

	const FString PlacingNothing = MakeHint(EStageEditMode::Place, FText::GetEmpty()).ToString();
	TestTrue(TEXT("Place without item points to the Library"), PlacingNothing.Contains(TEXT("Library")));

	const FString Selecting = MakeHint(EStageEditMode::Select, Crate).ToString();
	TestTrue(TEXT("Select hint offers P for the armed item"), Selecting.Contains(TEXT("P: place Crate")));
	TestFalse(TEXT("Select hint without item does not offer P"), MakeHint(EStageEditMode::Select, FText::GetEmpty()).ToString().Contains(TEXT("P: place")));
	TestTrue(TEXT("Select hint teaches Ctrl+Click"), Selecting.Contains(TEXT("Ctrl+Click")));

	const FString Group = MakeHint(EStageEditMode::Select, Crate, 3).ToString();
	TestTrue(TEXT("Group hint counts the selection"), Group.Contains(TEXT("3 items selected")));
	TestTrue(TEXT("Group hint names group delete with its key"), Group.Contains(TEXT("Del: remove all")));
	TestFalse(TEXT("One selected item reads like a single selection"), MakeHint(EStageEditMode::Select, Crate, 1).ToString().Contains(TEXT("items selected")));
	TestTrue(TEXT("Place mode ignores the selection count"), MakeHint(EStageEditMode::Place, Crate, 3).ToString().Contains(TEXT("Placing")));
	TestTrue(TEXT("Select hint names the fly keys"), MakeHint(EStageEditMode::Select, FText::GetEmpty()).ToString().Contains(TEXT("Right mouse + WASD: fly")));

	// The hints follow the user's key map (Docs/ADR/0013-preferences-key-bindings-and-audit-fixes.md §2.4).
	FStageKeyBindings Remapped = FStageKeyBindings::MakeDefaults();
	Remapped.Assign(EStageInputCommand::TogglePlaceMode, EStageKeySlot::Primary, FStageKeyChord(EKeys::L));
	Remapped.Clear(EStageInputCommand::Cancel, EStageKeySlot::Primary);
	Remapped.Clear(EStageInputCommand::CycleTransformTool, EStageKeySlot::Primary);
	Remapped.Clear(EStageInputCommand::FlyLeft, EStageKeySlot::Primary);
	const StageStatusBar::FHintKeys Keys = StageStatusBar::FHintKeys::FromBindings(Remapped);
	TestTrue(TEXT("Remapped: the place key is the new one"), MakeHint(EStageEditMode::Select, Crate, 0, Keys).ToString().Contains(TEXT("L: place Crate")));
	TestEqual(TEXT("Remapped: an unbound Esc is left out of the finish hint"), MakeHint(EStageEditMode::Place, Crate, 0, Keys).ToString(),
		FString(TEXT("Placing Crate: each click places a copy   ·   L: finish")));
	const FString NoTools = MakeHint(EStageEditMode::Select, FText::GetEmpty(), 0, Keys).ToString();
	TestFalse(TEXT("Remapped: an unbound tool cycle is left out"), NoTools.Contains(TEXT("Move / Rotate / Scale")));
	TestTrue(TEXT("Remapped: without all fly keys the hint says look around"), NoTools.Contains(TEXT("Right mouse: look around")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageSpanHintTest, "StageCraft.UI.SpanHint", StageUITests::Flags)

bool FStageSpanHintTest::RunTest(const FString& Parameters)
{
	using StageStatusBar::MakeSpanHint;

	FStageSpanToolStatus Status;
	Status.ItemName = FText::FromString(TEXT("F34 Truss"));
	Status.Phase = EStageSpanToolPhase::PickStart;
	TestTrue(TEXT("Start hint asks for the start point"), MakeSpanHint(Status).ToString().Contains(TEXT("starts")));

	Status.Phase = EStageSpanToolPhase::PickEnd;
	Status.PointCount = 1;
	TestTrue(TEXT("Hint without a run asks for the next point"), MakeSpanHint(Status).ToString().Contains(TEXT("next point")));

	Status.bHasRun = true;
	Status.Length = 600.0;
	Status.SegmentCount = 3;
	Status.LegCount = 1;
	const FString Run = MakeSpanHint(Status).ToString();
	TestTrue(TEXT("Run hint gives the length in metres"), Run.Contains(TEXT("6.00 m")));
	TestTrue(TEXT("Run hint counts segments"), Run.Contains(TEXT("3 segments")));
	TestTrue(TEXT("With only the start placed, a double-click places the run"), Run.Contains(TEXT("double-click")));
	TestFalse(TEXT("Enter is not offered before a second point"), Run.Contains(TEXT("Enter")));

	Status.PointCount = 3;
	Status.LegCount = 3;
	Status.Length = 1200.0;
	Status.SegmentCount = 6;
	const FString Path = MakeSpanHint(Status).ToString();
	TestTrue(TEXT("Path hint counts legs"), Path.Contains(TEXT("in 3 legs")));
	TestTrue(TEXT("Path hint offers Enter"), Path.Contains(TEXT("Enter")));
	TestTrue(TEXT("Path hint offers Backspace"), Path.Contains(TEXT("Backspace")));

	Status.bClosing = true;
	TestTrue(TEXT("On the start point the hint offers closing"), MakeSpanHint(Status).ToString().Contains(TEXT("close the loop")));
	Status.bClosing = false;

	// A loop that cannot close shows a refused ghost; the hint must name the real reason, not the stage's limits.
	Status.bCloseBlocked = true;
	Status.bRefused = true;
	const FString Blocked = MakeSpanHint(Status).ToString();
	TestTrue(TEXT("Blocked close explains whole sections"), Blocked.Contains(TEXT("does not close in whole sections")));
	TestFalse(TEXT("Blocked close is not blamed on limits"), Blocked.Contains(TEXT("not allowed")));
	Status.bCloseBlocked = false;
	Status.bRefused = false;

	Status.LegCount = 1;
	Status.SegmentCount = 1;
	TestTrue(TEXT("Singular segment"), MakeSpanHint(Status).ToString().Contains(TEXT("1 segment ")));

	Status.bRefused = true;
	TestTrue(TEXT("Refused path says so"), MakeSpanHint(Status).ToString().Contains(TEXT("not allowed")));
	Status.bRefused = false;

	// Remapped keys: the finish key moves to F; an unbound Backspace is left out.
	FStageKeyBindings Remapped = FStageKeyBindings::MakeDefaults();
	Remapped.Assign(EStageInputCommand::FinishPath, EStageKeySlot::Primary, FStageKeyChord(EKeys::F));
	Remapped.Clear(EStageInputCommand::RemovePathPoint, EStageKeySlot::Primary);
	Status.PointCount = 3;
	const FString RemappedPath = MakeSpanHint(Status, StageStatusBar::FHintKeys::FromBindings(Remapped)).ToString();
	TestTrue(TEXT("Remapped: the finish key is named"), RemappedPath.Contains(TEXT("F or double-click: place")));
	TestFalse(TEXT("Remapped: Enter is not offered"), RemappedPath.Contains(TEXT("Enter")));
	TestFalse(TEXT("Remapped: an unbound undo-point key is left out"), RemappedPath.Contains(TEXT("undo point")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
