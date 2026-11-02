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

	FStageEnvironmentLook SanitizeLook(const FStageEnvironmentLook& Look)
	{
		const FStageEnvironmentLook Defaults;
		FStageEnvironmentLook Out;
		Out.ExposureCompensation = FMath::Clamp(Finite(Look.ExposureCompensation, Defaults.ExposureCompensation), -4.f, 4.f);
		Out.BloomIntensity = FMath::Clamp(Finite(Look.BloomIntensity, Defaults.BloomIntensity), 0.f, 4.f);
		Out.AmbientOcclusionIntensity = Unit(Look.AmbientOcclusionIntensity, Defaults.AmbientOcclusionIntensity);
		Out.WhiteBalanceTemp = FMath::Clamp(Finite(Look.WhiteBalanceTemp, Defaults.WhiteBalanceTemp), 2000.f, 15000.f);
		Out.Saturation = FMath::Clamp(Finite(Look.Saturation, Defaults.Saturation), 0.f, 2.f);
		Out.Contrast = FMath::Clamp(Finite(Look.Contrast, Defaults.Contrast), 0.5f, 1.5f);
		Out.VignetteIntensity = Unit(Look.VignetteIntensity, Defaults.VignetteIntensity);
		Out.StudioKeyIntensity = FMath::Clamp(Finite(Look.StudioKeyIntensity, Defaults.StudioKeyIntensity), 0.f, 20.f);
		Out.StudioKeyColor = FLinearColor(Unit(Look.StudioKeyColor.R, 1.f), Unit(Look.StudioKeyColor.G, 1.f), Unit(Look.StudioKeyColor.B, 1.f), 1.f);
		return Out;
	}

	FStageEnvironmentSettings Sanitize(const FStageEnvironmentSettings& Settings)
	{
		const FStageEnvironmentSettings Defaults;
		FStageEnvironmentSettings Out;
		const uint8 Source = uint8(Settings.Lighting);
		Out.Lighting = Source <= uint8(EStageLightingSource::Studio) ? Settings.Lighting : EStageLightingSource::Level;
		Out.TimeOfDay = Wrap(Finite(Settings.TimeOfDay, Defaults.TimeOfDay), 24.f);
		Out.SunElevation = FMath::Clamp(Finite(Settings.SunElevation, Defaults.SunElevation), MinSunElevation, MaxSunElevation);
		Out.SunAzimuth = Wrap(Finite(Settings.SunAzimuth, Defaults.SunAzimuth), 360.f);
		Out.CloudCoverage = Unit(Settings.CloudCoverage, Defaults.CloudCoverage);
		Out.Rain = Unit(Settings.Rain, Defaults.Rain);
		Out.Snow = Unit(Settings.Snow, Defaults.Snow);
		Out.Fog = Unit(Settings.Fog, Defaults.Fog);
		Out.Look = SanitizeLook(Settings.Look);
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
	return Lighting == Other.Lighting
		&& Look.Equals(Other.Look)
		&& FMath::IsNearlyEqual(TimeOfDay, Other.TimeOfDay, Tolerance)
		&& FMath::IsNearlyEqual(SunElevation, Other.SunElevation, Tolerance)
		&& FMath::IsNearlyEqual(SunAzimuth, Other.SunAzimuth, Tolerance)
		&& FMath::IsNearlyEqual(CloudCoverage, Other.CloudCoverage, Tolerance)
		&& FMath::IsNearlyEqual(Rain, Other.Rain, Tolerance)
		&& FMath::IsNearlyEqual(Snow, Other.Snow, Tolerance)
		&& FMath::IsNearlyEqual(Fog, Other.Fog, Tolerance);
}

