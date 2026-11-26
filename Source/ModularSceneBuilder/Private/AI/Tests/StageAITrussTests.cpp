// Copyright Epic Games, Inc. All Rights Reserved.

// Fixtures the assistant hangs on a truss run already on the stage (Docs/ADR/0016-ai-assistant.md §8): the local grammar's
// mount words, the online tool's mount field, the proposal card's wording, and the pure spot layout (free spots along a run's
// bottom chord, clear of the fixtures already hanging, spread evenly). Pure; the runner's world side is checked in -game runs.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.AI.TrussHang; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AI/StageAIArmenian.h"
#include "AI/StageAICommandParser.h"
#include "AI/StageAICommandSchema.h"
#include "AI/StageAIToolCatalogue.h"
#include "Algo/AllOf.h"
#include "Misc/AutomationTest.h"

namespace StageAITrussTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	FString MountName(EStageAIFixtureMount Mount)
	{
		switch (Mount)
		{
		case EStageAIFixtureMount::Floor:		return TEXT("floor");
		case EStageAIFixtureMount::Truss:		return TEXT("truss");
		case EStageAIFixtureMount::FrontTruss:	return TEXT("front truss");
		case EStageAIFixtureMount::BackTruss:	return TEXT("back truss");
		default:								return TEXT("?");
		}
	}

	/** "add_fixtures 4 'pars' truss", or "none". */
	FString Describe(const TCHAR* Prompt)
	{
		const TOptional<FStageAICommand> Command = StageAICommandParser::Parse(Prompt);
		if (!Command.IsSet())
		{
			return TEXT("none");
		}
		return FString::Printf(TEXT("%s %d '%s' %s"), *StageAICommandSchema::GetIntentName(Command->Intent), Command->Count.Get(1), *Command->ItemQuery,
			*MountName(Command->Mount));
	}

	/** The Y of each spot, for compact comparisons. */
	FString Ys(TConstArrayView<FVector> Spots)
	{
		TArray<FString> Parts;
		for (const FVector& Spot : Spots)
		{
			Parts.Add(FString::Printf(TEXT("%.0f"), Spot.Y));
		}
		return FString::Join(Parts, TEXT(" "));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAITrussHangTest, "StageCraft.AI.TrussHang", StageAITrussTests::Flags)

