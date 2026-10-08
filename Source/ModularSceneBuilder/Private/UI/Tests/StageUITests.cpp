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

#endif // WITH_DEV_AUTOMATION_TESTS
