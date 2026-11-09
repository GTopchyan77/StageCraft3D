// Copyright Epic Games, Inc. All Rights Reserved.

#include "Fixtures/StageGdtfUnits.h"

namespace StageGdtfUnits
{
	FLinearColor ChromaticityToLinear(const FStageGdtfColorCIE& Color)
	{
		if (Color.Y <= 0.0)
		{
			return FLinearColor::White;
		}
		// xyY at Y = 1 to XYZ, then XYZ to linear sRGB / Rec. 709 primaries (D65 white).
		const double X = Color.X / Color.Y;
		const double Z = (1.0 - Color.X - Color.Y) / Color.Y;
		const double R = 3.2404542 * X - 1.5371385 - 0.4985314 * Z;
		const double G = -0.9692660 * X + 1.8760108 + 0.0415560 * Z;
		const double B = 0.0556434 * X - 0.2040259 + 1.0572252 * Z;
		FLinearColor Linear(float(FMath::Max(R, 0.0)), float(FMath::Max(G, 0.0)), float(FMath::Max(B, 0.0)), 1.f);
		const float Peak = Linear.GetMax();
		return Peak > UE_SMALL_NUMBER ? FLinearColor(Linear.R / Peak, Linear.G / Peak, Linear.B / Peak, 1.f) : FLinearColor::White;
	}

	FVector PositionToUnrealCm(const double (&Matrix)[16])
	{
		return FVector(Matrix[3] * 100.0, -Matrix[7] * 100.0, Matrix[11] * 100.0);
	}

	FVector SizeToUnrealCm(const FVector& SizeM)
	{
		return SizeM * 100.0;
	}
}
