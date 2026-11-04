// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for playbacks and masters (Docs/ADR/0026-cue-timing-tracking-playbacks.md §6): the pure HTP / LTP / masters mix, playback
// faders on real fixtures (fader start, HTP with the main cue list, LTP by activation, Go through a tracking list, flash, release,
// Grand Master and Blackout, a fade mixed with a raised fader, recording without the masters, undo), and the scene format v8
// playbacks. The Playback panel is exercised by the scripted -game run (STATE.md #62).
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Show.Playback; Automation RunTests StageCraft.Scene.PlaybackData; Quit" -unattended -nullrhi

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
#include "Show/StageShowMix.h"
#include "Subsystems/ShowControlSubsystem.h"

namespace StagePlaybackTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	struct FScopedWorld
	{
		UWorld* World = nullptr;

		FScopedWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("StagePlaybackTestWorld"));
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

	/** Forwards playback steps to the world's show control; nothing else occurs in these tests. */
	class FPlaybackEditor final : public IStageItemEditor
	{
	public:
		explicit FPlaybackEditor(UWorld& InWorld) : World(InWorld) {}

		virtual bool CaptureItem(const FGuid&, FStageItemSnapshot&) const override { return false; }
		virtual EStageCommandResult RestoreItem(const FStageItemSnapshot&) override { return EStageCommandResult::Invalid; }
		virtual EStageCommandResult RemoveItem(const FGuid&) override { return EStageCommandResult::Invalid; }
		virtual EStageCommandResult SetItemTransform(const FGuid&, const FTransform&) override { return EStageCommandResult::Invalid; }
		virtual EStageCommandResult SetItemParameter(const FGuid&, const FGameplayTag&, const FStageParameterValue&) override { return EStageCommandResult::Invalid; }

		virtual EStageCommandResult SetPlaybacks(const TArray<FStagePlayback>& Playbacks) override
		{
			World.GetSubsystem<UShowControlSubsystem>()->ReplacePlaybacks(Playbacks);
			return EStageCommandResult::Succeeded;
		}

	private:
		UWorld& World;
	};

	FShowCueFixtureState Entry(int32 FixtureId, TConstArrayView<TPair<FGameplayTag, float>> Values)
	{
		FShowCueFixtureState State;
		State.FixtureId = FixtureId;
		for (const TPair<FGameplayTag, float>& Value : Values)
		{
			State.Attributes.Add(Value.Key, Value.Value);
		}
		return State;
	}

	TMap<FGameplayTag, float> Map(TConstArrayView<TPair<FGameplayTag, float>> Values)
	{
		TMap<FGameplayTag, float> Result;
		for (const TPair<FGameplayTag, float>& Value : Values)
		{
			Result.Add(Value.Key, Value.Value);
		}
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageShowPlaybackMixTest, "StageCraft.Show.PlaybackMix", StagePlaybackTests::Flags)

