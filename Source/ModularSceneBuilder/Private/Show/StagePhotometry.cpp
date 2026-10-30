// Copyright Epic Games, Inc. All Rights Reserved.

#include "Show/StagePhotometry.h"

namespace StagePhotometry
{
	namespace
	{
		constexpr double CmToMetres = 0.01;

		double Sanitize(double Value)
		{
			return FMath::IsFinite(Value) ? FMath::Max(0.0, Value) : 0.0;
		}

		double SanitizeFraction(double Value)
		{
			return FMath::IsFinite(Value) ? FMath::Clamp(Value, 0.0, 1.0) : 0.0;
		}

		double ClampBeamAngle(double FullAngleDegrees)
		{
			return FMath::Clamp(FMath::IsFinite(FullAngleDegrees) ? FullAngleDegrees : MinBeamAngleDegrees, MinBeamAngleDegrees, MaxBeamAngleDegrees);
		}

		/** Iris scale of the beam's radius: 1 open, MinFraction fully closed. */
		double GetIrisScale(double Iris, double MinFraction)
		{
			return FMath::Lerp(1.0, SanitizeFraction(MinFraction), SanitizeFraction(Iris));
		}
	}

	double GetConeSolidAngle(double FullAngleDegrees)
	{
		const double HalfAngle = FMath::DegreesToRadians(ClampBeamAngle(FullAngleDegrees) * 0.5);
		return 2.0 * UE_DOUBLE_PI * (1.0 - FMath::Cos(HalfAngle));
	}

	double GetColorTransmission(const FLinearColor& LinearColor)
	{
		// Rec. 709 / sRGB luminance weights on linear components.
		const double Luminance = 0.2126 * SanitizeFraction(LinearColor.R) + 0.7152 * SanitizeFraction(LinearColor.G) + 0.0722 * SanitizeFraction(LinearColor.B);
		return FMath::Clamp(Luminance, 0.0, 1.0);
	}

	double GetShutterOpenFraction(double Top, double Right, double Bottom, double Left)
	{
		// Insertion 1 reaches the centre, i.e. half the beam's height or width.
		const double OpenHeight = FMath::Max(0.0, 1.0 - 0.5 * (SanitizeFraction(Top) + SanitizeFraction(Bottom)));
		const double OpenWidth = FMath::Max(0.0, 1.0 - 0.5 * (SanitizeFraction(Left) + SanitizeFraction(Right)));
		return OpenHeight * OpenWidth;
	}

	double ApplyIris(double BeamAngleDegrees, double Iris, double MinFraction)
	{
		// The iris scales the beam's radius at the gate, so the tangent of the half angle scales with it.
		const double HalfAngle = FMath::DegreesToRadians(ClampBeamAngle(BeamAngleDegrees) * 0.5);
		const double Scaled = FMath::Atan(FMath::Tan(HalfAngle) * GetIrisScale(Iris, MinFraction));
		return ClampBeamAngle(FMath::RadiansToDegrees(Scaled) * 2.0);
	}

	double GetIrisTransmission(double Iris, double MinFraction)
	{
		return FMath::Square(GetIrisScale(Iris, MinFraction));
	}

	double ApplyFrost(double AngleDegrees, double Frost, double FullFrostMultiplier)
	{
		const double Multiplier = FMath::IsFinite(FullFrostMultiplier) ? FMath::Max(1.0, FullFrostMultiplier) : 1.0;
		return ClampBeamAngle(ClampBeamAngle(AngleDegrees) * FMath::Lerp(1.0, Multiplier, SanitizeFraction(Frost)));
	}

	double GetFieldAngle(double BeamAngleDegrees, double FieldAngleRatio)
	{
		const double Ratio = FMath::IsFinite(FieldAngleRatio) ? FMath::Max(1.0, FieldAngleRatio) : 1.0;
		return ClampBeamAngle(ClampBeamAngle(BeamAngleDegrees) * Ratio);
	}

	double GetInnerConeAngle(double BeamAngleDegrees, double FieldAngleDegrees)
	{
		const double Beam = ClampBeamAngle(BeamAngleDegrees);
		const double Field = FMath::Max(Beam, ClampBeamAngle(FieldAngleDegrees));
		// Full intensity inside the inner cone, falling to nothing at the field edge: halfway is the beam edge.
		return FMath::Clamp(2.0 * Beam - Field, 0.0, Field);
	}

	FResult Compute(const FInputs& Inputs)
	{
		FResult Result;
		Result.BeamAngleDegrees = ClampBeamAngle(Inputs.BeamAngleDegrees);

		const double Flux = Sanitize(Inputs.LuminousFluxLm) * SanitizeFraction(Inputs.Transmission);
		Result.IntensityCd = Flux / GetConeSolidAngle(Result.BeamAngleDegrees);

		const double Throw = Sanitize(Inputs.ThrowCm) * CmToMetres;
		if (Throw > 0.0)
		{
			Result.bHasThrow = true;
			Result.ThrowMetres = Throw;
			Result.IlluminanceLux = Result.IntensityCd / FMath::Square(Throw);
			Result.BeamDiameterMetres = 2.0 * Throw * FMath::Tan(FMath::DegreesToRadians(Result.BeamAngleDegrees * 0.5));
		}
		return Result;
	}
}
