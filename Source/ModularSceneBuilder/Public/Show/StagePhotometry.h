// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Approximate photometrics of a stage fixture's beam (Docs/ADR/0007-lighting-patch-paths-and-pipeline-hardening.md §5.4):
 * peak intensity, illuminance at the throw distance and beam diameter, for planning and pre-vis, not certified light
 * calculations.
 *
 * The beam is modelled as a cone of uniform intensity: I = Phi * T / Omega, with Omega = 2 pi (1 - cos(theta / 2)). Real
 * fixtures have a peaked distribution, so the centre of a real beam is brighter than this and its edge darker.
 *
 * Pure and deterministic: no world, no UObjects. Covered by the StageCraft.Lighting.Photometry tests.
 */
namespace StagePhotometry
{
	/** Narrowest and widest beam the model accepts, in degrees (full angle). Inputs outside are clamped. */
	constexpr double MinBeamAngleDegrees = 0.1;
	constexpr double MaxBeamAngleDegrees = 179.0;

	struct FInputs
	{
		/** Output of the fixture at full intensity, open white, in lumens. */
		double LuminousFluxLm = 0.0;

		/** Full beam angle in degrees, after zoom and iris. */
		double BeamAngleDegrees = 0.0;

		/** Fraction of the flux that leaves the fixture, 0..1 (dimmer, colour filter, gobo, blades). */
		double Transmission = 1.0;

		/** Distance to the lit surface along the beam axis, in cm. Zero or less: nothing is lit in range. */
		double ThrowCm = 0.0;
	};

	struct FResult
	{
		/** The beam angle used, in degrees (clamped). */
		double BeamAngleDegrees = 0.0;

		/** Peak (on-axis) intensity in candela. */
		double IntensityCd = 0.0;

		/** True when a surface is in range; the throw-dependent values below are zero otherwise. */
		bool bHasThrow = false;

		double ThrowMetres = 0.0;

		/** Illuminance on a surface perpendicular to the beam axis at the throw distance, in lux. */
		double IlluminanceLux = 0.0;

		/** Diameter of the beam at the throw distance, in metres. */
		double BeamDiameterMetres = 0.0;
	};

	/** Solid angle of a cone with the given full angle, in steradians. */
	MODULARSCENEBUILDER_API double GetConeSolidAngle(double FullAngleDegrees);

	/**
	 * Fraction of a white source's light that a colour mix passes, 0..1: the Rec. 709 luminance of the linear mix. Full white
	 * passes everything; saturated blue passes about 7 %.
	 */
	MODULARSCENEBUILDER_API double GetColorTransmission(const FLinearColor& LinearColor);

	/**
	 * Open fraction of the beam with framing shutters inserted, 0..1. Insertions are 0 (out) to 1 (blade at the beam centre),
	 * top, right, bottom, left. Opposite blades that meet close the beam. Approximated over the beam's bounding square.
	 */
	MODULARSCENEBUILDER_API double GetShutterOpenFraction(double Top, double Right, double Bottom, double Left);

	/** Beam angle after an iris closed by Iris (0 open .. 1 closed to MinFraction of the beam). */
	MODULARSCENEBUILDER_API double ApplyIris(double BeamAngleDegrees, double Iris, double MinFraction);

	/**
	 * Light passed by an iris (the area it leaves open). An iris crops a uniform beam: flux and solid angle shrink together,
	 * so the peak intensity stays the same while the beam gets narrower.
	 */
	MODULARSCENEBUILDER_API double GetIrisTransmission(double Iris, double MinFraction);

	/** Peak intensity, illuminance and beam diameter. Non-finite or negative inputs count as zero. */
	MODULARSCENEBUILDER_API FResult Compute(const FInputs& Inputs);
}