bool FStageShowPlaybackMixTest::RunTest(const FString& Parameters)
{
	using namespace StageShowMix;
	using namespace StagePlaybackTests;
	const FGameplayTag Dimmer = StageCraftTags::Attribute_Dimmer;
	const FGameplayTag Pan = StageCraftTags::Attribute_Pan;
	const FGameplayTag CellDimmer = StageFixtureCells::GetCellAttribute(2, EStageCellChannel::Dimmer);

	const FAttributeMap Home = Map({ { Dimmer, 0.f }, { Pan, 0.f }, { CellDimmer, 0.4f } });
	const FAttributeMap Main = Map({ { Dimmer, 0.5f }, { Pan, 10.f } });
	const FAttributeMap First = Map({ { Dimmer, 0.8f }, { Pan, 30.f }, { CellDimmer, 1.f } });
	const FAttributeMap Second = Map({ { Dimmer, 0.3f }, { Pan, 60.f } });

	FFixtureLayers Layers;
	Layers.Home = &Home;
	Layers.Playback = &Main;
	Layers.Playbacks.Add({ &First, 0.5f, 1 });
	Layers.Playbacks.Add({ &Second, 1.f, 2 });
	float Value = -1.f;
	TestTrue(TEXT("HTP: the main list's 0.5 beats 0.8 x 0.5 and 0.3"), ResolveAttribute(Dimmer, Layers, Value) && FMath::IsNearlyEqual(Value, 0.5f));
	TestTrue(TEXT("LTP: the playback activated last"), ResolveAttribute(Pan, Layers, Value) && Value == 60.f);
	Layers.Playbacks[0].Level = 1.f;
	Layers.Playbacks[0].Order = 3;
	TestTrue(TEXT("HTP: the first playback at full wins"), ResolveAttribute(Dimmer, Layers, Value) && FMath::IsNearlyEqual(Value, 0.8f));
	TestTrue(TEXT("LTP: activated again, it wins pan too"), ResolveAttribute(Pan, Layers, Value) && Value == 30.f);
	TestTrue(TEXT("A cell dimmer is intensity: HTP with its fader"), ResolveAttribute(CellDimmer, Layers, Value) && FMath::IsNearlyEqual(Value, 1.f));

	const FAttributeMap Programmer = Map({ { Pan, 90.f } });
	Layers.Programmer = &Programmer;
	TestTrue(TEXT("The programmer is above the playbacks"), ResolveAttribute(Pan, Layers, Value) && Value == 90.f);

	Layers.IntensityScale = 0.5f;
	TestTrue(TEXT("Grand Master halves intensity"), ResolveAttribute(Dimmer, Layers, Value) && FMath::IsNearlyEqual(Value, 0.4f));
	TestTrue(TEXT("... never position"), ResolveAttribute(Pan, Layers, Value) && Value == 90.f);
	const FAttributeMap Highlight = Map({ { Dimmer, 1.f } });
	Layers.Highlight = &Highlight;
	TestTrue(TEXT("... the highlight included"), ResolveAttribute(Dimmer, Layers, Value) && FMath::IsNearlyEqual(Value, 0.5f));
	Layers.IntensityScale = 0.f;
	TestTrue(TEXT("Blackout: zero"), ResolveAttribute(Dimmer, Layers, Value) && Value == 0.f);

	FFixtureLayers HomeOnly;
	HomeOnly.Home = &Home;
	HomeOnly.IntensityScale = 0.5f;
	TestTrue(TEXT("The home look is mastered too"), ResolveAttribute(CellDimmer, HomeOnly, Value) && FMath::IsNearlyEqual(Value, 0.2f));
	FFixtureLayers PlaybackOnly;
	PlaybackOnly.Playbacks.Add({ &Second, 0.5f, 1 });
	TestTrue(TEXT("A playback alone gives its scaled intensity"), ResolveAttribute(Dimmer, PlaybackOnly, Value) && FMath::IsNearlyEqual(Value, 0.15f));
	TestFalse(TEXT("Nobody holds tilt"), ResolveAttribute(StageCraftTags::Attribute_Tilt, PlaybackOnly, Value));

	// The rules.
	TArray<FStagePlayback> Playbacks;
	Playbacks.SetNum(StagePlaybackRules::Count + 2);
	FShowCue Bad;
	Bad.Number = -1.f;
	Playbacks[0].Cues = { Bad };
	TestEqual(TEXT("Extra playbacks and bad cues dropped"), StagePlaybackRules::SanitizePlaybacks(Playbacks), 3);
	TestEqual(TEXT("Always ten"), Playbacks.Num(), StagePlaybackRules::Count);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageShowPlaybacksTest, "StageCraft.Show.Playbacks", StagePlaybackTests::Flags)

