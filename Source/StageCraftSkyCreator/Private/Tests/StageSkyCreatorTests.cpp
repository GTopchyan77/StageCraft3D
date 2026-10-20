// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for the Sky Creator sky backend (Docs/ADR/0009-outdoor-environment.md §3): the pure weather mapping, and the
// backend driven through UStageEnvironmentSubsystem in a real game world (spawn, level sky hidden and restored, values
// reaching the sky actor, idempotent re-apply).
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/ExponentialHeightFogComponent.h"
#include "Components/LightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/Engine.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Environment/StageEnvironmentSubsystem.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/AutomationTest.h"
#include "StageSkyCreatorWeather.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4305) // See StageSkyCreatorWeather.h.
#endif
#include "SkyCreatorActor.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace StageSkyCreatorTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	TArray<ASkyCreator*> FindSkies(UWorld& World)
	{
		TArray<ASkyCreator*> Skies;
		for (TActorIterator<ASkyCreator> It(&World); It; ++It)
		{
			if (IsValid(*It))
			{
				Skies.Add(*It);
			}
		}
		return Skies;
	}

	/** A game world that has begun play, with a level sun and fog like L_StageTest's; destroyed with the scope. */
	struct FScopedGameWorld
	{
		UWorld* World = nullptr;
		ADirectionalLight* LevelSun = nullptr;
		AExponentialHeightFog* LevelFog = nullptr;

		FScopedGameWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("StageEnvironmentTestWorld"));
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			LevelSun = World->SpawnActor<ADirectionalLight>();
			LevelFog = World->SpawnActor<AExponentialHeightFog>();
			// Without a game mode, UWorld::BeginPlay runs the subsystems' OnWorldBeginPlay; actors begin play through the
			// world settings, as AGameModeBase::StartPlay would do.
			World->BeginPlay();
			World->GetWorldSettings()->NotifyBeginPlay();
		}

		~FScopedGameWorld()
		{
			// The engine's order for ending a level: tear down, end play, destroy the world, then drop its context.
			World->BeginTearingDown();
			World->EndPlay(EEndPlayReason::Quit);
			World->DestroyWorld(/*bInformEngineOfWorld*/ false);
			GEngine->DestroyWorldContext(World);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageSkyCreatorMappingTest, "StageCraft.Environment.SkyCreatorMapping", StageSkyCreatorTests::Flags)

