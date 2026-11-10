// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for importing MVR scenes (Docs/ADR/0034 §6, phases MV2 and MV3): geometry bounds (glTF, GLB, 3DS, malformed files), the
// pan / tilt aim maths and the hanging rest pose, the import plan (types, generic fixtures, IDs, patch, focus, trusses, duplicates,
// multipatch), placing a plan a slice at a time as one undo step (undo, redo, all-or-nothing refusal), and reading an archive into
// a review with its GDTF types added to the library. Self-authored files only.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Mvr; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Actors/LightingFixtureActor.h"
#include "Actors/StageSpanActor.h"
#include "Components/SelectionComponent.h"
#include "Components/SpawnSystemComponent.h"
#include "Data/LightingFixtureData.h"
#include "Data/StageParameterTypes.h"
#include "Data/StageSpanData.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Fixtures/StageFixtureLibraryRules.h"
#include "Fixtures/StageFixtureLibrarySubsystem.h"
#include "Fixtures/StageGdtfFixtureBuilder.h"
#include "Fixtures/StageGdtfMapping.h"
#include "Fixtures/StageGdtfParser.h"
#include "Fixtures/Tests/StageGdtfTestFiles.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "History/StageEditHistoryComponent.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Mvr/StageMvrArchive.h"
#include "Mvr/StageMvrGeometry.h"
#include "Mvr/StageMvrImportPlan.h"
#include "Mvr/StageMvrImportSubsystem.h"
#include "Mvr/StageMvrParser.h"
#include "Mvr/StageMvrPlacement.h"
#include "Subsystems/StageSessionSubsystem.h"
#include "Tests/StageTestGameInstance.h"
#include "UObject/Package.h"