bool FStageAITrussHangTest::RunTest(const FString& Parameters)
{
	using namespace StageAITrussTests;

	AddInfo(TEXT("Grammar: where the new fixtures go, and the words that say so are not part of the fixture's name."));
	TestEqual(TEXT("Hang on the truss"), Describe(TEXT("hang 4 pars on the truss")), FString(TEXT("add_fixtures 4 'pars' truss")));
	TestEqual(TEXT("Add ... to the truss"), Describe(TEXT("add 2 moving head spots to the truss")), FString(TEXT("add_fixtures 2 'moving head spots' truss")));
	TestEqual(TEXT("Hanging alone means the truss"), Describe(TEXT("hang 3 washes")), FString(TEXT("add_fixtures 3 'washes' truss")));
	TestEqual(TEXT("Back truss"), Describe(TEXT("add 2 spots on the back truss")), FString(TEXT("add_fixtures 2 'spots' back truss")));
	TestEqual(TEXT("Upstage truss"), Describe(TEXT("put 6 pars on the upstage truss")), FString(TEXT("add_fixtures 6 'pars' back truss")));
	TestEqual(TEXT("Front truss"), Describe(TEXT("hang 4 beams from the front truss")), FString(TEXT("add_fixtures 4 'beams' front truss")));
	TestEqual(TEXT("Plain add stays on the floor"), Describe(TEXT("add 4 pars")), FString(TEXT("add_fixtures 4 'pars' floor")));
	TestEqual(TEXT("Hang ... on the floor stays on the floor"), Describe(TEXT("hang 2 pars on the floor")), FString(TEXT("add_fixtures 2 'pars' floor")));
	TestEqual(TEXT("Front without truss is the floor row (as before)"), Describe(TEXT("add 2 pars in front")), FString(TEXT("add_fixtures 2 'pars' floor")));
	{
		const TOptional<FStageAICommand> Armenian = StageAICommandParser::Parse(TEXT("կախիր 4 լույս ֆերմայից"));
		TestTrue(TEXT("Armenian: hang on the truss"), Armenian.IsSet() && Armenian->Intent == EStageAIIntent::AddFixtures && Armenian->Count.Get(1) == 4
			&& Armenian->Mount == EStageAIFixtureMount::Truss && Armenian->Language == EStageAILanguage::Armenian);
		// Which run, in Armenian; the run words must not reach the fixture query ("light", not "light հետևի").
		TestEqual(TEXT("Armenian: back truss"), Describe(TEXT("կախիր 2 լույս հետևի ֆերմայից")), FString(TEXT("add_fixtures 2 'light' back truss")));
		TestEqual(TEXT("Armenian: front truss"), Describe(TEXT("կախիր 3 սփոթ առջևի ֆերմայից")), FString(TEXT("add_fixtures 3 'spot' front truss")));
		TestEqual(TEXT("Armenian: colloquial front"), Describe(TEXT("ավելացրու 2 փար առաջի ֆերմային")), FString(TEXT("add_fixtures 2 'par' front truss")));
		TestEqual(TEXT("Armenian: back, the other spelling of և"), Describe(TEXT("կախիր 2 փար հետեւի ֆերմայից")), FString(TEXT("add_fixtures 2 'par' back truss")));
		TestEqual(TEXT("Armenian: follow is not back"), StageAIArmenian::TranslateToGrammar(TEXT("լույսերը հետևեն երաժշտությանը")).Contains(TEXT("back")), false);
		TestEqual(TEXT("Armenian: hang alone means the truss"),Describe(TEXT("կախիր 4 փար")), FString(TEXT("add_fixtures 4 'par' truss")));
		TestEqual(TEXT("Armenian: a plain add stays on the floor"), Describe(TEXT("ավելացրու 2 փար")), FString(TEXT("add_fixtures 2 'par' floor")));
	}
	{
		const TArray<FStageAICommand> Steps = StageAICommandParser::ParsePlan(TEXT("hang 4 pars on the truss and make them blue"));
		TestTrue(TEXT("Multi-step: the hung add stays one step, then the colour"), Steps.Num() == 2 && Steps[0].Intent == EStageAIIntent::AddFixtures
			&& Steps[0].Mount == EStageAIFixtureMount::Truss && Steps[1].Intent == EStageAIIntent::SetFixture);
	}

	AddInfo(TEXT("Online tool: the mount field, validated at the trust boundary."));
	{
		FStageAICommand Command;
		FText Error;
		TestTrue(TEXT("mount back_truss reads"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"add_fixtures\",\"count\":3,\"item\":\"spot\",\"mount\":\"back_truss\"}"), Command, Error)
			&& Command.Mount == EStageAIFixtureMount::BackTruss && Command.Count.Get(0) == 3);
		FStageAICommand Default;
		TestTrue(TEXT("no mount is the floor"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"add_fixtures\",\"item\":\"spot\"}"), Default, Error)
			&& Default.Mount == EStageAIFixtureMount::Floor);
		FStageAICommand Unknown;
		TestFalse(TEXT("an unknown mount is refused"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"add_fixtures\",\"mount\":\"ceiling\"}"), Unknown, Error));
		TestTrue(TEXT("... saying what it can be"), Error.ToString().Contains(TEXT("front truss")));
		TestTrue(TEXT("The tool definition offers the field"), StageAICommandSchema::MakeToolDefinitionJson().Contains(TEXT("\"front_truss\"")));
	}

	AddInfo(TEXT("Proposal card: APPLY is agreed to knowing where they hang."));
	{
		FStageAICommand Command = StageAICommandParser::Parse(TEXT("add 2 spots on the back truss")).Get(FStageAICommand());
		TestEqual(TEXT("English card"), StageAIToolCatalogue::DescribeStep(Command, EStageAILanguage::English).ToString(), FString(TEXT("Add 2 spots on the back truss")));
		Command.Mount = EStageAIFixtureMount::Floor;
		TestEqual(TEXT("Floor card unchanged"), StageAIToolCatalogue::DescribeStep(Command, EStageAILanguage::English).ToString(), FString(TEXT("Add 2 spots")));
		Command.Mount = EStageAIFixtureMount::Truss;
		TestEqual(TEXT("Armenian card"), StageAIToolCatalogue::DescribeStep(Command, EStageAILanguage::Armenian).ToString(), FString(TEXT("Ավելացնել 2 spots ֆերմայի վրա")));
	}

	AddInfo(TEXT("Spots: the Concert stage's front truss, 16 m along Y at 7 m, with five Beams already hung every 3 m."));
	const TArray<FVector> Front = { FVector(-600.0, -800.0, 700.0), FVector(-600.0, 800.0, 700.0) };
	const TArray<FVector> Beams = { FVector(-600.0, -600.0, 700.0), FVector(-600.0, -300.0, 700.0), FVector(-600.0, 0.0, 700.0),
		FVector(-600.0, 300.0, 700.0), FVector(-600.0, 600.0, 700.0) };
	const TArray<FVector> Empty = StageAICommandParser::FindFreeTrussSpots(Front, false, {});
	TestEqual(TEXT("An empty run: a spot every 50 cm, both ends included"), Empty.Num(), 33);
	const TArray<FVector> Free = StageAICommandParser::FindFreeTrussSpots(Front, false, Beams);
	TestEqual(TEXT("Each Beam takes its spot and the two beside it"), Free.Num(), 18);
	TestEqual(TEXT("... the free spots, in order"), Ys(Free),
		FString(TEXT("-800 -750 -700 -500 -450 -400 -200 -150 -100 100 150 200 400 450 500 700 750 800")));
	TestTrue(TEXT("... on the chord's height"), Algo::AllOf(Free, [](const FVector& Spot) { return Spot.X == -600.0 && Spot.Z == 700.0; }));
	TestEqual(TEXT("Four pars spread evenly between the Beams"), Ys(StageAICommandParser::SpreadOverSpots(Free, 4)), FString(TEXT("-700 -200 200 700")));
	TestEqual(TEXT("All of them when asked for all"), StageAICommandParser::SpreadOverSpots(Free, 18).Num(), 18);
	TestEqual(TEXT("More than there is room for: none"), StageAICommandParser::SpreadOverSpots(Free, 19).Num(), 0);
	TestEqual(TEXT("Zero: none"), StageAICommandParser::SpreadOverSpots(Free, 0).Num(), 0);

	// A fixture on the floor under the truss is not on it.
	const TArray<FVector> Floor = { FVector(-600.0, 0.0, 0.0) };
	TestEqual(TEXT("A fixture far below does not take a spot"), StageAICommandParser::FindFreeTrussSpots(Front, false, Floor).Num(), 33);

	// A closed 2 x 2 m square: 16 spots round it, the start not counted twice, and the corners kept.
	const TArray<FVector> Square = { FVector(0.0, 0.0, 600.0), FVector(200.0, 0.0, 600.0), FVector(200.0, 200.0, 600.0), FVector(0.0, 200.0, 600.0) };
	const TArray<FVector> Round = StageAICommandParser::FindFreeTrussSpots(Square, true, {});
	TestEqual(TEXT("Closed run: perimeter / 50 spots"), Round.Num(), 16);
	TestTrue(TEXT("... the corners among them"), Round.ContainsByPredicate([](const FVector& Spot) { return Spot.Equals(FVector(200.0, 200.0, 600.0)); }));
	TestEqual(TEXT("Open square path: one more spot at the far end"), StageAICommandParser::FindFreeTrussSpots(Square, false, {}).Num(), 13);

	TestEqual(TEXT("A run of one point has no spots"), StageAICommandParser::FindFreeTrussSpots({ FVector::ZeroVector }, false, {}).Num(), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
