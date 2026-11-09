// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Fixtures/StageGdtfTypes.h"

/**
 * Every unit and axis conversion between GDTF and StageCraft (Docs/ADR/0033 §5.5). No other code converts.
 *
 * GDTF uses metres, a right-handed frame with Z up, CIE 1931 xyY colours and degrees per second for rotation speeds. StageCraft uses
 * centimetres, Unreal's left-handed Z-up frame (Y mirrored), linear Rec. 709 colours and revolutions per minute.
 */
namespace StageGdtfUnits
{
	/** Linear Rec. 709 colour of a chromaticity, scaled so its brightest channel is 1 (negative channels, outside the gamut, clamp to 0). */
	FLinearColor ChromaticityToLinear(const FStageGdtfColorCIE& Color);

	/** A GDTF Position matrix's translation (fourth column, metres) as an Unreal offset in centimetres. */
	FVector PositionToUnrealCm(const double (&Matrix)[16]);

	/** A model's Length, Width and Height (metres) as Unreal sizes X, Y, Z in centimetres. */
	FVector SizeToUnrealCm(const FVector& SizeM);

	inline float DegreesPerSecondToRpm(float DegreesPerSecond) { return DegreesPerSecond / 6.f; }
}
