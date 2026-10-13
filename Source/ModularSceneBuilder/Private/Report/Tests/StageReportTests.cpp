// Copyright Epic Games, Inc. All Rights Reserved.

// Unit tests for the bill of materials (Docs/ADR/0005-spans-outliner-and-bill-of-materials.md §5): aggregation, totals,
// defensive handling of bad data, CSV export and display formats. No world and no assets.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Report; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Algo/Reverse.h"
#include "Data/StageItemTypes.h"
#include "Misc/AutomationTest.h"
#include "Misc/DateTime.h"
#include "Report/StageBillOfMaterials.h"
#include "UI/StageBillOfMaterialsPanel.h"

namespace StageReportTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	FStageBomSource MakeSource(const TCHAR* Path, const TCHAR* Name, const FGameplayTag& Category, double WeightKg, double PowerW, int32 Quantity = 1)
	{
		FStageBomSource Source;
		Source.ItemPath = FSoftObjectPath(Path);
		Source.ItemName = FText::FromString(Name);
		Source.Manufacturer = FText::FromString(TEXT("Acme"));
		Source.Category = Category;
		Source.UnitWeightKg = WeightKg;
		Source.UnitPowerWatts = PowerW;
		Source.Quantity = Quantity;
		return Source;
	}

	TArray<FStageBomSource> MakeStage()
	{
		return {
			MakeSource(TEXT("/Game/Items/DA_Spot.DA_Spot"), TEXT("Spot 300"), StageCraftTags::Category_Lighting_MovingHead, 22.5, 750.0),
			MakeSource(TEXT("/Game/Items/DA_Truss.DA_Truss"), TEXT("F34 Truss Run"), StageCraftTags::Category_Truss_Straight, 14.0, 0.0, 3),
			MakeSource(TEXT("/Game/Items/DA_Spot.DA_Spot"), TEXT("Spot 300"), StageCraftTags::Category_Lighting_MovingHead, 22.5, 750.0),
			MakeSource(TEXT("/Game/Items/DA_Truss.DA_Truss"), TEXT("F34 Truss Run"), StageCraftTags::Category_Truss_Straight, 14.0, 0.0, 2),
			MakeSource(TEXT("/Game/Items/DA_Par.DA_Par"), TEXT("Par 64"), StageCraftTags::Category_Lighting_Par, 2.0, 120.0),
		};
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageReportBuildTest, "StageCraft.Report.Build", StageReportTests::Flags)

