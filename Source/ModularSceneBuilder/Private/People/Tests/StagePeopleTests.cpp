// Copyright Epic Games, Inc. All Rights Reserved.

// Tests of Phase I's stand-in people (Docs/ADR/0031, STATE.md #76): the figure rig (mesh LODs, clips, the animation texture
// contract), the audience fill maths, the shared GPU assets and the audience and prop items in a world. The editor's UI sounds are
// gone (StageCraft.People.NoUISounds belongs to the same change: Gevor's brief removed them).
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.People; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Actors/StageAudienceActor.h"
#include "Actors/StagePropActor.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Data/StageAudienceData.h"
#include "Data/StageParameterTypes.h"
#include "Data/StagePropData.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "History/StageItemSnapshot.h"
#include "Interaction/StageCraftCollision.h"
#include "Materials/Material.h"
#include "Misc/AutomationTest.h"
#include "People/StageAudienceMath.h"
#include "People/StageFigureActors.h"
#include "People/StageFigureAssetSubsystem.h"
#include "People/StageFigureRig.h"
#include "Player/ModularPlayerController.h"
#include "StaticMeshResources.h"

#include <limits>

namespace StagePeopleTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	using namespace StageFigure;

	/** Where a rest-pose point of Bone ends up in Pose. */
	FVector Posed(const FPose& Pose, EBone Bone, const FVector& Rest)
	{
		return Pose[static_cast<int32>(Bone)].Apply(Rest);
	}

	const FVector WristL(0.0, -20.5, 93.0);
	const FVector WristR(0.0, 20.5, 93.0);
	const FVector AnkleL(0.0, -9.5, 8.0);
	const FVector AnkleR(0.0, 9.5, 8.0);
	const FVector HeadTop(0.0, 0.0, 175.0);

	/** A game world that has begun play, destroyed with the scope. */
	struct FScopedWorld
	{
		UWorld* World = nullptr;

		FScopedWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("StagePeopleTestWorld"));
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			World->BeginPlay();
			World->GetWorldSettings()->NotifyBeginPlay();
		}

		~FScopedWorld()
		{
			World->BeginTearingDown();
			World->EndPlay(EEndPlayReason::Quit);
			World->DestroyWorld(/*bInformEngineOfWorld*/ false);
			GEngine->DestroyWorldContext(World);
		}

		template <typename TActor>
		TActor* Spawn(UBaseItemData* Data, const FStageItemSnapshot* Snapshot = nullptr) const
		{
			TActor* Actor = World->SpawnActorDeferred<TActor>(TActor::StaticClass(), FTransform::Identity);
			Actor->AssignInstanceId(FGuid::NewGuid());
			if (Snapshot)
			{
				Actor->RestoreSnapshotState(*Snapshot);
			}
			Actor->InitializeFromItemData(Data);
			Actor->FinishSpawning(FTransform::Identity);
			return Actor;
		}
	};

	UStageAudienceData* MakeAudience()
	{
		UStageAudienceData* Data = NewObject<UStageAudienceData>(GetTransientPackage());
		Data->FigureMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
		Data->DefaultSize = FVector2D(1000.0, 500.0);
		Data->DefaultDensity = 2.0;
		Data->TopColors = { FLinearColor::Red, FLinearColor::Black };
		return Data;
	}

	template <typename TValue>
	TValue* Field(UObject& Object, const TCHAR* Name)
	{
		const FProperty* Property = Object.GetClass()->FindPropertyByName(Name);
		return Property ? Property->ContainerPtrToValuePtr<TValue>(&Object) : nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageFigureRigTest, "StageCraft.People.FigureRig", StagePeopleTests::Flags)

