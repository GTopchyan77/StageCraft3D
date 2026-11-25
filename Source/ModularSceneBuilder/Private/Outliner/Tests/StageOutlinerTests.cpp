// Copyright Epic Games, Inc. All Rights Reserved.

// Unit tests for the outliner model (Docs/ADR/0005-spans-outliner-and-bill-of-materials.md §4): discipline grouping, order,
// flag summaries and the filter. No world.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Outliner; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Algo/Reverse.h"
#include "Data/StageItemTypes.h"
#include "Misc/AutomationTest.h"
#include "Outliner/StageOutlinerModel.h"

namespace StageOutlinerTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	FStageOutlinerItem MakeItem(const TCHAR* Label, const FGameplayTag& Category, bool bHidden = false, bool bLocked = false)
	{
		FStageOutlinerItem Item;
		Item.InstanceId = FGuid::NewGuid();
		Item.Label = FText::FromString(Label);
		Item.ModelName = FText::FromString(TEXT("Model"));
		Item.Category = Category;
		Item.bHidden = bHidden;
		Item.bLocked = bLocked;
		return Item;
	}

	TArray<FString> Labels(const FStageOutlinerGroup& Group)
	{
		TArray<FString> Result;
		for (const FStageOutlinerItem& Item : Group.Items)
		{
			Result.Add(Item.Label.ToString());
		}
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageOutlinerModelTest, "StageCraft.Outliner.Model", StageOutlinerTests::Flags)

