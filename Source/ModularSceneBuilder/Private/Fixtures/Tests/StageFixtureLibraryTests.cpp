// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Data/StageItemTypes.h"
#include "Data/StageParameterTypes.h"
#include "Engine/StaticMesh.h"
#include "Fixtures/StageFixtureLibraryRules.h"
#include "Fixtures/StageFixtureLibrarySubsystem.h"
#include "Fixtures/StageGdtfFixtureBuilder.h"
#include "Fixtures/StageGdtfMapping.h"
#include "Fixtures/StageGdtfParser.h"
#include "Fixtures/Tests/StageGdtfTestFiles.h"
#include "HAL/FileManager.h"
#include "Marketplace/StagePackage.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "PhysicsEngine/BodySetup.h"
#include "Scene/StageSceneStore.h"
#include "Show/StageFixtureCells.h"
#include "Subsystems/StageItemSubsystem.h"
#include "Tests/StageTestGameInstance.h"
#include "UObject/Package.h"

namespace StageFixtureLibraryTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	const TCHAR* const BarId = TEXT("0A1B2C3D-4E5F-4061-8273-94A5B6C7D8E9");

	FGuid ParseId(const TCHAR* Text)
	{
		FGuid Id;
		FGuid::Parse(Text, Id);
		return Id;
	}

	struct FBuiltFile
	{
		FStageGdtfParseResult Parsed;
		TArray<FStageGdtfModeLayout> Layouts;
		FStageGdtfCapabilities Capabilities;
	};

	FBuiltFile ParseAndMap(const FString& Xml)
	{
		FBuiltFile File;
		File.Parsed = StageGdtf::Parse(StageGdtfTestFiles::MakeGdtf(Xml));
		for (int32 Mode = 0; File.Parsed.IsOk() && Mode < File.Parsed.Type.Modes.Num(); ++Mode)
		{
			File.Layouts.Add(StageGdtfMapping::BuildModeLayout(File.Parsed.Type, Mode));
		}
		File.Capabilities = StageGdtfMapping::DeriveCapabilities(File.Parsed.Type, File.Layouts);
		return File;
	}

	ULightingFixtureData* Build(const FBuiltFile& File, int32 Mode, TOptional<EStageFixtureKind> Kind = {})
	{
		ULightingFixtureData* Data = NewObject<ULightingFixtureData>(GetTransientPackage(), NAME_None, RF_Transient);
		StageGdtfFixtureBuilder::Fill(*Data, File.Parsed.Type, File.Capabilities, File.Layouts[Mode], Kind, nullptr);
		return Data;
	}

	const FStageFixtureDMXChannel* FindChannel(const ULightingFixtureData& Data, int32 Channel)
	{
		return Data.DMXChannels.FindByPredicate([Channel](const FStageFixtureDMXChannel& Candidate) { return Candidate.Channel == Channel; });
	}

	bool SlotsUsedOnce(const ULightingFixtureData& Data)
	{
		TArray<int32> Uses;
		Uses.SetNumZeroed(Data.GetDMXFootprint() + 1);
		for (const FStageFixtureDMXChannel& Channel : Data.DMXChannels)
		{
			for (int32 Slot = Channel.Channel; Slot < Channel.Channel + Channel.GetWidth(); ++Slot)
			{
				if (!Uses.IsValidIndex(Slot))
				{
					return false;
				}
				++Uses[Slot];
			}
		}
		for (int32 Slot = 1; Slot < Uses.Num(); ++Slot)
		{
			if (Uses[Slot] != 1)
			{
				return false;
			}
		}
		return true;
	}

	FStageItemSnapshot MakeLibraryItem(const FGuid& Id, const FString& Slug, const FString& Mode)
	{
		FStageItemSnapshot Item;
		Item.InstanceId = FGuid::NewGuid();
		Item.Item = TSoftObjectPtr<UBaseItemData>(StageFixtureLibrary::MakeItemPath(Id, Slug));
		Item.DisplayName = FText::FromString(TEXT("Spot"));
		Item.FixtureType.Id = Id;
		Item.FixtureType.Manufacturer = TEXT("StageCraft");
		Item.FixtureType.Name = TEXT("Test Spot");
		Item.FixtureType.Mode = Mode;
		return Item;
	}

	FString MakeRoot(const TCHAR* Name)
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::AutomationTransientDir(), Name, FGuid::NewGuid().ToString()));
	}

	FString Str(EStageSceneResult Result) { return UEnum::GetValueAsString(Result); }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageFixturesLibraryRulesTest, "StageCraft.Fixtures.LibraryRules", StageFixtureLibraryTests::Flags)

