// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for exporting MVR scenes (Docs/ADR/0034 §7, phase MV4): the ZIP writer against the ZIP reader, the box .glb against the
// geometry reader, the pure scene writer read back by the parser and the import plan (fixtures, patch, IDs, GDTF files, layers,
// trusses, refusals), and a stage exported through the subsystem and read back item by item, with the GameMode's rule.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Mvr; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Actors/LightingFixtureActor.h"
#include "Actors/StageSpanActor.h"
#include "Components/SelectionComponent.h"
#include "Components/SpawnSystemComponent.h"
#include "Data/LightingFixtureData.h"
#include "Data/StageItemTypes.h"
#include "Data/StageSpanData.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Fixtures/StageFixtureLibraryRules.h"
#include "Fixtures/StageFixtureLibrarySubsystem.h"
#include "Fixtures/StageZipReader.h"
#include "Fixtures/StageZipWriter.h"
#include "Fixtures/Tests/StageGdtfTestFiles.h"
#include "Game/StageCraftGameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "History/StageEditHistoryComponent.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Mvr/StageMvrArchive.h"
#include "Mvr/StageMvrExportGather.h"
#include "Mvr/StageMvrExportSubsystem.h"
#include "Mvr/StageMvrGeometry.h"
#include "Mvr/StageMvrImportPlan.h"
#include "Mvr/StageMvrParser.h"
#include "Mvr/StageMvrWriter.h"
#include "Subsystems/StageSessionSubsystem.h"
#include "Subsystems/StageShowLibrarySubsystem.h"
#include "Tests/StageTestGameInstance.h"
#include "UObject/Package.h"