bool FStageOutlinerModelTest::RunTest(const FString& Parameters)
{
	using namespace StageOutlinerTests;
	using StageOutliner::GetGroupCategory;

	// Discipline of a category: the direct child of StageCraft.Category.
	TestTrue(TEXT("Leaf maps to its discipline"), GetGroupCategory(StageCraftTags::Category_Lighting_MovingHead) == FGameplayTag(StageCraftTags::Category_Lighting));
	TestTrue(TEXT("A discipline maps to itself"), GetGroupCategory(StageCraftTags::Category_Truss) == FGameplayTag(StageCraftTags::Category_Truss));
	TestFalse(TEXT("The root is no discipline"), GetGroupCategory(StageCraftTags::Category).IsValid());
	TestFalse(TEXT("No category is no discipline"), GetGroupCategory(FGameplayTag()).IsValid());
	TestEqual(TEXT("Group label"), StageOutliner::MakeGroupLabel(StageCraftTags::Category_Audio).ToString(), FString(TEXT("Audio")));
	TestEqual(TEXT("No group reads Other"), StageOutliner::MakeGroupLabel(FGameplayTag()).ToString(), FString(TEXT("Other")));

	TArray<FStageOutlinerItem> Items;
	Items.Add(MakeItem(TEXT("Spot 10"), StageCraftTags::Category_Lighting_MovingHead));
	Items.Add(MakeItem(TEXT("Crate"), StageCraftTags::Category_Prop, /*bHidden*/ true));
	Items.Add(MakeItem(TEXT("Spot 2"), StageCraftTags::Category_Lighting_MovingHead, false, /*bLocked*/ true));
	Items.Add(MakeItem(TEXT("Truss Run 1"), StageCraftTags::Category_Truss_Straight, true, true));
	Items.Add(MakeItem(TEXT("Mystery"), FGameplayTag()));
	Items.Add(MakeItem(TEXT("Par 1"), StageCraftTags::Category_Lighting_Par, true));

	const TArray<FStageOutlinerGroup> Groups = StageOutliner::BuildGroups(Items);
	if (!TestEqual(TEXT("Four disciplines"), Groups.Num(), 4))
	{
		return false;
	}
	TestTrue(TEXT("Truss first"), Groups[0].Category == FGameplayTag(StageCraftTags::Category_Truss));
	TestTrue(TEXT("Lighting second"), Groups[1].Category == FGameplayTag(StageCraftTags::Category_Lighting));
	TestTrue(TEXT("Props third"), Groups[2].Category == FGameplayTag(StageCraftTags::Category_Prop));
	TestFalse(TEXT("Other last"), Groups[3].Category.IsValid());

	TestTrue(TEXT("Natural label order (2 before 10)"), Labels(Groups[1]) == TArray<FString>{ TEXT("Par 1"), TEXT("Spot 2"), TEXT("Spot 10") });
	TestTrue(TEXT("Lighting partly hidden"), Groups[1].Hidden == EStageOutlinerFlagState::Some);
	TestTrue(TEXT("Lighting partly locked"), Groups[1].Locked == EStageOutlinerFlagState::Some);
	TestTrue(TEXT("Truss fully hidden"), Groups[0].Hidden == EStageOutlinerFlagState::All);
	TestTrue(TEXT("Truss fully locked"), Groups[0].Locked == EStageOutlinerFlagState::All);
	TestTrue(TEXT("Other not locked"), Groups[3].Locked == EStageOutlinerFlagState::None);

	// Deterministic: the order items arrive in does not matter.
	TArray<FStageOutlinerItem> Reversed = Items;
	Algo::Reverse(Reversed);
	const TArray<FStageOutlinerGroup> Again = StageOutliner::BuildGroups(Reversed);
	bool bSame = Again.Num() == Groups.Num();
	for (int32 Index = 0; bSame && Index < Groups.Num(); ++Index)
	{
		bSame = Labels(Again[Index]) == Labels(Groups[Index]);
	}
	TestTrue(TEXT("Same grouping and order for reversed input"), bSame);

	// Equal labels keep a stable order by id.
	TArray<FStageOutlinerItem> Twins = { MakeItem(TEXT("Twin"), StageCraftTags::Category_Prop), MakeItem(TEXT("Twin"), StageCraftTags::Category_Prop) };
	const FGuid First = FMath::Min(Twins[0].InstanceId, Twins[1].InstanceId);
	TestTrue(TEXT("Equal labels ordered by id"), StageOutliner::BuildGroups(Twins)[0].Items[0].InstanceId == First);
	Algo::Reverse(Twins);
	TestTrue(TEXT("... in any input order"), StageOutliner::BuildGroups(Twins)[0].Items[0].InstanceId == First);

	// Filter: every word in the label, model or discipline.
	const FStageOutlinerItem& Spot = Groups[1].Items[1];
	TestTrue(TEXT("Empty filter matches"), StageOutliner::MatchesFilter(Spot, Groups[1].Label, TEXT("")));
	TestTrue(TEXT("Label word"), StageOutliner::MatchesFilter(Spot, Groups[1].Label, TEXT("spot")));
	TestTrue(TEXT("Discipline word"), StageOutliner::MatchesFilter(Spot, Groups[1].Label, TEXT("lighting 2")));
	TestTrue(TEXT("Model word"), StageOutliner::MatchesFilter(Spot, Groups[1].Label, TEXT("model")));
	TestFalse(TEXT("Unmatched word"), StageOutliner::MatchesFilter(Spot, Groups[1].Label, TEXT("spot truss")));

	TestEqual(TEXT("No items, no groups"), StageOutliner::BuildGroups({}).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageOutlinerListingTest, "StageCraft.Outliner.Listing", StageOutlinerTests::Flags)

bool FStageOutlinerListingTest::RunTest(const FString& Parameters)
{
	using namespace StageOutlinerTests;

	TArray<FStageOutlinerItem> Items;
	Items.Add(MakeItem(TEXT("Truss Run 1"), StageCraftTags::Category_Truss_Straight, /*bHidden*/ true, /*bLocked*/ true));
	Items.Add(MakeItem(TEXT("Spot 1"), StageCraftTags::Category_Lighting_MovingHead));
	Items.Add(MakeItem(TEXT("Spot 2"), StageCraftTags::Category_Lighting_MovingHead, false, /*bLocked*/ true));
	Items.Add(MakeItem(TEXT("Crate"), StageCraftTags::Category_Prop, /*bHidden*/ true));
	const TArray<FStageOutlinerGroup> Groups = StageOutliner::BuildGroups(Items);
	if (!TestEqual(TEXT("Truss, Lighting, Prop"), Groups.Num(), 3))
	{
		return false;
	}

	// Lines as "G<group>" for a header (with "+" when expanded) and "<group>.<item>" for an item.
	const auto Describe = [](const FStageOutlinerListing& Listing)
	{
		TArray<FString> Lines;
		for (const FStageOutlinerRowRef& Row : Listing.Rows)
		{
			Lines.Add(Row.IsGroupRow() ? FString::Printf(TEXT("G%d%s"), Row.GroupIndex, Row.bExpanded ? TEXT("+") : TEXT(""))
				: FString::Printf(TEXT("%d.%d"), Row.GroupIndex, Row.ItemIndex));
		}
		return FString::Join(Lines, TEXT(" "));
	};

	// Everything expanded: each header followed by its items, in model order.
	const FStageOutlinerListing All = StageOutliner::BuildListing(Groups, {}, TEXT(""));
	TestEqual(TEXT("All lines"), Describe(All), FString(TEXT("G0+ 0.0 G1+ 1.0 1.1 G2+ 2.0")));
	TestEqual(TEXT("All listed"), All.Listed, 4);
	TestEqual(TEXT("Hidden on stage"), All.Hidden, 2);
	TestEqual(TEXT("Locked on stage"), All.Locked, 2);

	// A collapsed group keeps its header (closed) and its count; its items are not lines.
	const TSet<FGameplayTag> Collapsed = { FGameplayTag(StageCraftTags::Category_Lighting) };
	const FStageOutlinerListing Closed = StageOutliner::BuildListing(Groups, Collapsed, TEXT(""));
	TestEqual(TEXT("Collapsed lines"), Describe(Closed), FString(TEXT("G0+ 0.0 G1 G2+ 2.0")));
	TestEqual(TEXT("Collapsed items still count as listed"), Closed.Listed, 4);

	// Filtering lists only matching items, drops groups without one, and opens collapsed groups. Flags count the whole stage.
	const FStageOutlinerListing Filtered = StageOutliner::BuildListing(Groups, Collapsed, TEXT("spot 2"));
	TestEqual(TEXT("Filtered lines"), Describe(Filtered), FString(TEXT("G1+ 1.1")));
	TestEqual(TEXT("Filtered listed"), Filtered.Listed, 1);
	TestEqual(TEXT("Filtered hidden counts the stage"), Filtered.Hidden, 2);
	TestEqual(TEXT("Blank filter is no filter"), Describe(StageOutliner::BuildListing(Groups, Collapsed, TEXT("   "))), Describe(Closed));

	const FStageOutlinerListing None = StageOutliner::BuildListing(Groups, {}, TEXT("nothing matches this"));
	TestEqual(TEXT("No match, no lines"), None.Rows.Num(), 0);
	TestEqual(TEXT("No match, none listed"), None.Listed, 0);
	TestEqual(TEXT("Empty stage, no lines"), StageOutliner::BuildListing({}, {}, TEXT("")).Rows.Num(), 0);

	// A large stage is one header per group plus one line per item; the list makes widgets only for the lines on screen.
	TArray<FStageOutlinerItem> Rig;
	for (int32 Index = 0; Index < 500; ++Index)
	{
		Rig.Add(MakeItem(*FString::Printf(TEXT("Beam %d"), Index + 1), StageCraftTags::Category_Lighting_MovingHead));
	}
	const TArray<FStageOutlinerGroup> RigGroups = StageOutliner::BuildGroups(Rig);
	TestEqual(TEXT("500 items, 501 lines"), StageOutliner::BuildListing(RigGroups, {}, TEXT("")).Rows.Num(), 501);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
