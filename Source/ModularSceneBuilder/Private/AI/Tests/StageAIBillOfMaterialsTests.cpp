// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for the assistant's bill of materials answers (STATE.md #114): which prompts ask for the report, the answer by discipline
// with the totals, and rental units (truss sections, deck panels) in count answers. No world.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.AI.BillOfMaterials; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AI/StageAICommandParser.h"
#include "AI/StageAICommandSchema.h"
#include "Data/StageItemTypes.h"
#include "Misc/AutomationTest.h"
#include "Report/StageBillOfMaterials.h"

namespace StageAIBillOfMaterialsTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	const TCHAR* TrussName = TEXT("F34 Truss Run (2 m sections)");
	const TCHAR* DeckName = TEXT("Stage Deck / Riser (2 x 1 m panels)");
	const TCHAR* HeadName = TEXT("Beam 280 (Moving Head)");

	FString IntentOf(const TCHAR* Text)
	{
		const TOptional<FStageAICommand> Command = StageAICommandParser::Parse(Text);
		return Command.IsSet() ? StageAICommandSchema::GetIntentName(Command->Intent) : FString(TEXT("none"));
	}

	FStageBomSource MakeSource(const TCHAR* Path, const TCHAR* Name, const FGameplayTag& Category, double WeightKg, double PowerW, int32 Quantity)
	{
		FStageBomSource Source;
		Source.ItemPath = FSoftObjectPath(Path);
		Source.ItemName = FText::FromString(Name);
		Source.Category = Category;
		Source.UnitWeightKg = WeightKg;
		Source.UnitPowerWatts = PowerW;
		Source.Quantity = Quantity;
		return Source;
	}

	/** Two truss runs (6 + 10 sections), two decks (27 + 27 panels) and nine moving heads, as the scene view holds them. */
	FStageAISceneView MakeScene()
	{
		TArray<FStageBomSource> Sources;
		FStageAISceneView Scene;
		const auto Place = [&Sources, &Scene](const TCHAR* Path, const TCHAR* Name, const FGameplayTag& Category, double WeightKg, double PowerW, int32 Quantity)
		{
			Sources.Add(MakeSource(Path, Name, Category, WeightKg, PowerW, Quantity));
			Scene.ItemNames.Add(Name);
		};
		Place(TEXT("/Game/Items/DA_Truss.DA_Truss"), TrussName, StageCraftTags::Category_Truss_Straight, 14.0, 0.0, 6);
		Place(TEXT("/Game/Items/DA_Truss.DA_Truss"), TrussName, StageCraftTags::Category_Truss_Straight, 14.0, 0.0, 10);
		Place(TEXT("/Game/Items/DA_Deck.DA_Deck"), DeckName, FGameplayTag(), 28.0, 0.0, 27);
		Place(TEXT("/Game/Items/DA_Deck.DA_Deck"), DeckName, FGameplayTag(), 28.0, 0.0, 27);
		for (int32 Index = 0; Index < 9; ++Index)
		{
			Place(TEXT("/Game/Items/DA_Beam.DA_Beam"), HeadName, StageCraftTags::Category_Lighting_MovingHead, 17.0, 470.0, 1);
		}
		Scene.PlacedItems = Scene.ItemNames.Num();
		Scene.BillOfMaterials = StageBillOfMaterials::Build(Sources);
		return Scene;
	}

	FString Answer(const TCHAR* Prompt, const FStageAISceneView& Scene)
	{
		const TOptional<FStageAICommand> Command = StageAICommandParser::Parse(Prompt);
		return Command.IsSet() ? StageAIAnswers::AnswerQuestion(*Command, Scene).ToString() : FString();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAIBillOfMaterialsTest, "StageCraft.AI.BillOfMaterials", StageAIBillOfMaterialsTests::Flags)

