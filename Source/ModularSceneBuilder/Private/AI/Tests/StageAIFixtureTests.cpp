// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for the assistant's fixture commands (STATE.md #49, Docs/ADR/0020 §7): the English and Armenian grammar of set_fixture
// (dimmer, colour, gel, zoom, frost, iris, CTO, prism, gobo, spin, strobe, pan, tilt), its targets, what it leaves to other
// commands, validation, and the tool's wire fields.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.AI.Fixtures; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AI/StageAICommandParser.h"
#include "AI/StageAICommandSchema.h"
#include "Misc/AutomationTest.h"

#include <limits>

namespace StageAIFixtureTests
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

	const FStageAIFixtureSetting* Find(const FStageAICommand& Command, EStageAIFixtureControl Control)
	{
		return Command.FixtureSettings.FindByPredicate([Control](const FStageAIFixtureSetting& Setting) { return Setting.Control == Control; });
	}

	bool Has(const FStageAICommand& Command, EStageAIFixtureControl Control, double Value)
	{
		const FStageAIFixtureSetting* Setting = Find(Command, Control);
		return Setting && Setting->Value.IsSet() && FMath::IsNearlyEqual(*Setting->Value, Value, 0.001);
	}

	bool HasName(const FStageAICommand& Command, EStageAIFixtureControl Control, const TCHAR* Name)
	{
		const FStageAIFixtureSetting* Setting = Find(Command, Control);
		return Setting && Setting->Name == Name;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAIFixtureGrammarTest, "StageCraft.AI.Fixtures", StageAIFixtureTests::Flags)