namespace StageMvrExportTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	FGuid Id(const TCHAR* Text)
	{
		FGuid Out;
		FGuid::Parse(Text, Out);
		return Out;
	}

	const FStageMvrObject* Find(const FStageMvrScene& Scene, const FGuid& ObjectId)
	{
		return Scene.FindObject(ObjectId);
	}

	bool SameTransform(const FTransform& A, const FTransform& B)
	{
		return A.GetLocation().Equals(B.GetLocation(), 0.01) && A.GetRotation().Equals(B.GetRotation(), 1e-4);
	}

	StageMvrImport::FStageMvrModeTarget Target(ULightingFixtureData& Data)
	{
		StageMvrImport::FStageMvrModeTarget Out;
		Out.Item = &Data;
		Out.Label = Data.DisplayName.ToString();
		return Out;
	}

	UStageSpanData* MakeTruss()
	{
		UStageSpanData* Truss = NewObject<UStageSpanData>(GetTransientPackage());
		Truss->DisplayName = FText::FromString(TEXT("Test Truss"));
		Truss->SegmentLength = 100.f;
		Truss->CategoryTag = StageCraftTags::Category_Truss;
		return Truss;
	}

	/**
	 * Layer 0 "Rig & Truss <1>": spot 101 (Test@Spot.gdtf Standard, 1.010, hanging and turned), spot 7 whose GDTF is missing,
	 * spot 9 on 3.512; a 4 m truss leg along Y at 5 m. Layer 1 "Floor": a fixture without GDTF or patch, two equal set pieces
	 * (scaled 2x along X).
	 */
	StageMvrExport::FScene MakeScene()
	{
		StageMvrExport::FScene Scene;
		Scene.ProviderVersion = TEXT("tests");
		Scene.Layers = { { Id(TEXT("6A000000-0000-4000-8000-000000000001")), TEXT("Rig & Truss <1>") }, { Id(TEXT("6A000000-0000-4000-8000-000000000002")), TEXT("Floor") } };

		StageMvrExport::FFixture Spot;
		Spot.Id = Id(TEXT("6A000000-0000-4000-8000-000000000010"));
		Spot.Name = TEXT("Spot \"A\"");
		Spot.Transform = FTransform(FRotator(10.0, 30.0, 180.0), FVector(100.0, -250.0, 600.0));
		Spot.GdtfSpec = TEXT("Test@Spot.gdtf");
		Spot.GdtfMode = TEXT("Standard");
		Spot.FixtureId = 101;
		Spot.Universe = 1;
		Spot.Address = 10;
		Scene.Fixtures.Add(Spot);

		StageMvrExport::FFixture Gone = Spot;
		Gone.Id = Id(TEXT("6A000000-0000-4000-8000-000000000011"));
		Gone.Name = TEXT("Spot B");
		Gone.GdtfSpec = TEXT("Gone.gdtf");
		Gone.FixtureId = 7;
		Gone.Address = 30;
		Scene.Fixtures.Add(Gone);

		StageMvrExport::FFixture Last = Spot;
		Last.Id = Id(TEXT("6A000000-0000-4000-8000-000000000012"));
		Last.Name = TEXT("Spot C");
		Last.FixtureId = 9;
		Last.Universe = 3;
		Last.Address = 512;
		Last.Transform = FTransform(FRotator(0.0, -90.0, 0.0), FVector(-300.0, 0.0, 0.0));
		Scene.Fixtures.Add(Last);

		StageMvrExport::FFixture Plain;
		Plain.Id = Id(TEXT("6A000000-0000-4000-8000-000000000013"));
		Plain.Name = TEXT("Floor Wash");
		Plain.Layer = 1;
		Scene.Fixtures.Add(Plain);

		StageMvrExport::FBoxObject Truss;
		Truss.Kind = StageMvrExport::EBoxKind::Truss;
		Truss.Id = Id(TEXT("6A000000-0000-4000-8000-000000000020"));
		Truss.Name = TEXT("Truss 1");
		Truss.Transform = FTransform(FRotator(0.0, 90.0, 0.0), FVector(0.0, 0.0, 500.0));
		Truss.LocalBounds = FBox(FVector(0.0, -15.0, 0.0), FVector(400.0, 15.0, 30.0));
		Scene.Objects.Add(Truss);

		for (int32 Index = 0; Index < 2; ++Index)
		{
			StageMvrExport::FBoxObject Set;
			Set.Id = Id(*FString::Printf(TEXT("6A000000-0000-4000-8000-00000000003%d"), Index));
			Set.Name = FString::Printf(TEXT("Riser %d"), Index + 1);
			Set.Layer = 1;
			Set.Transform = FTransform(FRotator::ZeroRotator, FVector(200.0 * Index, 800.0, 0.0), FVector(2.0, 1.0, 1.0));
			Set.LocalBounds = FBox(FVector(-50.0, -50.0, 0.0), FVector(50.0, 50.0, 100.0));
			Scene.Objects.Add(Set);
		}
		return Scene;
	}

	/** A game world that has begun play, with an actor carrying the spawn, selection and history components. */
	struct FScopedStage
	{
		UWorld* World = nullptr;
		USpawnSystemComponent* Spawner = nullptr;

		FScopedStage()
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("StageMvrExportTestWorld"));
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			World->BeginPlay();
			World->GetWorldSettings()->NotifyBeginPlay();
			AActor* Owner = World->SpawnActor<AActor>();
			for (UClass* ComponentClass : { USpawnSystemComponent::StaticClass(), USelectionComponent::StaticClass(), UStageEditHistoryComponent::StaticClass() })
			{
				UActorComponent* Component = NewObject<UActorComponent>(Owner, ComponentClass);
				Component->RegisterComponent();
			}
			Spawner = Owner->FindComponentByClass<USpawnSystemComponent>();
		}

		~FScopedStage()
		{
			World->SetGameInstance(nullptr);
			World->BeginTearingDown();
			World->EndPlay(EEndPlayReason::Quit);
			World->DestroyWorld(/*bInformEngineOfWorld*/ false);
			GEngine->DestroyWorldContext(World);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageMvrZipWriterTest, "StageCraft.Mvr.ZipWriter", StageMvrExportTests::Flags)

