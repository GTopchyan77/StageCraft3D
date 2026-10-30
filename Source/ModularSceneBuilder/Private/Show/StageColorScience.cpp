// Copyright Epic Games, Inc. All Rights Reserved.

#include "Show/StageColorScience.h"

namespace StageColorScience
{
	namespace
	{
		constexpr double Epsilon = 1e-9;

		FLinearColor NormalizeToPeak(const FLinearColor& Color)
		{
			const float Peak = FMath::Max3(Color.R, Color.G, Color.B);
			if (!(Peak > UE_SMALL_NUMBER) || !FMath::IsFinite(Peak))
			{
				return FLinearColor::Black;
			}
			FLinearColor Normalized = Color / Peak;
			Normalized.A = 1.f;
			return Normalized;
		}

		bool IsFiniteColor(const FLinearColor& Color)
		{
			return FMath::IsFinite(Color.R) && FMath::IsFinite(Color.G) && FMath::IsFinite(Color.B);
		}

		/** 3x3 matrix with the first three outputs as columns, inverted; false when they do not span the colour space. */
		bool InvertPrimaries(TConstArrayView<FLinearColor> Outputs, double (&OutInverse)[3][3])
		{
			const double M[3][3] = {
				{ Outputs[0].R, Outputs[1].R, Outputs[2].R },
				{ Outputs[0].G, Outputs[1].G, Outputs[2].G },
				{ Outputs[0].B, Outputs[1].B, Outputs[2].B },
			};
			const double Det = M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1])
				- M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0])
				+ M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
			if (FMath::Abs(Det) < Epsilon || !FMath::IsFinite(Det))
			{
				return false;
			}
			const double InvDet = 1.0 / Det;
			OutInverse[0][0] = (M[1][1] * M[2][2] - M[1][2] * M[2][1]) * InvDet;
			OutInverse[0][1] = (M[0][2] * M[2][1] - M[0][1] * M[2][2]) * InvDet;
			OutInverse[0][2] = (M[0][1] * M[1][2] - M[0][2] * M[1][1]) * InvDet;
			OutInverse[1][0] = (M[1][2] * M[2][0] - M[1][0] * M[2][2]) * InvDet;
			OutInverse[1][1] = (M[0][0] * M[2][2] - M[0][2] * M[2][0]) * InvDet;
			OutInverse[1][2] = (M[0][2] * M[1][0] - M[0][0] * M[1][2]) * InvDet;
			OutInverse[2][0] = (M[1][0] * M[2][1] - M[1][1] * M[2][0]) * InvDet;
			OutInverse[2][1] = (M[0][1] * M[2][0] - M[0][0] * M[2][1]) * InvDet;
			OutInverse[2][2] = (M[0][0] * M[1][1] - M[0][1] * M[1][0]) * InvDet;
			return true;
		}

		void Multiply(const double (&Inverse)[3][3], const FLinearColor& Color, double (&Out)[3])
		{
			for (int32 Row = 0; Row < 3; ++Row)
			{
				Out[Row] = Inverse[Row][0] * Color.R + Inverse[Row][1] * Color.G + Inverse[Row][2] * Color.B;
			}
		}
	}

	double GetLuminance(const FLinearColor& Color)
	{
		return 0.2126 * Color.R + 0.7152 * Color.G + 0.0722 * Color.B;
	}

	FLinearColor KelvinToLinear(double Kelvin)
	{
		const double Clamped = FMath::IsFinite(Kelvin) ? FMath::Clamp(Kelvin, MinKelvin, MaxKelvin) : 6500.0;
		return NormalizeToPeak(FLinearColor::MakeFromColorTemperature(static_cast<float>(Clamped)));
	}

	double ShiftKelvinByMired(double Kelvin, double Mired)
	{
		const double Source = FMath::IsFinite(Kelvin) ? FMath::Clamp(Kelvin, MinKelvin, MaxKelvin) : 6500.0;
		const double Shift = FMath::IsFinite(Mired) ? Mired : 0.0;
		// Mired = 1e6 / K. The shifted reciprocal is clamped, so a strong CTB on a cool source cannot divide by zero.
		const double ShiftedMired = FMath::Clamp(1.0e6 / Source + Shift, 1.0e6 / MaxKelvin, 1.0e6 / MinKelvin);
		return 1.0e6 / ShiftedMired;
	}

	FMix MixAdditive(TConstArrayView<FLinearColor> Outputs, TConstArrayView<float> Levels)
	{
		FLinearColor Sum = FLinearColor::Black;
		FLinearColor Full = FLinearColor::Black;
		for (int32 Index = 0; Index < Outputs.Num(); ++Index)
		{
			if (!IsFiniteColor(Outputs[Index]))
			{
				continue;
			}
			const float Level = Levels.IsValidIndex(Index) && FMath::IsFinite(Levels[Index]) ? FMath::Clamp(Levels[Index], 0.f, 1.f) : 0.f;
			Sum += Outputs[Index] * Level;
			Full += Outputs[Index];
		}

		FMix Mix;
		const double FullLuminance = GetLuminance(Full);
		Mix.Color = NormalizeToPeak(Sum);
		Mix.LuminousFraction = FullLuminance > Epsilon ? FMath::Clamp(GetLuminance(Sum) / FullLuminance, 0.0, 1.0) : 0.0;
		return Mix;
	}

	FMix MixSubtractive(double LampKelvin, const FLinearColor& Filter, double Cto, double FullCtoMired)
	{
		const double CtoAmount = FMath::IsFinite(Cto) ? FMath::Clamp(Cto, 0.0, 1.0) : 0.0;
		const FLinearColor Lamp = KelvinToLinear(LampKelvin);
		const FLinearColor Warmed = KelvinToLinear(ShiftKelvinByMired(LampKelvin, CtoAmount * FullCtoMired));
		const FLinearColor Passed = IsFiniteColor(Filter)
			? FLinearColor(FMath::Clamp(Filter.R, 0.f, 1.f), FMath::Clamp(Filter.G, 0.f, 1.f), FMath::Clamp(Filter.B, 0.f, 1.f))
			: FLinearColor::Black;

		// Both whites have their brightest channel at 1, so the CTO's own loss shows as the warmer white's lower luminance.
		const FLinearColor Out = Warmed * Passed;
		FMix Mix;
		const double LampLuminance = GetLuminance(Lamp);
		Mix.Color = NormalizeToPeak(Out);
		Mix.LuminousFraction = LampLuminance > Epsilon ? FMath::Clamp(GetLuminance(Out) / LampLuminance, 0.0, 1.0) : 0.0;
		return Mix;
	}

	FMix ApplyFilter(const FMix& Mix, const FLinearColor& Filter)
	{
		const FLinearColor Clamped = IsFiniteColor(Filter)
			? FLinearColor(FMath::Clamp(Filter.R, 0.f, 1.f), FMath::Clamp(Filter.G, 0.f, 1.f), FMath::Clamp(Filter.B, 0.f, 1.f))
			: FLinearColor::Black;
		const FLinearColor Passed = Mix.Color * Clamped;
		const double Before = GetLuminance(Mix.Color);

		FMix Result;
		Result.Color = NormalizeToPeak(Passed);
		Result.LuminousFraction = Before > Epsilon ? FMath::Clamp(Mix.LuminousFraction * GetLuminance(Passed) / Before, 0.0, 1.0) : 0.0;
		return Result;
	}

	FMix Combine(TConstArrayView<FMix> Sources)
	{
		FMix Result;
		if (Sources.IsEmpty())
		{
			return Result;
		}
		FLinearColor Sum = FLinearColor::Black;
		double Fraction = 0.0;
		for (const FMix& Source : Sources)
		{
			const double SourceFraction = FMath::IsFinite(Source.LuminousFraction) ? FMath::Clamp(Source.LuminousFraction, 0.0, 1.0) : 0.0;
			const double Luminance = GetLuminance(Source.Color);
			// Each source's colour at its real luminance, so a dim red cell does not tint a bright white bar.
			if (Luminance > Epsilon && IsFiniteColor(Source.Color))
			{
				Sum += Source.Color * static_cast<float>(SourceFraction / Luminance);
			}
			Fraction += SourceFraction;
		}
		Result.Color = NormalizeToPeak(Sum);
		Result.LuminousFraction = Fraction / Sources.Num();
		return Result;
	}

	TArray<float> MatchAdditive(TConstArrayView<FLinearColor> Outputs, const FLinearColor& Target)
	{
		TArray<float> Levels;
		Levels.SetNumZeroed(Outputs.Num());
		const bool bValidTarget = IsFiniteColor(Target) && FMath::Max3(Target.R, Target.G, Target.B) > UE_SMALL_NUMBER;
		if (!bValidTarget || Outputs.IsEmpty())
		{
			return Levels;
		}

		double Inverse[3][3];
		if (Outputs.Num() < 3 || !InvertPrimaries(Outputs, Inverse))
		{
			// No colour mixing to solve: every emitter follows the target's brightness.
			const float Peak = FMath::Clamp(FMath::Max3(Target.R, Target.G, Target.B), 0.f, 1.f);
			for (float& Level : Levels)
			{
				Level = Peak;
			}
			return Levels;
		}

		double Rgb[3];
		Multiply(Inverse, FLinearColor(FMath::Max(Target.R, 0.f), FMath::Max(Target.G, 0.f), FMath::Max(Target.B, 0.f)), Rgb);
		for (double& Level : Rgb)
		{
			Level = FMath::Max(Level, 0.0);
		}

		// Extra emitters (white, amber) replace part of the RGB mix: an amount k of extra j takes k x (j in RGB terms) off the
		// RGB levels. The brightest reproduction is the one whose highest level is lowest before scaling up to 1. That peak is a
		// maximum of affine functions of the amounts, so it is convex, and a ternary search per extra finds its minimum. Two
		// extras (RGBWA) are searched jointly; an emitter outside the RGB gamut cannot be a non-negative RGB mix and stays off.
		struct FExtra
		{
			int32 Index = INDEX_NONE;
			double InRgb[3] = { 0.0, 0.0, 0.0 };
		};
		TArray<FExtra, TInlineAllocator<2>> Extras;
		for (int32 Index = 3; Index < Outputs.Num() && Extras.Num() < 2; ++Index)
		{
			FExtra Extra;
			Extra.Index = Index;
			Multiply(Inverse, Outputs[Index], Extra.InRgb);
			const bool bInGamut = IsFiniteColor(Outputs[Index]) && Extra.InRgb[0] >= -Epsilon && Extra.InRgb[1] >= -Epsilon && Extra.InRgb[2] >= -Epsilon
				&& FMath::Max3(Extra.InRgb[0], Extra.InRgb[1], Extra.InRgb[2]) > Epsilon;
			if (bInGamut)
			{
				Extras.Add(Extra);
			}
		}

		const auto Remaining = [&Rgb, &Extras](double Amount0, double Amount1, int32 Channel)
		{
			double Level = Rgb[Channel];
			if (Extras.Num() > 0) { Level -= Amount0 * Extras[0].InRgb[Channel]; }
			if (Extras.Num() > 1) { Level -= Amount1 * Extras[1].InRgb[Channel]; }
			return Level;
		};
		const auto PeakOf = [&Remaining](double Amount0, double Amount1)
		{
			return FMath::Max(FMath::Max3(Remaining(Amount0, Amount1, 0), Remaining(Amount0, Amount1, 1), Remaining(Amount0, Amount1, 2)),
				FMath::Max(Amount0, Amount1));
		};
		// Most of extra j the RGB levels left after Amount0 of extra 0 can give up.
		const auto MaxAmount = [&Remaining, &Extras](int32 Extra, double Amount0)
		{
			double Max = TNumericLimits<double>::Max();
			for (int32 Channel = 0; Channel < 3; ++Channel)
			{
				if (Extras[Extra].InRgb[Channel] > Epsilon)
				{
					Max = FMath::Min(Max, FMath::Max(Remaining(Extra == 0 ? 0.0 : Amount0, 0.0, Channel), 0.0) / Extras[Extra].InRgb[Channel]);
				}
			}
			return Max == TNumericLimits<double>::Max() ? 0.0 : Max;
		};
		const auto Minimize = [](double Low, double High, TFunctionRef<double(double)> Cost)
		{
			// 60 steps shrink the interval by (2/3)^60, about 3e-11 of a level.
			for (int32 Step = 0; Step < 60 && High - Low > 1e-9; ++Step)
			{
				const double A = Low + (High - Low) / 3.0;
				const double B = High - (High - Low) / 3.0;
				if (Cost(A) <= Cost(B))
				{
					High = B;
				}
				else
				{
					Low = A;
				}
			}
			return 0.5 * (Low + High);
		};

		double Amount0 = 0.0;
		double Amount1 = 0.0;
		if (Extras.Num() == 1)
		{
			Amount0 = Minimize(0.0, MaxAmount(0, 0.0), [&PeakOf](double Amount) { return PeakOf(Amount, 0.0); });
		}
		else if (Extras.Num() == 2)
		{
			const auto BestSecond = [&](double First)
			{
				return Minimize(0.0, MaxAmount(1, First), [&PeakOf, First](double Second) { return PeakOf(First, Second); });
			};
			Amount0 = Minimize(0.0, MaxAmount(0, 0.0), [&PeakOf, &BestSecond](double First) { return PeakOf(First, BestSecond(First)); });
			Amount1 = BestSecond(Amount0);
		}

		const double Peak = PeakOf(Amount0, Amount1);
		if (!(Peak > Epsilon))
		{
			return Levels;
		}
		for (int32 Channel = 0; Channel < 3; ++Channel)
		{
			Levels[Channel] = static_cast<float>(FMath::Clamp(Remaining(Amount0, Amount1, Channel) / Peak, 0.0, 1.0));
		}
		if (Extras.Num() > 0)
		{
			Levels[Extras[0].Index] = static_cast<float>(FMath::Clamp(Amount0 / Peak, 0.0, 1.0));
		}
		if (Extras.Num() > 1)
		{
			Levels[Extras[1].Index] = static_cast<float>(FMath::Clamp(Amount1 / Peak, 0.0, 1.0));
		}
		return Levels;
	}
}
