// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Sketch/StageSketchTypes.h"

/** An 8-bit grayscale image, row by row from the top-left (0 = black ink, 255 = white paper). */
struct FStageSketchGrayImage
{
	int32 Width = 0;
	int32 Height = 0;
	TArray<uint8> Pixels;

	bool IsValid() const { return Width > 0 && Height > 0 && Pixels.Num() == Width * Height; }
	uint8 At(int32 X, int32 Y) const { return Pixels[Y * Width + X]; }
};

/** How a traced drawing is scaled into the sketch. */
struct FStageSketchTraceOptions
{
	/** Real width of the whole image, in cm; the image is centred on the plan origin. */
	double WidthCm = 2000.0;

	/** Height given to the decks found (nested ones stand one step higher each). */
	float DeckHeight = 60.f;
};

/** What a drawing became: sketch strokes in plan cm, and what was found, dropped or assumed. */
struct FStageSketchTraceResult
{
	TArray<FStageSketchStroke> Strokes;
	int32 DeckCount = 0;
	int32 TrussCount = 0;
	int32 FixtureCount = 0;

	/** Small marks that are neither lines nor fixture dots (text, smudges), left out. */
	int32 IgnoredMarks = 0;

	/** Light lines on a dark background (a blackboard, a negative scan) were read inverted. */
	bool bInverted = false;

	/** User-facing notes: nothing found, limits reached, the image read inverted. */
	TArray<FText> Warnings;
};

/**
 * Reading a hand-drawn stage plan, a scan or a photo of one (Docs/ADR/0017-multilingual-voice-and-photo-import.md §2.6), with the
 * Sketch window's own conventions: a closed outline is a deck, an open line is a truss run, a small dot or circle is a fixture.
 *
 *  1. Ink: an adaptive threshold (each pixel against the mean of its neighbourhood, from an integral image), so uneven light and
 *     shadows on paper do not turn into ink; mostly-ink images are read as light lines on dark. A 3 x 3 closing bridges one-pixel
 *     gaps in pencil lines, and specks are removed.
 *  2. Decks: background regions that do not reach the image border are enclosed by ink. Large ones are traced along their edge
 *     (Moore neighbour tracing) and simplified (Ramer-Douglas-Peucker). A deck inside another stands one deck height higher.
 *  3. Fixtures: small compact marks (filled dots, or small circles) become fixtures at their centre.
 *  4. Truss: the remaining ink is thinned to one-pixel lines (Zhang-Suen) and followed from end to end or junction; short spurs are
 *     dropped and each run is simplified.
 *  5. Pixels become plan cm: the image's width is WidthCm, its centre the plan origin, image up is plan +X and image right +Y (the
 *     canvas's top view).
 *
 * Pure and deterministic (the same pixels give the same strokes); no world, no UObjects, safe on a worker thread. Covered by the
 * StageCraft.Sketch.Image tests. The strokes are only a starting point: they land in the sketch to be checked and corrected, and
 * the sketch sanitizes them again (StageSketchMath::SanitizeStroke).
 */
namespace StageSketchImageTrace
{
	/** Longest side the drawing is traced at; larger images are scaled down first. */
	constexpr int32 MaxTraceSide = 800;

	/** Most elements one drawing gives (the sketch holds StageSketchMath::MaxStrokes in all). */
	constexpr int32 MaxDecks = 32;
	constexpr int32 MaxTrussLines = 64;
	constexpr int32 MaxFixtures = 96;

	/** The image scaled down (box filter) so its longest side is at most MaxSide; unchanged when it already fits. */
	MODULARSCENEBUILDER_API FStageSketchGrayImage Downscale(const FStageSketchGrayImage& Image, int32 MaxSide);

	/**
	 * The ink mask (1 = ink) of an adaptive threshold; light ink on a dark background is inverted first. bOutInverted says whether
	 * it was. Empty for an invalid image.
	 */
	MODULARSCENEBUILDER_API TArray<uint8> FindInk(const FStageSketchGrayImage& Image, bool& bOutInverted);

	/** A pixel position (x right, y down, in pixels) of a Width x Height image as a plan point in cm (see the namespace comment). */
	MODULARSCENEBUILDER_API FVector2D PixelToPlan(const FVector2D& Pixel, int32 Width, int32 Height, double WidthCm);

	/** The whole pipeline. Images larger than MaxTraceSide are scaled down first. */
	MODULARSCENEBUILDER_API FStageSketchTraceResult Trace(const FStageSketchGrayImage& Image, const FStageSketchTraceOptions& Options);
}
