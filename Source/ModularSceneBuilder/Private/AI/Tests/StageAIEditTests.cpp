// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for the assistant's targeted edits and stage shapes (Docs/ADR/0018 §6): the English and Armenian grammar for delete, move,
// replace and build_shape, sizes and units, validation, the tool's wire fields, and item-name matching.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.AI.Edits; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AI/StageAICommandParser.h"
#include "AI/StageAICommandSchema.h"
#include "Misc/AutomationTest.h"

#include <limits>

namespace StageAIEditTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	FString Name(EStageAIIntent Intent) { return StageAICommandSchema::GetIntentName(Intent); }

	FStageAICommand ParseOrDefault(const TCHAR* Text)
	{
		return StageAICommandParser::Parse(Text).Get(FStageAICommand());
	}

	FString IntentOf(const TCHAR* Text)
	{
		const TOptional<FStageAICommand> Command = StageAICommandParser::Parse(Text);
		return Command.IsSet() ? Name(Command->Intent) : FString(TEXT("none"));
	}

	bool Near(const TOptional<double>& Value, double Expected)
	{
		return Value.IsSet() && FMath::IsNearlyEqual(*Value, Expected, 0.01);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAIEditGrammarTest, "StageCraft.AI.Edits.Grammar", StageAIEditTests::Flags)

