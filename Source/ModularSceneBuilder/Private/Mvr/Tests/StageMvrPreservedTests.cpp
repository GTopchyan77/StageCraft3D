// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for keeping the MVR data StageCraft does not use (Docs/ADR/0034 §18, MV5): what a fixture or truss keeps and what it leaves
// out, the canonical form, reading a kept fragment back as untrusted input, and the round trip through an export and a new import.
// Self-authored files only.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Mvr.Preserved; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Mvr/StageMvrArchive.h"
#include "Mvr/StageMvrImportPlan.h"
#include "Mvr/StageMvrParser.h"
#include "Mvr/StageMvrPreserved.h"
#include "Mvr/StageMvrWriter.h"

namespace StageMvrPreservedTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	const TCHAR* const IdLayer = TEXT("7A000000-0000-4000-8000-000000000001");
	const TCHAR* const IdSpot = TEXT("7A000000-0000-4000-8000-000000000002");
	const TCHAR* const IdTruss = TEXT("7A000000-0000-4000-8000-000000000003");
	const TCHAR* const IdFocus = TEXT("7A000000-0000-4000-8000-000000000004");
	const TCHAR* const IdPlain = TEXT("7A000000-0000-4000-8000-000000000005");

	/**
	 * A fixture with everything an exporter may put on it: what StageCraft writes itself, references it cannot carry (Focus, Gobo,
	 * Classing, Connections), and data it keeps (a vendor attribute, CastShadow, Color, CustomId, UnitNumber, nested CustomCommands
	 * with text that needs escaping, a vendor element with attributes). A truss with its own GDTF file and Alignments; a plain one.
	 */
	FString SceneXml()
	{
		return FString::Printf(TEXT(
			"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
			"<GeneralSceneDescription verMajor=\"1\" verMinor=\"6\" provider=\"tests\">\n"
			"<Scene><Layers><Layer uuid=\"%s\" name=\"Rig\"><ChildList>\n"
			" <Fixture uuid=\"%s\" name=\"Spot\" vendor:tag=\"A &amp; B\" multipatch=\"\">\n"
			"  <Matrix>{1,0,0}{0,1,0}{0,0,1}{0,0,5000}</Matrix><GDTFSpec>Spot.gdtf</GDTFSpec><GDTFMode>Standard</GDTFMode>\n"
			"  <Focus>%s</Focus><Classing>7A000000-0000-4000-8000-0000000000AA</Classing>\n"
			"  <Addresses><Address break=\"0\">1.1</Address></Addresses><FixtureID>12</FixtureID><FixtureIDNumeric>12</FixtureIDNumeric>\n"
			"  <UnitNumber>7</UnitNumber><CustomId>44</CustomId><CastShadow>true</CastShadow><Color>0.3127,0.3290,100</Color>\n"
			"  <Gobo rotation=\"30\">gobo.png</Gobo><Connections><Connection own=\"In\" other=\"Out\" toObject=\"7A000000-0000-4000-8000-0000000000BB\"/></Connections>\n"
			"  <CustomCommands><CustomCommand>Body_Pan,f 50 &lt; 60</CustomCommand><CustomCommand>Yoke_Tilt,f 10</CustomCommand></CustomCommands>\n"
			"  <VendorData a=\"1\" b='two'/>\n"
			" </Fixture>\n"
			" <FocusPoint uuid=\"%s\" name=\"DSC\"><Matrix>{1,0,0}{0,1,0}{0,0,1}{0,2000,0}</Matrix></FocusPoint>\n"
			" <Truss uuid=\"%s\" name=\"Pipe\"><Matrix>{1,0,0}{0,1,0}{0,0,1}{0,0,6000}</Matrix><GDTFSpec>Pipe.gdtf</GDTFSpec><GDTFMode>Default</GDTFMode>\n"
			"  <Geometries><Geometry3D fileName=\"pipe.glb\"/></Geometries><Alignments><Alignment geometry=\"Beam\" up=\"0,0,1\" direction=\"0,0,-1\"/></Alignments></Truss>\n"
			" <Fixture uuid=\"%s\" name=\"Plain\"><Matrix>{1,0,0}{0,1,0}{0,0,1}{1000,0,5000}</Matrix></Fixture>\n"
			"</ChildList></Layer></Layers></Scene></GeneralSceneDescription>\n"),
			IdLayer, IdSpot, IdFocus, IdFocus, IdTruss, IdPlain);
	}

	const FStageMvrObject* Find(const FStageMvrScene& Scene, const TCHAR* Id)
	{
		FGuid Guid;
		FGuid::Parse(Id, Guid);
		return Scene.FindObject(Guid);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageMvrPreservedTest, "StageCraft.Mvr.Preserved", StageMvrPreservedTests::Flags)

