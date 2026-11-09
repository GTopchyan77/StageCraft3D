// Copyright Epic Games, Inc. All Rights Reserved.

// Tests of the Vision Inspector (Docs/ADR/0032 §8, STATE.md #77): the stage_inventory tool and request, validation of the model's
// answer (it is untrusted input), real-world specs, catalog matching, CSV, the layout Build in Scene places, and the subsystem's
// flow with a test reader (no network, no API key).
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Vision; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Actors/ModularBaseActor.h"
#include "Components/SelectionComponent.h"
#include "Components/SpawnSystemComponent.h"
#include "Data/BaseItemData.h"
#include "Data/StageItemTypes.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "History/StageEditHistoryComponent.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Sketch/StageSketchBuilder.h"
#include "Subsystems/StageSessionSubsystem.h"
#include "UObject/Package.h"
#include "Vision/StageVisionBom.h"
#include "Vision/StageVisionBomSubsystem.h"
#include "Vision/Tests/StageTestVisionBomBackend.h"

#include <limits>

namespace StageVisionBomTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	/** A Messages API answer with one stage_inventory call whose input is InputJson. */
	FString MakeToolAnswer(const FString& InputJson, const TCHAR* ToolName = TEXT("stage_inventory"))
	{
		return FString::Printf(TEXT("{\"stop_reason\":\"tool_use\",\"content\":[{\"type\":\"text\",\"text\":\"Counting.\"},")
			TEXT("{\"type\":\"tool_use\",\"id\":\"toolu_1\",\"name\":\"%s\",\"input\":%s}]}"), ToolName, *InputJson);
	}

	FString MakeLine(const TCHAR* Kind, double Quantity, const TCHAR* Extra = TEXT(""))
	{
		return FString::Printf(TEXT("{\"kind\":\"%s\",\"quantity\":%g,\"confidence\":0.9,\"side\":\"centre\",\"depth\":\"midstage\",\"level\":\"floor\"%s}"),
			Kind, Quantity, Extra);
	}

	FStageVisionBomLine Line(EStageVisionBomKind Kind, int32 Quantity, EStageVisionBomSide Side = EStageVisionBomSide::Centre,
		EStageVisionBomDepth Depth = EStageVisionBomDepth::Midstage, EStageVisionBomLevel Level = EStageVisionBomLevel::Floor)
	{
		FStageVisionBomLine Result;
		Result.Kind = Kind;
		Result.Quantity = Quantity;
		Result.Confidence = 0.9f;
		Result.Side = Side;
		Result.Depth = Depth;
		Result.Level = Level;
		return Result;
	}

	UBaseItemData* MakeItem(const TCHAR* Name, const FGameplayTag& Category, float WeightKg = 0.f, float PowerW = 0.f)
	{
		UBaseItemData* Item = NewObject<UBaseItemData>(GetTransientPackage());
		Item->DisplayName = FText::FromString(Name);
		Item->CategoryTag = Category;
		Item->ActorClass = AModularBaseActor::StaticClass();
		Item->Specs.WeightKg = WeightKg;
		Item->Specs.PowerDrawWatts = PowerW;
		return Item;
	}

	/** A game world that has begun play, with the spawn, selection and history components. Destroyed with the scope. */
	struct FScopedStage
	{
		UWorld* World = nullptr;
		USpawnSystemComponent* Spawner = nullptr;
		UStageSessionSubsystem* Session = nullptr;

		FScopedStage()
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("StageVisionTestWorld"));
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			World->BeginPlay();
			World->GetWorldSettings()->NotifyBeginPlay();
			AActor* Owner = World->SpawnActor<AActor>();
			for (UClass* ComponentClass : { USpawnSystemComponent::StaticClass(), USelectionComponent::StaticClass(), UStageEditHistoryComponent::StaticClass() })
			{
				UActorComponent* Component = NewObject<UActorComponent>(Owner, ComponentClass);
				Component->RegisterComponent();
			}
			Spawner = Owner->FindComponentByClass<USpawnSystemComponent>();
			Session = UWorld::GetSubsystem<UStageSessionSubsystem>(World);
		}

		~FScopedStage()
		{
			World->BeginTearingDown();
			World->EndPlay(EEndPlayReason::Quit);
			World->DestroyWorld(/*bInformEngineOfWorld*/ false);
			GEngine->DestroyWorldContext(World);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageVisionBomRequestTest, "StageCraft.Vision.Request", StageVisionBomTests::Flags)

