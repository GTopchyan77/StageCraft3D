// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for the pure DMX address allocation (Docs/ADR/0019 §2): fitting a footprint, the first free range, conflicts (overlaps
// and ranges past channel 512) with a sweep, consecutive plans that are all or nothing, conflict repair and universe usage,
// independent of the order the fixtures come in.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.DMX; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Show/StageDMXPatching.h"

namespace StageDMXPatchingTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	FStageDMXPatch MakePatch(int32 Universe, int32 Address)
	{
		FStageDMXPatch Patch;
		Patch.Universe = Universe;
		Patch.Address = Address;
		return Patch;
	}

	FStageDMXPatchSlot MakeSlot(int32 Key, int32 Universe, int32 Address, int32 Footprint)
	{
		return { Key, MakePatch(Universe, Address), Footprint };
	}

	FString Describe(const FStageDMXPatch& Patch)
	{
		return FString::Printf(TEXT("%d.%03d"), Patch.Universe, Patch.Address);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageDMXFitTest, "StageCraft.DMX.Patching.Fit", StageDMXPatchingTests::Flags)

bool FStageDMXFitTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("21 channels at 1"), StageDMXPatching::Fits(1, 21));
	TestTrue(TEXT("21 channels at 492 end on 512"), StageDMXPatching::Fits(492, 21));
	TestFalse(TEXT("21 channels at 493 run past 512"), StageDMXPatching::Fits(493, 21));
	TestFalse(TEXT("address 0 is unpatched"), StageDMXPatching::Fits(0, 1));
	TestFalse(TEXT("no footprint"), StageDMXPatching::Fits(1, 0));
	TestFalse(TEXT("footprint above a universe"), StageDMXPatching::Fits(1, 513));
	TestEqual(TEXT("last start of 512"), StageDMXPatching::GetLastStartAddress(512), 1);
	TestEqual(TEXT("last start of 1"), StageDMXPatching::GetLastStartAddress(1), 512);
	TestEqual(TEXT("last start of nothing"), StageDMXPatching::GetLastStartAddress(0), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageDMXFindFreeTest, "StageCraft.DMX.Patching.FindFree", StageDMXPatchingTests::Flags)

