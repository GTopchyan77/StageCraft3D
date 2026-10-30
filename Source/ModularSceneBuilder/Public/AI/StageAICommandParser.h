// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AI/StageAITypes.h"

/** A lighting fixture the assistant may add, described by plain names so matching needs no assets. */
struct FStageAIFixtureCandidate
{
	/** The catalog display name ("Spot 350"). */
	FString Name;

	/** The fixture type's display name ("Moving Head (Spot)"). */
	FString KindName;
};

/**
 * The assistant's local interpreter (Docs/ADR/0016-ai-assistant.md §2.1, §2.3): a fixed English grammar that turns a request into
 * one FStageAICommand. An Armenian prompt is first rewritten into its words (StageAIArmenian, Docs/ADR/0017 §2.2) and the command
 * then replies in Armenian. Pure and deterministic, works offline, and covered by the StageCraft.AI.Parser tests. Its output is still
 * validated by StageAICommandSchema::Validate before anything runs, like a command from the online model.
 */
namespace StageAICommandParser
{
	/** Most fixtures one command adds. */
	constexpr int32 MaxFixturesPerCommand = 24;

	/** Added fixtures: spacing along the row, gap from the items already placed, and the grid they snap to (cm). */
	constexpr double FixtureRowSpacing = 100.0;
	constexpr double FixtureRowGap = 200.0;
	constexpr double FixtureRowGrid = 50.0;

	/** Longest prompt read; the rest is ignored (the panel also caps what can be typed). */
	constexpr int32 MaxPromptLength = 500;

	/** Most items one delete, move or replace command changes. */
	constexpr int32 MaxEditTargets = 200;

	/** A move without a distance goes one metre; no move goes further than MaxMoveDistanceCm. */
	constexpr double DefaultMoveDistanceCm = 100.0;
	constexpr double MaxMoveDistanceCm = 5000.0;

	/** A SetFixture command sets at most this many controls. */
	constexpr int32 MaxFixtureSettings = 12;

	/** Strobe without a rate ("strobe the blinders") flashes this many times a second. */
	constexpr double DefaultStrobeHz = 10.0;

	/** Spin without a speed ("spin the gobo") turns this many times a minute. */
	constexpr double DefaultSpinRpm = 10.0;

	/**
	 * The linear colour a colour name stands for ("red", "deep blue", "warm white", "uv"), or unset. Saturated named colours are
	 * targets for colour mixing; the fixture reaches them as closely as its emitters or filters allow.
	 */
	MODULARSCENEBUILDER_API TOptional<FLinearColor> FindColorName(const FString& Name);

	/** The command Text asks for, or unset when the grammar does not understand it. */
	MODULARSCENEBUILDER_API TOptional<FStageAICommand> Parse(const FString& Text);

	/**
	 * The candidate Query names: the one whose name or fixture type shares the most words with it (plurals and generic words such as
	 * "lights" and "fixtures" ignored). A query of only generic words picks the first candidate. INDEX_NONE when none match.
	 */
	MODULARSCENEBUILDER_API int32 MatchFixture(TConstArrayView<FStageAIFixtureCandidate> Candidates, const FString& Query);

	/** Query has no words, or only generic fixture words ("lights", "fixtures"): it names "a light", not a particular item. */
	MODULARSCENEBUILDER_API bool IsGenericFixtureQuery(const FString& Query);

	/**
	 * A placed item's catalog name answers Query (a Named target, CountMatching): every word of Query, singular and lowercase, is a
	 * word of Name, except generic fixture words, which match any item with bIsFixture ("the lights" names every fixture). False
	 * for an empty query.
	 */
	MODULARSCENEBUILDER_API bool MatchesItemName(const FString& Name, const FString& Query, bool bIsFixture = false);

	/**
	 * Plan positions (X, Y in cm) for Count added fixtures: a row along Y, FixtureRowSpacing apart and centred on Y = 0, at X = 0
	 * on an empty stage or FixtureRowGap beyond (towards -X) the lowest X of the items already placed, snapped to FixtureRowGrid.
	 * Count is clamped to 1..MaxFixturesPerCommand.
	 */
	MODULARSCENEBUILDER_API TArray<FVector2D> LayoutFixtureRow(int32 Count, TOptional<double> LowestPlacedX);
}

/** The assistant's words (Docs/ADR/0016-ai-assistant.md §2.3): help, answers to questions and explanations. Pure. */
namespace StageAIAnswers
{
	/** What the assistant can do, with examples, in Language. */
	MODULARSCENEBUILDER_API FText DescribeHelp(EStageAILanguage Language = EStageAILanguage::English);

	/** The reply to a prompt the grammar did not understand, in the prompt's language. */
	MODULARSCENEBUILDER_API FText DescribeNotUnderstood(const FString& Prompt);

	/** Why an Unsupported command is not run and where to find it instead, in Language. */
	MODULARSCENEBUILDER_API FText DescribeUnsupported(const FString& Operation, EStageAILanguage Language = EStageAILanguage::English);

	/** The answer to a question command (FStageAICommand::IsQuestion) about Scene, in the command's Language. */
	MODULARSCENEBUILDER_API FText AnswerQuestion(const FStageAICommand& Command, const FStageAISceneView& Scene);

	/** "14:30" for 14.5 hours. */
	MODULARSCENEBUILDER_API FString FormatHour(float Hour);

	/** The weather preset's lowercase name ("rain") in Language. */
	MODULARSCENEBUILDER_API FText DescribeWeather(EStageWeatherPreset Preset, EStageAILanguage Language = EStageAILanguage::English);
}