bool FStageReportBuildTest::RunTest(const FString& Parameters)
{
	using namespace StageReportTests;

	const FStageBillOfMaterials Report = StageBillOfMaterials::Build(MakeStage());
	if (!TestEqual(TEXT("Two disciplines"), Report.Groups.Num(), 2))
	{
		return false;
	}

	const FStageBomGroup& Truss = Report.Groups[0];
	TestEqual(TEXT("Truss first"), Truss.Label.ToString(), FString(TEXT("Truss")));
	TestEqual(TEXT("Both runs merge into one line"), Truss.Lines.Num(), 1);
	TestEqual(TEXT("5 sections in total"), Truss.Lines[0].Quantity, 5);
	TestEqual(TEXT("in 2 runs"), Truss.Lines[0].Placements, 2);
	TestEqual(TEXT("Truss weight per section x 5"), Truss.TotalWeightKg, 70.0);

	const FStageBomGroup& Lighting = Report.Groups[1];
	TestEqual(TEXT("Moving heads and pars share Lighting"), Lighting.Lines.Num(), 2);
	TestEqual(TEXT("Lines in natural name order"), Lighting.Lines[0].ItemName.ToString(), FString(TEXT("Par 64")));
	TestEqual(TEXT("Two spots"), Lighting.Lines[1].Quantity, 2);
	TestEqual(TEXT("Spots: placements equal quantity"), Lighting.Lines[1].Placements, 2);
	TestEqual(TEXT("Lighting power"), Lighting.TotalPowerWatts, 1620.0);

	TestEqual(TEXT("Line count"), Report.LineCount, 3);
	TestEqual(TEXT("Units"), Report.Quantity, 8);
	TestEqual(TEXT("Placements"), Report.Placements, 5);
	TestEqual(TEXT("Total weight"), Report.TotalWeightKg, 70.0 + 45.0 + 2.0);
	TestEqual(TEXT("Total power"), Report.TotalPowerWatts, 1620.0);

	// Order independence: totals and order are identical for the reversed stage.
	TArray<FStageBomSource> Reversed = MakeStage();
	Algo::Reverse(Reversed);
	const FStageBillOfMaterials Again = StageBillOfMaterials::Build(Reversed);
	TestEqual(TEXT("Reversed: same total weight"), Again.TotalWeightKg, Report.TotalWeightKg);
	TestEqual(TEXT("Reversed: same first line"), Again.Groups[1].Lines[0].ItemName.ToString(), FString(TEXT("Par 64")));

	// Bad data never makes a total negative or NaN.
	TArray<FStageBomSource> Bad = {
		MakeSource(TEXT("/Game/Items/DA_Bad.DA_Bad"), TEXT("Bad"), StageCraftTags::Category_Prop, -10.0, std::numeric_limits<double>::quiet_NaN(), 0),
	};
	const FStageBillOfMaterials Defended = StageBillOfMaterials::Build(Bad);
	TestEqual(TEXT("Quantity below 1 counts as 1"), Defended.Quantity, 1);
	TestEqual(TEXT("Negative weight counts as 0"), Defended.TotalWeightKg, 0.0);
	TestEqual(TEXT("NaN power counts as 0"), Defended.TotalPowerWatts, 0.0);

	TestTrue(TEXT("Empty stage, empty report"), StageBillOfMaterials::Build({}).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageReportCsvTest, "StageCraft.Report.Csv", StageReportTests::Flags)

bool FStageReportCsvTest::RunTest(const FString& Parameters)
{
	using namespace StageReportTests;
	using StageBillOfMaterials::EscapeCsvField;

	TestEqual(TEXT("Plain field unchanged"), EscapeCsvField(TEXT("Spot 300")), FString(TEXT("Spot 300")));
	TestEqual(TEXT("Comma quoted"), EscapeCsvField(TEXT("Truss, 2 m")), FString(TEXT("\"Truss, 2 m\"")));
	TestEqual(TEXT("Quotes doubled"), EscapeCsvField(TEXT("12\" wedge")), FString(TEXT("\"12\"\" wedge\"")));
	TestEqual(TEXT("Line break quoted"), EscapeCsvField(TEXT("a\nb")), FString(TEXT("\"a\nb\"")));
	TestEqual(TEXT("Formula neutralised"), EscapeCsvField(TEXT("=HYPERLINK(1)")), FString(TEXT("'=HYPERLINK(1)")));
	TestEqual(TEXT("Leading minus neutralised"), EscapeCsvField(TEXT("-1+2")), FString(TEXT("'-1+2")));
	TestEqual(TEXT("At sign neutralised"), EscapeCsvField(TEXT("@SUM")), FString(TEXT("'@SUM")));
	TestEqual(TEXT("Formula with comma: neutralised and quoted"), EscapeCsvField(TEXT("=A1,B1")), FString(TEXT("\"'=A1,B1\"")));

	const FDateTime When(2026, 10, 13, 14, 5, 9);
	const FString Csv = StageBillOfMaterials::ToCsv(StageBillOfMaterials::Build(MakeStage()), TEXT("Arena, Night 1"), When);
	TestTrue(TEXT("Starts with the UTF-8 byte order mark"), Csv.Len() > 0 && Csv[0] == TCHAR(0xFEFF));
	TestTrue(TEXT("Scene name escaped"), Csv.Contains(TEXT("Scene,\"Arena, Night 1\"\r\n")));
	TestTrue(TEXT("Date"), Csv.Contains(TEXT("Generated,2026-10-13 14:05\r\n")));
	TestTrue(TEXT("Header row"), Csv.Contains(TEXT("Category,Item,Manufacturer,Quantity,Placements,Unit Weight (kg),Total Weight (kg),Unit Power (W),Total Power (W)\r\n")));
	TestTrue(TEXT("Truss line"), Csv.Contains(TEXT("Truss,F34 Truss Run,Acme,5,2,14.00,70.00,0.00,0.00\r\n")));
	TestTrue(TEXT("Lighting subtotal"), Csv.Contains(TEXT("Lighting subtotal,,,3,3,,47.00,,1620.00\r\n")));
	TestTrue(TEXT("Grand total"), Csv.EndsWith(TEXT("TOTAL,,,8,5,,117.00,,1620.00\r\n")));

	TestEqual(TEXT("File name"), StageBillOfMaterials::MakeFileName(TEXT("Arena Night/1"), When), FString(TEXT("BOM_Arena_Night_1_20261013-140509.csv")));
	TestEqual(TEXT("Untitled file name"), StageBillOfMaterials::MakeFileName(TEXT("  "), When), FString(TEXT("BOM_Untitled_20261013-140509.csv")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageReportFormatTest, "StageCraft.Report.Format", StageReportTests::Flags)

bool FStageReportFormatTest::RunTest(const FString& Parameters)
{
	using namespace StageReportFormat;

	// Culture-dependent separators are avoided by testing values below 1000 in their unit and the unit switch itself.
	TestEqual(TEXT("Kilograms"), FormatWeight(512.44).ToString(), FString(TEXT("512.4 kg")));
	TestTrue(TEXT("Tonnes from 1000 kg"), FormatWeight(1250.0).ToString().EndsWith(TEXT(" t")));
	TestEqual(TEXT("Watts"), FormatPower(850.0).ToString(), FString(TEXT("850 W")));
	TestTrue(TEXT("Kilowatts from 1000 W"), FormatPower(8400.0).ToString().EndsWith(TEXT(" kW")));

	FStageBomLine Line;
	Line.Manufacturer = FText::FromString(TEXT("Prolyte"));
	Line.Quantity = 5;
	Line.Placements = 2;
	TestEqual(TEXT("Runs named"), MakeLineDetail(Line).ToString(), FString(TEXT("Prolyte · in 2 runs")));
	Line.Placements = 5;
	TestEqual(TEXT("Single pieces: maker only"), MakeLineDetail(Line).ToString(), FString(TEXT("Prolyte")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
