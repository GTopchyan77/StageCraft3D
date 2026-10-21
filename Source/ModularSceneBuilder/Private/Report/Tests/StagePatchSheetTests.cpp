// Copyright Epic Games, Inc. All Rights Reserved.

// Unit tests for the DMX patch sheet and the PDF writer behind it: sorting, counts and sanitizing, address formats, CSV export,
// and the PDF file structure (cross-reference offsets, stream lengths, page count, text escaping and encoding). No world.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Report; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/DateTime.h"
#include "Report/StagePatchSheet.h"
#include "Report/StagePdfWriter.h"
#include "Report/Tests/StageReportTestUtils.h"

namespace StagePatchSheetTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	using StageReportTestUtils::BytesToLatin1;
	using StageReportTestUtils::CountOccurrences;
	using StageReportTestUtils::VerifyPdfStructure;

	FStagePatchSheetRow MakeRow(int32 FixtureId, const TCHAR* Label, int32 Universe, int32 Address, int32 Footprint, bool bConflict = false)
	{
		FStagePatchSheetRow Row;
		Row.FixtureId = FixtureId;
		Row.Label = Label;
		Row.FixtureType = TEXT("Spot 300");
		Row.Mode = TEXT("16ch");
		Row.Universe = Universe;
		Row.Address = Address;
		Row.Footprint = Footprint;
		Row.LocationMeters = FVector(1.0, 2.0, 3.0);
		Row.bConflict = bConflict;
		return Row;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStagePatchSheetTest, "StageCraft.Report.PatchSheet", StagePatchSheetTests::Flags)

