// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for the AI assistant (Docs/ADR/0016-ai-assistant.md): the local grammar, validation at the trust boundary, the online
// model's wire format (tool schema, request body, response parsing), answers, fixture matching and layout, and the conversation
// flow of UStageAICommandSubsystem against a real game world with a recording executor.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.AI; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AI/StageAICommandParser.h"
#include "AI/StageAICommandSchema.h"
#include "AI/StageAICommandSubsystem.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include <limits>

namespace StageAITests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	FString Name(EStageAIIntent Intent) { return StageAICommandSchema::GetIntentName(Intent); }

	/** The intent Text parses to, or "none". */
	FString IntentOf(const TCHAR* Text)
	{
		const TOptional<FStageAICommand> Command = StageAICommandParser::Parse(Text);
		return Command.IsSet() ? Name(Command->Intent) : FString(TEXT("none"));
	}

	FStageAICommand ParseOrDefault(const TCHAR* Text)
	{
		return StageAICommandParser::Parse(Text).Get(FStageAICommand());
	}

	TSharedPtr<FJsonObject> ParseJson(const FString& Json)
	{
		TSharedPtr<FJsonObject> Object;
		FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(Json), Object);
		return Object;
	}

	/** A Messages API response with one stage_command call. */
	FString ToolResponse(const FString& InputJson)
	{
		return FString::Printf(TEXT("{\"id\":\"msg_1\",\"type\":\"message\",\"role\":\"assistant\",\"stop_reason\":\"tool_use\",\"content\":[")
			TEXT("{\"type\":\"text\",\"text\":\"Adding them.\"},{\"type\":\"tool_use\",\"id\":\"toolu_1\",\"name\":\"stage_command\",\"input\":%s}]}"), *InputJson);
	}

	FStageAIMessage Line(EStageAISpeaker Speaker, const TCHAR* Text)
	{
		FStageAIMessage Message;
		Message.Speaker = Speaker;
		Message.Text = FText::FromString(Text);
		return Message;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAIParserTest, "StageCraft.AI.Parser", StageAITests::Flags)

