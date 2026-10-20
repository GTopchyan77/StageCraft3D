// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "StageEnvironmentTypes.generated.h"

/** One-click weather looks in the Environment panel. Custom: the weather values match none of them. */
UENUM(BlueprintType)
enum class EStageWeatherPreset : uint8
{
	Clear,
	Cloudy,
	Rain,
	Snow,
	Fog,
	Custom
};

/**
 * The outdoor environment of a stage (Docs/ADR/0009-outdoor-environment.md): time of day, sun path and weather.
 *
 * Every value is normalized and backend-neutral; a sky backend (UStageEnvironmentBackend) maps it onto its own sky,
 * clouds, particles and fog. Values outside the ranges below are clamped or wrapped by StageEnvironment::Sanitize,
 * which UStageEnvironmentSubsystem::SetSettings always applies.
 */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageEnvironmentSettings
{
	GENERATED_BODY()

	/**
	 * Whether the outdoor sky is on. Off is the indoor stage: the level's own lights, sky light and fog, untouched.
	 * On, the sky backend replaces them with its sun, sky, clouds, precipitation and fog.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Environment")
	bool bSkyEnabled = false;

	/** Local time in hours, [0, 24). Wraps: 25 is 01:00. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Environment", meta = (UIMin = "0", UIMax = "24", Units = "Hours"))
	float TimeOfDay = 14.f;

	/** Height of the sun at noon in degrees above the horizon, [5, 90]: low for winter or high latitudes, 90 straight overhead. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Environment", meta = (UIMin = "5", UIMax = "90", Units = "Degrees"))
	float SunElevation = 55.f;

	/** Compass direction of the sun's path in degrees, [0, 360). Wraps. Turns the light around the stage. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Environment", meta = (UIMin = "0", UIMax = "360", Units = "Degrees"))
	float SunAzimuth = 270.f;

	/** Share of the sky covered by cloud, [0, 1]: 0 clear, 1 overcast. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Environment", meta = (UIMin = "0", UIMax = "1"))
	float CloudCoverage = 0.3f;

	/** Rain intensity, [0, 1]. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Environment", meta = (UIMin = "0", UIMax = "1"))
	float Rain = 0.f;

	/** Snowfall intensity, [0, 1]. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Environment", meta = (UIMin = "0", UIMax = "1"))
	float Snow = 0.f;

	/** Atmospheric fog, [0, 1]: 0 a clear day, 1 a few tens of metres of visibility. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Environment", meta = (UIMin = "0", UIMax = "1"))
	float Fog = 0.1f;

	/** Same values, with a tolerance far below what a slider can express. */
	bool Equals(const FStageEnvironmentSettings& Other) const;
};

namespace StageEnvironment
{
	constexpr float MinSunElevation = 5.f;
	constexpr float MaxSunElevation = 90.f;

	/**
	 * Settings with every value inside its range: TimeOfDay and SunAzimuth wrapped, the others clamped. A NaN or infinite
	 * value is replaced by the default, so bad input from a script or a file can never reach the sky.
	 */
	MODULARSCENEBUILDER_API FStageEnvironmentSettings Sanitize(const FStageEnvironmentSettings& Settings);

	/**
	 * Settings with the weather values (clouds, rain, snow, fog) of Preset; time, sun path and bSkyEnabled are kept.
	 * Custom changes nothing.
	 */
	MODULARSCENEBUILDER_API FStageEnvironmentSettings ApplyWeatherPreset(const FStageEnvironmentSettings& Settings, EStageWeatherPreset Preset);

	/** The preset whose weather values Settings has, or Custom. */
	MODULARSCENEBUILDER_API EStageWeatherPreset FindWeatherPreset(const FStageEnvironmentSettings& Settings);
}