bool FStageAIFixtureGrammarTest::RunTest(const FString& Parameters)
{
	using namespace StageAIFixtureTests;
	using C = EStageAIFixtureControl;
	const FString SetFixture = StageAICommandSchema::GetIntentName(EStageAIIntent::SetFixture);

	// English: one control at a time, with the forms people use.
	struct FCase { const TCHAR* Prompt; C Control; double Value; };
	const FCase Cases[] = {
		{ TEXT("dimmer 50%"), C::Dimmer, 0.5 }, { TEXT("set the dimmer to 75 percent"), C::Dimmer, 0.75 }, { TEXT("intensity 0.3"), C::Dimmer, 0.3 },
		{ TEXT("set them to 40%"), C::Dimmer, 0.4 }, { TEXT("bring the brightness to full"), C::Dimmer, 1.0 }, { TEXT("blackout the washes"), C::Dimmer, 0.0 },
		{ TEXT("switch off the pars"), C::Dimmer, 0.0 }, { TEXT("zoom 20 degrees"), C::Zoom, 20.0 }, { TEXT("zoom to 35°"), C::Zoom, 35.0 },
		{ TEXT("frost on"), C::Frost, 1.0 }, { TEXT("half frost"), C::Frost, 0.5 }, { TEXT("frost 30%"), C::Frost, 0.3 }, { TEXT("remove the frost"), C::Frost, 0.0 },
		{ TEXT("close the iris"), C::Iris, 1.0 }, { TEXT("iris 60%"), C::Iris, 0.6 }, { TEXT("open the iris"), C::Iris, 0.0 },
		{ TEXT("full cto"), C::CTO, 1.0 }, { TEXT("half cto on the spots"), C::CTO, 0.5 }, { TEXT("quarter cto"), C::CTO, 0.25 }, { TEXT("no cto"), C::CTO, 0.0 },
		{ TEXT("prism in"), C::Prism, 1.0 }, { TEXT("prism 2"), C::Prism, 2.0 }, { TEXT("prism out"), C::Prism, 0.0 },
		{ TEXT("spin the prism 5 rpm"), C::PrismSpin, 5.0 }, { TEXT("gobo 3"), C::Gobo, 3.0 }, { TEXT("open gobo"), C::Gobo, 0.0 },
		{ TEXT("rotate the gobo 12 rpm"), C::GoboSpin, 12.0 }, { TEXT("strobe the blinders"), C::Strobe, StageAICommandParser::DefaultStrobeHz },
		{ TEXT("strobe 15 hz"), C::Strobe, 15.0 }, { TEXT("stop the strobe"), C::Strobe, 0.0 }, { TEXT("fast strobe"), C::Strobe, 20.0 },
		{ TEXT("pan 90"), C::Pan, 90.0 }, { TEXT("tilt to 45 degrees"), C::Tilt, 45.0 },
	};
	for (const FCase& Case : Cases)
	{
		const FStageAICommand Command = ParseOrDefault(Case.Prompt);
		TestTrue(*FString::Printf(TEXT("'%s' -> set_fixture %d = %.3f"), Case.Prompt, int32(Case.Control), Case.Value),
			Command.Intent == EStageAIIntent::SetFixture && Has(Command, Case.Control, Case.Value));
	}

	// Names: colours, gels, gobos and zoom ends.
	TestTrue(TEXT("make the washes red"), ParseOrDefault(TEXT("make the washes red")).FixtureSettings.Num() == 1 && Find(ParseOrDefault(TEXT("make the washes red")), C::Color)
		&& Find(ParseOrDefault(TEXT("make the washes red")), C::Color)->Color.Equals(FLinearColor(1.f, 0.f, 0.f)));
	TestTrue(TEXT("light blue is one colour, not a light"), HasName(ParseOrDefault(TEXT("light blue on the spots")), C::Color, TEXT("light blue"))
		&& ParseOrDefault(TEXT("light blue on the spots")).ItemQuery == TEXT("spots"));
	TestTrue(TEXT("warm white"), HasName(ParseOrDefault(TEXT("warm white")), C::Color, TEXT("warm white")));
	TestTrue(TEXT("lee 106"), HasName(ParseOrDefault(TEXT("lee 106 gel on the selected")), C::Gel, TEXT("lee 106")));
	TestTrue(TEXT("R27"), HasName(ParseOrDefault(TEXT("put r27 in the fresnels")), C::Gel, TEXT("r27")));
	TestTrue(TEXT("a gel by name is not a colour"), HasName(ParseOrDefault(TEXT("primary red gel")), C::Gel, TEXT("primary red"))
		&& !Find(ParseOrDefault(TEXT("primary red gel")), C::Color));
	TestTrue(TEXT("gobo star"), HasName(ParseOrDefault(TEXT("gobo star")), C::Gobo, TEXT("star")));
	TestTrue(TEXT("zoom wide"), HasName(ParseOrDefault(TEXT("zoom out wide")), C::Zoom, TEXT("wide")));
	TestTrue(TEXT("zoom narrow"), HasName(ParseOrDefault(TEXT("narrowest zoom")), C::Zoom, TEXT("narrow")));

	// Several controls and targets.
	const FStageAICommand Many = ParseOrDefault(TEXT("make the 4 washes blue at 50% dimmer with frost"));
	TestTrue(TEXT("several controls"), Find(Many, C::Color) && Has(Many, C::Dimmer, 0.5) && Has(Many, C::Frost, 1.0));
	TestTrue(TEXT("... on 4 washes"), Many.Target == EStageAITarget::Named && Many.ItemQuery == TEXT("washes") && Many.Count.Get(0) == 4);
	TestTrue(TEXT("them = the selection"), ParseOrDefault(TEXT("make them green")).Target == EStageAITarget::Selection);
	TestTrue(TEXT("nothing named = the selection"), ParseOrDefault(TEXT("zoom 15")).Target == EStageAITarget::Selection);
	TestEqual(TEXT("the pixel bars"), ParseOrDefault(TEXT("turn the pixel bars magenta")).ItemQuery, FString(TEXT("pixel bars")));
	TestTrue(TEXT("\"beam\" names a fixture"), Has(ParseOrDefault(TEXT("prism 2 on the beam 280")), C::Prism, 2.0) && ParseOrDefault(TEXT("prism 2 on the beam 280")).ItemQuery == TEXT("beam 280"));
	TestTrue(TEXT("change the spots to blue is a colour"), ParseOrDefault(TEXT("change the spots to blue")).Intent == EStageAIIntent::SetFixture);

	// Other commands keep their prompts.
	TestEqual(TEXT("change to beams is a replace"), IntentOf(TEXT("change the spots to beams")), StageAICommandSchema::GetIntentName(EStageAIIntent::ReplaceItems));
	TestEqual(TEXT("remove the red pars deletes"), IntentOf(TEXT("remove the red pars")), StageAICommandSchema::GetIntentName(EStageAIIntent::DeleteItems));
	TestEqual(TEXT("add red pars adds"), IntentOf(TEXT("add 4 pars")), StageAICommandSchema::GetIntentName(EStageAIIntent::AddFixtures));
	TestEqual(TEXT("move stays a move"), IntentOf(TEXT("move the spots 2 m left")), StageAICommandSchema::GetIntentName(EStageAIIntent::MoveItems));
	TestEqual(TEXT("turn the sky on stays the sky"), IntentOf(TEXT("turn the sky on")), StageAICommandSchema::GetIntentName(EStageAIIntent::SetSky));
	TestEqual(TEXT("store cue stays a cue"), IntentOf(TEXT("store cue 3")), StageAICommandSchema::GetIntentName(EStageAIIntent::StoreCue));
	TestNotEqual(TEXT("a question is not an edit"), IntentOf(TEXT("what is the dimmer?")), SetFixture);
	TestNotEqual(TEXT("'switch off' alone is too vague"), IntentOf(TEXT("switch off")), SetFixture);
	TestTrue(TEXT("turn them off is the selection's dimmer"), Has(ParseOrDefault(TEXT("turn them off")), C::Dimmer, 0.0));

	// Armenian.
	struct FArmenianCase { const TCHAR* Prompt; C Control; };
	const FArmenianCase Armenian[] = {
		{ TEXT("վոշերը դարձրու կարմիր"), C::Color }, { TEXT("դիմեր 50 տոկոս"), C::Dimmer }, { TEXT("զում 20 աստիճան"), C::Zoom }, { TEXT("միացրու ֆրոստը"), C::Frost },
		{ TEXT("պրիզմա"), C::Prism }, { TEXT("գոբո 2"), C::Gobo }, { TEXT("ստրոբ"), C::Strobe }, { TEXT("լի 106 ժել"), C::Gel }, { TEXT("կես CTO"), C::CTO },
		{ TEXT("փակիր իրիսը"), C::Iris }, { TEXT("պան 90"), C::Pan }, { TEXT("թիլթ 30"), C::Tilt }, { TEXT("պիքսել բարերը դարձրու կապույտ"), C::Color },
	};
	for (const FArmenianCase& Case : Armenian)
	{
		const FStageAICommand Command = ParseOrDefault(Case.Prompt);
		TestTrue(*FString::Printf(TEXT("'%s' -> control %d"), Case.Prompt, int32(Case.Control)),
			Command.Intent == EStageAIIntent::SetFixture && Find(Command, Case.Control) != nullptr && Command.Language == EStageAILanguage::Armenian);
	}
	TestTrue(TEXT("Armenian degrees are not stairs"), Has(ParseOrDefault(TEXT("զում 20 աստիճան")), C::Zoom, 20.0) && ParseOrDefault(TEXT("զում 20 աստիճան")).ItemQuery.IsEmpty());
	TestTrue(TEXT("Armenian dimmer value"), Has(ParseOrDefault(TEXT("դիմեր 50 տոկոս")), C::Dimmer, 0.5));
	TestEqual(TEXT("Armenian target"), ParseOrDefault(TEXT("վոշերը դարձրու կարմիր")).ItemQuery, FString(TEXT("wash")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAIFixtureSchemaTest, "StageCraft.AI.FixtureSchema", StageAIFixtureTests::Flags)

bool FStageAIFixtureSchemaTest::RunTest(const FString& Parameters)
{
	using namespace StageAIFixtureTests;
	using C = EStageAIFixtureControl;

	const auto Valid = [](FStageAICommand Command)
	{
		FText Error;
		return StageAICommandSchema::Validate(Command, Error);
	};
	const auto With = [](C Control, TOptional<double> Value, const TCHAR* Name = TEXT(""))
	{
		FStageAICommand Command;
		Command.Intent = EStageAIIntent::SetFixture;
		FStageAIFixtureSetting& Setting = Command.FixtureSettings.AddDefaulted_GetRef();
		Setting.Control = Control;
		Setting.Value = Value;
		Setting.Name = Name;
		return Command;
	};
	TestTrue(TEXT("dimmer 0.5"), Valid(With(C::Dimmer, 0.5)));
	TestFalse(TEXT("dimmer 2"), Valid(With(C::Dimmer, 2.0)));
	TestFalse(TEXT("dimmer NaN"), Valid(With(C::Dimmer, std::numeric_limits<double>::quiet_NaN())));
	TestFalse(TEXT("dimmer without a value"), Valid(With(C::Dimmer, {})));
	TestTrue(TEXT("zoom wide"), Valid(With(C::Zoom, {}, TEXT("wide"))));
	TestFalse(TEXT("zoom 200"), Valid(With(C::Zoom, 200.0)));
	TestFalse(TEXT("gobo 1.5"), Valid(With(C::Gobo, 1.5)));
	TestTrue(TEXT("gel by name"), Valid(With(C::Gel, {}, TEXT("lee 106"))));
	TestFalse(TEXT("gel without a name"), Valid(With(C::Gel, {})));
	TestTrue(TEXT("strobe off"), Valid(With(C::Strobe, 0.0)));
	TestFalse(TEXT("strobe 80 Hz"), Valid(With(C::Strobe, 80.0)));
	FStageAICommand None;
	None.Intent = EStageAIIntent::SetFixture;
	TestFalse(TEXT("nothing to set"), Valid(None));
	FStageAICommand TooMany = With(C::Dimmer, 1.0);
	TooMany.FixtureSettings.SetNum(StageAICommandParser::MaxFixtureSettings + 1);
	TestFalse(TEXT("too many settings"), Valid(TooMany));

	// The tool's wire fields.
	TestTrue(TEXT("the tool lists set_fixture and its settings"), StageAICommandSchema::MakeToolDefinitionJson().Contains(TEXT("set_fixture"))
		&& StageAICommandSchema::MakeToolDefinitionJson().Contains(TEXT("fixture_settings")));
	FStageAICommand FromTool;
	FText Error;
	TestTrue(TEXT("tool input parses"), StageAICommandSchema::CommandFromToolInputJson(
		TEXT("{\"intent\":\"set_fixture\",\"item\":\"wash\",\"fixture_settings\":[{\"control\":\"color\",\"name\":\"deep blue\"},{\"control\":\"dimmer\",\"value\":0.4},{\"control\":\"gel\",\"name\":\"rosco 27\"}]}"),
		FromTool, Error));
	TestTrue(TEXT("... three settings on the named washes"), FromTool.FixtureSettings.Num() == 3 && FromTool.Target == EStageAITarget::Named
		&& Find(FromTool, C::Color) && Find(FromTool, C::Color)->Color.B > 0.99f && Has(FromTool, C::Dimmer, 0.4));
	TestFalse(TEXT("an unknown colour is refused"), StageAICommandSchema::CommandFromToolInputJson(
		TEXT("{\"intent\":\"set_fixture\",\"fixture_settings\":[{\"control\":\"color\",\"name\":\"octarine\"}]}"), FromTool, Error));
	TestFalse(TEXT("an unknown control is refused"), StageAICommandSchema::CommandFromToolInputJson(
		TEXT("{\"intent\":\"set_fixture\",\"fixture_settings\":[{\"control\":\"focus\",\"value\":1}]}"), FromTool, Error));
	TestFalse(TEXT("set_fixture without settings is refused"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"set_fixture\"}"), FromTool, Error));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
