// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AI/StageAICommandSchema.h"
#include "GameplayTagContainer.h"
#include "Sketch/StageSketchTypes.h"

/** What a line of equipment is, as the online model may name it (wire names in StageVisionBom::GetKindName). Order is not saved. */
enum class EStageVisionBomKind : uint8
{
	TrussStraight,
	TrussCorner,
	TrussTower,
	GroundSupport,
	ChainHoist,
	Deck,
	LedWall,
	MovingHeadSpot,
	MovingHeadWash,
	MovingHeadBeam,
	ParWash,
	Strobe,
	Blinder,
	Followspot,
	Laser,
	LineArray,
	Subwoofer,
	SpeakerStack,
	MonitorWedge,
	MixingDesk,
	LightingDesk,
	Barricade,
	HazeMachine,
	Other,
	Count
};

/** Across the stage, seen from the audience's side of the stage (stage left is the performers' left, world +Y). */
enum class EStageVisionBomSide : uint8
{
	FullWidth,
	StageLeft,
	Centre,
	StageRight,
	Count
};

/** Along the stage: upstage is away from the audience (world +X); front of house is out in the audience area. */
enum class EStageVisionBomDepth : uint8
{
	Midstage,
	Upstage,
	Downstage,
	FrontOfHouse,
	Count
};

enum class EStageVisionBomLevel : uint8
{
	Floor,
	Deck,
	Flown,
	Count
};

/** One line of the bill of materials read from a photo. Everything in it came from the model and was validated (§55). */
struct FStageVisionBomLine
{
	EStageVisionBomKind Kind = EStageVisionBomKind::Other;

	/** 1..MaxQuantity. For straight truss: the number of runs; for decks and LED walls: the number of separate surfaces. */
	int32 Quantity = 1;

	/** The model's own confidence, 0..1. Lines under NeedsReviewBelow are marked for a person to check. */
	float Confidence = 0.f;

	/** Metres, 0 when not given. LengthM: a truss run's length or a deck's depth; WidthM: a deck's or LED wall's width; HeightM: an LED wall's or tower's height. */
	double LengthM = 0.0;
	double WidthM = 0.0;
	double HeightM = 0.0;

	EStageVisionBomSide Side = EStageVisionBomSide::Centre;
	EStageVisionBomDepth Depth = EStageVisionBomDepth::Midstage;
	EStageVisionBomLevel Level = EStageVisionBomLevel::Floor;

	/** What the model saw (a make or model, a colour), at most MaxNotesLength characters. */
	FString Notes;

	/** The catalog item that builds this line (StageVisionBom::MatchCatalog); unset when the catalog has none. */
	TWeakObjectPtr<class UBaseItemData> Match;

	bool NeedsReview() const;
};

/** The stage the model estimated, in metres; 0 = not given. Clamped to plausible ranges when read. */
struct FStageVisionBomStage
{
	double WidthM = 0.0;
	double DepthM = 0.0;
	double DeckHeightM = 0.0;
};

/** What the online model read in a stage photo. */
struct FStageVisionBomResult
{
	enum class EStatus : uint8
	{
		/** A stage_inventory call, validated. */
		Inventory,
		/** The model answered in words (for example: this is not a stage). */
		Text,
		/** The model declined (stop_reason "refusal"). */
		Refused,
		/** Transport, status, format or validation failure; Error says which. */
		Failed,
	};

	EStatus Status = EStatus::Failed;
	TArray<FStageVisionBomLine> Lines;
	FStageVisionBomStage Stage;

	/** Lines with an unknown kind, a bad quantity or past MaxLines, left out. */
	int32 Dropped = 0;

	/** Inventory: the model's notes on what it could not see. Text: its answer. */
	FString Text;
	FText Error;
};

/** Weight and power of the matched discrete items (spans and decks are measured by the scene's bill of materials once built). */
struct FStageVisionBomTotals
{
	double WeightKg = 0.0;
	double PowerW = 0.0;
	int32 MatchedLines = 0;
	int32 UnmatchedLines = 0;
	int32 LinesToReview = 0;
};

/** Standard truss sections one straight run is cut into (rental stock: 3, 2, 1 and 0.5 m). */
struct FStageVisionBomTrussSections
{
	int32 ThreeMetre = 0;
	int32 TwoMetre = 0;
	int32 OneMetre = 0;
	int32 HalfMetre = 0;

	int32 Num() const { return ThreeMetre + TwoMetre + OneMetre + HalfMetre; }
};

/** Where Build in Scene puts the stage: centred on the world origin, audience towards -X (as the starter stages). */
struct FStageVisionBomLayout
{
	double StageWidthCm = 1200.0;
	double StageDepthCm = 800.0;
	double DeckHeightCm = 120.0;
	double TrussHeightCm = 600.0;
	/** Distance from the downstage edge to the front-of-house position. */
	double FrontOfHouseCm = 1500.0;
};

