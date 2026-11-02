// Copyright Epic Games, Inc. All Rights Reserved.

// Atmospheres, the studio rig and blended transitions (Docs/ADR/0024-startup-atmospheres-starter-stages.md §3-5).
//  - AtmosphereBlend: the pure blend rules (ends exact, easing, the time of day running forward through a sunset, the sun
//    azimuth taking the short way round, the lighting source switching at the midpoint inside an exposure dip).
//  - AtmospherePresets: the shipped presets load through the Asset Manager in order, are already in range, are distinct and
//    are found again from their own settings.
//  - StudioWorld: in a game world, Studio hides the level's lights behind the rig, lights flat when its HDRI is unavailable,
//    and gives the level back; a Blend shows the old settings, switches the source at the midpoint and ends on the stored
//    settings, while the stored settings are the new ones from the start; only a blend ticks.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Environment; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/ExponentialHeightFogComponent.h"
#include "Components/LightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/Engine.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/TextureCube.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Environment/StageAtmospherePreset.h"
#include "Environment/StageAtmosphereSubsystem.h"
#include "Environment/StageEnvironmentSubsystem.h"
#include "Environment/StageStudioRig.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/AutomationTest.h"
#include "Tests/StageTestFixtures.h"
#include "Tests/StageTestGameInstance.h"

#include <limits>

namespace StageAtmosphereTests
{
	int32 CountRigs(UWorld& World)
	{
		int32 Count = 0;
		for (TActorIterator<AStageStudioRig> It(&World); It; ++It)
		{
			Count += IsValid(*It) ? 1 : 0;
		}
		return Count;
	}

	/** A game world that has begun play, with a level sun and fog; destroyed with the scope. */
	struct FScopedGameWorld
	{
		UWorld* World = nullptr;
		ADirectionalLight* LevelSun = nullptr;
		AExponentialHeightFog* LevelFog = nullptr;

		FScopedGameWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("StageAtmosphereTestWorld"));
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			LevelSun = World->SpawnActor<ADirectionalLight>();
			LevelFog = World->SpawnActor<AExponentialHeightFog>();
			World->BeginPlay();
			World->GetWorldSettings()->NotifyBeginPlay();
		}

		~FScopedGameWorld()
		{
			World->BeginTearingDown();
			World->EndPlay(EEndPlayReason::Quit);
			World->DestroyWorld(/*bInformEngineOfWorld*/ false);
			GEngine->DestroyWorldContext(World);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAtmosphereBlendTest, "StageCraft.Environment.AtmosphereBlend", StageTestFixtures::Flags)