namespace StageMvrImportTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	const TCHAR* const IdA = TEXT("5D8C2E10-1111-4A00-8000-00000000000A");
	const TCHAR* const IdB = TEXT("5D8C2E10-1111-4A00-8000-00000000000B");
	const TCHAR* const IdC = TEXT("5D8C2E10-1111-4A00-8000-00000000000C");
	const TCHAR* const IdD = TEXT("5D8C2E10-1111-4A00-8000-00000000000D");
	const TCHAR* const IdE = TEXT("5D8C2E10-1111-4A00-8000-00000000000E");
	const TCHAR* const IdF = TEXT("5D8C2E10-1111-4A00-8000-00000000000F");
	const TCHAR* const IdFocus = TEXT("5D8C2E10-1111-4A00-8000-000000000010");
	const TCHAR* const IdTruss = TEXT("5D8C2E10-1111-4A00-8000-000000000011");
	const TCHAR* const IdTower = TEXT("5D8C2E10-1111-4A00-8000-000000000012");
	const TCHAR* const IdSet = TEXT("5D8C2E10-1111-4A00-8000-000000000013");
	const TCHAR* const IdLayerRig = TEXT("5D8C2E10-1111-4A00-8000-000000000020");
	const TCHAR* const IdLayerFloor = TEXT("5D8C2E10-1111-4A00-8000-000000000021");

	FGuid Id(const TCHAR* Text)
	{
		FGuid Out;
		FGuid::Parse(Text, Out);
		return Out;
	}

	/**
	 * Layer "Rig": spot A (101, 1.10, aims at the focus point), spot B in a mode the type lacks (ID "7"), C without GDTF (513),
	 * D a multipatch of A, a truss along X on truss.glb and a tower on tower.glb, a scene object. Layer "Floor": spot E asking for
	 * 101 again, spot F (the test marks it as already on the stage).
	 */
	FString PlanSceneXml()
	{
		return FString::Printf(TEXT(
			"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
			"<GeneralSceneDescription verMajor=\"1\" verMinor=\"6\" provider=\"StageCraft tests\" providerVersion=\"1\">\n"
			"<Scene><Layers>\n"
			"<Layer uuid=\"%s\" name=\"Rig\"><ChildList>\n"
			" <Fixture uuid=\"%s\" name=\"Spot A\"><Matrix>{1,0,0}{0,1,0}{0,0,1}{0,0,6000}</Matrix><GDTFSpec>Spot.gdtf</GDTFSpec><GDTFMode>Standard</GDTFMode>\n"
			"  <Focus>%s</Focus><FixtureIDNumeric>101</FixtureIDNumeric><Addresses><Address break=\"0\">1.10</Address></Addresses></Fixture>\n"
			" <Fixture uuid=\"%s\" name=\"Spot B\"><Matrix>{1,0,0}{0,1,0}{0,0,1}{1000,0,6000}</Matrix><GDTFSpec>Spot.gdtf</GDTFSpec><GDTFMode>Missing</GDTFMode><FixtureID>7</FixtureID></Fixture>\n"
			" <Fixture uuid=\"%s\" name=\"\"><Matrix>{1,0,0}{0,1,0}{0,0,1}{2000,0,6000}</Matrix><Addresses><Address>513</Address></Addresses></Fixture>\n"
			" <Fixture uuid=\"%s\" name=\"Spot A again\" multipatch=\"%s\"><GDTFSpec>Spot.gdtf</GDTFSpec><GDTFMode>Standard</GDTFMode></Fixture>\n"
			" <FocusPoint uuid=\"%s\" name=\"DSC\"><Matrix>{1,0,0}{0,1,0}{0,0,1}{2000,1000,0}</Matrix></FocusPoint>\n"
			" <Truss uuid=\"%s\" name=\"Upstage\"><Matrix>{1,0,0}{0,1,0}{0,0,1}{0,3000,6000}</Matrix><Geometries><Geometry3D fileName=\"truss.glb\"/></Geometries></Truss>\n"
			" <Truss uuid=\"%s\" name=\"Tower\"><Geometries><Geometry3D fileName=\"tower.glb\"/></Geometries></Truss>\n"
			" <SceneObject uuid=\"%s\" name=\"Set\"><Geometries><Geometry3D fileName=\"set.glb\"/></Geometries></SceneObject>\n"
			"</ChildList></Layer>\n"
			"<Layer uuid=\"%s\" name=\"Floor\"><ChildList>\n"
			" <Fixture uuid=\"%s\" name=\"Spot E\"><Matrix>{1,0,0}{0,1,0}{0,0,1}{0,-2000,0}</Matrix><GDTFSpec>Spot.gdtf</GDTFSpec><GDTFMode>Standard</GDTFMode><FixtureIDNumeric>101</FixtureIDNumeric></Fixture>\n"
			" <Fixture uuid=\"%s\" name=\"Spot F\"><GDTFSpec>Spot.gdtf</GDTFSpec><GDTFMode>Standard</GDTFMode></Fixture>\n"
			"</ChildList></Layer>\n"
			"</Layers></Scene></GeneralSceneDescription>\n"),
			IdLayerRig, IdA, IdFocus, IdB, IdC, IdD, IdA, IdFocus, IdTruss, IdTower, IdSet, IdLayerFloor, IdE, IdF);
	}

	/** A GLB with one mesh whose POSITION accessor spans Min..Max (metres, Y up), under a node with Translation and Scale. */
	TArray<uint8> MakeGlb(const FVector& Min, const FVector& Max, const FVector& Translation = FVector::ZeroVector, const FVector& Scale = FVector::OneVector)
	{
		FString Json = FString::Printf(TEXT("{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],")
			TEXT("\"nodes\":[{\"mesh\":0,\"translation\":[%f,%f,%f],\"scale\":[%f,%f,%f]}],")
			TEXT("\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],")
			TEXT("\"accessors\":[{\"componentType\":5126,\"count\":8,\"type\":\"VEC3\",\"min\":[%f,%f,%f],\"max\":[%f,%f,%f]}]}"),
			Translation.X, Translation.Y, Translation.Z, Scale.X, Scale.Y, Scale.Z, Min.X, Min.Y, Min.Z, Max.X, Max.Y, Max.Z);
		while (Json.Len() % 4 != 0)
		{
			Json += TEXT(" ");
		}
		const TArray<uint8> Chunk = StageGdtfTestFiles::Utf8(Json);
		TArray<uint8> Bytes;
		const auto Put32 = [&Bytes](uint32 Value) { for (int32 Shift = 0; Shift < 32; Shift += 8) { Bytes.Add(uint8(Value >> Shift)); } };
		Put32(0x46546C67);
		Put32(2);
		Put32(uint32(12 + 8 + Chunk.Num()));
		Put32(uint32(Chunk.Num()));
		Put32(0x4E4F534A);
		Bytes.Append(Chunk);
		return Bytes;
	}

	/** A 3DS file with one mesh of the given vertices (mm). */
	TArray<uint8> Make3ds(const TArray<FVector3f>& Vertices)
	{
		TArray<uint8> Vertex;
		const auto Put16 = [](TArray<uint8>& Out, uint16 Value) { Out.Add(uint8(Value)); Out.Add(uint8(Value >> 8)); };
		const auto Put32 = [](TArray<uint8>& Out, uint32 Value) { for (int32 Shift = 0; Shift < 32; Shift += 8) { Out.Add(uint8(Value >> Shift)); } };
		const auto Wrap = [&Put16, &Put32](uint16 ChunkId, const TArray<uint8>& Body)
		{
			TArray<uint8> Out;
			Put16(Out, ChunkId);
			Put32(Out, uint32(6 + Body.Num()));
			Out.Append(Body);
			return Out;
		};
		Put16(Vertex, uint16(Vertices.Num()));
		for (const FVector3f& Point : Vertices)
		{
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				uint32 Bits;
				const float Value = Point[Axis];
				FMemory::Memcpy(&Bits, &Value, 4);
				Put32(Vertex, Bits);
			}
		}
		TArray<uint8> Object = { 'b', 'o', 'x', 0 };
		Object.Append(Wrap(0x4100, Wrap(0x4110, Vertex)));
		return Wrap(0x4D4D, Wrap(0x3D3D, Wrap(0x4000, Object)));
	}

	/** The test spot's built modes (Standard, Basic), as the library builds them, outside any package path. */
	TArray<ULightingFixtureData*> BuildSpotModes()
	{
		const FStageGdtfParseResult Parsed = StageGdtf::Parse(StageGdtfTestFiles::MakeGdtf(StageGdtfTestFiles::SpotXml()));
		TArray<FStageGdtfModeLayout> Layouts;
		for (int32 Mode = 0; Parsed.IsOk() && Mode < Parsed.Type.Modes.Num(); ++Mode)
		{
			Layouts.Add(StageGdtfMapping::BuildModeLayout(Parsed.Type, Mode));
		}
		const FStageGdtfCapabilities Capabilities = StageGdtfMapping::DeriveCapabilities(Parsed.Type, Layouts);
		TArray<ULightingFixtureData*> Modes;
		for (const FStageGdtfModeLayout& Layout : Layouts)
		{
			if (Layout.IsUsable())
			{
				ULightingFixtureData* Data = NewObject<ULightingFixtureData>(GetTransientPackage(), NAME_None, RF_Transient);
				StageGdtfFixtureBuilder::Fill(*Data, Parsed.Type, Capabilities, Layout, {}, nullptr);
				Modes.Add(Data);
			}
		}
		return Modes;
	}

	StageMvrImport::FStageMvrModeTarget Target(ULightingFixtureData& Data)
	{
		StageMvrImport::FStageMvrModeTarget Out;
		Out.Item = &Data;
		Out.Label = Data.DisplayName.ToString();
		float Min = 0.f;
		float Max = 0.f;
		Out.bHasPanTilt = Data.bHasPanTilt && Data.GetAttributeRange(StageCraftTags::Attribute_Pan, Min, Max);
		Out.PanRange = FVector2D(Min, Max);
		Data.GetAttributeRange(StageCraftTags::Attribute_Tilt, Min, Max);
		Out.TiltRange = FVector2D(Min, Max);
		return Out;
	}

	/** Inputs for PlanSceneXml: Spot.gdtf Standard is Spot, a generic fixture, a 1 m-section truss, geometry bounds. */
	StageMvrImport::FInputs MakeInputs(ULightingFixtureData& Spot, ULightingFixtureData& Generic, UStageSpanData& Truss)
	{
		StageMvrImport::FInputs Inputs;
		Inputs.Modes.Add(TEXT("spot.gdtf")).Add(TEXT("standard"), Target(Spot));
		Inputs.Fallback = Target(Generic);
		Inputs.TrussItem = &Truss;
		// truss.glb: 2 m along X, 30 cm square; tower.glb: 3 m tall.
		Inputs.GeometryBounds.Add(TEXT("truss.glb"), FBox(FVector(-1000, -150, -150), FVector(1000, 150, 150)));
		Inputs.GeometryBounds.Add(TEXT("tower.glb"), FBox(FVector(-150, -150, 0), FVector(150, 150, 3000)));
		Inputs.UsedFixtureIds = { 5 };
		Inputs.ExistingMvrIds = { Id(IdF) };
		return Inputs;
	}

	UStageSpanData* MakeTruss()
	{
		UStageSpanData* Truss = NewObject<UStageSpanData>(GetTransientPackage());
		Truss->DisplayName = FText::FromString(TEXT("Test Truss"));
		Truss->SegmentLength = 100.f;
		return Truss;
	}

	/** A game world that has begun play, with an actor carrying the spawn, selection and history components. */
	struct FScopedStage
	{
		UWorld* World = nullptr;
		USpawnSystemComponent* Spawner = nullptr;
		UStageEditHistoryComponent* EditHistory = nullptr;
		UStageSessionSubsystem* Session = nullptr;

		FScopedStage()
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("StageMvrTestWorld"));
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
			EditHistory = Owner->FindComponentByClass<UStageEditHistoryComponent>();
			Session = UWorld::GetSubsystem<UStageSessionSubsystem>(World);
		}

		~FScopedStage()
		{
			World->BeginTearingDown();
			World->EndPlay(EEndPlayReason::Quit);
			World->DestroyWorld(/*bInformEngineOfWorld*/ false);
			GEngine->DestroyWorldContext(World);
		}

		int32 CountPlaced() const { return Session ? Session->GetPlacedItems().Num() : 0; }

		ALightingFixtureActor* FindFixture(const FGuid& MvrId) const
		{
			for (AModularBaseActor* Item : Session->GetPlacedItems())
			{
				if (Item && Item->GetMvrId() == MvrId)
				{
					return Cast<ALightingFixtureActor>(Item);
				}
			}
			return nullptr;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageMvrGeometryTest, "StageCraft.Mvr.Geometry", StageMvrImportTests::Flags)