bool FStageAIParserTest::RunTest(const FString& Parameters)
{
	using namespace StageAITests;

	// Nothing to understand.
	TestEqual(TEXT("Empty"), IntentOf(TEXT("")), FString(TEXT("none")));
	TestEqual(TEXT("Only punctuation"), IntentOf(TEXT("?!...")), FString(TEXT("none")));
	TestEqual(TEXT("Gibberish"), IntentOf(TEXT("purple elephant dances")), FString(TEXT("none")));

	// Help and history.
	TestEqual(TEXT("help"), IntentOf(TEXT("help")), Name(EStageAIIntent::Help));
	TestEqual(TEXT("what can you do"), IntentOf(TEXT("What can you do?")), Name(EStageAIIntent::Help));
	TestEqual(TEXT("undo"), IntentOf(TEXT("Undo")), Name(EStageAIIntent::Undo));
	TestEqual(TEXT("undo that"), IntentOf(TEXT("undo that please")), Name(EStageAIIntent::Undo));
	TestEqual(TEXT("redo"), IntentOf(TEXT("redo")), Name(EStageAIIntent::Redo));

	// Questions.
	TestEqual(TEXT("status"), IntentOf(TEXT("status")), Name(EStageAIIntent::StageSummary));
	TestEqual(TEXT("summary"), IntentOf(TEXT("Give me a summary of the stage")), Name(EStageAIIntent::StageSummary));
	TestEqual(TEXT("weight"), IntentOf(TEXT("How heavy is the stage?")), Name(EStageAIIntent::TotalWeight));
	TestEqual(TEXT("total weight"), IntentOf(TEXT("total weight")), Name(EStageAIIntent::TotalWeight));
	TestEqual(TEXT("power"), IntentOf(TEXT("What's the power draw?")), Name(EStageAIIntent::PowerDraw));
	TestEqual(TEXT("watts"), IntentOf(TEXT("how many watts do we use")), Name(EStageAIIntent::PowerDraw));
	TestEqual(TEXT("fixtures"), IntentOf(TEXT("How many fixtures are there?")), Name(EStageAIIntent::FixtureCount));
	TestEqual(TEXT("lights"), IntentOf(TEXT("lights")), Name(EStageAIIntent::FixtureCount));
	TestEqual(TEXT("items"), IntentOf(TEXT("how many items are placed")), Name(EStageAIIntent::ItemCount));
	const FStageAICommand Truss = ParseOrDefault(TEXT("How many truss sections?"));
	TestEqual(TEXT("count truss"), Name(Truss.Intent), Name(EStageAIIntent::CountMatching));
	TestEqual(TEXT("... query without the generic word"), Truss.ItemQuery, FString(TEXT("truss")));
	TestEqual(TEXT("cue list"), IntentOf(TEXT("list cues")), Name(EStageAIIntent::ListCues));
	TestEqual(TEXT("cue count"), IntentOf(TEXT("how many cues do we have?")), Name(EStageAIIntent::ListCues));
	TestEqual(TEXT("weather question"), IntentOf(TEXT("what's the weather like?")), Name(EStageAIIntent::EnvironmentStatus));
	TestEqual(TEXT("time question"), IntentOf(TEXT("What time is it on stage?")), Name(EStageAIIntent::EnvironmentStatus));

	// Adding fixtures.
	const FStageAICommand Six = ParseOrDefault(TEXT("Add 6 moving heads"));
	TestEqual(TEXT("add 6"), Name(Six.Intent), Name(EStageAIIntent::AddFixtures));
	TestEqual(TEXT("... count"), Six.Count.Get(0), 6);
	TestEqual(TEXT("... query"), Six.ItemQuery, FString(TEXT("moving heads")));
	const FStageAICommand OnePar = ParseOrDefault(TEXT("place a LED par on the stage"));
	TestEqual(TEXT("a = 1"), OnePar.Count.Get(0), 1);
	TestEqual(TEXT("... query"), OnePar.ItemQuery, FString(TEXT("led par")));
	TestEqual(TEXT("word number"), ParseOrDefault(TEXT("spawn twelve lights")).Count.Get(0), 12);
	TestEqual(TEXT("a dozen"), ParseOrDefault(TEXT("add a dozen spots")).Count.Get(0), 12);
	TestEqual(TEXT("a couple of"), ParseOrDefault(TEXT("put a couple of pars on stage")).Count.Get(0), 2);
	TestEqual(TEXT("can you add"), IntentOf(TEXT("can you add 4 fixtures")), Name(EStageAIIntent::AddFixtures));
	TestFalse(TEXT("No count given"), ParseOrDefault(TEXT("add fixtures")).Count.IsSet());
	TestEqual(TEXT("A huge count is kept for validation to refuse"), ParseOrDefault(TEXT("add 5000 lights")).Count.Get(0), 1000);

	// Cues.
	const FStageAICommand Store = ParseOrDefault(TEXT("Store cue 5 called Big Intro fade 3"));
	TestEqual(TEXT("store"), Name(Store.Intent), Name(EStageAIIntent::StoreCue));
	TestEqual(TEXT("... number"), Store.CueNumber.Get(0.f), 5.f);
	TestEqual(TEXT("... label keeps capitals"), Store.Label, FString(TEXT("Big Intro")));
	TestEqual(TEXT("... fade"), Store.FadeSeconds.Get(-1.f), 3.f);
	const FStageAICommand Quoted = ParseOrDefault(TEXT("record cue 2.5 \"Walk in\" in 4 seconds"));
	TestEqual(TEXT("quoted label"), Quoted.Label, FString(TEXT("Walk in")));
	TestEqual(TEXT("... decimal number"), Quoted.CueNumber.Get(0.f), 2.5f);
	TestEqual(TEXT("... seconds"), Quoted.FadeSeconds.Get(-1.f), 4.f);
	const FStageAICommand NextCue = ParseOrDefault(TEXT("save a new cue"));
	TestEqual(TEXT("store without number"), Name(NextCue.Intent), Name(EStageAIIntent::StoreCue));
	TestFalse(TEXT("... number left to the runner"), NextCue.CueNumber.IsSet());
	const FStageAICommand GoTo = ParseOrDefault(TEXT("go to cue 2"));
	TestEqual(TEXT("go to cue"), Name(GoTo.Intent), Name(EStageAIIntent::GoToCue));
	TestEqual(TEXT("... number"), GoTo.CueNumber.Get(0.f), 2.f);
	TestEqual(TEXT("cue 3"), IntentOf(TEXT("cue 3")), Name(EStageAIIntent::GoToCue));
	TestEqual(TEXT("go"), IntentOf(TEXT("go")), Name(EStageAIIntent::CueGo));
	TestEqual(TEXT("next cue"), IntentOf(TEXT("next cue please")), Name(EStageAIIntent::CueGo));
	TestEqual(TEXT("back"), IntentOf(TEXT("back")), Name(EStageAIIntent::CueBack));
	TestEqual(TEXT("previous cue"), IntentOf(TEXT("previous cue")), Name(EStageAIIntent::CueBack));

	// Environment.
	const FStageAICommand Rain = ParseOrDefault(TEXT("make it rain"));
	TestEqual(TEXT("rain"), Name(Rain.Intent), Name(EStageAIIntent::SetWeather));
	TestTrue(TEXT("... preset"), Rain.Weather == EStageWeatherPreset::Rain);
	TestTrue(TEXT("stop the rain"), ParseOrDefault(TEXT("stop the rain")).Weather == EStageWeatherPreset::Clear);
	TestTrue(TEXT("foggy"), ParseOrDefault(TEXT("Foggy please")).Weather == EStageWeatherPreset::Fog);
	TestTrue(TEXT("overcast"), ParseOrDefault(TEXT("overcast")).Weather == EStageWeatherPreset::Cloudy);
	TestTrue(TEXT("snow"), ParseOrDefault(TEXT("let it snow")).Weather == EStageWeatherPreset::Snow);
	TestTrue(TEXT("clear sky"), ParseOrDefault(TEXT("clear sky")).Weather == EStageWeatherPreset::Clear);
	const FStageAICommand HalfSix = ParseOrDefault(TEXT("set time to 18:30"));
	TestEqual(TEXT("18:30"), Name(HalfSix.Intent), Name(EStageAIIntent::SetTimeOfDay));
	TestEqual(TEXT("... hour"), HalfSix.Hour, 18.5f);
	TestEqual(TEXT("6 pm"), ParseOrDefault(TEXT("make it 6 pm")).Hour, 18.f);
	TestEqual(TEXT("9pm"), ParseOrDefault(TEXT("set the clock to 9pm")).Hour, 21.f);
	TestEqual(TEXT("12 am"), ParseOrDefault(TEXT("12 am")).Hour, 0.f);
	TestEqual(TEXT("sunset"), ParseOrDefault(TEXT("sunset")).Hour, 19.5f);
	TestEqual(TEXT("midnight"), ParseOrDefault(TEXT("midnight")).Hour, 0.f);
	const FStageAICommand Outdoors = ParseOrDefault(TEXT("go outdoors"));
	TestEqual(TEXT("outdoors"), Name(Outdoors.Intent), Name(EStageAIIntent::SetSky));
	TestTrue(TEXT("... on"), Outdoors.bSkyOn);
	TestFalse(TEXT("indoor stage"), ParseOrDefault(TEXT("indoor stage")).bSkyOn);
	TestFalse(TEXT("turn off the sky"), ParseOrDefault(TEXT("turn off the sky")).bSkyOn);

	// Exports.
	TestTrue(TEXT("bom"), ParseOrDefault(TEXT("export the bill of materials")).Report == EStageAIReport::BillOfMaterials);
	TestTrue(TEXT("patch"), ParseOrDefault(TEXT("Export DMX patch sheet")).Report == EStageAIReport::PatchSheet);
	TestTrue(TEXT("cables"), ParseOrDefault(TEXT("export cable schedule")).Report == EStageAIReport::CableSchedule);
	TestTrue(TEXT("power plan is the cable schedule, not a question"), ParseOrDefault(TEXT("export power plan")).Report == EStageAIReport::CableSchedule);
	TestTrue(TEXT("all"), ParseOrDefault(TEXT("export all reports")).Report == EStageAIReport::All);
	TestTrue(TEXT("export alone"), ParseOrDefault(TEXT("export")).Report == EStageAIReport::All);

	// Clearing the whole stage is explained, not run; deleting named items is a targeted edit (StageCraft.AI.Edits).
	const FStageAICommand Clear = ParseOrDefault(TEXT("clear the stage"));
	TestEqual(TEXT("clear stage"), Name(Clear.Intent), Name(EStageAIIntent::Unsupported));
	TestEqual(TEXT("... operation"), Clear.ItemQuery, FString(TEXT("delete")));
	TestEqual(TEXT("delete everything"), IntentOf(TEXT("delete everything")), Name(EStageAIIntent::Unsupported));
	TestEqual(TEXT("delete all lights"), IntentOf(TEXT("delete all lights")), Name(EStageAIIntent::DeleteItems));
	TestEqual(TEXT("open scene"), ParseOrDefault(TEXT("open my scene")).ItemQuery, FString(TEXT("open")));
	TestEqual(TEXT("save"), ParseOrDefault(TEXT("save the show")).ItemQuery, FString(TEXT("save")));

	// A very long prompt is read only up to the cap.
	FString Long = TEXT("add 2 lights ");
	for (int32 Index = 0; Index < 200; ++Index)
	{
		Long += TEXT("words ");
	}
	TestEqual(TEXT("Long prompt still parses"), IntentOf(*Long), Name(EStageAIIntent::AddFixtures));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAISchemaTest, "StageCraft.AI.Schema", StageAITests::Flags)