bool FStageAIEditGrammarTest::RunTest(const FString& Parameters)
{
	using namespace StageAIEditTests;

	// Delete: the selection, items by name, a count, and the whole stage still explained.
	{
		const FStageAICommand Selected = ParseOrDefault(TEXT("delete the selected"));
		TestEqual(TEXT("delete selected"), Name(Selected.Intent), Name(EStageAIIntent::DeleteItems));
		TestTrue(TEXT("... targets the selection"), Selected.Target == EStageAITarget::Selection && Selected.ItemQuery.IsEmpty());
		TestTrue(TEXT("delete alone is the selection"), ParseOrDefault(TEXT("delete")).Target == EStageAITarget::Selection);
		TestTrue(TEXT("delete it"), ParseOrDefault(TEXT("delete it")).Target == EStageAITarget::Selection);

		const FStageAICommand Named = ParseOrDefault(TEXT("remove the moving heads"));
		TestTrue(TEXT("remove named"), Named.Intent == EStageAIIntent::DeleteItems && Named.Target == EStageAITarget::Named);
		TestEqual(TEXT("... query"), Named.ItemQuery, FString(TEXT("moving heads")));
		TestFalse(TEXT("... no count"), Named.Count.IsSet());

		const FStageAICommand Counted = ParseOrDefault(TEXT("delete 2 spots"));
		TestTrue(TEXT("count"), Counted.Count.Get(0) == 2 && Counted.ItemQuery == TEXT("spots"));
		TestEqual(TEXT("selected spots narrow the selection"), ParseOrDefault(TEXT("delete the selected spots")).ItemQuery, FString(TEXT("spots")));
		TestTrue(TEXT("... still the selection"), ParseOrDefault(TEXT("delete the selected spots")).Target == EStageAITarget::Selection);

		TestEqual(TEXT("clear everything is explained"), IntentOf(TEXT("clear everything")), Name(EStageAIIntent::Unsupported));
		TestEqual(TEXT("delete the whole scene is explained"), IntentOf(TEXT("delete the whole scene")), Name(EStageAIIntent::Unsupported));
		TestNotEqual(TEXT("clear the selection never deletes"), IntentOf(TEXT("clear the selection")), Name(EStageAIIntent::DeleteItems));
		TestEqual(TEXT("delete cue 3 is explained"), IntentOf(TEXT("delete cue 3")), Name(EStageAIIntent::Unsupported));
		TestNotEqual(TEXT("a question is not an edit"), IntentOf(TEXT("how many spots can I delete?")), Name(EStageAIIntent::DeleteItems));
		TestEqual(TEXT("a polite question is"), IntentOf(TEXT("can you delete the spots?")), Name(EStageAIIntent::DeleteItems));
	}

	// Weather words with remove / clear still clear the weather.
	TestEqual(TEXT("remove the fog"), IntentOf(TEXT("remove the fog")), Name(EStageAIIntent::SetWeather));
	TestTrue(TEXT("... to clear"), ParseOrDefault(TEXT("remove the fog")).Weather == EStageWeatherPreset::Clear);
	TestTrue(TEXT("clear the rain"), ParseOrDefault(TEXT("clear the rain")).Weather == EStageWeatherPreset::Clear);

	// Move: direction, distance and units, target.
	{
		const FStageAICommand Left = ParseOrDefault(TEXT("move the selected 2 m left"));
		TestTrue(TEXT("move left"), Left.Intent == EStageAIIntent::MoveItems && Left.Direction == EStageAIDirection::Left);
		TestTrue(TEXT("... 2 m"), Near(Left.DistanceCm, 200.0));
		TestTrue(TEXT("... selection"), Left.Target == EStageAITarget::Selection);

		const FStageAICommand Raise = ParseOrDefault(TEXT("raise the truss 50 cm"));
		TestTrue(TEXT("raise is up"), Raise.Intent == EStageAIIntent::MoveItems && Raise.Direction == EStageAIDirection::Up);
		TestTrue(TEXT("... 50 cm"), Near(Raise.DistanceCm, 50.0));
		TestTrue(TEXT("... the truss"), Raise.Target == EStageAITarget::Named && Raise.ItemQuery == TEXT("truss"));

		TestTrue(TEXT("glued unit"), Near(ParseOrDefault(TEXT("move it 1.5m forward")).DistanceCm, 150.0));
		TestTrue(TEXT("bare number after the direction is metres"), Near(ParseOrDefault(TEXT("move it right 3")).DistanceCm, 300.0));
		TestTrue(TEXT("by"), Near(ParseOrDefault(TEXT("lower the pars by 30 cm")).DistanceCm, 30.0));
		TestFalse(TEXT("no distance"), ParseOrDefault(TEXT("move the decks back")).DistanceCm.IsSet());
		TestTrue(TEXT("upstage is back"), ParseOrDefault(TEXT("move the decks upstage")).Direction == EStageAIDirection::Back);
		TestTrue(TEXT("count and distance"), ParseOrDefault(TEXT("move 2 spots left 1 m")).Count.Get(0) == 2);
		TestEqual(TEXT("no direction is not a move"), IntentOf(TEXT("move the spots")), FString(TEXT("none")));
	}

	// Replace.
	{
		const FStageAICommand Replace = ParseOrDefault(TEXT("replace the pars with washes"));
		TestTrue(TEXT("replace"), Replace.Intent == EStageAIIntent::ReplaceItems && Replace.Target == EStageAITarget::Named);
		TestEqual(TEXT("... target"), Replace.ItemQuery, FString(TEXT("pars")));
		TestEqual(TEXT("... replacement"), Replace.Replacement, FString(TEXT("washes")));
		const FStageAICommand Change = ParseOrDefault(TEXT("change the selected lights to moving heads"));
		TestTrue(TEXT("change ... to"), Change.Intent == EStageAIIntent::ReplaceItems && Change.Target == EStageAITarget::Selection);
		TestEqual(TEXT("... replacement"), Change.Replacement, FString(TEXT("moving heads")));
		TestEqual(TEXT("swap ... for"), ParseOrDefault(TEXT("swap 3 spots for a beam")).Replacement, FString(TEXT("beam")));
		TestEqual(TEXT("change the time is not a replace"), IntentOf(TEXT("change the time to 18:00")), Name(EStageAIIntent::SetTimeOfDay));
		TestEqual(TEXT("replace without with keeps an empty replacement"), ParseOrDefault(TEXT("replace the spots")).Replacement, FString());
	}

	// Shapes and their sizes.
	{
		const FStageAICommand Round = ParseOrDefault(TEXT("build a 4 m round riser 60 cm high"));
		TestTrue(TEXT("round riser"), Round.Intent == EStageAIIntent::BuildShape && Round.Shape == EStageAIShape::RoundRiser);
		TestTrue(TEXT("... unlabelled size is the diameter"), Near(Round.RadiusCm, 200.0));
		TestTrue(TEXT("... height"), Near(Round.HeightCm, 60.0));
		TestTrue(TEXT("radius named"), Near(ParseOrDefault(TEXT("add a circular platform with a radius of 3 m")).RadiusCm, 300.0));

		const FStageAICommand Stairs = ParseOrDefault(TEXT("add stairs 80 cm high and 1.2 m wide without handrails"));
		TestTrue(TEXT("stairs"), Stairs.Shape == EStageAIShape::Stairs && Near(Stairs.HeightCm, 80.0) && Near(Stairs.WidthCm, 120.0));
		TestFalse(TEXT("... no handrails"), Stairs.bHandrails);
		TestTrue(TEXT("stairs with handrails by default"), ParseOrDefault(TEXT("add some steps")).bHandrails);

		const FStageAICommand Circle = ParseOrDefault(TEXT("add a 6 m circle truss at 7 m"));
		TestTrue(TEXT("circle truss"), Circle.Shape == EStageAIShape::ArcTruss && Near(Circle.AngleDegrees, 360.0));
		TestTrue(TEXT("... diameter"), Near(Circle.RadiusCm, 300.0));
		TestTrue(TEXT("... hang height"), Near(Circle.HeightCm, 700.0));

		const FStageAICommand Arc = ParseOrDefault(TEXT("make a 120° arc truss radius 5 m"));
		TestTrue(TEXT("arc truss"), Arc.Shape == EStageAIShape::ArcTruss && Near(Arc.AngleDegrees, 120.0) && Near(Arc.RadiusCm, 500.0));
		TestTrue(TEXT("semicircle is 180"), Near(ParseOrDefault(TEXT("build a semicircle truss")).AngleDegrees, 180.0));
		TestTrue(TEXT("curved riser"), ParseOrDefault(TEXT("add a curved riser 90 degrees")).Shape == EStageAIShape::ArcRiser);

		TestEqual(TEXT("fixtures in a circle are fixtures"), IntentOf(TEXT("add 8 spots in a circle on the truss")), Name(EStageAIIntent::AddFixtures));
		TestEqual(TEXT("a plain riser is not a shape"), IntentOf(TEXT("add a riser")), Name(EStageAIIntent::AddFixtures));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAIEditArmenianTest, "StageCraft.AI.Edits.Armenian", StageAIEditTests::Flags)

bool FStageAIEditArmenianTest::RunTest(const FString& Parameters)
{
	using namespace StageAIEditTests;

	const FStageAICommand Delete = ParseOrDefault(TEXT("ջնջիր ընտրվածը"));
	TestTrue(TEXT("delete selected"), Delete.Intent == EStageAIIntent::DeleteItems && Delete.Target == EStageAITarget::Selection);
	TestTrue(TEXT("... replies in Armenian"), Delete.Language == EStageAILanguage::Armenian);
	const FStageAICommand Named = ParseOrDefault(TEXT("հեռացրու բոլոր սփոթերը"));
	TestTrue(TEXT("remove the spots"), Named.Intent == EStageAIIntent::DeleteItems && Named.Target == EStageAITarget::Named && Named.ItemQuery == TEXT("spot"));
	TestEqual(TEXT("clear the stage is explained"), IntentOf(TEXT("մաքրիր ամբողջ բեմը")), Name(EStageAIIntent::Unsupported));

	const FStageAICommand Move = ParseOrDefault(TEXT("տեղափոխիր ընտրվածը 2 մետր ձախ"));
	TestTrue(TEXT("move left"), Move.Intent == EStageAIIntent::MoveItems && Move.Direction == EStageAIDirection::Left && Near(Move.DistanceCm, 200.0));
	const FStageAICommand Raise = ParseOrDefault(TEXT("բարձրացրու ֆերման 50 սմ"));
	TestTrue(TEXT("raise the truss"), Raise.Direction == EStageAIDirection::Up && Near(Raise.DistanceCm, 50.0) && Raise.ItemQuery == TEXT("truss"));
	TestTrue(TEXT("glued Armenian unit"), Near(ParseOrDefault(TEXT("տեղափոխիր այն 3մ աջ")).DistanceCm, 300.0));

	const FStageAICommand Replace = ParseOrDefault(TEXT("փոխարինիր փարերը վոշերով"));
	TestTrue(TEXT("replace with (instrumental)"), Replace.Intent == EStageAIIntent::ReplaceItems && Replace.ItemQuery == TEXT("par") && Replace.Replacement == TEXT("wash"));

	const FStageAICommand Round = ParseOrDefault(TEXT("կառուցիր 4 մ կլոր հարթակ 60 սմ բարձրությամբ"));
	TestTrue(TEXT("round riser"), Round.Shape == EStageAIShape::RoundRiser && Near(Round.RadiusCm, 200.0) && Near(Round.HeightCm, 60.0));
	const FStageAICommand Stairs = ParseOrDefault(TEXT("ավելացրու աստիճաններ 80 սմ բարձրությամբ առանց բազրիքի"));
	TestTrue(TEXT("stairs"), Stairs.Shape == EStageAIShape::Stairs && Near(Stairs.HeightCm, 80.0) && !Stairs.bHandrails);
	const FStageAICommand Arc = ParseOrDefault(TEXT("ավելացրու 180 աստիճան աղեղաձև ֆերմա 5 մ շառավղով"));
	TestTrue(TEXT("degrees, not stairs"), Arc.Shape == EStageAIShape::ArcTruss && Near(Arc.AngleDegrees, 180.0) && Near(Arc.RadiusCm, 500.0));
	TestTrue(TEXT("circle truss"), Near(ParseOrDefault(TEXT("ավելացրու 6 մ շրջանաձև ֆերմա")).AngleDegrees, 360.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAIEditSchemaTest, "StageCraft.AI.Edits.Schema", StageAIEditTests::Flags)

bool FStageAIEditSchemaTest::RunTest(const FString& Parameters)
{
	using namespace StageAIEditTests;
	FText Error;

	// Validation.
	{
		FStageAICommand Command;
		Command.Intent = EStageAIIntent::DeleteItems;
		Command.Target = EStageAITarget::Named;
		TestFalse(TEXT("named without a name"), StageAICommandSchema::Validate(Command, Error));
		Command.ItemQuery = TEXT("  spots  ");
		TestTrue(TEXT("named with a name"), StageAICommandSchema::Validate(Command, Error) && Command.ItemQuery == TEXT("spots"));
		Command.Count = 0;
		TestFalse(TEXT("count 0"), StageAICommandSchema::Validate(Command, Error));
		Command.Count = StageAICommandParser::MaxEditTargets + 1;
		TestFalse(TEXT("count above the limit"), StageAICommandSchema::Validate(Command, Error));

		FStageAICommand Move;
		Move.Intent = EStageAIIntent::MoveItems;
		TestTrue(TEXT("move default distance"), StageAICommandSchema::Validate(Move, Error) && Near(Move.DistanceCm, StageAICommandParser::DefaultMoveDistanceCm));
		Move.DistanceCm = StageAICommandParser::MaxMoveDistanceCm * 2.0;
		TestFalse(TEXT("move too far"), StageAICommandSchema::Validate(Move, Error));
		Move.DistanceCm = std::numeric_limits<double>::quiet_NaN();
		TestFalse(TEXT("move NaN"), StageAICommandSchema::Validate(Move, Error));

		FStageAICommand Replace;
		Replace.Intent = EStageAIIntent::ReplaceItems;
		TestFalse(TEXT("replace with nothing"), StageAICommandSchema::Validate(Replace, Error));
		Replace.Replacement = TEXT("wash");
		TestTrue(TEXT("replace with something"), StageAICommandSchema::Validate(Replace, Error));

		FStageAICommand Shape;
		Shape.Intent = EStageAIIntent::BuildShape;
		TestTrue(TEXT("shape with defaults"), StageAICommandSchema::Validate(Shape, Error));
		Shape.AngleDegrees = 400.0;
		TestFalse(TEXT("angle above 360"), StageAICommandSchema::Validate(Shape, Error));
		Shape.AngleDegrees.Reset();
		Shape.RadiusCm = -5.0;
		TestFalse(TEXT("negative radius"), StageAICommandSchema::Validate(Shape, Error));
		Shape.RadiusCm = std::numeric_limits<double>::infinity();
		TestFalse(TEXT("infinite radius"), StageAICommandSchema::Validate(Shape, Error));
	}

	// The tool's wire fields.
	{
		FStageAICommand Command;
		TestTrue(TEXT("tool: delete named"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"delete_items\",\"target\":\"named\",\"item\":\"par\",\"count\":2}"), Command, Error));
		TestTrue(TEXT("... fields"), Command.Intent == EStageAIIntent::DeleteItems && Command.Target == EStageAITarget::Named && Command.Count.Get(0) == 2);
		TestTrue(TEXT("tool: item without target is named"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"delete_items\",\"item\":\"par\"}"), Command, Error)
			&& Command.Target == EStageAITarget::Named);
		TestTrue(TEXT("tool: move"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"move_items\",\"target\":\"selection\",\"direction\":\"up\",\"distance_cm\":50}"), Command, Error)
			&& Command.Direction == EStageAIDirection::Up && Near(Command.DistanceCm, 50.0));
		TestFalse(TEXT("tool: move without direction"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"move_items\"}"), Command, Error));
		TestFalse(TEXT("tool: unknown direction"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"move_items\",\"direction\":\"sideways\"}"), Command, Error));
		TestTrue(TEXT("tool: replace"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"replace_items\",\"replacement\":\"wash\"}"), Command, Error)
			&& Command.Replacement == TEXT("wash"));
		TestTrue(TEXT("tool: shape"), StageAICommandSchema::CommandFromToolInputJson(
			TEXT("{\"intent\":\"build_shape\",\"shape\":\"stairs\",\"height_cm\":80,\"width_cm\":120,\"handrails\":false}"), Command, Error)
			&& Command.Shape == EStageAIShape::Stairs && Near(Command.HeightCm, 80.0) && !Command.bHandrails);
		TestFalse(TEXT("tool: shape without shape"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"build_shape\"}"), Command, Error));
		TestFalse(TEXT("tool: unknown shape"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"build_shape\",\"shape\":\"pyramid\"}"), Command, Error));
		TestFalse(TEXT("tool: size not a number"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"build_shape\",\"shape\":\"stairs\",\"height_cm\":\"tall\"}"), Command, Error));

		const FString Tool = StageAICommandSchema::MakeToolDefinitionJson();
		for (const TCHAR* Field : { TEXT("\"target\""), TEXT("\"replacement\""), TEXT("\"direction\""), TEXT("\"distance_cm\""), TEXT("\"shape\""), TEXT("\"radius_cm\""),
			TEXT("\"angle_degrees\""), TEXT("\"handrails\""), TEXT("\"delete_items\""), TEXT("\"build_shape\"") })
		{
			TestTrue(FString::Printf(TEXT("Tool lists %s"), Field), Tool.Contains(Field));
		}
		TestFalse(TEXT("System prompt no longer says deleting is impossible"), StageAICommandSchema::GetSystemPrompt().Contains(TEXT("deleting or clearing items")));
	}

	// The selection reaches the model's scene summary.
	{
		FStageAISceneView Scene;
		Scene.SelectedItemNames = { TEXT("Spot 350"), TEXT("Spot 350"), TEXT("LED Par") };
		TestTrue(TEXT("Scene lists the selection"), StageAICommandSchema::DescribeScene(Scene).Contains(TEXT("Selected: 3 - 2 x Spot 350, 1 x LED Par.")));
	}

	// Item names: every word, singular, and generic fixture words for fixtures.
	TestTrue(TEXT("name: plural query"), StageAICommandParser::MatchesItemName(TEXT("Spot 350"), TEXT("spots")));
	TestTrue(TEXT("name: two words"), StageAICommandParser::MatchesItemName(TEXT("Moving Head Wash"), TEXT("moving heads")));
	TestFalse(TEXT("name: a word missing"), StageAICommandParser::MatchesItemName(TEXT("LED Par"), TEXT("moving heads")));
	TestTrue(TEXT("name: lights are fixtures"), StageAICommandParser::MatchesItemName(TEXT("LED Par"), TEXT("lights"), /*bIsFixture*/ true));
	TestFalse(TEXT("name: lights are not decks"), StageAICommandParser::MatchesItemName(TEXT("Deck 2x1"), TEXT("lights"), /*bIsFixture*/ false));
	TestFalse(TEXT("name: empty query"), StageAICommandParser::MatchesItemName(TEXT("LED Par"), TEXT("")));
	TestTrue(TEXT("generic: lights"), StageAICommandParser::IsGenericFixtureQuery(TEXT("lights")));
	TestFalse(TEXT("generic: wash"), StageAICommandParser::IsGenericFixtureQuery(TEXT("wash")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
