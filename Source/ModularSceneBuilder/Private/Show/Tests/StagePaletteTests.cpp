// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for palettes and palette references (Docs/ADR/0028-palettes-and-references.md): the pure rules (kinds, resolution order,
// sanitizing, starter palettes), references on real fixtures through the programmer and cues (re-recording a palette changes the
// cue that uses it, a missing palette falls back to the stored value), the undo step, and the scene format v7 sections. The
// controller's apply / record / delete requests and the panel are exercised by the scripted -game run (STATE.md #60).
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Show.Palette; Automation RunTests StageCraft.Scene.PaletteData; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Actors/LightingFixtureActor.h"
#include "Data/LightingFixtureData.h"
#include "Data/StageParameterTypes.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "History/StageItemSnapshot.h"
#include "History/StageShowCommands.h"
#include "Misc/AutomationTest.h"
#include "Scene/StageSceneStore.h"
#include "Show/StageFixtureCells.h"
#include "Subsystems/ShowControlSubsystem.h"
#include "Subsystems/StageProgrammerSubsystem.h"
#include "Subsystems/StageShowLibrarySubsystem.h"

namespace StagePaletteTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	/** A game world that has begun play, destroyed with the scope. Fixtures register with its show control on BeginPlay. */
	struct FScopedWorld
	{
		UWorld* World = nullptr;

		FScopedWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("StagePaletteTestWorld"));
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			World->BeginPlay();
			World->GetWorldSettings()->NotifyBeginPlay();
		}

		~FScopedWorld()
		{
			World->BeginTearingDown();
			World->EndPlay(EEndPlayReason::Quit);
			World->DestroyWorld(/*bInformEngineOfWorld*/ false);
			GEngine->DestroyWorldContext(World);
		}

		/** An RGBW moving head, initialised the way the spawn system does it. */
		ALightingFixtureActor* SpawnMover() const
		{
			ULightingFixtureData* Data = NewObject<ULightingFixtureData>(GetTransientPackage());
			Data->ColorSystem = EStageFixtureColorSystem::RGBW;
			Data->bHasPanTilt = true;
			ALightingFixtureActor* Fixture = World->SpawnActorDeferred<ALightingFixtureActor>(ALightingFixtureActor::StaticClass(), FTransform::Identity);
			Fixture->InitializeFromItemData(Data);
			Fixture->FinishSpawning(FTransform::Identity);
			return Fixture;
		}
	};

	/** Forwards palette and programmer steps to the world's subsystems; item steps never occur in these tests. */
	class FPaletteEditor final : public IStageItemEditor
	{
	public:
		explicit FPaletteEditor(UWorld& InWorld) : World(InWorld) {}

		virtual bool CaptureItem(const FGuid&, FStageItemSnapshot&) const override { return false; }
		virtual EStageCommandResult RestoreItem(const FStageItemSnapshot&) override { return EStageCommandResult::Invalid; }
		virtual EStageCommandResult RemoveItem(const FGuid&) override { return EStageCommandResult::Invalid; }
		virtual EStageCommandResult SetItemTransform(const FGuid&, const FTransform&) override { return EStageCommandResult::Invalid; }
		virtual EStageCommandResult SetItemParameter(const FGuid&, const FGameplayTag&, const FStageParameterValue&) override { return EStageCommandResult::Invalid; }

		virtual EStageCommandResult SetPalettes(const TArray<FStagePalette>& Palettes) override
		{
			World.GetSubsystem<UStageShowLibrarySubsystem>()->SetPalettes(Palettes);
			return EStageCommandResult::Succeeded;
		}

	private:
		UWorld& World;
	};

	TMap<FGameplayTag, float> Values(const FGameplayTag& Tag, float Value)
	{
		TMap<FGameplayTag, float> Map;
		Map.Add(Tag, Value);
		return Map;
	}

	FStagePalette MakePositionPalette(float Pan)
	{
		FStagePalette Palette;
		Palette.Kind = EStagePaletteKind::Position;
		Palette.Name = FText::FromString(TEXT("Downstage"));
		Palette.GlobalValues.Add(StageCraftTags::Attribute_Pan, Pan);
		return Palette;
	}

	/** True when Fixture's output holds every value in Expected. */
	bool ShowsValues(const ALightingFixtureActor& Fixture, const TMap<FGameplayTag, float>& Expected)
	{
		for (const TPair<FGameplayTag, float>& Pair : Expected)
		{
			if (!FMath::IsNearlyEqual(Fixture.GetAttribute(Pair.Key), Pair.Value, 1e-4f))
			{
				return false;
			}
		}
		return !Expected.IsEmpty();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageShowPaletteRulesTest, "StageCraft.Show.PaletteRules", StagePaletteTests::Flags)

