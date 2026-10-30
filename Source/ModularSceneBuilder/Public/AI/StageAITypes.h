// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AI/StageAILanguage.h"
#include "Environment/StageEnvironmentTypes.h"
#include "StageAITypes.generated.h"

/**
 * Everything the AI assistant can do (Docs/ADR/0016-ai-assistant.md §2.3). A closed set: text from the user or from a language
 * model only ever becomes one of these, and StageAICommandSchema::Validate checks its parameters before it runs.
 */
enum class EStageAIIntent : uint8
{
	Help,

	// Questions (read-only).
	StageSummary,
	ItemCount,
	TotalWeight,
	PowerDraw,
	FixtureCount,
	/** Placed items whose name matches ItemQuery. */
	CountMatching,
	ListCues,
	EnvironmentStatus,

	// Actions (through the controller's requests).
	AddFixtures,
	StoreCue,
	GoToCue,
	CueGo,
	CueBack,
	SetWeather,
	SetTimeOfDay,
	SetSky,
	Export,
	Undo,
	Redo,

	// Targeted edits of placed items (Docs/ADR/0018 §6): the selection, or the placed items whose name matches ItemQuery.
	DeleteItems,
	MoveItems,
	/** Each target becomes one Replacement item at its place. */
	ReplaceItems,
	/** A procedural stage shape (Shape and its sizes). */
	BuildShape,
	/** Fixture controls (FixtureSettings) on the targeted fixtures (Docs/ADR/0020 §7). */
	SetFixture,

	/** A stage operation the assistant deliberately does not run (clearing the whole stage, open, save): it answers where to find it. */
	Unsupported,

	Count,
};

/** Which production report an Export command writes. */
enum class EStageAIReport : uint8
{
	BillOfMaterials,
	PatchSheet,
	CableSchedule,
	All,
};

/** Which placed items a targeted edit (DeleteItems, MoveItems, ReplaceItems) applies to. */
enum class EStageAITarget : uint8
{
	/** The asking player's selected items. */
	Selection,
	/** Placed items whose catalog name contains every word of ItemQuery (at most Count of them, the latest placed first). */
	Named,
};

/** A MoveItems direction, as seen from the camera and snapped to the nearest stage axis (Up and Down are vertical). */
enum class EStageAIDirection : uint8
{
	Left,
	Right,
	Forward,
	Back,
	Up,
	Down,
};

/** A BuildShape shape. */
enum class EStageAIShape : uint8
{
	/** A round deck (Radius, Height). */
	RoundRiser,
	/** A curved deck ring in front of the stage (Radius to its outer edge, Width deep, Angle wide, Height). */
	ArcRiser,
	/** Stairs (Height = rise, Width). */
	Stairs,
	/** Curved truss (Radius, Angle; 360 is a circle; Height = hang height). */
	ArcTruss,
};

/** A control a SetFixture command sets on fixtures. */
enum class EStageAIFixtureControl : uint8
{
	Dimmer,
	Color,
	Gel,
	Zoom,
	Frost,
	Iris,
	CTO,
	Prism,
	PrismSpin,
	Gobo,
	GoboSpin,
	Strobe,
	Pan,
	Tilt,
	Count,
};

/** One control of a SetFixture command. */
struct MODULARSCENEBUILDER_API FStageAIFixtureSetting
{
	EStageAIFixtureControl Control = EStageAIFixtureControl::Dimmer;

	/**
	 * Dimmer, Frost, Iris, CTO: 0..1. Zoom, Pan, Tilt: degrees. GoboSpin, PrismSpin: rpm. Strobe: flashes per second (0 = off).
	 * Prism, Gobo: a slot (0 = out / open). Unset for a Gel, a Color, a named Gobo and a Zoom given as "narrow" / "wide".
	 */
	TOptional<double> Value;

	/** Gel: the gel ("lee 106", "primary red"). Gobo: a gobo name ("star"). Zoom: "narrow" or "wide". Color: the colour's name. */
	FString Name;

	/** Color: the target colour, linear. */
	FLinearColor Color = FLinearColor::White;
};

/**
 * One interpreted request. Only the fields of its intent are meaningful; an unset optional means "the default" (one fixture,
 * the next free cue number, the default fade, the item's default size).
 */
struct MODULARSCENEBUILDER_API FStageAICommand
{
	EStageAIIntent Intent = EStageAIIntent::Help;

	/** AddFixtures: how many. DeleteItems, MoveItems, ReplaceItems with a Named target: at most how many (unset: all that match). */
	TOptional<int32> Count;

	/**
	 * AddFixtures: words naming the fixture ("moving head"). CountMatching and a Named target: words naming the placed item.
	 * Unsupported: the operation.
	 */
	FString ItemQuery;

	/** DeleteItems, MoveItems, ReplaceItems. */
	EStageAITarget Target = EStageAITarget::Selection;

