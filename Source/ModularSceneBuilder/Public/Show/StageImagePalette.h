// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** One dominant colour of an image (StageImagePalette::Extract). */
struct FStageImageSwatch
{
	/** The colour as seen (sRGB, gamma-encoded, 0..1). */
	FLinearColor DisplayColor = FLinearColor::White;
	/** The same colour scaled to full brightness for a light (linear RGB, brightest channel 1): the hue and saturation to send. */
	FLinearColor LightColor = FLinearColor::White;
	/** Relative luminance of DisplayColor (0..1, Rec. 709 on linear values). */
	float Luminance = 0.f;
	/** Share of the image's pixels (0..1) this colour stands for. */
	float Share = 0.f;
};

/**
 * Dominant colours of a photo, for a colour look on fixtures (STATE.md #86). Pure functions with no engine state: deterministic for
 * the same pixels (Rules.md §54), any thread.
 */
namespace StageImagePalette
{
	/** Longest side the image is reduced to before clustering, so the cost is fixed whatever the photo's size. */
	constexpr int32 MaxSide = 128;

	/** Most and fewest colours Extract returns. */
	constexpr int32 MinColors = 1;
	constexpr int32 MaxColors = 8;

	/**
	 * Up to Count dominant colours of BGRA8 pixels (k-means in sRGB, seeded from luminance quantiles, fixed iterations), largest share
	 * first. Colours under 2 % of the image are dropped. Empty for an empty or invalid image.
	 */
	MODULARSCENEBUILDER_API TArray<FStageImageSwatch> Extract(TConstArrayView<uint8> BGRA, int32 Width, int32 Height, int32 Count);

	/**
	 * Dimmer (0..1) for a swatch in a look whose brightest swatch has BrightestLuminance: the dimmest colour still reads (Floor),
	 * the brightest is full.
	 */
	MODULARSCENEBUILDER_API float DimmerFor(const FStageImageSwatch& Swatch, float BrightestLuminance, float Floor = 0.25f);
}
