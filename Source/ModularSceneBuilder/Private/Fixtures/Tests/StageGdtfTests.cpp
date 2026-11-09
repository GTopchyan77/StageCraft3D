// Copyright Epic Games, Inc. All Rights Reserved.

// Tests of the GDTF reader (Docs/ADR/0033-gdtf-fixture-import-and-device-library.md, STATE.md #80): the ZIP and XML readers'
// limits and refusals, the GDTF parser, units, attribute mapping, mode layouts with value ranges and cells, capabilities and the
// import report. Every file is self-authored (Fixtures/Tests/StageGdtfTestFiles), never a manufacturer's.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Fixtures; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Data/StageParameterTypes.h"
#include "Fixtures/StageGdtfMapping.h"
#include "Fixtures/StageGdtfParser.h"
#include "Fixtures/StageGdtfUnits.h"
#include "Fixtures/StageXmlReader.h"
#include "Fixtures/StageZipReader.h"
#include "Fixtures/Tests/StageGdtfTestFiles.h"
#include "Misc/AutomationTest.h"
#include "Show/StageFixtureCells.h"

namespace StageGdtfTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	const FStageGdtfLayoutChannel* FindChannel(const FStageGdtfModeLayout& Layout, int32 FirstOffset)
	{
		return Layout.Channels.FindByPredicate([FirstOffset](const FStageGdtfLayoutChannel& Channel) { return !Channel.Offsets.IsEmpty() && Channel.Offsets[0] == FirstOffset; });
	}

	const FStageGdtfRange* FindRange(const FStageGdtfLayoutChannel& Channel, int64 DMXValue)
	{
		return Channel.Ranges.FindByPredicate([DMXValue](const FStageGdtfRange& Range) { return Range.DMXFrom <= DMXValue && DMXValue <= Range.DMXTo; });
	}

	bool AnyContains(const TArray<FString>& Lines, const TCHAR* Text)
	{
		return Lines.ContainsByPredicate([Text](const FString& Line) { return Line.Contains(Text); });
	}

	/** A spot description with one substitution, for refusal tests. */
	FString SpotWith(const TCHAR* From, const TCHAR* To)
	{
		FString Xml = StageGdtfTestFiles::SpotXml();
		Xml.ReplaceInline(From, To, ESearchCase::CaseSensitive);
		return Xml;
	}

	void BuildAll(const FStageGdtfParseResult& Parsed, TArray<FStageGdtfModeLayout>& OutLayouts)
	{
		OutLayouts.Reset();
		for (int32 Mode = 0; Mode < Parsed.Type.Modes.Num(); ++Mode)
		{
			OutLayouts.Add(StageGdtfMapping::BuildModeLayout(Parsed.Type, Mode));
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageFixturesZipTest, "StageCraft.Fixtures.Zip", StageGdtfTests::Flags)

bool FStageFixturesZipTest::RunTest(const FString& Parameters)
{
	using namespace StageGdtfTestFiles;
	const TArray<uint8> Check = Utf8(TEXT("123456789"));
	TestEqual(TEXT("CRC-32 check value"), StageZip::Crc32(Check), 0xCBF43926u);

	const TArray<uint8> Text = Utf8(TEXT("Hello, stage. Hello, stage. Hello, stage."));
	for (const bool bDeflate : { false, true })
	{
		const TArray<uint8> Archive = MakeZip({ { TEXT("a.txt"), Text }, { TEXT("dir/b.bin"), TArray<uint8>({ 1, 2, 3 }) } }, bDeflate);
		StageZip::FDirectory Directory;
		FString Error;
		TestTrue(TEXT("Directory read"), StageZip::ReadDirectory(Archive, StageZip::FLimits(), Directory, Error));
		TestEqual(TEXT("Two entries"), Directory.Entries.Num(), 2);
		const StageZip::FEntry* A = Directory.Find(TEXT("A.TXT"));
		TArray<uint8> Bytes;
		TestTrue(TEXT("Found ignoring case and extracted"), A && StageZip::Extract(Archive, *A, StageZip::FLimits(), Bytes, Error) && Bytes == Text);
		TestEqual(TEXT("Method"), A ? int32(A->Method) : -1, bDeflate ? 8 : 0);
	}

	StageZip::FDirectory Directory;
	FString Error;
	TestFalse(TEXT("Not a zip"), StageZip::ReadDirectory(Text, StageZip::FLimits(), Directory, Error));
	TArray<uint8> Truncated = MakeZip({ { TEXT("a.txt"), Text } }, false);
	Truncated.SetNum(Truncated.Num() - 5);
	TestFalse(TEXT("Truncated"), StageZip::ReadDirectory(Truncated, StageZip::FLimits(), Directory, Error));

	for (const TCHAR* Unsafe : { TEXT("../evil.txt"), TEXT("/abs.txt"), TEXT("C:/x.txt"), TEXT("a//b.txt"), TEXT("a/./b.txt"), TEXT("..\\up.txt") })
	{
		TestFalse(FString::Printf(TEXT("Unsafe name %s"), Unsafe), StageZip::ReadDirectory(MakeZip({ { Unsafe, Text } }, false), StageZip::FLimits(), Directory, Error));
	}
	TestTrue(TEXT("Safe nested name"), StageZip::IsSafeEntryName(TEXT("wheels/gobo_dots.png")));
	TestFalse(TEXT("Duplicate names"), StageZip::ReadDirectory(MakeZip({ { TEXT("a.txt"), Text }, { TEXT("A.txt"), Text } }, false), StageZip::FLimits(), Directory, Error));

	StageZip::FLimits Few;
	Few.MaxEntries = 1;
	TestFalse(TEXT("Too many entries"), StageZip::ReadDirectory(MakeZip({ { TEXT("a"), Text }, { TEXT("b"), Text } }, false), Few, Directory, Error));
	StageZip::FLimits Small;
	Small.MaxEntryBytes = 8;
	TestFalse(TEXT("Entry over its size limit"), StageZip::ReadDirectory(MakeZip({ { TEXT("a"), Text } }, false), Small, Directory, Error));

	TArray<uint8> Zeros;
	Zeros.SetNumZeroed(2 * 1024 * 1024);
	TestFalse(TEXT("Zip bomb ratio refused"), StageZip::ReadDirectory(MakeZip({ { TEXT("zeros"), Zeros } }, true), StageZip::FLimits(), Directory, Error)
		&& Error.Contains(TEXT("zip bomb")));

	TArray<uint8> Encrypted = MakeZip({ { TEXT("a"), Text } }, false);
	const int32 DirectoryFlags = Encrypted.Num() - 22 - (46 + 1) + 8; // The only directory entry's flags.
	Encrypted[DirectoryFlags] |= 0x01;
	TestFalse(TEXT("Encrypted entry"), StageZip::ReadDirectory(Encrypted, StageZip::FLimits(), Directory, Error) || !Error.Contains(TEXT("encrypted")));

	TArray<uint8> Damaged = MakeZip({ { TEXT("a.txt"), Text } }, false);
	Damaged[30 + 5] ^= 0xFF; // A byte of the stored data, after the 30-byte local header and the 5-byte name.
	TArray<uint8> Bytes;
	TestTrue(TEXT("Damaged archive still lists"), StageZip::ReadDirectory(Damaged, StageZip::FLimits(), Directory, Error));
	TestFalse(TEXT("CRC mismatch refused"), StageZip::Extract(Damaged, Directory.Entries[0], StageZip::FLimits(), Bytes, Error) || !Bytes.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageFixturesXmlTest, "StageCraft.Fixtures.Xml", StageGdtfTests::Flags)

bool FStageFixturesXmlTest::RunTest(const FString& Parameters)
{
	StageXml::FDocument Document;
	FString Error;
	const TCHAR* Good = TEXT("\xFEFF<?xml version=\"1.0\"?>\n<!-- c -->\n<Root a=\"1\" b='two &amp; &lt;3&gt; &#65;&#x42; &quot;q&quot; &apos;'>\n")
		TEXT("  <Child Name=\"x\"/><![CDATA[ <not> parsed ]]>text\n  <Child Name=\"y\">\n</Child>\n</Root>\n");
	TestTrue(TEXT("Well-formed document"), StageXml::Parse(Good, StageXml::FLimits(), Document, Error));
	if (Document.Nodes.Num() == 3)
	{
		TestEqual(TEXT("Root name"), Document.Nodes[0].Name, FString(TEXT("Root")));
		TestEqual(TEXT("Single-quoted value with references"), *Document.Nodes[0].FindAttribute(TEXT("b")), FString(TEXT("two & <3> AB \"q\" '")));
		TestEqual(TEXT("Children by name"), Document.GetChildren(0, TEXT("Child")).Num(), 2);
		TestEqual(TEXT("Line numbers"), Document.Nodes[2].Line, 5);
		TestEqual(TEXT("Path"), Document.GetPath(2), FString(TEXT("Root/Child")));
		TestNull(TEXT("Attribute names are case-sensitive"), Document.Nodes[0].FindAttribute(TEXT("A")));
	}
	else
	{
		AddError(FString::Printf(TEXT("Expected 3 nodes, got %d (%s)"), Document.Nodes.Num(), *Error));
	}

	struct FBad { const TCHAR* Xml; const TCHAR* Expect; };
	const FBad Bad[] = {
		{ TEXT("<!DOCTYPE x [<!ENTITY e \"boom\">]><x/>"), TEXT("document type") },
		{ TEXT("<a><b></a>"), TEXT("does not match") },
		{ TEXT("<a>"), TEXT("not closed") },
		{ TEXT("<a x=\"1\" x=\"2\"/>"), TEXT("twice") },
		{ TEXT("<a x=\"&bogus;\"/>"), TEXT("unknown entity") },
		{ TEXT("<a x=\"<\"/>"), TEXT("'<'") },
		{ TEXT("<a/><b/>"), TEXT("second root") },
		{ TEXT("<a x=1/>"), TEXT("quoted") },
		{ TEXT("<a x=\"&#0;\"/>"), TEXT("does not allow") },
		{ TEXT("<A></a>"), TEXT("does not match") },
		{ TEXT("<a>\n\n<b x=\"1\"y=\"2\"/></a>"), TEXT("line 3") },
		{ TEXT("<?xml version=\"1.0\" ?"), TEXT("processing instruction") },
		{ TEXT(""), TEXT("no root") },
	};
	for (const FBad& Case : Bad)
	{
		TestFalse(FString::Printf(TEXT("Refused: %s"), Case.Xml), StageXml::Parse(Case.Xml, StageXml::FLimits(), Document, Error));
		TestTrue(FString::Printf(TEXT("Reason for %s names '%s' (got '%s')"), Case.Xml, Case.Expect, *Error), Error.Contains(Case.Expect));
		TestTrue(TEXT("A refused document is empty"), Document.Nodes.IsEmpty());
	}

	StageXml::FLimits Tight;
	Tight.MaxDepth = 3;
	TestFalse(TEXT("Depth limit"), StageXml::Parse(TEXT("<a><b><c><d/></c></b></a>"), Tight, Document, Error));
	Tight = StageXml::FLimits();
	Tight.MaxNodes = 2;
	TestFalse(TEXT("Node limit"), StageXml::Parse(TEXT("<a><b/><c/></a>"), Tight, Document, Error));
	Tight = StageXml::FLimits();
	Tight.MaxValueLength = 4;
	TestFalse(TEXT("Value length limit"), StageXml::Parse(TEXT("<a x=\"12345\"/>"), Tight, Document, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageFixturesGdtfParseTest, "StageCraft.Fixtures.GdtfParse", StageGdtfTests::Flags)

bool FStageFixturesGdtfParseTest::RunTest(const FString& Parameters)
{
	using namespace StageGdtfTests;
	const FStageGdtfParseResult Parsed = StageGdtf::Parse(StageGdtfTestFiles::MakeGdtf(StageGdtfTestFiles::SpotXml()));
	TestTrue(FString::Printf(TEXT("Spot parses (%s)"), *FString::Join(Parsed.Errors, TEXT("; "))), Parsed.IsOk());
	TestTrue(TEXT("No warnings with its images"), Parsed.Warnings.IsEmpty());
	const FStageGdtfFixtureType& Type = Parsed.Type;
	TestEqual(TEXT("Version"), Type.DataVersion, FString(TEXT("1.2")));
	TestEqual(TEXT("Manufacturer"), Type.Manufacturer, FString(TEXT("StageCraft")));
	TestEqual(TEXT("Description decoded"), Type.Description, FString(TEXT("A spot for tests & nothing else")));
	TestEqual(TEXT("Fixture type id"), Type.FixtureTypeId.ToString(EGuidFormats::DigitsWithHyphens), FString(StageGdtfTestFiles::SpotId));
	TestEqual(TEXT("Wheels"), Type.Wheels.Num(), 3);
	TestEqual(TEXT("Prism facets counted"), Type.Wheels.Num() == 3 ? Type.Wheels[2].Slots[1].PrismFacets : 0, 3);
	TestTrue(TEXT("Slot colour"), Type.Wheels.Num() > 0 && Type.Wheels[0].Slots[1].Color.bSet && FMath::IsNearlyEqual(Type.Wheels[0].Slots[1].Color.X, 0.64));
	TestEqual(TEXT("Weight"), Type.Properties.WeightKg, 22.5f);
	TestEqual(TEXT("Power"), Type.Properties.PowerW, 470.f);
	TestEqual(TEXT("Models"), Type.Models.Num(), 3);
	TestEqual(TEXT("Geometries (Base, Yoke, Head, Beam)"), Type.Geometries.Num(), 4);
	const int32 Beam = Type.FindGeometry(TEXT("Beam"));
	TestTrue(TEXT("Beam under Base"), Beam != INDEX_NONE && Type.IsInSubtree(Beam, Type.FindGeometry(TEXT("Base"))));
	TestTrue(TEXT("Beam values"), Beam != INDEX_NONE && Type.Geometries[Beam].Beam.BeamType == EStageGdtfBeamType::Spot && Type.Geometries[Beam].Beam.LuminousFlux == 18000.f);
	TestTrue(TEXT("Position translation"), FMath::IsNearlyEqual(Type.Geometries[Type.FindGeometry(TEXT("Yoke"))].Position[11], 0.15));
	TestEqual(TEXT("Modes"), Type.Modes.Num(), 3);
	if (Type.Modes.Num() == 3)
	{
		const FStageGdtfMode& Standard = Type.Modes[0];
		TestEqual(TEXT("Standard channels"), Standard.Channels.Num(), 12);
		TestEqual(TEXT("16-bit offsets"), Standard.Channels[0].Offsets, TArray<int32>({ 1, 2 }));
		TestEqual(TEXT("Shutter functions"), Standard.Channels[3].LogicalChannels[0].Functions.Num(), 3);
		TestEqual(TEXT("Channel sets"), Standard.Channels[4].LogicalChannels[0].Functions[0].Sets.Num(), 3);
		TestEqual(TEXT("Break read"), Type.Modes[2].Channels[0].DMXBreak, 2);
	}

	// Missing images only warn; the same description without an archive does not check them.
	const FStageGdtfParseResult NoImages = StageGdtf::Parse(StageGdtfTestFiles::MakeGdtf(StageGdtfTestFiles::SpotXml(), false, false));
	TestTrue(TEXT("Missing images warn"), NoImages.IsOk() && AnyContains(NoImages.Warnings, TEXT("wheels/gobo_dots.png")));
	TestTrue(TEXT("Description only"), StageGdtf::ParseDescription(StageGdtfTestFiles::SpotXml()).IsOk());

	// Refusals name what is wrong.
	struct FBad { FString Xml; const TCHAR* Expect; };
	const FBad Bad[] = {
		{ SpotWith(StageGdtfTestFiles::SpotId, TEXT("not-a-guid")), TEXT("FixtureTypeID") },
		{ SpotWith(TEXT("Default=\"16/1\""), TEXT("Default=\"300/1\"")), TEXT("is not a DMX value") },
		{ SpotWith(TEXT("Offset=\"13\""), TEXT("Offset=\"0\"")), TEXT("Offset") },
		{ SpotWith(TEXT("Offset=\"14\""), TEXT("Offset=\"513\"")), TEXT("Offset") },
		{ SpotWith(TEXT("<DMXMode Name=\"Basic\" Geometry=\"Base\">"), TEXT("<DMXMode Name=\"Basic\" Geometry=\"Head\">")), TEXT("top-level") },
		{ SpotWith(TEXT("Wheel=\"Gobo1\""), TEXT("Wheel=\"Gobo9\"")), TEXT("Gobo9") },
		{ SpotWith(TEXT("BeamAngle=\"20\""), TEXT("BeamAngle=\"twenty\"")), TEXT("not a number") },
		{ SpotWith(TEXT("Length=\"0.40\""), TEXT("Length=\"-1\"")), TEXT("model sizes") },
		{ SpotWith(TEXT("<GDTF DataVersion"), TEXT("<NotGdtf DataVersion")).Replace(TEXT("</GDTF>"), TEXT("</NotGdtf>")), TEXT("<GDTF>") },
		{ SpotWith(TEXT("<DMXMode Name=\"Basic\""), TEXT("<DMXMode Name=\"Standard\"")), TEXT("used twice") },
	};
	for (const FBad& Case : Bad)
	{
		const FStageGdtfParseResult Result = StageGdtf::ParseDescription(Case.Xml);
		TestTrue(FString::Printf(TEXT("Refused with '%s' (got: %s)"), Case.Expect, *FString::Join(Result.Errors, TEXT(" | "))), !Result.IsOk() && AnyContains(Result.Errors, Case.Expect));
	}
	TestFalse(TEXT("Not a GDTF archive"), StageGdtf::Parse(StageGdtfTestFiles::MakeZip({ { TEXT("other.xml"), StageGdtfTestFiles::Utf8(TEXT("<a/>")) } }, false)).IsOk());
	StageGdtf::FLimits Tight;
	Tight.MaxModes = 2;
	TestTrue(TEXT("Mode limit"), AnyContains(StageGdtf::ParseDescription(StageGdtfTestFiles::SpotXml(), Tight).Errors, TEXT("DMX modes")));

	// DMX values.
	FStageGdtfDMXValue Value;
	TestTrue(TEXT("128/1"), StageGdtf::ParseDMXValue(TEXT("128/1"), 4, Value) && Value.Value == 128 && Value.Bytes == 1 && !Value.bShift);
	TestTrue(TEXT("255/1s"), StageGdtf::ParseDMXValue(TEXT("255/1s"), 4, Value) && Value.bShift);
	TestTrue(TEXT("Bare number is one byte"), StageGdtf::ParseDMXValue(TEXT("7"), 4, Value) && Value.Bytes == 1);
	TestFalse(TEXT("256/1 does not fit"), StageGdtf::ParseDMXValue(TEXT("256/1"), 4, Value));
	TestFalse(TEXT("Five bytes"), StageGdtf::ParseDMXValue(TEXT("1/5"), 4, Value));
	TestFalse(TEXT("Garbage"), StageGdtf::ParseDMXValue(TEXT("12x/1"), 4, Value));
	auto To = [](int64 V, int32 B, bool bS, int32 Channel) { FStageGdtfDMXValue X; X.Value = V; X.Bytes = B; X.bShift = bS; return StageGdtf::ToChannelValue(X, Channel); };
	TestEqual(TEXT("255/1 mirrored to 16 bits"), To(255, 1, false, 2), int64(65535));
	TestEqual(TEXT("128/1 mirrored to 16 bits"), To(128, 1, false, 2), int64(32896));
	TestEqual(TEXT("255/1s shifted to 16 bits"), To(255, 1, true, 2), int64(65280));
	TestEqual(TEXT("32768/2 to 8 bits"), To(32768, 2, false, 1), int64(128));
	TestEqual(TEXT("1/1 mirrored to 24 bits"), To(1, 1, false, 3), int64(0x010101));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageFixturesGdtfUnitsTest, "StageCraft.Fixtures.GdtfUnits", StageGdtfTests::Flags)

bool FStageFixturesGdtfUnitsTest::RunTest(const FString& Parameters)
{
	FStageGdtfColorCIE D65;
	const FLinearColor White = StageGdtfUnits::ChromaticityToLinear(D65);
	TestTrue(TEXT("D65 is white"), White.Equals(FLinearColor::White, 0.01f));
	FStageGdtfColorCIE Red;
	Red.X = 0.64;
	Red.Y = 0.33;
	const FLinearColor RedLinear = StageGdtfUnits::ChromaticityToLinear(Red);
	TestTrue(TEXT("Rec. 709 red primary"), FMath::IsNearlyEqual(RedLinear.R, 1.f, 0.01f) && RedLinear.G < 0.02f && RedLinear.B < 0.02f);
	FStageGdtfColorCIE Bad;
	Bad.Y = 0.0;
	TestEqual(TEXT("Degenerate chromaticity is white"), StageGdtfUnits::ChromaticityToLinear(Bad), FLinearColor::White);
	const double Matrix[16] = { 1, 0, 0, 0.1, 0, 1, 0, 0.02, 0, 0, 1, 0.15, 0, 0, 0, 1 };
	TestTrue(TEXT("Translation to Unreal centimetres with Y mirrored"), StageGdtfUnits::PositionToUnrealCm(Matrix).Equals(FVector(10.0, -2.0, 15.0), 0.001));
	TestTrue(TEXT("Sizes to centimetres"), StageGdtfUnits::SizeToUnrealCm(FVector(0.4, 0.3, 0.15)).Equals(FVector(40.0, 30.0, 15.0), 0.001));
	TestEqual(TEXT("Degrees per second to rpm"), StageGdtfUnits::DegreesPerSecondToRpm(720.f), 120.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageFixturesGdtfMappingTest, "StageCraft.Fixtures.GdtfMapping", StageGdtfTests::Flags)

bool FStageFixturesGdtfMappingTest::RunTest(const FString& Parameters)
{
	using namespace StageGdtfTests;
	using namespace StageCraftTags;

	// The attribute table.
	TestTrue(TEXT("Dimmer"), StageGdtfMapping::MapAttribute(TEXT("Dimmer")).Attribute == Attribute_Dimmer);
	TestTrue(TEXT("Amber is ColorAdd_RY"), StageGdtfMapping::MapAttribute(TEXT("ColorAdd_RY")).Attribute == Attribute_ColorA);
	TestTrue(TEXT("Legacy RGB name"), StageGdtfMapping::MapAttribute(TEXT("ColorRGB_Red")).Attribute == Attribute_ColorR);
	TestTrue(TEXT("Gobo spin in rpm"), StageGdtfMapping::MapAttribute(TEXT("Gobo1PosRotate")).Kind == EStageGdtfValueKind::Rpm);
	TestTrue(TEXT("Blade approximated"), StageGdtfMapping::MapAttribute(TEXT("Blade3A")).Attribute == Attribute_BladeC && StageGdtfMapping::MapAttribute(TEXT("Blade3A")).bApproximated);
	TestFalse(TEXT("Focus has no control"), StageGdtfMapping::MapAttribute(TEXT("Focus1")).Attribute.IsValid());
	TestFalse(TEXT("Names are case-sensitive"), StageGdtfMapping::MapAttribute(TEXT("dimmer")).Attribute.IsValid());
	TestFalse(TEXT("Second shutter is not the first"), StageGdtfMapping::MapAttribute(TEXT("Shutter2")).Attribute.IsValid());

	// Spot: three modes, the third refused.
	const FStageGdtfParseResult Spot = StageGdtf::Parse(StageGdtfTestFiles::MakeGdtf(StageGdtfTestFiles::SpotXml()));
	TArray<FStageGdtfModeLayout> Layouts;
	BuildAll(Spot, Layouts);
	if (!TestEqual(TEXT("Three layouts"), Layouts.Num(), 3))
	{
		return false;
	}
	const FStageGdtfModeLayout& Standard = Layouts[0];
	TestTrue(FString::Printf(TEXT("Standard usable (%s)"), *FString::Join(Standard.Errors, TEXT("; "))), Standard.IsUsable());
	TestEqual(TEXT("Standard footprint"), Standard.Footprint, 15);
	TestTrue(TEXT("Basic usable, 3 channels"), Layouts[1].IsUsable() && Layouts[1].Footprint == 3);
	TestTrue(TEXT("Broken refused for its DMX break"), !Layouts[2].IsUsable() && AnyContains(Layouts[2].Errors, TEXT("break")));

	if (const FStageGdtfLayoutChannel* Pan = FindChannel(Standard, 1))
	{
		TestTrue(TEXT("Pan 16-bit"), Pan->Attribute == Attribute_Pan && Pan->Is16Bit() && Pan->Status == EStageGdtfChannelStatus::Mapped);
		TestEqual(TEXT("Pan default 128/1 mirrored"), Pan->DefaultDMX, int64(32896));
		TestTrue(TEXT("Pan range in degrees"), Pan->Ranges.Num() == 1 && Pan->Ranges[0].DMXTo == 65535 && Pan->Ranges[0].ValueFrom == -270.f && Pan->Ranges[0].ValueTo == 270.f);
	}
	else { AddError(TEXT("No pan channel")); }
	if (const FStageGdtfLayoutChannel* Shutter = FindChannel(Standard, 6))
	{
		TestTrue(TEXT("Shutter owned"), Shutter->Attribute == Attribute_Shutter);
		TestEqual(TEXT("Shutter default from the initial function"), Shutter->DefaultDMX, int64(16));
		const FStageGdtfRange* Closed = FindRange(*Shutter, 5);
		const FStageGdtfRange* Open = FindRange(*Shutter, 20);
		const FStageGdtfRange* Strobe = FindRange(*Shutter, 32);
		const FStageGdtfRange* StrobeTop = FindRange(*Shutter, 127);
		const FStageGdtfRange* Random = FindRange(*Shutter, 200);
		TestTrue(TEXT("Closed is never selected"), Closed && !Closed->Attribute.IsValid());
		TestTrue(TEXT("Open is Shutter 0"), Open && Open->Attribute == Attribute_Shutter && Open->ValueFrom == 0.f && Open->DMXFrom == 16 && Open->DMXTo == 31);
		TestTrue(TEXT("Strobe from the floor to 1"), Strobe && Strobe == StrobeTop && FMath::IsNearlyEqual(Strobe->ValueFrom, StageGdtfMapping::StrobeFloor) && FMath::IsNearlyEqual(Strobe->ValueTo, 1.f));
		TestTrue(TEXT("Random strobe not selected"), Random && !Random->Attribute.IsValid());
		TestTrue(TEXT("Strobe rates"), Standard.StrobeRateHz.Equals(FVector2D(1.0, 25.0)));
	}
	else { AddError(TEXT("No shutter channel")); }
	if (const FStageGdtfLayoutChannel* Color = FindChannel(Standard, 7))
	{
		const FStageGdtfRange* OpenSlot = FindRange(*Color, 5);
		const FStageGdtfRange* RedSlot = FindRange(*Color, 15);
		const FStageGdtfRange* BlueSlot = FindRange(*Color, 100);
		const FStageGdtfRange* Spin = FindRange(*Color, 200);
		TestTrue(TEXT("Colour wheel slots (open is 0)"), OpenSlot && OpenSlot->Slot == 0 && RedSlot && RedSlot->Slot == 1 && BlueSlot && BlueSlot->Slot == 2 && BlueSlot->DMXTo == 127);
		TestTrue(TEXT("Wheel spin not selected"), Spin && !Spin->Attribute.IsValid());
	}
	else { AddError(TEXT("No colour channel")); }
	if (const FStageGdtfLayoutChannel* GoboPos = FindChannel(Standard, 9))
	{
		TestTrue(TEXT("Index owns the shared channel, approximated"), GoboPos->Attribute == Attribute_GoboRotation && GoboPos->Status == EStageGdtfChannelStatus::Approximated
			&& GoboPos->Note.Contains(TEXT("Gobo1PosRotate")));
	}
	if (const FStageGdtfLayoutChannel* Zoom = FindChannel(Standard, 11))
	{
		TestTrue(TEXT("Zoom 16-bit in degrees"), Zoom->Is16Bit() && Zoom->Ranges.Num() == 1 && Zoom->Ranges[0].ValueFrom == 8.f && Zoom->Ranges[0].ValueTo == 40.f);
	}
	if (const FStageGdtfLayoutChannel* Focus = FindChannel(Standard, 13))
	{
		TestTrue(TEXT("Focus kept at its default"), Focus->Status == EStageGdtfChannelStatus::Default && !Focus->Attribute.IsValid() && Focus->DefaultDMX == 128);
	}
	if (const FStageGdtfLayoutChannel* Control = FindChannel(Standard, 14))
	{
		TestTrue(TEXT("Control kept at 0"), Control->Status == EStageGdtfChannelStatus::Default && Control->DefaultDMX == 0);
	}
	if (const FStageGdtfLayoutChannel* Blade = FindChannel(Standard, 15))
	{
		TestTrue(TEXT("Blade normalised from percent"), Blade->Attribute == Attribute_BladeA && Blade->Status == EStageGdtfChannelStatus::Approximated
			&& Blade->Ranges.Num() == 1 && Blade->Ranges[0].ValueTo == 1.f);
	}

	const FStageGdtfCapabilities Caps = StageGdtfMapping::DeriveCapabilities(Spot.Type, Layouts);
	TestEqual(TEXT("Kind"), Caps.Kind, EStageFixtureKind::MovingHeadSpot);
	TestTrue(TEXT("Category"), Caps.Category == Category_Lighting_MovingHead);
	TestEqual(TEXT("No colour mixing"), Caps.ColorSystem, EStageFixtureColorSystem::None);
	TestTrue(TEXT("Pan and tilt limits"), Caps.bHasPanTilt && Caps.PanLimits.Equals(FVector2D(-270.0, 270.0)) && Caps.TiltLimits.Equals(FVector2D(-135.0, 135.0)));
	TestTrue(TEXT("Zoom range"), Caps.BeamAngleMin == 8.f && Caps.BeamAngleMax == 40.f);
	TestTrue(TEXT("Field ratio from the beam"), FMath::IsNearlyEqual(Caps.FieldAngleRatio, 1.5f));
	TestTrue(TEXT("Photometrics"), Caps.LuminousFlux == 18000.f && Caps.ColorTemperatureK == 7000.f && FMath::IsNearlyEqual(Caps.LensDiameterCm, 12.f));
	TestTrue(TEXT("Colour wheel"), Caps.ColorWheel.Num() == 2 && Caps.ColorWheel[0].Name == TEXT("Red") && Caps.ColorWheel[0].Filter.R > 0.9f && Caps.ColorWheel[0].Filter.G < 0.1f);
	TestTrue(TEXT("Gobo wheel with images"), Caps.GoboWheel.Num() == 2 && Caps.GoboWheel[1].MediaFileName == TEXT("gobo_star"));
	TestTrue(TEXT("Prism"), Caps.Prisms.Num() == 1 && Caps.Prisms[0].Facets == 3);
	TestFalse(TEXT("Spin shares a channel: not available"), Caps.bGoboSpin);
	TestTrue(TEXT("Blades"), Caps.BeamShaper == EStageBeamShaper::FramingShutters);
	TestTrue(TEXT("Shutter and strobe"), Caps.bHasShutter && Caps.StrobeRateHz.Equals(FVector2D(1.0, 25.0)));
	TestTrue(TEXT("Body sizes"), Caps.BaseSizeCm.Equals(FVector(40, 30, 15), 0.01) && Caps.YokeSizeCm.Equals(FVector(35, 12, 40), 0.01) && Caps.HeadSizeCm.Equals(FVector(25, 25, 35), 0.01));
	TestTrue(TEXT("Pivots"), Caps.PanPivotOffsetCm.Equals(FVector(0, -2, 15), 0.01) && Caps.TiltPivotOffsetCm.Equals(FVector(0, 0, 25), 0.01) && Caps.BeamOriginOffsetCm.Equals(FVector(0, 0, -10), 0.01));
	TestTrue(TEXT("Specs"), Caps.WeightKg == 22.5f && Caps.PowerW == 470.f && Caps.DimensionsCm.Equals(FVector(40, 30, 40), 0.01));
	TestFalse(TEXT("A moving head takes no gel frame"), Caps.bAcceptsGel);

	const FStageGdtfImportReport Report = StageGdtfMapping::BuildReport(Spot, Layouts, Caps);
	TestTrue(TEXT("Report can import"), Report.bCanImport && Report.Errors.IsEmpty());
	TestEqual(TEXT("Report title"), Report.Title, FString(TEXT("StageCraft Test Spot")));
	TestTrue(TEXT("Report counts"), AnyContains(Report.Lines, TEXT("Standard: 15 channels; 8 mapped, 2 approximated, 2 kept at default (Focus1, Control1).")));
	TestTrue(TEXT("Report names the left-out mode"), AnyContains(Report.Warnings, TEXT("Mode \"Broken\" is left out")));
	TestTrue(TEXT("Report text"), Report.ToText().Contains(TEXT("2 of 3 modes")));

	// Pixel bar: 8 RGB cells from GeometryReference breaks.
	const FStageGdtfParseResult Bar = StageGdtf::Parse(StageGdtfTestFiles::MakeGdtf(StageGdtfTestFiles::BarXml(8), false, false));
	TestTrue(FString::Printf(TEXT("Bar parses (%s)"), *FString::Join(Bar.Errors, TEXT("; "))), Bar.IsOk());
	BuildAll(Bar, Layouts);
	if (TestEqual(TEXT("One bar mode"), Layouts.Num(), 1))
	{
		const FStageGdtfModeLayout& Pixels = Layouts[0];
		TestTrue(FString::Printf(TEXT("Bar usable (%s)"), *FString::Join(Pixels.Errors, TEXT("; "))), Pixels.IsUsable());
		TestEqual(TEXT("Bar footprint"), Pixels.Footprint, 25);
		TestEqual(TEXT("Cells"), Pixels.CellCount, 8);
		const FStageGdtfLayoutChannel* Master = FindChannel(Pixels, 1);
		TestTrue(TEXT("Master dimmer on 1, full by default"), Master && Master->Attribute == Attribute_Dimmer && Master->DefaultDMX == 255);
		const FStageGdtfLayoutChannel* FirstRed = FindChannel(Pixels, 2);
		const FStageGdtfLayoutChannel* LastBlue = FindChannel(Pixels, 25);
		TestTrue(TEXT("Cell 1 red on 2"), FirstRed && FirstRed->Attribute == StageFixtureCells::GetCellAttribute(0, EStageCellChannel::ColorR) && FirstRed->Cell == 0);
		TestTrue(TEXT("Cell 8 blue on 25"), LastBlue && LastBlue->Attribute == StageFixtureCells::GetCellAttribute(7, EStageCellChannel::ColorB));

		const FStageGdtfCapabilities BarCaps = StageGdtfMapping::DeriveCapabilities(Bar.Type, Layouts);
		TestEqual(TEXT("Bar kind"), BarCaps.Kind, EStageFixtureKind::PixelBar);
		TestTrue(TEXT("Bar category"), BarCaps.Category == Category_Lighting_Pixel);
		TestTrue(TEXT("Bar cells"), BarCaps.CellCount == 8 && BarCaps.CellControl == EStageCellControl::Color);
		TestEqual(TEXT("Bar colour system"), BarCaps.ColorSystem, EStageFixtureColorSystem::RGB);
		TestTrue(TEXT("Bar emitters from the file"), BarCaps.Emitters.Num() == 3 && BarCaps.Emitters[0].R > 0.9f && BarCaps.Emitters[1].G > 0.9f && BarCaps.Emitters[2].B > 0.9f);
		TestTrue(TEXT("Bar beam from the repeated pixel"), BarCaps.LuminousFlux == 300.f && BarCaps.BeamAngleMax == 30.f);
	}

	// More cells than StageCraft supports refuse the mode.
	const FStageGdtfParseResult Long = StageGdtf::ParseDescription(StageGdtfTestFiles::BarXml(StageFixtureCells::MaxCells + 1));
	TestTrue(TEXT("Long bar parses"), Long.IsOk());
	const FStageGdtfModeLayout LongLayout = StageGdtfMapping::BuildModeLayout(Long.Type, 0);
	TestTrue(TEXT("Too many cells refused"), !LongLayout.IsUsable() && AnyContains(LongLayout.Errors, TEXT("cells")));
	const FStageGdtfImportReport LongReport = StageGdtfMapping::BuildReport(Long, MakeArrayView(&LongLayout, 1), StageGdtfMapping::DeriveCapabilities(Long.Type, MakeArrayView(&LongLayout, 1)));
	TestTrue(TEXT("Nothing usable: cannot import"), !LongReport.bCanImport && AnyContains(LongReport.Errors, TEXT("No mode")));

	// Two channels on one slot refuse the mode.
	const FStageGdtfParseResult Overlap = StageGdtf::ParseDescription(SpotWith(TEXT("Offset=\"13\""), TEXT("Offset=\"5\"")));
	TestTrue(TEXT("Overlap parses"), Overlap.IsOk());
	TestTrue(TEXT("Overlapping channels refused"), AnyContains(StageGdtfMapping::BuildModeLayout(Overlap.Type, 0).Errors, TEXT("used twice")));

	// Functions must rise in DMX value.
	const FStageGdtfParseResult Falling = StageGdtf::ParseDescription(SpotWith(TEXT("DMXFrom=\"128/1\" Default=\"128/1\""), TEXT("DMXFrom=\"20/1\" Default=\"20/1\"")));
	TestTrue(TEXT("Falling functions refused"), AnyContains(StageGdtfMapping::BuildModeLayout(Falling.Type, 0).Errors, TEXT("do not rise")));
	return true;
}

#endif