bool FStageVisionBomRequestTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Tool;
	TestTrue(TEXT("Tool definition is JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(StageVisionBom::MakeToolDefinitionJson()), Tool) && Tool.IsValid());
	if (!Tool.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("Tool name"), Tool->GetStringField(TEXT("name")), FString(TEXT("stage_inventory")));
	TestTrue(TEXT("Strict"), Tool->GetBoolField(TEXT("strict")));
	const TSharedPtr<FJsonObject> Kind = Tool->GetObjectField(TEXT("input_schema"))->GetObjectField(TEXT("properties"))->GetObjectField(TEXT("items"))
		->GetObjectField(TEXT("items"))->GetObjectField(TEXT("properties"))->GetObjectField(TEXT("kind"));
	TestEqual(TEXT("Every kind is offered"), Kind->GetArrayField(TEXT("enum")).Num(), int32(EStageVisionBomKind::Count));
	for (int32 Index = 0; Index < int32(EStageVisionBomKind::Count); ++Index)
	{
		const EStageVisionBomKind Each = EStageVisionBomKind(Index);
		TestTrue(FString::Printf(TEXT("Kind %d round-trips its wire name"), Index), StageVisionBom::ParseKind(StageVisionBom::GetKindName(Each)) == TOptional<EStageVisionBomKind>(Each));
		TestFalse(FString::Printf(TEXT("Kind %d has a display name"), Index), StageVisionBom::GetKindDisplayName(Each).IsEmpty());
	}
	TestFalse(TEXT("Unknown kind"), StageVisionBom::ParseKind(TEXT("TRUSS_STRAIGHT")).IsSet());

	FStageAIModelOptions Options;
	Options.Model = TEXT("claude-opus-5-5");
	Options.MaxTokens = 1024;
	Options.Effort = TEXT("low");
	TSharedPtr<FJsonObject> Body;
	TestTrue(TEXT("Body is JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(StageVisionBom::BuildRequestBody(Options, TEXT("QUJD"))), Body) && Body.IsValid());
	if (!Body.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("Model"), Body->GetStringField(TEXT("model")), Options.Model);
	TestEqual(TEXT("At least 4096 tokens to list a big rig"), int32(Body->GetNumberField(TEXT("max_tokens"))), 4096);
	TestEqual(TEXT("Effort never below high"), Body->GetObjectField(TEXT("output_config"))->GetStringField(TEXT("effort")), FString(TEXT("high")));
	TestEqual(TEXT("One tool"), Body->GetArrayField(TEXT("tools")).Num(), 1);
	const TSharedPtr<FJsonObject> Image = Body->GetArrayField(TEXT("messages"))[0]->AsObject()->GetArrayField(TEXT("content"))[0]->AsObject();
	TestEqual(TEXT("Image block"), Image->GetStringField(TEXT("type")), FString(TEXT("image")));
	TestEqual(TEXT("JPEG"), Image->GetObjectField(TEXT("source"))->GetStringField(TEXT("media_type")), FString(TEXT("image/jpeg")));
	TestEqual(TEXT("Data"), Image->GetObjectField(TEXT("source"))->GetStringField(TEXT("data")), FString(TEXT("QUJD")));
	Options.Effort = TEXT("max");
	FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(StageVisionBom::BuildRequestBody(Options, TEXT("QUJD"))), Body);
	TestEqual(TEXT("Max effort is kept"), Body->GetObjectField(TEXT("output_config"))->GetStringField(TEXT("effort")), FString(TEXT("max")));
	TestTrue(TEXT("System prompt names the tool"), StageVisionBom::GetSystemPrompt().Contains(TEXT("stage_inventory")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageVisionBomParseTest, "StageCraft.Vision.Parse", StageVisionBomTests::Flags)

bool FStageVisionBomParseTest::RunTest(const FString& Parameters)
{
	using namespace StageVisionBomTests;
	using EStatus = FStageVisionBomResult::EStatus;

	// A good answer, every field.
	const FString Good = MakeToolAnswer(FString::Printf(TEXT("{\"items\":[%s,%s],\"stage\":{\"width_m\":14,\"depth_m\":9,\"deck_height_m\":1.5},\"notes\":\"Rear truss hidden.\"}"),
		*MakeLine(TEXT("moving_head_spot"), 12, TEXT(",\"side\":\"stage_left\",\"depth\":\"upstage\",\"level\":\"flown\",\"notes\":\"Probably Robe\"")),
		TEXT("{\"kind\":\"truss_straight\",\"quantity\":3,\"confidence\":0.7,\"length_m\":12,\"side\":\"full_width\",\"depth\":\"downstage\",\"level\":\"flown\"}")));
	const FStageVisionBomResult Result = StageVisionBom::ParseResponseBody(200, Good);
	if (!TestTrue(TEXT("Inventory"), Result.Status == EStatus::Inventory) || !TestEqual(TEXT("Two lines"), Result.Lines.Num(), 2))
	{
		return false;
	}
	const FStageVisionBomLine& Spots = Result.Lines[0];
	TestTrue(TEXT("Kind"), Spots.Kind == EStageVisionBomKind::MovingHeadSpot);
	TestEqual(TEXT("Quantity"), Spots.Quantity, 12);
	TestTrue(TEXT("Side"), Spots.Side == EStageVisionBomSide::StageLeft);
	TestTrue(TEXT("Depth"), Spots.Depth == EStageVisionBomDepth::Upstage);
	TestTrue(TEXT("Level"), Spots.Level == EStageVisionBomLevel::Flown);
	TestEqual(TEXT("Notes"), Spots.Notes, FString(TEXT("Probably Robe")));
	TestFalse(TEXT("Confident line needs no review"), Spots.NeedsReview());
	TestEqual(TEXT("Truss length"), Result.Lines[1].LengthM, 12.0);
	TestEqual(TEXT("Stage width"), Result.Stage.WidthM, 14.0);
	TestEqual(TEXT("Deck height"), Result.Stage.DeckHeightM, 1.5);
	TestEqual(TEXT("Model notes"), Result.Text, FString(TEXT("Rear truss hidden.")));
	TestEqual(TEXT("Nothing dropped"), Result.Dropped, 0);

	// Hostile and sloppy values: dropped or clamped, never trusted.
	const FString Sloppy = MakeToolAnswer(FString::Printf(TEXT("{\"items\":[%s,%s,%s,%s,%s,%s,%s,\"text\"]}"),
		*MakeLine(TEXT("flamethrower"), 2),
		*MakeLine(TEXT("strobe"), 0),
		*MakeLine(TEXT("strobe"), -4),
		*MakeLine(TEXT("blinder"), 100000),
		TEXT("{\"kind\":\"par_wash\",\"quantity\":2.6,\"confidence\":7,\"length_m\":-1,\"width_m\":150,\"height_m\":2,\"side\":\"backstage\",\"depth\":\"sky\",\"level\":\"moon\",\"notes\":\"line1\\nline2\\t=SUM(A1)\"}"),
		TEXT("{\"kind\":\"laser\",\"quantity\":1,\"side\":\"centre\",\"depth\":\"midstage\",\"level\":\"floor\"}"),
		TEXT("{\"quantity\":1}")));
	const FStageVisionBomResult Cleaned = StageVisionBom::ParseResponseBody(200, Sloppy);
	if (!TestTrue(TEXT("Sloppy answer still reads"), Cleaned.Status == EStatus::Inventory) || !TestEqual(TEXT("Three usable lines"), Cleaned.Lines.Num(), 3))
	{
		return false;
	}
	TestEqual(TEXT("Unknown kind, zero, negative, missing kind and a non-object are dropped"), Cleaned.Dropped, 5);
	TestEqual(TEXT("Quantity capped"), Cleaned.Lines[0].Quantity, StageVisionBom::MaxQuantity);
	const FStageVisionBomLine& Par = Cleaned.Lines[1];
	TestEqual(TEXT("Quantity rounded"), Par.Quantity, 3);
	TestEqual(TEXT("Confidence clamped"), Par.Confidence, 1.f);
	TestEqual(TEXT("Negative length ignored"), Par.LengthM, 0.0);
	TestEqual(TEXT("Huge width ignored"), Par.WidthM, 0.0);
	TestEqual(TEXT("Height kept"), Par.HeightM, 2.0);
	TestTrue(TEXT("Unknown side defaults"), Par.Side == EStageVisionBomSide::Centre);
	TestTrue(TEXT("Unknown depth defaults"), Par.Depth == EStageVisionBomDepth::Midstage);
	TestTrue(TEXT("Unknown level defaults"), Par.Level == EStageVisionBomLevel::Floor);
	TestFalse(TEXT("Control characters removed from notes"), Par.Notes.Contains(TEXT("\n")) || Par.Notes.Contains(TEXT("\t")));
	TestEqual(TEXT("Missing confidence is 0"), Cleaned.Lines[2].Confidence, 0.f);
	TestTrue(TEXT("... and needs review"), Cleaned.Lines[2].NeedsReview());

	// Long notes are capped; more than MaxLines lines are dropped.
	FString Many;
	for (int32 Index = 0; Index < StageVisionBom::MaxLines + 5; ++Index)
	{
		Many += (Index ? TEXT(",") : TEXT("")) + MakeLine(TEXT("strobe"), 1, *FString::Printf(TEXT(",\"notes\":\"%s\""), *FString::ChrN(400, TEXT('x'))));
	}
	const FStageVisionBomResult Capped = StageVisionBom::ParseResponseBody(200, MakeToolAnswer(FString::Printf(TEXT("{\"items\":[%s]}"), *Many)));
	TestEqual(TEXT("At most MaxLines"), Capped.Lines.Num(), StageVisionBom::MaxLines);
	TestEqual(TEXT("The rest dropped"), Capped.Dropped, 5);
	TestEqual(TEXT("Notes capped"), Capped.Lines[0].Notes.Len(), StageVisionBom::MaxNotesLength);

	// Shapes that are not the tool's.
	TestTrue(TEXT("Unknown field refused"), StageVisionBom::ParseResponseBody(200, MakeToolAnswer(TEXT("{\"items\":[],\"price\":5}"))).Status == EStatus::Failed);
	TestTrue(TEXT("No items refused"), StageVisionBom::ParseResponseBody(200, MakeToolAnswer(TEXT("{\"notes\":\"x\"}"))).Status == EStatus::Failed);
	TestTrue(TEXT("Empty list is an empty inventory"), StageVisionBom::ParseResponseBody(200, MakeToolAnswer(TEXT("{\"items\":[]}"))).Status == EStatus::Inventory);
	TestTrue(TEXT("Another tool is ignored"), StageVisionBom::ParseResponseBody(200, MakeToolAnswer(TEXT("{\"items\":[]}"), TEXT("sketch_plan"))).Status == EStatus::Text);
	TestTrue(TEXT("Text answer"), StageVisionBom::ParseResponseBody(200, TEXT("{\"stop_reason\":\"end_turn\",\"content\":[{\"type\":\"text\",\"text\":\"This is a cat.\"}]}")).Status == EStatus::Text);
	TestTrue(TEXT("Refusal"), StageVisionBom::ParseResponseBody(200, TEXT("{\"stop_reason\":\"refusal\",\"content\":[]}")).Status == EStatus::Refused);
	const FStageVisionBomResult Unauthorized = StageVisionBom::ParseResponseBody(401, TEXT("{\"type\":\"error\",\"error\":{\"type\":\"authentication_error\",\"message\":\"invalid x-api-key\"}}"));
	TestTrue(TEXT("HTTP error fails"), Unauthorized.Status == EStatus::Failed && Unauthorized.Error.ToString().Contains(TEXT("invalid x-api-key")));
	TestTrue(TEXT("Error object with 200 fails"), StageVisionBom::ParseResponseBody(200, TEXT("{\"error\":{\"message\":\"overloaded\"}}")).Status == EStatus::Failed);
	TestTrue(TEXT("Malformed"), StageVisionBom::ParseResponseBody(200, TEXT("not json")).Status == EStatus::Failed);
	TestTrue(TEXT("Odd content blocks are skipped"), StageVisionBom::ParseResponseBody(200, TEXT("{\"stop_reason\":\"end_turn\",\"content\":[\"x\",7,{\"type\":\"text\",\"text\":\"Hi.\"}]}")).Status == EStatus::Text);
	TestTrue(TEXT("No content"), StageVisionBom::ParseResponseBody(200, TEXT("{\"stop_reason\":\"end_turn\"}")).Status == EStatus::Failed);
	const FStageVisionBomResult OutOfRoom = StageVisionBom::ParseResponseBody(200, TEXT("{\"stop_reason\":\"max_tokens\",\"content\":[]}"));
	TestTrue(TEXT("Out of room"), OutOfRoom.Status == EStatus::Failed && OutOfRoom.Error.ToString().Contains(TEXT("room")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageVisionBomSpecTest, "StageCraft.Vision.Spec", StageVisionBomTests::Flags)

bool FStageVisionBomSpecTest::RunTest(const FString& Parameters)
{
	using namespace StageVisionBomTests;

	const auto Sections = [](double Length) { const FStageVisionBomTrussSections S = StageVisionBom::SplitTrussRun(Length); return FString::Printf(TEXT("%d,%d,%d,%d"), S.ThreeMetre, S.TwoMetre, S.OneMetre, S.HalfMetre); };
	TestEqual(TEXT("12 m"), Sections(12.0), FString(TEXT("4,0,0,0")));
	TestEqual(TEXT("7.5 m"), Sections(7.5), FString(TEXT("2,0,1,1")));
	TestEqual(TEXT("2 m"), Sections(2.0), FString(TEXT("0,1,0,0")));
	TestEqual(TEXT("5 m"), Sections(5.0), FString(TEXT("1,1,0,0")));
	TestEqual(TEXT("0.2 m rounds up"), Sections(0.2), FString(TEXT("0,0,0,1")));
	TestEqual(TEXT("10.1 m rounds up to 10.5"), Sections(10.1), FString(TEXT("3,0,1,1")));
	TestEqual(TEXT("0 m"), StageVisionBom::SplitTrussRun(0.0).Num(), 0);
	TestEqual(TEXT("Negative"), StageVisionBom::SplitTrussRun(-3.0).Num(), 0);
	TestEqual(TEXT("NaN"), StageVisionBom::SplitTrussRun(std::numeric_limits<double>::quiet_NaN()).Num(), 0);

	TestEqual(TEXT("6 x 3.5 m LED"), StageVisionBom::GetLedPanelCount(6.0, 3.5), 84);
	TestEqual(TEXT("LED without height"), StageVisionBom::GetLedPanelCount(6.0, 0.0), 0);

	FStageVisionBomLine Truss = Line(EStageVisionBomKind::TrussStraight, 2);
	Truss.LengthM = 7.5;
	TestEqual(TEXT("Truss spec"), StageVisionBom::DescribeSpec(Truss).ToString(), FString(TEXT("2 runs of 7.5 m: 4 x 3 m, 2 x 1 m, 2 x 0.5 m sections")));
	FStageVisionBomLine Led = Line(EStageVisionBomKind::LedWall, 1);
	Led.WidthM = 6.0;
	Led.HeightM = 3.5;
	TestEqual(TEXT("LED spec"), StageVisionBom::DescribeSpec(Led).ToString(), FString(TEXT("6 x 3.5 m: 84 panels of 500 x 500 mm")));
	FStageVisionBomLine Deck = Line(EStageVisionBomKind::Deck, 1);
	Deck.WidthM = 10.0;
	Deck.LengthM = 6.0;
	TestTrue(TEXT("Deck spec counts 2 x 1 m decks"), StageVisionBom::DescribeSpec(Deck).ToString().Contains(TEXT("30 decks")));
	FStageVisionBomLine Tower = Line(EStageVisionBomKind::TrussTower, 4);
	Tower.HeightM = 6.0;
	TestEqual(TEXT("Tower spec"), StageVisionBom::DescribeSpec(Tower).ToString(), FString(TEXT("6 m high")));
	TestTrue(TEXT("No spec for a strobe"), StageVisionBom::DescribeSpec(Line(EStageVisionBomKind::Strobe, 4)).IsEmpty());
	TestTrue(TEXT("No spec without a length"), StageVisionBom::DescribeSpec(Line(EStageVisionBomKind::TrussStraight, 4)).IsEmpty());

	// Totals: discrete matched items count weight and power; spans and unmatched lines do not.
	UBaseItemData* Spot = MakeItem(TEXT("Spot 1"), StageCraftTags::Category_Lighting_MovingHead, 25.f, 700.f);
	UBaseItemData* TrussItem = MakeItem(TEXT("Truss"), StageCraftTags::Category_Truss_Straight, 500.f, 0.f);
	TArray<FStageVisionBomLine> Lines = { Line(EStageVisionBomKind::MovingHeadSpot, 4), Truss, Line(EStageVisionBomKind::Laser, 1) };
	Lines[0].Match = Spot;
	Lines[1].Match = TrussItem;
	Lines[2].Confidence = 0.2f;
	const FStageVisionBomTotals Totals = StageVisionBom::ComputeTotals(Lines);
	TestEqual(TEXT("Weight"), Totals.WeightKg, 100.0);
	TestEqual(TEXT("Power"), Totals.PowerW, 2800.0);
	TestEqual(TEXT("Matched"), Totals.MatchedLines, 2);
	TestEqual(TEXT("Unmatched"), Totals.UnmatchedLines, 1);
	TestEqual(TEXT("To review"), Totals.LinesToReview, 1);

	// CSV: a header, one row per line, quoting, and no formula injection from the model's notes.
	Lines[0].Notes = TEXT("=HYPERLINK(\"http://x\")");
	Lines[2].Notes = TEXT("red, \"green\"");
	const FString Csv = StageVisionBom::ToCsv(Lines);
	TArray<FString> Rows;
	Csv.ParseIntoArray(Rows, TEXT("\r\n"));
	TestEqual(TEXT("Header + 3 rows"), Rows.Num(), 4);
	TestTrue(TEXT("Header"), Rows[0].StartsWith(TEXT("Kind,Quantity,Position")));
	TestTrue(TEXT("Formula neutralised"), Rows[1].Contains(TEXT("\"'=HYPERLINK(\"\"http://x\"\")\"")));
	TestTrue(TEXT("Weight and power each"), Rows[1].Contains(TEXT(",25.0,700,")));
	TestTrue(TEXT("Commas and quotes escaped"), Rows[3].EndsWith(TEXT("\"red, \"\"green\"\"\"")));
	TestTrue(TEXT("Empty bill is a header"), StageVisionBom::ToCsv({}).Equals(TEXT("Kind,Quantity,Position,Spec,Confidence,Catalog item,Weight each (kg),Power each (W),Notes\r\n")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageVisionBomMatchTest, "StageCraft.Vision.Match", StageVisionBomTests::Flags)

bool FStageVisionBomMatchTest::RunTest(const FString& Parameters)
{
	using namespace StageVisionBomTests;

	UBaseItemData* Wash = MakeItem(TEXT("Wash Zoom 19"), StageCraftTags::Category_Lighting_MovingHead);
	UBaseItemData* Spot = MakeItem(TEXT("Profile Spot 700"), StageCraftTags::Category_Lighting_MovingHead);
	UBaseItemData* Strobe = MakeItem(TEXT("LED Strobe 1000"), StageCraftTags::Category_Lighting_Strobe);
	UBaseItemData* Blinder = MakeItem(TEXT("LED Blinder 4-Cell"), StageCraftTags::Category_Lighting_Strobe);
	UBaseItemData* Profile = MakeItem(TEXT("Ellipsoidal Profile"), StageCraftTags::Category_Lighting_Conventional);
	UBaseItemData* Stagehand = MakeItem(TEXT("Stagehand"), StageCraftTags::Category_People_Crew);
	UBaseItemData* FohSound = MakeItem(TEXT("FOH Sound Position"), StageCraftTags::Category_People_Crew);
	UBaseItemData* Imported = MakeItem(TEXT("My Beam"), StageCraftTags::Category_Imported);
	const TArray<const UBaseItemData*> Catalog = { Imported, Wash, Spot, Strobe, Blinder, Profile, Stagehand, FohSound, nullptr };

	TArray<FStageVisionBomLine> Lines = {
		Line(EStageVisionBomKind::MovingHeadSpot, 1), Line(EStageVisionBomKind::MovingHeadBeam, 1), Line(EStageVisionBomKind::Strobe, 1),
		Line(EStageVisionBomKind::Blinder, 1), Line(EStageVisionBomKind::Followspot, 1), Line(EStageVisionBomKind::MixingDesk, 1),
		Line(EStageVisionBomKind::LightingDesk, 1), Line(EStageVisionBomKind::Other, 1), Line(EStageVisionBomKind::Deck, 1) };
	StageVisionBom::MatchCatalog(Lines, Catalog);
	TestTrue(TEXT("Spot prefers the item named Spot"), Lines[0].Match.Get() == Spot);
	TestTrue(TEXT("Beam falls back to the first moving head, never the imported one"), Lines[1].Match.Get() == Wash);
	TestTrue(TEXT("Strobe is the strobe, not the blinder"), Lines[2].Match.Get() == Strobe);
	TestTrue(TEXT("Blinder is the blinder"), Lines[3].Match.Get() == Blinder);
	TestFalse(TEXT("A followspot is not any profile"), Lines[4].Match.IsValid());
	TestTrue(TEXT("Mixing desk is the FOH sound position"), Lines[5].Match.Get() == FohSound);
	TestFalse(TEXT("Lighting desk needs the FOH lighting position, not the stagehand"), Lines[6].Match.IsValid());
	TestFalse(TEXT("Other never matches"), Lines[7].Match.IsValid());
	TestFalse(TEXT("No deck in this catalog"), Lines[8].Match.IsValid());

	// A new catalog re-matches (an old match is cleared).
	StageVisionBom::MatchCatalog(Lines, {});
	TestFalse(TEXT("Empty catalog clears matches"), Lines[0].Match.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageVisionBomLayoutTest, "StageCraft.Vision.Layout", StageVisionBomTests::Flags)

bool FStageVisionBomLayoutTest::RunTest(const FString& Parameters)
{
	using namespace StageVisionBomTests;

	// The stage estimate is clamped; the defaults stand in for what was not given.
	FStageVisionBomStage Stage;
	FStageVisionBomLayout Layout = StageVisionBom::MakeLayout(Stage, false);
	TestEqual(TEXT("Default width"), Layout.StageWidthCm, 1200.0);
	TestEqual(TEXT("No deck, no deck height"), Layout.DeckHeightCm, 0.0);
	Stage.WidthM = 90.0;
	Stage.DepthM = 1.0;
	Stage.DeckHeightM = 1.8;
	Layout = StageVisionBom::MakeLayout(Stage, true);
	TestEqual(TEXT("Width clamped"), Layout.StageWidthCm, 4000.0);
	TestEqual(TEXT("Depth clamped"), Layout.StageDepthCm, 300.0);
	TestEqual(TEXT("Deck height"), Layout.DeckHeightCm, 180.0);
	TestEqual(TEXT("Truss clears the deck"), Layout.TrussHeightCm, 630.0);

	Layout = FStageVisionBomLayout();
	Layout.DeckHeightCm = 120.0;
	UBaseItemData* Spot = MakeItem(TEXT("Spot"), StageCraftTags::Category_Lighting_MovingHead);
	UBaseItemData* Desk = MakeItem(TEXT("FOH Sound Position"), StageCraftTags::Category_People_Crew);
	UBaseItemData* Sub = MakeItem(TEXT("Sub"), StageCraftTags::Category_Audio_Subwoofer);
	FStageVisionBomLine Truss = Line(EStageVisionBomKind::TrussStraight, 2, EStageVisionBomSide::FullWidth, EStageVisionBomDepth::Downstage, EStageVisionBomLevel::Flown);
	Truss.LengthM = 8.0;
	TArray<FStageVisionBomLine> Lines = {
		Line(EStageVisionBomKind::Deck, 1),
		Truss,
		Line(EStageVisionBomKind::MovingHeadSpot, 6, EStageVisionBomSide::FullWidth, EStageVisionBomDepth::Upstage, EStageVisionBomLevel::Flown),
		Line(EStageVisionBomKind::MixingDesk, 1, EStageVisionBomSide::Centre, EStageVisionBomDepth::FrontOfHouse),
		Line(EStageVisionBomKind::Subwoofer, 30, EStageVisionBomSide::StageLeft, EStageVisionBomDepth::Downstage, EStageVisionBomLevel::Deck),
		Line(EStageVisionBomKind::Barricade, 1),
		Line(EStageVisionBomKind::Laser, 2) };
	Lines[2].Match = Spot;
	Lines[3].Match = Desk;
	Lines[4].Match = Sub;
	const FStageVisionBomBuildInput Input = StageVisionBom::MakeBuildInput(Lines, Layout);

	if (!TestEqual(TEXT("Deck + 2 truss strokes"), Input.Strokes.Num(), 3))
	{
		return false;
	}
	TestTrue(TEXT("Deck is a closed shape"), Input.Strokes[0].Kind == EStageSketchStrokeKind::Shape && Input.Strokes[0].bClosed && Input.Strokes[0].Points.Num() == 4);
	TestEqual(TEXT("Deck height"), Input.Strokes[0].DeckHeight, 120.f);
	TestTrue(TEXT("Deck spans the stage"), Input.Strokes[0].Points[2].Equals(FVector2D(400.0, 600.0)));
	TestTrue(TEXT("First run downstage"), Input.Strokes[1].Points[0].Equals(FVector2D(-350.0, -400.0)));
	TestTrue(TEXT("Second run upstage"), Input.Strokes[2].Points[1].Equals(FVector2D(350.0, 400.0)));

	TestEqual(TEXT("6 spots + 1 desk + 30 subs"), Input.Items.Num(), 37);
	for (int32 Index = 0; Index < 6; ++Index)
	{
		const FTransform& Hung = Input.Items[Index].Transform;
		TestEqual(TEXT("Spot hangs on the upstage run"), Hung.GetLocation().X, 350.0);
		TestEqual(TEXT("... at truss height"), Hung.GetLocation().Z, 600.0);
		TestTrue(TEXT("... within the run"), FMath::Abs(Hung.GetLocation().Y) <= 375.0 + UE_KINDA_SMALL_NUMBER);
		TestTrue(TEXT("... upside down"), FMath::IsNearlyEqual(FMath::Abs(Hung.Rotator().Roll), 180.0, 0.01));
	}
	const FTransform& DeskAt = Input.Items[6].Transform;
	TestEqual(TEXT("Desk at front of house"), DeskAt.GetLocation(), FVector(-1900.0, 0.0, 0.0));
	TestTrue(TEXT("Desk faces the stage"), FMath::IsNearlyZero(DeskAt.Rotator().Yaw, 0.01));
	const FTransform& FirstSub = Input.Items[7].Transform;
	TestTrue(TEXT("Subs on stage left"), FirstSub.GetLocation().Y >= 200.0 - UE_KINDA_SMALL_NUMBER);
	TestEqual(TEXT("Subs on the deck"), FirstSub.GetLocation().Z, 120.0);
	TestTrue(TEXT("Subs face the audience"), FMath::IsNearlyEqual(FMath::Abs(FirstSub.Rotator().Yaw), 180.0, 0.01));
	TestTrue(TEXT("A full row continues further upstage"), Input.Items.Last().Transform.GetLocation().X > FirstSub.GetLocation().X);
	TestTrue(TEXT("Barricade warned"), Input.Warnings.ContainsByPredicate([](const FText& Text) { return Text.ToString().Contains(TEXT("barricade")); }));
	TestTrue(TEXT("Unmatched laser warned"), Input.Warnings.ContainsByPredicate([](const FText& Text) { return Text.ToString().Contains(TEXT("not in the catalog")); }));

	// Caps: at most MaxPlacedItems items and 8 truss runs, with warnings.
	TArray<FStageVisionBomLine> Huge = { Line(EStageVisionBomKind::Strobe, 500), Line(EStageVisionBomKind::TrussStraight, 12) };
	Huge[0].Match = Spot;
	const FStageVisionBomBuildInput Capped = StageVisionBom::MakeBuildInput(Huge, Layout);
	TestEqual(TEXT("Items capped"), Capped.Items.Num(), StageVisionBom::MaxPlacedItems);
	TestEqual(TEXT("Runs capped"), Capped.Strokes.Num(), 8);
	TestEqual(TEXT("Two warnings"), Capped.Warnings.Num(), 2);
	TestEqual(TEXT("Empty bill builds nothing"), StageVisionBom::MakeBuildInput({}, Layout).Items.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageVisionBomFlowTest, "StageCraft.Vision.Flow", StageVisionBomTests::Flags)

bool FStageVisionBomFlowTest::RunTest(const FString& Parameters)
{
	using namespace StageVisionBomTests;

	FScopedStage Stage;
	UStageVisionBomSubsystem* Vision = UWorld::GetSubsystem<UStageVisionBomSubsystem>(Stage.World);
	if (!TestNotNull(TEXT("Vision subsystem in a game world"), Vision) || !TestNotNull(TEXT("Spawner"), Stage.Spawner))
	{
		return false;
	}
	UStageTestVisionBomBackend* Reader = NewObject<UStageTestVisionBomBackend>(Vision);
	Vision->SetBackendForTesting(Reader);
	int32 Changes = 0;
	const FDelegateHandle Handle = Vision->OnChanged.AddLambda([&Changes]() { ++Changes; });
	const TArray<uint8> Jpeg = { 0xFF, 0xD8, 0xFF, 0xD9 };
	FText Error;

	// Not available: refused with the reader's reason, nothing sent.
	Reader->bAvailable = false;
	TestFalse(TEXT("Unavailable reader refuses"), Vision->InspectJpegForTesting(Jpeg, TEXT("stage.jpg"), Error));
	TestFalse(TEXT("... with its reason"), Error.IsEmpty());
	TestFalse(TEXT("Missing file refused"), Vision->Inspect(TEXT("Z:/nothing.jpg"), Error));
	Reader->bAvailable = true;
	TestFalse(TEXT("Not a photo"), Vision->Inspect(FPaths::Combine(FPaths::ProjectDir(), TEXT("Rules.md")), Error));
	TestTrue(TEXT("Nothing read"), Vision->GetState() == EStageVisionBomState::Idle && Reader->Reads == 0);

	// A good answer.
	Reader->Body = MakeToolAnswer(FString::Printf(TEXT("{\"items\":[%s,%s,%s],\"stage\":{\"width_m\":10,\"depth_m\":6}}"),
		*MakeLine(TEXT("deck"), 1),
		*MakeLine(TEXT("moving_head_spot"), 4, TEXT(",\"level\":\"flown\"")),
		*MakeLine(TEXT("followspot"), 2)));
	TestTrue(TEXT("Read starts"), Vision->InspectJpegForTesting(Jpeg, TEXT("stage.jpg"), Error));
	if (!TestTrue(TEXT("Ready"), Vision->GetState() == EStageVisionBomState::Ready) || !TestEqual(TEXT("Three lines"), Vision->GetLines().Num(), 3))
	{
		return false;
	}
	TestEqual(TEXT("Source"), Vision->GetSourceName(), FString(TEXT("stage.jpg")));
	TestTrue(TEXT("Message counts the lines"), Vision->GetMessage().ToString().Contains(TEXT("3 lines")));
	TestTrue(TEXT("Changes announced"), Changes > 0);
	TestEqual(TEXT("Stage estimate kept"), Vision->GetLayout().StageWidthCm, 1000.0);
	TestEqual(TEXT("Deck in the bill gives a deck height"), Vision->GetLayout().DeckHeightCm, 120.0);

	// Editing the bill.
	TestTrue(TEXT("Quantity set"), Vision->SetLineQuantity(1, 8) && Vision->GetLines()[1].Quantity == 8);
	TestTrue(TEXT("Quantity clamped"), Vision->SetLineQuantity(1, 99999) && Vision->GetLines()[1].Quantity == StageVisionBom::MaxQuantity);
	TestTrue(TEXT("Zero clamps to one"), Vision->SetLineQuantity(1, 0) && Vision->GetLines()[1].Quantity == 1);
	TestFalse(TEXT("Bad index"), Vision->SetLineQuantity(7, 2));
	Vision->SetLineQuantity(1, 4);

	// Nothing matched yet (a test world has no catalog): nothing to build.
	FStageSketchBuildPlan Plan;
	FText Reason;
	TestFalse(TEXT("Unmatched bill builds nothing"), Vision->MakeBuildPlan(Plan, Reason));
	TestFalse(TEXT("... with a reason"), Reason.IsEmpty());

	// Matched: the spots are placed for real through the sketch builder, all of them.
	UBaseItemData* Spot = MakeItem(TEXT("Test Spot"), StageCraftTags::Category_Lighting_MovingHead);
	Vision->MatchCatalogForTesting({ Spot });
	TestTrue(TEXT("Spots matched"), Vision->GetLines()[1].Match.Get() == Spot);
	TestTrue(TEXT("Plan made"), Vision->MakeBuildPlan(Plan, Reason));
	TestEqual(TEXT("Four items in the plan"), Plan.Items.Num(), 4);
	TestTrue(TEXT("Unmatched followspot warned"), Plan.Warnings.ContainsByPredicate([](const FText& Text) { return Text.ToString().Contains(TEXT("not in the catalog")); }));
	const FStageSketchBuildOutcome Outcome = StageSketchBuilder::Execute(Plan, *Stage.Spawner,
		[](const UBaseItemData&, int32) { return FStageEconomyResultInfo::Ok(); });
	TestTrue(TEXT("Built"), Outcome.IsSuccess());
	TestEqual(TEXT("Four spots placed"), Outcome.ItemCount, 4);
	TestEqual(TEXT("In the session"), Stage.Session ? Stage.Session->GetPlacedItems().Num() : 0, 4);

	// CSV saved where asked.
	const FString CsvPath = FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("VisionBom"), TEXT("bill.csv"));
	TestTrue(TEXT("CSV saved"), Vision->SaveCsv(CsvPath, Error));
	FString Saved;
	TestTrue(TEXT("CSV readable"), FFileHelper::LoadFileToString(Saved, *CsvPath) && Saved.Contains(TEXT("Test Spot")));
	IFileManager::Get().DeleteDirectory(*FPaths::GetPath(CsvPath), false, true);

	// A late answer to a cancelled read changes nothing.
	Reader->bAnswerNow = false;
	TestTrue(TEXT("Second read starts"), Vision->InspectJpegForTesting(Jpeg, TEXT("other.jpg"), Error));
	TestTrue(TEXT("Reading"), Vision->GetState() == EStageVisionBomState::Reading && Vision->IsBusy());
	TestFalse(TEXT("No edits while reading"), Vision->RemoveLine(0));
	FText Busy;
	TestFalse(TEXT("One photo at a time"), Vision->InspectJpegForTesting(Jpeg, TEXT("third.jpg"), Busy));
	Vision->Cancel();
	TestTrue(TEXT("Back to the previous bill"), Vision->GetState() == EStageVisionBomState::Ready && Vision->GetSourceName() == TEXT("stage.jpg"));
	TestFalse(TEXT("Cancelled request is not answered"), Reader->AnswerHeld());

	// Failures keep the reason; the bill can be removed line by line and cleared.
	Reader->bAnswerNow = true;
	Reader->HttpStatus = 529;
	Reader->Body = TEXT("{\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"Overloaded\"}}");
	TestTrue(TEXT("Third read starts"), Vision->InspectJpegForTesting(Jpeg, TEXT("third.jpg"), Error));
	TestTrue(TEXT("Failed"), Vision->GetState() == EStageVisionBomState::Failed && Vision->GetMessage().ToString().Contains(TEXT("Overloaded")));
	Reader->HttpStatus = 200;
	Reader->Body = TEXT("{\"stop_reason\":\"end_turn\",\"content\":[{\"type\":\"text\",\"text\":\"This is a cat.\"}]}");
	TestTrue(TEXT("Fourth read starts"), Vision->InspectJpegForTesting(Jpeg, TEXT("cat.jpg"), Error));
	TestTrue(TEXT("Words, not a bill"), Vision->GetState() == EStageVisionBomState::Failed && Vision->GetMessage().ToString().Contains(TEXT("cat")));
	TestTrue(TEXT("Line removed"), Vision->RemoveLine(0) && Vision->GetLines().Num() == 2);
	Vision->Clear();
	TestTrue(TEXT("Cleared"), Vision->GetLines().IsEmpty() && Vision->GetState() == EStageVisionBomState::Idle);
	TestFalse(TEXT("Nothing to save"), Vision->SaveCsv(CsvPath, Error));

	Vision->OnChanged.Remove(Handle);
	Vision->SetBackendForTesting(nullptr);
	return true;
}

#endif