bool FStageSkyCreatorMappingTest::RunTest(const FString& Parameters)
{
	using StageSkyCreator::MakeWeather;

	FSkyCreatorWeatherSettings Base;
	Base.WeatherFXSettings.RainColor = FLinearColor(0.1f, 0.2f, 0.3f, 0.4f);
	Base.SkyAtmosphereSettings.HeightFogContribution = 0.37f;

	FStageEnvironmentSettings Settings;
	Settings.CloudCoverage = 0.f;
	Settings.Rain = 0.f;
	Settings.Snow = 0.f;
	Settings.Fog = 0.f;
	const FSkyCreatorWeatherSettings Clear = MakeWeather(Base, Settings);
	TestEqual(TEXT("No coverage: no cumulus"), Clear.VolumetricCloudSettings.CumulusCoverage, 0.f);
	TestEqual(TEXT("No coverage: no stratus layer"), Clear.VolumetricCloudSettings.StratusCoverage, 0.f);
	TestEqual(TEXT("No fog: zero density"), Clear.ExponentialHeightFogSettings.FogDensity, 0.f);
	TestEqual(TEXT("No rain: no rain particles"), Clear.WeatherFXSettings.RainAmount, 0.f);

	Settings.CloudCoverage = 0.6f;
	TestEqual(TEXT("Stratus stays open up to 60 % cover"), MakeWeather(Base, Settings).VolumetricCloudSettings.StratusCoverage, 0.f, 1.e-5f);
	Settings.CloudCoverage = 0.8f;
	TestEqual(TEXT("Stratus is half closed at 80 % cover"), MakeWeather(Base, Settings).VolumetricCloudSettings.StratusCoverage, 0.5f, 1.e-5f);
	Settings.CloudCoverage = 1.f;
	const FSkyCreatorWeatherSettings Overcast = MakeWeather(Base, Settings);
	TestEqual(TEXT("Full cover closes the stratus layer"), Overcast.VolumetricCloudSettings.StratusCoverage, 1.f);
	TestEqual(TEXT("Full cover: cumulus follows"), Overcast.VolumetricCloudSettings.CumulusCoverage, 1.f);
	TestEqual(TEXT("Background clouds follow cover"), Overcast.BackgroundCloudSettings.BackgroundCloudsLayerA, 1.f);

	Settings.Rain = 0.7f;
	Settings.Snow = 0.4f;
	Settings.Fog = 1.f;
	const FSkyCreatorWeatherSettings Wet = MakeWeather(Base, Settings);
	TestEqual(TEXT("Rain amount passes through"), Wet.WeatherFXSettings.RainAmount, 0.7f);
	TestEqual(TEXT("Rain wets surfaces"), Wet.MaterialFXSettings.WetnessAmount, 0.7f);
	TestEqual(TEXT("Snow amount passes through"), Wet.WeatherFXSettings.SnowAmount, 0.4f);
	TestEqual(TEXT("Full fog is the maximum density"), Wet.ExponentialHeightFogSettings.FogDensity, StageSkyCreator::MaxFogDensity);

	Settings.Fog = 0.5f;
	const float HalfFog = MakeWeather(Base, Settings).ExponentialHeightFogSettings.FogDensity;
	TestTrue(TEXT("Fog density grows with the slider, slowly at first"), HalfFog > 0.f && HalfFog < StageSkyCreator::MaxFogDensity * 0.5f);

	TestEqual(TEXT("Sky Creator's own tuning is kept (rain colour)"), Wet.WeatherFXSettings.RainColor, Base.WeatherFXSettings.RainColor);
	TestEqual(TEXT("Sky Creator's own tuning is kept (atmosphere)"), Wet.SkyAtmosphereSettings.HeightFogContribution, 0.37f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageSkyCreatorWorldTest, "StageCraft.Environment.SkyCreatorWorld", StageSkyCreatorTests::Flags)

bool FStageSkyCreatorWorldTest::RunTest(const FString& Parameters)
{
	using namespace StageSkyCreatorTests;

	FScopedGameWorld Scope;
	UWorld& World = *Scope.World;
	UStageEnvironmentSubsystem* Environment = World.GetSubsystem<UStageEnvironmentSubsystem>();
	if (!TestNotNull(TEXT("Game worlds have an environment"), Environment))
	{
		return false;
	}
	TestTrue(TEXT("The Sky Creator backend is configured and created"), Environment->IsSkyAvailable());
	TestFalse(TEXT("The sky starts off"), Environment->GetSettings().bSkyEnabled);
	TestEqual(TEXT("No sky actor while the sky is off"), FindSkies(World).Num(), 0);

	// On: one sky, the level's own sun and fog hidden.
	FStageEnvironmentSettings Settings = Environment->GetSettings();
	Settings.bSkyEnabled = true;
	Settings.TimeOfDay = 18.f;
	Settings.Rain = 0.5f;
	Environment->SetSettings(Settings);
	TArray<ASkyCreator*> Skies = FindSkies(World);
	TestEqual(TEXT("Sky on spawns one sky actor"), Skies.Num(), 1);
	TestFalse(TEXT("Level sun is hidden while the sky is on"), Scope.LevelSun->GetLightComponent()->IsVisible());
	TestFalse(TEXT("Level fog is hidden while the sky is on"), Scope.LevelFog->GetComponent()->IsVisible());
	if (Skies.Num() == 1)
	{
		TestEqual(TEXT("Time reaches the sky"), Skies[0]->GetTime(), 18.f, 1.e-3f);
		TestEqual(TEXT("Rain reaches the sky"), Skies[0]->GetWeatherSettings().WeatherFXSettings.RainAmount, 0.5f, 1.e-5f);
	}

	// A change while on updates the same sky; it never spawns a second one.
	Settings.Fog = 1.f;
	Environment->SetSettings(Settings);
	Environment->ApplyWeatherPreset(EStageWeatherPreset::Snow);
	Skies = FindSkies(World);
	TestEqual(TEXT("Further changes reuse the sky actor"), Skies.Num(), 1);
	if (Skies.Num() == 1)
	{
		TestTrue(TEXT("Snow preset reaches the sky"), Skies[0]->GetWeatherSettings().WeatherFXSettings.SnowAmount > 0.f);
		TestEqual(TEXT("Snow preset stops the rain"), Skies[0]->GetWeatherSettings().WeatherFXSettings.RainAmount, 0.f);
	}

	// Off: the sky goes, the level's sun and fog come back.
	Settings = Environment->GetSettings();
	Settings.bSkyEnabled = false;
	Environment->SetSettings(Settings);
	TestEqual(TEXT("Sky off removes the sky actor"), FindSkies(World).Num(), 0);
	TestTrue(TEXT("Level sun is restored"), Scope.LevelSun->GetLightComponent()->IsVisible());
	TestTrue(TEXT("Level fog is restored"), Scope.LevelFog->GetComponent()->IsVisible());
	TestEqual(TEXT("Weather values are kept while off"), Environment->GetSettings().Snow, Settings.Snow);

	// On again: a fresh sky, values re-applied.
	Settings.bSkyEnabled = true;
	Environment->SetSettings(Settings);
	TestEqual(TEXT("Sky on again spawns one sky actor"), FindSkies(World).Num(), 1);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