/** One catalog item at a transform. */
struct FStageVisionBomPlacement
{
	TWeakObjectPtr<class UBaseItemData> Item;
	FTransform Transform = FTransform::Identity;
};

/** What Build in Scene makes from the bill: sketch strokes for the deck and truss (planned by the sketch generator) and items. */
struct FStageVisionBomBuildInput
{
	TArray<FStageSketchStroke> Strokes;
	TArray<FStageVisionBomPlacement> Items;
	/** User-facing, one per line or kind that is not built ("2 barricade lines: draw them with the barricade tool"). */
	TArray<FText> Warnings;
};

/**
 * The Vision Inspector (Docs/ADR/0032 §8): a reference photo of a real stage read by the online model into a bill of materials, matched
 * to the catalog and laid out for building. The model only lists what it sees; every answer is untrusted input and validated here.
 * Pure: no HTTP, no world, no assets loaded; covered by the StageCraft.Vision.* tests.
 */
namespace StageVisionBom
{
	constexpr int32 MaxLines = 128;
	constexpr int32 MaxQuantity = 500;
	constexpr int32 MaxNotesLength = 160;
	constexpr double MaxDimensionM = 100.0;
	constexpr float NeedsReviewBelow = 0.5f;
	/** Build in Scene places at most this many items; the rest are listed as warnings. */
	constexpr int32 MaxPlacedItems = 400;

	MODULARSCENEBUILDER_API const TCHAR* GetToolName();
	MODULARSCENEBUILDER_API FString MakeToolDefinitionJson();
	MODULARSCENEBUILDER_API FString GetSystemPrompt();

	/** The JSON body of POST /v1/messages: one user turn with the JPEG (base64). */
	MODULARSCENEBUILDER_API FString BuildRequestBody(const FStageAIModelOptions& Options, const FString& JpegBase64);

	/** Reads a response: status, error object, stop_reason, then one stage_inventory call (validated) or the text blocks. */
	MODULARSCENEBUILDER_API FStageVisionBomResult ParseResponseBody(int32 HttpStatus, const FString& Body);

	MODULARSCENEBUILDER_API const TCHAR* GetKindName(EStageVisionBomKind Kind);
	MODULARSCENEBUILDER_API TOptional<EStageVisionBomKind> ParseKind(const FString& Name);
	MODULARSCENEBUILDER_API FText GetKindDisplayName(EStageVisionBomKind Kind);
	MODULARSCENEBUILDER_API FText DescribePosition(const FStageVisionBomLine& Line);

	/** The catalog category that builds Kind; empty for kinds the catalog cannot build. */
	MODULARSCENEBUILDER_API FGameplayTag GetCategoryForKind(EStageVisionBomKind Kind);

	/**
	 * Sets each line's Match: the first catalog item (in catalog order) in the kind's category whose name contains the kind's keyword,
	 * else the first in the category. Imported models never match. Deterministic for the same catalog.
	 */
	MODULARSCENEBUILDER_API void MatchCatalog(TArray<FStageVisionBomLine>& Lines, TConstArrayView<const class UBaseItemData*> Catalog);

	/** A run of LengthM cut into standard sections, rounded up to 0.5 m; empty for a length of 0 or less. */
	MODULARSCENEBUILDER_API FStageVisionBomTrussSections SplitTrussRun(double LengthM);

	/** 500 x 500 mm panels covering WidthM x HeightM (rounded up per side); 0 when either is not given. */
	MODULARSCENEBUILDER_API int32 GetLedPanelCount(double WidthM, double HeightM);

	/** The real-world spec of a line ("2 runs of 12 m: 8 x 3 m sections", "6 x 3.5 m: 84 panels of 500 x 500 mm"); empty when none. */
	MODULARSCENEBUILDER_API FText DescribeSpec(const FStageVisionBomLine& Line);

	MODULARSCENEBUILDER_API FStageVisionBomTotals ComputeTotals(TConstArrayView<FStageVisionBomLine> Lines);

	/** RFC 4180 CSV with a header row; cells that a spreadsheet would run as a formula are prefixed with an apostrophe. */
	MODULARSCENEBUILDER_API FString ToCsv(TConstArrayView<FStageVisionBomLine> Lines);

	/** The layout for the model's stage estimate: given sizes clamped to plausible ranges, the defaults otherwise. */
	MODULARSCENEBUILDER_API FStageVisionBomLayout MakeLayout(const FStageVisionBomStage& Stage, bool bHasDeck);

	/**
	 * Lays the bill out on Layout's stage: one deck stroke (when the bill has a deck), straight truss runs across the stage
	 * (downstage, upstage, then between), and every other matched line as items spread over its region. Flown lighting hangs on the
	 * nearest run, upside down. Deterministic.
	 */
	MODULARSCENEBUILDER_API FStageVisionBomBuildInput MakeBuildInput(TConstArrayView<FStageVisionBomLine> Lines, const FStageVisionBomLayout& Layout);
}