bool FStageDMXFindFreeTest::RunTest(const FString& Parameters)
{
	using namespace StageDMXPatchingTests;
	const FStageDMXPatch Origin = MakePatch(1, 1);

	TestEqual(TEXT("empty patch"), Describe(StageDMXPatching::FindFree({}, 21, Origin)), FString(TEXT("1.001")));
	TestEqual(TEXT("after one fixture"), Describe(StageDMXPatching::FindFree({ MakeSlot(0, 1, 1, 21) }, 21, Origin)), FString(TEXT("1.022")));

	// A gap exactly as wide as the footprint is used; a wider footprint goes past the next fixture.
	const TArray<FStageDMXPatchSlot> Gap = { MakeSlot(0, 1, 1, 10), MakeSlot(1, 1, 20, 11) };
	TestEqual(TEXT("fits the 9-channel gap"), Describe(StageDMXPatching::FindFree(Gap, 9, Origin)), FString(TEXT("1.011")));
	TestEqual(TEXT("10 channels go after"), Describe(StageDMXPatching::FindFree(Gap, 10, Origin)), FString(TEXT("1.031")));

	// Order of the slots does not matter.
	const TArray<FStageDMXPatchSlot> Reversed = { Gap[1], Gap[0] };
	TestEqual(TEXT("order independent"), Describe(StageDMXPatching::FindFree(Reversed, 9, Origin)), FString(TEXT("1.011")));

	// The start address and universe are respected; a full universe moves on.
	TestEqual(TEXT("from 1.005"), Describe(StageDMXPatching::FindFree({ MakeSlot(0, 1, 1, 10) }, 4, MakePatch(1, 5))), FString(TEXT("1.011")));
	TestEqual(TEXT("full universe"), Describe(StageDMXPatching::FindFree({ MakeSlot(0, 1, 1, 512) }, 1, Origin)), FString(TEXT("2.001")));
	TestEqual(TEXT("no room left in the start universe"), Describe(StageDMXPatching::FindFree({}, 21, MakePatch(1, 500))), FString(TEXT("2.001")));
	TestEqual(TEXT("from universe 3"), Describe(StageDMXPatching::FindFree({ MakeSlot(0, 1, 1, 21) }, 21, MakePatch(3, 1))), FString(TEXT("3.001")));

	// Unpatched and footprint-less slots occupy nothing; impossible footprints find nothing.
	TestEqual(TEXT("unpatched occupies nothing"), Describe(StageDMXPatching::FindFree({ MakeSlot(0, 0, 0, 21), MakeSlot(1, 1, 1, 0) }, 21, Origin)), FString(TEXT("1.001")));
	TestFalse(TEXT("footprint 0"), StageDMXPatching::FindFree({}, 0, Origin).IsPatched());
	TestFalse(TEXT("footprint 513"), StageDMXPatching::FindFree({}, 513, Origin).IsPatched());

	// A range that runs past 512 still occupies what it covers.
	TestEqual(TEXT("overflowing range blocks its channels"), Describe(StageDMXPatching::FindFree({ MakeSlot(0, 1, 500, 21) }, 20, MakePatch(1, 495))), FString(TEXT("2.001")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageDMXConflictTest, "StageCraft.DMX.Patching.Conflicts", StageDMXPatchingTests::Flags)

bool FStageDMXConflictTest::RunTest(const FString& Parameters)
{
	using namespace StageDMXPatchingTests;

	TestEqual(TEXT("overlap"), StageDMXPatching::FindConflicts({ MakeSlot(1, 1, 1, 21), MakeSlot(2, 1, 10, 21) }), TArray<int32>{ 1, 2 });
	TestEqual(TEXT("adjacent is fine"), StageDMXPatching::FindConflicts({ MakeSlot(1, 1, 1, 21), MakeSlot(2, 1, 22, 21) }).Num(), 0);
	TestEqual(TEXT("other universe is fine"), StageDMXPatching::FindConflicts({ MakeSlot(1, 1, 1, 21), MakeSlot(2, 2, 1, 21) }).Num(), 0);
	TestEqual(TEXT("past 512 alone"), StageDMXPatching::FindConflicts({ MakeSlot(7, 1, 500, 21) }), TArray<int32>{ 7 });
	TestEqual(TEXT("nested ranges all conflict"),
		StageDMXPatching::FindConflicts({ MakeSlot(3, 1, 50, 11), MakeSlot(1, 1, 1, 100), MakeSlot(2, 1, 10, 11) }), TArray<int32>{ 1, 2, 3 });
	TestEqual(TEXT("chained overlaps"),
		StageDMXPatching::FindConflicts({ MakeSlot(1, 1, 1, 10), MakeSlot(2, 1, 5, 10), MakeSlot(3, 1, 14, 5), MakeSlot(4, 1, 40, 5) }), TArray<int32>{ 1, 2, 3 });
	TestEqual(TEXT("unpatched never conflicts"), StageDMXPatching::FindConflicts({ MakeSlot(1, 0, 0, 21), MakeSlot(2, 0, 0, 21) }).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageDMXPlanTest, "StageCraft.DMX.Patching.Plans", StageDMXPatchingTests::Flags)

bool FStageDMXPlanTest::RunTest(const FString& Parameters)
{
	using namespace StageDMXPatchingTests;

	// Consecutive, around a fixture that stays.
	const TArray<FStageDMXPatchSlot> Moving = { MakeSlot(10, 0, 0, 21), MakeSlot(11, 0, 0, 0), MakeSlot(12, 0, 0, 21), MakeSlot(13, 0, 0, 21) };
	TArray<FStageDMXPatchPlanEntry> Plan = StageDMXPatching::PlanConsecutive({ MakeSlot(0, 1, 1, 21) }, Moving, MakePatch(1, 1));
	if (TestEqual(TEXT("three planned, the footprint-less one skipped"), Plan.Num(), 3))
	{
		TestEqual(TEXT("first after the fixed one"), Describe(Plan[0].Patch), FString(TEXT("1.022")));
		TestEqual(TEXT("then packed"), Describe(Plan[1].Patch), FString(TEXT("1.043")));
		TestEqual(TEXT("key kept"), Plan[2].Key, 13);
	}

	// Over a universe boundary.
	Plan = StageDMXPatching::PlanConsecutive({}, { MakeSlot(1, 0, 0, 21), MakeSlot(2, 0, 0, 21) }, MakePatch(1, 492));
	if (TestEqual(TEXT("two planned"), Plan.Num(), 2))
	{
		TestEqual(TEXT("last fit of universe 1"), Describe(Plan[0].Patch), FString(TEXT("1.492")));
		TestEqual(TEXT("next universe"), Describe(Plan[1].Patch), FString(TEXT("2.001")));
	}

	// All or nothing.
	TestEqual(TEXT("an impossible footprint plans nothing"), StageDMXPatching::PlanConsecutive({}, { MakeSlot(1, 0, 0, 21), MakeSlot(2, 0, 0, 600) }, MakePatch(1, 1)).Num(), 0);

	// Conflict repair: the first in each overlap stays (lower key on a tie), the rest move to free addresses; overflow moves.
	const TArray<FStageDMXPatchSlot> Broken = { MakeSlot(0, 1, 1, 21), MakeSlot(1, 1, 10, 21), MakeSlot(2, 1, 1, 21), MakeSlot(3, 1, 500, 21), MakeSlot(4, 2, 1, 21) };
	Plan = StageDMXPatching::PlanConflictRepair(Broken);
	TArray<FStageDMXPatchSlot> Repaired = Broken;
	for (const FStageDMXPatchPlanEntry& Entry : Plan)
	{
		TestTrue(TEXT("never moves the first one"), Entry.Key != 0);
		TestTrue(TEXT("never moves the fine one in universe 2"), Entry.Key != 4);
		Repaired[Entry.Key].Patch = Entry.Patch;
	}
	TestEqual(TEXT("three moved"), Plan.Num(), 3);
	TestEqual(TEXT("no conflict left"), StageDMXPatching::FindConflicts(Repaired).Num(), 0);
	TestEqual(TEXT("nothing to repair"), StageDMXPatching::PlanConflictRepair(Repaired).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageDMXUsageTest, "StageCraft.DMX.Patching.Usage", StageDMXPatchingTests::Flags)

bool FStageDMXUsageTest::RunTest(const FString& Parameters)
{
	using namespace StageDMXPatchingTests;

	const TArray<FStageDMXUniverseUsage> Usage = StageDMXPatching::SummarizeUsage({ MakeSlot(1, 2, 1, 512), MakeSlot(2, 1, 1, 21), MakeSlot(3, 1, 10, 21), MakeSlot(4, 0, 0, 21) });
	if (TestEqual(TEXT("two universes"), Usage.Num(), 2))
	{
		TestEqual(TEXT("ascending"), Usage[0].Universe, 1);
		TestEqual(TEXT("overlap counted once"), Usage[0].UsedChannels, 30);
		TestEqual(TEXT("fixtures"), Usage[0].Fixtures, 2);
		TestEqual(TEXT("conflicts"), Usage[0].Conflicts, 2);
		TestEqual(TEXT("full universe"), Usage[1].UsedChannels, 512);
		TestEqual(TEXT("no conflict there"), Usage[1].Conflicts, 0);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