bool FStagePatchSheetTest::RunTest(const FString& Parameters)
{
	using namespace StagePatchSheetTests;

	const FDateTime When(2026, 10, 13, 14, 5, 9);
	FStagePatchSheetRow Broken = MakeRow(4, TEXT("Broken"), 1, 600, -3, true);
	Broken.LocationMeters = FVector(std::numeric_limits<double>::quiet_NaN(), 5.0, std::numeric_limits<double>::infinity());

	const FStagePatchSheet Sheet = StagePatchSheet::Build({
		MakeRow(3, TEXT("Wash"), 2, 1, 12, true),
		MakeRow(1, TEXT("Spot B"), 1, 1, 16),
		MakeRow(2, TEXT("Par"), 0, 0, 7),
		Broken,
		MakeRow(1, TEXT("Spot A"), 2, 13, 16, true),
	}, TEXT("Arena"), When);

	if (!TestEqual(TEXT("Five rows"), Sheet.Rows.Num(), 5))
	{
		return false;
	}
	TestEqual(TEXT("Sorted by ID, then label"), Sheet.Rows[0].Label, FString(TEXT("Spot A")));
	TestEqual(TEXT("Same ID: label order"), Sheet.Rows[1].Label, FString(TEXT("Spot B")));
	TestEqual(TEXT("Then ID 2"), Sheet.Rows[2].FixtureId, 2);
	TestEqual(TEXT("ID 4 last"), Sheet.Rows[4].FixtureId, 4);
	TestEqual(TEXT("Patched"), Sheet.PatchedCount, 3);
	TestEqual(TEXT("Unpatched: no universe, and an address beyond 512"), Sheet.UnpatchedCount, 2);
	TestEqual(TEXT("Conflicts: the unpatched row's flag is dropped"), Sheet.ConflictCount, 2);
	TestTrue(TEXT("Universes unique and sorted"), Sheet.Universes == TArray<int32>({ 1, 2 }));

	const FStagePatchSheetRow& Sanitized = Sheet.Rows[4];
	TestEqual(TEXT("Negative footprint counts as 0"), Sanitized.Footprint, 0);
	TestFalse(TEXT("Address 600 is unpatched"), StagePatchSheet::IsPatched(Sanitized));
	TestFalse(TEXT("Unpatched is never a conflict"), Sanitized.bConflict);
	TestEqual(TEXT("NaN coordinate reads 0"), Sanitized.LocationMeters.X, 0.0);
	TestEqual(TEXT("Finite coordinate kept"), Sanitized.LocationMeters.Y, 5.0);
	TestEqual(TEXT("Infinite coordinate reads 0"), Sanitized.LocationMeters.Z, 0.0);

	const FStagePatchSheetRow Spot = MakeRow(1, TEXT("Spot"), 1, 1, 16);
	TestEqual(TEXT("End address"), StagePatchSheet::GetEndAddress(Spot), 16);
	TestEqual(TEXT("Address format"), StagePatchSheet::FormatAddress(Spot), FString(TEXT("1.001")));
	TestEqual(TEXT("End address format"), StagePatchSheet::FormatEndAddress(Spot), FString(TEXT("1.016")));
	const FStagePatchSheetRow Unpatched = MakeRow(2, TEXT("Par"), 0, 0, 7);
	TestEqual(TEXT("Unpatched: no end"), StagePatchSheet::GetEndAddress(Unpatched), 0);
	TestEqual(TEXT("Unpatched address"), StagePatchSheet::FormatAddress(Unpatched), FString(TEXT("-")));
	TestEqual(TEXT("Zero footprint: no end"), StagePatchSheet::GetEndAddress(MakeRow(5, TEXT("Dummy"), 1, 100, 0)), 0);

	const FStagePatchSheet Empty = StagePatchSheet::Build({}, FString(), When);
	TestTrue(TEXT("Empty stage, empty sheet"), Empty.IsEmpty() && Empty.Universes.IsEmpty() && Empty.PatchedCount == 0);

	// CSV
	const FStagePatchSheet CsvSheet = StagePatchSheet::Build({
		MakeRow(1, TEXT("Spot A"), 1, 1, 16),
		MakeRow(2, TEXT("Par"), 0, 0, 7),
		MakeRow(3, TEXT("=cmd"), 1, 10, 16, true),
		MakeRow(4, TEXT("Spot, left"), 2, 1, 16),
	}, TEXT("Arena, Night 1"), When);
	const FString Csv = StagePatchSheet::ToCsv(CsvSheet);
	TestTrue(TEXT("Starts with the UTF-8 byte order mark"), Csv.Len() > 0 && Csv[0] == TCHAR(0xFEFF));
	TestTrue(TEXT("Title"), Csv.Contains(TEXT("StageCraft 3D DMX Patch Sheet\r\n")));
	TestTrue(TEXT("Scene escaped"), Csv.Contains(TEXT("Scene,\"Arena, Night 1\"\r\n")));
	TestTrue(TEXT("Date"), Csv.Contains(TEXT("Generated,2026-10-13 14:05\r\n")));
	TestTrue(TEXT("Fixture count"), Csv.Contains(TEXT("Fixtures,4\r\n")));
	TestTrue(TEXT("Universes"), Csv.Contains(TEXT("Universes,\"1, 2\"\r\n")));
	TestTrue(TEXT("Conflict count"), Csv.Contains(TEXT("Conflicts,1\r\n")));
	TestTrue(TEXT("Header row"), Csv.Contains(TEXT("\r\n\r\nFixture ID,Label,Fixture Type,Mode,Universe,Address,End Address,Channels,X (m),Y (m),Z (m),Notes\r\n")));
	TestTrue(TEXT("Patched row"), Csv.Contains(TEXT("\r\n1,Spot A,Spot 300,16ch,1,1,16,16,1.00,2.00,3.00,\r\n")));
	TestTrue(TEXT("Unpatched row"), Csv.Contains(TEXT("\r\n2,Par,Spot 300,16ch,,,,7,1.00,2.00,3.00,Unpatched\r\n")));
	TestTrue(TEXT("Conflict row, formula neutralised"), Csv.Contains(TEXT("\r\n3,'=cmd,Spot 300,16ch,1,10,25,16,1.00,2.00,3.00,Patch conflict\r\n")));
	TestTrue(TEXT("Comma in label quoted"), Csv.Contains(TEXT("\r\n4,\"Spot, left\",Spot 300,")));

	TestEqual(TEXT("File stem"), StagePatchSheet::MakeFileStem(TEXT("Arena Night/1"), When), FString(TEXT("PatchSheet_Arena_Night_1_20261013-140509")));
	TestEqual(TEXT("Untitled file stem"), StagePatchSheet::MakeFileStem(TEXT("  "), When), FString(TEXT("PatchSheet_Untitled_20261013-140509")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStagePdfTest, "StageCraft.Report.Pdf", StagePatchSheetTests::Flags)

bool FStagePdfTest::RunTest(const FString& Parameters)
{
	using namespace StagePatchSheetTests;

	// Metrics: Helvetica "Hello" = H 722 + e 556 + l 222 + l 222 + o 556 = 2278 units; bold = 722 + 556 + 278 + 278 + 611.
	TestTrue(TEXT("Helvetica 10 pt Hello"), FMath::IsNearlyEqual(FStagePdfDocument::MeasureText(TEXT("Hello"), EStagePdfFont::Regular, 10.0), 22.78, 1e-9));
	TestTrue(TEXT("Helvetica-Bold 10 pt Hello"), FMath::IsNearlyEqual(FStagePdfDocument::MeasureText(TEXT("Hello"), EStagePdfFont::Bold, 10.0), 24.45, 1e-9));
	TestEqual(TEXT("Fitting text unchanged"), FStagePdfDocument::FitText(TEXT("Hello"), EStagePdfFont::Regular, 10.0, 30.0), FString(TEXT("Hello")));
	const FString Long = TEXT("Moving head spot, stage left upstage truss position 4");
	const FString Fitted = FStagePdfDocument::FitText(Long, EStagePdfFont::Regular, 8.5, 80.0);
	TestTrue(TEXT("Truncated text ends with ..."), Fitted.EndsWith(TEXT("...")) && Fitted.Len() < Long.Len());
	TestTrue(TEXT("Truncated text fits"), FStagePdfDocument::MeasureText(Fitted, EStagePdfFont::Regular, 8.5) <= 80.0 + 1e-9);
	TestTrue(TEXT("Nothing fits a tiny cell"), FStagePdfDocument::FitText(Long, EStagePdfFont::Regular, 8.5, 2.0).IsEmpty());

	// A document without pages still saves a valid one-page PDF.
	const TArray<uint8> Blank = FStagePdfDocument(FStagePdfDocument::A4LandscapeWidth, FStagePdfDocument::A4LandscapeHeight).Save();
	VerifyPdfStructure(*this, Blank, TEXT("Blank"));
	TestEqual(TEXT("Blank: one page"), CountOccurrences(BytesToLatin1(Blank), TEXT("/Type /Page /")), 1);

	const FDateTime When(2026, 10, 13, 14, 5, 9);

	// Empty stage: one page with the message.
	const TArray<uint8> EmptyPdf = StagePatchSheet::ToPdf(StagePatchSheet::Build({}, TEXT("Arena"), When));
	VerifyPdfStructure(*this, EmptyPdf, TEXT("Empty"));
	const FString EmptyText = BytesToLatin1(EmptyPdf);
	TestEqual(TEXT("Empty: one page"), CountOccurrences(EmptyText, TEXT("/Type /Page /")), 1);
	TestTrue(TEXT("Empty: says so"), EmptyText.Contains(TEXT("(No lighting fixtures on this stage.) Tj")));
	TestTrue(TEXT("Creation date"), EmptyText.Contains(TEXT("/CreationDate (D:20261013140509)")));

	// 100 fixtures: the table spans pages, each with header and footer.
	TArray<FStagePatchSheetRow> Rows;
	for (int32 Id = 1; Id <= 100; ++Id)
	{
		Rows.Add(MakeRow(Id, TEXT("Spot"), 1 + (Id - 1) / 32, 1 + ((Id - 1) % 32) * 16, 16));
	}
	Rows[0].Label = TEXT("(x) \\ y");
	Rows[1].Label = TEXT("При");		// Cyrillic
	Rows[2].Label = TEXT("ԱԲ");				// Armenian
	Rows[3].Label = TEXT("Café – front");	// WinAnsi: e acute 0xE9, en dash 0x96
	Rows[4].bConflict = true;

	const int32 ExpectedPages = FMath::DivideAndRoundUp(100, StagePatchSheet::GetRowsPerPage());
	const TArray<uint8> Pdf = StagePatchSheet::ToPdf(StagePatchSheet::Build(Rows, TEXT("Arena"), When));
	VerifyPdfStructure(*this, Pdf, TEXT("Patch sheet"));
	const FString Text = BytesToLatin1(Pdf);
	TestTrue(TEXT("More than one page"), ExpectedPages > 1);
	TestEqual(TEXT("Page count"), CountOccurrences(Text, TEXT("/Type /Page /")), ExpectedPages);
	TestTrue(TEXT("Pages tree count"), Text.Contains(FString::Printf(TEXT("/Count %d >>"), ExpectedPages)));
	TestEqual(TEXT("Header repeats on every page"), CountOccurrences(Text, TEXT("(Fixture Type) Tj")), ExpectedPages);
	TestTrue(TEXT("Footer page numbers"), Text.Contains(FString::Printf(TEXT("(Page %d of %d) Tj"), ExpectedPages, ExpectedPages)));
	TestTrue(TEXT("Parentheses and backslash escaped"), Text.Contains(TEXT("(\\(x\\) \\\\ y) Tj")));
	TestTrue(TEXT("Cyrillic prints as ?"), Text.Contains(TEXT("(???) Tj")));
	TestTrue(TEXT("Armenian prints as ?"), Text.Contains(TEXT("(??) Tj")));
	TestTrue(TEXT("Latin-1 and en dash as octal escapes"), Text.Contains(TEXT("(Caf\\351 \\226 front) Tj")));
	TestTrue(TEXT("Conflict marked"), Text.Contains(TEXT("(CONFLICT) Tj")));
	TestTrue(TEXT("Address column"), Text.Contains(TEXT("(1.001) Tj")) && Text.Contains(TEXT("(1.016) Tj")));
	TestTrue(TEXT("Standard fonts, WinAnsi"), Text.Contains(TEXT("/BaseFont /Helvetica /Encoding /WinAnsiEncoding")) && Text.Contains(TEXT("/BaseFont /Helvetica-Bold")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