bool FStageAISchemaTest::RunTest(const FString& Parameters)
{
	using namespace StageAITests;
	FText Error;

	// Validation: ranges and normalization.
	{
		FStageAICommand Command;
		Command.Intent = EStageAIIntent::AddFixtures;
		TestTrue(TEXT("Add without count is one"), StageAICommandSchema::Validate(Command, Error) && Command.Count.Get(0) == 1);
		Command.Count = 0;
		TestFalse(TEXT("Zero fixtures refused"), StageAICommandSchema::Validate(Command, Error));
		Command.Count = StageAICommandParser::MaxFixturesPerCommand + 1;
		TestFalse(TEXT("Too many fixtures refused"), StageAICommandSchema::Validate(Command, Error));
		TestFalse(TEXT("... with a reason"), Error.IsEmpty());
		Command.Count = StageAICommandParser::MaxFixturesPerCommand;
		Command.ItemQuery = TEXT("   ") + FString::ChrN(200, TEXT('x'));
		TestTrue(TEXT("Max fixtures allowed"), StageAICommandSchema::Validate(Command, Error));
		TestEqual(TEXT("Query trimmed and capped"), Command.ItemQuery.Len(), StageAICommandSchema::MaxQueryLength);
	}
	{
		FStageAICommand Command;
		Command.Intent = EStageAIIntent::StoreCue;
		Command.CueNumber = 0.f;
		TestFalse(TEXT("Cue 0 refused"), StageAICommandSchema::Validate(Command, Error));
		Command.CueNumber = std::numeric_limits<float>::quiet_NaN();
		TestFalse(TEXT("NaN cue refused"), StageAICommandSchema::Validate(Command, Error));
		Command.CueNumber = 2.0004f;
		Command.FadeSeconds = 601.f;
		TestFalse(TEXT("Fade above 600 s refused"), StageAICommandSchema::Validate(Command, Error));
		Command.FadeSeconds = std::numeric_limits<float>::infinity();
		TestFalse(TEXT("Infinite fade refused"), StageAICommandSchema::Validate(Command, Error));
		Command.FadeSeconds = 0.f;
		TestTrue(TEXT("Valid store"), StageAICommandSchema::Validate(Command, Error));
		TestEqual(TEXT("Number rounded like the cue list"), Command.CueNumber.Get(0.f), 2.f);
		Command.CueNumber.Reset();
		TestTrue(TEXT("Store without number is allowed (next free)"), StageAICommandSchema::Validate(Command, Error));
	}
	{
		FStageAICommand Command;
		Command.Intent = EStageAIIntent::GoToCue;
		TestFalse(TEXT("Go to cue needs a number"), StageAICommandSchema::Validate(Command, Error));
		Command.Intent = EStageAIIntent::SetWeather;
		Command.Weather = EStageWeatherPreset::Custom;
		TestFalse(TEXT("Custom weather refused"), StageAICommandSchema::Validate(Command, Error));
		Command.Intent = EStageAIIntent::SetTimeOfDay;
		Command.Hour = 25.f;
		TestFalse(TEXT("Hour 25 refused"), StageAICommandSchema::Validate(Command, Error));
		Command.Hour = 24.f;
		TestTrue(TEXT("Hour 24 allowed"), StageAICommandSchema::Validate(Command, Error) && Command.Hour == 0.f);
		Command.Intent = EStageAIIntent::CountMatching;
		Command.ItemQuery = TEXT("  ");
		TestFalse(TEXT("Count of nothing refused"), StageAICommandSchema::Validate(Command, Error));
		Command.Intent = EStageAIIntent::Unsupported;
		Command.ItemQuery = TEXT("format disk");
		TestTrue(TEXT("Unsupported normalized"), StageAICommandSchema::Validate(Command, Error) && Command.ItemQuery == TEXT("delete"));
		Command.Intent = EStageAIIntent(200);
		TestFalse(TEXT("Out-of-range intent refused"), StageAICommandSchema::Validate(Command, Error));
	}

	// Tool input from the model: untrusted JSON.
	{
		FStageAICommand Command;
		TestTrue(TEXT("Tool: add fixtures"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"add_fixtures\",\"count\":4,\"item\":\"wash\"}"), Command, Error));
		TestTrue(TEXT("... intent"), Command.Intent == EStageAIIntent::AddFixtures);
		TestEqual(TEXT("... count"), Command.Count.Get(0), 4);
		TestEqual(TEXT("... item"), Command.ItemQuery, FString(TEXT("wash")));
		TestTrue(TEXT("Tool: weather"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"set_weather\",\"weather\":\"fog\"}"), Command, Error)
			&& Command.Weather == EStageWeatherPreset::Fog);
		TestTrue(TEXT("Tool: time"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"set_time_of_day\",\"hour\":20.25}"), Command, Error)
			&& Command.Hour == 20.25f);
		TestTrue(TEXT("Tool: export"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"export\",\"report\":\"patch_sheet\"}"), Command, Error)
			&& Command.Report == EStageAIReport::PatchSheet);
		TestTrue(TEXT("Tool: null optional fields are ignored"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"store_cue\",\"cue_number\":null}"), Command, Error)
			&& !Command.CueNumber.IsSet());

		TestFalse(TEXT("Tool: not JSON"), StageAICommandSchema::CommandFromToolInputJson(TEXT("add 4 lights"), Command, Error));
		TestFalse(TEXT("Tool: no intent"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"count\":4}"), Command, Error));
		TestFalse(TEXT("Tool: unknown intent"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"delete_everything\"}"), Command, Error));
		TestFalse(TEXT("Tool: unknown field"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"help\",\"shell\":\"rm -rf\"}"), Command, Error));
		TestFalse(TEXT("Tool: fractional count"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"add_fixtures\",\"count\":2.5}"), Command, Error));
		TestFalse(TEXT("Tool: count as text"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"add_fixtures\",\"count\":\"lots\"}"), Command, Error));
		TestFalse(TEXT("Tool: too many"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"add_fixtures\",\"count\":500}"), Command, Error));
		TestFalse(TEXT("Tool: weather missing"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"set_weather\"}"), Command, Error));
		TestFalse(TEXT("Tool: unknown weather"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"set_weather\",\"weather\":\"custom\"}"), Command, Error));
		TestFalse(TEXT("Tool: time missing"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"set_time_of_day\"}"), Command, Error));
		TestFalse(TEXT("Tool: sky missing"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"set_sky\"}"), Command, Error));
		TestFalse(TEXT("Tool: bad report"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"export\",\"report\":\"passwords\"}"), Command, Error));
		TestFalse(TEXT("Tool: go to cue 0"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"go_to_cue\",\"cue_number\":0}"), Command, Error));
	}

	// Every intent has a distinct wire name the tool lists.
	{
		const TSharedPtr<FJsonObject> Tool = ParseJson(StageAICommandSchema::MakeToolDefinitionJson());
		TestTrue(TEXT("Tool definition is JSON"), Tool.IsValid());
		if (Tool.IsValid())
		{
			TestEqual(TEXT("Tool name"), Tool->GetStringField(TEXT("name")), FString(TEXT("stage_command")));
			TestTrue(TEXT("Strict"), Tool->GetBoolField(TEXT("strict")));
			const TSharedPtr<FJsonObject> Schema = Tool->GetObjectField(TEXT("input_schema"));
			TestFalse(TEXT("No additional properties"), Schema->GetBoolField(TEXT("additionalProperties")));
			const TArray<TSharedPtr<FJsonValue>> Intents = Schema->GetObjectField(TEXT("properties"))->GetObjectField(TEXT("intent"))->GetArrayField(TEXT("enum"));
			TestEqual(TEXT("Every intent listed"), Intents.Num(), int32(EStageAIIntent::Count));
			TSet<FString> Unique;
			for (int32 Intent = 0; Intent < int32(EStageAIIntent::Count); ++Intent)
			{
				Unique.Add(Name(EStageAIIntent(Intent)));
			}
			TestEqual(TEXT("Wire names are unique"), Unique.Num(), int32(EStageAIIntent::Count));
		}
	}

	// The request body.
	{
		const TArray<FStageAIMessage> History = {
			Line(EStageAISpeaker::Assistant, TEXT("leading assistant line is dropped")),
			Line(EStageAISpeaker::User, TEXT("status")),
			Line(EStageAISpeaker::Assistant, TEXT("The stage has 3 items.")),
			Line(EStageAISpeaker::Assistant, TEXT("joined with the line before")),
			Line(EStageAISpeaker::User, TEXT("a dangling user line before the prompt is dropped")),
		};
		FStageAIModelOptions Options;
		const TSharedPtr<FJsonObject> Body = ParseJson(StageAICommandSchema::BuildRequestBody(Options, History, TEXT("add 2 pars"), TEXT("Placed items: 3.")));
		TestTrue(TEXT("Body is JSON"), Body.IsValid());
		if (Body.IsValid())
		{
			TestEqual(TEXT("Model"), Body->GetStringField(TEXT("model")), FString(TEXT("claude-opus-5-5")));
			TestEqual(TEXT("Effort"), Body->GetObjectField(TEXT("output_config"))->GetStringField(TEXT("effort")), FString(TEXT("low")));
			TestEqual(TEXT("Fallbacks"), Body->GetStringField(TEXT("fallbacks")), FString(TEXT("default")));
			TestFalse(TEXT("No thinking field (adaptive by default)"), Body->HasField(TEXT("thinking")));
			TestEqual(TEXT("Auto tool choice"), Body->GetObjectField(TEXT("tool_choice"))->GetStringField(TEXT("type")), FString(TEXT("auto")));
			TestTrue(TEXT("One call at most"), Body->GetObjectField(TEXT("tool_choice"))->GetBoolField(TEXT("disable_parallel_tool_use")));
			TestEqual(TEXT("One tool"), Body->GetArrayField(TEXT("tools")).Num(), 1);
			const TArray<TSharedPtr<FJsonValue>> Messages = Body->GetArrayField(TEXT("messages"));
			TestEqual(TEXT("Turns: user, assistant, new user"), Messages.Num(), 3);
			if (Messages.Num() == 3)
			{
				TestEqual(TEXT("First is user"), Messages[0]->AsObject()->GetStringField(TEXT("role")), FString(TEXT("user")));
				TestEqual(TEXT("Second is assistant"), Messages[1]->AsObject()->GetStringField(TEXT("role")), FString(TEXT("assistant")));
				TestTrue(TEXT("Assistant lines joined"), Messages[1]->AsObject()->GetStringField(TEXT("content")).Contains(TEXT("joined with the line before")));
				const FString Last = Messages[2]->AsObject()->GetStringField(TEXT("content"));
				TestTrue(TEXT("New turn has the scene"), Last.Contains(TEXT("Placed items: 3.")));
				TestTrue(TEXT("New turn has the prompt"), Last.Contains(TEXT("add 2 pars")));
			}
		}
	}

	// Response parsing.
	{
		using EStatus = FStageAIModelResponse::EStatus;
		const FStageAIModelResponse Call = StageAICommandSchema::ParseResponseBody(200, ToolResponse(TEXT("{\"intent\":\"add_fixtures\",\"count\":2}")));
		TestTrue(TEXT("Tool call"), Call.Status == EStatus::Command && Call.Command.Intent == EStageAIIntent::AddFixtures && Call.Command.Count.Get(0) == 2);
		const FStageAIModelResponse Invalid = StageAICommandSchema::ParseResponseBody(200, ToolResponse(TEXT("{\"intent\":\"add_fixtures\",\"count\":99}")));
		TestTrue(TEXT("Invalid tool call fails, flagged"), Invalid.Status == EStatus::Failed && Invalid.bInvalidCommand && !Invalid.Error.IsEmpty());
		const FStageAIModelResponse Text = StageAICommandSchema::ParseResponseBody(200,
			TEXT("{\"stop_reason\":\"end_turn\",\"content\":[{\"type\":\"thinking\",\"thinking\":\"\"},{\"type\":\"text\",\"text\":\" Which truss? \"}]}"));
		TestTrue(TEXT("Text answer"), Text.Status == EStatus::Text && Text.Text == TEXT("Which truss?"));
		TestTrue(TEXT("Refusal"), StageAICommandSchema::ParseResponseBody(200, TEXT("{\"stop_reason\":\"refusal\",\"content\":[]}")).Status == EStatus::Refused);
		const FStageAIModelResponse Unauthorized = StageAICommandSchema::ParseResponseBody(401,
			TEXT("{\"type\":\"error\",\"error\":{\"type\":\"authentication_error\",\"message\":\"invalid x-api-key\"}}"));
		TestTrue(TEXT("HTTP error"), Unauthorized.Status == EStatus::Failed && !Unauthorized.bInvalidCommand);
		TestTrue(TEXT("... quotes the API message"), Unauthorized.Error.ToString().Contains(TEXT("invalid x-api-key")));
		TestTrue(TEXT("Malformed body"), StageAICommandSchema::ParseResponseBody(200, TEXT("<html>")).Status == EStatus::Failed);
		TestTrue(TEXT("Max tokens, no text"), StageAICommandSchema::ParseResponseBody(200, TEXT("{\"stop_reason\":\"max_tokens\",\"content\":[]}")).Status == EStatus::Failed);
		TestTrue(TEXT("Another tool's call is ignored"), StageAICommandSchema::ParseResponseBody(200,
			TEXT("{\"stop_reason\":\"tool_use\",\"content\":[{\"type\":\"tool_use\",\"id\":\"t\",\"name\":\"bash\",\"input\":{\"intent\":\"undo\"}}]}")).Status == EStatus::Failed);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAIAnswersTest, "StageCraft.AI.Answers", StageAITests::Flags)

