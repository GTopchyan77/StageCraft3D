// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Placement/StageDeckMath.h"

/** A procedural mesh as sections (one material each), ready for UProceduralMeshComponent::CreateMeshSection. */
struct FStageShapeMesh
{
	TArray<FStageDeckMesh::FSection> Sections;

	int32 GetTriangleCount() const;
	bool IsValid() const;
};

/** Stairs as built: steps of equal rise climbing toward +X, centred on the origin, standing on the floor. */
struct FStageStairsLayout
{
	int32 Steps = 0;
	/** Rise of one step and the total, in cm. */
	double StepRise = 0.0;
	double TotalRise = 0.0;
	/** Depth of one tread and the whole run (Steps × Tread), in cm. */
	double Tread = 0.0;
	double Run = 0.0;
	double Width = 0.0;

	bool IsValid() const { return Steps > 0 && StepRise > 0.0 && Tread > 0.0 && Width > 0.0; }
};

/**
 * A curved box truss along a circular arc. Local space: the arc's middle (apex) is the origin and its tangent there is +X; the
 * arc bends toward -Y around the centre (0, -Radius); the profile is centred on the arc line (Z from -Width/2 to +Width/2).
 */
struct FStageArcTrussLayout
{
	double Radius = 0.0;
	double AngleDegrees = 0.0;
	/** Along the centre line, in cm. */
	double ArcLength = 0.0;
	/** Rental sections (⌈arc length ÷ section length⌉): the bill of materials counts these. */
	int32 Sections = 0;
	/** Bracing bays along the arc (about one profile width each). */
	int32 Bays = 0;
	/** Sample points along each chord (curve smoothness). */
	int32 Samples = 0;
	double ProfileWidth = 0.0;

	bool IsValid() const { return Radius > 0.0 && AngleDegrees > 0.0 && Sections > 0 && Bays > 0 && Samples >= 2 && ProfileWidth > 0.0; }
	bool IsFullCircle() const { return AngleDegrees >= 360.0 - UE_KINDA_SMALL_NUMBER; }

	/** Where the centre line is at Fraction 0..1 along the arc, and the unit direction pointing away from the arc's centre. */
	FVector2D GetPoint(double Fraction) const;
	FVector2D GetOutward(double Fraction) const;
};

/**
 * Pure geometry of the assistant's non-standard stage shapes (Docs/ADR/0018 §5): round and arc riser outlines (built by the deck
 * actor), stairs and curved truss meshes. No world and no UObjects; covered by the StageCraft.Shapes tests. Deterministic.
 */
namespace StageProceduralShapes
{
	/** Corners of a circle that keep every chord within MaxSagittaCm of the true curve, 16..96 (a deck allows 128). */
	MODULARSCENEBUILDER_API int32 GetCircleSegments(double Radius, double MaxSagittaCm = 1.0);

	/** A circle around the origin, counter-clockwise (a round riser's outline). Empty for a non-positive or non-finite radius. */
	MODULARSCENEBUILDER_API TArray<FVector2D> MakeCircle(double Radius);

	/**
	 * A ring sector around the origin (a curved riser in front of a stage, a thrust edge): between InnerRadius and OuterRadius,
	 * AngleDegrees wide, symmetric about +X, counter-clockwise. InnerRadius 0 gives a pie slice; 360 degrees is not a simple
	 * polygon with a hole, so the angle is limited to 359. Empty for invalid sizes.
	 */
	MODULARSCENEBUILDER_API TArray<FVector2D> MakeArcRing(double InnerRadius, double OuterRadius, double AngleDegrees);

	/** Equal steps no taller than MaxStepRise that reach TotalRise; every input clamped to a usable size first. */
	MODULARSCENEBUILDER_API FStageStairsLayout LayoutStairs(double TotalRise, double Width, double Tread, double MaxStepRise);

	/**
	 * Section 0: the steps (one solid block per step, floor to its tread). Section 1, with bHandrails: two rails at
	 * HandrailHeight above the nosing line, each on a post at the bottom and the top step. Every face points outward.
	 */
	MODULARSCENEBUILDER_API FStageShapeMesh BuildStairsMesh(const FStageStairsLayout& Layout, bool bHandrails, double HandrailHeight = 90.0);

	/** The arc's sizes, sections and bays; Radius and AngleDegrees clamped to 50..5000 cm and 5..360 degrees. */
	MODULARSCENEBUILDER_API FStageArcTrussLayout LayoutArcTruss(double Radius, double AngleDegrees, double ProfileWidth, double SectionLength);

	/**
	 * Section 0: four chord tubes swept along the arc, rungs around the profile at every bay and one diagonal per face per bay
	 * (a box truss like F34). Tubes have 8 sides; ChordDiameter and BraceDiameter in cm.
	 */
	MODULARSCENEBUILDER_API FStageShapeMesh BuildArcTrussMesh(const FStageArcTrussLayout& Layout, double ChordDiameter, double BraceDiameter);

	/** One convex box per rental section around the profile, for selection and placing on it (the tubes are too thin to aim at). */
	MODULARSCENEBUILDER_API TArray<TArray<FVector>> BuildArcTrussCollision(const FStageArcTrussLayout& Layout);
}