bool FStageEnvironmentLook::Equals(const FStageEnvironmentLook& Other) const
{
	constexpr float Tolerance = 1.e-4f;
	return FMath::IsNearlyEqual(ExposureCompensation, Other.ExposureCompensation, Tolerance)
		&& FMath::IsNearlyEqual(BloomIntensity, Other.BloomIntensity, Tolerance)
		&& FMath::IsNearlyEqual(AmbientOcclusionIntensity, Other.AmbientOcclusionIntensity, Tolerance)
		&& FMath::IsNearlyEqual(WhiteBalanceTemp, Other.WhiteBalanceTemp, 1.e-2f)
		&& FMath::IsNearlyEqual(Saturation, Other.Saturation, Tolerance)
		&& FMath::IsNearlyEqual(Contrast, Other.Contrast, Tolerance)
		&& FMath::IsNearlyEqual(VignetteIntensity, Other.VignetteIntensity, Tolerance)
		&& FMath::IsNearlyEqual(StudioKeyIntensity, Other.StudioKeyIntensity, Tolerance)
		&& StudioKeyColor.Equals(Other.StudioKeyColor, Tolerance);
}

namespace StageEnvironment
{
	FStageEnvironmentSettings BlendForDisplay(const FStageEnvironmentSettings& From, const FStageEnvironmentSettings& To, float Alpha)
	{
		const float T = FMath::Clamp(FMath::IsFinite(Alpha) ? Alpha : 1.f, 0.f, 1.f);
		if (T <= 0.f)
		{
			return From;
		}
		if (T >= 1.f)
		{
			return To;
		}
		const float E = FMath::InterpEaseInOut(0.f, 1.f, T, 2.f);
		auto Mix = [E](float A, float B) { return FMath::Lerp(A, B, E); };

		FStageEnvironmentSettings Out = To;

		// Forward through the day when the target is less than half a day ahead, so Daytime -> Sunset sets the sun.
		const float Ahead = FMath::Fmod(To.TimeOfDay - From.TimeOfDay + 24.f, 24.f);
		const float TimeDelta = Ahead <= 12.f ? Ahead : Ahead - 24.f;
		Out.TimeOfDay = FMath::Fmod(From.TimeOfDay + TimeDelta * E + 24.f, 24.f);
		Out.SunAzimuth = FMath::Fmod(From.SunAzimuth + FMath::FindDeltaAngleDegrees(From.SunAzimuth, To.SunAzimuth) * E + 360.f, 360.f);
		Out.SunElevation = Mix(From.SunElevation, To.SunElevation);
		Out.CloudCoverage = Mix(From.CloudCoverage, To.CloudCoverage);
		Out.Rain = Mix(From.Rain, To.Rain);
		Out.Snow = Mix(From.Snow, To.Snow);
		Out.Fog = Mix(From.Fog, To.Fog);

		FStageEnvironmentLook& Look = Out.Look;
		Look.ExposureCompensation = Mix(From.Look.ExposureCompensation, To.Look.ExposureCompensation);
		Look.BloomIntensity = Mix(From.Look.BloomIntensity, To.Look.BloomIntensity);
		Look.AmbientOcclusionIntensity = Mix(From.Look.AmbientOcclusionIntensity, To.Look.AmbientOcclusionIntensity);
		Look.WhiteBalanceTemp = Mix(From.Look.WhiteBalanceTemp, To.Look.WhiteBalanceTemp);
		Look.Saturation = Mix(From.Look.Saturation, To.Look.Saturation);
		Look.Contrast = Mix(From.Look.Contrast, To.Look.Contrast);
		Look.VignetteIntensity = Mix(From.Look.VignetteIntensity, To.Look.VignetteIntensity);
		Look.StudioKeyIntensity = Mix(From.Look.StudioKeyIntensity, To.Look.StudioKeyIntensity);
		Look.StudioKeyColor = FLinearColor::LerpUsingHSV(From.Look.StudioKeyColor, To.Look.StudioKeyColor, E);

		if (From.Lighting != To.Lighting)
		{
			Out.Lighting = T < 0.5f ? From.Lighting : To.Lighting;
			// Darkest exactly at the switch; a raw sine, so the dip itself is smooth at both ends.
			Look.ExposureCompensation -= SourceSwitchDipEV * FMath::Sin(PI * T);
		}
		return Out;
	}
}
