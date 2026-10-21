// Copyright Epic Games, Inc. All Rights Reserved.

// World tests for a lighting fixture's output path after the fixture benchmark (Docs/ADR/0012-production-reports-and-fixture-
// benchmark.md §3): the beam-shaping light function is attached only while something shapes the beam, pan and tilt reach
// the head in one transform update, and a crossfade writes only the attributes that change.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Lighting; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Actors/LightingFixtureActor.h"
#include "Components/SpotLightComponent.h"
#include "Data/LightingFixtureData.h"
#include "Data/StageParameterTypes.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/AutomationTest.h"
#include "Subsystems/ShowControlSubsystem.h"

namespace StageFixturePlaybackTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	/** A game world that has begun play, destroyed with the scope. Fixtures register with its show control on BeginPlay. */
	struct FScopedWorld
	{
		UWorld* World = nullptr;

		FScopedWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("StageFixturePlaybackTestWorld"));
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

		/** A moving head with a gobo wheel, iris and framing shutters, initialised the way the spawn system does it. */
		ALightingFixtureActor* SpawnProfile() const
		{
			ULightingFixtureData* Data = NewObject<ULightingFixtureData>(GetTransientPackage());
			Data->ColorSystem = EStageFixtureColorSystem::RGBW;
			Data->GoboWheel.SetNum(2);
			Data->bHasIris = true;
			Data->bHasFramingShutters = true;
			Data->bHasPanTilt = true;
			Data->BeamAngleMin = 5.f;
			Data->BeamAngleMax = 50.f;

			ALightingFixtureActor* Fixture = World->SpawnActorDeferred<ALightingFixtureActor>(ALightingFixtureActor::StaticClass(), FTransform::Identity);
			Fixture->InitializeFromItemData(Data);
			Fixture->FinishSpawning(FTransform::Identity);
			return Fixture;
		}
	};

	USceneComponent* FindComponent(const AActor& Actor, const TCHAR* Name)
	{
		TInlineComponentArray<USceneComponent*> Components(&Actor);
		for (USceneComponent* Component : Components)
		{
			if (Component->GetFName() == Name)
			{
				return Component;
			}
		}
		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageFixtureBeamShapingTest, "StageCraft.Lighting.BeamShapingOnDemand", StageFixturePlaybackTests::Flags)

bool FStageFixtureBeamShapingTest::RunTest(const FString& Parameters)
{
	using namespace StageFixturePlaybackTests;

	FScopedWorld Scope;
	ALightingFixtureActor* Fixture = Scope.SpawnProfile();
	USpotLightComponent* Beam = Fixture ? Fixture->FindComponentByClass<USpotLightComponent>() : nullptr;
	if (!TestNotNull(TEXT("Fixture with a beam light"), Beam))
	{
		return false;
	}

	TestNull(TEXT("An open beam has no light function"), Beam->LightFunctionMaterial.Get());

	Fixture->SetAttribute(StageCraftTags::Attribute_Gobo, 1.f);
	TestNotNull(TEXT("A gobo attaches the beam-shaping light function"), Beam->LightFunctionMaterial.Get());
	Fixture->SetAttribute(StageCraftTags::Attribute_Gobo, 0.f);
	TestNull(TEXT("Back to the open slot: detached again"), Beam->LightFunctionMaterial.Get());

	Fixture->SetAttribute(StageCraftTags::Attribute_Iris, 0.5f);
	TestNotNull(TEXT("A closing iris attaches it"), Beam->LightFunctionMaterial.Get());
	Fixture->SetAttribute(StageCraftTags::Attribute_Iris, 0.f);
	TestNull(TEXT("An open iris detaches it"), Beam->LightFunctionMaterial.Get());

	Fixture->SetAttribute(StageCraftTags::Attribute_BladeC, 0.3f);
	TestNotNull(TEXT("A framing blade attaches it"), Beam->LightFunctionMaterial.Get());
	Fixture->SetAttribute(StageCraftTags::Attribute_Dimmer, 0.7f);
	TestNotNull(TEXT("A dimmer change keeps it while the blade is in"), Beam->LightFunctionMaterial.Get());
	Fixture->SetAttribute(StageCraftTags::Attribute_BladeC, 0.f);
	TestNull(TEXT("All blades out: detached"), Beam->LightFunctionMaterial.Get());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageFixturePanTiltTest, "StageCraft.Lighting.PanTiltTransform", StageFixturePlaybackTests::Flags)

