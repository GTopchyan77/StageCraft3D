// Copyright Epic Games, Inc. All Rights Reserved.

// Unit tests for the outdoor environment's settings rules (Docs/ADR/0009-outdoor-environment.md). The sky backend is
// tested with a real world in the StageCraftSkyCreator module.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Environment/StageEnvironmentTypes.h"
#include "Misc/AutomationTest.h"

#include <limits>

namespace StageEnvironmentTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageEnvironmentSanitizeTest, "StageCraft.Environment.Sanitize", StageEnvironmentTests::Flags)

bool FStageEnvironmentSanitizeTest::RunTest(const FString& Parameters)
{
	using StageEnvironment::Sanitize;
	const FStageEnvironmentSettings Defaults;
	TestTrue(TEXT("Defaults are already sanitized"), Sanitize(Defaults).Equals(Defaults));

	FStageEnvironmentSettings Settings;
	Settings.bSkyEnabled = true;
	TestTrue(TEXT("Sky switch is kept"), Sanitize(Settings).bSkyEnabled);

	// Time and sun direction wrap; the result is always in [0, period).
	const auto Time = [&Settings](float Hours) { FStageEnvironmentSettings Copy = Settings; Copy.TimeOfDay = Hours; return Sanitize(Copy).TimeOfDay; };
	TestEqual(TEXT("25 h wraps to 01:00"), Time(25.f), 1.f, 1.e-4f);
	TestEqual(TEXT("-1 h wraps to 23:00"), Time(-1.f), 23.f, 1.e-4f);
	TestEqual(TEXT("24 h is midnight"), Time(24.f), 0.f, 1.e-4f);
	TestEqual(TEXT("72.5 h wraps to 00:30"), Time(72.5f), 0.5f, 1.e-4f);
	TestTrue(TEXT("A tiny negative time stays below 24"), Time(-1.e-7f) < 24.f);

	const auto Azimuth = [&Settings](float Degrees) { FStageEnvironmentSettings Copy = Settings; Copy.SunAzimuth = Degrees; return Sanitize(Copy).SunAzimuth; };
	TestEqual(TEXT("370 deg wraps to 10"), Azimuth(370.f), 10.f, 1.e-3f);
	TestEqual(TEXT("-90 deg wraps to 270"), Azimuth(-90.f), 270.f, 1.e-3f);
	TestEqual(TEXT("360 deg is 0"), Azimuth(360.f), 0.f, 1.e-3f);

	// Sun height and the weather values clamp.
	const auto Elevation = [&Settings](float Degrees) { FStageEnvironmentSettings Copy = Settings; Copy.SunElevation = Degrees; return Sanitize(Copy).SunElevation; };
	TestEqual(TEXT("Sun height below the minimum clamps up"), Elevation(0.f), StageEnvironment::MinSunElevation);
	TestEqual(TEXT("Sun height above 90 clamps down"), Elevation(120.f), StageEnvironment::MaxSunElevation);

	FStageEnvironmentSettings OutOfRange;
	OutOfRange.CloudCoverage = -0.5f;
	OutOfRange.Rain = 3.f;
	OutOfRange.Snow = -1.f;
	OutOfRange.Fog = 1.5f;
	const FStageEnvironmentSettings Clamped = Sanitize(OutOfRange);
	TestEqual(TEXT("Clouds clamp to 0"), Clamped.CloudCoverage, 0.f);
	TestEqual(TEXT("Rain clamps to 1"), Clamped.Rain, 1.f);
	TestEqual(TEXT("Snow clamps to 0"), Clamped.Snow, 0.f);
	TestEqual(TEXT("Fog clamps to 1"), Clamped.Fog, 1.f);

	// Non-finite input never reaches the sky: each value falls back to its default.
	constexpr float NaN = std::numeric_limits<float>::quiet_NaN();
	constexpr float Infinity = std::numeric_limits<float>::infinity();
	FStageEnvironmentSettings Broken;
	Broken.TimeOfDay = NaN;
	Broken.SunElevation = Infinity;
	Broken.SunAzimuth = -Infinity;
	Broken.CloudCoverage = NaN;
	Broken.Rain = NaN;
	Broken.Snow = Infinity;
	Broken.Fog = NaN;
	TestTrue(TEXT("NaN and infinity fall back to the defaults"), Sanitize(Broken).Equals(Defaults));

	// Sanitize is idempotent.
	TestTrue(TEXT("Sanitizing twice changes nothing"), Sanitize(Sanitize(OutOfRange)).Equals(Sanitize(OutOfRange)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageEnvironmentPresetTest, "StageCraft.Environment.Presets", StageEnvironmentTests::Flags)

bool FStageEnvironmentPresetTest::RunTest(const FString& Parameters)
{
	using namespace StageEnvironment;

	FStageEnvironmentSettings Base;
	Base.bSkyEnabled = true;
	Base.TimeOfDay = 18.25f;
	Base.SunElevation = 30.f;
	Base.SunAzimuth = 120.f;

	const UEnum* Enum = StaticEnum<EStageWeatherPreset>();
	for (int32 Index = 0; Index < int32(EStageWeatherPreset::Custom); ++Index)
	{
		const EStageWeatherPreset Preset = static_cast<EStageWeatherPreset>(Index);
		const FString Name = Enum->GetNameStringByValue(Index);
		const FStageEnvironmentSettings Applied = ApplyWeatherPreset(Base, Preset);
		TestEqual(*FString::Printf(TEXT("%s is recognised after applying it"), *Name), FindWeatherPreset(Applied), Preset);
		TestTrue(*FString::Printf(TEXT("%s keeps time, sun path and the sky switch"), *Name), Applied.bSkyEnabled == Base.bSkyEnabled
			&& Applied.TimeOfDay == Base.TimeOfDay && Applied.SunElevation == Base.SunElevation && Applied.SunAzimuth == Base.SunAzimuth);
		TestTrue(*FString::Printf(TEXT("%s values are inside their ranges"), *Name), Sanitize(Applied).Equals(Applied));
	}

	TestTrue(TEXT("Custom changes nothing"), ApplyWeatherPreset(Base, EStageWeatherPreset::Custom).Equals(Base));

	FStageEnvironmentSettings Odd = ApplyWeatherPreset(Base, EStageWeatherPreset::Rain);
	Odd.Rain = 0.42f;
	TestEqual(TEXT("Edited weather is Custom"), FindWeatherPreset(Odd), EStageWeatherPreset::Custom);

	// Precipitation comes with heavy cloud; a clear sky has none.
	TestTrue(TEXT("Rain preset is overcast and raining"), ApplyWeatherPreset(Base, EStageWeatherPreset::Rain).Rain > 0.f
		&& ApplyWeatherPreset(Base, EStageWeatherPreset::Rain).CloudCoverage > 0.6f);
	TestTrue(TEXT("Snow preset snows and does not rain"), ApplyWeatherPreset(Base, EStageWeatherPreset::Snow).Snow > 0.f
		&& ApplyWeatherPreset(Base, EStageWeatherPreset::Snow).Rain == 0.f);
	TestTrue(TEXT("Clear preset has no precipitation"), ApplyWeatherPreset(Base, EStageWeatherPreset::Clear).Rain == 0.f
		&& ApplyWeatherPreset(Base, EStageWeatherPreset::Clear).Snow == 0.f);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
