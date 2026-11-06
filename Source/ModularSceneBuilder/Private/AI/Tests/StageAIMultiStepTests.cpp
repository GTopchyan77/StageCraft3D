// Copyright Epic Games, Inc. All Rights Reserved.

// Phase H4 (Docs/ADR/0030 §6, §17): several changes in one typed prompt as one plan, the grammar of the selection, palette, group
// and effect tools, fixture targets by ID and group, "them" and "again", name lookup in the pools, and the online schema of the
// new tools. Pure: no world. The subsystem side (memory, plans of typed prompts) is StageCraft.Assistant.Memory.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Assistant; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AI/StageAICommandParser.h"
#include "AI/StageAICommandSchema.h"
#include "AI/StageAIToolCatalogue.h"
#include "Misc/AutomationTest.h"
#include "Show/StageEffectTypes.h"

namespace StageAIMultiStepTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	FStageAICommand Parsed(const TCHAR* Prompt)
	{
		return StageAICommandParser::Parse(Prompt).Get(FStageAICommand());
	}

	FString Ids(const FStageAICommand& Command)
	{
		return FString::JoinBy(Command.FixtureIds, TEXT(","), [](int32 Id) { return FString::FromInt(Id); });
	}

	/** The intents of a plan, by wire name ("add_fixtures store_cue"). */
	FString Intents(TConstArrayView<FStageAICommand> Steps)
	{
		return FString::JoinBy(Steps, TEXT(" "), [](const FStageAICommand& Step) { return StageAICommandSchema::GetIntentName(Step.Intent); });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAssistantMultiStepTest, "StageCraft.Assistant.MultiStep", StageAIMultiStepTests::Flags)