bool FStageAIAnswersTest::RunTest(const FString& Parameters)
{
	using namespace StageAITests;

	// Fixture matching.
	const TArray<FStageAIFixtureCandidate> Candidates = {
		{ TEXT("Spot 350"), TEXT("Moving Head (Spot)") },
		{ TEXT("Aura Wash"), TEXT("Moving Head (Wash)") },
		{ TEXT("RGBW Par 12"), TEXT("LED Par") },
		{ TEXT("Atomic Strobe"), TEXT("Strobe / Blinder") },
	};
	TestEqual(TEXT("Generic picks the first"), StageAICommandParser::MatchFixture(Candidates, TEXT("lights")), 0);
	TestEqual(TEXT("Empty picks the first"), StageAICommandParser::MatchFixture(Candidates, TEXT("")), 0);
	TestEqual(TEXT("Wash"), StageAICommandParser::MatchFixture(Candidates, TEXT("moving head washes")), 1);
	TestEqual(TEXT("Pars"), StageAICommandParser::MatchFixture(Candidates, TEXT("LED pars")), 2);
	TestEqual(TEXT("Slang: par cans"), StageAICommandParser::MatchFixture(Candidates, TEXT("par cans")), 2);
	TestEqual(TEXT("Slang: movers"), StageAICommandParser::MatchFixture(Candidates, TEXT("movers")), 0);
	TestEqual(TEXT("By name"), StageAICommandParser::MatchFixture(Candidates, TEXT("atomic")), 3);
	TestEqual(TEXT("No match"), StageAICommandParser::MatchFixture(Candidates, TEXT("laser")), int32(INDEX_NONE));
	TestEqual(TEXT("Empty catalog"), StageAICommandParser::MatchFixture({}, TEXT("lights")), int32(INDEX_NONE));

	// Row layout.
	const TArray<FVector2D> Four = StageAICommandParser::LayoutFixtureRow(4, {});
	TestEqual(TEXT("Four positions"), Four.Num(), 4);
	TestTrue(TEXT("Centred on the empty stage"), Four.Num() == 4 && Four[0] == FVector2D(0.0, -150.0) && Four[3] == FVector2D(0.0, 150.0));
	const TArray<FVector2D> Beyond = StageAICommandParser::LayoutFixtureRow(1, 333.0);
	TestTrue(TEXT("Beyond placed items, on the grid"), Beyond.Num() == 1 && Beyond[0] == FVector2D(150.0, 0.0));
	TestEqual(TEXT("Count clamped high"), StageAICommandParser::LayoutFixtureRow(100, {}).Num(), StageAICommandParser::MaxFixturesPerCommand);
	TestEqual(TEXT("Count clamped low"), StageAICommandParser::LayoutFixtureRow(-3, {}).Num(), 1);

	// Answers from a scene snapshot.
	FStageAISceneView Scene;
	Scene.PlacedItems = 7;
	Scene.TotalPowerWatts = 4200.f;
	Scene.TotalWeightKg = 312.4f;
	Scene.MaxPowerWatts = 10000.f;
	Scene.ItemNames = { TEXT("Truss F34 (3 m)"), TEXT("Truss F34 (3 m)"), TEXT("Spot 350"), TEXT("Spot 350"), TEXT("Spot 350"), TEXT("Stage Deck"), TEXT("Speaker") };
	Scene.FixturesByName = { { TEXT("Spot 350"), 3 } };
	Scene.Cues = { { 1.f, TEXT("Walk-in"), 3.f }, { 2.5f, TEXT(""), 2.f } };
	Scene.ActiveCue = 2.5f;
	Scene.bHasEnvironment = true;
	Scene.bSkyAvailable = true;

	const auto Answer = [&Scene](EStageAIIntent Intent, const TCHAR* Query = TEXT(""))
	{
		FStageAICommand Command;
		Command.Intent = Intent;
		Command.ItemQuery = Query;
		return StageAIAnswers::AnswerQuestion(Command, Scene).ToString();
	};
	TestTrue(TEXT("Power in kW with the limit"), Answer(EStageAIIntent::PowerDraw).Contains(TEXT("4.2 kW")) && Answer(EStageAIIntent::PowerDraw).Contains(TEXT("10.0 kW")));
	TestTrue(TEXT("Weight in kg"), Answer(EStageAIIntent::TotalWeight).Contains(TEXT("312 kg")));
	TestTrue(TEXT("Item count"), Answer(EStageAIIntent::ItemCount).Contains(TEXT("7 items are placed")));
	TestTrue(TEXT("Fixtures by name"), Answer(EStageAIIntent::FixtureCount).Contains(TEXT("3 fixtures")) && Answer(EStageAIIntent::FixtureCount).Contains(TEXT("Spot 350")));
	TestTrue(TEXT("Count matching truss"), Answer(EStageAIIntent::CountMatching, TEXT("truss")).Contains(TEXT("2 placed items match")));
	TestTrue(TEXT("Count matching is word-based (plural query)"), Answer(EStageAIIntent::CountMatching, TEXT("spots")).Contains(TEXT("3 placed items match")));
	TestTrue(TEXT("No match"), Answer(EStageAIIntent::CountMatching, TEXT("laser")).Contains(TEXT("None of the 7")));
	const FString Cues = Answer(EStageAIIntent::ListCues);
	TestTrue(TEXT("Cue list"), Cues.Contains(TEXT("2 cues")) && Cues.Contains(TEXT("Q1 Walk-in (3 s)")) && Cues.Contains(TEXT("Q2.5 (2 s)")));
	TestTrue(TEXT("Active cue"), Cues.Contains(TEXT("Cue 2.5 is on stage")));
	TestTrue(TEXT("Indoor environment"), Answer(EStageAIIntent::EnvironmentStatus).Contains(TEXT("Indoor stage")));
	Scene.Environment.bSkyEnabled = true;
	Scene.Environment = StageEnvironment::ApplyWeatherPreset(Scene.Environment, EStageWeatherPreset::Rain);
	Scene.Environment.TimeOfDay = 18.5f;
	TestTrue(TEXT("Outdoor environment"), Answer(EStageAIIntent::EnvironmentStatus).Contains(TEXT("18:30")) && Answer(EStageAIIntent::EnvironmentStatus).Contains(TEXT("rain")));
	TestTrue(TEXT("Summary"), Answer(EStageAIIntent::StageSummary).Contains(TEXT("7 items (3 fixtures)")));

	Scene = FStageAISceneView();
	TestTrue(TEXT("Empty stage: no fixtures"), Answer(EStageAIIntent::FixtureCount).Contains(TEXT("No lighting fixtures")));
	TestTrue(TEXT("Empty stage: no cues"), Answer(EStageAIIntent::ListCues).Contains(TEXT("no cues")));
	TestTrue(TEXT("No environment"), Answer(EStageAIIntent::EnvironmentStatus).Contains(TEXT("no outdoor environment")));
	TestTrue(TEXT("No limit shown when unlimited"), !Answer(EStageAIIntent::PowerDraw).Contains(TEXT("venue")));

	TestEqual(TEXT("FormatHour"), StageAIAnswers::FormatHour(9.75f), FString(TEXT("09:45")));
	TestEqual(TEXT("FormatHour wraps"), StageAIAnswers::FormatHour(23.999f), FString(TEXT("00:00")));
	TestTrue(TEXT("Scene for the model"), StageAICommandSchema::DescribeScene(Scene).Contains(TEXT("Placed items: 0")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAIConversationTest, "StageCraft.AI.Conversation", StageAITests::Flags)

// The conversation against a real game world: questions answered without the executor, actions handed to it with validated
// commands, invalid commands never reaching it, a pending action blocking the next prompt until it finishes, and clearing.
bool FStageAIConversationTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("StageAITestWorld"));
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	World->InitializeActorsForPlay(FURL());
	World->BeginPlay();
	World->GetWorldSettings()->NotifyBeginPlay();
	ON_SCOPE_EXIT
	{
		World->BeginTearingDown();
		World->EndPlay(EEndPlayReason::Quit);
		World->DestroyWorld(/*bInformEngineOfWorld*/ false);
		GEngine->DestroyWorldContext(World);
	};

	UStageAICommandSubsystem* Assistant = UWorld::GetSubsystem<UStageAICommandSubsystem>(World);
	if (!TestNotNull(TEXT("Assistant subsystem in a game world"), Assistant))
	{
		return false;
	}

	TArray<FStageAICommand> Executed;
	EStageAIOutcome NextOutcome = EStageAIOutcome::Succeeded;
	const auto MakeExecutor = [&Executed, &NextOutcome]()
	{
		return FStageAICommandExecutor::CreateLambda([&Executed, &NextOutcome](const FStageAICommand& Command)
		{
			Executed.Add(Command);
			return FStageAIActionOutcome::Make(NextOutcome, FText::FromString(TEXT("ran")));
		});
	};
	const auto LastLine = [Assistant]() { return Assistant->GetMessages().IsEmpty() ? FStageAIMessage() : Assistant->GetMessages().Last(); };

	TestFalse(TEXT("Empty prompt refused"), Assistant->SubmitPrompt(TEXT("   "), MakeExecutor()));
	TestEqual(TEXT("... nothing added"), Assistant->GetMessages().Num(), 0);

	// A question: answered from the world, no executor call.
	TestTrue(TEXT("Question accepted"), Assistant->SubmitPrompt(TEXT("how many fixtures?"), MakeExecutor()));
	TestEqual(TEXT("... user and assistant lines"), Assistant->GetMessages().Num(), 2);
	TestEqual(TEXT("... executor not called"), Executed.Num(), 0);
	TestTrue(TEXT("... answered"), LastLine().Text.ToString().Contains(TEXT("No lighting fixtures")));
	TestFalse(TEXT("... idle"), Assistant->IsBusy());

	// Not understood, help and unsupported: replies only.
	Assistant->SubmitPrompt(TEXT("purple elephant"), MakeExecutor());
	TestTrue(TEXT("Not understood"), LastLine().Text.ToString().Contains(TEXT("didn't understand")));
	Assistant->SubmitPrompt(TEXT("clear the stage"), MakeExecutor());
	TestTrue(TEXT("Unsupported explained"), LastLine().Text.ToString().Contains(TEXT("Clear Stage")));
	TestEqual(TEXT("... executor still not called"), Executed.Num(), 0);

	// An action: validated, then handed to the executor.
	TestTrue(TEXT("Action accepted"), Assistant->SubmitPrompt(TEXT("add 3 lights"), MakeExecutor()));
	TestEqual(TEXT("... executed once"), Executed.Num(), 1);
	TestTrue(TEXT("... with the parsed command"), Executed.Num() == 1 && Executed[0].Intent == EStageAIIntent::AddFixtures && Executed[0].Count.Get(0) == 3);
	TestTrue(TEXT("... result line"), LastLine().Outcome == EStageAIOutcome::Succeeded);

	// An invalid action never reaches the executor.
	Assistant->SubmitPrompt(TEXT("add 500 lights"), MakeExecutor());
	TestEqual(TEXT("Invalid action not executed"), Executed.Num(), 1);
	TestTrue(TEXT("... refused with the reason"), LastLine().Outcome == EStageAIOutcome::Refused && LastLine().Text.ToString().Contains(TEXT("1 to 24")));

	// No player: an action is refused, not run.
	Assistant->SubmitPrompt(TEXT("make it rain"), FStageAICommandExecutor());
	TestTrue(TEXT("Unbound executor refused"), LastLine().Outcome == EStageAIOutcome::Refused);

	// A pending action keeps the assistant busy until it finishes.
	NextOutcome = EStageAIOutcome::Pending;
	TestTrue(TEXT("Pending action accepted"), Assistant->SubmitPrompt(TEXT("add 2 lights"), MakeExecutor()));
	TestTrue(TEXT("... busy"), Assistant->GetState() == EStageAIAssistantState::Acting);
	TestTrue(TEXT("... pending line"), LastLine().Outcome == EStageAIOutcome::Pending);
	TestFalse(TEXT("Second prompt refused while busy"), Assistant->SubmitPrompt(TEXT("status"), MakeExecutor()));
	Assistant->ClearConversation();
	TestTrue(TEXT("Clear ignored while busy"), Assistant->GetMessages().Num() > 0);
	Assistant->FinishPendingAction(FStageAIActionOutcome::Make(EStageAIOutcome::Succeeded, FText::FromString(TEXT("placed"))));
	TestFalse(TEXT("Finished: idle"), Assistant->IsBusy());
	TestEqual(TEXT("... result line"), LastLine().Text.ToString(), FString(TEXT("placed")));
	const int32 Lines = Assistant->GetMessages().Num();
	Assistant->FinishPendingAction(FStageAIActionOutcome::Make(EStageAIOutcome::Succeeded, FText::FromString(TEXT("twice"))));
	TestEqual(TEXT("A second finish is ignored"), Assistant->GetMessages().Num(), Lines);

	Assistant->ClearConversation();
	TestEqual(TEXT("Cleared"), Assistant->GetMessages().Num(), 0);

	// The transcript is capped.
	NextOutcome = EStageAIOutcome::Succeeded;
	for (int32 Index = 0; Index < UStageAICommandSubsystem::MaxMessages; ++Index)
	{
		Assistant->SubmitPrompt(TEXT("help"), MakeExecutor());
	}
	TestEqual(TEXT("Capped"), Assistant->GetMessages().Num(), UStageAICommandSubsystem::MaxMessages);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
