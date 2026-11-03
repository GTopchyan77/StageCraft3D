// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for the programmer and the output mix (Docs/ADR/0025-programmer-and-output-mix.md): the pure mix and fan rules, the layers
// on real fixtures (home look, playing cue, programmer, highlight; External DMX), the undo commands, the fixture group pool, and
// the scene format v6 sections. The controller's selection spread and the panel are exercised by the scripted -game run (STATE.md #59).
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Show; Automation RunTests StageCraft.Scene.ShowData; Quit" -unattended -nullrhi

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
#include "Show/StageShowMix.h"
#include "Subsystems/ShowControlSubsystem.h"
#include "Subsystems/StageProgrammerSubsystem.h"
#include "Subsystems/StageShowLibrarySubsystem.h"

namespace StageProgrammerTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	/** A game world that has begun play, destroyed with the scope. Fixtures register with its show control on BeginPlay. */
	struct FScopedWorld
	{
		UWorld* World = nullptr;

		FScopedWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("StageProgrammerTestWorld"));
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

	/** Forwards the show steps to the world's subsystems; item steps never occur in these tests. */
	class FShowEditor final : public IStageItemEditor
	{
	public:
		explicit FShowEditor(UWorld& InWorld) : World(InWorld) {}

		virtual bool CaptureItem(const FGuid&, FStageItemSnapshot&) const override { return false; }
		virtual EStageCommandResult RestoreItem(const FStageItemSnapshot&) override { return EStageCommandResult::Invalid; }
		virtual EStageCommandResult RemoveItem(const FGuid&) override { return EStageCommandResult::Invalid; }
		virtual EStageCommandResult SetItemTransform(const FGuid&, const FTransform&) override { return EStageCommandResult::Invalid; }
		virtual EStageCommandResult SetItemParameter(const FGuid&, const FGameplayTag&, const FStageParameterValue&) override { return EStageCommandResult::Invalid; }

		virtual EStageCommandResult SetProgrammerValues(TConstArrayView<int32> FixtureIds, TConstArrayView<FShowCueFixtureState> Entries) override
		{
			World.GetSubsystem<UStageProgrammerSubsystem>()->RestoreFixtures(FixtureIds, Entries);
			return EStageCommandResult::Succeeded;
		}

		virtual EStageCommandResult SetFixtureGroups(const TArray<FStageFixtureGroup>& Groups) override
		{
			World.GetSubsystem<UStageShowLibrarySubsystem>()->SetGroups(Groups);
			return EStageCommandResult::Succeeded;
		}

	private:
		UWorld& World;
	};

	FShowCue MakeCue(float Number, float Fade, TConstArrayView<TPair<int32, float>> Dimmers)
	{
		FShowCue Cue;
		Cue.Number = Number;
		Cue.FadeTime = Fade;
		for (const TPair<int32, float>& Dimmer : Dimmers)
		{
			FShowCueFixtureState& State = Cue.Fixtures.AddDefaulted_GetRef();
			State.FixtureId = Dimmer.Key;
			State.Attributes.Add(StageCraftTags::Attribute_Dimmer, Dimmer.Value);
		}
		return Cue;
	}

	TMap<FGameplayTag, float> Values(const FGameplayTag& Tag, float Value)
	{
		TMap<FGameplayTag, float> Map;
		Map.Add(Tag, Value);
		return Map;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageShowOutputMixTest, "StageCraft.Show.OutputMix", StageProgrammerTests::Flags)