bool FStageMvrPreservedTest::RunTest(const FString& Parameters)
{
	using namespace StageMvrPreservedTests;

	const FStageMvrParseResult Parsed = StageMvr::ParseSceneDescription(SceneXml());
	if (!TestTrue(FString::Printf(TEXT("The scene parses (%s)"), *FString::Join(Parsed.Errors, TEXT("; "))), Parsed.IsValid()))
	{
		return false;
	}
	const FStageMvrObject* Spot = Find(Parsed.Scene, IdSpot);
	const FStageMvrObject* Truss = Find(Parsed.Scene, IdTruss);
	const FStageMvrObject* Plain = Find(Parsed.Scene, IdPlain);
	const FStageMvrObject* Focus = Find(Parsed.Scene, IdFocus);
	if (!TestTrue(TEXT("Every object is read"), Spot && Truss && Plain && Focus))
	{
		return false;
	}
	TestEqual(TEXT("Two objects keep data (the plain fixture has none; a focus point is never captured)"), Parsed.Scene.PreservedObjects, 2);
	TestTrue(TEXT("The plain fixture and the focus point keep nothing"), Plain->Preserved.IsEmpty() && Focus->Preserved.IsEmpty());

	// What the fixture keeps, in the canonical form: escaped, in document order, attributes first.
	const FString& Kept = Spot->Preserved;
	TestTrue(*FString::Printf(TEXT("Wrapped in <Keep> with the vendor attribute escaped (%s)"), *Kept), Kept.StartsWith(TEXT("<Keep vendor:tag=\"A &amp; B\">")) && Kept.EndsWith(TEXT("</Keep>")));
	for (const TCHAR* Expected : { TEXT("<UnitNumber>7</UnitNumber>"), TEXT("<CustomId>44</CustomId>"), TEXT("<CastShadow>true</CastShadow>"),
		TEXT("<Color>0.3127,0.3290,100</Color>"), TEXT("<CustomCommands><CustomCommand>Body_Pan,f 50 &lt; 60</CustomCommand><CustomCommand>Yoke_Tilt,f 10</CustomCommand></CustomCommands>"),
		TEXT("<VendorData a=\"1\" b=\"two\"/>") })
	{
		TestTrue(*FString::Printf(TEXT("Kept: %s"), Expected), Kept.Contains(Expected));
	}
	for (const TCHAR* Left : { TEXT("Matrix"), TEXT("GDTFSpec"), TEXT("GDTFMode"), TEXT("Addresses"), TEXT("FixtureID>"), TEXT("FixtureIDNumeric"),
		TEXT("Focus"), TEXT("Classing"), TEXT("Gobo"), TEXT("Connections"), TEXT("multipatch"), TEXT("uuid"), TEXT("name=") })
	{
		TestFalse(*FString::Printf(TEXT("Not kept: %s"), Left), Kept.Contains(Left));
	}
	TestTrue(TEXT("The order of the file is kept"), Kept.Find(TEXT("UnitNumber")) < Kept.Find(TEXT("CastShadow")) && Kept.Find(TEXT("CastShadow")) < Kept.Find(TEXT("VendorData")));
	TestTrue(TEXT("References that cannot be carried are named in the report (not Focus: it aims the fixture)"),
		Parsed.Warnings.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("<Gobo>")) && Line.Contains(TEXT("not kept for export")); })
		&& Parsed.Warnings.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("<Classing>")); })
		&& !Parsed.Warnings.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("<Focus>")); }));
	TestTrue(*FString::Printf(TEXT("The truss keeps its Alignments, not its GDTF file (%s)"), *Truss->Preserved),
		Truss->Preserved.Contains(TEXT("<Alignments><Alignment geometry=\"Beam\" up=\"0,0,1\" direction=\"0,0,-1\"/></Alignments>"))
		&& !Truss->Preserved.Contains(TEXT("GDTF")) && !Truss->Preserved.Contains(TEXT("Geometries")));

	// Reading back: the canonical form reads; anything off is refused whole (scene files are untrusted).
	StageMvrPreserved::FForWriting ForWriting;
	TestTrue(TEXT("The kept fragment reads back"), StageMvrPreserved::ReadForWriting(Kept, EStageMvrObjectKind::Fixture, ForWriting));
	TestEqual(TEXT("...with its attribute"), ForWriting.Attributes, FString(TEXT(" vendor:tag=\"A &amp; B\"")));
	TestTrue(TEXT("...and its six elements, UnitNumber among them"), ForWriting.Children.Num() == 6 && ForWriting.ChildNames.Contains(TEXT("UnitNumber")));
	const auto Refused = [](const FString& Fragment, EStageMvrObjectKind Kind = EStageMvrObjectKind::Fixture)
	{
		StageMvrPreserved::FForWriting Out;
		return !StageMvrPreserved::ReadForWriting(Fragment, Kind, Out);
	};
	TestTrue(TEXT("Empty is nothing to write"), Refused(FString()));
	TestTrue(TEXT("Not well-formed"), Refused(TEXT("<Keep><CastShadow>true</Keep>")));
	TestTrue(TEXT("Another root"), Refused(TEXT("<Fixture><CastShadow>true</CastShadow></Fixture>")));
	TestTrue(TEXT("An element the writer owns"), Refused(TEXT("<Keep><Matrix>{1,0,0}{0,1,0}{0,0,1}{0,0,0}</Matrix></Keep>")));
	TestTrue(TEXT("An attribute the writer owns"), Refused(TEXT("<Keep uuid=\"7A000000-0000-4000-8000-000000000099\"/>")));
	TestTrue(TEXT("A reference that would dangle"), Refused(TEXT("<Keep><Gobo>gobo.png</Gobo></Keep>")));
	TestTrue(TEXT("A truss's GDTF file"), Refused(TEXT("<Keep><GDTFSpec>Pipe.gdtf</GDTFSpec></Keep>"), EStageMvrObjectKind::Truss));
	TestTrue(TEXT("A DOCTYPE (entity expansion)"), Refused(TEXT("<!DOCTYPE Keep [<!ENTITY x \"y\">]><Keep><A>&x;</A></Keep>")));
	TestTrue(TEXT("Longer than the cap"), Refused(TEXT("<Keep><A>") + FString::ChrN(StageMvrPreserved::MaxChars, TEXT('x')) + TEXT("</A></Keep>")));

	// Over the cap on import: dropped and reported, never cut.
	StageMvr::FLimits Limits;
	FString Many;
	for (int32 Index = 0; Index < 6; ++Index)
	{
		Many += TEXT("<VendorData>") + FString::ChrN(3000, TEXT('x')) + TEXT("</VendorData>");
	}
	const FStageMvrParseResult Big = StageMvr::ParseSceneDescription(SceneXml().Replace(TEXT("<VendorData a=\"1\" b='two'/>"), *Many), Limits);
	const FStageMvrObject* BigSpot = Big.IsValid() ? Find(Big.Scene, IdSpot) : nullptr;
	TestTrue(TEXT("A fixture with more than the cap keeps nothing and says so"), BigSpot && BigSpot->Preserved.IsEmpty()
		&& Big.Warnings.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("not kept for export")) && Line.Contains(TEXT("Spot")); }));

	// The round trip: written back by the export, read by a new import, kept the same.
	StageMvrExport::FScene Scene;
	Scene.Layers.Add({ FGuid::NewGuid(), TEXT("Rig") });
	StageMvrExport::FFixture& Fixture = Scene.Fixtures.AddDefaulted_GetRef();
	Fixture.Id = Spot->Id;
	Fixture.Name = TEXT("Spot");
	Fixture.Transform = StageMvrImport::FixtureTransform(Spot->World);
	Fixture.FixtureId = 12;
	Fixture.Preserved = Spot->Preserved;
	StageMvrExport::FFixture& Damaged = Scene.Fixtures.AddDefaulted_GetRef();
	Damaged.Id = Plain->Id;
	Damaged.Name = TEXT("Plain");
	Damaged.Preserved = TEXT("<Keep><Broken></Keep>");
	StageMvrExport::FBoxObject& Pipe = Scene.Objects.AddDefaulted_GetRef();
	Pipe.Kind = StageMvrExport::EBoxKind::Truss;
	Pipe.Id = Truss->Id;
	Pipe.Name = TEXT("Pipe");
	Pipe.LocalBounds = FBox(FVector(0, -15, 0), FVector(300, 15, 30));
	Pipe.Preserved = Truss->Preserved;
	const StageMvrExport::FWriteResult Written = StageMvrExport::Write(Scene, {});
	if (!TestTrue(FString::Printf(TEXT("The export writes (%s)"), *Written.Error), Written.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("Two objects written with their kept data"), Written.PreservedObjects, 2);
	TestTrue(TEXT("The damaged fragment is named and left out"), Written.Notes.ContainsByPredicate([](const FString& Note) { return Note.StartsWith(TEXT("Plain:")) && Note.Contains(TEXT("could not be read back")); }));

	const FStageMvrArchive Back = StageMvrArchive::Read(Written.Bytes);
	if (!TestTrue(FString::Printf(TEXT("The exported file reads back (%s)"), *FString::Join(Back.Parsed.Errors, TEXT("; "))), Back.Parsed.IsValid()))
	{
		return false;
	}
	const FStageMvrObject* SpotBack = Find(Back.Parsed.Scene, IdSpot);
	const FStageMvrObject* TrussBack = Find(Back.Parsed.Scene, IdTruss);
	const FStageMvrObject* PlainBack = Find(Back.Parsed.Scene, IdPlain);
	TestTrue(TEXT("The fixture keeps exactly what it kept before"), SpotBack && SpotBack->Preserved == Spot->Preserved);
	TestTrue(TEXT("...its own UnitNumber 7, not the default 0, and no second one"), SpotBack && SpotBack->Preserved.Contains(TEXT("<UnitNumber>7</UnitNumber>")));
	TestTrue(TEXT("The truss keeps its Alignments"), TrussBack && TrussBack->Preserved == Truss->Preserved);
	TestTrue(TEXT("The damaged one is written plainly; its default UnitNumber 0 is not kept again"), PlainBack && PlainBack->Preserved.IsEmpty());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