	/** ReplaceItems: words naming the catalog item to put in place ("wash"). */
	FString Replacement;

	/** MoveItems. */
	EStageAIDirection Direction = EStageAIDirection::Forward;

	/** MoveItems, in cm (unset: one metre). */
	TOptional<double> DistanceCm;

	/** BuildShape. */
	EStageAIShape Shape = EStageAIShape::RoundRiser;

	/** BuildShape sizes in cm and degrees; unset takes the item's default (see EStageAIShape for which apply). */
	TOptional<double> RadiusCm;
	TOptional<double> WidthCm;
	TOptional<double> HeightCm;
	TOptional<double> AngleDegrees;

	/** BuildShape Stairs. */
	bool bHandrails = true;

	/** SetFixture: what to set, in the order asked (at most StageAICommandParser::MaxFixtureSettings). */
	TArray<FStageAIFixtureSetting> FixtureSettings;

	/** StoreCue (unset: the next whole number after the last cue), GoToCue. */
	TOptional<float> CueNumber;

	/** StoreCue. */
	FString Label;

	/** StoreCue, in seconds. */
	TOptional<float> FadeSeconds;

	/** SetWeather. Never Custom. */
	EStageWeatherPreset Weather = EStageWeatherPreset::Clear;

	/** SetTimeOfDay, in hours [0, 24). */
	float Hour = 12.f;

	/** SetSky. */
	bool bSkyOn = true;

	/** Export. */
	EStageAIReport Report = EStageAIReport::All;

	/** The language the reply is written in: the prompt's (StageAILanguage::Detect), or the online model's reply_language. */
	EStageAILanguage Language = EStageAILanguage::English;

	bool IsQuestion() const;
	bool IsAction() const;
};

/** Who wrote a conversation line. */
UENUM(BlueprintType)
enum class EStageAISpeaker : uint8
{
	User,
	Assistant,
};

/** How an assistant line should read: a plain answer, something done, or something refused or failed. */
UENUM(BlueprintType)
enum class EStageAIOutcome : uint8
{
	Info,
	Succeeded,
	Refused,
	/** Started; a later line reports how it ended (placing fixtures waits for their assets). */
	Pending,
};

/** One line of the conversation. */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageAIMessage
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Assistant")
	EStageAISpeaker Speaker = EStageAISpeaker::Assistant;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Assistant")
	EStageAIOutcome Outcome = EStageAIOutcome::Info;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Assistant")
	FText Text;
};

/** What a prompt was understood as: a command to run, or a reply to show as it is. */
struct FStageAIInterpretation
{
	enum class EKind : uint8
	{
		Command,
		Reply,
	};

	EKind Kind = EKind::Reply;
	FStageAICommand Command;

	/** Kind == Reply: the answer. */
	FText Reply;
	EStageAIOutcome ReplyOutcome = EStageAIOutcome::Info;

	/** Shown after the result, e.g. that the online model was unavailable and the local parser was used. May be empty. */
	FText Note;

	static FStageAIInterpretation MakeCommand(const FStageAICommand& InCommand);
	static FStageAIInterpretation MakeReply(const FText& InReply, EStageAIOutcome InOutcome = EStageAIOutcome::Info);
};

/** How running one action ended, as the conversation reports it. */
struct FStageAIActionOutcome
{
	EStageAIOutcome Outcome = EStageAIOutcome::Refused;
	FText Message;

	static FStageAIActionOutcome Make(EStageAIOutcome InOutcome, const FText& InMessage)
	{
		FStageAIActionOutcome Result;
		Result.Outcome = InOutcome;
		Result.Message = InMessage;
		return Result;
	}
};

/**
 * A read-only snapshot of the stage for answering questions and for the online model's context (StageAICommandParser::
 * AnswerQuestion, StageAICommandSchema::DescribeScene). Plain values, so answers are testable without a world.
 */
struct FStageAISceneView
{
	int32 PlacedItems = 0;
	float TotalPowerWatts = 0.f;
	float TotalWeightKg = 0.f;

	/** Session limits; 0 = unlimited. */
	int32 MaxPlacedItems = 0;
	float MaxPowerWatts = 0.f;
	float MaxWeightKg = 0.f;

	/** Placed item names (the catalog display name), one entry per placed item. */
	TArray<FString> ItemNames;

	/** The asking player's selected items (catalog display names). */
	TArray<FString> SelectedItemNames;

	/** Fixture count per fixture name, in first-seen order. */
	TArray<TPair<FString, int32>> FixturesByName;

	struct FCue
	{
		float Number = 0.f;
		FString Label;
		float FadeSeconds = 0.f;
	};
	TArray<FCue> Cues;
	/** The cue on stage, or 0 for none. */
	float ActiveCue = 0.f;

	bool bHasEnvironment = false;
	bool bSkyAvailable = false;
	FStageEnvironmentSettings Environment;

	int32 GetFixtureCount() const;
};
