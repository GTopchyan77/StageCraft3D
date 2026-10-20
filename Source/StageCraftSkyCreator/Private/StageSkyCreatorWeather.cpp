// Copyright Epic Games, Inc. All Rights Reserved.

#include "StageSkyCreatorWeather.h"

namespace StageSkyCreator
{
	FSkyCreatorWeatherSettings MakeWeather(const FSkyCreatorWeatherSettings& Base, const FStageEnvironmentSettings& Settings)
	{
		constexpr float OvercastFrom = 0.6f;

		FSkyCreatorWeatherSettings Weather = Base;

		const float Coverage = Settings.CloudCoverage;
		FSkyCreatorVolumetricCloudSettings& Clouds = Weather.VolumetricCloudSettings;
		Clouds.CumulusCoverage = Coverage;
		Clouds.StratocumulusCoverage = Coverage;
		Clouds.StratusCoverage = FMath::GetMappedRangeValueClamped(FVector2f(OvercastFrom, 1.f), FVector2f(0.f, 1.f), Coverage);
		Clouds.CumulonimbusCoverage = 0.f;

		FSkyCreatorBackgroundCloudSettings& Background = Weather.BackgroundCloudSettings;
		Background.BackgroundCloudsLayerA = Coverage;
		Background.BackgroundCloudsLayerB = Coverage;
		Background.BackgroundCloudsLayerC = Coverage;

		Weather.WeatherFXSettings.RainAmount = Settings.Rain;
		Weather.WeatherFXSettings.SnowAmount = Settings.Snow;
		Weather.MaterialFXSettings.WetnessAmount = Settings.Rain;

		Weather.ExponentialHeightFogSettings.FogDensity = MaxFogDensity * FMath::Square(Settings.Fog);
		return Weather;
	}
}
