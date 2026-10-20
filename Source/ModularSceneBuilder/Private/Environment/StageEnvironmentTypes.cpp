// Copyright Epic Games, Inc. All Rights Reserved.

#include "Environment/StageEnvironmentTypes.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageEnvironmentTypes)

namespace StageEnvironment
{
	namespace
	{
		constexpr float Tolerance = 1.e-3f;

		/** Weather values of one preset. */
		struct FWeather
		{
			float CloudCoverage;
			float Rain;
			float Snow;
			float Fog;
		};

		/** Indexed by EStageWeatherPreset (Custom excluded). Rain and snow come with the cloud cover they fall from. */
		constexpr FWeather Presets[] =
		{
			/* Clear  */ { 0.05f, 0.f, 0.f, 0.05f },
			/* Cloudy */ { 0.6f, 0.f, 0.f, 0.15f },
			/* Rain   */ { 0.95f, 0.8f, 0.f, 0.3f },
			/* Snow   */ { 0.9f, 0.f, 0.7f, 0.35f },
			/* Fog    */ { 0.5f, 0.f, 0.f, 0.85f },
		};
		static_assert(UE_ARRAY_COUNT(Presets) == int32(EStageWeatherPreset::Custom), "One entry per preset, in enum order.");

		float Finite(float Value, float Default)
		{
			return FMath::IsFinite(Value) ? Value : Default;
		}

		float Wrap(float Value, float Period)
		{
			const float Wrapped = FMath::Fmod(Value, Period);
			// Fmod keeps the sign; a value that lands on Period after rounding becomes 0, so the result is [0, Period).
			const float Positive = Wrapped < 0.f ? Wrapped + Period : Wrapped;
			return Positive >= Period ? 0.f : Positive;
		}

		float Unit(float Value, float Default)
		{
			return FMath::Clamp(Finite(Value, Default), 0.f, 1.f);
		}
	}

	FStageEnvironmentSettings Sanitize(const FStageEnvironmentSettings& Settings)
	{
		const FStageEnvironmentSettings Defaults;
		FStageEnvironmentSettings Out;
		Out.bSkyEnabled = Settings.bSkyEnabled;
		Out.TimeOfDay = Wrap(Finite(Settings.TimeOfDay, Defaults.TimeOfDay), 24.f);
		Out.SunElevation = FMath::Clamp(Finite(Settings.SunElevation, Defaults.SunElevation), MinSunElevation, MaxSunElevation);
		Out.SunAzimuth = Wrap(Finite(Settings.SunAzimuth, Defaults.SunAzimuth), 360.f);
		Out.CloudCoverage = Unit(Settings.CloudCoverage, Defaults.CloudCoverage);
		Out.Rain = Unit(Settings.Rain, Defaults.Rain);
		Out.Snow = Unit(Settings.Snow, Defaults.Snow);
		Out.Fog = Unit(Settings.Fog, Defaults.Fog);
		return Out;
	}

	FStageEnvironmentSettings ApplyWeatherPreset(const FStageEnvironmentSettings& Settings, EStageWeatherPreset Preset)
	{
		if (Preset == EStageWeatherPreset::Custom || int32(Preset) >= UE_ARRAY_COUNT(Presets))
		{
			return Settings;
		}
		const FWeather& Weather = Presets[int32(Preset)];
		FStageEnvironmentSettings Out = Settings;
		Out.CloudCoverage = Weather.CloudCoverage;
		Out.Rain = Weather.Rain;
		Out.Snow = Weather.Snow;
		Out.Fog = Weather.Fog;
		return Out;
	}

	EStageWeatherPreset FindWeatherPreset(const FStageEnvironmentSettings& Settings)
	{
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(Presets); ++Index)
		{
			const FWeather& Weather = Presets[Index];
			if (FMath::IsNearlyEqual(Settings.CloudCoverage, Weather.CloudCoverage, Tolerance)
				&& FMath::IsNearlyEqual(Settings.Rain, Weather.Rain, Tolerance)
				&& FMath::IsNearlyEqual(Settings.Snow, Weather.Snow, Tolerance)
				&& FMath::IsNearlyEqual(Settings.Fog, Weather.Fog, Tolerance))
			{
				return static_cast<EStageWeatherPreset>(Index);
			}
		}
		return EStageWeatherPreset::Custom;
	}
}

bool FStageEnvironmentSettings::Equals(const FStageEnvironmentSettings& Other) const
{
	constexpr float Tolerance = 1.e-4f;
	return bSkyEnabled == Other.bSkyEnabled
		&& FMath::IsNearlyEqual(TimeOfDay, Other.TimeOfDay, Tolerance)
		&& FMath::IsNearlyEqual(SunElevation, Other.SunElevation, Tolerance)
		&& FMath::IsNearlyEqual(SunAzimuth, Other.SunAzimuth, Tolerance)
		&& FMath::IsNearlyEqual(CloudCoverage, Other.CloudCoverage, Tolerance)
		&& FMath::IsNearlyEqual(Rain, Other.Rain, Tolerance)
		&& FMath::IsNearlyEqual(Snow, Other.Snow, Tolerance)
		&& FMath::IsNearlyEqual(Fog, Other.Fog, Tolerance);
}
