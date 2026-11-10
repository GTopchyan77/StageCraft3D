// Copyright Epic Games, Inc. All Rights Reserved.

#include "Show/StageImagePalette.h"

namespace StageImagePalette
{
	namespace
	{
		constexpr int32 Iterations = 12;
		constexpr float MinShare = 0.02f;

		/** Box-filtered BGRA8 → sRGB triples (0..255), at most MaxSide on the longest side. */
		TArray<FVector3f> ReducePixels(TConstArrayView<uint8> BGRA, int32 Width, int32 Height)
		{
			const double Scale = FMath::Max(Width, Height) > MaxSide ? double(MaxSide) / double(FMath::Max(Width, Height)) : 1.0;
			const int32 W = FMath::Max(1, FMath::RoundToInt(Width * Scale));
			const int32 H = FMath::Max(1, FMath::RoundToInt(Height * Scale));
			TArray<FVector3f> Pixels;
			Pixels.Reserve(W * H);
			for (int32 Y = 0; Y < H; ++Y)
			{
				const int32 Y0 = int32(double(Y) * Height / H);
				const int32 Y1 = FMath::Clamp(int32(double(Y + 1) * Height / H), Y0 + 1, Height);
				for (int32 X = 0; X < W; ++X)
				{
					const int32 X0 = int32(double(X) * Width / W);
					const int32 X1 = FMath::Clamp(int32(double(X + 1) * Width / W), X0 + 1, Width);
					FVector3f Sum = FVector3f::ZeroVector;
					for (int32 SY = Y0; SY < Y1; ++SY)
					{
						for (int32 SX = X0; SX < X1; ++SX)
						{
							const int32 Index = (SY * Width + SX) * 4;
							Sum += FVector3f(BGRA[Index + 2], BGRA[Index + 1], BGRA[Index]);
						}
					}
					Pixels.Add(Sum / float((Y1 - Y0) * (X1 - X0)));
				}
			}
			return Pixels;
		}

		float Luma(const FVector3f& Rgb)
		{
			return 0.299f * Rgb.X + 0.587f * Rgb.Y + 0.114f * Rgb.Z;
		}
	}

	TArray<FStageImageSwatch> Extract(TConstArrayView<uint8> BGRA, int32 Width, int32 Height, int32 Count)
	{
		TArray<FStageImageSwatch> Result;
		if (Width <= 0 || Height <= 0 || BGRA.Num() != Width * Height * 4)
		{
			return Result;
		}
		const TArray<FVector3f> Pixels = ReducePixels(BGRA, Width, Height);
		const int32 K = FMath::Clamp(Count, MinColors, FMath::Min(MaxColors, Pixels.Num()));

		// Seeds at luminance quantiles: deterministic, and spread from the shadows to the highlights.
		TArray<int32> ByLuma;
		ByLuma.Reserve(Pixels.Num());
		for (int32 Index = 0; Index < Pixels.Num(); ++Index)
		{
			ByLuma.Add(Index);
		}
		ByLuma.StableSort([&Pixels](int32 A, int32 B) { return Luma(Pixels[A]) < Luma(Pixels[B]); });
		TArray<FVector3f> Centres;
		for (int32 Cluster = 0; Cluster < K; ++Cluster)
		{
			Centres.Add(Pixels[ByLuma[FMath::Min(Pixels.Num() - 1, int32((Cluster + 0.5) * Pixels.Num() / K))]]);
		}

		TArray<int32> Members;
		for (int32 Iteration = 0; Iteration < Iterations; ++Iteration)
		{
			TArray<FVector3f> Sums;
			Sums.Init(FVector3f::ZeroVector, K);
			Members.Init(0, K);
			for (const FVector3f& Pixel : Pixels)
			{
				int32 Best = 0;
				float BestDistance = TNumericLimits<float>::Max();
				for (int32 Cluster = 0; Cluster < K; ++Cluster)
				{
					const float Distance = FVector3f::DistSquared(Pixel, Centres[Cluster]);
					if (Distance < BestDistance)
					{
						BestDistance = Distance;
						Best = Cluster;
					}
				}
				Sums[Best] += Pixel;
				++Members[Best];
			}
			for (int32 Cluster = 0; Cluster < K; ++Cluster)
			{
				// An empty cluster keeps its centre; it is dropped below for its zero share.
				if (Members[Cluster] > 0)
				{
					Centres[Cluster] = Sums[Cluster] / float(Members[Cluster]);
				}
			}
		}

		for (int32 Cluster = 0; Cluster < K; ++Cluster)
		{
			const float Share = float(Members[Cluster]) / float(Pixels.Num());
			if (Share < MinShare)
			{
				continue;
			}
			FStageImageSwatch Swatch;
			Swatch.Share = Share;
			const FVector3f Display = Centres[Cluster] / 255.f;
			Swatch.DisplayColor = FLinearColor(Display.X, Display.Y, Display.Z, 1.f);
			const FLinearColor Linear = FLinearColor::FromSRGBColor(FColor(
				uint8(FMath::RoundToInt(FMath::Clamp(Centres[Cluster].X, 0.f, 255.f))),
				uint8(FMath::RoundToInt(FMath::Clamp(Centres[Cluster].Y, 0.f, 255.f))),
				uint8(FMath::RoundToInt(FMath::Clamp(Centres[Cluster].Z, 0.f, 255.f)))));
			Swatch.Luminance = 0.2126f * Linear.R + 0.7152f * Linear.G + 0.0722f * Linear.B;
			const float Peak = FMath::Max3(Linear.R, Linear.G, Linear.B);
			// Black has no hue: it becomes white light at the lowest dimmer rather than an undefined colour.
			Swatch.LightColor = Peak > UE_KINDA_SMALL_NUMBER ? FLinearColor(Linear.R / Peak, Linear.G / Peak, Linear.B / Peak, 1.f) : FLinearColor::White;
			Result.Add(Swatch);
		}
		Result.StableSort([](const FStageImageSwatch& A, const FStageImageSwatch& B) { return A.Share > B.Share; });
		return Result;
	}

	float DimmerFor(const FStageImageSwatch& Swatch, float BrightestLuminance, float Floor)
	{
		const float SafeFloor = FMath::Clamp(Floor, 0.f, 1.f);
		if (BrightestLuminance <= UE_KINDA_SMALL_NUMBER)
		{
			return 1.f;
		}
		const float Relative = FMath::Clamp(Swatch.Luminance / BrightestLuminance, 0.f, 1.f);
		return SafeFloor + (1.f - SafeFloor) * Relative;
	}
}
