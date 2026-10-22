// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AI/StageAICommandSchema.h"
#include "Sketch/StageSketchTypes.h"

/** What the online model read in a stage drawing. */
struct FStageSketchVisionResult
{
	enum class EStatus : uint8
	{
		/** A sketch_plan call, validated. */
		Plan,
		/** The model answered in words instead (for example: this is not a stage plan). */
		Text,
		/** The model declined (stop_reason "refusal"). */
		Refused,
		/** Transport, status, format or validation failure; Error says which. */
		Failed,
	};

	EStatus Status = EStatus::Failed;

	/** Plan: strokes in image fractions (X 0..1 from the left edge, Y 0..1 from the top edge). A deck height of 0 means "not given". */
	TArray<FStageSketchStroke> Strokes;

	/** Elements or points the model gave that were out of range or over the limits, left out. */
	int32 Dropped = 0;

	FString Text;
	FText Error;
};

/**
 * The online reading of a stage drawing (Docs/ADR/0017-multilingual-voice-and-photo-import.md §2.7): the strict sketch_plan tool,
 * the Messages API request with the image, and validation of the answer. The model only describes the drawing; what it returns is
 * untrusted input, checked here and sanitized again by the sketch before anything is drawn or built. Pure: no HTTP, no world;
 * covered by the StageCraft.Sketch.Image tests.
 */
namespace StageSketchVision
{
	/** Name of the one tool the model may call. */
	MODULARSCENEBUILDER_API const TCHAR* GetToolName();

	/** The sketch_plan tool definition as a JSON object. */
	MODULARSCENEBUILDER_API FString MakeToolDefinitionJson();

	/** The reading instructions. Stable text, so it can be cached. */
	MODULARSCENEBUILDER_API FString GetSystemPrompt();

	/** The JSON body of POST /v1/messages: one user turn with the JPEG (base64) and the request to read it. */
	MODULARSCENEBUILDER_API FString BuildRequestBody(const FStageAIModelOptions& Options, const FString& JpegBase64);

	/** Reads a response: status, error object, stop_reason, then one sketch_plan call (validated) or the text blocks. */
	MODULARSCENEBUILDER_API FStageSketchVisionResult ParseResponseBody(int32 HttpStatus, const FString& Body);

	/** Strokes from image fractions to plan cm, for a Width x Height image WidthCm wide; decks without a height get DeckHeight. */
	MODULARSCENEBUILDER_API TArray<FStageSketchStroke> ToPlan(TConstArrayView<FStageSketchStroke> Fractions, int32 Width, int32 Height, double WidthCm, float DeckHeight);
}