bool FStageFixturesLibraryRulesTest::RunTest(const FString& Parameters)
{
	using namespace StageFixtureLibraryTests;

	const TArray<FString> Slugs = StageFixtureLibrary::MakeModeSlugs({ TEXT("Mode 1"), TEXT("Mode_1"), TEXT("mode 1"), TEXT(""), TEXT("§§"), TEXT("  Extended (24ch)  ") });
	TestEqual(TEXT("Spaces become one underscore"), Slugs[0], FString(TEXT("Mode_1")));
	TestEqual(TEXT("A repeat gets _2"), Slugs[1], FString(TEXT("Mode_1_2")));
	TestEqual(TEXT("Repeats compare without case"), Slugs[2], FString(TEXT("mode_1_3")));
	TestEqual(TEXT("Nothing left is Mode"), Slugs[3], FString(TEXT("Mode")));
	TestEqual(TEXT("and repeats like any slug"), Slugs[4], FString(TEXT("Mode_2")));
	TestEqual(TEXT("Punctuation and edges dropped"), Slugs[5], FString(TEXT("Extended_24ch")));
	TestEqual(TEXT("The same names give the same slugs"), StageFixtureLibrary::MakeModeSlugs({ TEXT("Mode 1"), TEXT("Mode_1"), TEXT("mode 1"), TEXT(""), TEXT("§§"), TEXT("  Extended (24ch)  ") }), Slugs);
	TestTrue(TEXT("Long names are cut"), StageFixtureLibrary::MakeModeSlugs({ FString::ChrN(200, TEXT('A')) })[0].Len() == StageFixtureLibrary::MaxSlugLength);
	TestTrue(TEXT("A cut repeat keeps its suffix within the limit"), StageFixtureLibrary::MakeModeSlugs({ FString::ChrN(200, TEXT('A')), FString::ChrN(201, TEXT('A')) })[1].EndsWith(TEXT("_2")));

	const FGuid SpotId = ParseId(StageGdtfTestFiles::SpotId);
	const FSoftObjectPath Path = StageFixtureLibrary::MakeItemPath(SpotId, TEXT("Standard"));
	TestEqual(TEXT("Stable item path"), Path.ToString(), FString(TEXT("/Temp/StageCraftFixtures/6C9F1A2B3D4E4F508A617B8C9D0E1F20/Standard.Standard")));
	TestTrue(TEXT("A library path"), StageFixtureLibrary::IsFixtureItemPath(Path.GetLongPackageName()));
	TestFalse(TEXT("Not hex"), StageFixtureLibrary::IsFixtureItemPath(TEXT("/Temp/StageCraftFixtures/XYZ/Standard")));
	TestFalse(TEXT("Short id"), StageFixtureLibrary::IsFixtureItemPath(TEXT("/Temp/StageCraftFixtures/6C9F1A2B/Standard")));
	TestFalse(TEXT("Deeper path"), StageFixtureLibrary::IsFixtureItemPath(TEXT("/Temp/StageCraftFixtures/6C9F1A2B3D4E4F508A617B8C9D0E1F20/A/B")));
	TestFalse(TEXT("Relative segments"), StageFixtureLibrary::IsFixtureItemPath(TEXT("/Temp/StageCraftFixtures/6C9F1A2B3D4E4F508A617B8C9D0E1F20/..")));
	TestFalse(TEXT("No slug"), StageFixtureLibrary::IsFixtureItemPath(TEXT("/Temp/StageCraftFixtures/6C9F1A2B3D4E4F508A617B8C9D0E1F20/")));
	TestFalse(TEXT("Other roots"), StageFixtureLibrary::IsFixtureItemPath(TEXT("/Game/StageCraft/Items/DA_Test_Light")));
	TestFalse(TEXT("Case of the root matters"), StageFixtureLibrary::IsFixtureItemPath(TEXT("/temp/stagecraftfixtures/6C9F1A2B3D4E4F508A617B8C9D0E1F20/Standard")));

	FGuid FileId;
	TestTrue(TEXT("Library file name"), StageFixtureLibrary::ParseLibraryFileName(TEXT("6C9F1A2B3D4E4F508A617B8C9D0E1F20.gdtf"), FileId) && FileId == SpotId);
	TestEqual(TEXT("and back"), FPaths::GetCleanFilename(StageFixtureLibrary::GetLibraryFilePath(SpotId)), FString(TEXT("6C9F1A2B3D4E4F508A617B8C9D0E1F20.gdtf")));
	TestFalse(TEXT("A user's file name"), StageFixtureLibrary::ParseLibraryFileName(TEXT("Robe MegaPointe.gdtf"), FileId));
	TestFalse(TEXT("Other extensions"), StageFixtureLibrary::ParseLibraryFileName(TEXT("6C9F1A2B3D4E4F508A617B8C9D0E1F20.zip"), FileId));

	AddInfo(TEXT("The index file round-trips; damaged entries are skipped, unknown formats refused."));
	StageFixtureLibrary::FIndexEntry Corrected;
	Corrected.FixtureTypeId = SpotId;
	Corrected.SourceFileName = TEXT("Test Spot.gdtf");
	Corrected.ImportedUtc = FDateTime(2026, 11, 9, 10, 30, 0);
	Corrected.Kind = EStageFixtureKind::MovingHeadHybrid;
	StageFixtureLibrary::FIndexEntry Plain;
	Plain.FixtureTypeId = ParseId(BarId);
	Plain.SourceFileName = TEXT("Bar.gdtf");
	TArray<StageFixtureLibrary::FIndexEntry> Read;
	int32 Skipped = 0;
	TestTrue(TEXT("Index reads"), StageFixtureLibrary::ReadIndex(StageFixtureLibrary::WriteIndex({ Corrected, Plain }), Read, Skipped));
	if (TestEqual(TEXT("Two entries"), Read.Num(), 2))
	{
		TestTrue(TEXT("Id, file, time and kind kept"), Read[0].FixtureTypeId == SpotId && Read[0].SourceFileName == Corrected.SourceFileName
			&& Read[0].ImportedUtc == Corrected.ImportedUtc && Read[0].Kind == TOptional<EStageFixtureKind>(EStageFixtureKind::MovingHeadHybrid));
		TestFalse(TEXT("No kind stays unset"), Read[1].Kind.IsSet());
	}
	const FString Damaged = FString::Printf(TEXT("{\"format\":\"StageCraftFixtureLibrary\",\"version\":1,\"fixtures\":[{\"id\":\"nope\"},{\"id\":\"%s\",\"kind\":\"Toaster\"},")
		TEXT("{\"id\":\"%s\",\"sourceFile\":\"C:/Users/x/../../a.gdtf\"},{\"id\":\"%s\"},7]}"), StageGdtfTestFiles::SpotId, StageGdtfTestFiles::SpotId, StageGdtfTestFiles::SpotId);
	TestTrue(TEXT("Damaged index still reads"), StageFixtureLibrary::ReadIndex(Damaged, Read, Skipped));
	TestEqual(TEXT("one good entry"), Read.Num(), 1);
	TestEqual(TEXT("four skipped (bad id, bad kind, repeat, not an object)"), Skipped, 4);
	TestEqual(TEXT("A file name only, never a path"), Read.IsEmpty() ? FString() : Read[0].SourceFileName, FString(TEXT("a.gdtf")));
	TestFalse(TEXT("Another format"), StageFixtureLibrary::ReadIndex(TEXT("{\"format\":\"Other\",\"version\":1,\"fixtures\":[]}"), Read, Skipped));
	TestFalse(TEXT("A newer version"), StageFixtureLibrary::ReadIndex(TEXT("{\"format\":\"StageCraftFixtureLibrary\",\"version\":2,\"fixtures\":[]}"), Read, Skipped));
	TestFalse(TEXT("Not JSON"), StageFixtureLibrary::ReadIndex(TEXT("fixtures"), Read, Skipped));

	AddInfo(TEXT("Missing types are named by type and mode, with how many fixtures use them."));
	TArray<FStageItemSnapshot> Unplaced;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		Unplaced.Add(MakeLibraryItem(SpotId, TEXT("Standard"), TEXT("Standard")));
	}
	Unplaced.Add(MakeLibraryItem(SpotId, TEXT("Basic"), TEXT("Basic")));
	FStageItemSnapshot Content;
	Content.InstanceId = FGuid::NewGuid();
	Content.Item = TSoftObjectPtr<UBaseItemData>(FSoftObjectPath(TEXT("/Game/StageCraft/Items/DA_Test_Crate.DA_Test_Crate")));
	Unplaced.Add(Content);
	const FString Message = StageFixtureLibrary::DescribeMissingTypes(Unplaced).ToString();
	TestTrue(FString::Printf(TEXT("Three of one mode (%s)"), *Message), Message.Contains(TEXT("StageCraft Test Spot (Standard) is missing. Import its GDTF file to see these 3 fixtures.")));
	TestTrue(TEXT("One of another"), Message.Contains(TEXT("StageCraft Test Spot (Basic) is missing. Import its GDTF file to see this fixture.")));
	TestTrue(TEXT("Listing is capped"), StageFixtureLibrary::DescribeMissingTypes(Unplaced, 1).ToString().EndsWith(TEXT("One more fixture type is missing.")));
	TestTrue(TEXT("Nothing missing, nothing said"), StageFixtureLibrary::DescribeMissingTypes({ Content }).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageFixturesBuilderTest, "StageCraft.Fixtures.Builder", StageFixtureLibraryTests::Flags)