bool FStageFigureRigTest::RunTest(const FString& Parameters)
{
	using namespace StagePeopleTests;

	// The rest pose leaves every bone where it is.
	const FPose Rest = Solve(FLocalPose());
	bool bRestIdentity = true;
	for (const StageFigure::FBoneTransform& Bone : Rest)
	{
		bRestIdentity &= Bone.Rotation.Equals(FQuat::Identity, 1e-6) && Bone.Translation.IsNearlyZero(1e-4);
	}
	TestTrue(TEXT("The rest pose moves nothing"), bRestIdentity);
	for (int32 BoneIndex = 1; BoneIndex < BoneCount; ++BoneIndex)
	{
		TestTrue(FString::Printf(TEXT("Bone %d has its parent first"), BoneIndex), static_cast<int32>(GetParent(static_cast<EBone>(BoneIndex))) < BoneIndex);
	}

	// Three LODs, each coarser, every vertex on a bone and a region, every face wound outward (its normal agrees with its vertices').
	int32 PreviousTriangles = MAX_int32;
	for (int32 Lod = 0; Lod < LodCount; ++Lod)
	{
		const FMeshData Mesh = BuildMesh(Lod);
		if (!TestTrue(FString::Printf(TEXT("LOD %d is valid"), Lod), Mesh.IsValid()))
		{
			continue;
		}
		TestTrue(FString::Printf(TEXT("LOD %d (%d triangles) is coarser than the one before"), Lod, Mesh.GetTriangleCount()), Mesh.GetTriangleCount() < PreviousTriangles);
		PreviousTriangles = Mesh.GetTriangleCount();
		FBox Bounds(ForceInit);
		int32 Inward = 0;
		bool bRegions = true;
		for (int32 Index = 0; Index < Mesh.Positions.Num(); ++Index)
		{
			Bounds += FVector(Mesh.Positions[Index]);
			bRegions &= Mesh.Regions[Index] == RegionSkin || Mesh.Regions[Index] == RegionTop || Mesh.Regions[Index] == RegionBottom || Mesh.Regions[Index] == RegionDark;
		}
		for (int32 Index = 0; Index + 2 < Mesh.Indices.Num(); Index += 3)
		{
			const FVector P0(Mesh.Positions[Mesh.Indices[Index]]);
			const FVector P1(Mesh.Positions[Mesh.Indices[Index + 1]]);
			const FVector P2(Mesh.Positions[Mesh.Indices[Index + 2]]);
			const FVector Face = (P1 - P2) ^ (P0 - P2);
			const FVector VertexNormals = FVector(Mesh.Normals[Mesh.Indices[Index]] + Mesh.Normals[Mesh.Indices[Index + 1]] + Mesh.Normals[Mesh.Indices[Index + 2]]);
			Inward += (Face | VertexNormals) < 0.0 ? 1 : 0;
		}
		TestEqual(FString::Printf(TEXT("LOD %d: no face is wound inward"), Lod), Inward, 0);
		TestTrue(FString::Printf(TEXT("LOD %d: every vertex has a known region"), Lod), bRegions);
		TestTrue(FString::Printf(TEXT("LOD %d stands on the floor (%.1f) and is about %.0f cm tall (%.1f)"), Lod, Bounds.Min.Z, Height, Bounds.Max.Z),
			FMath::IsNearlyEqual(Bounds.Min.Z, 0.0, 1.0) && Bounds.Max.Z > Height - 10.0 && Bounds.Max.Z < Height + 5.0);
	}
	TestTrue(TEXT("LOD 0 is a few hundred triangles"), BuildMesh(0).GetTriangleCount() < 800);
	TestFalse(TEXT("An unknown LOD builds nothing"), BuildMesh(LodCount).IsValid());

	// Every clip: finite, unit rotations, loops seamlessly and moves smoothly from frame to frame.
	for (int32 ClipIndex = 0; ClipIndex < ClipCount; ++ClipIndex)
	{
		const EStageFigureClip Clip = static_cast<EStageFigureClip>(ClipIndex);
		const FString Name = GetClipName(Clip);
		TestTrue(Name + TEXT(" has a positive rate"), GetClipCyclesPerSecond(Clip) > 0.f);
		bool bFinite = true;
		double LargestStep = 0.0;
		FPose Previous = EvaluateClip(Clip, 0.0);
		for (int32 Frame = 1; Frame <= FramesPerClip; ++Frame)
		{
			const FPose Pose = EvaluateClip(Clip, double(Frame) / FramesPerClip);
			for (int32 BoneIndex = 0; BoneIndex < BoneCount; ++BoneIndex)
			{
				bFinite &= !Pose[BoneIndex].Translation.ContainsNaN() && FMath::IsNearlyEqual(Pose[BoneIndex].Rotation.Size(), 1.0, 1e-4);
			}
			for (const FVector& Point : { WristL, WristR, AnkleL, AnkleR, HeadTop })
			{
				const EBone Bone = Point == HeadTop ? EBone::Head : Point == WristL ? EBone::ForearmL : Point == WristR ? EBone::ForearmR
					: Point == AnkleL ? EBone::ShinL : EBone::ShinR;
				LargestStep = FMath::Max(LargestStep, FVector::Dist(Posed(Pose, Bone, Point), Posed(Previous, Bone, Point)));
			}
			Previous = Pose;
		}
		TestTrue(Name + TEXT(" is finite with unit rotations"), bFinite);
		TestTrue(FString::Printf(TEXT("%s moves at most 20 cm between frames, the wrap included (%.1f)"), *Name, LargestStep), LargestStep < 20.0);
		TestTrue(Name + TEXT(" ends where it starts"), Posed(EvaluateClip(Clip, 1.0), EBone::ForearmL, WristL).Equals(Posed(EvaluateClip(Clip, 0.0), EBone::ForearmL, WristL), 1e-3));
	}

	// What each clip is for.
	for (int32 Frame = 0; Frame < FramesPerClip; ++Frame)
	{
		const double T = double(Frame) / FramesPerClip;
		const FPose Cheer = EvaluateClip(EStageFigureClip::CrowdCheer, T);
		if (Posed(Cheer, EBone::ForearmL, WristL).Z < HeadTop.Z || Posed(Cheer, EBone::ForearmR, WristR).Z < HeadTop.Z)
		{
			AddError(FString::Printf(TEXT("Cheering hands are above the head (frame %d)"), Frame));
			break;
		}
	}
	for (const EStageFigureClip Standing : { EStageFigureClip::CrowdIdle, EStageFigureClip::CrowdBounce, EStageFigureClip::CrowdClap, EStageFigureClip::CrowdCheer,
		EStageFigureClip::Guitarist, EStageFigureClip::Vocalist, EStageFigureClip::Keyboardist, EStageFigureClip::DeskOperator, EStageFigureClip::Stagehand })
	{
		double Lowest = UE_BIG_NUMBER;
		double Highest = -UE_BIG_NUMBER;
		for (int32 Frame = 0; Frame < FramesPerClip; ++Frame)
		{
			const FPose Pose = EvaluateClip(Standing, double(Frame) / FramesPerClip);
			for (const double Z : { Posed(Pose, EBone::ShinL, AnkleL).Z, Posed(Pose, EBone::ShinR, AnkleR).Z })
			{
				Lowest = FMath::Min(Lowest, Z);
				Highest = FMath::Max(Highest, Z);
			}
		}
		TestTrue(FString::Printf(TEXT("%s keeps the feet on the floor (ankles %.1f to %.1f cm)"), *GetClipName(Standing), Lowest, Highest), Lowest > 3.0 && Highest < 18.0);
	}
	const FPose Seated = EvaluateClip(EStageFigureClip::Drummer, 0.0);
	TestTrue(TEXT("The drummer sits (pelvis about 55 cm up)"), FMath::IsNearlyEqual(Posed(Seated, EBone::Pelvis, FVector(0.0, 0.0, 84.0)).Z, 56.0, 6.0));
	TestTrue(TEXT("The drummer's feet reach the pedals on the floor"), FMath::IsNearlyEqual(Posed(Seated, EBone::ShinR, AnkleR).Z, 8.0, 6.0));
	const double BounceDown = Posed(EvaluateClip(EStageFigureClip::CrowdBounce, 0.0), EBone::Head, HeadTop).Z;
	const double BounceUp = Posed(EvaluateClip(EStageFigureClip::CrowdBounce, 0.5), EBone::Head, HeadTop).Z;
	TestTrue(FString::Printf(TEXT("A bounce is down on the beat (%.1f) and up between beats (%.1f)"), BounceDown, BounceUp), BounceUp > BounceDown + 4.0);
	const double ClapClosed = FVector::Dist(Posed(EvaluateClip(EStageFigureClip::CrowdClap, 0.0), EBone::ForearmL, WristL), Posed(EvaluateClip(EStageFigureClip::CrowdClap, 0.0), EBone::ForearmR, WristR));
	const double ClapOpen = FVector::Dist(Posed(EvaluateClip(EStageFigureClip::CrowdClap, 0.25), EBone::ForearmL, WristL), Posed(EvaluateClip(EStageFigureClip::CrowdClap, 0.25), EBone::ForearmR, WristR));
	TestTrue(FString::Printf(TEXT("Clapping hands meet (%.1f cm) and open (%.1f cm)"), ClapClosed, ClapOpen), ClapClosed < 15.0 && ClapOpen > ClapClosed + 10.0);

	// The texture is the clips, frame by frame: two texels per bone, rotation then translation.
	const TArray<FVector4f> Texels = BakeAnimationTexels();
	TestEqual(TEXT("One texel pair per bone per frame of every clip"), Texels.Num(), TextureWidth * TextureHeight);
	const EStageFigureClip Probe = EStageFigureClip::Guitarist;
	const int32 ProbeFrame = 7;
	const FPose Expected = EvaluateClip(Probe, double(ProbeFrame) / FramesPerClip);
	const int32 Row = static_cast<int32>(Probe) * FramesPerClip + ProbeFrame;
	const int32 ProbeBone = static_cast<int32>(EBone::ForearmR);
	const FVector4f Rotation = Texels[Row * TextureWidth + ProbeBone * 2];
	const FVector4f Translation = Texels[Row * TextureWidth + ProbeBone * 2 + 1];
	const FQuat Stored(Rotation.X, Rotation.Y, Rotation.Z, Rotation.W);
	TestTrue(TEXT("A texel holds its bone's rotation (either sign)"), FMath::Abs(Stored | Expected[ProbeBone].Rotation) > 0.9999);
	TestTrue(TEXT("and the next one its translation"), FVector(Translation.X, Translation.Y, Translation.Z).Equals(Expected[ProbeBone].Translation, 0.01));
	bool bHemisphere = true;
	for (int32 Frame = 1; Frame < FramesPerClip; ++Frame)
	{
		const FVector4f A = Texels[(Row - ProbeFrame + Frame - 1) * TextureWidth + ProbeBone * 2];
		const FVector4f B = Texels[(Row - ProbeFrame + Frame) * TextureWidth + ProbeBone * 2];
		bHemisphere &= (A.X * B.X + A.Y * B.Y + A.Z * B.Z + A.W * B.W) >= 0.f;
	}
	TestTrue(TEXT("Neighbouring frames keep their rotations in one hemisphere (the material blends them)"), bHemisphere);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAudienceMathTest, "StageCraft.People.AudienceMath", StagePeopleTests::Flags)

