// Copyright Epic Games, Inc. All Rights Reserved.

// Unit tests for the shopping and assembly list of Docs/ADR/0014-shopping-and-assembly-list.md: rule sanitizing, route
// measuring, stock-length assembly with the safety margin, rigging classification, clamps and safety bonds, distro counting,
// and the list in the cable schedule's CSV and PDF. No world.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Report.ShoppingList; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Algo/Reverse.h"
#include "Misc/AutomationTest.h"
#include "Misc/DateTime.h"
#include "Report/StageCableSchedule.h"
#include "Report/StageShoppingList.h"
#include "Report/Tests/StageReportTestUtils.h"

namespace StageShoppingListTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;
	constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

	const FDateTime When(2026, 10, 21, 18, 0, 0);

	FStagePowerLoad MakeLoad(const TCHAR* Label, double Watts, const FVector& Location)
	{
		FStagePowerLoad Load;
		Load.Label = Label;
		Load.ItemType = TEXT("Spot 300");
		Load.Watts = Watts;
		Load.LocationMeters = Location;
		return Load;
	}

	FStageRiggedFixture MakeFixture(int32 FixtureId, double WeightKg, const FVector& Location, double UpZ = 1.0)
	{
		FStageRiggedFixture Fixture;
		Fixture.FixtureId = FixtureId;
		Fixture.Label = FString::Printf(TEXT("Fixture %d"), FixtureId);
		Fixture.WeightKg = WeightKg;
		Fixture.LocationMeters = Location;
		Fixture.UpZ = UpZ;
		return Fixture;
	}

	/** Quantity of the line with this section and size, or -1 when there is none. */
	int32 FindQuantity(const FStageShoppingList& List, EStageShoppingSection Section, const FString& Size)
	{
		const FStageShoppingLine* Line = List.Lines.FindByPredicate([Section, &Size](const FStageShoppingLine& Candidate)
		{
			return Candidate.Section == Section && Candidate.Size == Size;
		});
		return Line ? Line->Quantity : -1;
	}

	FString Assemble(double RouteMeters, const FStageCablingRules& Rules = FStageCablingRules())
	{
		return StageShoppingList::FormatAssembly(StageShoppingList::AssembleRun(RouteMeters, Rules));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageShoppingListRulesTest, "StageCraft.Report.ShoppingList.Rules", StageShoppingListTests::Flags)