bool FStageFixturesBuilderTest::RunTest(const FString& Parameters)
{
	using namespace StageFixtureLibraryTests;
	using namespace StageCraftTags;

	const FBuiltFile Spot = ParseAndMap(StageGdtfTestFiles::SpotXml());
	if (!TestTrue(TEXT("Spot parses"), Spot.Parsed.IsOk()) || !TestEqual(TEXT("three layouts"), Spot.Layouts.Num(), 3))
	{
		return false;
	}
	ULightingFixtureData* Standard = Build(Spot, 0);
	TestEqual(TEXT("Name with mode"), Standard->DisplayName.ToString(), FString(TEXT("StageCraft Test Spot (Standard)")));
	TestTrue(TEXT("A library type"), Standard->IsLibraryType() && Standard->FixtureTypeId == ParseId(StageGdtfTestFiles::SpotId));
	TestTrue(TEXT("From GDTF, type name kept"), Standard->SourceFormat == StageFixtureLibrary::GdtfFormat && Standard->SourceTypeName == TEXT("Test Spot"));
	TestFalse(TEXT("Needs no asset streaming"), Standard->RequiresAssetStreaming());
	TestEqual(TEXT("Mode name"), Standard->DMXModeName.ToString(), FString(TEXT("Standard")));
	TestEqual(TEXT("Real footprint"), Standard->GetDMXFootprint(), 15);
	TestEqual(TEXT("Twelve channels"), Standard->DMXChannels.Num(), 12);
	TestTrue(TEXT("Every slot used once"), SlotsUsedOnce(*Standard));
	const FStageFixtureDMXChannel* Pan = FindChannel(*Standard, 1);
	TestTrue(TEXT("Pan 16-bit on 1"), Pan && Pan->Attribute == Attribute_Pan && Pan->b16Bit && Pan->Label == TEXT("Pan"));
	const FStageFixtureDMXChannel* Zoom = FindChannel(*Standard, 11);
	TestTrue(TEXT("Zoom 16-bit on 11"), Zoom && Zoom->Attribute == Attribute_Zoom && Zoom->b16Bit);
	const FStageFixtureDMXChannel* Focus = FindChannel(*Standard, 13);
	TestTrue(TEXT("Focus keeps its slot without an attribute"), Focus && !Focus->Attribute.IsValid() && Focus->Label == TEXT("Focus1"));
	const FStageFixtureDMXChannel* Control = FindChannel(*Standard, 14);
	TestTrue(TEXT("Control keeps its slot without an attribute"), Control && !Control->Attribute.IsValid() && Control->Label == TEXT("Control1"));
	TestTrue(TEXT("Moving head with pan and tilt"), Standard->bHasPanTilt && Standard->SupportsAttribute(Attribute_Pan) && Standard->SupportsAttribute(Attribute_Zoom));
	TestTrue(TEXT("Category from the kind"), Standard->CategoryTag == Category_Lighting_MovingHead);
	TestEqual(TEXT("Manufacturer"), Standard->Specs.Manufacturer.ToString(), FString(TEXT("StageCraft")));
	TestEqual(TEXT("Gobos from the wheel"), Standard->GoboWheel.Num(), Spot.Capabilities.GoboWheel.Num());
	TestTrue(TEXT("Model sizes give the head"), Standard->Body.HeadSizeCm.Equals(Spot.Capabilities.HeadSizeCm.ComponentMax(FVector(1.0)), 0.01));
	TestTrue(TEXT("The beam leaves above the tilt pivot"), Standard->BeamOriginOffset.Z > 0.0 && Standard->TiltPivotOffset.Z > Standard->PanPivotOffset.Z);
	TestTrue(TEXT("Engine shapes for yoke and head"), Standard->YokeMesh.ToSoftObjectPath().ToString() == TEXT("/Engine/BasicShapes/Cube.Cube")
		&& Standard->HeadMesh.ToSoftObjectPath().ToString() == TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	TestEqual(TEXT("Basic mode: 3 channels"), Build(Spot, 1)->GetDMXFootprint(), 3);

	ULightingFixtureData* Corrected = Build(Spot, 0, EStageFixtureKind::Strobe);
	TestTrue(TEXT("A kind correction moves the category"), Corrected->FixtureKind == EStageFixtureKind::Strobe && Corrected->CategoryTag == Category_Lighting_Strobe);

	AddInfo(TEXT("Channel shapes the layout can have."));
	FStageGdtfModeLayout Layout;
	FStageGdtfLayoutChannel& Split = Layout.Channels.AddDefaulted_GetRef();
	Split.Offsets = { 5, 9 };
	Split.Attribute = Attribute_Dimmer;
	Split.Status = EStageGdtfChannelStatus::Mapped;
	Split.GdtfAttribute = TEXT("Dimmer");
	FStageGdtfLayoutChannel& Wide = Layout.Channels.AddDefaulted_GetRef();
	Wide.Offsets = { 1, 2, 3 };
	Wide.Attribute = Attribute_Pan;
	Wide.Status = EStageGdtfChannelStatus::Approximated;
	Wide.GdtfAttribute = TEXT("Pan");
	FStageGdtfLayoutChannel& Virtual = Layout.Channels.AddDefaulted_GetRef();
	Virtual.Status = EStageGdtfChannelStatus::Virtual;
	FStageGdtfLayoutChannel& Unmapped = Layout.Channels.AddDefaulted_GetRef();
	Unmapped.Offsets = { 6, 7 };
	Unmapped.Attribute = Attribute_Iris;
	Unmapped.Status = EStageGdtfChannelStatus::Default;
	Unmapped.GdtfAttribute = TEXT("Fan");
	const TArray<FStageFixtureDMXChannel> Channels = StageGdtfFixtureBuilder::MakeDMXChannels(Layout);
	TArray<int32> Order;
	for (const FStageFixtureDMXChannel& Channel : Channels)
	{
		Order.Add(Channel.Channel);
	}
	TestEqual(TEXT("Sorted, virtual channels left out"), Order, TArray<int32>({ 1, 3, 5, 6, 9 }));
	if (Channels.Num() == 5)
	{
		TestTrue(TEXT("24-bit: 16-bit coarse and fine, then the third byte without an attribute"), Channels[0].b16Bit && Channels[0].Attribute == Attribute_Pan && !Channels[1].Attribute.IsValid());
		TestTrue(TEXT("Fine apart from coarse: an 8-bit coarse"), !Channels[2].b16Bit && Channels[2].Attribute == Attribute_Dimmer && !Channels[4].Attribute.IsValid());
		TestTrue(TEXT("Default status never drives an attribute"), Channels[3].b16Bit && !Channels[3].Attribute.IsValid() && Channels[3].Label == TEXT("Fan"));
	}

	AddInfo(TEXT("A 64-cell bar (ADR 0033 G4)."));
	const FBuiltFile Bar = ParseAndMap(StageGdtfTestFiles::BarXml(StageFixtureCells::MaxCells));
	if (TestTrue(TEXT("64-cell bar maps"), Bar.Layouts.Num() == 1 && Bar.Layouts[0].IsUsable()))
	{
		ULightingFixtureData* Pixels = Build(Bar, 0);
		TestEqual(TEXT("64 cells"), Pixels->CellCount, 64);
		TestEqual(TEXT("Footprint 1 + 64 x 3"), Pixels->GetDMXFootprint(), 193);
		TestTrue(TEXT("Every slot used once"), SlotsUsedOnce(*Pixels));
		const FGameplayTag LastBlue = StageFixtureCells::GetCellAttribute(63, EStageCellChannel::ColorB);
		const FStageFixtureDMXChannel* Last = FindChannel(*Pixels, 193);
		TestTrue(TEXT("Cell 64 blue on 193"), LastBlue.IsValid() && Last && Last->Attribute == LastBlue);
		TestTrue(TEXT("Cell 64 is an attribute of the fixture"), Pixels->SupportsAttribute(LastBlue) && Pixels->GetSupportedAttributes().Contains(LastBlue));
		TestTrue(TEXT("A pixel bar with a box head and a cell strip"), Pixels->FixtureKind == EStageFixtureKind::PixelBar
			&& Pixels->HeadMesh.ToSoftObjectPath().ToString() == TEXT("/Engine/BasicShapes/Cube.Cube") && Pixels->Body.CellStripSizeCm.X > 0.0);
		TestTrue(TEXT("Cell values fit the scene and programmer limits"), Pixels->GetSupportedAttributes().Num() <= FStageSceneStore::MaxAttributesPerFixture);
	}

	AddInfo(TEXT("The base mesh is built at its size, with a collision box to pick it by."));
	UStaticMesh* Base = StageGdtfFixtureBuilder::BuildBaseMesh(*GetTransientPackage(), NAME_None, FVector(40.0, 30.0, 15.0));
	if (TestNotNull(TEXT("Base mesh built"), Base))
	{
		TestTrue(TEXT("Bounds are the size"), Base->GetBounds().BoxExtent.Equals(FVector(20.0, 15.0, 7.5), 0.01));
		TestTrue(TEXT("Simple collision for picking"), Base->GetBodySetup() && Base->GetBodySetup()->AggGeom.GetElementCount() > 0);
	}
	TestNull(TEXT("No size, no mesh"), StageGdtfFixtureBuilder::BuildBaseMesh(*GetTransientPackage(), NAME_None, FVector::ZeroVector));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageFixturesLibraryTest, "StageCraft.Fixtures.Library", StageFixtureLibraryTests::Flags)

bool FStageFixturesLibraryTest::RunTest(const FString& Parameters)
{
	using namespace StageFixtureLibraryTests;

	const FString Root = MakeRoot(TEXT("StageFixtureLibrary"));
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, /*RequireExists*/ false, /*Tree*/ true); };
	const FString Source = FPaths::Combine(Root, TEXT("Downloads"));
	const FString SpotFile = FPaths::Combine(Source, TEXT("Test Spot.gdtf"));
	const FString BarFile = FPaths::Combine(Source, TEXT("Test Bar.gdtf"));
	const FString DamagedFile = FPaths::Combine(Source, TEXT("Damaged.gdtf"));
	const FString TextFile = FPaths::Combine(Source, TEXT("Notes.txt"));
	FFileHelper::SaveArrayToFile(StageGdtfTestFiles::MakeGdtf(StageGdtfTestFiles::SpotXml()), *SpotFile);
	FFileHelper::SaveArrayToFile(StageGdtfTestFiles::MakeGdtf(StageGdtfTestFiles::BarXml(StageFixtureCells::MaxCells)), *BarFile);
	FFileHelper::SaveArrayToFile(TArray<uint8>({ 'P', 'K', 3, 4, 0, 0, 0 }), *DamagedFile);
	FFileHelper::SaveStringToFile(TEXT("not a fixture"), *TextFile);

	const FGuid SpotId = ParseId(StageGdtfTestFiles::SpotId);
	const FGuid BarTypeId = ParseId(BarId);
	FStageTestGameInstanceOptions Options;
	Options.Root = Root;
	TArray<FString> FirstRunPaths;

	{
		FStageTestGameInstance Game(Options);
		UStageFixtureLibrarySubsystem* Library = Game.Subsystem<UStageFixtureLibrarySubsystem>();
		UStageItemSubsystem* Items = Game.Subsystem<UStageItemSubsystem>();
		if (!TestTrue(TEXT("The test game instance starts"), Game.IsValid()) || !TestNotNull(TEXT("Library"), Library) || !TestNotNull(TEXT("Catalog"), Items))
		{
			return false;
		}
		TestTrue(TEXT("The empty library is read"), Game.Pump([Library]() { return !Library->HasLoadsInProgress(); }));
		TestEqual(TEXT("and holds nothing"), Library->GetTypes().Num(), 0);

		const TArray<FStageFixtureImportOutcome> Refused = Library->RequestImport({ TextFile, FPaths::Combine(Source, TEXT("Missing.gdtf")) });
		TestTrue(TEXT("Wrong format and missing files are refused at once"), Refused.Num() == 2 && Refused[0].Result == EStageFixtureImportResult::UnsupportedFormat
			&& Refused[1].Result == EStageFixtureImportResult::FileNotFound);

		TestTrue(TEXT("Three files accepted"), Library->RequestImport({ SpotFile, BarFile, DamagedFile }).IsEmpty());
		TestTrue(TEXT("A file importing now is refused"), Library->RequestImport({ SpotFile }).Num() == 1);
		TestTrue(TEXT("A scene load would wait"), Library->HasLoadsInProgress());
		TestTrue(TEXT("Imports finish"), Game.Pump([Library]() { return !Library->HasLoadsInProgress(); }, 20.0));
		TestEqual(TEXT("Two types: the damaged file is refused"), Library->GetTypes().Num(), 2);

		const FStageFixtureLibraryType* Spot = Library->FindType(SpotId);
		const FStageFixtureLibraryType* Bar = Library->FindType(BarTypeId);
		if (!TestNotNull(TEXT("Spot in the library"), Spot) || !TestNotNull(TEXT("Bar in the library"), Bar))
		{
			return false;
		}
		TestEqual(TEXT("Spot: two usable modes, the broken one left out"), Spot->Modes.Num(), 2);
		TestTrue(TEXT("Stable path of each mode"), Spot->Modes.Num() == 2 && FSoftObjectPath(Spot->Modes[0].Get()).ToString() == StageFixtureLibrary::MakeItemPath(SpotId, TEXT("Standard")).ToString()
			&& FSoftObjectPath(Spot->Modes[1].Get()).ToString() == StageFixtureLibrary::MakeItemPath(SpotId, TEXT("Basic")).ToString());
		TestTrue(TEXT("A base mesh shared by the modes"), Spot->BaseMesh && Spot->Modes.Num() == 2 && Spot->Modes[0]->Mesh.Get() == Spot->BaseMesh && Spot->Modes[1]->Mesh.Get() == Spot->BaseMesh);
		TestTrue(TEXT("Bar: 64 cells"), Bar->Modes.Num() == 1 && Bar->Modes[0]->CellCount == 64);
		TestEqual(TEXT("The user's file name is kept"), Spot->SourceFileName, FString(TEXT("Test Spot.gdtf")));
		for (const FStageFixtureLibraryType& Type : Library->GetTypes())
		{
			for (const ULightingFixtureData* Mode : Type.Modes)
			{
				TestTrue(FString::Printf(TEXT("%s is in the catalog"), *Mode->GetPathName()), Items->GetCatalog().Contains(Mode));
				FirstRunPaths.Add(Mode->GetPathName());
			}
		}

		TestTrue(TEXT("The same type again is accepted for reading"), Library->RequestImport({ SpotFile }).IsEmpty());
		TestTrue(TEXT("and finishes"), Game.Pump([Library]() { return !Library->HasLoadsInProgress(); }, 20.0));
		TestEqual(TEXT("but is not added twice"), Library->GetTypes().Num(), 2);

		Game.Shutdown();
		TestTrue(TEXT("The spot is copied into the library"), IFileManager::Get().FileExists(*StageFixtureLibrary::GetLibraryFilePath(SpotId)));
		TestTrue(TEXT("The bar is copied into the library"), IFileManager::Get().FileExists(*StageFixtureLibrary::GetLibraryFilePath(BarTypeId)));
		TestFalse(TEXT("The damaged file is not"), IFileManager::Get().FileExists(*FPaths::Combine(StageFixtureLibrary::GetLibraryDirectory(), TEXT("Damaged.gdtf"))));
		FString IndexJson;
		TArray<StageFixtureLibrary::FIndexEntry> Index;
		int32 Skipped = 0;
		TestTrue(TEXT("The index lists both"), FFileHelper::LoadFileToString(IndexJson, *StageFixtureLibrary::GetIndexPath())
			&& StageFixtureLibrary::ReadIndex(IndexJson, Index, Skipped) && Index.Num() == 2 && Skipped == 0);
		TestTrue(TEXT("A stopped library leaves its paths free"), FirstRunPaths.Num() == 3 && FindObject<ULightingFixtureData>(nullptr, *FirstRunPaths[0]) == nullptr);
	}

	AddInfo(TEXT("The next run reads the library again and builds the same paths."));
	{
		FStageTestGameInstance Game(Options);
		UStageFixtureLibrarySubsystem* Library = Game.Subsystem<UStageFixtureLibrarySubsystem>();
		UStageItemSubsystem* Items = Game.Subsystem<UStageItemSubsystem>();
		if (!TestTrue(TEXT("The second game instance starts"), Game.IsValid() && Library && Items))
		{
			return false;
		}
		TestTrue(TEXT("The library is restored"), Game.Pump([Library]() { return !Library->HasLoadsInProgress(); }, 20.0));
		TestEqual(TEXT("with both types"), Library->GetTypes().Num(), 2);
		for (const FString& Path : FirstRunPaths)
		{
			const ULightingFixtureData* Mode = Cast<ULightingFixtureData>(FSoftObjectPath(Path).ResolveObject());
			TestTrue(FString::Printf(TEXT("%s resolves again and is in the catalog"), *Path), Mode && Items->GetCatalog().Contains(Mode));
		}
		const FStageFixtureLibraryType* Spot = Library->FindType(SpotId);
		TestTrue(TEXT("The file name comes back from the index"), Spot && Spot->SourceFileName == TEXT("Test Spot.gdtf"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageSceneFixtureTypesTest, "StageCraft.Scene.FixtureTypes", StageFixtureLibraryTests::Flags)

bool FStageSceneFixtureTypesTest::RunTest(const FString& Parameters)
{
	using namespace StageFixtureLibraryTests;

	const FGuid SpotId = ParseId(StageGdtfTestFiles::SpotId);
	FStageSceneDocument Document;
	Document.Name = TEXT("GDTF Show");
	Document.Items.Add(MakeLibraryItem(SpotId, TEXT("Standard"), TEXT("Standard")));
	FStageItemSnapshot& Crate = Document.Items.AddDefaulted_GetRef();
	Crate.InstanceId = FGuid::NewGuid();
	Crate.Item = TSoftObjectPtr<UBaseItemData>(FSoftObjectPath(TEXT("/Game/StageCraft/Items/DA_Test_Crate.DA_Test_Crate")));

	const FString Json = FStageSceneStore::Serialize(Document);
	TestTrue(TEXT("Version 12 is written"), Json.Contains(TEXT("\"version\": 12")));
	TestTrue(TEXT("The fixture names its type"), Json.Contains(TEXT("\"fixtureType\"")) && Json.Contains(TEXT("\"manufacturer\": \"StageCraft\"")));
	FStageSceneDocument Parsed;
	FStageSceneParseReport Report;
	if (!TestEqual(TEXT("v12 reads back"), Str(FStageSceneStore::Parse(Json, Parsed, Report)), Str(EStageSceneResult::Success)) || !TestEqual(TEXT("two items"), Parsed.Items.Num(), 2))
	{
		return false;
	}
	const FStageItemSnapshot& Fixture = Parsed.Items[0];
	TestEqual(TEXT("Library path kept"), Fixture.Item.ToSoftObjectPath().ToString(), StageFixtureLibrary::MakeItemPath(SpotId, TEXT("Standard")).ToString());
	TestTrue(TEXT("Type reference kept"), Fixture.FixtureType.Id == SpotId && Fixture.FixtureType.Manufacturer == TEXT("StageCraft")
		&& Fixture.FixtureType.Name == TEXT("Test Spot") && Fixture.FixtureType.Mode == TEXT("Standard"));
	TestFalse(TEXT("Content items name no type"), Parsed.Items[1].FixtureType.IsSet());
	TestFalse(TEXT("Library fixtures make a scene not portable"), StagePackage::IsPortable(Parsed));
	TestEqual(TEXT("Nothing dropped"), Report.InvalidItems, 0);

	AddInfo(TEXT("Kept items (not placed on this PC) are saved back with their type."));
	FStageSceneDocument Resaved;
	Resaved.Name = Document.Name;
	FStageSceneStore::AppendUnplacedItems(Resaved, Parsed.Items);
	FStageSceneDocument Again;
	TestTrue(TEXT("Resaved file keeps the reference"), FStageSceneStore::Parse(FStageSceneStore::Serialize(Resaved), Again, Report) == EStageSceneResult::Success
		&& Again.Items.Num() == 2 && Again.Items[0].FixtureType.Id == SpotId && Again.Items[0].FixtureType.Mode == TEXT("Standard"));

	AddInfo(TEXT("Version 11 files load as before."));
	const FString Version11 = Json.Replace(TEXT("\"version\": 12"), TEXT("\"version\": 11"));
	FStageSceneDocument Old;
	TestTrue(TEXT("A v11 file loads"), FStageSceneStore::Parse(Version11, Old, Report) == EStageSceneResult::Success && Old.Items.Num() == 2);
	FString Crate11 = FString::Printf(TEXT("{\"format\":\"StageCraftScene\",\"version\":11,\"name\":\"T\",\"items\":[{\"id\":\"%s\",\"item\":\"/Game/StageCraft/Items/DA_Test_Crate.DA_Test_Crate\",")
		TEXT("\"location\":[1,2,3],\"rotation\":[0,0,0,1],\"scale\":[1,1,1],\"label\":\"Crate\",\"state\":\"\"}]}"), *FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens));
	TestTrue(TEXT("A plain v11 scene loads unchanged"), FStageSceneStore::Parse(Crate11, Old, Report) == EStageSceneResult::Success && Old.Items.Num() == 1 && !Old.Items[0].FixtureType.IsSet());
	TestEqual(TEXT("Version 13 is refused"), Str(FStageSceneStore::Parse(Json.Replace(TEXT("\"version\": 12"), TEXT("\"version\": 13")), Old, Report)), Str(EStageSceneResult::VersionMismatch));

	AddInfo(TEXT("Hostile references: the item loads, the reference does not."));
	const FString Path = StageFixtureLibrary::MakeItemPath(SpotId, TEXT("Standard")).ToString();
	const auto ParseOne = [](const FString& ItemPath, const FString& TypeJson, FStageSceneDocument& Out, FStageSceneParseReport& OutReport)
	{
		const FString File = FString::Printf(TEXT("{\"format\":\"StageCraftScene\",\"version\":12,\"name\":\"T\",\"items\":[{\"id\":\"%s\",\"item\":\"%s\",")
			TEXT("\"location\":[0,0,0],\"rotation\":[0,0,0,1],\"scale\":[1,1,1],\"label\":\"X\",\"state\":\"\"%s}]}"), *FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens), *ItemPath, *TypeJson);
		return FStageSceneStore::Parse(File, Out, OutReport);
	};
	FStageSceneDocument Hostile;
	FStageSceneParseReport HostileReport;
	TestTrue(TEXT("A bad id drops only the reference"), ParseOne(Path, TEXT(",\"fixtureType\":{\"id\":\"nope\",\"name\":\"X\"}"), Hostile, HostileReport) == EStageSceneResult::Success
		&& Hostile.Items.Num() == 1 && !Hostile.Items[0].FixtureType.IsSet());
	TestTrue(TEXT("Not an object"), ParseOne(Path, TEXT(",\"fixtureType\":7"), Hostile, HostileReport) == EStageSceneResult::Success && Hostile.Items.Num() == 1 && !Hostile.Items[0].FixtureType.IsSet());
	const FString LongName = FString::ChrN(5000, TEXT('N'));
	TestTrue(TEXT("Long names are cut"), ParseOne(Path, FString::Printf(TEXT(",\"fixtureType\":{\"id\":\"%s\",\"name\":\"%s\"}"), StageGdtfTestFiles::SpotId, *LongName), Hostile, HostileReport) == EStageSceneResult::Success
		&& Hostile.Items.Num() == 1 && Hostile.Items[0].FixtureType.Name.Len() == StageFixtureLibrary::MaxRefTextLength);
	TestTrue(TEXT("A content item cannot claim a type"), ParseOne(TEXT("/Game/StageCraft/Items/DA_Test_Crate.DA_Test_Crate"),
		FString::Printf(TEXT(",\"fixtureType\":{\"id\":\"%s\"}"), StageGdtfTestFiles::SpotId), Hostile, HostileReport) == EStageSceneResult::Success
		&& Hostile.Items.Num() == 1 && !Hostile.Items[0].FixtureType.IsSet());
	TestTrue(TEXT("A forged library path drops the item"), ParseOne(TEXT("/Temp/StageCraftFixtures/NOTHEX/Standard.Standard"), TEXT(""), Hostile, HostileReport) == EStageSceneResult::Success
		&& Hostile.Items.IsEmpty() && HostileReport.InvalidItems == 1);
	TestTrue(TEXT("A path outside the roots drops the item"), ParseOne(TEXT("/Temp/Elsewhere/Standard.Standard"), TEXT(""), Hostile, HostileReport) == EStageSceneResult::Success
		&& Hostile.Items.IsEmpty());

	const FString Missing = StageFixtureLibrary::DescribeMissingTypes(Parsed.Items).ToString();
	TestEqual(TEXT("A load without the type says what to import"), Missing, FString(TEXT("StageCraft Test Spot (Standard) is missing. Import its GDTF file to see this fixture.")));
	return true;
}

#endif