bool FStageAtmosphereBlendTest::RunTest(const FString& Parameters)
{
	using StageEnvironment::BlendForDisplay;

	FStageEnvironmentSettings Day;
	Day.Lighting = EStageLightingSource::OutdoorSky;
	Day.TimeOfDay = 13.f;
	Day.SunAzimuth = 350.f;
	Day.Fog = 0.f;
	Day.Look.WhiteBalanceTemp = 6500.f;

	FStageEnvironmentSettings Sunset = Day;
	Sunset.TimeOfDay = 19.f;
	Sunset.SunAzimuth = 10.f;
	Sunset.Fog = 1.f;
	Sunset.Look.WhiteBalanceTemp = 7500.f;

	TestTrue(TEXT("Alpha 0 is From exactly"), BlendForDisplay(Day, Sunset, 0.f).Equals(Day));
	TestTrue(TEXT("Alpha 1 is To exactly"), BlendForDisplay(Day, Sunset, 1.f).Equals(Sunset));
	TestTrue(TEXT("A NaN alpha ends the blend"), BlendForDisplay(Day, Sunset, std::numeric_limits<float>::quiet_NaN()).Equals(Sunset));

	const FStageEnvironmentSettings Half = BlendForDisplay(Day, Sunset, 0.5f);
	TestEqual(TEXT("Halfway (eased, symmetric): fog in the middle"), Half.Fog, 0.5f, 1.e-4f);
	TestEqual(TEXT("Halfway: time forward, 16:00"), Half.TimeOfDay, 16.f, 1.e-3f);
	TestEqual(TEXT("Halfway: the azimuth crosses north, not the long way round"), FMath::Fmod(Half.SunAzimuth + 360.f, 360.f), 0.f, 1.e-2f);
	TestEqual(TEXT("Halfway: white balance in the middle"), Half.Look.WhiteBalanceTemp, 7000.f, 1.e-1f);
	TestTrue(TEXT("Ease-in: a quarter in, less than a quarter done"), BlendForDisplay(Day, Sunset, 0.25f).Fog < 0.25f);

	// Night (23:00) to morning (07:00) is 8 h ahead: forward through midnight, never back through the afternoon.
	FStageEnvironmentSettings Night = Day;
	Night.TimeOfDay = 23.f;
	FStageEnvironmentSettings Morning = Day;
	Morning.TimeOfDay = 7.f;
	TestEqual(TEXT("Forward through midnight"), BlendForDisplay(Night, Morning, 0.5f).TimeOfDay, 3.f, 1.e-3f);
	// 07:00 to 23:00 is 16 h ahead, more than half a day: backward, through 03:00.
	TestEqual(TEXT("More than 12 h ahead goes backward"), BlendForDisplay(Morning, Night, 0.5f).TimeOfDay, 3.f, 1.e-3f);

	// A source change: the old source until the midpoint, the new one after, darkest at the switch.
	FStageEnvironmentSettings Studio = Day;
	Studio.Lighting = EStageLightingSource::Studio;
	TestTrue(TEXT("Before the midpoint: the old source"), BlendForDisplay(Day, Studio, 0.4f).Lighting == EStageLightingSource::OutdoorSky);
	TestTrue(TEXT("After the midpoint: the new source"), BlendForDisplay(Day, Studio, 0.6f).Lighting == EStageLightingSource::Studio);
	TestEqual(TEXT("Darkest at the switch"), BlendForDisplay(Day, Studio, 0.5f).Look.ExposureCompensation, -StageEnvironment::SourceSwitchDipEV, 1.e-3f);
	TestTrue(TEXT("The dip eases out again"), BlendForDisplay(Day, Studio, 0.95f).Look.ExposureCompensation > -1.f);
	TestEqual(TEXT("No dip without a source change"), BlendForDisplay(Day, Sunset, 0.5f).Look.ExposureCompensation, 0.f, 1.e-4f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAtmospherePresetsTest, "StageCraft.Environment.AtmospherePresets", StageTestFixtures::Flags)

bool FStageAtmospherePresetsTest::RunTest(const FString& Parameters)
{
	using namespace StageTestFixtures;

	FStageTestGameInstance Game;
	if (!TestTrue(TEXT("The test game instance starts"), Game.IsValid()))
	{
		return false;
	}
	UStageAtmosphereSubsystem* Catalog = Game.Subsystem<UStageAtmosphereSubsystem>();
	if (!TestNotNull(TEXT("Atmosphere catalog"), Catalog))
	{
		return false;
	}
	TestTrue(TEXT("The presets load"), Game.Pump([Catalog]() { return Catalog->IsLoaded(); }, 20.0));
	const TArray<UStageAtmospherePreset*>& Presets = Catalog->GetAtmospheres();
	TestEqual(TEXT("Four presets ship"), Presets.Num(), 4);
	TestEqual(TEXT("Progress is complete"), Catalog->GetLoadProgress(), 1.f);

	TSet<EStageLightingSource> Sources;
	for (int32 Index = 0; Index < Presets.Num(); ++Index)
	{
		const UStageAtmospherePreset* Preset = Presets[Index];
		if (!TestNotNull(TEXT("Preset"), Preset))
		{
			continue;
		}
		const FString Name = Preset->GetName();
		TestFalse(*FString::Printf(TEXT("%s has a display name"), *Name), Preset->DisplayName.IsEmpty());
		TestTrue(*FString::Printf(TEXT("%s is already in range"), *Name), StageEnvironment::Sanitize(Preset->Settings).Equals(Preset->Settings));
		TestTrue(*FString::Printf(TEXT("%s is found again from its own settings"), *Name), Catalog->FindMatchingAtmosphere(Preset->Settings) == Preset);
		TestTrue(*FString::Printf(TEXT("%s is found by id"), *Name), Catalog->FindAtmosphere(Preset->GetPrimaryAssetId()) == Preset);
		if (Index > 0 && Presets[Index - 1])
		{
			TestTrue(*FString::Printf(TEXT("%s is in sort order"), *Name), Presets[Index - 1]->SortOrder <= Preset->SortOrder);
			TestFalse(*FString::Printf(TEXT("%s differs from the one before"), *Name), Presets[Index - 1]->Settings.Equals(Preset->Settings));
		}
		Sources.Add(Preset->Settings.Lighting);
	}
	TestTrue(TEXT("Outdoor and studio atmospheres both ship"), Sources.Contains(EStageLightingSource::OutdoorSky) && Sources.Contains(EStageLightingSource::Studio));
	TestNull(TEXT("The default settings match no preset"), Catalog->FindMatchingAtmosphere(FStageEnvironmentSettings()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageStudioWorldTest, "StageCraft.Environment.StudioWorld", StageTestFixtures::Flags)

bool FStageStudioWorldTest::RunTest(const FString& Parameters)
{
	using namespace StageAtmosphereTests;

	// No HDRI configured: the rig must light with its flat fallback. A one-second blend keeps the timeline readable.
	const TUniquePtr<FStageScopedDefault> NoHdri = FStageScopedDefault::Make(UStageEnvironmentSubsystem::StaticClass(), TEXT("StudioCubemap"), TSoftObjectPtr<UTextureCube>());
	const TUniquePtr<FStageScopedDefault> OneSecond = FStageScopedDefault::Make(UStageEnvironmentSubsystem::StaticClass(), TEXT("BlendSeconds"), 1.f);
	if (!TestTrue(TEXT("Config overridden"), NoHdri->IsValid() && OneSecond->IsValid()))
	{
		return false;
	}

	FScopedGameWorld Scope;
	UWorld& World = *Scope.World;
	UStageEnvironmentSubsystem* Environment = World.GetSubsystem<UStageEnvironmentSubsystem>();
	if (!TestNotNull(TEXT("Game worlds have an environment"), Environment))
	{
		return false;
	}
	TestFalse(TEXT("At rest nothing ticks"), Environment->IsTickable());

	// Studio, at once: one rig, the level's sun and fog hidden, a flat light without the HDRI.
	FStageEnvironmentSettings Studio = Environment->GetSettings();
	Studio.Lighting = EStageLightingSource::Studio;
	Environment->SetSettings(Studio);
	TestEqual(TEXT("Studio spawns one rig"), CountRigs(World), 1);
	TestFalse(TEXT("The level sun is hidden"), Scope.LevelSun->GetLightComponent()->IsVisible());
	TestFalse(TEXT("The level fog is hidden"), Scope.LevelFog->GetComponent()->IsVisible());
	TestFalse(TEXT("Without an HDRI the rig lights flat"), Environment->IsStudioHdriAvailable());
	Environment->SetSettings(Studio);
	TestEqual(TEXT("Setting it again changes nothing"), CountRigs(World), 1);

	// Back to the level, blended: stored at once, shown over a second, the source switching at the midpoint.
	FStageEnvironmentSettings Venue = Studio;
	Venue.Lighting = EStageLightingSource::Level;
	Venue.Look.BloomIntensity = 2.f;
	Environment->SetSettings(Venue, EStageEnvironmentTransition::Blend);
	TestTrue(TEXT("The stored settings are the new ones at once"), Environment->GetSettings().Equals(StageEnvironment::Sanitize(Venue)));
	TestTrue(TEXT("A blend ticks"), Environment->IsBlending() && Environment->IsTickable());
	TestEqual(TEXT("Before the first tick the rig is still there"), CountRigs(World), 1);

	Environment->Tick(0.25f);
	TestTrue(TEXT("A quarter in: still the studio"), Environment->GetDisplayedSettings().Lighting == EStageLightingSource::Studio);
	TestTrue(TEXT("A quarter in: darker than both ends"), Environment->GetDisplayedSettings().Look.ExposureCompensation < -1.f);
	Environment->Tick(0.5f);
	TestTrue(TEXT("Past the midpoint: the level's lighting"), Environment->GetDisplayedSettings().Lighting == EStageLightingSource::Level);
	TestEqual(TEXT("Past the midpoint the rig is gone"), CountRigs(World), 0);
	TestTrue(TEXT("The level sun is back"), Scope.LevelSun->GetLightComponent()->IsVisible());
	Environment->Tick(0.5f);
	TestFalse(TEXT("The blend ends"), Environment->IsBlending() || Environment->IsTickable());
	TestTrue(TEXT("It ends on the stored settings"), Environment->GetDisplayedSettings().Equals(Environment->GetSettings()));

	// A blend to the studio and a change in the middle of it: the second blend starts from what is on screen.
	Environment->SetSettings(Studio, EStageEnvironmentTransition::Blend);
	Environment->Tick(0.3f);
	const float ShownBloom = Environment->GetDisplayedSettings().Look.BloomIntensity;
	Environment->SetSettings(Venue, EStageEnvironmentTransition::Blend);
	Environment->Tick(0.f);
	TestEqual(TEXT("A new blend continues from the screen, without a jump"), Environment->GetDisplayedSettings().Look.BloomIntensity, ShownBloom, 1.e-3f);
	Environment->Tick(2.f);
	TestEqual(TEXT("And ends without a rig"), CountRigs(World), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
