// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for the Armenian assistant (Docs/ADR/0017-multilingual-voice-and-photo-import.md §2.1-2.3): script detection, the offline
// Armenian reading of prompts (StageAIArmenian), replies in the prompt's language, and reply_language at the online trust boundary.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.AI; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AI/StageAIArmenian.h"
#include "AI/StageAICommandParser.h"
#include "AI/StageAICommandSchema.h"
#include "AI/StageAILanguage.h"
#include "Misc/AutomationTest.h"

namespace StageAIArmenianTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	FStageAICommand ParseOrDefault(const TCHAR* Text)
	{
		return StageAICommandParser::Parse(Text).Get(FStageAICommand());
	}

	FString IntentOf(const TCHAR* Text)
	{
		const TOptional<FStageAICommand> Command = StageAICommandParser::Parse(Text);
		return Command.IsSet() ? StageAICommandSchema::GetIntentName(Command->Intent) : FString(TEXT("none"));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAIArmenianTest, "StageCraft.AI.Armenian", StageAIArmenianTests::Flags)

bool FStageAIArmenianTest::RunTest(const FString& Parameters)
{
	using namespace StageAIArmenianTests;

	// --- Script detection ---
	TestTrue(TEXT("Detect: English"), StageAILanguage::Detect(TEXT("add 4 moving heads")) == EStageAILanguage::English);
	TestTrue(TEXT("Detect: Armenian"), StageAILanguage::Detect(TEXT("ավելացրու 4 շարժական գլուխ")) == EStageAILanguage::Armenian);
	TestTrue(TEXT("Detect: mostly Armenian with a Latin name"), StageAILanguage::Detect(TEXT("Ավելացրու 2 Spot 350")) == EStageAILanguage::Armenian);
	TestTrue(TEXT("Detect: one Armenian word in an English prompt"), StageAILanguage::Detect(TEXT("add 4 wash fixtures to the left side please, թող")) == EStageAILanguage::English);
	TestTrue(TEXT("Detect: empty and digits only"), StageAILanguage::Detect(TEXT("")) == EStageAILanguage::English && StageAILanguage::Detect(TEXT("18:30")) == EStageAILanguage::English);
	TestEqual(TEXT("Lowercase: capital Ayb"), StageAILanguage::ToLowerArmenian(TCHAR(0x0531)), TCHAR(0x0561));
	TestEqual(TEXT("Lowercase: other characters unchanged"), StageAILanguage::ToLowerArmenian(TEXT('A')), TEXT('A'));
	TestEqual(TEXT("Codes"), FString(StageAILanguage::GetCode(EStageAILanguage::Armenian)), FString(TEXT("hy")));
	TestTrue(TEXT("Codes: unknown"), !StageAILanguage::FromCode(TEXT("fr")).IsSet() && StageAILanguage::FromCode(TEXT("HY")) == EStageAILanguage::Armenian);
	TestEqual(TEXT("Translation leaves English unchanged"), StageAIArmenian::TranslateToGrammar(TEXT("add 4 pars")), FString(TEXT("add 4 pars")));

	// --- Fixtures ---
	{
		const FStageAICommand Add = ParseOrDefault(TEXT("Ավելացրու 6 շարժական գլուխ"));
		TestEqual(TEXT("Add: intent"), StageAICommandSchema::GetIntentName(Add.Intent), FString(TEXT("add_fixtures")));
		TestEqual(TEXT("Add: count"), Add.Count.Get(0), 6);
		TestEqual(TEXT("Add: query"), Add.ItemQuery, FString(TEXT("moving head")));
		TestTrue(TEXT("Add: replies in Armenian"), Add.Language == EStageAILanguage::Armenian);
	}
	{
		const FStageAICommand Add = ParseOrDefault(TEXT("ավելացրու երկու վոշ"));
		TestTrue(TEXT("Add: number word and slang"), Add.Count.Get(0) == 2 && Add.ItemQuery == TEXT("wash"));
	}
	{
		const FStageAICommand Add = ParseOrDefault(TEXT("տեղադրիր մեկ LED փար"));
		TestTrue(TEXT("Add: Latin brand words kept"), Add.Count.Get(0) == 1 && Add.ItemQuery == TEXT("led par"));
	}
	{
		const FStageAICommand Add = ParseOrDefault(TEXT("ավելացրու մի քանի լույս"));
		TestTrue(TEXT("Add: a few"), Add.Count.Get(0) == 3 && Add.ItemQuery == TEXT("light"));
	}
	{
		const FStageAICommand Add = ParseOrDefault(TEXT("խնդրում եմ, ավելացրու տասներկու սփոթ"));
		TestTrue(TEXT("Add: polite filler and twelve"), Add.Intent == EStageAIIntent::AddFixtures && Add.Count.Get(0) == 12 && Add.ItemQuery == TEXT("spot"));
	}
	{
		// An unknown fixture word must not fall back to the first fixture in the catalog.
		const FStageAICommand Add = ParseOrDefault(TEXT("ավելացրու 2 վիշապ"));
		TestTrue(TEXT("Add: unknown word kept"), Add.Intent == EStageAIIntent::AddFixtures && Add.ItemQuery == TEXT("վիշապ"));
		const FStageAIFixtureCandidate Candidates[] = { { TEXT("Test Moving Head Spot"), TEXT("Moving Head (Spot)") }, { TEXT("LED Par"), TEXT("Par") } };
		TestEqual(TEXT("Add: unknown word matches nothing"), StageAICommandParser::MatchFixture(Candidates, Add.ItemQuery), INDEX_NONE);
		TestEqual(TEXT("Add: Armenian moving head matches"), StageAICommandParser::MatchFixture(Candidates, ParseOrDefault(TEXT("ավելացրու 2 շարժական գլուխ")).ItemQuery), 0);
	}
	{
		FStageAICommand TooMany = ParseOrDefault(TEXT("ավելացրու 500 լույս"));
		FText Error;
		TestFalse(TEXT("Add: 500 refused"), StageAICommandSchema::Validate(TooMany, Error));
		TestTrue(TEXT("Add: refusal in Armenian"), Error.ToString().Contains(TEXT("սարք")));
	}

	// --- Questions ---
	TestEqual(TEXT("Q: fixtures"), IntentOf(TEXT("քանի՞ սարք կա")), FString(TEXT("fixture_count")));
	TestEqual(TEXT("Q: lights"), IntentOf(TEXT("Քանի՞ լույս ունենք")), FString(TEXT("fixture_count")));
	{
		const FStageAICommand Truss = ParseOrDefault(TEXT("Քանի՞ ֆերմա կա բեմում"));
		TestTrue(TEXT("Q: count truss"), Truss.Intent == EStageAIIntent::CountMatching && Truss.ItemQuery == TEXT("truss"));
	}
	TestEqual(TEXT("Q: weight"), IntentOf(TEXT("ընդհանուր քաշը")), FString(TEXT("total_weight")));
	TestEqual(TEXT("Q: how heavy"), IntentOf(TEXT("որքա՞ն է ծանր բեմը")), FString(TEXT("total_weight")));
	TestEqual(TEXT("Q: power"), IntentOf(TEXT("որքա՞ն է հզորությունը")), FString(TEXT("power_draw")));
	TestEqual(TEXT("Q: status"), IntentOf(TEXT("վիճակը")), FString(TEXT("stage_summary")));
	TestEqual(TEXT("Q: items"), IntentOf(TEXT("քանի՞ իր է տեղադրված")), FString(TEXT("item_count")));
	TestEqual(TEXT("Q: cues"), IntentOf(TEXT("կյուների ցանկը")), FString(TEXT("list_cues")));
	TestEqual(TEXT("Q: weather"), IntentOf(TEXT("եղանակը ինչպիսի՞ն է")), FString(TEXT("environment_status")));
	TestEqual(TEXT("Q: time"), IntentOf(TEXT("ժամը քանի՞սն է")), FString(TEXT("environment_status")));

	// --- Cues ---
	{
		const FStageAICommand Store = ParseOrDefault(TEXT("պահիր կյու 5 «Ներածություն» 3 վայրկյան"));
		TestEqual(TEXT("Cue: intent"), StageAICommandSchema::GetIntentName(Store.Intent), FString(TEXT("store_cue")));
		TestTrue(TEXT("Cue: number"), Store.CueNumber.IsSet() && FMath::IsNearlyEqual(*Store.CueNumber, 5.f));
		TestEqual(TEXT("Cue: label as typed"), Store.Label, FString(TEXT("Ներածություն")));
		TestTrue(TEXT("Cue: fade"), Store.FadeSeconds.IsSet() && FMath::IsNearlyEqual(*Store.FadeSeconds, 3.f));
	}
	{
		const FStageAICommand Store = ParseOrDefault(TEXT("պահիր երրորդ կյուն"));
		TestTrue(TEXT("Cue: ordinal before the noun"), Store.Intent == EStageAIIntent::StoreCue && Store.CueNumber.IsSet() && FMath::IsNearlyEqual(*Store.CueNumber, 3.f));
	}
	{
		const FStageAICommand Store = ParseOrDefault(TEXT("պահիր կյու 7-րդ"));
		TestTrue(TEXT("Cue: number with an ending"), Store.CueNumber.IsSet() && FMath::IsNearlyEqual(*Store.CueNumber, 7.f));
	}
	{
		const FStageAICommand Go = ParseOrDefault(TEXT("գնա կյու 2"));
		TestTrue(TEXT("Cue: go to 2"), Go.Intent == EStageAIIntent::GoToCue && Go.CueNumber.IsSet() && FMath::IsNearlyEqual(*Go.CueNumber, 2.f));
		const FStageAICommand On = ParseOrDefault(TEXT("միացրու կյու 4"));
		TestTrue(TEXT("Cue: turn on cue 4"), On.Intent == EStageAIIntent::GoToCue && On.CueNumber.IsSet() && FMath::IsNearlyEqual(*On.CueNumber, 4.f));
	}
	TestEqual(TEXT("Cue: go"), IntentOf(TEXT("գնա")), FString(TEXT("cue_go")));
	TestEqual(TEXT("Cue: next"), IntentOf(TEXT("հաջորդ կյուն")), FString(TEXT("cue_go")));
	TestEqual(TEXT("Cue: back"), IntentOf(TEXT("հետ")), FString(TEXT("cue_back")));
	TestEqual(TEXT("Cue: previous"), IntentOf(TEXT("նախորդ կյուն")), FString(TEXT("cue_back")));

	// --- Environment ---
	{
		const FStageAICommand Rain = ParseOrDefault(TEXT("թող անձրև գա"));
		TestTrue(TEXT("Env: rain"), Rain.Intent == EStageAIIntent::SetWeather && Rain.Weather == EStageWeatherPreset::Rain);
		TestTrue(TEXT("Env: stop the rain"), ParseOrDefault(TEXT("դադարեցրու անձրևը")).Weather == EStageWeatherPreset::Clear);
		TestTrue(TEXT("Env: fog"), ParseOrDefault(TEXT("մառախուղ")).Weather == EStageWeatherPreset::Fog);
		TestTrue(TEXT("Env: snow"), ParseOrDefault(TEXT("թող ձյուն գա")).Weather == EStageWeatherPreset::Snow);
		TestTrue(TEXT("Env: cloudy"), ParseOrDefault(TEXT("ամպամած եղանակ")).Weather == EStageWeatherPreset::Cloudy);
	}
	{
		const auto HourOf = [](const TCHAR* Text) { const FStageAICommand Command = StageAIArmenianTests::ParseOrDefault(Text);
			return Command.Intent == EStageAIIntent::SetTimeOfDay ? Command.Hour : -1.f; };
		TestEqual(TEXT("Env: 18:30"), HourOf(TEXT("ժամը 18:30")), 18.5f);
		TestEqual(TEXT("Env: 18:30 with an ending"), HourOf(TEXT("ժամը 18:30-ին")), 18.5f);
		TestEqual(TEXT("Env: 8 in the evening"), HourOf(TEXT("ժամը 8 երեկոյան")), 20.f);
		TestEqual(TEXT("Env: evening at 8"), HourOf(TEXT("երեկոյան 8-ին")), 20.f);
		TestEqual(TEXT("Env: 9 in the morning"), HourOf(TEXT("առավոտյան 9-ին")), 9.f);
		TestEqual(TEXT("Env: sunset"), HourOf(TEXT("մայրամուտ")), 19.5f);
		TestEqual(TEXT("Env: noon"), HourOf(TEXT("կեսօր")), 12.f);
	}
	{
		const FStageAICommand Outdoors = ParseOrDefault(TEXT("բացօթյա"));
		TestTrue(TEXT("Env: outdoors"), Outdoors.Intent == EStageAIIntent::SetSky && Outdoors.bSkyOn);
		const FStageAICommand SkyOn = ParseOrDefault(TEXT("միացրու երկինքը"));
		TestTrue(TEXT("Env: sky on"), SkyOn.Intent == EStageAIIntent::SetSky && SkyOn.bSkyOn);
		const FStageAICommand SkyOff = ParseOrDefault(TEXT("անջատիր երկինքը"));
		TestTrue(TEXT("Env: sky off"), SkyOff.Intent == EStageAIIntent::SetSky && !SkyOff.bSkyOn);
	}

	// --- Reports, history, unsupported, help ---
	TestTrue(TEXT("Export: BOM"), ParseOrDefault(TEXT("արտահանիր նյութերի ցուցակը")).Report == EStageAIReport::BillOfMaterials);
	TestTrue(TEXT("Export: patch"), ParseOrDefault(TEXT("արտահանիր պատչը")).Report == EStageAIReport::PatchSheet);
	TestTrue(TEXT("Export: cables"), ParseOrDefault(TEXT("արտահանիր մալուխների ցուցակը")).Report == EStageAIReport::CableSchedule);
	{
		const FStageAICommand All = ParseOrDefault(TEXT("արտահանիր բոլոր հաշվետվությունները"));
		TestTrue(TEXT("Export: all"), All.Intent == EStageAIIntent::Export && All.Report == EStageAIReport::All);
	}
	TestEqual(TEXT("History: undo"), IntentOf(TEXT("հետարկիր")), FString(TEXT("undo")));
	TestEqual(TEXT("History: polite undo"), IntentOf(TEXT("խնդրում եմ հետարկիր")), FString(TEXT("undo")));
	TestEqual(TEXT("History: redo"), IntentOf(TEXT("կրկնիր")), FString(TEXT("redo")));
	TestTrue(TEXT("Unsupported: delete"), ParseOrDefault(TEXT("ջնջիր ամեն ինչ")).Intent == EStageAIIntent::Unsupported && ParseOrDefault(TEXT("ջնջիր ամեն ինչ")).ItemQuery == TEXT("delete"));
	TestEqual(TEXT("Unsupported: open"), ParseOrDefault(TEXT("բացիր տեսարանը")).ItemQuery, FString(TEXT("open")));
	TestEqual(TEXT("Unsupported: save"), ParseOrDefault(TEXT("պահպանիր տեսարանը")).ItemQuery, FString(TEXT("save")));
	TestEqual(TEXT("Help"), IntentOf(TEXT("օգնություն")), FString(TEXT("help")));
	TestEqual(TEXT("Help: hello"), IntentOf(TEXT("բարև")), FString(TEXT("help")));
	TestEqual(TEXT("Help: what can you do"), IntentOf(TEXT("ինչ կարող ես անել")), FString(TEXT("help")));
	TestEqual(TEXT("Nonsense"), IntentOf(TEXT("կապույտ փղեր")), FString(TEXT("none")));

	// Words that begin like a command word but are not one (found in review).
	{
		const FStageAICommand Want = ParseOrDefault(TEXT("ցանկանում եմ ավելացնել 4 սփոթ"));
		TestTrue(TEXT("Stems: 'I want to add' is not 'list'"), Want.Intent == EStageAIIntent::AddFixtures && Want.Count.Get(0) == 4 && Want.ItemQuery == TEXT("spot"));
		const FStageAICommand Double = ParseOrDefault(TEXT("ավելացրու կրկնակի ֆերմա"));
		TestTrue(TEXT("Stems: 'double' is not 'redo'"), Double.Intent == EStageAIIntent::AddFixtures);
		TestEqual(TEXT("Stems: the list is still a list"), IntentOf(TEXT("կյուների ցանկը")), FString(TEXT("list_cues")));
		TestEqual(TEXT("Negation: 'don't add a light' runs nothing"), IntentOf(TEXT("մի՛ ավելացրու լույս")), FString(TEXT("none")));
		TestEqual(TEXT("Negation: plain 'a' still counts one"), ParseOrDefault(TEXT("ավելացրու մի լույս")).Count.Get(0), 1);
		const FStageAICommand Evening = ParseOrDefault(TEXT("ավելացրու 4 սփոթ երեկոյան"));
		TestTrue(TEXT("Part of day: a fixture count is not an hour"), Evening.Intent == EStageAIIntent::AddFixtures && Evening.Count.Get(0) == 4);
	}

	// Every example in the Armenian help text is understood.
	{
		const FString Help = StageAIAnswers::DescribeHelp(EStageAILanguage::Armenian).ToString();
		TArray<FString> Lines;
		Help.ParseIntoArrayLines(Lines);
		int32 Examples = 0;
		for (const FString& Line : Lines)
		{
			if (!Line.StartsWith(TEXT("•")))
			{
				continue;
			}
			TArray<FString> Parts;
			Line.RightChop(1).ParseIntoArray(Parts, TEXT("·"));
			for (const FString& Part : Parts)
			{
				const FString Example = Part.TrimStartAndEnd();
				const TOptional<FStageAICommand> Command = StageAICommandParser::Parse(Example);
				TestTrue(FString::Printf(TEXT("Help example understood: %s"), *Example), Command.IsSet() && Command->Language == EStageAILanguage::Armenian);
				++Examples;
			}
		}
		TestEqual(TEXT("Help examples counted"), Examples, 40);
	}

	// --- Replies follow the prompt ---
	{
		FStageAISceneView Scene;
		Scene.PlacedItems = 6;
		Scene.TotalWeightKg = 212.f;
		Scene.TotalPowerWatts = 2820.f;
		FStageAICommand Weight;
		Weight.Intent = EStageAIIntent::TotalWeight;
		TestTrue(TEXT("Reply: English"), StageAIAnswers::AnswerQuestion(Weight, Scene).ToString().StartsWith(TEXT("Total weight: 212 kg")));
		Weight.Language = EStageAILanguage::Armenian;
		const FString Armenian = StageAIAnswers::AnswerQuestion(Weight, Scene).ToString();
		TestEqual(TEXT("Reply: Armenian weight"), Armenian, FString(TEXT("Ընդհանուր քաշը՝ 212 կգ (6 իր)։")));
		FStageAICommand Power;
		Power.Intent = EStageAIIntent::PowerDraw;
		Power.Language = EStageAILanguage::Armenian;
		TestTrue(TEXT("Reply: Armenian power in kW"), StageAIAnswers::AnswerQuestion(Power, Scene).ToString().StartsWith(TEXT("Ընդհանուր հզորությունը՝ 2.8 կՎտ")));
		FStageAICommand Summary;
		Summary.Intent = EStageAIIntent::StageSummary;
		Summary.Language = EStageAILanguage::Armenian;
		TestTrue(TEXT("Reply: Armenian summary"), StageAIAnswers::AnswerQuestion(Summary, Scene).ToString().StartsWith(TEXT("Բեմում կա 6 իր (0 լուսային սարք)")));
	}
	TestTrue(TEXT("Not understood: Armenian"), StageAIAnswers::DescribeNotUnderstood(TEXT("կապույտ փղեր")).ToString().Contains(TEXT("չհասկացա")));
	TestTrue(TEXT("Not understood: English"), StageAIAnswers::DescribeNotUnderstood(TEXT("blue elephants")).ToString().StartsWith(TEXT("Sorry")));
	TestTrue(TEXT("Unsupported: Armenian"), StageAIAnswers::DescribeUnsupported(TEXT("delete"), EStageAILanguage::Armenian).ToString().Contains(TEXT("Clear Stage")));
	TestEqual(TEXT("Weather name: Armenian"), StageAIAnswers::DescribeWeather(EStageWeatherPreset::Rain, EStageAILanguage::Armenian).ToString(), FString(TEXT("անձրև")));

	// --- reply_language at the online trust boundary ---
	{
		FStageAICommand Command;
		FText Error;
		TestTrue(TEXT("Tool: reply_language hy"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"total_weight\",\"reply_language\":\"hy\"}"), Command, Error)
			&& Command.Language == EStageAILanguage::Armenian);
		TestTrue(TEXT("Tool: reply_language en overrides the prompt"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"total_weight\",\"reply_language\":\"en\"}"),
			Command, Error, EStageAILanguage::Armenian) && Command.Language == EStageAILanguage::English);
		TestTrue(TEXT("Tool: no reply_language uses the prompt's"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"help\"}"), Command, Error, EStageAILanguage::Armenian)
			&& Command.Language == EStageAILanguage::Armenian);
		TestFalse(TEXT("Tool: unknown reply_language refused"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"help\",\"reply_language\":\"fr\"}"), Command, Error));
		TestFalse(TEXT("Tool: Armenian count refused in Armenian"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"add_fixtures\",\"count\":99,\"reply_language\":\"hy\"}"), Command, Error));
		TestTrue(TEXT("Tool: refusal text"), Error.ToString().Contains(TEXT("սարք")));

		const FStageAIModelResponse Response = StageAICommandSchema::ParseResponseBody(200,
			TEXT("{\"stop_reason\":\"tool_use\",\"content\":[{\"type\":\"tool_use\",\"id\":\"t\",\"name\":\"stage_command\",\"input\":{\"intent\":\"power_draw\"}}]}"), EStageAILanguage::Armenian);
		TestTrue(TEXT("Response: prompt language kept"), Response.Status == FStageAIModelResponse::EStatus::Command && Response.Command.Language == EStageAILanguage::Armenian);
		TestTrue(TEXT("Tool definition lists reply_language"), StageAICommandSchema::MakeToolDefinitionJson().Contains(TEXT("\"reply_language\"")));
		TestTrue(TEXT("System prompt mentions Armenian"), StageAICommandSchema::GetSystemPrompt().Contains(TEXT("Armenian")));
	}

	// English prompts are untouched.
	TestTrue(TEXT("English stays English"), ParseOrDefault(TEXT("add 4 moving heads")).Language == EStageAILanguage::English);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
