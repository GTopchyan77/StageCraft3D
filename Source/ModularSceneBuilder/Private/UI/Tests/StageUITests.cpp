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
	TestTrue(TEXT("Group hint names group delete"), Group.Contains(TEXT("Delete")));
	TestFalse(TEXT("One selected item reads like a single selection"), MakeHint(EStageEditMode::Select, Crate, 1).ToString().Contains(TEXT("items selected")));
	TestTrue(TEXT("Place mode ignores the selection count"), MakeHint(EStageEditMode::Place, Crate, 3).ToString().Contains(TEXT("Placing")));
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
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
