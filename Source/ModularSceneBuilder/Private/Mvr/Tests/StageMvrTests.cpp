// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for reading MVR files (Docs/ADR/0034-mvr-scene-exchange-and-product-tiers.md §5, phase MV1): the XML reader's element
// text, matrices, addresses, UUIDs, units, a self-authored scene (layers, a group, a fixture with patch and focus, a truss with
// geometry and a symbol), refusals of malformed or capped files, and the archive rules. Written from the public MVR 1.6
// specification; no third-party files are committed.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Mvr; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/StageXmlReader.h"
#include "Fixtures/Tests/StageGdtfTestFiles.h"
#include "Misc/AutomationTest.h"
#include "Mvr/StageMvrParser.h"

namespace StageMvrTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	const TCHAR* const LayerA = TEXT("8E5A0D5C-4E2B-4C1A-9D3E-000000000001");
	const TCHAR* const LayerB = TEXT("8E5A0D5C-4E2B-4C1A-9D3E-000000000002");
	const TCHAR* const Group = TEXT("8E5A0D5C-4E2B-4C1A-9D3E-000000000003");
	const TCHAR* const Spot = TEXT("8E5A0D5C-4E2B-4C1A-9D3E-000000000004");
	const TCHAR* const Focus = TEXT("8E5A0D5C-4E2B-4C1A-9D3E-000000000005");
	const TCHAR* const Truss = TEXT("8E5A0D5C-4E2B-4C1A-9D3E-000000000006");
	const TCHAR* const Symdef = TEXT("8E5A0D5C-4E2B-4C1A-9D3E-000000000007");
	const TCHAR* const Screen = TEXT("8E5A0D5C-4E2B-4C1A-9D3E-000000000008");
	const TCHAR* const Wash = TEXT("8e5a0d5c4e2b4c1a9d3e000000000009");

	/**
	 * A small rig: layer A (raised 1 m) with a group (moved 1 m along X) holding a spot patched at 1.10 aiming at a focus point,
	 * a wash without GDTF on absolute address 513, and a truss with 3DS geometry and a symbol; layer B with a video screen and an
	 * unknown element. Extra replaces the closing of layer A's ChildList, to add elements in tests.
	 */
	FString SceneXml(const FString& SpotAddress = TEXT("1.10"), const FString& Extra = FString())
	{
		return FString::Printf(TEXT(
			"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\" ?>\n"
			"<GeneralSceneDescription verMajor=\"1\" verMinor=\"6\" provider=\"StageCraft tests\" providerVersion=\"1.0\">\n"
			"  <Scene>\n"
			"    <AUXData>\n"
			"      <Symdef uuid=\"%s\" name=\"Clamp\"><ChildList><Geometry3D fileName=\"clamp.glb\"/></ChildList></Symdef>\n"
			"    </AUXData>\n"
			"    <Layers>\n"
			"      <Layer uuid=\"%s\" name=\"Rig\">\n"
			"        <Matrix>{1,0,0}{0,1,0}{0,0,1}{0,0,1000}</Matrix>\n"
			"        <ChildList>\n"
			"          <GroupObject uuid=\"%s\" name=\"Front\">\n"
			"            <Matrix>{1,0,0}{0,1,0}{0,0,1}{1000,0,0}</Matrix>\n"
			"            <ChildList>\n"
			"              <Fixture uuid=\"%s\" name=\"Spot &amp; 1\">\n"
			"                <Matrix>{1,0,0}{0,1,0}{0,0,1}{250, 500 ,6000}</Matrix>\n"
			"                <GDTFSpec>Spot</GDTFSpec>\n"
			"                <GDTFMode>Standard</GDTFMode>\n"
			"                <Focus>%s</Focus>\n"
			"                <FixtureID>101</FixtureID>\n"
			"                <FixtureIDNumeric>101</FixtureIDNumeric>\n"
			"                <UnitNumber>1</UnitNumber>\n"
			"                <Addresses><Address break=\"0\">%s</Address></Addresses>\n"
			"              </Fixture>\n"
			"            </ChildList>\n"
			"          </GroupObject>\n"
			"          <FocusPoint uuid=\"%s\" name=\"DSC\"><Matrix>{1,0,0}{0,1,0}{0,0,1}{0,0,0}</Matrix></FocusPoint>\n"
			"          <Fixture uuid=\"%s\" name=\"Wash\"><Addresses><Address>513</Address></Addresses></Fixture>\n"
			"          <Truss uuid=\"%s\" name=\"Upstage\">\n"
			"            <Geometries><Geometry3D fileName=\"truss\"/><Symbol uuid=\"8E5A0D5C-4E2B-4C1A-9D3E-0000000000AA\" symdef=\"%s\"/></Geometries>\n"
			"          </Truss>\n"
			"          %s\n"
			"        </ChildList>\n"
			"      </Layer>\n"
			"      <Layer uuid=\"%s\" name=\"Video\">\n"
			"        <ChildList><VideoScreen uuid=\"%s\" name=\"LED\"/><Foo/></ChildList>\n"
			"      </Layer>\n"
			"    </Layers>\n"
			"  </Scene>\n"
			"</GeneralSceneDescription>\n"),
			Symdef, LayerA, Group, Spot, Focus, *SpotAddress, Focus, Wash, Truss, Symdef, *Extra, LayerB, Screen);
	}

	FGuid Id(const TCHAR* Text)
	{
		FGuid Out;
		StageMvr::ParseUuid(Text, Out);
		return Out;
	}

	bool HasMessage(const TArray<FString>& Messages, const TCHAR* Part)
	{
		return Messages.ContainsByPredicate([Part](const FString& Message) { return Message.Contains(Part); });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageMvrXmlTextTest, "StageCraft.Mvr.XmlText", StageMvrTests::Flags)

