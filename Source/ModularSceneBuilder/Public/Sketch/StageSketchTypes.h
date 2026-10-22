// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "StageSketchTypes.generated.h"

/** How a stroke was drawn in the Sketch window (Docs/ADR/0015-sketch-to-stage.md §2.3). */
UENUM(BlueprintType)
enum class EStageSketchStrokeKind : uint8
{
	/** Drawn by dragging: closed when it ends near its start (a stage shape), otherwise a curved truss line. */
	Freehand,
	/** A polygon clicked corner by corner: a stage deck. Always closed. */
	Shape,
	/** A polyline clicked point by point: a truss path; closed when finished on its first point. */
	Truss,
	/** One clicked point: a fixture. */
	Fixture,
};

/** What a stroke becomes in 3D. */
UENUM(BlueprintType)
enum class EStageSketchElement : uint8
{
	Deck,
	Truss,
	Fixture,
	Count	UMETA(Hidden),
};

/** The Sketch window's tools: one active at a time. Erase removes the stroke under a click. */
UENUM(BlueprintType)
enum class EStageSketchTool : uint8
{
	Freehand,
	Shape,
	Truss,
	Fixture,
	Erase,
};

/** Who reads an imported photo or scan (Docs/ADR/0017-multilingual-voice-and-photo-import.md §2.6-2.7). */
UENUM(BlueprintType)
enum class EStageSketchReader : uint8
{
	/** The edge pipeline on this computer (StageSketchImageTrace): offline and deterministic. */
	Local,
	/** Claude reading the image (StageSketchVision), with the local reading as the fallback. */
	Claude,
};

/** What a photo import is doing. */
UENUM(BlueprintType)
enum class EStageSketchImportState : uint8
{
	Idle,
	/** The file is being decoded and traced on a worker thread. */
	Reading,
	/** Waiting for the online reading. */
	ReadingOnline,
};

/** Where a generated fixture stands. */
enum class EStageSketchFixtureMount : uint8
{
	Floor,
	/** On top of a generated deck. */
	Deck,
	/** Hung from a generated truss, upside down. */
	Truss,
};

/**
 * One stroke of the sketch, in world plan coordinates: X and Y in cm (the viewport's Top projection: screen up is +X, screen
 * right is +Y). Bounded and sanitized by StageSketchMath::SanitizeStroke when added to the sketch.
 */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageSketchStroke
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Sketch")
	EStageSketchStrokeKind Kind = EStageSketchStrokeKind::Freehand;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Sketch")
	TArray<FVector2D> Points;

	/** The last point connects back to the first (shapes, closed freehand strokes, truss loops). The first point is not repeated. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Sketch")
	bool bClosed = false;

	/** Deck top above the floor in cm, for strokes that become decks: the panel's deck height when the stroke was drawn. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Sketch", meta = (Units = "cm"))
	float DeckHeight = 60.f;

	EStageSketchElement GetElement() const;
};

/** The Sketch window's settings: the height of new shapes, the truss height and the grid. Sanitized by the subsystem. */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageSketchSettings
{
	GENERATED_BODY()

	/** Height given to new stage shapes, in cm. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Sketch", meta = (Units = "cm"))
	float DeckHeight = 60.f;

	/** Height of the bottom of generated truss, in cm. Fixtures hang from it. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Sketch", meta = (Units = "cm"))
	float TrussHeight = 600.f;

	/** Grid step in cm: canvas grid, and the snap of deck corners and fixtures. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Sketch", meta = (Units = "cm"))
	float GridStep = 50.f;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Sketch")
	bool bSnapToGrid = true;

	/** Freehand stabiliser, 0 (raw) to MaxSmoothing: how far each sample trails the cursor. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Sketch")
	float Smoothing = 0.5f;

	/** Real width, in cm, of the next photo or scan imported into the sketch (Docs/ADR/0017 §2.6): it sets the drawing's scale. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Sketch", meta = (Units = "cm"))
	float PhotoWidth = 2000.f;

	bool operator==(const FStageSketchSettings& Other) const
	{
		return DeckHeight == Other.DeckHeight && TrussHeight == Other.TrussHeight && GridStep == Other.GridStep
			&& bSnapToGrid == Other.bSnapToGrid && Smoothing == Other.Smoothing && PhotoWidth == Other.PhotoWidth;
	}
};

/** One deck to build: a sanitized outline around its origin, which stands on the floor. */
struct FStageSketchDeckPlan
{
	int32 StrokeIndex = INDEX_NONE;
	FTransform Transform = FTransform::Identity;
	TArray<FVector2D> LocalOutline;
	double Height = 0.0;
	double Area = 0.0;
	int32 Panels = 0;
};

/** One truss path to build, already laid out by StageSpanMath as the span actor will normalise it. */
struct FStageSketchTrussPlan
{
	int32 StrokeIndex = INDEX_NONE;
	FTransform Transform = FTransform::Identity;
	TArray<FVector> LocalPoints;
	bool bClosed = false;
	int32 SegmentCount = 0;
};

struct FStageSketchFixturePlan
{
	int32 StrokeIndex = INDEX_NONE;
	FTransform Transform = FTransform::Identity;
	EStageSketchFixtureMount Mount = EStageSketchFixtureMount::Floor;
};

/**
 * What a sketch will build (StageSketchGenerator::BuildPlan): plain values plus weak references to the catalog items, so a plan
 * kept while its items stream in never keeps an asset alive or points at a destroyed one.
 */
struct FStageSketchBuildPlan
{
	TWeakObjectPtr<class UStageDeckData> DeckItem;
	TWeakObjectPtr<class UStageSpanData> TrussItem;
	TWeakObjectPtr<class UBaseItemData> FixtureItem;

	TArray<FStageSketchDeckPlan> Decks;
	TArray<FStageSketchTrussPlan> Trusses;
	TArray<FStageSketchFixturePlan> Fixtures;

	/** Strokes skipped or changed, user-facing, in stroke order. */
	TArray<FText> Warnings;

	int32 Num() const { return Decks.Num() + Trusses.Num() + Fixtures.Num(); }
	bool IsEmpty() const { return Num() == 0; }
	int32 GetPanelCount() const;
	int32 GetSegmentCount() const;
};

/** How a generation request ended, for the Sketch window. */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageSketchGenerateResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Sketch")
	bool bSucceeded = false;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Sketch")
	int32 DeckCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Sketch")
	int32 TrussCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Sketch")
	int32 FixtureCount = 0;

	/** User-facing summary: what was built, or why nothing was. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Sketch")
	FText Message;
};