bool FStageShoppingListRulesTest::RunTest(const FString& Parameters)
{
	using namespace StageShoppingListTests;

	// Sanitizing: every value into its range; bad list entries dropped; empty lists and names take the defaults.
	FStageCablingRules Broken;
	Broken.SafetyMargin = NaN;
	Broken.StockLengthsMeters = { NaN, -1.0, 5.0, 2.0, 5.0, 1000.0 };
	Broken.PowerCableName = TEXT("   ");
	Broken.DistroOutputs = 0;
	Broken.DistroFeedAmps = -10.0;
	Broken.DistroLocationMeters = FVector(NaN, 1.0e9, -3.0);
	Broken.WeightPerClampKg = 0.0;
	Broken.SafetyBondRatingsKg = { NaN, 0.0 };
	const FStageCablingRules Safe = StageShoppingList::Sanitize(Broken);
	const FStageCablingRules Defaults;
	TestEqual(TEXT("NaN margin takes the default"), Safe.SafetyMargin, 0.1);
	TestTrue(TEXT("Stock: bad entries dropped, sorted, unique"), Safe.StockLengthsMeters == TArray<double>({ 2.0, 5.0 }));
	TestEqual(TEXT("Blank cable name takes the default"), Safe.PowerCableName, Defaults.PowerCableName);
	TestEqual(TEXT("At least one distro output"), Safe.DistroOutputs, 1);
	TestEqual(TEXT("Feed at least 1 A"), Safe.DistroFeedAmps, 1.0);
	TestTrue(TEXT("Location: NaN reads 0, far values clamp to 10 km"), Safe.DistroLocationMeters.Equals(FVector(0.0, 10000.0, -3.0)));
	TestEqual(TEXT("At least 1 kg per clamp"), Safe.WeightPerClampKg, 1.0);
	TestTrue(TEXT("No usable bond rating: the defaults"), Safe.SafetyBondRatingsKg == Defaults.SafetyBondRatingsKg);

	// Routes run along floor and truss: across, then up.
	TestEqual(TEXT("Route: horizontal + vertical"), StageShoppingList::GetRouteMeters(FVector::ZeroVector, FVector(3.0, 4.0, 6.0)), 11.0, 1e-9);
	TestEqual(TEXT("Route: NaN reads 0"), StageShoppingList::GetRouteMeters(FVector(NaN, 0.0, 0.0), FVector(0.0, 0.0, 2.0)), 2.0, 1e-9);

	// Stock assembly with the default +10 % and 2/5/10/15/20 m.
	TestEqual(TEXT("Nothing to bridge: the shortest cable"), Assemble(0.0), FString(TEXT("2 m")));
	TestEqual(TEXT("NaN route: the shortest cable"), Assemble(NaN), FString(TEXT("2 m")));
	TestEqual(TEXT("Exactly 2 m with the margin fits a 2 m cable"), Assemble(2.0 / 1.1), FString(TEXT("2 m")));
	TestEqual(TEXT("Just over 5 m with the margin takes 10 m"), Assemble(4.6), FString(TEXT("10 m")));
	TestEqual(TEXT("Exactly 20 m with the margin is one cable"), Assemble(20.0 / 1.1), FString(TEXT("20 m")));
	TestEqual(TEXT("27.5 m: a 20 m and a 10 m"), Assemble(25.0), FString(TEXT("20 m + 10 m")));
	TestEqual(TEXT("Exactly 40 m: two 20 m, no tail"), Assemble(40.0 / 1.1), FString(TEXT("2 x 20 m")));
	TestEqual(TEXT("A rest above the second-longest stock is one more full length"), Assemble(35.0 / 1.1 + 0.5), FString(TEXT("2 x 20 m")));
	const FStageCableAssembly Joined = StageShoppingList::AssembleRun(25.0, FStageCablingRules());
	TestTrue(TEXT("Joined: two cables, 30 m"), Joined.IsJoined() && Joined.GetCableCount() == 2 && Joined.GetTotalMeters() == 30.0);

	FStageCablingRules NoMargin;
	NoMargin.SafetyMargin = 0.0;
	TestEqual(TEXT("No margin: 5 m takes 5 m"), Assemble(5.0, NoMargin), FString(TEXT("5 m")));
	FStageCablingRules ShortStock;
	ShortStock.StockLengthsMeters = { 0.5, 2.5 };
	TestEqual(TEXT("Half-metre stock prints its decimal"), Assemble(0.3, ShortStock), FString(TEXT("0.5 m")));
	TestEqual(TEXT("Long run on short stock"), Assemble(100.0, ShortStock), FString(TEXT("44 x 2.5 m")));

	// Rigging: high or tilted fixtures are rigged; an upright fixture on the floor or a deck is not.
	FStageCablingRules Rules;
	TestFalse(TEXT("Upright at 1 m: standing"), StageShoppingList::IsRigged(MakeFixture(1, 20.0, FVector(0.0, 0.0, 1.0)), Rules));
	TestTrue(TEXT("At the rigging height: rigged"), StageShoppingList::IsRigged(MakeFixture(1, 20.0, FVector(0.0, 0.0, 2.0)), Rules));
	TestTrue(TEXT("Upside down low: rigged"), StageShoppingList::IsRigged(MakeFixture(1, 20.0, FVector(0.0, 0.0, 0.5), -1.0), Rules));
	TestTrue(TEXT("On its side: rigged"), StageShoppingList::IsRigged(MakeFixture(1, 20.0, FVector::ZeroVector, 0.0), Rules));
	TestFalse(TEXT("NaN up vector reads upright"), StageShoppingList::IsRigged(MakeFixture(1, 20.0, FVector::ZeroVector, NaN), Rules));

	TestEqual(TEXT("Clamps: no weight data takes one"), StageShoppingList::GetClampCount(0.0, Rules), 1);
	TestEqual(TEXT("Clamps: NaN takes one"), StageShoppingList::GetClampCount(NaN, Rules), 1);
	TestEqual(TEXT("Clamps: exactly 20 kg takes one"), StageShoppingList::GetClampCount(20.0, Rules), 1);
	TestEqual(TEXT("Clamps: 20.5 kg takes two"), StageShoppingList::GetClampCount(20.5, Rules), 2);
	TestEqual(TEXT("Clamps: 45 kg takes three"), StageShoppingList::GetClampCount(45.0, Rules), 3);

	TestEqual(TEXT("Bond: no weight data takes the lightest"), StageShoppingList::PickSafetyBond(0.0, Rules), 10.0);
	TestEqual(TEXT("Bond: exactly its rating"), StageShoppingList::PickSafetyBond(10.0, Rules), 10.0);
	TestEqual(TEXT("Bond: just over takes the next"), StageShoppingList::PickSafetyBond(10.1, Rules), 25.0);
	TestEqual(TEXT("Bond: heavier than every bond"), StageShoppingList::PickSafetyBond(150.0, Rules), -1.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageShoppingListBuildTest, "StageCraft.Report.ShoppingList.Build", StageShoppingListTests::Flags)

bool FStageShoppingListBuildTest::RunTest(const FString& Parameters)
{
	using namespace StageShoppingListTests;
	using StageReportTestUtils::BytesToLatin1;
	using StageReportTestUtils::VerifyPdfStructure;

	FStageCablingRules Rules;
	Rules.DistroAnchor = EStageCableAnchor::Fixed;
	Rules.DistroLocationMeters = FVector::ZeroVector;

	const TArray<FStagePowerLoad> Loads = {
		MakeLoad(TEXT("Truss spot"), 1000.0, FVector(10.0, 0.0, 6.0)),		// 10 + 6 = 16 m, +10 % = 17.6: 20 m
		MakeLoad(TEXT("Floor spot"), 1000.0, FVector(3.0, 4.0, 0.0)),		// 5 m, 5.5: 10 m
		MakeLoad(TEXT("At the distro"), 1000.0, FVector::ZeroVector),		// 0 m: 2 m
	};
	const TArray<FStageRiggedFixture> Fixtures = {
		MakeFixture(1, 30.0, FVector(0.0, 0.0, 6.0)),			// 2 clamps, 50 kg bond
		MakeFixture(2, 8.0, FVector(0.0, 0.0, 6.0)),			// 1 clamp, 10 kg bond
		MakeFixture(3, 25.0, FVector(0.0, 0.0, 0.0)),			// standing: nothing
		MakeFixture(4, 150.0, FVector(0.0, 0.0, 1.0), -1.0),	// upside down: 8 clamps, no bond strong enough
		MakeFixture(5, 0.0, FVector(0.0, 0.0, 5.0)),			// no weight data: 1 clamp, 10 kg bond
	};
	const FStageCableSchedule Plan = StageCableSchedule::Build(Loads, {}, Fixtures, FStagePowerDistribution(), Rules, TEXT("Arena"), When);
	const FStageShoppingList& List = Plan.Shopping;

	TestEqual(TEXT("Power cable 2 m"), FindQuantity(List, EStageShoppingSection::PowerCables, TEXT("2 m")), 1);
	TestEqual(TEXT("Power cable 10 m"), FindQuantity(List, EStageShoppingSection::PowerCables, TEXT("10 m")), 1);
	TestEqual(TEXT("Power cable 20 m"), FindQuantity(List, EStageShoppingSection::PowerCables, TEXT("20 m")), 1);
	TestEqual(TEXT("No 5 m power cable"), FindQuantity(List, EStageShoppingSection::PowerCables, TEXT("5 m")), -1);
	TestTrue(TEXT("Cable lines name type and length"), List.Lines[0].Item == TEXT("PowerCON TRUE1 2 m") && List.Lines[0].Section == EStageShoppingSection::PowerCables);
	TestTrue(TEXT("Power cable totals"), List.PowerCableCount == 3 && List.PowerCableMeters == 32.0);
	TestEqual(TEXT("One distro for three circuits"), FindQuantity(List, EStageShoppingSection::Power, TEXT("12 x 16 A out, 63 A 3-phase feed")), 1);
	TestEqual(TEXT("No DMX without patched fixtures"), List.DmxCableCount, 0);

	TestEqual(TEXT("Four rigged fixtures"), List.RiggedFixtures, 4);
	TestEqual(TEXT("Rigged weight counts known weights"), List.RiggedWeightKg, 188.0, 1e-9);
	TestEqual(TEXT("Clamps: 2 + 1 + 8 + 1"), FindQuantity(List, EStageShoppingSection::Rigging, TEXT("1 per 20 kg of fixture")), 12);
	TestEqual(TEXT("Two 10 kg bonds"), FindQuantity(List, EStageShoppingSection::Rigging, TEXT("WLL 10 kg")), 2);
	TestEqual(TEXT("One 50 kg bond"), FindQuantity(List, EStageShoppingSection::Rigging, TEXT("WLL 50 kg")), 1);
	TestEqual(TEXT("No 25 kg bond"), FindQuantity(List, EStageShoppingSection::Rigging, TEXT("WLL 25 kg")), -1);
	const FStageShoppingLine& Last = List.Lines.Last();
	TestTrue(TEXT("The overweight fixture is listed last and flagged"), Last.Size == TEXT("above WLL 100 kg") && Last.Quantity == 1 && Last.bNeedsAttention);
	TestTrue(TEXT("Counted: overweight and missing weight"), List.OverweightFixtures == 1 && List.FixturesWithoutWeight == 1);

	const TArray<FString> Warnings = StageCableSchedule::GetWarnings(Plan);
	TestTrue(TEXT("Warns about the overweight fixture"), Warnings.ContainsByPredicate([](const FString& Warning) { return Warning.Contains(TEXT("secondary suspension")); }));
	TestTrue(TEXT("Warns about the missing weight"), Warnings.ContainsByPredicate([](const FString& Warning) { return Warning.Contains(TEXT("without weight data")); }));

	// Determinism: the same fixtures in another order give the same list.
	TArray<FStageRiggedFixture> Reversed = Fixtures;
	Algo::Reverse(Reversed);
	const FStageCableSchedule Again = StageCableSchedule::Build(Loads, {}, Reversed, FStagePowerDistribution(), Rules, TEXT("Arena"), When);
	bool bSame = Again.Shopping.Lines.Num() == List.Lines.Num();
	for (int32 Index = 0; bSame && Index < List.Lines.Num(); ++Index)
	{
		bSame = Again.Shopping.Lines[Index].Item == List.Lines[Index].Item && Again.Shopping.Lines[Index].Size == List.Lines[Index].Size
			&& Again.Shopping.Lines[Index].Quantity == List.Lines[Index].Quantity;
	}
	TestTrue(TEXT("Deterministic: same list in any input order"), bSame);

	// Distros: outputs and the feed both limit a distro.
	{
		TArray<FStagePowerLoad> Six;
		for (int32 Index = 1; Index <= 6; ++Index)
		{
			Six.Add(MakeLoad(*FString::Printf(TEXT("Load %d"), Index), 1000.0, FVector::ZeroVector));
		}
		FStageCablingRules TwoOutputs;
		TwoOutputs.DistroOutputs = 2;
		const FStageCableSchedule ByOutputs = StageCableSchedule::Build(Six, {}, {}, FStagePowerDistribution(), TwoOutputs, TEXT("Arena"), When);
		TestEqual(TEXT("Three circuits on two-way distros: two distros"), ByOutputs.Distros.Num(), 2);
		TestTrue(TEXT("Circuits 1 and 2 on PD1, 3 on PD2"), ByOutputs.Circuits.Num() == 3 && ByOutputs.Circuits[1].DistroNumber == 1 && ByOutputs.Circuits[2].DistroNumber == 2);
		TestEqual(TEXT("Cables start at their distro"), ByOutputs.PowerCables.Last().From.Left(4), FString(TEXT("PD2 ")));

		FStageCablingRules SmallFeed;
		SmallFeed.DistroFeedAmps = 5.0;
		const FStageCableSchedule ByFeed = StageCableSchedule::Build(Six, {}, {}, FStagePowerDistribution(), SmallFeed, TEXT("Arena"), When);
		TestEqual(TEXT("Circuits of 8.7 A on a 5 A feed: one distro each"), ByFeed.Distros.Num(), 3);
	}

	// Automatic distro position: the centre of the loads, on the floor.
	{
		const FStageCableSchedule Automatic = StageCableSchedule::Build(Loads, {}, {}, FStagePowerDistribution(), FStageCablingRules(), TEXT("Arena"), When);
		TestTrue(TEXT("Automatic distro position"), Automatic.DistroLocationMeters.Equals(FVector(13.0 / 3.0, 4.0 / 3.0, 0.0), 1e-9));
		TestTrue(TEXT("Automatic: no rigged fixtures, no rigging lines"), !Automatic.Shopping.Lines.ContainsByPredicate([](const FStageShoppingLine& Line)
		{
			return Line.Section == EStageShoppingSection::Rigging;
		}));
	}

	// Exports.
	const FString Csv = StageCableSchedule::ToCsv(Plan);
	TestTrue(TEXT("CSV: shopping header"), Csv.Contains(TEXT("\r\nSection,Item,Size,Quantity,Note\r\n")));
	TestTrue(TEXT("CSV: a cable line"), Csv.Contains(TEXT("\r\nPower cables,PowerCON TRUE1 20 m,20 m,1,\r\n")));
	TestTrue(TEXT("CSV: the bond line"), Csv.Contains(TEXT("\r\nRigging,Safety bond,WLL 10 kg,2,\r\n")));
	TestTrue(TEXT("CSV: cable totals"), Csv.Contains(TEXT("\r\nTotal,Power cable,32.0 m,3,\r\n")));
	TestTrue(TEXT("CSV: the power cable row shows route and cables"), Csv.Contains(TEXT(",16.0,20 m,\r\n")));

	const TArray<uint8> Pdf = StageCableSchedule::ToPdf(Plan);
	TestTrue(TEXT("PDF: structure"), VerifyPdfStructure(*this, Pdf, TEXT("Shopping list PDF")));
	const FString Text = BytesToLatin1(Pdf);
	TestTrue(TEXT("PDF: the section"), Text.Contains(TEXT("(Shopping & assembly list) Tj")));
	TestTrue(TEXT("PDF: a cable line with its quantity"), Text.Contains(TEXT("(PowerCON TRUE1 20 m) Tj")) && Text.Contains(TEXT("(12x) Tj")));

	// Nothing on the stage: an empty list that still prints.
	const FStageCableSchedule Empty = StageCableSchedule::Build({}, {}, {}, FStagePowerDistribution(), FStageCablingRules(), FString(), When);
	TestTrue(TEXT("Empty: no lines"), Empty.Shopping.IsEmpty() && Empty.Distros.IsEmpty());
	TestTrue(TEXT("Empty PDF: says so"), BytesToLatin1(StageCableSchedule::ToPdf(Empty)).Contains(TEXT("(Nothing on the stage needs cables or rigging hardware.) Tj")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
