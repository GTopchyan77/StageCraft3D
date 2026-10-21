// Copyright Epic Games, Inc. All Rights Reserved.

// Unit tests for the production reports of Docs/ADR/0012-production-reports-and-fixture-benchmark.md: the cable schedule and
// power plan (supply sanitizing, phase balancing, circuit packing, overloads, DMX chains, warnings, CSV, PDF), the bill of
// materials PDF, and the shared PDF composer (page breaks with repeated table headers). No world.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Report; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Algo/Reverse.h"
#include "Misc/AutomationTest.h"
#include "Misc/DateTime.h"
#include "Report/StageBillOfMaterials.h"
#include "Report/StageCableSchedule.h"
#include "Report/StagePdfWriter.h"
#include "Report/StageReportPdfComposer.h"
#include "Report/Tests/StageReportTestUtils.h"

namespace StageProductionReportTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	using StageReportTestUtils::BytesToLatin1;
	using StageReportTestUtils::CountOccurrences;
	using StageReportTestUtils::VerifyPdfStructure;

	const FDateTime When(2026, 10, 21, 16, 30, 0);

	FStagePowerLoad MakeLoad(const TCHAR* Label, double Watts, int32 FixtureId = 0)
	{
		FStagePowerLoad Load;
		Load.Label = Label;
		Load.ItemType = FixtureId > 0 ? TEXT("Spot 300") : TEXT("Amplifier");
		Load.FixtureId = FixtureId;
		Load.Watts = Watts;
		return Load;
	}

	FStageDmxNode MakeNode(int32 FixtureId, const TCHAR* Label, int32 Universe, int32 Address, const FVector& Location)
	{
		FStageDmxNode Node;
		Node.FixtureId = FixtureId;
		Node.Label = Label;
		Node.Universe = Universe;
		Node.Address = Address;
		Node.Footprint = 16;
		Node.LocationMeters = Location;
		return Node;
	}

	/** A plan with no rigged fixtures and the default cabling rules, for the tests that are about power and DMX. */
	FStageCableSchedule BuildPlan(TArray<FStagePowerLoad> Loads, TArray<FStageDmxNode> Nodes, const FStagePowerDistribution& Supply, const FString& SceneName,
		const FDateTime& GeneratedAt, const FStageCablingRules& Cabling = FStageCablingRules())
	{
		return StageCableSchedule::Build(MoveTemp(Loads), MoveTemp(Nodes), {}, Supply, Cabling, SceneName, GeneratedAt);
	}

	/** Circuit number of the load called Label, or 0 when it is not in the plan. */
	int32 FindCircuitOf(const FStageCableSchedule& Schedule, const FString& Label)
	{
		for (const FStagePlannedLoad& Planned : Schedule.Loads)
		{
			if (Planned.Load.Label == Label)
			{
				return Schedule.Circuits[Planned.CircuitIndex].Number;
			}
		}
		return 0;
	}

	int32 GetPdfPageCount(const FString& PdfText)
	{
		const int32 CountPos = PdfText.Find(TEXT("/Type /Pages /Kids ["));
		const int32 Count = CountPos == INDEX_NONE ? INDEX_NONE : PdfText.Find(TEXT("/Count "), ESearchCase::CaseSensitive, ESearchDir::FromStart, CountPos);
		return Count == INDEX_NONE ? 0 : FCString::Atoi(*PdfText.Mid(Count + 7, 6));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageCableSchedulePowerTest, "StageCraft.Report.CableSchedule.Power", StageProductionReportTests::Flags)