bool FStageFixturePanTiltTest::RunTest(const FString& Parameters)
{
	using namespace StageFixturePlaybackTests;

	FScopedWorld Scope;
	ALightingFixtureActor* Fixture = Scope.SpawnProfile();
	USceneComponent* PanPivot = Fixture ? FindComponent(*Fixture, TEXT("PanPivot")) : nullptr;
	USceneComponent* TiltPivot = Fixture ? FindComponent(*Fixture, TEXT("TiltPivot")) : nullptr;
	USceneComponent* Head = Fixture ? FindComponent(*Fixture, TEXT("HeadMesh")) : nullptr;
	USpotLightComponent* Beam = Fixture ? Fixture->FindComponentByClass<USpotLightComponent>() : nullptr;
	if (!TestNotNull(TEXT("Pan pivot"), PanPivot) || !TestNotNull(TEXT("Tilt pivot"), TiltPivot) || !TestNotNull(TEXT("Head"), Head) || !TestNotNull(TEXT("Beam"), Beam))
	{
		return false;
	}

	const auto ExpectWorldFollowsAttributes = [&](const TCHAR* What)
	{
		// The pivots hold the attributes, and each world transform is the relative one on top of the parent's current world one.
		TestTrue(*FString::Printf(TEXT("%s: pan pivot holds the pan"), What),
			PanPivot->GetRelativeRotation().Equals(FRotator(0.0, Fixture->GetAttribute(StageCraftTags::Attribute_Pan), 0.0), 1e-3));
		TestTrue(*FString::Printf(TEXT("%s: tilt pivot holds the tilt"), What),
			TiltPivot->GetRelativeRotation().Equals(FRotator(Fixture->GetAttribute(StageCraftTags::Attribute_Tilt), 0.0, 0.0), 1e-3));
		const FTransform ExpectedPan = PanPivot->GetRelativeTransform() * PanPivot->GetAttachParent()->GetComponentTransform();
		const FTransform ExpectedTilt = TiltPivot->GetRelativeTransform() * ExpectedPan;
		TestTrue(*FString::Printf(TEXT("%s: pan pivot"), What), PanPivot->GetComponentQuat().Equals(ExpectedPan.GetRotation(), 1e-4));
		TestTrue(*FString::Printf(TEXT("%s: tilt pivot"), What), TiltPivot->GetComponentQuat().Equals(ExpectedTilt.GetRotation(), 1e-4));
		TestTrue(*FString::Printf(TEXT("%s: head follows the tilt pivot"), What), Head->GetComponentQuat().Equals((Head->GetRelativeTransform() * ExpectedTilt).GetRotation(), 1e-4));
		TestTrue(*FString::Printf(TEXT("%s: beam follows the tilt pivot"), What), Beam->GetComponentQuat().Equals((Beam->GetRelativeTransform() * ExpectedTilt).GetRotation(), 1e-4));
	};

	TMap<FGameplayTag, float> Both;
	Both.Add(StageCraftTags::Attribute_Pan, 90.f);
	Both.Add(StageCraftTags::Attribute_Tilt, 45.f);
	TestTrue(TEXT("Pan and tilt together"), Fixture->SetAttributes(Both));
	ExpectWorldFollowsAttributes(TEXT("Pan 90, tilt 45"));

	// Tilt alone: the pan pivot's own transform does not change, and the update must still reach everything below it.
	TestTrue(TEXT("Tilt alone"), Fixture->SetAttribute(StageCraftTags::Attribute_Tilt, -30.f));
	ExpectWorldFollowsAttributes(TEXT("Tilt -30"));

	TestTrue(TEXT("Pan alone"), Fixture->SetAttribute(StageCraftTags::Attribute_Pan, -120.f));
	ExpectWorldFollowsAttributes(TEXT("Pan -120"));

	const FQuat HeadBefore = Head->GetComponentQuat();
	TestTrue(TEXT("Dimmer"), Fixture->SetAttribute(StageCraftTags::Attribute_Dimmer, 0.25f));
	TestTrue(TEXT("A dimmer change leaves the head where it is"), Head->GetComponentQuat().Equals(HeadBefore, 1e-6));
	TestEqual(TEXT("A dimmer change reaches the beam (lumens)"), Beam->Intensity, 20000.f * 0.25f, 1e-3f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageFixtureFadeTest, "StageCraft.Lighting.FadeChangedOnly", StageFixturePlaybackTests::Flags)

bool FStageFixtureFadeTest::RunTest(const FString& Parameters)
{
	using namespace StageFixturePlaybackTests;

	FScopedWorld Scope;
	ALightingFixtureActor* Fixture = Scope.SpawnProfile();
	UShowControlSubsystem* Show = UWorld::GetSubsystem<UShowControlSubsystem>(Scope.World);
	if (!TestNotNull(TEXT("Fixture"), Fixture) || !TestNotNull(TEXT("Show control"), Show) || !TestEqual(TEXT("Registered"), Show->GetFixtures().Num(), 1))
	{
		return false;
	}

	Fixture->SetAttribute(StageCraftTags::Attribute_Dimmer, 0.2f);
	Fixture->SetAttribute(StageCraftTags::Attribute_Pan, 10.f);
	TestTrue(TEXT("Store cue 1"), Show->StoreCue(1.f, FText::FromString(TEXT("Dim")), 2.f));
	Fixture->SetAttribute(StageCraftTags::Attribute_Dimmer, 1.f);
	TestTrue(TEXT("Store cue 2 (only the dimmer differs)"), Show->StoreCue(2.f, FText::FromString(TEXT("Full")), 2.f));

	TestTrue(TEXT("Go to cue 1"), Show->GoToCue(1.f));
	Show->FinishFade();
	TestEqual(TEXT("Cue 1 dimmer"), Fixture->GetAttribute(StageCraftTags::Attribute_Dimmer), 0.2f, 1e-5f);

	// Between the fade frames something else moves the head: a fade that wrote every recorded attribute would put it back.
	TestTrue(TEXT("Go to cue 2"), Show->GoToCue(2.f));
	Fixture->SetAttribute(StageCraftTags::Attribute_Pan, 50.f);
	Show->Tick(1.f);
	TestTrue(TEXT("Still fading"), Show->IsFading());
	TestEqual(TEXT("Halfway: dimmer 0.6"), Fixture->GetAttribute(StageCraftTags::Attribute_Dimmer), 0.6f, 1e-4f);
	TestEqual(TEXT("Halfway: an attribute the cues do not change is not written"), Fixture->GetAttribute(StageCraftTags::Attribute_Pan), 50.f, 1e-4f);

	Show->Tick(1.5f);
	TestFalse(TEXT("Fade complete"), Show->IsFading());
	TestEqual(TEXT("End: dimmer at cue 2"), Fixture->GetAttribute(StageCraftTags::Attribute_Dimmer), 1.f, 1e-5f);
	TestEqual(TEXT("End: every recorded value is applied (pan back to 10)"), Fixture->GetAttribute(StageCraftTags::Attribute_Pan), 10.f, 1e-4f);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
