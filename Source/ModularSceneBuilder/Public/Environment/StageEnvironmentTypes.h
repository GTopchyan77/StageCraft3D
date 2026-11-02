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
 * What lights the stage (Docs/ADR/0009-outdoor-environment.md, amended by ADR 0024 §4). Exactly one at a time.
 * Saved by name in scene files (format v5): never reorder; append new sources.
 */
UENUM(BlueprintType)
enum class EStageLightingSource : uint8
{
	/** The level's own lights, sky light and fog, untouched: the indoor stage as the level was built. */
	Level,
	/** The sky backend's sun, sky, clouds, precipitation and fog replace the level's. */
	OutdoorSky,
	/** StageCraft's studio rig: a soft HDRI sky light, a key light and a light-grey backdrop haze replace the level's. */
	Studio
};

/** How a change of the environment reaches the screen. The stored settings change at once either way. */
UENUM(BlueprintType)
enum class EStageEnvironmentTransition : uint8
{
	/** Shown at once: scene load, undo and redo, scripts. */
	Instant,
	/** Eased over the environment's blend time: atmosphere presets picked by the user (ADR 0024 §5). */
	Blend
};

/**
 * The camera look of the live view (ADR 0024 §3): exposure, bloom, ambient occlusion and grading, plus the studio rig's key
 * light. The defaults are the engine's own post-process defaults, so a stage that never set a look renders as before.
 */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageEnvironmentLook
{
	GENERATED_BODY()

	/** Exposure offset in EV, [-4, 4]. Positive brightens. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Look", meta = (UIMin = "-4", UIMax = "4"))
	float ExposureCompensation = 0.f;

	/** Glow around bright areas, [0, 4]. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Look", meta = (UIMin = "0", UIMax = "4"))
	float BloomIntensity = 0.675f;

	/** Screen-space ambient occlusion on top of Lumen, [0, 1]. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Look", meta = (UIMin = "0", UIMax = "1"))
	float AmbientOcclusionIntensity = 0.5f;

	/** White balance in kelvin, [2000, 15000]. Lower renders cooler (bluer), higher warmer. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Look", meta = (UIMin = "2000", UIMax = "15000"))
	float WhiteBalanceTemp = 6500.f;

	/** Colour saturation, [0, 2]: 0 grey, 1 unchanged. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Look", meta = (UIMin = "0", UIMax = "2"))
	float Saturation = 1.f;

	/** Contrast, [0.5, 1.5]: 1 unchanged. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Look", meta = (UIMin = "0.5", UIMax = "1.5"))
	float Contrast = 1.f;

	/** Darkening of the image corners, [0, 1]. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Look", meta = (UIMin = "0", UIMax = "1"))
	float VignetteIntensity = 0.4f;

	/** The studio rig's key light in lux, [0, 20]. Used only with EStageLightingSource::Studio. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Look", meta = (UIMin = "0", UIMax = "20"))
	float StudioKeyIntensity = 3.f;

	/** The studio rig's key light colour (opaque). Used only with EStageLightingSource::Studio. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Look")
	FLinearColor StudioKeyColor = FLinearColor::White;

	/** Same values, with a tolerance far below what a slider can express. */
	bool Equals(const FStageEnvironmentLook& Other) const;
};

/**
 * The environment of a stage (Docs/ADR/0009-outdoor-environment.md, ADR 0024): what lights it, time of day, sun path,
 * weather and the camera look.
 *
 * Every value is normalized and backend-neutral; a sky backend (UStageEnvironmentBackend) maps the outdoor values onto its
 * own sky, clouds, particles and fog. Values outside the ranges below are clamped or wrapped by StageEnvironment::Sanitize,
 * which UStageEnvironmentSubsystem::SetSettings always applies.
 */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageEnvironmentSettings
{
	GENERATED_BODY()

	/** What lights the stage. The time, sun and weather values only show with OutdoorSky; they are kept with the others. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Environment")
	EStageLightingSource Lighting = EStageLightingSource::Level;

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

	/** The camera look, whatever lights the stage. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Environment")
	FStageEnvironmentLook Look;

	bool IsOutdoorSky() const { return Lighting == EStageLightingSource::OutdoorSky; }

	/** Same values, with a tolerance far below what a slider can express. */
	bool Equals(const FStageEnvironmentSettings& Other) const;
};

namespace StageEnvironment
{
	constexpr float MinSunElevation = 5.f;
	constexpr float MaxSunElevation = 90.f;

	/** How deep the picture dips (EV) at the midpoint of a blend that switches the lighting source. */
	constexpr float SourceSwitchDipEV = 6.f;

	/**
	 * Settings with every value inside its range: TimeOfDay and SunAzimuth wrapped, the others clamped, an unknown lighting
	 * source replaced by Level, the key colour made opaque. A NaN or infinite value is replaced by the default, so bad input
	 * from a script or a file can never reach the sky.
	 */
	MODULARSCENEBUILDER_API FStageEnvironmentSettings Sanitize(const FStageEnvironmentSettings& Settings);

	/**
	 * Settings with the weather values (clouds, rain, snow, fog) of Preset; time, sun path, lighting and look are kept.
	 * Custom changes nothing.
	 */
	MODULARSCENEBUILDER_API FStageEnvironmentSettings ApplyWeatherPreset(const FStageEnvironmentSettings& Settings, EStageWeatherPreset Preset);

	/** The preset whose weather values Settings has, or Custom. */
	MODULARSCENEBUILDER_API EStageWeatherPreset FindWeatherPreset(const FStageEnvironmentSettings& Settings);

	/**
	 * What to show at Alpha in [0, 1] of a blend from From to To (ADR 0024 §5). Pure; both inputs sanitized.
	 *  - Numbers ease in and out. The time of day runs forward when To is less than 12 h ahead (the sun visibly sets),
	 *    otherwise backward; the sun azimuth takes the shorter way round. The key colour blends linearly.
	 *  - A change of lighting source cannot blend (lights appear and disappear): From's source is shown up to the
	 *    midpoint, To's after it, and the exposure dips by SourceSwitchDipEV around the switch so it happens in the dark.
	 *  - Alpha 0 gives From, 1 gives To exactly.
	 */
	MODULARSCENEBUILDER_API FStageEnvironmentSettings BlendForDisplay(const FStageEnvironmentSettings& From, const FStageEnvironmentSettings& To, float Alpha);
}