bool FStageShowOutputMixTest::RunTest(const FString& Parameters)
{
	using namespace StageShowMix;
	const FGameplayTag Dimmer = StageCraftTags::Attribute_Dimmer;
	const FGameplayTag Pan = StageCraftTags::Attribute_Pan;
	const FGameplayTag Tilt = StageCraftTags::Attribute_Tilt;

	FAttributeMap Home;
	Home.Add(Dimmer, 0.f);
	Home.Add(Pan, 0.f);
	Home.Add(Tilt, 0.f);
	FAttributeMap Playback;
	Playback.Add(Dimmer, 0.5f);
	Playback.Add(Pan, 30.f);
	FAttributeMap Programmer;
	Programmer.Add(Pan, 90.f);
	FAttributeMap Highlight;
	Highlight.Add(Dimmer, 1.f);

	FFixtureLayers Layers;
	Layers.Home = &Home;
	float Value = -1.f;
	TestTrue(TEXT("Home only: the home look"), ResolveAttribute(Dimmer, Layers, Value) && Value == 0.f);
	Layers.Playback = &Playback;
	TestTrue(TEXT("The cue is above the home look"), ResolveAttribute(Dimmer, Layers, Value) && Value == 0.5f);
	TestTrue(TEXT("An attribute the cue does not hold stays home"), ResolveAttribute(Tilt, Layers, Value) && Value == 0.f);
	Layers.Programmer = &Programmer;
	TestTrue(TEXT("The programmer is above the cue"), ResolveAttribute(Pan, Layers, Value) && Value == 90.f);
	TestTrue(TEXT("... only where it holds a value"), ResolveAttribute(Dimmer, Layers, Value) && Value == 0.5f);
	Layers.Highlight = &Highlight;
	TestTrue(TEXT("The highlight is above everything"), ResolveAttribute(Dimmer, Layers, Value) && Value == 1.f);
	TestTrue(TEXT("Held above the cue: programmer"), IsHeldAbovePlayback(Pan, Layers));
	TestTrue(TEXT("Held above the cue: highlight"), IsHeldAbovePlayback(Dimmer, Layers));
	TestFalse(TEXT("Not held: tilt"), IsHeldAbovePlayback(Tilt, Layers));
	TestFalse(TEXT("No layer holds an unknown attribute"), ResolveAttribute(StageCraftTags::Attribute_Zoom, Layers, Value));

	FAttributeMap Mixed;
	ResolveFixture({ Dimmer, Pan, Tilt, StageCraftTags::Attribute_Zoom }, Layers, Mixed);
	TestEqual(TEXT("Every held attribute is mixed, the unheld one left out"), Mixed.Num(), 3);

	// Fan: the share of the primary's change each fixture takes, in selection order.
	TestEqual(TEXT("Fan off: all the same"), FanFactor(EStageFanMode::Off, 0, 5), 1.f);
	TestEqual(TEXT("One fixture: the change itself"), FanFactor(EStageFanMode::Linear, 0, 1), 1.f);
	TestEqual(TEXT("Linear: first -1"), FanFactor(EStageFanMode::Linear, 0, 3), -1.f);
	TestEqual(TEXT("Linear: middle 0"), FanFactor(EStageFanMode::Linear, 1, 3), 0.f);
	TestEqual(TEXT("Linear: last +1"), FanFactor(EStageFanMode::Linear, 2, 3), 1.f);
	TestEqual(TEXT("Linear: a quarter along"), FanFactor(EStageFanMode::Linear, 1, 5), -0.5f, 1e-6f);
	TestEqual(TEXT("V: ends full"), FanFactor(EStageFanMode::V, 0, 3), 1.f);
	TestEqual(TEXT("V: middle none"), FanFactor(EStageFanMode::V, 1, 3), 0.f);
	TestEqual(TEXT("Inverted V: middle full"), FanFactor(EStageFanMode::InvertedV, 1, 3), 1.f);
	TestEqual(TEXT("Inverted V: ends none"), FanFactor(EStageFanMode::InvertedV, 2, 3), 0.f);
	TestEqual(TEXT("An index past the end counts as the end"), FanFactor(EStageFanMode::Linear, 9, 3), 1.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageShowProgrammerTest, "StageCraft.Show.Programmer", StageProgrammerTests::Flags)

bool FStageShowProgrammerTest::RunTest(const FString& Parameters)
{
	using namespace StageProgrammerTests;
	const FGameplayTag Dimmer = StageCraftTags::Attribute_Dimmer;
	const FGameplayTag Pan = StageCraftTags::Attribute_Pan;

	FScopedWorld Scope;
	ALightingFixtureActor* A = Scope.SpawnMover();
	ALightingFixtureActor* B = Scope.SpawnMover();
	UShowControlSubsystem* Show = Scope.World->GetSubsystem<UShowControlSubsystem>();
	UStageProgrammerSubsystem* Programmer = Scope.World->GetSubsystem<UStageProgrammerSubsystem>();
	if (!TestNotNull(TEXT("Fixture A"), A) || !TestNotNull(TEXT("Fixture B"), B) || !TestNotNull(TEXT("Show control"), Show) || !TestNotNull(TEXT("Programmer"), Programmer))
	{
		return false;
	}
	const int32 IdA = A->GetFixtureId();
	const int32 IdB = B->GetFixtureId();
	const float HomeDimmer = A->GetHomeAttributes().FindRef(Dimmer);
	TestEqual(TEXT("A new fixture's home look is its output"), A->GetHomeAttributes().Num(), A->GetAttributes().Num());

	// The programmer is above the home look, and Clear brings the home look back.
	Programmer->SetValues(IdA, Values(Dimmer, 0.8f));
	TestEqual(TEXT("Programmer value reaches the output"), A->GetAttribute(Dimmer), 0.8f, 1e-5f);
	TestEqual(TEXT("... of that fixture only"), B->GetAttribute(Dimmer), HomeDimmer, 1e-5f);
	Programmer->ClearAll();
	TestEqual(TEXT("Clear: back to the home look"), A->GetAttribute(Dimmer), HomeDimmer, 1e-5f);

	// A fade drives what the programmer does not hold, and never what it does.
	Show->SetCues({ MakeCue(1.f, 0.f, { { IdA, 0.2f }, { IdB, 0.2f } }), MakeCue(2.f, 2.f, { { IdA, 1.f }, { IdB, 1.f } }) });
	TestTrue(TEXT("Go to cue 1"), Show->GoToCue(1.f));
	TestEqual(TEXT("Cue 1 output"), B->GetAttribute(Dimmer), 0.2f, 1e-5f);
	Programmer->SetValues(IdA, Values(Dimmer, 0.9f));
	TestTrue(TEXT("Go to cue 2"), Show->GoToCue(2.f));
	Show->Tick(1.f);
	TestEqual(TEXT("Halfway: B follows the cue"), B->GetAttribute(Dimmer), 0.6f, 1e-4f);
	TestEqual(TEXT("Halfway: A keeps the programmer value"), A->GetAttribute(Dimmer), 0.9f, 1e-5f);
	Programmer->ClearFixtures({ IdA });
	TestEqual(TEXT("Clear mid-fade: A takes the cue where it is now"), A->GetAttribute(Dimmer), 0.6f, 1e-4f);
	Show->Tick(1.5f);
	TestEqual(TEXT("End: A at cue 2"), A->GetAttribute(Dimmer), 1.f, 1e-5f);

	// Highlight shows full on top and is never recorded.
	Programmer->SetValues(IdB, Values(Dimmer, 0.3f));
	Programmer->SetHighlightedFixtures({ IdB });
	TestEqual(TEXT("Highlight: full"), B->GetAttribute(Dimmer), 1.f, 1e-5f);
	TestTrue(TEXT("Store cue 3"), Show->StoreCue(3.f, FText::GetEmpty(), 0.f));
	const FShowCue* Cue3 = Show->FindCue(3.f);
	const FShowCueFixtureState* Recorded = Cue3 ? Cue3->Fixtures.FindByPredicate([IdB](const FShowCueFixtureState& State) { return State.FixtureId == IdB; }) : nullptr;
	TestTrue(TEXT("The cue records the look under the highlight"), Recorded && FMath::IsNearlyEqual(Recorded->Attributes.FindRef(Dimmer), 0.3f, 1e-5f));
	Programmer->SetHighlightedFixtures({});
	TestEqual(TEXT("Highlight off: the programmer value again"), B->GetAttribute(Dimmer), 0.3f, 1e-5f);

	// An undo step restores exactly the programmer it captured.
	FShowEditor Editor(*Scope.World);
	const TArray<FShowCueFixtureState> Before = Programmer->CaptureFixtures({ IdA, IdB });
	Programmer->SetValues(IdA, Values(Pan, 45.f));
	const TArray<FShowCueFixtureState> After = Programmer->CaptureFixtures({ IdA, IdB });
	TestTrue(TEXT("A change is meaningful"), FStageProgrammerCommand::IsMeaningfulChange(Before, After));
	TestFalse(TEXT("No change is not"), FStageProgrammerCommand::IsMeaningfulChange(After, After));
	FStageProgrammerCommand Step(FText::FromString(TEXT("Pan")), { IdA, IdB }, Before, After);
	TestTrue(TEXT("Undo"), Step.Undo(Editor) == EStageCommandResult::Succeeded);
	TestNull(TEXT("Undo: A holds nothing again"), Programmer->FindValues(IdA));
	TestTrue(TEXT("Undo: B keeps its value"), Programmer->FindValues(IdB) != nullptr);
	TestTrue(TEXT("Redo"), Step.Redo(Editor) == EStageCommandResult::Succeeded);
	TestEqual(TEXT("Redo: A pan held again"), A->GetAttribute(Pan), 45.f, 1e-4f);

	// Untrusted entries: bad ids, non-attribute tags and non-finite values never enter.
	FShowCueFixtureState Bad;
	Bad.FixtureId = -3;
	Bad.Attributes.Add(Dimmer, 0.5f);
	FShowCueFixtureState Hostile;
	Hostile.FixtureId = IdB;
	Hostile.Attributes.Add(Dimmer, std::numeric_limits<float>::quiet_NaN());
	Hostile.Attributes.Add(StageCraftTags::Param_Patch_Address, 7.f);
	Programmer->ReplaceAll({ Bad, Hostile });
	TestTrue(TEXT("A load of only bad entries leaves the programmer empty"), Programmer->IsEmpty());

	// A loaded show resumes its cue without a fade.
	TestTrue(TEXT("Restore cue 1"), Show->RestoreActiveCue(1.f));
	TestFalse(TEXT("... without a fade"), Show->IsFading());
	TestEqual(TEXT("... at cue 1's values"), B->GetAttribute(Dimmer), 0.2f, 1e-5f);

	// Under External DMX only what the user holds is written; the console's other values stay.
	Show->SetControlSource(EStageControlSource::External);
	B->SetAttribute(Dimmer, 0.7f);
	B->SetAttribute(Pan, -20.f);
	Programmer->SetValues(IdB, Values(Pan, 10.f));
	TestEqual(TEXT("External: the programmer value is written"), B->GetAttribute(Pan), 10.f, 1e-4f);
	TestEqual(TEXT("External: the console's dimmer stays"), B->GetAttribute(Dimmer), 0.7f, 1e-5f);
	Show->SetControlSource(EStageControlSource::Internal);
	TestEqual(TEXT("Back to Internal: the cue takes the dimmer back"), B->GetAttribute(Dimmer), 0.2f, 1e-5f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageShowFixtureGroupsTest, "StageCraft.Show.FixtureGroups", StageProgrammerTests::Flags)

bool FStageShowFixtureGroupsTest::RunTest(const FString& Parameters)
{
	using namespace StageProgrammerTests;

	// The rules, pure.
	TestTrue(TEXT("Ids: positive, once, in order"), StageFixtureGroupRules::SanitizeFixtureIds({ 3, 0, 1, 3, -2, 2 }) == TArray<int32>({ 3, 1, 2 }));
	TestEqual(TEXT("Names are cut"), StageFixtureGroupRules::SanitizeName(FText::FromString(FString::ChrN(200, TEXT('x')))).ToString().Len(), StageFixtureGroupRules::MaxNameLength);
	TArray<FStageFixtureGroup> Groups;
	FStageFixtureGroup Valid;
	Valid.Id = FGuid::NewGuid();
	Valid.FixtureIds = { 1, 1, 2 };
	FStageFixtureGroup NoId;
	NoId.FixtureIds = { 4 };
	Groups = { Valid, NoId, Valid };
	TestEqual(TEXT("No id and a duplicate id are dropped"), StageFixtureGroupRules::SanitizeGroups(Groups), 2);
	TestTrue(TEXT("The kept group is named and deduplicated"), Groups.Num() == 1 && !Groups[0].Name.IsEmpty() && Groups[0].FixtureIds.Num() == 2);

	// The pool and its undo step.
	FScopedWorld Scope;
	UStageShowLibrarySubsystem* Library = Scope.World->GetSubsystem<UStageShowLibrarySubsystem>();
	if (!TestNotNull(TEXT("Show library"), Library))
	{
		return false;
	}
	TestFalse(TEXT("A group needs fixtures"), Library->AddGroup(FText::GetEmpty(), {}).IsValid());
	const TArray<FStageFixtureGroup> Before = Library->GetGroups();
	const FGuid Front = Library->AddGroup(FText::FromString(TEXT("Front wash")), { 5, 3, 4 });
	TestTrue(TEXT("Recorded"), Front.IsValid());
	TestTrue(TEXT("Selection order is kept"), Library->FindGroup(Front)->FixtureIds == TArray<int32>({ 5, 3, 4 }));
	const FGuid Unnamed = Library->AddGroup(FText::GetEmpty(), { 1 });
	TestEqual(TEXT("An unnamed group gets a name"), Library->FindGroup(Unnamed)->Name.ToString(), FString(TEXT("Group 2")));
	TestFalse(TEXT("An empty name is refused"), Library->RenameGroup(Front, FText::FromString(TEXT("   "))));
	TestTrue(TEXT("Rename"), Library->RenameGroup(Front, FText::FromString(TEXT("FOH"))));
	TestTrue(TEXT("Update"), Library->SetGroupFixtures(Front, { 7 }));
	TestTrue(TEXT("Delete"), Library->RemoveGroup(Unnamed));
	TestFalse(TEXT("Delete twice"), Library->RemoveGroup(Unnamed));

	FShowEditor Editor(*Scope.World);
	FStageFixtureGroupsCommand Step(FText::FromString(TEXT("Groups")), Before, Library->GetGroups());
	TestTrue(TEXT("Undo"), Step.Undo(Editor) == EStageCommandResult::Succeeded);
	TestEqual(TEXT("Undo: the pool as before"), Library->GetGroups().Num(), 0);
	TestTrue(TEXT("Redo"), Step.Redo(Editor) == EStageCommandResult::Succeeded);
	TestTrue(TEXT("Redo: FOH is back with its fixture"), Library->FindGroup(Front) && Library->FindGroup(Front)->FixtureIds == TArray<int32>({ 7 }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageSceneShowDataTest, "StageCraft.Scene.ShowData", StageProgrammerTests::Flags)

bool FStageSceneShowDataTest::RunTest(const FString& Parameters)
{
	FStageSceneDocument Document;
	Document.Name = TEXT("ShowData");
	FShowCueFixtureState Held;
	Held.FixtureId = 4;
	Held.Attributes.Add(StageCraftTags::Attribute_Dimmer, 0.25f);
	Document.Programmer = { Held };
	FStageFixtureGroup Group;
	Group.Id = FGuid::NewGuid();
	Group.Name = FText::FromString(TEXT("Back truss"));
	Group.FixtureIds = { 9, 4 };
	Document.Groups = { Group };
	Document.Cues = { StageProgrammerTests::MakeCue(2.5f, 1.f, { { 4, 1.f } }) };
	Document.ActiveCueNumber = 2.5f;

	FStageSceneDocument Parsed;
	FStageSceneParseReport Report;
	TestTrue(TEXT("Round trip parses"), FStageSceneStore::Parse(FStageSceneStore::Serialize(Document), Parsed, Report) == EStageSceneResult::Success);
	TestEqual(TEXT("Nothing was dropped"), Report.InvalidShowEntries, 0);
	TestTrue(TEXT("Programmer kept"), Parsed.Programmer.Num() == 1 && Parsed.Programmer[0].FixtureId == 4
		&& FMath::IsNearlyEqual(Parsed.Programmer[0].Attributes.FindRef(StageCraftTags::Attribute_Dimmer), 0.25f));
	TestTrue(TEXT("Group kept with its order"), Parsed.Groups.Num() == 1 && Parsed.Groups[0].Id == Group.Id
		&& Parsed.Groups[0].Name.ToString() == TEXT("Back truss") && Parsed.Groups[0].FixtureIds == TArray<int32>({ 9, 4 }));
	TestTrue(TEXT("Active cue kept"), Parsed.ActiveCueNumber.IsSet() && FMath::IsNearlyEqual(Parsed.ActiveCueNumber.GetValue(), 2.5f));

	// Hostile v6 sections: each bad entry is dropped and counted, the rest loads.
	const FString Hostile = FString::Printf(TEXT("{\"format\":\"StageCraftScene\",\"version\":%d,\"name\":\"H\",\"items\":[],")
		TEXT("\"programmer\":[{\"id\":-1,\"attributes\":{}},{\"id\":3,\"attributes\":{\"StageCraft.Attribute.Dimmer\":0.5}},{\"id\":3,\"attributes\":{}},\"x\"],")
		TEXT("\"groups\":[{\"id\":\"not a guid\",\"name\":\"A\",\"fixtures\":[1]},{\"id\":\"%s\",\"name\":\"\",\"fixtures\":[1,-4,1e12,\"y\",2]}],")
		TEXT("\"activeCue\":-7}"), FStageSceneStore::FormatVersion, *FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens));
	TestTrue(TEXT("A hostile file still parses"), FStageSceneStore::Parse(Hostile, Parsed = FStageSceneDocument(), Report = FStageSceneParseReport()) == EStageSceneResult::Success);
	TestEqual(TEXT("One programmer entry survives"), Parsed.Programmer.Num(), 1);
	TestTrue(TEXT("One group survives, named, with its valid fixtures"), Parsed.Groups.Num() == 1 && !Parsed.Groups[0].Name.IsEmpty()
		&& Parsed.Groups[0].FixtureIds == TArray<int32>({ 1, 2 }));
	TestFalse(TEXT("An invalid active cue is not kept"), Parsed.ActiveCueNumber.IsSet());
	TestTrue(TEXT("The drops are counted"), Report.InvalidShowEntries >= 8);

	// A version 5 file has no show data, whatever it contains.
	const FString V5 = TEXT("{\"format\":\"StageCraftScene\",\"version\":5,\"name\":\"Old\",\"items\":[],\"programmer\":[{\"id\":3,\"attributes\":{\"StageCraft.Attribute.Dimmer\":0.5}}],\"activeCue\":1}");
	TestTrue(TEXT("Version 5 parses"), FStageSceneStore::Parse(V5, Parsed = FStageSceneDocument(), Report = FStageSceneParseReport()) == EStageSceneResult::Success);
	TestTrue(TEXT("Version 5: empty programmer, no groups, no cue playing"), Parsed.Programmer.IsEmpty() && Parsed.Groups.IsEmpty() && !Parsed.ActiveCueNumber.IsSet());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
