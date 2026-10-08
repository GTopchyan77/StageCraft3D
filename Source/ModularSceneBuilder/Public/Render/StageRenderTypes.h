// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "StageRenderTypes.generated.h"

/** Output image size presets (Docs/ADR/0004-selection-scenes-and-rendering.md §5). */
UENUM(BlueprintType)
enum class EStageRenderResolution : uint8
{
	/** 3840 x 2160. */
	UltraHD4K		UMETA(DisplayName = "4K Ultra HD"),
	/** 1920 x 1080. */
	FullHD			UMETA(DisplayName = "1080p Full HD"),
	/** 2160 x 2160, for social posts and presentation slides. */
	Square			UMETA(DisplayName = "Square 1:1"),
};

UENUM(BlueprintType)
enum class EStageRenderAntiAliasing : uint8
{
	/** No anti-aliasing: hard edges, fastest. */
	Off,
	/** Single-frame FXAA. */
	FXAA			UMETA(DisplayName = "FXAA"),
	/** The project's temporal method (TSR / TAA), converged over several frames. */
	Temporal,
	/** Temporal, rendered above the output size (up to one 4K frame of pixels) and filtered down: the cleanest edges and finest detail. */
	Supersampled,
};

UENUM(BlueprintType)
enum class EStageRenderPostProcess : uint8
{
	/** Neutral technical image: no vignette, film grain, chromatic aberration, lens flares or bloom. */
	Clean,
	/** The level's post-processing, exactly as in the viewport. */
	Standard,
	/** Standard, with higher-quality global illumination, reflections and ambient occlusion and a longer warm-up. */
	Cinematic,
};

/** What a render produces. Persisted per user in UStageCraftUserSettings. */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageRenderSettings
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StageCraft|Render")
	EStageRenderResolution Resolution = EStageRenderResolution::FullHD;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StageCraft|Render")
	EStageRenderAntiAliasing AntiAliasing = EStageRenderAntiAliasing::Temporal;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StageCraft|Render")
	EStageRenderPostProcess PostProcess = EStageRenderPostProcess::Standard;

	/** Stamps a footer with the scene name, date, resolution and item count into the image. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StageCraft|Render")
	bool bWatermark = true;

	bool operator==(const FStageRenderSettings& Other) const = default;
};

/** Where a render job is. Idle is the only state that accepts a new render. */
UENUM(BlueprintType)
enum class EStageRenderState : uint8
{
	Idle,
	/** The GPU is rendering the warm-up frames (temporal AA, exposure and lighting converge). */
	Capturing,
	/** Waiting for the finished image to come back from the GPU. */
	Reading,
	/** Filtering, compressing and writing the PNG on a background thread. */
	Encoding,
};

/** One finished (or failed) render. */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageRenderResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Render")
	bool bSucceeded = false;

	/** Absolute path of the written PNG; empty on failure. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Render")
	FString FilePath;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Render")
	FIntPoint Size = FIntPoint::ZeroValue;

	/** From the request to the file on disk. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Render")
	double Seconds = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Render")
	FText Message;
};

/**
 * Pure rules of the render pipeline, shared by the subsystem, the panel and the tests (StageCraft.Render.Rules).
 * No world, no rendering: sizes, frame counts, file names and the watermark text.
 */
namespace StageRender
{
	/** Pixel size of the delivered image. */
	MODULARSCENEBUILDER_API FIntPoint GetOutputSize(EStageRenderResolution Resolution);

	/**
	 * Render-size multiplier for supersampling: 2x where the internal image stays within MaxSupersampledPixels, less for
	 * larger outputs (Square renders at 1.33x), never below 1, so a 4K output is already at the budget and renders at 1x.
	 * Other anti-aliasing modes render at 1x.
	 */
	MODULARSCENEBUILDER_API double GetSupersampleFactor(const FStageRenderSettings& Settings);

	/**
	 * Upper bound for the internal render of a supersampled image: one 4K frame. Measured on an 8 GB RTX 3060 Ti (STATE.md #26):
	 * an 8.3 MP Cinematic capture ran at ~28 ms per frame, a 20 MP one ran out of video memory and dropped the app to ~1 fps.
	 */
	inline constexpr double MaxSupersampledPixels = 3840.0 * 2160.0;

	/** Size the GPU renders at: the output size times the supersample factor, rounded to even pixels. */
	MODULARSCENEBUILDER_API FIntPoint GetInternalSize(const FStageRenderSettings& Settings);

	/**
	 * Frames rendered before the image is taken, so temporal anti-aliasing, auto exposure and Lumen settle:
	 * 4 without temporal AA, 16 with it, doubled for Cinematic.
	 */
	MODULARSCENEBUILDER_API int32 GetWarmupFrames(const FStageRenderSettings& Settings);

	/** Only letters, digits, '-' and '_' survive; anything else becomes '_'. Empty input gives "Untitled". */
	MODULARSCENEBUILDER_API FString SanitizeFileComponent(const FString& Text);

	/** "StageCraft_<Scene>_<yyyyMMdd-HHmmss>_<W>x<H>.png" in local time. */
	MODULARSCENEBUILDER_API FString MakeFileName(const FString& SceneName, const FDateTime& LocalTime, const FIntPoint& Size);

	/** Footer text: "StageCraft 3D  ·  <Scene>  ·  <date time>  ·  <W>×<H>  ·  <N> items". */
	MODULARSCENEBUILDER_API FString MakeWatermarkText(const FString& SceneName, const FDateTime& LocalTime, const FIntPoint& Size, int32 ItemCount);

	/** Values outside the enums (hand-edited ini) fall back to the defaults. */
	MODULARSCENEBUILDER_API FStageRenderSettings Sanitize(const FStageRenderSettings& Settings);
}