bool FStageCableSchedulePowerTest::RunTest(const FString& Parameters)
{
	using namespace StageProductionReportTests;

	// Supply: every value clamped into its range, non-finite values take the default.
	FStagePowerDistribution Broken;
	Broken.PhaseVoltage = std::numeric_limits<double>::quiet_NaN();
	Broken.PhaseCount = 2;
	Broken.CircuitRatingAmps = 0.0;
	Broken.UsableShare = 5.0;
	Broken.PowerFactor = 0.0;
	const FStagePowerDistribution Safe = StageCableSchedule::Sanitize(Broken);
	TestEqual(TEXT("NaN voltage takes the default"), Safe.PhaseVoltage, 230.0);
	TestEqual(TEXT("Two phases read three"), Safe.PhaseCount, 3);
	TestEqual(TEXT("Rating at least 1 A"), Safe.CircuitRatingAmps, 1.0);
	TestEqual(TEXT("Usable share at most 100 %"), Safe.UsableShare, 1.0);
	TestEqual(TEXT("Power factor at least 0.5"), Safe.PowerFactor, 0.5);
	TestEqual(TEXT("Default usable power: 230 V x 16 A x 80 %"), StageCableSchedule::GetUsableWattsPerCircuit(FStagePowerDistribution()), 2944.0, 1e-9);

	// Six equal loads on three phases: two per phase on one circuit each, perfectly balanced.
	{
		TArray<FStagePowerLoad> Loads;
		for (int32 Index = 1; Index <= 6; ++Index)
		{
			Loads.Add(MakeLoad(*FString::Printf(TEXT("Load %d"), Index), 1000.0));
		}
		const FStageCableSchedule Plan = BuildPlan(Loads, {}, FStagePowerDistribution(), TEXT("Arena"), When);
		TestEqual(TEXT("Balanced: three circuits"), Plan.Circuits.Num(), 3);
		TestEqual(TEXT("Balanced: three phases"), Plan.Phases.Num(), 3);
		for (const FStagePowerPhase& Phase : Plan.Phases)
		{
			TestEqual(*FString::Printf(TEXT("Balanced: L%d carries 2000 W"), Phase.Phase), Phase.Watts, 2000.0, 1e-9);
			TestEqual(*FString::Printf(TEXT("Balanced: L%d current"), Phase.Phase), Phase.Amps, 2000.0 / 230.0, 1e-9);
		}
		TestEqual(TEXT("Balanced: no imbalance"), Plan.ImbalancePercent, 0.0, 1e-9);
		TestEqual(TEXT("Balanced: total"), Plan.TotalWatts, 6000.0, 1e-9);
		TestEqual(TEXT("Balanced: loads 1 and 4 share circuit 1"), FindCircuitOf(Plan, TEXT("Load 4")), FindCircuitOf(Plan, TEXT("Load 1")));
		TestEqual(TEXT("Balanced: one power cable per load"), Plan.PowerCables.Num(), 6);
		TestEqual(TEXT("Balanced: first cable id"), Plan.PowerCables[0].CableId, FString(TEXT("P-001")));
		TestEqual(TEXT("Balanced: cable starts at its circuit"), Plan.PowerCables[0].From, FString(TEXT("PD1 C01 / L1")));
		TestTrue(TEXT("Balanced: no warnings"), StageCableSchedule::GetWarnings(Plan).IsEmpty());
	}

	// Single phase, circuit packing: 1400 W loads fit two to a 2944 W circuit, never three.
	{
		FStagePowerDistribution Single;
		Single.PhaseCount = 1;
		const FStageCableSchedule Plan = BuildPlan({ MakeLoad(TEXT("A"), 1400.0), MakeLoad(TEXT("B"), 1400.0), MakeLoad(TEXT("C"), 1400.0),
			MakeLoad(TEXT("D"), 1400.0), MakeLoad(TEXT("E"), 1400.0) }, {}, Single, TEXT("Club"), When);
		TestEqual(TEXT("Packing: one phase"), Plan.Phases.Num(), 1);
		TestEqual(TEXT("Packing: three circuits for five loads"), Plan.Circuits.Num(), 3);
		TestEqual(TEXT("Packing: circuit 1 holds two"), Plan.Circuits[0].Loads.Num(), 2);
		TestEqual(TEXT("Packing: circuit 3 holds the last"), Plan.Circuits[2].Loads.Num(), 1);
		TestEqual(TEXT("Packing: single phase has no imbalance"), Plan.ImbalancePercent, 0.0);
		TestTrue(TEXT("Packing: no circuit above its usable share"), Plan.Circuits[0].Watts <= StageCableSchedule::GetUsableWattsPerCircuit(Single));
		TestEqual(TEXT("Packing: rating share"), Plan.Circuits[0].RatingShare, 2800.0 / 230.0 / 16.0, 1e-9);
	}

	// A load exactly at the usable share fits; one above it gets its own circuit, flagged, never split.
	{
		const FStageCableSchedule Plan = BuildPlan({ MakeLoad(TEXT("LED wall"), 5000.0), MakeLoad(TEXT("Exact"), 2944.0), MakeLoad(TEXT("Small"), 100.0) },
			{}, FStagePowerDistribution(), TEXT("Arena"), When);
		TestEqual(TEXT("Overload: one overloaded circuit"), Plan.OverloadedCircuits, 1);
		TestTrue(TEXT("Overload: the largest load is first, alone and flagged"), Plan.Circuits[0].bOverloaded && Plan.Circuits[0].Loads.Num() == 1 && Plan.Loads[0].Load.Label == TEXT("LED wall"));
		TestFalse(TEXT("Overload: a load at exactly the usable share is not an overload"), Plan.Circuits[1].bOverloaded);
		TestEqual(TEXT("Overload: the small load goes to the empty phase"), Plan.Circuits[FindCircuitOf(Plan, TEXT("Small")) - 1].Phase, 3);
		const TArray<FString> Warnings = StageCableSchedule::GetWarnings(Plan);
		TestTrue(TEXT("Overload: warned first"), !Warnings.IsEmpty() && Warnings[0].Contains(TEXT("larger feed")));
		TestTrue(TEXT("Overload: imbalance warned too"), Warnings.ContainsByPredicate([](const FString& Warning) { return Warning.Contains(TEXT("imbalance")); }));
	}

	// Bad data: NaN and zero loads draw nothing; a lighting fixture without power data is counted.
	{
		const FStageCableSchedule Plan = BuildPlan({ MakeLoad(TEXT("NaN"), std::numeric_limits<double>::quiet_NaN()), MakeLoad(TEXT("Truss"), 0.0),
			MakeLoad(TEXT("Dark spot"), 0.0, 7), MakeLoad(TEXT("Negative"), -50.0), MakeLoad(TEXT("Real"), 500.0) }, {}, FStagePowerDistribution(), TEXT("Arena"), When);
		TestEqual(TEXT("Bad data: one real load"), Plan.Loads.Num(), 1);
		TestEqual(TEXT("Bad data: the fixture without power data is counted"), Plan.FixturesWithoutPowerData, 1);
		TestTrue(TEXT("Bad data: total is finite"), FMath::IsFinite(Plan.TotalWatts) && Plan.TotalWatts == 500.0);
	}

	// Determinism: the same loads in another order give the same plan.
	{
		TArray<FStagePowerLoad> Loads = { MakeLoad(TEXT("Spot 10"), 700.0, 10), MakeLoad(TEXT("Spot 2"), 700.0, 2), MakeLoad(TEXT("Amp"), 1800.0),
			MakeLoad(TEXT("Wash"), 450.0, 3), MakeLoad(TEXT("Hazer"), 1200.0), MakeLoad(TEXT("Spot 1"), 700.0, 1) };
		const FStageCableSchedule First = BuildPlan(Loads, {}, FStagePowerDistribution(), TEXT("Arena"), When);
		Algo::Reverse(Loads);
		Swap(Loads[0], Loads[3]);
		const FStageCableSchedule Second = BuildPlan(Loads, {}, FStagePowerDistribution(), TEXT("Arena"), When);
		bool bSame = First.Loads.Num() == Second.Loads.Num();
		for (int32 Index = 0; bSame && Index < First.Loads.Num(); ++Index)
		{
			bSame = First.Loads[Index].Load.Label == Second.Loads[Index].Load.Label && First.Loads[Index].CircuitIndex == Second.Loads[Index].CircuitIndex;
		}
		TestTrue(TEXT("Deterministic: same loads, same circuits, any input order"), bSame);
		// Equal loads are placed in natural label order: Spot 1 and Spot 2 fill L3 (C03) before Spot 10 tips the balance to L2.
		// Plain string order would place "Spot 10" before "Spot 2".
		TestEqual(TEXT("Deterministic: equal loads in natural label order"), FindCircuitOf(First, TEXT("Spot 2")), FindCircuitOf(First, TEXT("Spot 1")));
		TestNotEqual(TEXT("Deterministic: Spot 10 goes to the next phase"), FindCircuitOf(First, TEXT("Spot 10")), FindCircuitOf(First, TEXT("Spot 1")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageCableScheduleDmxTest, "StageCraft.Report.CableSchedule.Dmx", StageProductionReportTests::Flags)

bool FStageCableScheduleDmxTest::RunTest(const FString& Parameters)
{
	using namespace StageProductionReportTests;

	// The control node at the origin, so every length below can be worked out by hand.
	FStageCablingRules Cabling;
	Cabling.ControlAnchor = EStageCableAnchor::Fixed;
	Cabling.ControlLocationMeters = FVector::ZeroVector;

	const FStageCableSchedule Plan = BuildPlan({}, {
		MakeNode(3, TEXT("Spot C"), 1, 33, FVector(3.0, 4.0, 0.0)),
		MakeNode(1, TEXT("Spot A"), 1, 1, FVector::ZeroVector),
		MakeNode(2, TEXT("Spot B"), 1, 17, FVector(3.0, 4.0, 0.0)),
		MakeNode(4, TEXT(""), 2, 1, FVector(100.0, 0.0, 0.0)),
		MakeNode(5, TEXT("Far"), 2, 17, FVector(160.0, 0.0, 0.0)),
		MakeNode(6, TEXT("Unpatched"), 0, 0, FVector::ZeroVector),
		MakeNode(7, TEXT("Beyond 512"), 1, 600, FVector(std::numeric_limits<double>::infinity(), 0.0, 0.0)),
	}, FStagePowerDistribution(), TEXT("Arena"), When, Cabling);

	TestEqual(TEXT("Two unpatched fixtures are counted, not chained"), Plan.UnpatchedFixtures, 2);
	if (!TestEqual(TEXT("Five links"), Plan.DmxCables.Num(), 5))
	{
		return false;
	}
	TestTrue(TEXT("Universes 1 and 2"), Plan.Universes == TArray<int32>({ 1, 2 }));

	const FStageCableRun& First = Plan.DmxCables[0];
	TestEqual(TEXT("First link id"), First.CableId, FString(TEXT("D1-01")));
	TestEqual(TEXT("First link starts at the control node"), First.From, FString(TEXT("DMX node out U1")));
	TestEqual(TEXT("First link goes to address 1, named by fixture ID and label"), First.To, FString(TEXT("#1 Spot A")));
	TestTrue(TEXT("Node at the fixture: the shortest stock cable"), First.DistanceMeters == 0.0 && First.Cable.Count == 1 && First.Cable.StockMeters == 2.0);

	const FStageCableRun& Second = Plan.DmxCables[1];
	TestEqual(TEXT("Chain in address order"), Second.From + TEXT(">") + Second.To, FString(TEXT("#1 Spot A>#2 Spot B")));
	TestEqual(TEXT("Route 5 m"), Second.DistanceMeters, 5.0, 1e-9);
	TestEqual(TEXT("5 m + 10 % = 5.5 m takes a 10 m cable"), StageShoppingList::FormatAssembly(Second.Cable), FString(TEXT("10 m")));
	TestFalse(TEXT("Not the end of the universe"), Second.bTerminate);

	const FStageCableRun& Third = Plan.DmxCables[2];
	TestEqual(TEXT("Same position: shortest cable"), StageShoppingList::FormatAssembly(Third.Cable), FString(TEXT("2 m")));
	TestTrue(TEXT("Last of universe 1 is terminated"), Third.bTerminate && Third.To == TEXT("#3 Spot C") && Third.ToAddress == 33);

	const FStageCableRun& Fourth = Plan.DmxCables[3];
	TestEqual(TEXT("Universe 2 restarts numbering"), Fourth.CableId, FString(TEXT("D2-01")));
	TestEqual(TEXT("A fixture without a label reads its ID"), Fourth.To, FString(TEXT("#4")));
	TestEqual(TEXT("100 m + 10 % from the node: five 20 m cables and a 10 m"), StageShoppingList::FormatAssembly(Fourth.Cable), FString(TEXT("5 x 20 m + 10 m")));
	TestEqual(TEXT("60 m + 10 %: three 20 m cables and a 10 m"), StageShoppingList::FormatAssembly(Plan.DmxCables[4].Cable), FString(TEXT("3 x 20 m + 10 m")));
	TestTrue(TEXT("Last of universe 2 is terminated"), Plan.DmxCables[4].bTerminate);
	TestEqual(TEXT("Two joined runs"), Plan.Shopping.JoinedRuns, 2);

	const TArray<FString> Warnings = StageCableSchedule::GetWarnings(Plan);
	TestTrue(TEXT("Warns about unpatched fixtures"), Warnings.ContainsByPredicate([](const FString& Warning) { return Warning.Contains(TEXT("2 lighting fixtures are unpatched")); }));

	// Automatic: the node stands at the centre of the patched fixtures, on the floor.
	const FStageCableSchedule Automatic = BuildPlan({}, { MakeNode(1, TEXT("A"), 1, 1, FVector(0.0, 0.0, 6.0)), MakeNode(2, TEXT("B"), 1, 17, FVector(10.0, 4.0, 6.0)) },
		FStagePowerDistribution(), TEXT("Arena"), When);
	TestTrue(TEXT("Automatic node position"), Automatic.ControlLocationMeters.Equals(FVector(5.0, 2.0, 0.0), 1e-9));
	TestEqual(TEXT("Node to the first fixture: 5.4 m across + 6 m up"), Automatic.DmxCables[0].DistanceMeters, FMath::Sqrt(29.0) + 6.0, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageCableScheduleExportTest, "StageCraft.Report.CableSchedule.Export", StageProductionReportTests::Flags)

bool FStageCableScheduleExportTest::RunTest(const FString& Parameters)
{
	using namespace StageProductionReportTests;

	TArray<FStagePowerLoad> Loads = { MakeLoad(TEXT("=HYPERLINK(\"x\")"), 900.0), MakeLoad(TEXT("Amp, rack 2"), 1500.0) };
	for (int32 Index = 1; Index <= 90; ++Index)
	{
		Loads.Add(MakeLoad(*FString::Printf(TEXT("Spot %d"), Index), 450.0, Index));
	}
	TArray<FStageDmxNode> Nodes;
	for (int32 Index = 1; Index <= 90; ++Index)
	{
		Nodes.Add(MakeNode(Index, *FString::Printf(TEXT("Spot %d"), Index), 1 + (Index - 1) / 32, 1 + ((Index - 1) % 32) * 16, FVector(Index, 0.0, 6.0)));
	}
	const FStageCableSchedule Plan = BuildPlan(Loads, Nodes, FStagePowerDistribution(), TEXT("Arena Tour"), When);
	TestEqual(TEXT("All loads planned"), Plan.Loads.Num(), 92);
	TestEqual(TEXT("Three universes"), Plan.Universes.Num(), 3);
	TestTrue(TEXT("90 fixtures on 3 phases stay within 15 % imbalance"), Plan.ImbalancePercent <= StageCableSchedule::ImbalanceWarningPercent);

	const FString Csv = StageCableSchedule::ToCsv(Plan);
	TestTrue(TEXT("CSV: byte order mark"), Csv.StartsWith(TEXT("﻿")));
	TestTrue(TEXT("CSV: title (quoted: it contains a comma)"), Csv.Contains(TEXT("\"StageCraft 3D Cable Schedule, Power Plan and Shopping List (generated proposal)\"\r\n")));
	TestTrue(TEXT("CSV: supply line (quoted: it contains a comma)"), Csv.Contains(TEXT("Supply,\"3-phase 230 V")));
	TestTrue(TEXT("CSV: phase header"), Csv.Contains(TEXT("\r\nPhase,Circuits,Load (W),Current (A),Share of load (%)\r\n")));
	TestTrue(TEXT("CSV: circuit header"), Csv.Contains(TEXT("\r\nCircuit,Distro,Phase,Loads,Load (W),Current (A),Use of rating (%),Note\r\n")));
	TestTrue(TEXT("CSV: shopping list header"), Csv.Contains(TEXT("\r\nSection,Item,Size,Quantity,Note\r\n")));
	TestTrue(TEXT("CSV: one DMX terminator per universe"), Csv.Contains(TEXT("\r\nDMX,DMX terminator,\"XLR 5-pin, 120 ohm\",3,")));
	TestTrue(TEXT("CSV: DMX header"), Csv.Contains(TEXT("\r\nCable,Universe,From,To,Start address,Route (m),Cables,Note\r\n")));
	TestTrue(TEXT("CSV: a formula label is defused and quoted"), Csv.Contains(TEXT(",\"'=HYPERLINK(\"\"x\"\")\",")));
	TestTrue(TEXT("CSV: a comma in a label is quoted"), Csv.Contains(TEXT("\"Amp, rack 2\"")));
	TestTrue(TEXT("CSV: one power cable row per load"), Csv.Contains(TEXT("\r\nP-092,")) && !Csv.Contains(TEXT("\r\nP-093,")));
	TestTrue(TEXT("CSV: the last DMX link of a universe is terminated"), Csv.Contains(TEXT("D1-32,1,#31 Spot 31,#32 Spot 32,1.497,1.0,2 m,Terminate DMX out\r\n")));

	const TArray<uint8> Pdf = StageCableSchedule::ToPdf(Plan);
	TestTrue(TEXT("PDF: structure"), VerifyPdfStructure(*this, Pdf, TEXT("Cable schedule PDF")));
	const FString Text = BytesToLatin1(Pdf);
	const int32 Pages = GetPdfPageCount(Text);
	TestTrue(TEXT("PDF: several pages for 92 loads and 90 links"), Pages >= 4);
	TestTrue(TEXT("PDF: footer on the last page"), Text.Contains(FString::Printf(TEXT("(Page %d of %d) Tj"), Pages, Pages)));
	TestEqual(TEXT("PDF: title on every page"), CountOccurrences(Text, TEXT("(CABLE SCHEDULE & POWER) Tj")), Pages);
	TestTrue(TEXT("PDF: sections"), Text.Contains(TEXT("(Shopping & assembly list) Tj")) && Text.Contains(TEXT("(Phase loads) Tj")) && Text.Contains(TEXT("(Distros) Tj"))
		&& Text.Contains(TEXT("(Circuits) Tj")) && Text.Contains(TEXT("(DMX cables")));

	// Nothing to plan still prints a valid page.
	const TArray<uint8> Empty = StageCableSchedule::ToPdf(BuildPlan({}, {}, FStagePowerDistribution(), FString(), When));
	TestTrue(TEXT("Empty PDF: structure"), VerifyPdfStructure(*this, Empty, TEXT("Empty cable schedule PDF")));
	TestTrue(TEXT("Empty PDF: says so"), BytesToLatin1(Empty).Contains(TEXT("(No equipment on the stage draws power.) Tj")));

	TestEqual(TEXT("File stem"), StageCableSchedule::MakeFileStem(TEXT(" Arena / Tour "), When), FString(TEXT("CableSchedule_Arena___Tour_20261021-163000")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageBillOfMaterialsPdfTest, "StageCraft.Report.BomPdf", StageProductionReportTests::Flags)

bool FStageBillOfMaterialsPdfTest::RunTest(const FString& Parameters)
{
	using namespace StageProductionReportTests;

	TArray<FStageBomSource> Sources;
	for (int32 Index = 1; Index <= 150; ++Index)
	{
		FStageBomSource& Source = Sources.AddDefaulted_GetRef();
		Source.ItemPath = FSoftObjectPath(FString::Printf(TEXT("/Game/Test/DA_Item%d.DA_Item%d"), Index, Index));
		Source.ItemName = FText::FromString(FString::Printf(TEXT("Item %d"), Index));
		Source.Manufacturer = FText::FromString(TEXT("Acme"));
		Source.UnitWeightKg = 2.5;
		Source.UnitPowerWatts = 100.0;
		Source.Quantity = 2;
	}
	const FStageBillOfMaterials Report = StageBillOfMaterials::Build(Sources);

	const TArray<uint8> Pdf = StageBillOfMaterials::ToPdf(Report, TEXT("Arena"), When);
	TestTrue(TEXT("PDF: structure"), VerifyPdfStructure(*this, Pdf, TEXT("Bill of materials PDF")));
	const FString Text = BytesToLatin1(Pdf);
	const int32 Pages = GetPdfPageCount(Text);
	TestTrue(TEXT("PDF: 150 lines need several pages"), Pages >= 5);
	TestTrue(TEXT("PDF: the table header repeats on every page"), CountOccurrences(Text, TEXT("(Item) Tj")) >= Pages);
	TestTrue(TEXT("PDF: summary with totals"), Text.Contains(TEXT("(150 lines \\267 300 units in 150 placements \\267 750.0 kg \\267 30.00 kW) Tj")));
	TestTrue(TEXT("PDF: subtotal"), Text.Contains(TEXT("(Other subtotal) Tj")));
	TestTrue(TEXT("PDF: grand total"), Text.Contains(TEXT("(TOTAL) Tj")) && Text.Contains(TEXT("(30000) Tj")));
	TestTrue(TEXT("PDF: footer"), Text.Contains(FString::Printf(TEXT("(Page %d of %d) Tj"), Pages, Pages)));

	const TArray<uint8> Empty = StageBillOfMaterials::ToPdf(StageBillOfMaterials::Build({}), FString(), When);
	TestTrue(TEXT("Empty PDF: structure"), VerifyPdfStructure(*this, Empty, TEXT("Empty bill of materials PDF")));
	TestTrue(TEXT("Empty PDF: says so"), BytesToLatin1(Empty).Contains(TEXT("(The stage is empty: there is nothing to list.) Tj")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageReportComposerTest, "StageCraft.Report.Composer", StageProductionReportTests::Flags)

bool FStageReportComposerTest::RunTest(const FString& Parameters)
{
	using namespace StageProductionReportTests;

	FStagePdfDocument Document(FStagePdfDocument::A4LandscapeWidth, FStagePdfDocument::A4LandscapeHeight);
	TestFalse(TEXT("SetCurrentPage refuses a page that does not exist"), Document.SetCurrentPage(0));
	Document.AddPage();
	Document.AddPage();
	Document.Text(10.0, 10.0, TEXT("On page two"), EStagePdfFont::Regular, 10.0);
	TestTrue(TEXT("SetCurrentPage goes back to page 1"), Document.SetCurrentPage(0));
	TestFalse(TEXT("SetCurrentPage refuses a negative index"), Document.SetCurrentPage(-1));
	Document.Text(10.0, 10.0, TEXT("Back on page one"), EStagePdfFont::Regular, 10.0);
	const FString DocumentText = BytesToLatin1(Document.Save());
	// Content streams are written in page order, so page one's text comes first in the file.
	const int32 PageOneText = DocumentText.Find(TEXT("(Back on page one)"));
	TestTrue(TEXT("Text drawn after SetCurrentPage lands on that page"), PageOneText != INDEX_NONE && PageOneText < DocumentText.Find(TEXT("(On page two)")));

	// A heading near the bottom moves to the next page with its table; a long table repeats its header after the break.
	FStageReportPdfComposer Composer(TEXT("TEST REPORT"), TEXT("Test"), TEXT("Scene"), When);
	Composer.SetSummary(TEXT("Summary"));
	Composer.BeginTable({ { TEXT("Col A"), 0.0 }, { TEXT("Col B"), 2.0, EStageReportAlign::Right } });
	for (int32 Row = 0; Row < 60; ++Row)
	{
		Composer.AddRow({ FString::Printf(TEXT("Row %d"), Row), TEXT("x"), TEXT("ignored extra cell") });
	}
	Composer.AddHeading(TEXT("Second section"));
	Composer.BeginTable({ { TEXT("Col A"), 1.0 } });
	Composer.AddRow({ TEXT("Only row") }, EStageReportRowStyle::Total);
	Composer.AddRow({}, EStageReportRowStyle::Body);
	const TArray<uint8> Pdf = Composer.Finish();
	TestTrue(TEXT("Composer: structure"), VerifyPdfStructure(*this, Pdf, TEXT("Composer PDF")));
	const FString Text = BytesToLatin1(Pdf);
	const int32 Pages = GetPdfPageCount(Text);
	// 30 rows fit under a header: the first table fills two pages exactly, so the heading cannot stay with it and starts page 3.
	TestEqual(TEXT("Composer: three pages"), Pages, 3);
	TestEqual(TEXT("Composer: header on both pages of the first table, and once for the second"), CountOccurrences(Text, TEXT("(Col A) Tj")), 3);
	TestTrue(TEXT("Composer: the heading keeps with its table"), Text.Find(TEXT("(Second section) Tj")) > Text.Find(TEXT("(Row 59) Tj")));
	TestFalse(TEXT("Composer: extra cells are ignored"), Text.Contains(TEXT("ignored extra cell")));
	TestEqual(TEXT("Composer: frame on every page"), CountOccurrences(Text, TEXT("(TEST REPORT) Tj")), Pages);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