bool FStageMvrXmlTextTest::RunTest(const FString& Parameters)
{
	const FString Xml = TEXT("<A><B>  1.10 &amp; &#65;</B><C><D>x</D>tail</C><E><![CDATA[<raw>]]></E><F/></A>");
	StageXml::FDocument Document;
	FString Error;

	TestTrue(TEXT("Default: parses"), StageXml::Parse(Xml, StageXml::FLimits(), Document, Error));
	TestTrue(TEXT("Default: text is not kept (GDTF unchanged)"), Document.Nodes.Num() == 6 && Document.Nodes[1].Text.IsEmpty());

	StageXml::FLimits Limits;
	Limits.bKeepText = true;
	TestTrue(TEXT("Text: parses"), StageXml::Parse(Xml, Limits, Document, Error));
	const auto TextOf = [&Document](const TCHAR* Name)
	{
		const StageXml::FNode* Node = Document.Nodes.FindByPredicate([Name](const StageXml::FNode& Candidate) { return Candidate.Name == Name; });
		return Node ? Node->Text : FString(TEXT("<missing>"));
	};
	TestEqual(TEXT("Text: references decoded, not trimmed"), TextOf(TEXT("B")), FString(TEXT("  1.10 & A")));
	TestEqual(TEXT("Text: a leaf"), TextOf(TEXT("D")), FString(TEXT("x")));
	TestEqual(TEXT("Text: none around child elements"), TextOf(TEXT("C")), FString());
	TestEqual(TEXT("Text: CDATA kept as written"), TextOf(TEXT("E")), FString(TEXT("<raw>")));
	TestEqual(TEXT("Text: an empty element"), TextOf(TEXT("F")), FString());

	Limits.MaxValueLength = 8;
	TestFalse(TEXT("Text: over the cap is refused"), StageXml::Parse(TEXT("<A><B>123456789</B></A>"), Limits, Document, Error));
	TestTrue(TEXT("Text: the error names the element"), Error.Contains(TEXT("<B>")));
	TestTrue(TEXT("Text: white space before children does not count"), StageXml::Parse(TEXT("<A>                    <B>1</B></A>"), Limits, Document, Error));
	TestFalse(TEXT("Text: a bad reference is refused"), StageXml::Parse(TEXT("<A>&bogus;</A>"), StageXml::FLimits{ .bKeepText = true }, Document, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageMvrValuesTest, "StageCraft.Mvr.Values", StageMvrTests::Flags)

bool FStageMvrValuesTest::RunTest(const FString& Parameters)
{
	FMatrix Matrix;
	TestTrue(TEXT("Matrix: identity with an offset"), StageMvr::ParseMatrix(TEXT("{1,0,0}{0,1,0}{0,0,1}{10,-20,3.5e2}"), Matrix)
		&& Matrix.GetOrigin().Equals(FVector(10, -20, 350)) && Matrix.GetScaledAxis(EAxis::X).Equals(FVector::XAxisVector));
	TestTrue(TEXT("Matrix: white space anywhere"), StageMvr::ParseMatrix(TEXT(" { 1 , 0 , 0 }\n{0,1,0} {0,0,1}{0,0,0} "), Matrix));
	TestFalse(TEXT("Matrix: three groups"), StageMvr::ParseMatrix(TEXT("{1,0,0}{0,1,0}{0,0,1}"), Matrix));
	TestFalse(TEXT("Matrix: four numbers in a group"), StageMvr::ParseMatrix(TEXT("{1,0,0,0}{0,1,0}{0,0,1}{0,0,0}"), Matrix));
	TestFalse(TEXT("Matrix: not a number"), StageMvr::ParseMatrix(TEXT("{1,0,x}{0,1,0}{0,0,1}{0,0,0}"), Matrix));
	TestFalse(TEXT("Matrix: trailing text"), StageMvr::ParseMatrix(TEXT("{1,0,0}{0,1,0}{0,0,1}{0,0,0}x"), Matrix));
	TestFalse(TEXT("Matrix: infinity"), StageMvr::ParseMatrix(TEXT("{1,0,0}{0,1,0}{0,0,1}{inf,0,0}"), Matrix));
	TestFalse(TEXT("Matrix: empty"), StageMvr::ParseMatrix(TEXT(""), Matrix));

	FStageMvrAddress Address;
	TestTrue(TEXT("Address: Universe.Address"), StageMvr::ParseAddress(TEXT("2.512"), 0, Address) && Address == FStageMvrAddress{ 0, 2, 512 });
	TestTrue(TEXT("Address: absolute 513 is 2.1"), StageMvr::ParseAddress(TEXT(" 513 "), 0, Address) && Address == FStageMvrAddress{ 0, 2, 1 });
	TestTrue(TEXT("Address: absolute 1 is 1.1, break kept"), StageMvr::ParseAddress(TEXT("1"), 1, Address) && Address == FStageMvrAddress{ 1, 1, 1 });
	TestFalse(TEXT("Address: universe 0"), StageMvr::ParseAddress(TEXT("0.1"), 0, Address));
	TestFalse(TEXT("Address: address 513"), StageMvr::ParseAddress(TEXT("1.513"), 0, Address));
	TestFalse(TEXT("Address: absolute 0"), StageMvr::ParseAddress(TEXT("0"), 0, Address));
	TestFalse(TEXT("Address: three parts"), StageMvr::ParseAddress(TEXT("1.1.1"), 0, Address));
	TestFalse(TEXT("Address: words"), StageMvr::ParseAddress(TEXT("one"), 0, Address));
	TestFalse(TEXT("Address: negative break"), StageMvr::ParseAddress(TEXT("1.1"), -1, Address));

	FGuid Guid;
	TestTrue(TEXT("UUID: with dashes"), StageMvr::ParseUuid(TEXT("8E5A0D5C-4E2B-4C1A-9D3E-000000000004"), Guid));
	FGuid Same;
	TestTrue(TEXT("UUID: without dashes, lower case, in braces: the same id"), StageMvr::ParseUuid(TEXT("{8e5a0d5c4e2b4c1a9d3e000000000004}"), Same) && Same == Guid);
	TestFalse(TEXT("UUID: nil is refused"), StageMvr::ParseUuid(TEXT("00000000-0000-0000-0000-000000000000"), Guid));
	TestFalse(TEXT("UUID: too short"), StageMvr::ParseUuid(TEXT("8E5A0D5C-4E2B"), Guid));
	TestFalse(TEXT("UUID: not hex"), StageMvr::ParseUuid(TEXT("ZE5A0D5C-4E2B-4C1A-9D3E-000000000004"), Guid));

	// Units: MVR right-handed mm to Unreal left-handed cm.
	FMatrix Offset;
	StageMvr::ParseMatrix(TEXT("{1,0,0}{0,1,0}{0,0,1}{1000,2000,3000}"), Offset);
	TestTrue(TEXT("Units: mm to cm, Y mirrored"), StageMvrUnits::ToUnreal(Offset).GetLocation().Equals(FVector(100, -200, 300)));
	FMatrix Turn;
	StageMvr::ParseMatrix(TEXT("{0,1,0}{-1,0,0}{0,0,1}{0,0,0}"), Turn); // +90 degrees about Z in MVR: X goes to +Y
	TestTrue(TEXT("Units: a turn to MVR +Y faces Unreal -Y"), StageMvrUnits::ToUnreal(Turn).TransformVectorNoScale(FVector::XAxisVector).Equals(-FVector::YAxisVector, 1.e-6));
	FMatrix Mixed;
	StageMvr::ParseMatrix(TEXT("{0.8660254,0.5,0}{-0.5,0.8660254,0}{0,0,1}{1234,-567,890}"), Mixed);
	TestTrue(TEXT("Units: round trip"), StageMvrUnits::FromUnreal(StageMvrUnits::ToUnreal(Mixed)).Equals(Mixed, 1.e-4));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageMvrSceneTest, "StageCraft.Mvr.Scene", StageMvrTests::Flags)

bool FStageMvrSceneTest::RunTest(const FString& Parameters)
{
	using namespace StageMvrTests;
	const FStageMvrParseResult Result = StageMvr::ParseSceneDescription(SceneXml());
	TestTrue(TEXT("Scene: valid"), Result.IsValid());
	for (const FString& Message : Result.Errors)
	{
		AddInfo(Message);
	}
	const FStageMvrScene& Scene = Result.Scene;
	TestTrue(TEXT("Scene: version and provider"), Scene.VerMajor == 1 && Scene.VerMinor == 6 && Scene.Provider == TEXT("StageCraft tests"));
	TestEqual(TEXT("Scene: two layers"), Scene.Layers.Num(), 2);
	TestTrue(TEXT("Scene: counts"), Scene.Count(EStageMvrObjectKind::Fixture) == 2 && Scene.Count(EStageMvrObjectKind::GroupObject) == 1
		&& Scene.Count(EStageMvrObjectKind::FocusPoint) == 1 && Scene.Count(EStageMvrObjectKind::Truss) == 1 && Scene.Count(EStageMvrObjectKind::VideoScreen) == 1);

	const FStageMvrObject* SpotObject = Scene.FindObject(Id(Spot));
	TestTrue(TEXT("Spot: found, name decoded"), SpotObject && SpotObject->Name == TEXT("Spot & 1"));
	if (SpotObject)
	{
		TestTrue(TEXT("Spot: world = own + group + layer"), SpotObject->World.GetOrigin().Equals(FVector(1250, 500, 7000)));
		TestTrue(TEXT("Spot: parent is the group"), Scene.Objects.IsValidIndex(SpotObject->Parent) && Scene.Objects[SpotObject->Parent].Id == Id(Group));
		TestEqual(TEXT("Spot: GDTF file gets its extension"), SpotObject->GdtfSpec, FString(TEXT("Spot.gdtf")));
		TestEqual(TEXT("Spot: mode"), SpotObject->GdtfMode, FString(TEXT("Standard")));
		TestTrue(TEXT("Spot: patch 1.10"), SpotObject->Addresses.Num() == 1 && SpotObject->Addresses[0] == FStageMvrAddress{ 0, 1, 10 });
		TestTrue(TEXT("Spot: ids"), SpotObject->FixtureId == TEXT("101") && SpotObject->FixtureIdNumeric == 101 && SpotObject->UnitNumber == 1);
		TestTrue(TEXT("Spot: focus"), SpotObject->Focus == Id(Focus));
		TestTrue(TEXT("Spot: in Unreal, 1 m up and mirrored"), StageMvrUnits::ToUnreal(SpotObject->World).GetLocation().Equals(FVector(125, -50, 700)));
	}
	const FStageMvrObject* WashObject = Scene.FindObject(Id(Wash));
	TestTrue(TEXT("Wash: id without dashes, absolute address"), WashObject && WashObject->Addresses.Num() == 1 && WashObject->Addresses[0] == FStageMvrAddress{ 0, 2, 1 });
	const FStageMvrObject* TrussObject = Scene.FindObject(Id(Truss));
	TestTrue(TEXT("Truss: 3DS name completed, symbol kept"), TrussObject && TrussObject->GeometryFiles == TArray<FString>({ TEXT("truss.3ds") })
		&& TrussObject->Symbols.Num() == 1 && TrussObject->Symbols[0] == Id(Symdef));
	TestTrue(TEXT("Symdef: its geometry"), Scene.Symdefs.Num() == 1 && Scene.Symdefs[0].GeometryFiles == TArray<FString>({ TEXT("clamp.glb") }));
	TestTrue(TEXT("GDTF types"), Scene.GetGdtfSpecs() == TArray<FString>({ TEXT("Spot.gdtf") }));
	TestTrue(TEXT("Report: the wash is placed as a generic"), HasMessage(Result.Warnings, TEXT("\"Wash\"")));
	TestTrue(TEXT("Report: the unknown element"), HasMessage(Result.Warnings, TEXT("<Foo>")));
	TestTrue(TEXT("Describe"), StageMvr::Describe(Scene).Contains(TEXT("2 fixtures")));

	// Semantic gaps are warnings; the file still reads.
	const FStageMvrParseResult Gaps = StageMvr::ParseSceneDescription(SceneXml(TEXT("1.10"),
		TEXT("<Fixture uuid=\"8E5A0D5C-4E2B-4C1A-9D3E-0000000000B1\" name=\"Lost\"><GDTFSpec>Spot.gdtf</GDTFSpec><Focus>8E5A0D5C-4E2B-4C1A-9D3E-0000000000FF</Focus>")
		TEXT("<Addresses><Address break=\"1\">3.1</Address></Addresses></Fixture>")));
	TestTrue(TEXT("Gaps: valid"), Gaps.IsValid());
	TestTrue(TEXT("Gaps: no mode, unknown focus, break 1"), HasMessage(Gaps.Warnings, TEXT("names no GDTF mode")) && HasMessage(Gaps.Warnings, TEXT("focus point that is not in the file"))
		&& HasMessage(Gaps.Warnings, TEXT("DMX break 1")));
	const FStageMvrObject* Lost = Gaps.Scene.FindObject(Id(TEXT("8E5A0D5C-4E2B-4C1A-9D3E-0000000000B1")));
	TestTrue(TEXT("Gaps: an unknown focus is cleared"), Lost && !Lost->Focus.IsValid());

	// Structural errors refuse the whole file and name the line.
	const auto Refused = [this](const FString& Xml, const TCHAR* What, const TCHAR* Expected, const StageMvr::FLimits& Limits = StageMvr::FLimits())
	{
		const FStageMvrParseResult Bad = StageMvr::ParseSceneDescription(Xml, Limits);
		TestTrue(FString::Printf(TEXT("Refused: %s"), What), !Bad.IsValid() && Bad.Scene.Objects.IsEmpty() && HasMessage(Bad.Errors, Expected));
		if (!HasMessage(Bad.Errors, Expected))
		{
			AddInfo(FString::Join(Bad.Errors, TEXT(" | ")));
		}
	};
	Refused(SceneXml(TEXT("1.600")), TEXT("a bad address"), TEXT("is not Universe.Address"));
	Refused(SceneXml().Replace(TEXT("{250, 500 ,6000}"), TEXT("{250,500}")), TEXT("a bad matrix"), TEXT("<Matrix>"));
	Refused(SceneXml().Replace(Truss, Spot), TEXT("a duplicate uuid"), TEXT("used twice"));
	Refused(SceneXml().Replace(TEXT("verMajor=\"1\""), TEXT("verMajor=\"2\"")), TEXT("major version 2"), TEXT("not supported"));
	Refused(TEXT("<GDTF DataVersion=\"1.2\"/>"), TEXT("a GDTF description"), TEXT("not an MVR scene"));
	Refused(TEXT("<GeneralSceneDescription verMajor=\"1\" verMinor=\"6\"/>"), TEXT("no scene"), TEXT("no <Scene>"));
	Refused(SceneXml().Replace(TEXT("<FixtureIDNumeric>101"), TEXT("<FixtureIDNumeric>one")), TEXT("a bad number"), TEXT("not a whole number"));
	Refused(TEXT("<GeneralSceneDescription verMajor=\"1\"><Scene><Layers><Layer name=\"x\"/></Layers></Scene></GeneralSceneDescription>"), TEXT("a layer without uuid"), TEXT("no valid uuid"));
	StageMvr::FLimits Tight;
	Tight.MaxFixtures = 1;
	Refused(SceneXml(), TEXT("over the fixture cap"), TEXT("more than 1 fixtures"), Tight);
	Tight = StageMvr::FLimits();
	Tight.MaxObjectDepth = 1;
	Refused(SceneXml(), TEXT("nested too deep"), TEXT("nested deeper than 1"), Tight);
	Refused(TEXT("<GeneralSceneDescription verMajor=\"1\"><Scene></Scene>"), TEXT("broken XML"), TEXT("not closed"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageMvrArchiveTest, "StageCraft.Mvr.Archive", StageMvrTests::Flags)

bool FStageMvrArchiveTest::RunTest(const FString& Parameters)
{
	using namespace StageMvrTests;
	using namespace StageGdtfTestFiles;
	const TArray<uint8> Gdtf = MakeGdtf(SpotXml());
	for (const bool bDeflate : { false, true })
	{
		const TArray<uint8> File = MakeZip({ { TEXT("GeneralSceneDescription.xml"), Utf8(SceneXml()) }, { TEXT("Spot.gdtf"), Gdtf }, { TEXT("clamp.glb"), TArray<uint8>({ 1 }) } }, bDeflate);
		const FStageMvrParseResult Result = StageMvr::Parse(File);
		TestTrue(FString::Printf(TEXT("Archive (%s): valid"), bDeflate ? TEXT("deflate") : TEXT("stored")), Result.IsValid());
		TestTrue(TEXT("Archive: files listed in lower case"), Result.Scene.ArchiveFiles.Contains(TEXT("spot.gdtf")) && Result.Scene.ArchiveFiles.Contains(TEXT("generalscenedescription.xml")));
		TestFalse(TEXT("Archive: the GDTF file is found (any case)"), HasMessage(Result.Warnings, TEXT("GDTF file \"Spot.gdtf\" is not in the file")));
		TestTrue(TEXT("Archive: missing geometry is reported"), HasMessage(Result.Warnings, TEXT("Geometry \"truss.3ds\" is not in the file")));
		TestTrue(TEXT("Archive: 3DS is reported"), HasMessage(Result.Warnings, TEXT("3DS")));
		TestTrue(TEXT("Archive: the scene"), Result.Scene.Count(EStageMvrObjectKind::Fixture) == 2);
	}
	{
		const FStageMvrParseResult Missing = StageMvr::Parse(MakeZip({ { TEXT("GeneralSceneDescription.xml"), Utf8(SceneXml()) } }, true));
		TestTrue(TEXT("Archive: a missing GDTF file is reported, the file still reads"), Missing.IsValid() && HasMessage(Missing.Warnings, TEXT("GDTF file \"Spot.gdtf\" is not in the file")));
	}
	const FStageMvrParseResult Folder = StageMvr::Parse(MakeZip({ { TEXT("GeneralSceneDescription.xml"), Utf8(SceneXml()) }, { TEXT("models/truss.glb"), TArray<uint8>({ 1 }) } }, true));
	TestTrue(TEXT("Archive: a folder is refused"), !Folder.IsValid() && HasMessage(Folder.Errors, TEXT("archive root")));
	const FStageMvrParseResult NoScene = StageMvr::Parse(MakeZip({ { TEXT("description.xml"), Utf8(SpotXml()) } }, true));
	TestTrue(TEXT("Archive: a GDTF file is not an MVR file"), !NoScene.IsValid() && HasMessage(NoScene.Errors, TEXT("not an MVR file")));
	const TArray<uint8> Garbage = Utf8(TEXT("This is not a zip archive at all."));
	TestFalse(TEXT("Archive: garbage is refused"), StageMvr::Parse(Garbage).IsValid());
	TestFalse(TEXT("Archive: empty input is refused"), StageMvr::Parse(TArray<uint8>()).IsValid());
	StageMvr::FLimits Small;
	Small.MaxSceneBytes = 64;
	const FStageMvrParseResult TooBig = StageMvr::Parse(MakeZip({ { TEXT("GeneralSceneDescription.xml"), Utf8(SceneXml()) } }, true), Small);
	TestTrue(TEXT("Archive: an oversized scene is refused"), !TooBig.IsValid() && HasMessage(TooBig.Errors, TEXT("the limit is 64")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
