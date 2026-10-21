// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"

/** The extruded prism of a deck, ready for a procedural mesh: section 0 is the top, section 1 the skirt walls. */
struct FStageDeckMesh
{
	struct FSection
	{
		TArray<FVector> Vertices;
		TArray<int32> Triangles;
		TArray<FVector> Normals;
		TArray<FVector2D> UVs;

		bool IsEmpty() const { return Triangles.IsEmpty(); }
	};

	FSection Top;
	FSection Skirt;

	bool IsValid() const { return !Top.IsEmpty() && !Skirt.IsEmpty(); }
};

/**
 * Pure geometry of stage decks (Docs/ADR/0015-sketch-to-stage.md §2.6): simple polygons in the horizontal plane (cm, X/Y), their
 * triangulation and the extruded deck mesh. No world and no UObjects; covered by the StageCraft.Sketch.Deck tests. Deterministic.
 */
namespace StageDeckMath
{
	/** Most corners a deck outline may have; later points are dropped. */
	constexpr int32 MaxOutlinePoints = 128;

	/** Smallest deck, in cm² (0.25 m²). */
	constexpr double MinArea = 2500.0;

	/** Farthest a corner may be from the deck's origin, in cm. */
	constexpr double MaxExtent = 10000.0;

	/** Corners closer than this (cm) to the previous one are merged. */
	constexpr double MinEdgeLength = 1.0;

	/** Signed area in cm²: positive when the points run counter-clockwise in the X/Y plane (X to Y). */
	MODULARSCENEBUILDER_API double GetSignedArea(TConstArrayView<FVector2D> Polygon);

	MODULARSCENEBUILDER_API FVector2D GetCentroid(TConstArrayView<FVector2D> Polygon);

	/** True when no two non-adjacent edges touch or cross and there are three or more corners. */
	MODULARSCENEBUILDER_API bool IsSimplePolygon(TConstArrayView<FVector2D> Polygon);

	/** Even-odd test; points on an edge count as inside. */
	MODULARSCENEBUILDER_API bool IsPointInPolygon(TConstArrayView<FVector2D> Polygon, const FVector2D& Point);

	/**
	 * Ear-clipping triangulation of a simple polygon (either orientation). Writes indices into Polygon, three per triangle,
	 * every triangle counter-clockwise in X/Y. False, and OutTriangles empty, for a polygon that is not simple.
	 */
	MODULARSCENEBUILDER_API bool Triangulate(TConstArrayView<FVector2D> Polygon, TArray<int32>& OutTriangles);

	/**
	 * Makes an untrusted outline usable: non-finite points and points beyond MaxExtent dropped, corners closer than MinEdgeLength
	 * (including the last to the first) merged, collinear corners removed, at most MaxOutlinePoints kept, counter-clockwise.
	 * False when what is left is not a simple polygon of at least MinArea; OutOutline is then empty.
	 */
	MODULARSCENEBUILDER_API bool SanitizeOutline(TConstArrayView<FVector2D> Outline, TArray<FVector2D>& OutOutline);

	/** A Width x Depth rectangle centred on the origin, counter-clockwise. */
	MODULARSCENEBUILDER_API TArray<FVector2D> MakeRectangle(double Width, double Depth);

	/** Deck panels needed to cover AreaCm2 with panels of PanelAreaCm2: rounded up, at least 1 (0 for invalid input). */
	MODULARSCENEBUILDER_API int32 GetPanelCount(double AreaCm2, double PanelAreaCm2);

	/**
	 * The engine's front-face normal of a triangle, (P1 - P2) x (P0 - P2), as UKismetProceduralMeshLibrary computes it for
	 * procedural meshes. Builders check every triangle against it, so faces are never inside out.
	 */
	MODULARSCENEBUILDER_API FVector GetFaceNormal(const FVector& P0, const FVector& P1, const FVector& P2);

	/**
	 * The deck prism for a sanitized outline and a height (cm, > 0): a top at Z = Height facing up, and one flat-shaded wall per
	 * edge from Z = 0 to Height facing out. UVs are in metres (1 UV unit = 1 m). No bottom face: decks stand on a surface.
	 * Invalid (empty) when the outline cannot be triangulated or the height is not positive and finite.
	 */
	MODULARSCENEBUILDER_API FStageDeckMesh BuildMesh(TConstArrayView<FVector2D> Outline, double Height);
}