bool FStageAudienceMathTest::RunTest(const FString& Parameters)
{
	using namespace StageAudienceMath;

	TestEqual(TEXT("10 x 10 m at 2 per m² holds 200"), GetPeopleCount(1000.0, 1000.0, 2.0, 20000), 200);
	TestEqual(TEXT("The item's cap wins"), GetPeopleCount(5000.0, 5000.0, 4.0, 1500), 1500);
	TestEqual(TEXT("Density is capped at the packed limit"), GetPeopleCount(1000.0, 1000.0, 50.0, 20000), int32(MaxDensity * 100.0));
	TestEqual(TEXT("No area, nobody"), GetPeopleCount(0.0, 1000.0, 2.0, 100), 0);
	TestEqual(TEXT("A negative density, nobody"), GetPeopleCount(1000.0, 1000.0, -1.0, 100), 0);
	TestEqual(TEXT("A non-finite size, nobody"), GetPeopleCount(std::numeric_limits<double>::quiet_NaN(), 1000.0, 2.0, 100), 0);

	for (const EStageCrowdMood Mood : { EStageCrowdMood::Calm, EStageCrowdMood::Dancing, EStageCrowdMood::Cheering })
	{
		float Sum = 0.f;
		for (const TPair<EStageFigureClip, float>& Entry : GetMoodMix(Mood))
		{
			Sum += Entry.Value;
		}
		TestTrue(FString::Printf(TEXT("Mood %d's shares add up to 1"), static_cast<int32>(Mood)), FMath::IsNearlyEqual(Sum, 1.f, 1e-4f));
	}

	const double Width = 1200.0;
	const double Depth = 800.0;
	const int32 Count = GetPeopleCount(Width, Depth, 2.5, 20000);
	const TArray<FLinearColor> Colours = { FLinearColor::Red, FLinearColor::Blue, FLinearColor::White };
	TArray<FPerson> People;
	PlacePeople(Width, Depth, Count, EStageCrowdMood::Dancing, 42, Colours, People);
	TestEqual(TEXT("Everyone asked for is placed"), People.Num(), Count);

	TArray<FPerson> Again;
	PlacePeople(Width, Depth, Count, EStageCrowdMood::Dancing, 42, Colours, Again);
	bool bSame = Again.Num() == People.Num();
	for (int32 Index = 0; bSame && Index < People.Num(); ++Index)
	{
		bSame &= People[Index].Transform.Equals(Again[Index].Transform) && People[Index].Clip == Again[Index].Clip && People[Index].Phase == Again[Index].Phase;
	}
	TestTrue(TEXT("The same seed gives the same crowd"), bSame);
	TArray<FPerson> Other;
	PlacePeople(Width, Depth, Count, EStageCrowdMood::Dancing, 43, Colours, Other);
	TestFalse(TEXT("Another seed gives another crowd"), Other[0].Transform.Equals(People[0].Transform));

	const TArray<TPair<EStageFigureClip, float>> Mix = GetMoodMix(EStageCrowdMood::Dancing);
	bool bInside = true;
	bool bFacing = true;
	bool bFromMix = true;
	bool bColours = true;
	int32 Bouncing = 0;
	double Closest = UE_BIG_NUMBER;
	for (int32 Index = 0; Index < People.Num(); ++Index)
	{
		const FPerson& Person = People[Index];
		const FVector Location = Person.Transform.GetLocation();
		bInside &= Location.X > 0.0 && Location.X < Depth && FMath::Abs(Location.Y) < Width * 0.5 && Location.Z == 0.0;
		bFacing &= FMath::Abs(FRotator::NormalizeAxis(Person.Transform.Rotator().Yaw - 180.0)) <= 22.5;
		bFromMix &= Mix.ContainsByPredicate([&Person](const TPair<EStageFigureClip, float>& Entry) { return Entry.Key == Person.Clip; });
		bColours &= Colours.Contains(Person.TopColor) && Person.CyclesPerSecond > 0.f;
		Bouncing += Person.Clip == EStageFigureClip::CrowdBounce ? 1 : 0;
		for (int32 Other2 = Index + 1; Other2 < People.Num(); ++Other2)
		{
			Closest = FMath::Min(Closest, FVector::Dist2D(Location, People[Other2].Transform.GetLocation()));
		}
	}
	TestTrue(TEXT("Everyone stands inside the area, on the floor"), bInside);
	TestTrue(TEXT("Everyone faces the stage (-X), give or take a slight turn"), bFacing);
	TestTrue(TEXT("Every clip comes from the mood"), bFromMix);
	TestTrue(TEXT("Shirts come from the palette and everyone moves"), bColours);
	TestTrue(FString::Printf(TEXT("A dancing crowd mostly bounces (%d of %d)"), Bouncing, People.Num()), Bouncing > People.Num() / 2);
	TestTrue(FString::Printf(TEXT("Nobody stands in anybody (closest %.1f cm)"), Closest), Closest > 18.0);

	PlacePeople(Width, Depth, 0, EStageCrowdMood::Calm, 1, Colours, People);
	TestEqual(TEXT("Nobody asked for, nobody placed"), People.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageFigureAssetsTest, "StageCraft.People.FigureAssets", StagePeopleTests::Flags)

bool FStageFigureAssetsTest::RunTest(const FString& Parameters)
{
	UStaticMesh* Mesh = UStageFigureAssetSubsystem::BuildFigureMesh(GetTransientPackage());
	if (!TestNotNull(TEXT("The figure mesh builds"), Mesh))
	{
		return false;
	}
	TestEqual(TEXT("with every LOD"), Mesh->GetNumLODs(), StageFigure::LodCount);
	const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
	if (TestNotNull(TEXT("and render data"), RenderData))
	{
		TestTrue(TEXT("whose LODs switch as the figure gets smaller on screen"),
			RenderData->ScreenSize[1].Default < RenderData->ScreenSize[0].Default && RenderData->ScreenSize[2].Default < RenderData->ScreenSize[1].Default);
	}
	TestTrue(TEXT("Its bounds leave room for raised arms"), Mesh->GetBounds().GetBox().Max.Z > StageFigure::Height + 30.0);

	UTexture2D* Texture = UStageFigureAssetSubsystem::BuildAnimationTexture(GetTransientPackage());
	if (TestNotNull(TEXT("The animation texture builds"), Texture))
	{
		TestEqual(TEXT("Its width is two texels per bone"), Texture->GetSizeX(), StageFigure::TextureWidth);
		TestEqual(TEXT("Its height is every frame of every clip"), Texture->GetSizeY(), StageFigure::TextureHeight);
		TestTrue(TEXT("It is read texel by texel"), Texture->Filter == TF_Nearest);
		TestFalse(TEXT("without colour conversion"), Texture->SRGB != 0);
	}

	UStageFigureAssetSubsystem* Assets = GEngine ? GEngine->GetEngineSubsystem<UStageFigureAssetSubsystem>() : nullptr;
	if (TestNotNull(TEXT("The engine has the figure assets"), Assets))
	{
		TestTrue(TEXT("The shared mesh is built once"), Assets->GetFigureMesh() != nullptr && Assets->GetFigureMesh() == Assets->GetFigureMesh());
		UMaterialInterface* Parent = UMaterial::GetDefaultMaterial(MD_Surface);
		TestTrue(TEXT("One material instance per parent material"), Assets->GetFigureMaterial(Parent) == Assets->GetFigureMaterial(Parent));
		TestNull(TEXT("and none without a parent"), Assets->GetFigureMaterial(nullptr));
	}

	StageAudienceMath::FPerson Person;
	Person.Clip = EStageFigureClip::Vocalist;
	Person.Phase = 1.25f;
	Person.CyclesPerSecond = 0.5f;
	Person.TopColor = FLinearColor(0.1f, 0.2f, 0.3f);
	Person.SkinTone = 2.f;
	TArray<float> Data;
	StageFigureActors::MakeCustomData(Person, Data);
	TestEqual(TEXT("Eight custom data floats"), Data.Num(), UStageFigureAssetSubsystem::CustomDataCount);
	TestEqual(TEXT("The clip by its index"), Data[UStageFigureAssetSubsystem::Clip], float(static_cast<uint8>(EStageFigureClip::Vocalist)));
	TestEqual(TEXT("The phase within one loop"), Data[UStageFigureAssetSubsystem::Phase], 0.25f);
	TestEqual(TEXT("The rate"), Data[UStageFigureAssetSubsystem::Rate], 0.5f);
	TestEqual(TEXT("The shirt's green"), Data[UStageFigureAssetSubsystem::TopG], 0.2f);
	TestEqual(TEXT("A skin tone past dark is dark"), Data[UStageFigureAssetSubsystem::SkinTone], 1.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStagePeopleItemsTest, "StageCraft.People.Items", StagePeopleTests::Flags)

bool FStagePeopleItemsTest::RunTest(const FString& Parameters)
{
	using namespace StagePeopleTests;
	FScopedWorld Scope;

	// An audience area from the Library: the item's default size, density and mood, filled.
	UStageAudienceData* Audience = MakeAudience();
	AStageAudienceActor* Area = Scope.Spawn<AStageAudienceActor>(Audience);
	if (!TestNotNull(TEXT("The audience area spawns"), Area))
	{
		return false;
	}
	TestTrue(TEXT("It takes the default size"), Area->GetAreaSize().Equals(FVector2D(1000.0, 500.0)));
	TestEqual(TEXT("and fills it at the default density"), Area->GetPeopleCount(), 100);
	TestTrue(TEXT("dancing"), Area->GetMood() == EStageCrowdMood::Dancing);
	TestFalse(TEXT("It never ticks"), Area->PrimaryActorTick.bCanEverTick);
	TestFalse(TEXT("It cannot be scaled (its size is its area)"), Area->SupportsScaling());

	// Parameters: clamped writes rebuild the crowd; People is read-only.
	TestTrue(TEXT("Density is a parameter"), IStageParameterInterface::Execute_SetParameterValue(Area, StageCraftTags::Param_Audience_Density, FStageParameterValue::MakeFloat(4.0)));
	TestEqual(TEXT("which refills the area"), Area->GetPeopleCount(), 200);
	IStageParameterInterface::Execute_SetParameterValue(Area, StageCraftTags::Param_Audience_Width, FStageParameterValue::MakeFloat(1.0e7));
	TestEqual(TEXT("A width past the item's limit snaps to it"), Area->GetAreaSize().X, Audience->GetMaxSize());
	IStageParameterInterface::Execute_SetParameterValue(Area, StageCraftTags::Param_Audience_Width, FStageParameterValue::MakeFloat(1000.0));
	TestTrue(TEXT("Mood is a parameter"), IStageParameterInterface::Execute_SetParameterValue(Area, StageCraftTags::Param_Audience_Mood, FStageParameterValue::MakeEnum(2)));
	TestTrue(TEXT("cheering"), Area->GetMood() == EStageCrowdMood::Cheering);
	TestFalse(TEXT("An unknown mood is refused"), IStageParameterInterface::Execute_SetParameterValue(Area, StageCraftTags::Param_Audience_Mood, FStageParameterValue::MakeEnum(7)));
	TestFalse(TEXT("People cannot be written"), IStageParameterInterface::Execute_SetParameterValue(Area, StageCraftTags::Param_Audience_People, FStageParameterValue::MakeInteger(5)));
	FStageParameterValue PeopleValue;
	TestTrue(TEXT("People reads the crowd"), IStageParameterInterface::Execute_GetParameterValue(Area, StageCraftTags::Param_Audience_People, PeopleValue) && PeopleValue.Integer == 200);
	TestTrue(TEXT("Audience edits are undoable shape edits"), StageCraftTags::IsShapeParameter(StageCraftTags::Param_Audience_Density)
		&& StageCraftTags::IsShapeParameter(StageCraftTags::Param_Audience_Mood) && !StageCraftTags::IsShapeParameter(StageCraftTags::Param_Audience_People));

	// Saved and restored: the same crowd comes back; a damaged file is repaired and counted.
	const FStageItemSnapshot Snapshot = Area->CaptureSnapshot();
	AStageAudienceActor* Restored = Scope.Spawn<AStageAudienceActor>(Audience, &Snapshot);
	TestTrue(TEXT("A restored area has its size"), Restored->GetAreaSize().Equals(Area->GetAreaSize()));
	TestTrue(TEXT("its mood"), Restored->GetMood() == EStageCrowdMood::Cheering);
	TestEqual(TEXT("and its people"), Restored->GetPeopleCount(), Area->GetPeopleCount());
	TestEqual(TEXT("A clean restore repairs nothing"), Restored->GetStateRepairCount(), 0);
	TArray<UInstancedStaticMeshComponent*> ComponentsA;
	TArray<UInstancedStaticMeshComponent*> ComponentsB;
	Area->GetComponents(ComponentsA);
	Restored->GetComponents(ComponentsB);
	FTransform FirstA;
	FTransform FirstB;
	if (ComponentsA.Num() == 1 && ComponentsB.Num() == 1 && ComponentsA[0]->GetInstanceCount() > 0 && ComponentsB[0]->GetInstanceCount() > 0)
	{
		ComponentsA[0]->GetInstanceTransform(0, FirstA);
		ComponentsB[0]->GetInstanceTransform(0, FirstB);
		TestTrue(TEXT("The same people stand in the same places"), FirstA.Equals(FirstB));
		TestEqual(TEXT("Each carries its clip and colours"), ComponentsA[0]->NumCustomDataFloats, UStageFigureAssetSubsystem::CustomDataCount);
		TestFalse(TEXT("A crowd casts no shadows"), ComponentsA[0]->CastShadow);
		TestTrue(TEXT("and no trace hits a person"), ComponentsA[0]->GetCollisionEnabled() == ECollisionEnabled::NoCollision);
	}
	else
	{
		AddError(TEXT("An audience area has one crowd component with people"));
	}

	AStageAudienceActor* Damaged = Scope.World->SpawnActorDeferred<AStageAudienceActor>(AStageAudienceActor::StaticClass(), FTransform::Identity);
	Damaged->RestoreSnapshotState(Snapshot);
	*Field<double>(*Damaged, TEXT("Density")) = std::numeric_limits<double>::quiet_NaN();
	*Field<FVector2D>(*Damaged, TEXT("AreaSize")) = FVector2D(-5.0, 1.0e9);
	*Field<EStageCrowdMood>(*Damaged, TEXT("Mood")) = static_cast<EStageCrowdMood>(200);
	Damaged->InitializeFromItemData(Audience);
	Damaged->FinishSpawning(FTransform::Identity);
	TestEqual(TEXT("A damaged area is repaired: width, depth, density and mood"), Damaged->GetStateRepairCount(), 4);
	TestTrue(TEXT("within the item's limits"), Damaged->GetAreaSize().X >= Audience->GetMinSize() && Damaged->GetAreaSize().Y <= Audience->GetMaxSize()
		&& Damaged->GetDensity() >= Audience->GetMinDensity() && Damaged->GetMood() < EStageCrowdMood::Count);

	// A band member: parts and an animated figure, selectable through the stage item channel only.
	UStagePropData* Drums = NewObject<UStagePropData>(GetTransientPackage());
	Drums->FigureMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
	FStageSpanPart Kick;
	Kick.Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	Drums->Parts = { Kick, Kick };
	FStageFigurePlacement Drummer;
	Drummer.Clip = EStageFigureClip::Drummer;
	Drums->Figures = { Drummer };
	AStagePropActor* Kit = Scope.Spawn<AStagePropActor>(Drums);
	if (TestNotNull(TEXT("A prop spawns"), Kit))
	{
		TestEqual(TEXT("with its parts"), Kit->GetPartInstanceCount(), 2);
		TestEqual(TEXT("and its drummer"), Kit->GetFigureCount(), 1);
		TestFalse(TEXT("and never ticks"), Kit->PrimaryActorTick.bCanEverTick);
		TArray<UInstancedStaticMeshComponent*> Components;
		Kit->GetComponents(Components);
		const UInstancedStaticMeshComponent* const* Figures = Components.FindByPredicate([](const UInstancedStaticMeshComponent* Component)
		{
			return Component->NumCustomDataFloats == UStageFigureAssetSubsystem::CustomDataCount;
		});
		if (TestNotNull(TEXT("The figures have their own component"), Figures))
		{
			TestTrue(TEXT("A performer can be clicked (stage item channel)"), (*Figures)->GetCollisionResponseToChannel(StageCraftCollision::StageItemChannel) == ECR_Block);
			TestTrue(TEXT("but placement and the camera look through"), (*Figures)->GetCollisionResponseToChannel(ECC_Visibility) == ECR_Ignore);
		}
	}

	// Without a figure material the parts still show and the people are left out.
	UStagePropData* NoMaterial = NewObject<UStagePropData>(GetTransientPackage());
	NoMaterial->Parts = { Kick };
	NoMaterial->Figures = { Drummer };
	AddExpectedMessage(TEXT("has no figure material"), EAutomationExpectedErrorFlags::Contains, 0);
	AStagePropActor* Bare = Scope.Spawn<AStagePropActor>(NoMaterial);
	TestEqual(TEXT("A prop without a figure material keeps its parts"), Bare->GetPartInstanceCount(), 1);
	TestEqual(TEXT("and shows nobody"), Bare->GetFigureCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageNoUISoundsTest, "StageCraft.People.NoUISounds", StagePeopleTests::Flags)

bool FStageNoUISoundsTest::RunTest(const FString& Parameters)
{
	// Gevor's brief (STATE.md #76): no interface sounds. The player controller, where every click, selection, placement and refusal
	// arrives, carries no component that could play one.
	const AModularPlayerController* Controller = GetDefault<AModularPlayerController>();
	TArray<UActorComponent*> Components;
	Controller->GetComponents(Components);
	for (const UActorComponent* Component : Components)
	{
		TestFalse(FString::Printf(TEXT("%s is no sound feedback component"), *Component->GetName()), Component->GetName().Contains(TEXT("Audio")));
	}
	TestNull(TEXT("The feedback cue class is gone"), FindFirstObject<UClass>(TEXT("StageEditorAudioFeedbackComponent"), EFindFirstObjectOptions::None));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