bool FStageAssistantMultiStepTest::RunTest(const FString& Parameters)
{
	using StageAICommandParser::ParsePlan;
	using StageAIMultiStepTests::Intents;

	TArray<FStageAICommand> Steps = ParsePlan(TEXT("add 4 pars and store cue 1"));
	TestEqual(TEXT("Add and store"), Intents(Steps), FString(TEXT("add_fixtures store_cue")));
	if (Steps.Num() == 2)
	{
		TestTrue(TEXT("... 4 pars"), Steps[0].Count.Get(0) == 4 && Steps[0].ItemQuery.Contains(TEXT("par")));
		TestTrue(TEXT("... cue 1"), FMath::IsNearlyEqual(Steps[1].CueNumber.Get(0.f), 1.f));
	}
	TestEqual(TEXT("Comma and then"), Intents(ParsePlan(TEXT("store cue 2, then go to cue 2"))), FString(TEXT("store_cue go_to_cue")));
	TestEqual(TEXT("Three steps with a semicolon"), Intents(ParsePlan(TEXT("make it rain; set the time to 18:30 and store cue 4"))),
		FString(TEXT("set_weather set_time_of_day store_cue")));

	// One request across "and" stays one.
	TestTrue(TEXT("Controls of the same fixtures are one step"), ParsePlan(TEXT("set the spots to red and zoom wide")).IsEmpty());
	const FStageAICommand Both = StageAIMultiStepTests::Parsed(TEXT("set the spots to red and zoom wide"));
	TestTrue(TEXT("... read whole"), Both.Intent == EStageAIIntent::SetFixture && Both.FixtureSettings.Num() == 2);
	TestTrue(TEXT("Two targets of one delete are one step"), ParsePlan(TEXT("delete the spots and the washes")).IsEmpty());
	TestTrue(TEXT("A question is no step"), ParsePlan(TEXT("store cue 1 and how many fixtures are there?")).IsEmpty());
	TestTrue(TEXT("Undo is never part of a plan"), ParsePlan(TEXT("undo and redo")).IsEmpty());
	TestTrue(TEXT("Quotes are never split"), ParsePlan(TEXT("store cue 3 called \"rock and roll\"")).IsEmpty());
	TestTrue(TEXT("One change is no plan"), ParsePlan(TEXT("store cue 3")).IsEmpty());
	TestTrue(TEXT("Decimals are not sentence ends"), ParsePlan(TEXT("patch fixtures 1 to 4 at 2.001")).IsEmpty());

	// Each light in turn: "them" is resolved when the step runs.
	Steps = ParsePlan(TEXT("add 4 pars and make them blue"));
	TestTrue(TEXT("Add, then them"), Steps.Num() == 2 && Steps[1].Intent == EStageAIIntent::SetFixture && Steps[1].Target == EStageAITarget::Recent);
	Steps = ParsePlan(TEXT("select group 2 and set dimmer to 50%"));
	TestTrue(TEXT("A step without a target goes on with the step before"), Steps.Num() == 2 && Steps[0].Intent == EStageAIIntent::SelectFixtures
		&& Steps[0].Target == EStageAITarget::Group && Steps[0].Group == TEXT("2") && Steps[1].Target == EStageAITarget::Recent);
	Steps = ParsePlan(TEXT("store cue 1 and set dimmer to 50%"));
	TestTrue(TEXT("... but not after a cue"), Steps.Num() == 2 && Steps[1].Target == EStageAITarget::Selection);
	Steps = ParsePlan(TEXT("make the spots red and turn the washes off"));
	TestTrue(TEXT("Two named targets are two steps"), Steps.Num() == 2 && Steps[0].ItemQuery.Contains(TEXT("spot")) && Steps[1].ItemQuery.Contains(TEXT("wash")));

	// Armenian.
	Steps = ParsePlan(TEXT("ավելացրու 4 փար և պահիր կյու 1"));
	TestEqual(TEXT("HY add and store"), Intents(Steps), FString(TEXT("add_fixtures store_cue")));
	TestTrue(TEXT("... answered in Armenian"), Steps.Num() == 2 && Steps[0].Language == EStageAILanguage::Armenian && Steps[1].Language == EStageAILanguage::Armenian);
	Steps = ParsePlan(TEXT("ընտրիր 2 խումբը, հետո դրանք դարձրու կարմիր"));
	TestTrue(TEXT("HY select a group, then them red"), Steps.Num() == 2 && Steps[0].Intent == EStageAIIntent::SelectFixtures && Steps[0].Group == TEXT("2")
		&& Steps[1].Intent == EStageAIIntent::SetFixture && Steps[1].Target == EStageAITarget::Recent);
	TestEqual(TEXT("HY full stop"), Intents(ParsePlan(TEXT("պահիր կյու 2։ գնա կյու 2"))), FString(TEXT("store_cue go_to_cue")));

	// Bounds.
	FString Many;
	for (int32 Index = 0; Index <= StageAICommandParser::MaxPlanClauses; ++Index)
	{
		Many += TEXT("go, ");
	}
	TestTrue(TEXT("Past the clause cap nothing is split"), ParsePlan(Many).IsEmpty());
	TestTrue(TEXT("Empty"), ParsePlan(FString()).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAssistantToolGrammarTest, "StageCraft.Assistant.ToolGrammar", StageAIMultiStepTests::Flags)

bool FStageAssistantToolGrammarTest::RunTest(const FString& Parameters)
{
	using StageAIMultiStepTests::Parsed;
	using StageAIMultiStepTests::Ids;

	// Selection and fixture targets.
	FStageAICommand Command = Parsed(TEXT("select fixtures 1 to 4"));
	TestTrue(TEXT("Select by ID"), Command.Intent == EStageAIIntent::SelectFixtures && Command.Target == EStageAITarget::Fixtures && Ids(Command) == TEXT("1,2,3,4"));
	Command = Parsed(TEXT("select group front wash"));
	TestTrue(TEXT("Select a group by name"), Command.Target == EStageAITarget::Group && Command.Group == TEXT("front wash"));
	Command = Parsed(TEXT("select all"));
	TestTrue(TEXT("Select all: every fixture"), Command.Intent == EStageAIIntent::SelectFixtures && Command.Target == EStageAITarget::Named && Command.ItemQuery == TEXT("fixtures"));
	Command = Parsed(TEXT("select the spots"));
	TestTrue(TEXT("Select by name"), Command.Target == EStageAITarget::Named && Command.ItemQuery == TEXT("spots"));
	Command = Parsed(TEXT("set fixture 3 to 50%"));
	TestTrue(TEXT("A fixture by ID, its dimmer not a range"), Command.Intent == EStageAIIntent::SetFixture && Command.Target == EStageAITarget::Fixtures && Ids(Command) == TEXT("3")
		&& Command.FixtureSettings.Num() == 1 && FMath::IsNearlyEqual(Command.FixtureSettings[0].Value.Get(0.0), 0.5));
	Command = Parsed(TEXT("make group 2 red"));
	TestTrue(TEXT("A group's colour"), Command.Intent == EStageAIIntent::SetFixture && Command.Target == EStageAITarget::Group && Command.Group == TEXT("2"));
	Command = Parsed(TEXT("make them red"));
	TestTrue(TEXT("Them"), Command.Intent == EStageAIIntent::SetFixture && Command.Target == EStageAITarget::Recent);
	Command = Parsed(TEXT("make the selected red"));
	TestTrue(TEXT("The selected stay the selection"), Command.Target == EStageAITarget::Selection);
	Command = Parsed(TEXT("make 4 spots red"));
	TestTrue(TEXT("A count is not an ID"), Command.Target == EStageAITarget::Named && Command.Count.Get(0) == 4);

	// Palettes.
	Command = Parsed(TEXT("apply palette 3"));
	TestTrue(TEXT("Apply by number"), Command.Intent == EStageAIIntent::ApplyPalette && Command.Palette == TEXT("3") && Command.Target == EStageAITarget::Selection);
	Command = Parsed(TEXT("apply the warm palette to group 2"));
	TestTrue(TEXT("Apply by name to a group"), Command.Palette == TEXT("warm") && Command.Target == EStageAITarget::Group && Command.Group == TEXT("2"));
	Command = Parsed(TEXT("use palette red on them"));
	TestTrue(TEXT("Apply to them"), Command.Intent == EStageAIIntent::ApplyPalette && Command.Palette == TEXT("red") && Command.Target == EStageAITarget::Recent);
	Command = Parsed(TEXT("record a colour palette called Sunset"));
	TestTrue(TEXT("Record a colour palette"), Command.Intent == EStageAIIntent::RecordPalette && Command.PaletteKind == EStagePaletteKind::Color
		&& Command.Label == TEXT("Sunset") && Command.Target == EStageAITarget::Selection);
	Command = Parsed(TEXT("store a position palette from fixtures 1 to 4"));
	TestTrue(TEXT("Record from fixtures by ID"), Command.PaletteKind == EStagePaletteKind::Position && Ids(Command) == TEXT("1,2,3,4"));
	Command = Parsed(TEXT("delete palette 2"));
	TestTrue(TEXT("Delete a palette"), Command.Intent == EStageAIIntent::DeletePalette && Command.Palette == TEXT("2"));

	// Groups.
	Command = Parsed(TEXT("record a group called Front"));
	TestTrue(TEXT("Record a group"), Command.Intent == EStageAIIntent::RecordGroup && Command.Label == TEXT("Front") && Command.Target == EStageAITarget::Selection);
	Command = Parsed(TEXT("store the selection as group 5"));
	TestTrue(TEXT("Record as group 5"), Command.Intent == EStageAIIntent::RecordGroup && Command.Label == TEXT("Group 5") && Command.Target == EStageAITarget::Selection);
	Command = Parsed(TEXT("make a group of fixtures 1 to 4"));
	TestTrue(TEXT("A group of fixtures by ID"), Command.Intent == EStageAIIntent::RecordGroup && Ids(Command) == TEXT("1,2,3,4"));
	Command = Parsed(TEXT("delete group 2"));
	TestTrue(TEXT("Delete a group"), Command.Intent == EStageAIIntent::DeleteGroup && Command.Group == TEXT("2"));
	Command = Parsed(TEXT("remove the front group"));
	TestTrue(TEXT("Delete a group by name"), Command.Intent == EStageAIIntent::DeleteGroup && Command.Group == TEXT("front"));
	TestTrue(TEXT("Taking fixtures out of a group deletes no group"), Parsed(TEXT("remove the spots from group 2")).Intent != EStageAIIntent::DeleteGroup);

	// Effects.
	Command = Parsed(TEXT("start a circle effect on the movers"));
	TestTrue(TEXT("Start an effect"), Command.Intent == EStageAIIntent::StartEffect && Command.Effect == TEXT("circle") && Command.Target == EStageAITarget::Named
		&& Command.ItemQuery == TEXT("movers"));
	Command = Parsed(TEXT("run a rainbow on group 2 at 0.5 hz"));
	TestTrue(TEXT("Without the word effect, with a rate"), Command.Intent == EStageAIIntent::StartEffect && Command.Effect == TEXT("rainbow")
		&& Command.Group == TEXT("2") && FMath::IsNearlyEqual(Command.RateHz.Get(0.f), 0.5f));
	Command = Parsed(TEXT("add a dimmer chase effect to them"));
	TestTrue(TEXT("Add an effect to them"), Command.Intent == EStageAIIntent::StartEffect && Command.Effect == TEXT("dimmer chase") && Command.Target == EStageAITarget::Recent);
	Command = Parsed(TEXT("start figure eight at 120 bpm"));
	TestTrue(TEXT("Figure eight in bpm"), Command.Effect == TEXT("figure 8") && FMath::IsNearlyEqual(Command.RateHz.Get(0.f), 2.f));
	Command = Parsed(TEXT("stop all effects"));
	TestTrue(TEXT("Stop all"), Command.Intent == EStageAIIntent::StopEffect && Command.Effect.IsEmpty());
	Command = Parsed(TEXT("stop the circle"));
	TestTrue(TEXT("Stop one by name"), Command.Intent == EStageAIIntent::StopEffect && Command.Effect == TEXT("circle"));
	TestTrue(TEXT("\"Stop the strobe\" is still the strobe control"), Parsed(TEXT("stop the strobe")).Intent == EStageAIIntent::SetFixture);
	Command = Parsed(TEXT("set the effect speed to 2 hz"));
	TestTrue(TEXT("Change the rate of all"), Command.Intent == EStageAIIntent::UpdateEffect && Command.Effect.IsEmpty() && FMath::IsNearlyEqual(Command.RateHz.Get(0.f), 2.f));
	Command = Parsed(TEXT("circle effect size 40"));
	TestTrue(TEXT("Change the size of one"), Command.Intent == EStageAIIntent::UpdateEffect && Command.Effect == TEXT("circle") && FMath::IsNearlyEqual(Command.EffectSize.Get(0.f), 40.f));
	TestTrue(TEXT("A circle truss is no effect"), Parsed(TEXT("add a 6 m circle truss at 7 m")).Intent == EStageAIIntent::BuildShape);

	// Again.
	Command = Parsed(TEXT("again"));
	TestTrue(TEXT("Again"), Command.Intent == EStageAIIntent::Repeat && Command.Target == EStageAITarget::Recent);
	Command = Parsed(TEXT("do it again on group 2"));
	TestTrue(TEXT("Again on a group"), Command.Intent == EStageAIIntent::Repeat && Command.Target == EStageAITarget::Group && Command.Group == TEXT("2"));
	Command = Parsed(TEXT("same for the spots"));
	TestTrue(TEXT("The same for named fixtures"), Command.Intent == EStageAIIntent::Repeat && Command.Target == EStageAITarget::Named);
	TestTrue(TEXT("\"Make it red again\" is a colour"), Parsed(TEXT("make it red again")).Intent == EStageAIIntent::SetFixture);
	TestTrue(TEXT("Redo is the history's"), Parsed(TEXT("redo")).Intent == EStageAIIntent::Redo);

	// Armenian.
	Command = Parsed(TEXT("ընտրիր 1-4 սարքերը"));
	TestTrue(TEXT("HY select fixtures by ID"), Command.Intent == EStageAIIntent::SelectFixtures && Ids(Command) == TEXT("1,2,3,4") && Command.Language == EStageAILanguage::Armenian);
	Command = Parsed(TEXT("կիրառիր տաք պալիտրան"));
	TestTrue(TEXT("HY apply a palette"), Command.Intent == EStageAIIntent::ApplyPalette && Command.Palette == TEXT("warm"));
	Command = Parsed(TEXT("սկսիր ծիածան էֆեկտը"));
	TestTrue(TEXT("HY start an effect"), Command.Intent == EStageAIIntent::StartEffect && Command.Effect == TEXT("rainbow"));
	Command = Parsed(TEXT("կանգնեցրու բոլոր էֆեկտները"));
	TestTrue(TEXT("HY stop all effects"), Command.Intent == EStageAIIntent::StopEffect && Command.Effect.IsEmpty());
	Command = Parsed(TEXT("նորից արա 2 խմբի համար"));
	TestTrue(TEXT("HY again on group 2"), Command.Intent == EStageAIIntent::Repeat && Command.Target == EStageAITarget::Group && Command.Group == TEXT("2"));
	Command = Parsed(TEXT("դարձրու դրանք կապույտ"));
	TestTrue(TEXT("HY them blue"), Command.Intent == EStageAIIntent::SetFixture && Command.Target == EStageAITarget::Recent);
	Command = Parsed(TEXT("գրանցիր խումբ «Առջև»"));
	TestTrue(TEXT("HY record a group"), Command.Intent == EStageAIIntent::RecordGroup && Command.Label == TEXT("Առջև"));

	// Names in the pools.
	using StageAICommandParser::ResolveName;
	const TArray<FString> Palettes = { TEXT("Red"), TEXT("Warm White"), TEXT("Group 2"), TEXT("Deep Blue") };
	TestEqual(TEXT("A whole name"), ResolveName(Palettes, TEXT("warm white")).Index, 1);
	TestEqual(TEXT("A word of one name"), ResolveName(Palettes, TEXT("warm")).Index, 1);
	TestEqual(TEXT("A number inside a name"), ResolveName(Palettes, TEXT("2")).Index, 2);
	TestEqual(TEXT("A number as a place"), ResolveName(Palettes, TEXT("4")).Index, 3);
	TestEqual(TEXT("Nothing"), ResolveName(Palettes, TEXT("green")).Index, int32(INDEX_NONE));
	TestEqual(TEXT("Past the pool"), ResolveName(Palettes, TEXT("9")).Index, int32(INDEX_NONE));
	const TArray<FString> Twins = { TEXT("Blue Wash"), TEXT("Blue Spot") };
	const StageAICommandParser::FStageAINameMatch Several = ResolveName(Twins, TEXT("blue"));
	TestTrue(TEXT("Several are reported"), Several.Index == INDEX_NONE && Several.Ambiguous.Num() == 2);
	TArray<FString> Effects;
	for (const FStageEffect& Effect : StageEffectRules::MakeQuickEffects())
	{
		Effects.Add(Effect.Name.ToString());
	}
	for (const FStageEffect& Effect : StageEffectRules::MakeAudioQuickEffects())
	{
		Effects.Add(Effect.Name.ToString());
	}
	const int32 Chase = ResolveName(Effects, TEXT("chase"), /*bFirstOfSeveral*/ true).Index;
	TestTrue(TEXT("Chase is the Dimmer Chase"), Effects.IsValidIndex(Chase) && Effects[Chase] == TEXT("Dimmer Chase"));
	const int32 Figure = ResolveName(Effects, TEXT("figure 8"), true).Index;
	TestTrue(TEXT("Figure 8"), Effects.IsValidIndex(Figure) && Effects[Figure] == TEXT("Figure 8"));
	const int32 Fade = ResolveName(Effects, TEXT("color fade"), true).Index;
	TestTrue(TEXT("Color is colour"), Effects.IsValidIndex(Fade) && Effects[Fade] == TEXT("Colour Fade"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAssistantToolSchemaTest, "StageCraft.Assistant.ToolSchema", StageAIMultiStepTests::Flags)

bool FStageAssistantToolSchemaTest::RunTest(const FString& Parameters)
{
	using StageAIToolCatalogue::CanApplyWithoutAsking;
	using StageAIMultiStepTests::Parsed;

	// What may apply without asking (ADR §6): a selection, and palettes and effects on the selection or "them".
	TestTrue(TEXT("A selection applies at once"), CanApplyWithoutAsking(Parsed(TEXT("select group 2"))));
	TestTrue(TEXT("A palette on the selection applies at once"), CanApplyWithoutAsking(Parsed(TEXT("apply palette 3"))));
	TestTrue(TEXT("Colours on them apply at once"), CanApplyWithoutAsking(Parsed(TEXT("make them red"))));
	TestFalse(TEXT("A palette on a group asks"), CanApplyWithoutAsking(Parsed(TEXT("apply palette 3 to group 2"))));
	TestFalse(TEXT("Recording a group asks"), CanApplyWithoutAsking(Parsed(TEXT("record a group called Front"))));
	TestFalse(TEXT("Deleting a palette asks"), CanApplyWithoutAsking(Parsed(TEXT("delete palette 2"))));
	TestFalse(TEXT("A selection is no change the rollback misses"), StageAIToolCatalogue::HasStepsNotUndone({ Parsed(TEXT("select group 2")) }));

	// "repeat" is the local grammar's only.
	const FStageAITool& Repeat = StageAIToolCatalogue::GetTool(EStageAIIntent::Repeat);
	TestTrue(TEXT("Repeat is local only"), Repeat.bLocalOnly && !Repeat.IsChange());
	const FString Definition = StageAICommandSchema::MakeToolDefinitionJson();
	TestFalse(TEXT("The online tool does not offer repeat"), Definition.Contains(TEXT("\"repeat\"")));
	TestTrue(TEXT("... but the new tools and fields"), Definition.Contains(TEXT("\"apply_palette\"")) && Definition.Contains(TEXT("\"start_effect\""))
		&& Definition.Contains(TEXT("\"palette_kind\"")) && Definition.Contains(TEXT("\"recent\"")) && Definition.Contains(TEXT("\"rate_hz\"")));
	FStageAICommand Command;
	FText Error;
	TestFalse(TEXT("The online model cannot send repeat"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"repeat\"}"), Command, Error));

	// The new tools from the online model.
	TestTrue(TEXT("apply_palette to a group"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"apply_palette\",\"palette\":\"Warm\",\"target\":\"group\",\"group\":\"Front\"}"), Command, Error)
		&& Command.Intent == EStageAIIntent::ApplyPalette && Command.Palette == TEXT("Warm") && Command.Target == EStageAITarget::Group && Command.Group == TEXT("Front"));
	TestTrue(TEXT("start_effect on fixtures by ID"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"start_effect\",\"effect\":\"circle\",\"target\":\"fixtures\",\"fixture_ids\":[1,2],\"rate_hz\":0.5}"),
		Command, Error) && Command.Target == EStageAITarget::Fixtures && Command.FixtureIds.Num() == 2 && FMath::IsNearlyEqual(Command.RateHz.Get(0.f), 0.5f));
	TestTrue(TEXT("record_palette of a kind"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"record_palette\",\"palette_kind\":\"position\",\"label\":\"Downstage\"}"), Command, Error)
		&& Command.PaletteKind == EStagePaletteKind::Position && Command.Label == TEXT("Downstage"));
	TestFalse(TEXT("An unknown palette kind is refused"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"record_palette\",\"palette_kind\":\"smell\"}"), Command, Error));
	TestFalse(TEXT("A delete never acts on remembered fixtures"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"delete_items\",\"target\":\"recent\"}"), Command, Error));
	TestFalse(TEXT("A rate past the limit is refused"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"start_effect\",\"effect\":\"circle\",\"rate_hz\":100}"), Command, Error));
	TestFalse(TEXT("A change of nothing is refused"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"update_effect\"}"), Command, Error));
	TestFalse(TEXT("Select needs a target"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"select_fixtures\"}"), Command, Error));
	TestFalse(TEXT("A group target needs a group"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"set_fixture\",\"target\":\"group\",\"fixture_settings\":[{\"control\":\"dimmer\",\"value\":1}]}"),
		Command, Error));
	TestFalse(TEXT("Fixture IDs from 1"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"select_fixtures\",\"target\":\"fixtures\",\"fixture_ids\":[0]}"), Command, Error));
	TestFalse(TEXT("Delete a palette needs its name"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"delete_palette\"}"), Command, Error));
	TestFalse(TEXT("Validation refuses an unresolved repeat"), [&]() { FStageAICommand Again; Again.Intent = EStageAIIntent::Repeat; return StageAICommandSchema::Validate(Again, Error); }());

	// Card lines.
	const FString Line = StageAIToolCatalogue::DescribeStep(Parsed(TEXT("start a circle effect on group 2 at 0.5 hz")), EStageAILanguage::English).ToString();
	TestEqual(TEXT("Start effect line"), Line, FString(TEXT("Start effect \"circle\" (rate 0.5 Hz) on group \"2\"")));
	TestEqual(TEXT("Them line"), StageAIToolCatalogue::DescribeStep(Parsed(TEXT("use palette red on them")), EStageAILanguage::English).ToString(),
		FString(TEXT("Apply palette \"red\" to the same fixtures")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