bool FStageShowPaletteRulesTest::RunTest(const FString& Parameters)
{
	using namespace StagePaletteRules;
	const FGameplayTag Dimmer = StageCraftTags::Attribute_Dimmer;
	const FGameplayTag Pan = StageCraftTags::Attribute_Pan;
	const FGameplayTag ColorR = StageCraftTags::Attribute_ColorR;
	const FGameplayTag CellDimmer = StageFixtureCells::GetCellAttribute(0, EStageCellChannel::Dimmer);
	const FGameplayTag CellRed = StageFixtureCells::GetCellAttribute(3, EStageCellChannel::ColorR);

	// Kinds.
	TestTrue(TEXT("Dimmer is intensity"), GetAttributeKind(Dimmer) == EStagePaletteKind::Intensity);
	TestTrue(TEXT("A cell dimmer is intensity"), GetAttributeKind(CellDimmer) == EStagePaletteKind::Intensity);
	TestTrue(TEXT("Pan is position"), GetAttributeKind(Pan) == EStagePaletteKind::Position);
	TestTrue(TEXT("Red is colour"), GetAttributeKind(ColorR) == EStagePaletteKind::Color);
	TestTrue(TEXT("A cell colour is colour"), GetAttributeKind(CellRed) == EStagePaletteKind::Color);
	TestTrue(TEXT("The colour wheel is colour"), GetAttributeKind(StageCraftTags::Attribute_ColorWheel) == EStagePaletteKind::Color);
	TestTrue(TEXT("Zoom is focus"), GetAttributeKind(StageCraftTags::Attribute_Zoom) == EStagePaletteKind::Focus);
	TestTrue(TEXT("The gobo is beam"), GetAttributeKind(StageCraftTags::Attribute_Gobo) == EStagePaletteKind::Beam);
	TestTrue(TEXT("A position palette holds pan"), AllowsAttribute(EStagePaletteKind::Position, Pan));
	TestFalse(TEXT("... not the dimmer"), AllowsAttribute(EStagePaletteKind::Position, Dimmer));
	TestTrue(TEXT("ALL holds anything stored"), AllowsAttribute(EStagePaletteKind::All, Dimmer));
	TestFalse(TEXT("The composite colour row is never stored"), AllowsAttribute(EStagePaletteKind::Color, StageCraftTags::Attribute_ColorRGB));
	TestFalse(TEXT("A parameter is not an attribute"), AllowsAttribute(EStagePaletteKind::All, StageCraftTags::Param_Patch_Address));

	TMap<FGameplayTag, float> Mixed;
	Mixed.Add(Pan, 10.f);
	Mixed.Add(Dimmer, 0.5f);
	Mixed.Add(StageCraftTags::Attribute_Tilt, std::numeric_limits<float>::quiet_NaN());
	const TMap<FGameplayTag, float> Filtered = FilterValues(EStagePaletteKind::Position, Mixed);
	TestTrue(TEXT("Filter keeps the kind's finite values"), Filtered.Num() == 1 && Filtered.Contains(Pan));

	// Resolution order: the fixture's own value, the global value, the colour match, nothing.
	FStagePalette Palette;
	Palette.Kind = EStagePaletteKind::Color;
	Palette.bHasColor = true;
	Palette.Color = FLinearColor(1.f, 0.f, 0.f);
	Palette.GlobalValues.Add(StageCraftTags::Attribute_Zoom, 20.f); // Not of the kind: FindPaletteValue does not filter, sanitizing does.
	FShowCueFixtureState Own;
	Own.FixtureId = 7;
	Own.Attributes.Add(ColorR, 0.25f);
	Palette.FixtureValues = { Own };
	TMap<FGameplayTag, float> Match;
	Match.Add(ColorR, 0.9f);
	Match.Add(StageCraftTags::Attribute_ColorG, 0.1f);
	float Value = -1.f;
	TestTrue(TEXT("The fixture's own value first"), FindPaletteValue(Palette, 7, ColorR, &Match, Value) && Value == 0.25f);
	TestTrue(TEXT("Another fixture: the colour match"), FindPaletteValue(Palette, 8, ColorR, &Match, Value) && Value == 0.9f);
	TestTrue(TEXT("Global before the colour match"), FindPaletteValue(Palette, 8, StageCraftTags::Attribute_Zoom, &Match, Value) && Value == 20.f);
	TestFalse(TEXT("No fixture on stage: no colour match"), FindPaletteValue(Palette, 8, StageCraftTags::Attribute_ColorG, nullptr, Value));
	TestFalse(TEXT("Nothing for this attribute"), FindPaletteValue(Palette, 8, Pan, &Match, Value));

	// References need a value of their own and a valid id.
	FShowCueFixtureState State;
	State.FixtureId = 1;
	State.Attributes.Add(Pan, 5.f);
	State.PaletteRefs.Add(Pan, FGuid::NewGuid());
	State.PaletteRefs.Add(Dimmer, FGuid::NewGuid());
	State.PaletteRefs.Add(StageCraftTags::Attribute_Tilt, FGuid());
	TestEqual(TEXT("References without a value or an id are dropped"), SanitizeReferences(State), 2);
	TestTrue(TEXT("... the valid one stays"), State.PaletteRefs.Num() == 1 && State.PaletteRefs.Contains(Pan));

	// The pool's rules.
	FStagePalette Valid = Palette;
	Valid.Id = FGuid::NewGuid();
	FStagePalette NoId = Palette;
	FStagePalette WrongColor = StagePaletteTests::MakePositionPalette(30.f);
	WrongColor.Id = FGuid::NewGuid();
	WrongColor.bHasColor = true; // A colour on a position palette.
	WrongColor.GlobalValues.Add(ColorR, 1.f); // A colour value in a position palette.
	FStagePalette NanColor = Palette;
	NanColor.Id = FGuid::NewGuid();
	NanColor.Color = FLinearColor(std::numeric_limits<float>::quiet_NaN(), 0.f, 0.f);
	NanColor.Name = FText::GetEmpty();
	TArray<FStagePalette> Pool = { Valid, NoId, Valid, WrongColor, NanColor };
	TestTrue(TEXT("Sanitize drops and counts"), SanitizePalettes(Pool) >= 5);
	TestEqual(TEXT("Three palettes stay"), Pool.Num(), 3);
	if (Pool.Num() == 3)
	{
		TestTrue(TEXT("A colour palette keeps no global values (zoom is not colour; colour values are never global)"), Pool[0].GlobalValues.IsEmpty() && Pool[0].bHasColor);
		TestTrue(TEXT("A position palette keeps pan, loses the colour"), !Pool[1].bHasColor && Pool[1].GlobalValues.Num() == 1 && Pool[1].GlobalValues.Contains(Pan));
		TestTrue(TEXT("A non-finite colour is dropped, the palette is named"), !Pool[2].bHasColor && !Pool[2].Name.IsEmpty());
	}

	// Which cues use a palette.
	FShowCue CueA;
	CueA.Number = 3.f;
	FShowCueFixtureState Uses = State;
	Uses.PaletteRefs.Add(Pan, Valid.Id);
	CueA.Fixtures = { Uses };
	FShowCue CueB;
	CueB.Number = 1.f;
	CueB.Fixtures = { Uses };
	FShowCue CueC;
	CueC.Number = 2.f;
	TestTrue(TEXT("Cues using it, ascending"), FindCuesUsing({ CueA, CueB, CueC }, Valid.Id) == TArray<float>({ 1.f, 3.f }));

	// Starter palettes: valid as they are, fixed ids.
	TArray<FStagePalette> Starters = MakeStarterPalettes();
	TestEqual(TEXT("Ten starter palettes"), Starters.Num(), 10);
	TestEqual(TEXT("They need no repair"), SanitizePalettes(Starters), 0);
	TestTrue(TEXT("Their ids are the same every time"), MakeStarterPalettes()[0].Id == Starters[0].Id && Starters[0].Id.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageShowPaletteReferencesTest, "StageCraft.Show.PaletteReferences", StagePaletteTests::Flags)

bool FStageShowPaletteReferencesTest::RunTest(const FString& Parameters)
{
	using namespace StagePaletteTests;
	const FGameplayTag Pan = StageCraftTags::Attribute_Pan;

	FScopedWorld Scope;
	ALightingFixtureActor* A = Scope.SpawnMover();
	ALightingFixtureActor* B = Scope.SpawnMover();
	UShowControlSubsystem* Show = Scope.World->GetSubsystem<UShowControlSubsystem>();
	UStageProgrammerSubsystem* Programmer = Scope.World->GetSubsystem<UStageProgrammerSubsystem>();
	UStageShowLibrarySubsystem* Library = Scope.World->GetSubsystem<UStageShowLibrarySubsystem>();
	if (!TestNotNull(TEXT("Fixture A"), A) || !TestNotNull(TEXT("Fixture B"), B) || !TestNotNull(TEXT("Show control"), Show)
		|| !TestNotNull(TEXT("Programmer"), Programmer) || !TestNotNull(TEXT("Library"), Library))
	{
		return false;
	}
	const int32 IdA = A->GetFixtureId();
	const int32 IdB = B->GetFixtureId();
	TestEqual(TEXT("A new stage has the starter palettes"), Library->GetPalettes().Num(), 10);

	// A position palette in the programmer follows the palette.
	const FGuid Downstage = Library->AddPalette(MakePositionPalette(30.f));
	TestTrue(TEXT("Recorded"), Downstage.IsValid());
	FStagePalette Empty;
	Empty.Kind = EStagePaletteKind::Position;
	TestFalse(TEXT("A palette with nothing in it is refused"), Library->AddPalette(Empty).IsValid());
	Programmer->SetReferencedValues(IdA, Values(Pan, 30.f), Downstage);
	TestTrue(TEXT("The programmer holds a reference"), Programmer->FindReferences(IdA) && Programmer->FindReferences(IdA)->FindRef(Pan) == Downstage);
	TestEqual(TEXT("Applied: pan 30"), A->GetAttribute(Pan), 30.f, 1e-4f);
	TestTrue(TEXT("Update"), Library->SetPaletteContent(Downstage, MakePositionPalette(60.f)));
	TestEqual(TEXT("The programmer follows the palette"), A->GetAttribute(Pan), 60.f, 1e-4f);

	// A plain edit replaces the reference; the value change and the reference change are both undo-worthy.
	const TArray<FShowCueFixtureState> Referenced = Programmer->CaptureFixtures({ IdA });
	Programmer->SetValues(IdA, Values(Pan, 60.f));
	TestNull(TEXT("A plain edit drops the reference"), Programmer->FindReferences(IdA));
	TestTrue(TEXT("Same value, no reference: still a change"), FStageProgrammerCommand::IsMeaningfulChange(Referenced, Programmer->CaptureFixtures({ IdA })));
	Programmer->RestoreFixtures({ IdA }, Referenced);
	TestTrue(TEXT("Restored with its reference"), Programmer->FindReferences(IdA) != nullptr);

	// A colour palette mixes its colour on each fixture.
	const FStagePalette* Red = Library->GetPalettes().FindByPredicate([](const FStagePalette& Each) { return Each.Name.ToString() == TEXT("Red"); });
	if (!TestNotNull(TEXT("Starter Red"), Red))
	{
		return false;
	}
	const FGuid RedId = Red->Id;
	const TMap<FGameplayTag, float> RedOnB = B->MatchColor(FLinearColor(1.f, 0.f, 0.f));
	Programmer->SetReferencedValues(IdB, RedOnB, RedId);
	TestTrue(TEXT("B shows red"), ShowsValues(*B, RedOnB));

	// A cue stores the references; Clear and the cue bring the palette's look back.
	TestTrue(TEXT("Store cue 1"), Show->StoreCue(1.f, FText::GetEmpty(), 0.f));
	const FShowCue* Cue1 = Show->FindCue(1.f);
	const FShowCueFixtureState* StoredA = Cue1 ? Cue1->Fixtures.FindByPredicate([IdA](const FShowCueFixtureState& Each) { return Each.FixtureId == IdA; }) : nullptr;
	TestTrue(TEXT("The cue refers to Downstage for A's pan"), StoredA && StoredA->PaletteRefs.FindRef(Pan) == Downstage);
	TestTrue(TEXT("Stored without a reference where none was held"), StoredA && !StoredA->PaletteRefs.Contains(StageCraftTags::Attribute_Dimmer));
	Programmer->ClearAll();
	TestTrue(TEXT("Play cue 1"), Show->RestoreActiveCue(1.f));
	TestEqual(TEXT("Cue 1: pan from the palette"), A->GetAttribute(Pan), 60.f, 1e-4f);
	TestTrue(TEXT("Cue 1: B red"), ShowsValues(*B, RedOnB));

	// Re-recording the palette changes the playing cue.
	TestTrue(TEXT("Update again"), Library->SetPaletteContent(Downstage, MakePositionPalette(-45.f)));
	TestEqual(TEXT("The playing cue follows the palette"), A->GetAttribute(Pan), -45.f, 1e-4f);
	FStagePalette Blue = *Library->FindPalette(RedId);
	Blue.Color = FLinearColor(0.f, 0.f, 1.f);
	TestTrue(TEXT("Red becomes blue"), Library->SetPaletteContent(RedId, Blue));
	TestTrue(TEXT("B follows the colour, mixed by its own emitters"), ShowsValues(*B, B->MatchColor(FLinearColor(0.f, 0.f, 1.f))));

	// A Go fades to the palette's value as it is now.
	Programmer->SetValues(IdA, Values(Pan, 10.f));
	TestTrue(TEXT("Store cue 2 (plain pan 10)"), Show->StoreCue(2.f, FText::GetEmpty(), 2.f));
	Programmer->ClearAll();
	TestTrue(TEXT("Play cue 2"), Show->RestoreActiveCue(2.f));
	TestEqual(TEXT("Cue 2: pan 10"), A->GetAttribute(Pan), 10.f, 1e-4f);
	TestTrue(TEXT("Cue 1 fades in 2 s"), Show->UpdateCue(1.f, FText::GetEmpty(), 2.f));
	TestTrue(TEXT("Go back to cue 1"), Show->GoToCue(1.f));
	Show->Tick(1.f);
	TestEqual(TEXT("Halfway to the palette's -45"), A->GetAttribute(Pan), -17.5f, 1e-3f);
	Show->Tick(1.5f);
	TestEqual(TEXT("There"), A->GetAttribute(Pan), -45.f, 1e-4f);

	// Undo of a palette edit, through the history command.
	FPaletteEditor Editor(*Scope.World);
	const TArray<FStagePalette> Before = Library->GetPalettes();
	TestTrue(TEXT("Update to 15"), Library->SetPaletteContent(Downstage, MakePositionPalette(15.f)));
	FStagePalettesCommand Step(FText::FromString(TEXT("Update Downstage")), Before, Library->GetPalettes());
	TestEqual(TEXT("Updated: 15"), A->GetAttribute(Pan), 15.f, 1e-4f);
	TestTrue(TEXT("Undo"), Step.Undo(Editor) == EStageCommandResult::Succeeded);
	TestEqual(TEXT("Undo: -45 again"), A->GetAttribute(Pan), -45.f, 1e-4f);
	TestTrue(TEXT("Redo"), Step.Redo(Editor) == EStageCommandResult::Succeeded);
	TestEqual(TEXT("Redo: 15"), A->GetAttribute(Pan), 15.f, 1e-4f);

	// A colour-wheel fixture shows a colour palette by its nearest slot (Red at a dark level still picks red; white is Open).
	ULightingFixtureData* WheelData = NewObject<ULightingFixtureData>(GetTransientPackage());
	WheelData->ColorSystem = EStageFixtureColorSystem::None;
	FStageColorWheelSlot RedSlot;
	RedSlot.Filter = FLinearColor(1.f, 0.05f, 0.05f);
	FStageColorWheelSlot BlueSlot;
	BlueSlot.Filter = FLinearColor(0.05f, 0.1f, 1.f);
	WheelData->ColorWheel = { RedSlot, BlueSlot };
	ALightingFixtureActor* Wheel = Scope.World->SpawnActorDeferred<ALightingFixtureActor>(ALightingFixtureActor::StaticClass(), FTransform::Identity);
	Wheel->InitializeFromItemData(WheelData);
	Wheel->FinishSpawning(FTransform::Identity);
	TestEqual(TEXT("Wheel: dark red picks the red slot"), Wheel->MatchColor(FLinearColor(0.3f, 0.f, 0.f)).FindRef(StageCraftTags::Attribute_ColorWheel), 1.f);
	TestEqual(TEXT("Wheel: blue picks the blue slot"), Wheel->MatchColor(FLinearColor(0.f, 0.f, 1.f)).FindRef(StageCraftTags::Attribute_ColorWheel), 2.f);
	TestEqual(TEXT("Wheel: white is Open"), Wheel->MatchColor(FLinearColor::White).FindRef(StageCraftTags::Attribute_ColorWheel, -1.f), 0.f);
	TestEqual(TEXT("Wheel: only the wheel"), Wheel->MatchColor(FLinearColor::White).Num(), 1);

	// A palette that is gone (removed without the request that bakes its references) leaves the stored value.
	TestTrue(TEXT("Remove"), Library->RemovePalette(Downstage));
	TestEqual(TEXT("Missing palette: the value stored with the cue (60)"), A->GetAttribute(Pan), 60.f, 1e-4f);
	TestFalse(TEXT("Rename of a missing palette"), Library->RenamePalette(Downstage, FText::FromString(TEXT("X"))));
	TestFalse(TEXT("An empty name is refused"), Library->RenamePalette(RedId, FText::FromString(TEXT("  "))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageScenePaletteDataTest, "StageCraft.Scene.PaletteData", StagePaletteTests::Flags)

bool FStageScenePaletteDataTest::RunTest(const FString& Parameters)
{
	const FGameplayTag Pan = StageCraftTags::Attribute_Pan;
	FStageSceneDocument Document;
	Document.Name = TEXT("PaletteData");
	FStagePalette Position = StagePaletteTests::MakePositionPalette(25.f);
	Position.Id = FGuid::NewGuid();
	FShowCueFixtureState Aimed;
	Aimed.FixtureId = 3;
	Aimed.Attributes.Add(StageCraftTags::Attribute_Tilt, -12.f);
	Position.FixtureValues = { Aimed };
	FStagePalette Amber;
	Amber.Id = FGuid::NewGuid();
	Amber.Name = FText::FromString(TEXT("Amber"));
	Amber.Kind = EStagePaletteKind::Color;
	Amber.bHasColor = true;
	Amber.Color = FLinearColor(1.f, 0.5f, 0.f);
	Document.Palettes = { Position, Amber };
	FShowCueFixtureState Held;
	Held.FixtureId = 3;
	Held.Attributes.Add(Pan, 25.f);
	Held.PaletteRefs.Add(Pan, Position.Id);
	Document.Programmer = { Held };
	FShowCue Cue;
	Cue.Number = 1.f;
	Cue.Fixtures = { Held };
	Document.Cues = { Cue };

	FStageSceneDocument Parsed;
	FStageSceneParseReport Report;
	TestTrue(TEXT("Round trip parses"), FStageSceneStore::Parse(FStageSceneStore::Serialize(Document), Parsed, Report) == EStageSceneResult::Success);
	TestEqual(TEXT("Nothing was dropped"), Report.InvalidShowEntries + Report.InvalidCueEntries, 0);
	TestEqual(TEXT("Both palettes kept"), Parsed.Palettes.Num(), 2);
	if (Parsed.Palettes.Num() == 2)
	{
		const FStagePalette& First = Parsed.Palettes[0];
		TestTrue(TEXT("Position palette: id, kind, global and per-fixture values"), First.Id == Position.Id && First.Kind == EStagePaletteKind::Position
			&& FMath::IsNearlyEqual(First.GlobalValues.FindRef(Pan), 25.f) && First.FixtureValues.Num() == 1 && First.FixtureValues[0].FixtureId == 3);
		TestTrue(TEXT("Colour palette: its colour"), Parsed.Palettes[1].bHasColor && Parsed.Palettes[1].Color.Equals(Amber.Color, 1e-5f));
	}
	TestTrue(TEXT("Programmer reference kept"), Parsed.Programmer.Num() == 1 && Parsed.Programmer[0].PaletteRefs.FindRef(Pan) == Position.Id);
	TestTrue(TEXT("Cue reference kept"), Parsed.Cues.Num() == 1 && Parsed.Cues[0].Fixtures.Num() == 1 && Parsed.Cues[0].Fixtures[0].PaletteRefs.FindRef(Pan) == Position.Id);

	// Hostile v7 palettes and references: each bad entry is dropped and counted, the rest loads.
	const FString GoodId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
	const FString Hostile = FString::Printf(TEXT("{\"format\":\"StageCraftScene\",\"version\":%d,\"name\":\"H\",\"items\":[],")
		TEXT("\"palettes\":[{\"id\":\"nope\",\"kind\":\"Color\"},{\"id\":\"%s\",\"kind\":\"Laser\"},")
		TEXT("{\"id\":\"%s\",\"name\":\"P\",\"kind\":\"Position\",\"color\":[1,\"x\",0],\"values\":{\"StageCraft.Attribute.Pan\":10,\"StageCraft.Attribute.Dimmer\":1,\"Not.A.Tag\":3}},7],")
		TEXT("\"programmer\":[{\"id\":2,\"attributes\":{\"StageCraft.Attribute.Pan\":1},\"palettes\":{\"StageCraft.Attribute.Pan\":\"%s\",\"StageCraft.Attribute.Tilt\":\"%s\",\"StageCraft.Attribute.Zoom\":\"bad\"}}]}"),
		FStageSceneStore::FormatVersion, *FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens), *GoodId, *GoodId, *GoodId);
	TestTrue(TEXT("A hostile file still parses"), FStageSceneStore::Parse(Hostile, Parsed = FStageSceneDocument(), Report = FStageSceneParseReport()) == EStageSceneResult::Success);
	TestTrue(TEXT("One palette survives with pan only"), Parsed.Palettes.Num() == 1 && Parsed.Palettes[0].GlobalValues.Num() == 1
		&& Parsed.Palettes[0].GlobalValues.Contains(Pan) && !Parsed.Palettes[0].bHasColor);
	TestTrue(TEXT("Only the reference with a value and a valid id survives"), Parsed.Programmer.Num() == 1 && Parsed.Programmer[0].PaletteRefs.Num() == 1);
	TestTrue(TEXT("The drops are counted"), Report.InvalidShowEntries >= 8);

	// A version 7 file with no palettes keeps none; a version 6 file gets the starter palettes.
	const FString V7Empty = FString::Printf(TEXT("{\"format\":\"StageCraftScene\",\"version\":%d,\"name\":\"E\",\"items\":[],\"palettes\":[]}"), FStageSceneStore::FormatVersion);
	TestTrue(TEXT("Empty v7 parses"), FStageSceneStore::Parse(V7Empty, Parsed = FStageSceneDocument(), Report = FStageSceneParseReport()) == EStageSceneResult::Success);
	TestTrue(TEXT("A user who deleted every palette keeps none"), Parsed.Palettes.IsEmpty());
	const FString V6 = TEXT("{\"format\":\"StageCraftScene\",\"version\":6,\"name\":\"Old\",\"items\":[],\"palettes\":[{\"id\":\"x\"}]}");
	TestTrue(TEXT("Version 6 parses"), FStageSceneStore::Parse(V6, Parsed = FStageSceneDocument(), Report = FStageSceneParseReport()) == EStageSceneResult::Success);
	TestEqual(TEXT("Version 6: the starter palettes"), Parsed.Palettes.Num(), StagePaletteRules::MakeStarterPalettes().Num());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