bool FStageMvrGeometryTest::RunTest(const FString& Parameters)
{
	using namespace StageMvrImportTests;

	FBox Bounds(ForceInit);
	FString Error;
	// 2 x 0.3 x 0.4 m (Y up), moved 1 m up (glTF Y) and scaled 2x along glTF X.
	TestTrue(TEXT("GLB bounds are read"), StageMvrGeometry::ReadBounds(TEXT("Truss.GLB"), MakeGlb(FVector(-1, 0, -0.2), FVector(1, 0.3, 0.2), FVector(0, 1, 0), FVector(2, 1, 1)), Bounds, Error));
	// X scaled: -2..2 m; glTF Y 1..1.3 m -> MVR Z 1000..1300 mm; glTF Z -0.2..0.2 -> MVR Y = -Z -> -200..200 mm.
	TestTrue(*FString::Printf(TEXT("...converted to MVR mm, Z up: %s"), *Bounds.ToString()), Bounds.Min.Equals(FVector(-2000, -200, 1000), 0.01) && Bounds.Max.Equals(FVector(2000, 200, 1300), 0.01));

	const FString GltfText = TEXT("{\"asset\":{\"version\":\"2.0\"},\"nodes\":[{\"children\":[1],\"translation\":[0,0,1]},{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],")
		TEXT("\"accessors\":[{\"min\":[0,0,0],\"max\":[1,1,1]}]}");
	TestTrue(TEXT(".gltf text without scenes: roots are the nodes nobody parents"), StageMvrGeometry::ReadBounds(TEXT("a.gltf"), StageGdtfTestFiles::Utf8(GltfText), Bounds, Error)
		&& Bounds.Min.Equals(FVector(0, -2000, 0), 0.01) && Bounds.Max.Equals(FVector(1000, -1000, 1000), 0.01));

	TestTrue(TEXT("3DS vertices give the bounds in mm"), StageMvrGeometry::ReadBounds(TEXT("t.3ds"), Make3ds({ FVector3f(0, 0, 0), FVector3f(3000, 300, 400), FVector3f(-10, 5, 2) }), Bounds, Error)
		&& Bounds.Min.Equals(FVector(-10, 0, 0)) && Bounds.Max.Equals(FVector(3000, 300, 400)));

	// Refusals: each says why, nothing hangs.
	TestFalse(TEXT("Not a 3DS file"), StageMvrGeometry::ReadBounds(TEXT("x.3ds"), StageGdtfTestFiles::Utf8(TEXT("hello")), Bounds, Error));
	TArray<uint8> Truncated = MakeGlb(FVector::ZeroVector, FVector::OneVector);
	Truncated.SetNum(30);
	TestFalse(TEXT("A GLB whose JSON chunk runs past the file"), StageMvrGeometry::ReadBounds(TEXT("x.glb"), Truncated, Bounds, Error));
	TestFalse(TEXT("glTF without positions"), StageMvrGeometry::ReadBounds(TEXT("x.gltf"), StageGdtfTestFiles::Utf8(TEXT("{\"asset\":{\"version\":\"2.0\"},\"nodes\":[{}]}")), Bounds, Error));
	TestFalse(TEXT("A node that is its own child stops at the depth cap"), StageMvrGeometry::ReadBounds(TEXT("x.gltf"),
		StageGdtfTestFiles::Utf8(TEXT("{\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"children\":[0]}]}")), Bounds, Error));
	TestTrue(*FString::Printf(TEXT("...and says so (%s)"), *Error), Error.Contains(TEXT("deeper")));
	TestFalse(TEXT("Not JSON"), StageMvrGeometry::ReadBounds(TEXT("x.gltf"), StageGdtfTestFiles::Utf8(TEXT("{nope")), Bounds, Error));
	TestFalse(TEXT("Unknown extension"), StageMvrGeometry::ReadBounds(TEXT("x.obj"), StageGdtfTestFiles::Utf8(TEXT("v 0 0 0")), Bounds, Error));
	TArray<uint8> Damaged3ds = Make3ds({ FVector3f(1, 2, 3) });
	Damaged3ds[2] = 0xFF; // The main chunk claims more bytes than the file has.
	TestFalse(TEXT("A damaged 3DS chunk is not read past the file"), StageMvrGeometry::ReadBounds(TEXT("x.3ds"), Damaged3ds, Bounds, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageMvrAimTest, "StageCraft.Mvr.Aim", StageMvrImportTests::Flags)

bool FStageMvrAimTest::RunTest(const FString& Parameters)
{
	using namespace StageMvrImport;

	// The pivots' own maths: at rest the beam leaves along +Z.
	TestTrue(TEXT("At rest the beam points up the fixture's +Z"), BeamDirection(0, 0).Equals(FVector::UpVector, 1e-9));

	const FVector2D Pan(-270.0, 270.0);
	const FVector2D Tilt(-135.0, 135.0);
	int32 Checked = 0;
	for (double Theta = 5.0; Theta < 135.0; Theta += 17.0)
	{
		for (double Phi = -180.0; Phi < 180.0; Phi += 23.0)
		{
			const FVector Wanted = FRotator(0.0, Phi, 0.0).RotateVector(FRotator(Theta, 0.0, 0.0).RotateVector(FVector::UpVector));
			bool bClamped = true;
			const FVector2D Solved = SolvePanTilt(Wanted * 3.7, Pan, Tilt, bClamped);
			if (!BeamDirection(Solved.X, Solved.Y).Equals(Wanted, 1e-6) || bClamped || FMath::Abs(Solved.X) > 270.0 || FMath::Abs(Solved.Y) > 135.0)
			{
				AddError(FString::Printf(TEXT("Direction %s solved to pan %.2f tilt %.2f (clamped %d)"), *Wanted.ToString(), Solved.X, Solved.Y, bClamped));
			}
			++Checked;
		}
	}
	TestTrue(TEXT("Many directions solved exactly"), Checked > 50);

	bool bClamped = false;
	const FVector2D Small = SolvePanTilt(FVector(-1, 0, 0.0), FVector2D(-10, 10), FVector2D(-30, 30), bClamped);
	TestTrue(TEXT("Out of reach: clamped to the ranges and reported"), bClamped && FMath::IsNearlyEqual(FMath::Abs(Small.Y), 30.0) && FMath::Abs(Small.X) <= 10.0);
	TestTrue(TEXT("Along the rest direction: no movement"), SolvePanTilt(FVector(0, 0, 5), Pan, Tilt, bClamped).IsNearlyZero(1e-9) && !bClamped);
	SolvePanTilt(FVector(0, 0, -1), Pan, Tilt, bClamped);
	TestTrue(TEXT("Straight back needs 180 degrees of tilt: clamped to 135"), bClamped);
	TestTrue(TEXT("A zero direction gives no movement"), SolvePanTilt(FVector::ZeroVector, Pan, Tilt, bClamped).IsZero() && !bClamped);

	// MVR's rest pose hangs: the identity matrix points the beam straight down, the offset is mm, Y is mirrored.
	const FTransform Hung = FixtureTransform(FMatrix::Identity);
	TestTrue(TEXT("An MVR fixture at rest shines down"), Hung.TransformVectorNoScale(BeamDirection(0, 0)).Equals(FVector(0, 0, -1), 1e-9));
	FMatrix Placed = FMatrix::Identity;
	Placed.M[3][0] = 1000.0;
	Placed.M[3][1] = 2000.0;
	Placed.M[3][2] = 6000.0;
	Placed.M[0][0] = 2.0; // Scale is dropped.
	const FTransform Moved = FixtureTransform(Placed);
	TestTrue(TEXT("Its location is in cm with Y mirrored"), Moved.GetLocation().Equals(FVector(100, -200, 600), 1e-6));
	TestTrue(TEXT("Scale is dropped"), Moved.GetScale3D().Equals(FVector::OneVector, 1e-6));
	// A 90 degree turn about Z in MVR (X -> Y) faces the fixture's X along Unreal -Y once Y is mirrored.
	FMatrix Turned = FMatrix::Identity;
	Turned.M[0][0] = 0.0; Turned.M[0][1] = 1.0;
	Turned.M[1][0] = -1.0; Turned.M[1][1] = 0.0;
	TestTrue(TEXT("A turn keeps its sense through the mirror"), FixtureTransform(Turned).TransformVectorNoScale(FVector::ForwardVector).Equals(FVector(0, -1, 0), 1e-9));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageMvrPlanTest, "StageCraft.Mvr.Plan", StageMvrImportTests::Flags)

bool FStageMvrPlanTest::RunTest(const FString& Parameters)
{
	using namespace StageMvrImportTests;

	const FStageMvrParseResult Parsed = StageMvr::ParseSceneDescription(PlanSceneXml());
	if (!TestTrue(FString::Printf(TEXT("The scene parses (%s)"), *FString::Join(Parsed.Errors, TEXT("; "))), Parsed.IsValid()))
	{
		return false;
	}
	const TArray<ULightingFixtureData*> Modes = BuildSpotModes();
	if (!TestTrue(TEXT("The test spot builds two modes"), Modes.Num() == 2))
	{
		return false;
	}
	UStageSpanData* Truss = MakeTruss();
	const StageMvrImport::FInputs Inputs = MakeInputs(*Modes[0], *Modes[1], *Truss);
	const StageMvrImport::FPlan Plan = StageMvrImport::Build(Parsed.Scene, Inputs);

	TestEqual(TEXT("Fixtures placed: A, B, C, E (D is a multipatch, F already on the stage)"), Plan.Fixtures.Num(), 4);
	TestEqual(TEXT("One truss run (the tower is not a run)"), Plan.Trusses.Num(), 1);
	TestTrue(TEXT("Counts"), Plan.SkippedExisting == 1 && Plan.SkippedFixtures == 1 && Plan.SkippedTrusses == 1 && Plan.NotPlacedObjects == 1);
	TestTrue(TEXT("Layer names kept"), Plan.LayerNames == TArray<FString>({ TEXT("Rig"), TEXT("Floor") }));

	const auto Find = [&Plan](const TCHAR* MvrId) { return Plan.Fixtures.FindByPredicate([MvrId](const StageMvrImport::FFixturePlacement& Entry) { return Entry.MvrId == Id(MvrId); }); };
	const StageMvrImport::FFixturePlacement* A = Find(IdA);
	const StageMvrImport::FFixturePlacement* B = Find(IdB);
	const StageMvrImport::FFixturePlacement* C = Find(IdC);
	const StageMvrImport::FFixturePlacement* E = Find(IdE);
	if (!TestTrue(TEXT("A, B, C and E are planned"), A && B && C && E))
	{
		return false;
	}
	TestTrue(TEXT("A: its type's mode, its ID, its patch"), A->Item.Get() == Modes[0] && !A->bFallback && A->FixtureId == 101 && A->Patch.Universe == 1 && A->Patch.Address == 10);
	TestTrue(TEXT("A: hung 6 m up"), A->Transform.GetLocation().Equals(FVector(0, 0, 600)) && A->Transform.TransformVectorNoScale(FVector::UpVector).Equals(FVector(0, 0, -1), 1e-9));
	TestEqual(TEXT("A: its name"), A->Label, FString(TEXT("Spot A")));
	TestTrue(TEXT("B: a mode the type lacks -> the generic fixture"), B->Item.Get() == Modes[1] && B->bFallback && B->FixtureId == 7 && !B->Patch.IsPatched());
	TestTrue(TEXT("C: no GDTF -> the generic fixture, the next free ID, 513 is 2.001"), C->bFallback && C->FixtureId == 1 && C->Patch.Universe == 2 && C->Patch.Address == 1);
	TestTrue(TEXT("E: 101 is taken by A -> renumbered"), E->FixtureId == 2 && !E->bFallback && E->Layer == 1);
	TestTrue(TEXT("Counts: 2 generic, 1 renumbered"), Plan.FallbackFixtures == 2 && Plan.RenumberedFixtures == 1);

	// A aims at the focus point: its beam, turned by the solved pan and tilt, points from the fixture to the point.
	if (TestTrue(TEXT("A aims at its focus point"), A->PanTilt.IsSet()) && Plan.AimedFixtures == 1)
	{
		const FVector Focus(200, -100, 0);
		const FVector Beam = A->Transform.TransformVectorNoScale(StageMvrImport::BeamDirection(A->PanTilt->X, A->PanTilt->Y));
		TestTrue(*FString::Printf(TEXT("...exactly (beam %s)"), *Beam.ToString()), Beam.Equals((Focus - A->Transform.GetLocation()).GetSafeNormal(), 1e-6));
	}
	TestFalse(TEXT("B has no focus"), B->PanTilt.IsSet());

	const StageMvrImport::FTrussPlacement& Run = Plan.Trusses[0];
	TestEqual(TEXT("Truss: two 1 m sections"), Run.Sections, 2);
	TestTrue(*FString::Printf(TEXT("Truss: along X from (-100, -300) at its bottom, 585 cm (%s)"), *Run.Transform.GetLocation().ToString()),
		Run.Transform.GetLocation().Equals(FVector(-100, -300, 585), 1e-6) && Run.LocalPoints.Num() == 2 && Run.LocalPoints[1].Equals(FVector(200, 0, 0), 1e-6));
	TestTrue(TEXT("The tower and the multipatch are named in the notes"), Plan.Notes.ContainsByPredicate([](const FString& Note) { return Note.Contains(TEXT("Tower")); })
		&& Plan.Notes.ContainsByPredicate([](const FString& Note) { return Note.Contains(TEXT("multipatch")); }));

	// Without a generic fixture or a truss, those are skipped instead of failing the plan.
	StageMvrImport::FInputs Bare = Inputs;
	Bare.Fallback = StageMvrImport::FStageMvrModeTarget();
	Bare.TrussItem = nullptr;
	const StageMvrImport::FPlan Without = StageMvrImport::Build(Parsed.Scene, Bare);
	TestTrue(TEXT("No generic fixture: B and C are skipped; no truss item: no run"), Without.Fixtures.Num() == 2 && Without.Trusses.IsEmpty() && Without.SkippedTrusses == 2);

	// A second import of the same file places nothing new.
	StageMvrImport::FInputs Again = Inputs;
	for (const TCHAR* Existing : { IdA, IdB, IdC, IdE, IdTruss })
	{
		Again.ExistingMvrIds.Add(Id(Existing));
	}
	TestEqual(TEXT("Everything already on the stage -> an empty plan"), StageMvrImport::Build(Parsed.Scene, Again).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageMvrPlaceTest, "StageCraft.Mvr.Place", StageMvrImportTests::Flags)

bool FStageMvrPlaceTest::RunTest(const FString& Parameters)
{
	using namespace StageMvrImportTests;

	FScopedStage Stage;
	const TArray<ULightingFixtureData*> Modes = BuildSpotModes();
	UStageSpanData* Truss = MakeTruss();
	const FStageMvrParseResult Parsed = StageMvr::ParseSceneDescription(PlanSceneXml());
	if (!TestTrue(TEXT("World, spawner, history, modes and scene"), Stage.Spawner && Stage.EditHistory && Stage.Session && Modes.Num() == 2 && Parsed.IsValid()))
	{
		return false;
	}
	const StageMvrImport::FPlan Plan = StageMvrImport::Build(Parsed.Scene, MakeInputs(*Modes[0], *Modes[1], *Truss));
	const auto Allow = [](const UBaseItemData&, int32) { return FStageEconomyResultInfo::Ok(); };

	// A zero budget places one item per step: the slicing a big rig gets.
	FStageMvrPlacementJob Job(Plan, *Stage.Spawner, Allow);
	int32 Steps = 0;
	while (!Job.Step(0.0) && Steps < 100)
	{
		++Steps;
		TestEqual(TEXT("Each slice adds to the stage"), Stage.CountPlaced(), Job.GetDone());
	}
	TestTrue(TEXT("The job succeeds"), Job.Succeeded());
	TestEqual(TEXT("...one item per step"), Job.GetSteps(), Plan.Num());
	TestEqual(TEXT("Five items on the stage"), Stage.CountPlaced(), 5);

	const auto CheckA = [this, &Stage](const TCHAR* When)
	{
		const ALightingFixtureActor* A = Stage.FindFixture(Id(IdA));
		if (!TestNotNull(*FString::Printf(TEXT("%s: spot A"), When), A))
		{
			return;
		}
		TestEqual(*FString::Printf(TEXT("%s: ID 101"), When), A->GetFixtureId(), 101);
		TestTrue(*FString::Printf(TEXT("%s: patched 1.010"), When), A->GetPatch().Universe == 1 && A->GetPatch().Address == 10);
		TestEqual(*FString::Printf(TEXT("%s: its label"), When), A->GetInstanceLabel().ToString(), FString(TEXT("Spot A")));
		TestTrue(*FString::Printf(TEXT("%s: its aim is its home look"), When), FMath::Abs(A->GetHomeAttributes().FindRef(StageCraftTags::Attribute_Tilt)) > 1.f
			&& FMath::IsNearlyEqual(A->GetAttribute(StageCraftTags::Attribute_Tilt), A->GetHomeAttributes().FindRef(StageCraftTags::Attribute_Tilt)));
		TestEqual(*FString::Printf(TEXT("%s: no state repaired"), When), A->GetStateRepairCount(), 0);
	};
	CheckA(TEXT("Placed"));
	const ALightingFixtureActor* C = Stage.FindFixture(Id(IdC));
	TestTrue(TEXT("C: generic, ID 1, 2.001"), C && C->GetFixtureId() == 1 && C->GetPatch().Universe == 2 && C->GetPatch().Address == 1);
	TestEqual(TEXT("Fixture groups by layer: Rig 3, Floor 1"), Job.GetFixtureIdsByLayer().FindRef(0).Num() * 10 + Job.GetFixtureIdsByLayer().FindRef(1).Num(), 31);

	// One undo step for the whole rig; redo restores IDs, patch, aim and MVR UUIDs.
	TArray<AModularBaseActor*> Placed;
	for (const TWeakObjectPtr<AModularBaseActor>& Item : Job.GetPlaced())
	{
		Placed.Add(Item.Get());
	}
	Stage.EditHistory->RecordPlacements(Placed, FText::FromString(TEXT("Import test.mvr")));
	TestTrue(TEXT("Undo removes the rig in one step"), Stage.EditHistory->Undo() == EStageCommandResult::Succeeded && Stage.CountPlaced() == 0);
	TestTrue(TEXT("Redo brings it back"), Stage.EditHistory->Redo() == EStageCommandResult::Succeeded && Stage.CountPlaced() == 5);
	CheckA(TEXT("Redone"));

	// All or nothing: the third item is refused, everything this job placed goes again.
	const int32 Before = Stage.CountPlaced();
	int32 Checked = 0;
	StageMvrImport::FPlan Fresh = Plan;
	for (StageMvrImport::FFixturePlacement& Fixture : Fresh.Fixtures)
	{
		Fixture.MvrId = FGuid::NewGuid();
		Fixture.FixtureId = 0;
	}
	FStageMvrPlacementJob Refused(Fresh, *Stage.Spawner, [&Checked](const UBaseItemData&, int32)
	{
		return ++Checked == 3 ? FStageEconomyResultInfo::Fail(EStageEconomyResult::SessionRuleViolation, FText::FromString(TEXT("Stage limit reached."))) : FStageEconomyResultInfo::Ok();
	});
	while (!Refused.Step(0.0))
	{
	}
	TestFalse(TEXT("A refusal fails the job"), Refused.Succeeded());
	TestEqual(TEXT("...with the rule's reason"), Refused.GetResult().Message.ToString(), FString(TEXT("Stage limit reached.")));
	TestTrue(TEXT("...and leaves the stage as it was"), Refused.GetPlaced().IsEmpty() && Stage.CountPlaced() == Before);

	// Abort mid-way (the stage closing) rolls back the same way.
	FStageMvrPlacementJob Aborted(Fresh, *Stage.Spawner, Allow);
	Aborted.Step(0.0);
	Aborted.Abort(FText::FromString(TEXT("Closing.")));
	TestTrue(TEXT("Abort rolls back"), Aborted.IsFinished() && !Aborted.Succeeded() && Stage.CountPlaced() == Before);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageMvrReviewTest, "StageCraft.Mvr.Review", StageMvrImportTests::Flags)

bool FStageMvrReviewTest::RunTest(const FString& Parameters)
{
	using namespace StageMvrImportTests;

	// A whole archive: the scene, the spot's GDTF, truss geometry, and a damaged tower geometry.
	TArray<TPair<FString, TArray<uint8>>> Entries;
	Entries.Emplace(TEXT("GeneralSceneDescription.xml"), StageGdtfTestFiles::Utf8(PlanSceneXml()));
	Entries.Emplace(TEXT("Spot.gdtf"), StageGdtfTestFiles::MakeGdtf(StageGdtfTestFiles::SpotXml()));
	Entries.Emplace(TEXT("truss.glb"), MakeGlb(FVector(-1, -0.15, -0.15), FVector(1, 0.15, 0.15)));
	Entries.Emplace(TEXT("tower.glb"), StageGdtfTestFiles::Utf8(TEXT("not a model")));
	const TArray<uint8> Mvr = StageGdtfTestFiles::MakeZip(Entries, /*bDeflate*/ true);

	const FStageMvrArchive Archive = StageMvrArchive::Read(Mvr);
	TestTrue(FString::Printf(TEXT("The archive reads (%s)"), *FString::Join(Archive.Parsed.Errors, TEXT("; "))), Archive.Parsed.IsValid());
	const TSharedPtr<UStageFixtureLibrarySubsystem::FParsedFile>* Spot = Archive.GdtfFiles.Find(TEXT("spot.gdtf"));
	TestTrue(TEXT("The GDTF inside is parsed"), Spot && Spot->IsValid());
	TestTrue(TEXT("Truss geometry bounds are read (mm, Z up)"), Archive.GeometryBounds.Contains(TEXT("truss.glb"))
		&& Archive.GeometryBounds[TEXT("truss.glb")].Max.Equals(FVector(1000, 150, 150), 0.01));
	TestTrue(TEXT("Damaged geometry is reported, not fatal"), Archive.GeometryProblems.Num() == 1 && Archive.GeometryProblems[0].StartsWith(TEXT("tower.glb")));
	TestFalse(TEXT("Scene-object geometry is not read"), Archive.GeometryBounds.Contains(TEXT("set.glb")));
	TestTrue(TEXT("The read measures its working set"), Archive.ArchiveBytes == Mvr.Num() && Archive.LargestEntryBytes > 0);

	// Through the game instance: read for review, nothing changes, the library adds the embedded type once.
	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("StageMvrImport"), FGuid::NewGuid().ToString()));
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, /*RequireExists*/ false, /*Tree*/ true); };
	const FString File = FPaths::Combine(Root, TEXT("Downloads"), TEXT("Plot.mvr"));
	FFileHelper::SaveArrayToFile(Mvr, *File);
	FStageTestGameInstanceOptions Options;
	Options.Root = Root;
	FStageTestGameInstance Game(Options);
	UStageMvrImportSubsystem* Importer = Game.Subsystem<UStageMvrImportSubsystem>();
	UStageFixtureLibrarySubsystem* Library = Game.Subsystem<UStageFixtureLibrarySubsystem>();
	if (!TestTrue(TEXT("The game instance starts with the importer and the library"), Game.IsValid() && Importer && Library))
	{
		return false;
	}
	TestTrue(TEXT("The library is read"), Game.Pump([Library]() { return !Library->HasLoadsInProgress(); }));

	TestTrue(TEXT("A .txt file is refused at once"), Importer->RequestRead(FPaths::Combine(Root, TEXT("plot.txt"))).IsSet());
	TestTrue(TEXT("A missing file is refused at once"), Importer->RequestRead(FPaths::Combine(Root, TEXT("missing.mvr"))).IsSet());
	TestFalse(TEXT("The file is read"), Importer->RequestRead(File).IsSet());
	TestTrue(TEXT("A second file waits for the first"), Importer->RequestRead(File).IsSet());
	TestTrue(TEXT("The review arrives"), Game.Pump([Importer]() { return Importer->GetState() == EStageMvrImportState::Review; }, 20.0));
	const FStageMvrImportReview* Review = Importer->GetReview();
	if (!TestNotNull(TEXT("A review"), Review))
	{
		return false;
	}
	TestTrue(TEXT("It can be imported"), Review->CanImport());
	TestTrue(TEXT("Counts: 6 fixtures, 2 trusses, 2 layers, 1 focus point, 1 other object"), Review->Fixtures == 6 && Review->Trusses == 2 && Review->Layers == 2
		&& Review->FocusPoints == 1 && Review->OtherObjects == 1);
	TestTrue(TEXT("One type, new to the library, used by 4 fixtures (not the multipatch)"), Review->Types.Num() == 1 && Review->Types[0].bUsable && !Review->Types[0].bInLibrary
		&& Review->Types[0].Fixtures == 4 && Review->Types[0].Label.Contains(TEXT("Test Spot")));
	TestTrue(TEXT("The missing mode is named"), Review->Types.Num() == 1 && Review->Types[0].Problem.Contains(TEXT("Missing")));
	TestEqual(TEXT("Two fixtures would be generic (B's mode, C without GDTF)"), Review->GenericFixtures, 2);
	TestEqual(TEXT("Nothing is added to the library before the import"), Library->GetTypes().Num(), 0);

	// No stage in this game instance: the confirm is refused and changes nothing.
	const FStageEconomyResultInfo NoStage = Importer->StartImport(Review->ReviewId, UStageMvrImportSubsystem::FPlacementRequest());
	TestFalse(TEXT("Confirm without a spawner is refused"), NoStage.IsSuccess());
	TestTrue(TEXT("...the review stays and the library is unchanged"), Importer->GetState() == EStageMvrImportState::Review && Library->GetTypes().IsEmpty());
	TestTrue(TEXT("Cancel drops the review"), Importer->CancelReview(Review->ReviewId) && Importer->GetState() == EStageMvrImportState::Idle && !Importer->GetReview());

	// The library adds an embedded GDTF once, then reuses its own copy.
	TSharedRef<UStageFixtureLibrarySubsystem::FParsedFile> Embedded = UStageFixtureLibrarySubsystem::ParseEmbeddedFile(StageGdtfTestFiles::MakeGdtf(StageGdtfTestFiles::SpotXml()), TEXT("Plot.mvr/Spot.gdtf"));
	const FStageFixtureImportOutcome Added = Library->AddEmbeddedFile(*Embedded);
	TestTrue(TEXT("The embedded type is added with its two modes"), Added.IsSuccess() && Added.Modes.Num() == 2 && Library->GetTypes().Num() == 1);
	TSharedRef<UStageFixtureLibrarySubsystem::FParsedFile> Again = UStageFixtureLibrarySubsystem::ParseEmbeddedFile(StageGdtfTestFiles::MakeGdtf(StageGdtfTestFiles::SpotXml()), TEXT("Other.mvr/Spot.gdtf"));
	const FStageFixtureImportOutcome Reused = Library->AddEmbeddedFile(*Again);
	TestTrue(TEXT("The same type again reuses the library's copy"), Reused.Result == EStageFixtureImportResult::AlreadyInLibrary && Reused.Modes.Num() == 2 && Library->GetTypes().Num() == 1);
	TSharedRef<UStageFixtureLibrarySubsystem::FParsedFile> Broken = UStageFixtureLibrarySubsystem::ParseEmbeddedFile(StageGdtfTestFiles::Utf8(TEXT("PK nonsense")), TEXT("Plot.mvr/Broken.gdtf"));
	TestEqual(TEXT("A damaged embedded GDTF is refused"), Library->AddEmbeddedFile(*Broken).Result, EStageFixtureImportResult::Refused);
	Game.Shutdown();
	TestTrue(TEXT("The added type's file is kept in the library folder"), IFileManager::Get().FileExists(*StageFixtureLibrary::GetLibraryFilePath(Id(StageGdtfTestFiles::SpotId))));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