bool FStageMvrZipWriterTest::RunTest(const FString& Parameters)
{
	TArray<uint8> Text;
	for (int32 Index = 0; Index < 400; ++Index)
	{
		Text.Append(StageGdtfTestFiles::Utf8(FString::Printf(TEXT("line %d of a compressible file\n"), Index % 7)));
	}
	const TArray<uint8> Packed = StageGdtfTestFiles::MakeGdtf(StageGdtfTestFiles::SpotXml());

	const auto Build = [&]()
	{
		StageZip::FWriter Writer;
		FString Error;
		Writer.AddFile(TEXT("GeneralSceneDescription.xml"), Text, StageZip::EStore::Deflate, Error);
		Writer.AddFile(TEXT("Spot.gdtf"), Packed, StageZip::EStore::Store, Error);
		Writer.AddFile(TEXT("models/empty.txt"), {}, StageZip::EStore::Deflate, Error);
		return Writer.Finish();
	};
	const TArray<uint8> Archive = Build();
	TestTrue(TEXT("The same files give the same bytes"), Archive == Build());

	StageZip::FDirectory Directory;
	FString Error;
	if (!TestTrue(FString::Printf(TEXT("The reader accepts the archive (%s)"), *Error), StageZip::ReadDirectory(Archive, StageZip::FLimits(), Directory, Error)))
	{
		return false;
	}
	TestEqual(TEXT("Three files"), Directory.Entries.Num(), 3);
	const StageZip::FEntry* Scene = Directory.Find(TEXT("generalscenedescription.xml"));
	const StageZip::FEntry* Gdtf = Directory.Find(TEXT("Spot.gdtf"));
	const StageZip::FEntry* Empty = Directory.Find(TEXT("models/empty.txt"));
	TestTrue(TEXT("Text is deflated and smaller"), Scene && Scene->Method == 8 && Scene->CompressedSize < Scene->UncompressedSize);
	TestTrue(TEXT("The .gdtf is stored as it is"), Gdtf && Gdtf->Method == 0 && Gdtf->CompressedSize == Packed.Num());
	TestTrue(TEXT("An empty file is stored"), Empty && Empty->Method == 0 && Empty->UncompressedSize == 0);
	TArray<uint8> Out;
	TestTrue(TEXT("Deflated text extracts with its CRC"), Scene && StageZip::Extract(Archive, *Scene, StageZip::FLimits(), Out, Error) && Out == Text);
	TestTrue(TEXT("The stored .gdtf extracts unchanged"), Gdtf && StageZip::Extract(Archive, *Gdtf, StageZip::FLimits(), Out, Error) && Out == Packed);

	StageZip::FWriter Writer;
	TestTrue(TEXT("A first file is added"), Writer.AddFile(TEXT("a.txt"), Text, StageZip::EStore::Deflate, Error));
	TestFalse(TEXT("A repeated name (other case) is refused"), Writer.AddFile(TEXT("A.TXT"), Text, StageZip::EStore::Deflate, Error));
	TestFalse(TEXT("A parent path is refused"), Writer.AddFile(TEXT("../evil.txt"), Text, StageZip::EStore::Deflate, Error));
	TestFalse(TEXT("A drive path is refused"), Writer.AddFile(TEXT("C:/evil.txt"), Text, StageZip::EStore::Deflate, Error));
	TestFalse(TEXT("A backslash is refused"), Writer.AddFile(TEXT("dir\\evil.txt"), Text, StageZip::EStore::Deflate, Error));
	TestEqual(TEXT("...and nothing of them was added"), Writer.Num(), 1);
	Writer.Finish();
	TestEqual(TEXT("Finish empties the writer"), Writer.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageMvrWriterTest, "StageCraft.Mvr.Writer", StageMvrExportTests::Flags)

bool FStageMvrWriterTest::RunTest(const FString& Parameters)
{
	using namespace StageMvrExportTests;

	// The box .glb reads back as the box it was written from.
	const FBox Box(FVector(-120.5, 30.0, 0.0), FVector(4000.0, 330.25, 300.0));
	FBox ReadBack(ForceInit);
	FString Error;
	TestTrue(TEXT("A box .glb reads back as its box (mm, Z up)"), StageMvrGeometry::ReadBounds(TEXT("box.glb"), StageMvrGeometry::WriteBoxGlb(Box, TEXT("Box \"1\"")), ReadBack, Error)
		&& ReadBack.Min.Equals(Box.Min, 0.01) && ReadBack.Max.Equals(Box.Max, 0.01));

	// Text, names and matrices.
	TestEqual(TEXT("XML escaping"), StageMvrExport::EscapeXml(TEXT("a&b<c>\"d'\x01")), FString(TEXT("a&amp;b&lt;c&gt;&quot;d&apos;")));
	TestEqual(TEXT("GDTF names keep the user's file name"), StageMvrExport::MakeGdtfFileName(TEXT("C:/Downloads/Robe@MegaPointe.gdtf"), TEXT("X")), FString(TEXT("Robe@MegaPointe.gdtf")));
	TestEqual(TEXT("...without unsafe characters"), StageMvrExport::MakeGdtfFileName(TEXT("Spot?<>|*:.gdtf"), TEXT("X")), FString(TEXT("Spot.gdtf")));
	TestEqual(TEXT("...or the fallback"), StageMvrExport::MakeGdtfFileName(TEXT("..."), TEXT("ABC")), FString(TEXT("ABC.gdtf")));
	TestEqual(TEXT("A matrix as MVR writes it"), StageMvrExport::FormatMatrix(FMatrix::Identity), FString(TEXT("{1,0,0}{0,1,0}{0,0,1}{0,0,0}")));
	int32 Checked = 0;
	for (double Pitch = -80.0; Pitch <= 80.0; Pitch += 40.0)
	{
		for (double Yaw = -170.0; Yaw < 180.0; Yaw += 55.0)
		{
			for (const double Roll : { 0.0, 37.0, 180.0 })
			{
				const FTransform Placed(FRotator(Pitch, Yaw, Roll), FVector(Yaw, -Pitch * 3.0, 250.0 + Roll));
				const FTransform Back = StageMvrImport::FixtureTransform(StageMvrExport::FixtureMatrix(Placed));
				Checked += SameTransform(Back, Placed) ? 1 : 0;
			}
		}
	}
	TestEqual(TEXT("A fixture's matrix is the inverse of the import's transform, for every pose"), Checked, 5 * 7 * 3);

	// The scene, read back by the import's own reader.
	const StageMvrExport::FScene Scene = MakeScene();
	TMap<FString, TArray<uint8>> Gdtf;
	Gdtf.Add(TEXT("test@spot.GDTF"), StageGdtfTestFiles::MakeGdtf(StageGdtfTestFiles::SpotXml()));
	const StageMvrExport::FWriteResult Written = StageMvrExport::Write(Scene, Gdtf);
	if (!TestTrue(FString::Printf(TEXT("The scene is written (%s)"), *Written.Error), Written.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("Writing is deterministic"), StageMvrExport::Write(Scene, Gdtf).Bytes == Written.Bytes);
	TestTrue(TEXT("One GDTF file and two geometry files (the two risers share one)"), Written.GdtfFiles == 1 && Written.GeometryFiles == 2);
	TestTrue(TEXT("The missing GDTF file is noted"), Written.Notes.Num() == 1 && Written.Notes[0].Contains(TEXT("Gone.gdtf")));

	const FStageMvrArchive Archive = StageMvrArchive::Read(Written.Bytes);
	if (!TestTrue(FString::Printf(TEXT("The import reads the file (%s)"), *FString::Join(Archive.Parsed.Errors, TEXT("; "))), Archive.Parsed.IsValid()))
	{
		return false;
	}
	const FStageMvrScene& Read = Archive.Parsed.Scene;
	TestTrue(TEXT("MVR 1.6 from StageCraft"), Read.VerMajor == 1 && Read.VerMinor == 6 && Read.Provider == TEXT("StageCraft"));
	TestTrue(TEXT("Both layers, names unescaped"), Read.Layers.Num() == 2 && Read.Layers[0].Name == TEXT("Rig & Truss <1>") && Read.Layers[1].Name == TEXT("Floor")
		&& Read.Layers[0].Id == Scene.Layers[0].Id);
	TestTrue(TEXT("4 fixtures, 1 truss, 2 scene objects"), Read.Count(EStageMvrObjectKind::Fixture) == 4 && Read.Count(EStageMvrObjectKind::Truss) == 1
		&& Read.Count(EStageMvrObjectKind::SceneObject) == 2);
	TestTrue(TEXT("The GDTF file inside parses"), Archive.GdtfFiles.Contains(TEXT("test@spot.gdtf")) && Archive.GdtfFiles[TEXT("test@spot.gdtf")].IsValid());

	const FStageMvrObject* Spot = Find(Read, Scene.Fixtures[0].Id);
	if (TestNotNull(TEXT("Spot A by its UUID"), Spot))
	{
		TestEqual(TEXT("...its name"), Spot->Name, FString(TEXT("Spot \"A\"")));
		TestTrue(TEXT("...its type and mode"), Spot->GdtfSpec == TEXT("Test@Spot.gdtf") && Spot->GdtfMode == TEXT("Standard"));
		TestTrue(TEXT("...its patch 1.010"), Spot->Addresses.Num() == 1 && Spot->Addresses[0] == FStageMvrAddress{ 0, 1, 10 });
		TestTrue(TEXT("...its fixture ID"), Spot->FixtureId == TEXT("101") && Spot->FixtureIdNumeric == 101 && Spot->Layer == 0);
		TestTrue(TEXT("...and its pose"), SameTransform(StageMvrImport::FixtureTransform(Spot->World), Scene.Fixtures[0].Transform));
	}
	const FStageMvrObject* Gone = Find(Read, Scene.Fixtures[1].Id);
	TestTrue(TEXT("Spot B is written without a type"), Gone && Gone->GdtfSpec.IsEmpty() && Gone->GdtfMode.IsEmpty());
	const FStageMvrObject* Last = Find(Read, Scene.Fixtures[2].Id);
	TestTrue(TEXT("Spot C on 3.512"), Last && Last->Addresses.Num() == 1 && Last->Addresses[0] == FStageMvrAddress{ 0, 3, 512 });
	const FStageMvrObject* Plain = Find(Read, Scene.Fixtures[3].Id);
	TestTrue(TEXT("The floor wash: no patch, no ID, on Floor"), Plain && Plain->Addresses.IsEmpty() && Plain->FixtureIdNumeric == 0 && Plain->Layer == 1);
	const FStageMvrObject* Riser = Find(Read, Scene.Objects[2].Id);
	TestTrue(TEXT("The second riser keeps its place, without the scale in its matrix"), Riser && SameTransform(StageMvrUnits::ToUnreal(Riser->World), FTransform(FVector(200.0, 800.0, 0.0))));

	// The truss: its geometry is the box, and the import plans the same run.
	const FBox* TrussBounds = Archive.GeometryBounds.Find(TEXT("truss_001.glb"));
	TestTrue(TEXT("The truss geometry is its box in mm"), TrussBounds && TrussBounds->Min.Equals(FVector(0, -150, 0), 0.01) && TrussBounds->Max.Equals(FVector(4000, 150, 300), 0.01));
	StageMvrImport::FInputs Inputs;
	UStageSpanData* TrussItem = MakeTruss();
	Inputs.TrussItem = TrussItem;
	Inputs.GeometryBounds = Archive.GeometryBounds;
	const StageMvrImport::FPlan Plan = StageMvrImport::Build(Read, Inputs);
	if (TestEqual(TEXT("The import plans the truss"), Plan.Trusses.Num(), 1))
	{
		const StageMvrImport::FTrussPlacement& Run = Plan.Trusses[0];
		TestTrue(*FString::Printf(TEXT("...where it was (%s)"), *Run.Transform.GetLocation().ToString()), Run.Transform.GetLocation().Equals(FVector(0, 0, 500), 0.01));
		TestTrue(TEXT("...4 m along +Y in 4 sections"), Run.Sections == 4 && Run.LocalPoints.Num() == 2 && Run.LocalPoints[1].Equals(FVector(0, 400, 0), 0.01));
		TestEqual(TEXT("...with its UUID"), Run.MvrId, Scene.Objects[0].Id);
	}

	// Refusals: nothing is written.
	StageMvrExport::FScene Broken = MakeScene();
	Broken.Fixtures[1].Id = Broken.Fixtures[0].Id;
	TestTrue(TEXT("A repeated UUID is refused"), !StageMvrExport::Write(Broken, Gdtf).IsValid());
	Broken = MakeScene();
	Broken.Objects[0].Layer = 5;
	TestTrue(TEXT("An object on a missing layer is refused"), !StageMvrExport::Write(Broken, Gdtf).IsValid());
	Broken = MakeScene();
	Broken.Objects[1].LocalBounds = FBox(ForceInit);
	TestTrue(TEXT("A box without bounds is refused"), !StageMvrExport::Write(Broken, Gdtf).IsValid());
	Broken = MakeScene();
	Broken.Fixtures.Reset();
	Broken.Objects.Reset();
	const StageMvrExport::FWriteResult Empty = StageMvrExport::Write(Broken, Gdtf);
	TestTrue(TEXT("An empty scene is refused and writes nothing"), !Empty.IsValid() && Empty.Bytes.IsEmpty());
	TestTrue(TEXT("Truss legs get stable ids of their own"), StageMvrExportGather::LegId(Scene.Objects[0].Id, 0) == Scene.Objects[0].Id
		&& StageMvrExportGather::LegId(Scene.Objects[0].Id, 1) != Scene.Objects[0].Id && StageMvrExportGather::LegId(Scene.Objects[0].Id, 1) == StageMvrExportGather::LegId(Scene.Objects[0].Id, 1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageMvrExportTest, "StageCraft.Mvr.Export", StageMvrExportTests::Flags)

bool FStageMvrExportTest::RunTest(const FString& Parameters)
{
	using namespace StageMvrExportTests;

	FStageTestGameInstance Game;
	UStageFixtureLibrarySubsystem* Library = Game.Subsystem<UStageFixtureLibrarySubsystem>();
	if (!TestTrue(TEXT("A game instance with the fixture library"), Game.IsValid() && Library) || !TestTrue(TEXT("The library is read"), Game.Pump([Library]() { return !Library->HasLoadsInProgress(); })))
	{
		return false;
	}
	FScopedStage Stage;
	Stage.World->SetGameInstance(&Game.Get());
	UStageMvrExportSubsystem* Exporter = UWorld::GetSubsystem<UStageMvrExportSubsystem>(Stage.World);
	UStageShowLibrarySubsystem* Shows = UWorld::GetSubsystem<UStageShowLibrarySubsystem>(Stage.World);
	const UStageSessionSubsystem* Session = UWorld::GetSubsystem<UStageSessionSubsystem>(Stage.World);
	if (!TestTrue(TEXT("The stage has the exporter, the show library and the session"), Stage.Spawner && Exporter && Shows && Session))
	{
		return false;
	}
	const FString Path = FPaths::Combine(Game.GetRoot(), TEXT("Exports"), TEXT("Plot.mvr"));
	FText Reason;
	TestFalse(TEXT("An empty stage is refused"), Exporter->StartExport(Path, Reason));
	TestFalse(TEXT("A relative path is refused"), Exporter->StartExport(TEXT("Plot.mvr"), Reason));
	TestFalse(TEXT("A path that is not .mvr is refused"), Exporter->StartExport(FPaths::Combine(Game.GetRoot(), TEXT("Plot.txt")), Reason));

	// The stage: two spots of a library type (two modes), one in a group, and a 3 m truss.
	TSharedRef<UStageFixtureLibrarySubsystem::FParsedFile> Parsed = UStageFixtureLibrarySubsystem::ParseEmbeddedFile(StageGdtfTestFiles::MakeGdtf(StageGdtfTestFiles::SpotXml()), TEXT("Test Spot.gdtf"));
	const FStageFixtureImportOutcome Added = Library->AddEmbeddedFile(*Parsed);
	const FString LibraryFile = StageFixtureLibrary::GetLibraryFilePath(Added.FixtureTypeId);
	if (!TestTrue(TEXT("The spot type is in the library with two modes"), Added.IsSuccess() && Added.Modes.Num() == 2)
		|| !TestTrue(TEXT("...and its file is written"), Game.Pump([&LibraryFile]() { return IFileManager::Get().FileSize(*LibraryFile) > 0; })))
	{
		return false;
	}
	ULightingFixtureData* Standard = Added.Modes[0];
	ULightingFixtureData* Basic = Added.Modes[1];
	const auto SpawnSpot = [&Stage](ULightingFixtureData* Mode, const FTransform& Transform, int32 FixtureId, int32 Universe, int32 Address)
	{
		FStageDMXPatch Patch;
		Patch.Universe = Universe;
		Patch.Address = Address;
		return Cast<ALightingFixtureActor>(Stage.Spawner->SpawnConfiguredItem(Mode, Transform, [FixtureId, Patch](AModularBaseActor& Actor)
		{
			CastChecked<ALightingFixtureActor>(&Actor)->PrepareImportedPatch(FixtureId, Patch, TOptional<FVector2D>());
		}));
	};
	ALightingFixtureActor* Front = SpawnSpot(Standard, FTransform(FRotator(20.0, 45.0, 180.0), FVector(150.0, -300.0, 650.0)), 101, 1, 10);
	ALightingFixtureActor* Floor = SpawnSpot(Basic, FTransform(FVector(-200.0, 400.0, 0.0)), 202, 2, 1);
	UStageSpanData* TrussItem = MakeTruss();
	AModularBaseActor* Truss = Stage.Spawner->SpawnConfiguredItem(TrussItem, FTransform(FRotator(0.0, 90.0, 0.0), FVector(0.0, 0.0, 400.0)), [](AModularBaseActor& Actor)
	{
		CastChecked<AStageSpanActor>(&Actor)->InitializePath({ FVector::ZeroVector, FVector(300.0, 0.0, 0.0) }, false);
	});
	if (!TestTrue(TEXT("Two spots and a truss are placed"), Front && Floor && Truss && Session->GetPlacedItems().Num() == 3))
	{
		return false;
	}
	Shows->AddGroup(FText::FromString(TEXT("Front Light")), { 101 });
	Shows->AddGroup(FText::FromString(TEXT("Nobody here")), { 999 });

	// Exported through the subsystem: one at a time, written on a worker.
	TestTrue(TEXT("The export starts"), Exporter->StartExport(Path, Reason));
	TestFalse(TEXT("A second export waits for the first"), Exporter->StartExport(Path, Reason));
	TestTrue(TEXT("The export finishes"), Game.Pump([Exporter]() { return !Exporter->IsExporting(); }, 20.0));
	const FStageReportExportResult& Result = Exporter->GetLastExport();
	TestTrue(FString::Printf(TEXT("...successfully (%s)"), *Result.Message.ToString()), Result.bSucceeded && Result.FilePath == Path);
	TestTrue(TEXT("...and says what it wrote"), Result.Message.ToString().Contains(TEXT("2 fixtures, 1 truss")));

	TArray<uint8> Bytes;
	if (!TestTrue(TEXT("The file is there"), FFileHelper::LoadFileToArray(Bytes, *Path)))
	{
		return false;
	}
	const FStageMvrArchive Archive = StageMvrArchive::Read(MoveTemp(Bytes));
	if (!TestTrue(FString::Printf(TEXT("The import reads it (%s)"), *FString::Join(Archive.Parsed.Errors, TEXT("; "))), Archive.Parsed.IsValid()))
	{
		return false;
	}
	const FStageMvrScene& Read = Archive.Parsed.Scene;
	TestTrue(TEXT("Layers: the group with a fixture here, then StageCraft"), Read.Layers.Num() == 2 && Read.Layers[0].Name == TEXT("Front Light") && Read.Layers[1].Name == TEXT("StageCraft"));
	TestTrue(TEXT("The type's library file is inside, under the name it was imported as"), Archive.GdtfFiles.Contains(TEXT("test spot.gdtf")));

	// Read back as the import would place it: the same modes, IDs, patch, poses and UUIDs.
	StageMvrImport::FInputs Inputs;
	TMap<FString, StageMvrImport::FStageMvrModeTarget>& Modes = Inputs.Modes.Add(TEXT("test spot.gdtf"));
	Modes.Add(Standard->DMXModeName.ToString().ToLower(), Target(*Standard));
	Modes.Add(Basic->DMXModeName.ToString().ToLower(), Target(*Basic));
	Inputs.TrussItem = TrussItem;
	Inputs.GeometryBounds = Archive.GeometryBounds;
	const StageMvrImport::FPlan Plan = StageMvrImport::Build(Read, Inputs);
	TestTrue(TEXT("Two fixtures and a truss, none generic"), Plan.Fixtures.Num() == 2 && Plan.Trusses.Num() == 1 && Plan.FallbackFixtures == 0);
	for (const ALightingFixtureActor* Placed : { Front, Floor })
	{
		const StageMvrImport::FFixturePlacement* Back = Plan.Fixtures.FindByPredicate([Placed](const StageMvrImport::FFixturePlacement& Fixture) { return Fixture.MvrId == Placed->GetInstanceId(); });
		const FString Name = FString::FromInt(Placed->GetFixtureId());
		if (!TestNotNull(*FString::Printf(TEXT("Fixture %s comes back by its instance id"), *Name), Back))
		{
			continue;
		}
		TestTrue(*FString::Printf(TEXT("...%s in its mode"), *Name), Back->Item.Get() == Placed->GetItemData());
		TestTrue(*FString::Printf(TEXT("...%s with its ID and patch"), *Name), Back->FixtureId == Placed->GetFixtureId() && Back->Patch == Placed->GetPatch());
		TestTrue(*FString::Printf(TEXT("...%s where it was"), *Name), SameTransform(Back->Transform, Placed->GetActorTransform()));
	}
	if (Plan.Trusses.Num() == 1)
	{
		TestTrue(*FString::Printf(TEXT("The truss comes back where it was (%s)"), *Plan.Trusses[0].Transform.GetLocation().ToString()),
			Plan.Trusses[0].Transform.GetLocation().Equals(Truss->GetActorLocation(), 0.01) && Plan.Trusses[0].Sections == 3 && Plan.Trusses[0].MvrId == Truss->GetInstanceId());
		TestTrue(TEXT("...along its direction"), Plan.Trusses[0].LocalPoints.Num() == 2 && Plan.Trusses[0].LocalPoints[1].Equals(FVector(0.0, 300.0, 0.0), 0.01));
	}

	// The GameMode's rule.
	AStageCraftGameModeBase* GameMode = Stage.World->SpawnActor<AStageCraftGameModeBase>();
	const APlayerController* Player = Stage.World->SpawnActor<APlayerController>();
	if (TestTrue(TEXT("Game mode and player"), GameMode && Player))
	{
		TestFalse(TEXT("No player: refused"), GameMode->EvaluateMvrExport(nullptr).IsSuccess());
		TestTrue(TEXT("Allowed by default"), GameMode->EvaluateMvrExport(Player).IsSuccess());
		const FStructProperty* RulesProperty = FindFProperty<FStructProperty>(AStageCraftGameModeBase::StaticClass(), TEXT("SessionRules"));
		if (TestNotNull(TEXT("Session rules"), RulesProperty))
		{
			RulesProperty->ContainerPtrToValuePtr<FStageSessionRules>(GameMode)->bAllowMvrExport = false;
			const FStageEconomyResultInfo Verdict = GameMode->EvaluateMvrExport(Player);
			TestTrue(TEXT("Switched off for the session: refused with a reason"), !Verdict.IsSuccess() && Verdict.Code == EStageEconomyResult::SessionRuleViolation && !Verdict.Message.IsEmpty());
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