bool FStageShowPlaybacksTest::RunTest(const FString& Parameters)
{
	using namespace StagePlaybackTests;
	const FGameplayTag Dimmer = StageCraftTags::Attribute_Dimmer;
	const FGameplayTag Pan = StageCraftTags::Attribute_Pan;

	FScopedWorld Scope;
	ALightingFixtureActor* A = Scope.SpawnMover();
	ALightingFixtureActor* B = Scope.SpawnMover();
	UShowControlSubsystem* Show = Scope.World->GetSubsystem<UShowControlSubsystem>();
	if (!TestNotNull(TEXT("A"), A) || !TestNotNull(TEXT("B"), B) || !TestNotNull(TEXT("Show"), Show))
	{
		return false;
	}
	const int32 IdA = A->GetFixtureId();
	const int32 IdB = B->GetFixtureId();
	TestEqual(TEXT("Ten playbacks"), Show->GetPlaybacks().Num(), StagePlaybackRules::Count);

	// The main list: A and B at 0.2, A pan 0; cue 2 takes A to 0 over 2 s.
	FShowCue Cue1;
	Cue1.Number = 1.f;
	Cue1.FadeTime = 0.f;
	Cue1.bBlock = true;
	Cue1.Fixtures = { Entry(IdA, { { Dimmer, 0.2f }, { Pan, 0.f } }), Entry(IdB, { { Dimmer, 0.2f } }) };
	FShowCue Cue2;
	Cue2.Number = 2.f;
	Cue2.FadeTime = 2.f;
	Cue2.Fixtures = { Entry(IdA, { { Dimmer, 0.f } }) };
	Show->SetCues({ Cue1, Cue2 });
	TestTrue(TEXT("Main cue 1"), Show->GoToCue(1.f));

	// Playback 1: A full at pan 45. Playback 2: B 0.6 pan -30, then (tracking) pan 15. Playback 3: A pan -60 at 0.
	TestTrue(TEXT("Record playback 1"), Show->RecordPlayback(0, { Entry(IdA, { { Dimmer, 1.f }, { Pan, 45.f } }) }));
	TestTrue(TEXT("Record playback 2"), Show->RecordPlayback(1, { Entry(IdB, { { Dimmer, 0.6f }, { Pan, -30.f } }) }));
	TestTrue(TEXT("Record playback 2, cue 2"), Show->RecordPlayback(1, { Entry(IdB, { { Pan, 15.f } }) }));
	TestTrue(TEXT("Record playback 3"), Show->RecordPlayback(2, { Entry(IdA, { { Dimmer, 0.f }, { Pan, -60.f } }) }));
	TestFalse(TEXT("Nothing to record"), Show->RecordPlayback(3, {}));
	TestFalse(TEXT("Bad index"), Show->RecordPlayback(StagePlaybackRules::Count, { Entry(IdA, { { Dimmer, 1.f } }) }));
	TestEqual(TEXT("Recording does not play"), A->GetAttribute(Dimmer), 0.2f, 1e-4f);

	// Fader start, HTP with the main list.
	TestTrue(TEXT("Playback 1 to 50%"), Show->SetPlaybackLevel(0, 0.5f));
	TestTrue(TEXT("Active at its first cue"), Show->IsPlaybackActive(0) && Show->GetPlaybackCueIndex(0) == 0);
	TestEqual(TEXT("HTP: 1.0 x 50% beats the main 0.2"), A->GetAttribute(Dimmer), 0.5f, 1e-4f);
	TestEqual(TEXT("LTP: pan from the playback"), A->GetAttribute(Pan), 45.f, 1e-3f);
	TestTrue(TEXT("Playback 1 to 10%"), Show->SetPlaybackLevel(0, 0.1f));
	TestEqual(TEXT("HTP: the main 0.2 beats 0.1"), A->GetAttribute(Dimmer), 0.2f, 1e-4f);

	// LTP by activation; flash takes it back.
	TestTrue(TEXT("Playback 3 up"), Show->SetPlaybackLevel(2, 1.f));
	TestEqual(TEXT("LTP: playback 3, raised last"), A->GetAttribute(Pan), -60.f, 1e-3f);
	TestTrue(TEXT("Flash playback 1"), Show->SetPlaybackFlash(0, true));
	TestEqual(TEXT("Flash: full"), A->GetAttribute(Dimmer), 1.f, 1e-4f);
	TestEqual(TEXT("Flash: LTP back to playback 1"), A->GetAttribute(Pan), 45.f, 1e-3f);
	TestTrue(TEXT("Let go"), Show->SetPlaybackFlash(0, false));
	TestEqual(TEXT("Back to its fader"), A->GetAttribute(Dimmer), 0.2f, 1e-4f);

	// Go through a tracking playback list.
	TestTrue(TEXT("Playback 2 up"), Show->SetPlaybackLevel(1, 1.f));
	TestEqual(TEXT("Playback 2 cue 1: B"), B->GetAttribute(Dimmer), 0.6f, 1e-4f);
	TestTrue(TEXT("Go playback 2"), Show->PlaybackGo(1));
	TestEqual(TEXT("Playback 2 cue 2: pan"), B->GetAttribute(Pan), 15.f, 1e-3f);
	TestEqual(TEXT("... dimmer tracked"), B->GetAttribute(Dimmer), 0.6f, 1e-4f);
	TestFalse(TEXT("At its end"), Show->PlaybackGo(1));

	// Masters.
	Show->SetGrandMaster(0.5f);
	TestEqual(TEXT("Grand Master 50%: B"), B->GetAttribute(Dimmer), 0.3f, 1e-4f);
	TestEqual(TEXT("... pan untouched"), B->GetAttribute(Pan), 15.f, 1e-3f);
	Show->SetBlackout(true);
	TestEqual(TEXT("Blackout: A"), A->GetAttribute(Dimmer), 0.f, 1e-4f);
	TestEqual(TEXT("Blackout: B"), B->GetAttribute(Dimmer), 0.f, 1e-4f);
	Show->SetBlackout(false);
	TestEqual(TEXT("Blackout off: B back"), B->GetAttribute(Dimmer), 0.3f, 1e-4f);

	// A look store records the look, not what the masters make of it.
	TestTrue(TEXT("Store cue 9 (Look)"), Show->StoreCue(9.f, FText::GetEmpty(), 0.f));
	const FShowCueFixtureState* Stored = Show->FindCue(9.f)->Fixtures.FindByPredicate([IdB](const FShowCueFixtureState& State) { return State.FixtureId == IdB; });
	TestTrue(TEXT("Recorded unmastered: 0.6"), Stored && FMath::IsNearlyEqual(Stored->Attributes.FindRef(Dimmer), 0.6f, 1e-4f));
	Show->SetGrandMaster(1.f);

	// A main fade with a fader up is mixed every frame.
	TestTrue(TEXT("Playback 1 to 40%"), Show->SetPlaybackLevel(0, 0.4f));
	TestTrue(TEXT("Release playback 3"), Show->ReleasePlayback(2));
	TestTrue(TEXT("Main cue 2"), Show->GoToCue(2.f));
	Show->Tick(1.f);
	TestEqual(TEXT("Halfway: max(main 0.1, 1.0 x 40%)"), A->GetAttribute(Dimmer), 0.4f, 1e-4f);
	TestTrue(TEXT("Playback 1 to 0"), Show->SetPlaybackLevel(0, 0.f));
	Show->Tick(0.5f);
	TestEqual(TEXT("Fader down mid-fade: the main 0.05"), A->GetAttribute(Dimmer), 0.05f, 1e-3f);
	Show->Tick(1.f);
	TestEqual(TEXT("Main fade done"), A->GetAttribute(Dimmer), 0.f, 1e-4f);
	TestEqual(TEXT("Pan back to the main list"), A->GetAttribute(Pan), 0.f, 1e-3f);

	// Release, clear and undo.
	TestTrue(TEXT("Release playback 2"), Show->ReleasePlayback(1));
	TestTrue(TEXT("Released"), !Show->IsPlaybackActive(1) && Show->GetPlaybackCueIndex(1) == INDEX_NONE && Show->GetPlaybackLevel(1) == 0.f);
	TestEqual(TEXT("B back to the main list"), B->GetAttribute(Dimmer), 0.2f, 1e-4f);
	FPlaybackEditor Editor(*Scope.World);
	const TArray<FStagePlayback> Before = Show->GetPlaybacks();
	TestTrue(TEXT("Clear playback 2"), Show->ClearPlayback(1));
	TestTrue(TEXT("Rename playback 1"), Show->RenamePlayback(0, FText::FromString(TEXT("Front"))));
	FStagePlaybacksCommand Step(FText::FromString(TEXT("Playbacks")), Before, Show->GetPlaybacks());
	TestTrue(TEXT("Undo"), Step.Undo(Editor) == EStageCommandResult::Succeeded);
	TestTrue(TEXT("Undo: playback 2's cues back, playback 1 unnamed"), Show->GetPlaybacks()[1].Cues.Num() == 2 && Show->GetPlaybacks()[0].Name.IsEmpty());
	TestTrue(TEXT("Redo"), Step.Redo(Editor) == EStageCommandResult::Succeeded);
	TestTrue(TEXT("Redo: cleared and named"), Show->GetPlaybacks()[1].Cues.IsEmpty() && Show->GetPlaybacks()[0].Name.ToString() == TEXT("Front"));

	// Loading releases every playback.
	TestTrue(TEXT("Playback 1 up"), Show->SetPlaybackLevel(0, 1.f));
	Show->SetPlaybacks(Show->GetPlaybacks());
	TestFalse(TEXT("A load releases it"), Show->IsPlaybackActive(0));
	TestTrue(TEXT("Bad level refused"), !Show->SetPlaybackLevel(0, std::numeric_limits<float>::quiet_NaN()) && !Show->SetPlaybackLevel(-1, 1.f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageScenePlaybackDataTest, "StageCraft.Scene.PlaybackData", StagePlaybackTests::Flags)

bool FStageScenePlaybackDataTest::RunTest(const FString& Parameters)
{
	using namespace StagePlaybackTests;
	FStageSceneDocument Document;
	Document.Name = TEXT("PlaybackData");
	Document.Playbacks.SetNum(StagePlaybackRules::Count);
	FShowCue First;
	First.Number = 1.f;
	First.FadeTime = 0.f;
	First.Fixtures = { Entry(3, { { StageCraftTags::Attribute_Dimmer, 1.f } }) };
	FShowCue Second = First;
	Second.Number = 2.f;
	Document.Playbacks[3].Name = FText::FromString(TEXT("Blinders"));
	Document.Playbacks[3].Cues = { First, Second };

	FStageSceneDocument Parsed;
	FStageSceneParseReport Report;
	TestTrue(TEXT("Round trip parses"), FStageSceneStore::Parse(FStageSceneStore::Serialize(Document), Parsed, Report) == EStageSceneResult::Success);
	TestEqual(TEXT("Nothing dropped"), Report.InvalidCueEntries, 0);
	TestTrue(TEXT("Playback 4 kept with its name and cues, the rest empty"), Parsed.Playbacks.Num() == StagePlaybackRules::Count
		&& Parsed.Playbacks[3].Name.ToString() == TEXT("Blinders") && Parsed.Playbacks[3].Cues.Num() == 2 && Parsed.Playbacks[0].Cues.IsEmpty());

	const FString Hostile = FString::Printf(TEXT("{\"format\":\"StageCraftScene\",\"version\":%d,\"name\":\"H\",\"items\":[],\"playbacks\":[")
		TEXT("{\"index\":12,\"cues\":[]},{\"index\":1.5},{\"index\":2,\"name\":\"Two\",\"cues\":[{\"number\":-4},{\"number\":1,\"fade\":0,\"fixtures\":[]}]},")
		TEXT("{\"index\":2,\"name\":\"Again\"},\"x\"]}"), FStageSceneStore::FormatVersion);
	TestTrue(TEXT("A hostile file still parses"), FStageSceneStore::Parse(Hostile, Parsed = FStageSceneDocument(), Report = FStageSceneParseReport()) == EStageSceneResult::Success);
	TestTrue(TEXT("Playback 3 keeps its good cue"), Parsed.Playbacks.Num() == StagePlaybackRules::Count && Parsed.Playbacks[2].Name.ToString() == TEXT("Two")
		&& Parsed.Playbacks[2].Cues.Num() == 1);
	TestEqual(TEXT("Each bad entry counted"), Report.InvalidCueEntries, 5);

	const FString V7 = TEXT("{\"format\":\"StageCraftScene\",\"version\":7,\"name\":\"Old\",\"items\":[],\"playbacks\":[{\"index\":0,\"name\":\"X\"}]}");
	TestTrue(TEXT("Version 7 parses"), FStageSceneStore::Parse(V7, Parsed = FStageSceneDocument(), Report = FStageSceneParseReport()) == EStageSceneResult::Success);
	TestTrue(TEXT("Version 7: ten empty playbacks"), Parsed.Playbacks.Num() == StagePlaybackRules::Count && Parsed.Playbacks[0].Name.IsEmpty());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
