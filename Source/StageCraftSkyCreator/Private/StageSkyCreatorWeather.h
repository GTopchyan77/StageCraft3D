// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Environment/StageEnvironmentTypes.h"

// Sky Creator's header converts double literals to float in default arguments (MSVC C4305), which this module would
// report as its own warnings. The plugin is third-party and stays unedited, so the warning is silenced for its header only.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4305)
#endif
#include "SkyCreatorWeatherSettings.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

/** The mapping from StageCraft's normalized environment onto Sky Creator (Docs/ADR/0009-outdoor-environment.md §3.2). */
namespace StageSkyCreator
{
	/** Sunrise and sunset of the simple sun path, in hours. The sun's height and compass direction come from the settings. */
	constexpr float SunriseHour = 6.5f;
	constexpr float SunsetHour = 19.5f;

	/**
	 * Height fog density at Fog = 1. The engine applies density / 1000 per centimetre, so 1.0 dims a stage 30 m away to about
	 * 5 % (optical depth 3): heavy fog with tens of metres of visibility. Fog 0.3 (0.09) is a light mist across a stage,
	 * 0.1 (0.01) a distant haze.
	 */
	constexpr float MaxFogDensity = 1.f;

	/**
	 * Base (the sky actor's own default weather) with clouds, rain, snow and fog taken from Settings; everything else,
	 * cloud lighting, particle looks, wind, is Sky Creator's tuning. Settings must be sanitized.
	 *  - Clouds: cumulus and stratocumulus follow CloudCoverage; a flat stratus layer closes the sky above 60 %, so 1 is overcast.
	 *  - Rain and snow: particle amounts, plus surface wetness from rain.
	 *  - Fog: height fog density grows with Fog squared, so the low half of the slider stays usable for haze.
	 */
	FSkyCreatorWeatherSettings MakeWeather(const FSkyCreatorWeatherSettings& Base, const FStageEnvironmentSettings& Settings);
}