bool FStageAIBillOfMaterialsTest::RunTest(const FString& Parameters)
{
	using namespace StageAIBillOfMaterialsTests;

	// Prompts that ask for the report (before #114 these went to the help articles or were not understood).
	for (const TCHAR* Prompt : { TEXT("what is on the bill of materials"), TEXT("what do I need to rent"), TEXT("show me the BOM"), TEXT("list the equipment"),
		TEXT("bill of materials"), TEXT("give me the pick list"), TEXT("what gear do we need to hire"), TEXT("նյութերի ցուցակը") })
	{
		TestEqual(FString::Printf(TEXT("\"%s\" asks for the bill of materials"), Prompt), IntentOf(Prompt), FString(TEXT("bill_of_materials")));
	}
	// Prompts that stay with their own tools.
	TestEqual(TEXT("Exporting it is still an export"), IntentOf(TEXT("export the bill of materials")), FString(TEXT("export")));
	TestEqual(TEXT("Where the panel is stays with the help articles"), IntentOf(TEXT("where is the bill of materials")), FString(TEXT("none")));
	TestNotEqual(TEXT("Opening the panel is not the answer"), IntentOf(TEXT("open the bill of materials panel")), FString(TEXT("bill_of_materials")));
	TestEqual(TEXT("The total weight is still the weight"), IntentOf(TEXT("total weight of the materials")), FString(TEXT("total_weight")));

	// Counting what to rent counts the item, not the words around it.
	const TOptional<FStageAICommand> Truss = StageAICommandParser::Parse(TEXT("how many truss sections do I need"));
	if (TestTrue(TEXT("Truss sections are counted"), Truss.IsSet() && Truss->Intent == EStageAIIntent::CountMatching))
	{
		TestEqual(TEXT("... by the item word alone"), Truss->ItemQuery, FString(TEXT("truss")));
	}
	const TOptional<FStageAICommand> Deck = StageAICommandParser::Parse(TEXT("how many deck panels do we have to hire"));
	if (TestTrue(TEXT("Deck panels are counted"), Deck.IsSet() && Deck->Intent == EStageAIIntent::CountMatching))
	{
		TestEqual(TEXT("... by the item word alone"), Deck->ItemQuery, FString(TEXT("deck")));
	}

	// The answer: totals, then each discipline's lines in rental units.
	const FStageAISceneView Scene = MakeScene();
	const FString Report = Answer(TEXT("what is on the bill of materials"), Scene);
	TestTrue(TEXT("Totals: 79 units in 3 lines"), Report.Contains(TEXT("79 units in 3 lines")));
	TestTrue(TEXT("Truss in sections"), Report.Contains(FString::Printf(TEXT("16 × %s"), TrussName)));
	TestTrue(TEXT("Decks in panels"), Report.Contains(FString::Printf(TEXT("54 × %s"), DeckName)));
	TestTrue(TEXT("Fixtures one by one"), Report.Contains(FString::Printf(TEXT("9 × %s"), HeadName)));
	TestTrue(TEXT("Points to the export"), Report.Contains(TEXT("export the bill of materials")));
	const FString Armenian = Answer(TEXT("նյութերի ցուցակը"), Scene);
	TestTrue(TEXT("Armenian prompts get the Armenian answer"), Armenian.Contains(TEXT("Նյութերի ցուցակ")) && Armenian.Contains(FString::Printf(TEXT("16 × %s"), TrussName)));

	// Rental units join a count answer only where they differ from the placements.
	const FString TrussCount = Answer(TEXT("how many truss sections do I need"), Scene);
	TestTrue(TEXT("Truss: the runs are counted"), TrussCount.Contains(TEXT("2 placed items match \"truss\"")));
	TestTrue(TEXT("... and the sections to rent are named"), TrussCount.Contains(FString::Printf(TEXT("In rental units: 16 × %s"), TrussName)));
	const FString HeadCount = Answer(TEXT("how many moving heads"), Scene);
	TestTrue(TEXT("Moving heads are counted"), HeadCount.Contains(TEXT("9 placed items")));
	TestFalse(TEXT("... with no rental units line (one unit each)"), HeadCount.Contains(TEXT("rental units")));

	// A long discipline is cut short in the chat; the export holds every line.
	FStageAISceneView Long;
	TArray<FStageBomSource> Sources;
	for (int32 Index = 0; Index < 6; ++Index)
	{
		Sources.Add(MakeSource(*FString::Printf(TEXT("/Game/Items/DA_Par%d.DA_Par%d"), Index, Index), *FString::Printf(TEXT("Par %d"), Index),
			StageCraftTags::Category_Lighting_Par, 2.0, 100.0, 1));
	}
	Long.BillOfMaterials = StageBillOfMaterials::Build(Sources);
	TestTrue(TEXT("Six lines: four shown and 2 more"), Answer(TEXT("bill of materials"), Long).Contains(TEXT("and 2 more")));

	TestTrue(TEXT("An empty stage says so"), Answer(TEXT("bill of materials"), FStageAISceneView()).Contains(TEXT("The bill of materials is empty")));

	// The online model reaches the same answer through its tool.
	TestEqual(TEXT("Wire name"), StageAICommandSchema::GetIntentName(EStageAIIntent::BillOfMaterials), FString(TEXT("bill_of_materials")));
	FStageAICommand FromModel;
	FText Error;
	TestTrue(TEXT("The tool input parses"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"bill_of_materials\"}"), FromModel, Error)
		&& FromModel.Intent == EStageAIIntent::BillOfMaterials && FromModel.IsQuestion());
	TestTrue(TEXT("The tool definition offers it"), StageAICommandSchema::MakeToolDefinitionJson().Contains(TEXT("bill_of_materials")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
