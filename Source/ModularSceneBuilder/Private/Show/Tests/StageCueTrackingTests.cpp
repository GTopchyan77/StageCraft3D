// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for cue timing and tracking (Docs/ADR/0026-cue-timing-tracking-playbacks.md): the pure tracking and timing rules, the fade
// engine on real fixtures (delay, In / Out / feature times, a Follow and Wait chain, Back, release to the home look, store modes and
// Block), and the scene format v8 cue fields. The Cue panel is exercised by the scripted -game run (STATE.md #61).
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Show.Cue; Automation RunTests StageCraft.Scene.CueTimingData; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Actors/LightingFixtureActor.h"
#include "Data/LightingFixtureData.h"
#include "Data/StageParameterTypes.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/AutomationTest.h"
#include "Scene/StageSceneStore.h"
#include "Show/StageCueTracking.h"
#include "Subsystems/ShowControlSubsystem.h"
#include "Subsystems/StageProgrammerSubsystem.h"

namespace StageCueTrackingTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	/** A game world that has begun play, destroyed with the scope. Fixtures register with its show control on BeginPlay. */
	struct FScopedWorld
	{
		UWorld* World = nullptr;

		FScopedWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("StageCueTrackingTestWorld"));
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

	FShowCue MakeCue(float Number, float Fade, TArray<FShowCueFixtureState> Fixtures, bool bBlock = false)
	{
		FShowCue Cue;
		Cue.Number = Number;
		Cue.FadeTime = Fade;
		Cue.bBlock = bBlock;
		Cue.Fixtures = MoveTemp(Fixtures);
		return Cue;
	}

	float ValueIn(const StageCueTracking::FFixtureStates& State, int32 FixtureId, const FGameplayTag& Attribute)
	{
		const FShowCueFixtureState* Fixture = State.Find(FixtureId);
		const float* Value = Fixture ? Fixture->Attributes.Find(Attribute) : nullptr;
		return Value ? *Value : -999.f;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageShowCueTrackingTest, "StageCraft.Show.CueTracking", StageCueTrackingTests::Flags)

bool FStageShowCueTrackingTest::RunTest(const FString& Parameters)
{
	using namespace StageCueTrackingTests;
	const FGameplayTag Dimmer = StageCraftTags::Attribute_Dimmer;
	const FGameplayTag Pan = StageCraftTags::Attribute_Pan;

	// Tracking: each attribute keeps the latest value set at or before the cue.
	TArray<FShowCue> Cues = {
		MakeCue(1.f, 0.f, { Entry(1, { { Dimmer, 1.f }, { Pan, 10.f } }) }),
		MakeCue(2.f, 0.f, { Entry(1, { { Pan, 20.f } }) }),
		MakeCue(3.f, 0.f, { Entry(2, { { Dimmer, 0.5f } }) }),
	};
	StageCueTracking::FFixtureStates State;
	StageCueTracking::StateAt(Cues, 2, State);
	TestEqual(TEXT("Cue 3: fixture 1 dimmer tracked from cue 1"), ValueIn(State, 1, Dimmer), 1.f);
	TestEqual(TEXT("Cue 3: fixture 1 pan tracked from cue 2"), ValueIn(State, 1, Pan), 20.f);
	TestEqual(TEXT("Cue 3: fixture 2 from cue 3"), ValueIn(State, 2, Dimmer), 0.5f);
	StageCueTracking::StateAt(Cues, 0, State);
	TestFalse(TEXT("Cue 1 does not see later cues"), State.Contains(2));
	StageCueTracking::StateAt(Cues, 7, State);
	TestTrue(TEXT("Out of range: empty"), State.IsEmpty());

	// A block inherits nothing.
	Cues[2].bBlock = true;
	StageCueTracking::StateAt(Cues, 2, State);
	TestTrue(TEXT("Blocked cue 3: only its own fixture"), State.Num() == 1 && State.Contains(2));
	Cues[2].bBlock = false;

	// References track too, and a later plain value drops them.
	const FGuid Palette = FGuid::NewGuid();
	Cues[0].Fixtures[0].PaletteRefs.Add(Pan, Palette);
	StageCueTracking::StateAt(Cues, 0, State);
	TestTrue(TEXT("Cue 1: pan follows the palette"), State[1].PaletteRefs.FindRef(Pan) == Palette);
	StageCueTracking::StateAt(Cues, 1, State);
	TestFalse(TEXT("Cue 2's plain pan drops the reference"), State[1].PaletteRefs.Contains(Pan));

	// Merge: values replaced or added, fixtures appended.
	FShowCue Merged = Cues[1];
	StageCueTracking::MergeInto(Merged, { Entry(1, { { Dimmer, 0.3f } }), Entry(5, { { Pan, -5.f } }) });
	TestTrue(TEXT("Merge keeps cue 2's pan and adds the dimmer"), Merged.Fixtures.Num() == 2 && Merged.Fixtures[0].Attributes.Num() == 2
		&& Merged.Fixtures[0].Attributes.FindRef(Dimmer) == 0.3f);

	// Cue Only: the next cue keeps the old value of what changed, unless it sets it itself; home when nothing tracked.
	TArray<FShowCue> Pair = { MakeCue(1.f, 0.f, { Entry(1, { { Dimmer, 0.4f } }) }), MakeCue(2.f, 0.f, { Entry(1, { { Pan, 45.f } }) }) };
	StageCueTracking::FFixtureStates Before;
	StageCueTracking::StateAt(Pair, 0, Before);
	const TArray<FShowCueFixtureState> Changed = { Entry(1, { { Dimmer, 1.f }, { Pan, 5.f }, { StageCraftTags::Attribute_Zoom, 30.f } }), Entry(9, { { Dimmer, 1.f } }) };
	StageCueTracking::MergeInto(Pair[0], Changed);
	const int32 Written = StageCueTracking::KeepNextCue(Pair, 0, Before, Changed, [](int32 FixtureId, const FGameplayTag& Attribute, float& OutHome)
	{
		OutHome = 12.f;
		return FixtureId == 1; // Fixture 9 is not on stage: nothing to go back to.
	});
	TestEqual(TEXT("Cue Only wrote dimmer (old) and zoom (home)"), Written, 2);
	StageCueTracking::StateAt(Pair, 1, State);
	TestEqual(TEXT("Cue 2 keeps the old dimmer"), ValueIn(State, 1, Dimmer), 0.4f);
	TestEqual(TEXT("Cue 2 keeps its own pan"), ValueIn(State, 1, Pan), 45.f);
	TestEqual(TEXT("Cue 2 sends zoom home"), ValueIn(State, 1, StageCraftTags::Attribute_Zoom), 12.f);
	TestEqual(TEXT("A fixture not on stage still tracks"), ValueIn(State, 9, Dimmer), 1.f);

	// Timing per attribute.
	FShowCue Timed = MakeCue(1.f, 2.f, {});
	Timed.Timing.Delay = 0.5f;
	Timed.Timing.bHasOutTime = true;
	Timed.Timing.OutTime = 6.f;
	Timed.Timing.FeatureTimes.Add(EStagePaletteKind::Position, 4.f);
	float Delay = -1.f;
	float Duration = -1.f;
	StageCueTiming::GetAttributeTiming(Timed, Dimmer, /*bGoingDown*/ false, Delay, Duration);
	TestTrue(TEXT("Dimmer up: In time, cue delay"), Delay == 0.5f && Duration == 2.f);
	StageCueTiming::GetAttributeTiming(Timed, Dimmer, /*bGoingDown*/ true, Delay, Duration);
	TestEqual(TEXT("Dimmer down: Out time"), Duration, 6.f);
	StageCueTiming::GetAttributeTiming(Timed, Pan, /*bGoingDown*/ true, Delay, Duration);
	TestEqual(TEXT("Pan going down: its feature time, not Out"), Duration, 4.f);
	StageCueTiming::GetAttributeTiming(Timed, StageCraftTags::Attribute_Zoom, false, Delay, Duration);
	TestEqual(TEXT("Zoom: In time"), Duration, 2.f);
	TestEqual(TEXT("Before the delay: not started"), StageCueTiming::GetProgress(0.4f, 0.5f, 2.f), 0.f);
	TestEqual(TEXT("Half way"), StageCueTiming::GetProgress(1.5f, 0.5f, 2.f), 0.5f);
	TestEqual(TEXT("Past the end: done"), StageCueTiming::GetProgress(9.f, 0.5f, 2.f), 1.f);
	TestEqual(TEXT("Zero time jumps at its delay"), StageCueTiming::GetProgress(0.5f, 0.5f, 0.f), 1.f);
	TestEqual(TEXT("Linear"), StageCueTiming::ApplyCurve(EStageCueCurve::Linear, 0.25f), 0.25f);
	TestEqual(TEXT("S-curve eases in"), StageCueTiming::ApplyCurve(EStageCueCurve::SCurve, 0.25f), 0.15625f, 1e-6f);
	TestEqual(TEXT("S-curve half way is half"), StageCueTiming::ApplyCurve(EStageCueCurve::SCurve, 0.5f), 0.5f, 1e-6f);

	// Timing rules.
	FStageCueTiming Hostile;
	Hostile.Delay = std::numeric_limits<float>::quiet_NaN();
	Hostile.bHasOutTime = true;
	Hostile.OutTime = 5000.f;
	Hostile.WaitTime = -3.f;
	Hostile.FeatureTimes.Add(EStagePaletteKind::All, 3.f);
	Hostile.FeatureTimes.Add(EStagePaletteKind::Color, 3.f);
	TestEqual(TEXT("All is not a feature time"), StageCueRules::SanitizeTiming(Hostile), 1);
	TestTrue(TEXT("Times clamped"), Hostile.Delay == 0.f && Hostile.OutTime == StageCueRules::MaxFadeTime && Hostile.WaitTime == 0.f
		&& Hostile.FeatureTimes.Num() == 1);
	FStageCueTiming Same = Hostile;
	TestTrue(TEXT("Equal timings"), Same.Equals(Hostile));
	Same.Curve = EStageCueCurve::SCurve;
	TestFalse(TEXT("A different curve"), Same.Equals(Hostile));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageShowCueTimingTest, "StageCraft.Show.CueTiming", StageCueTrackingTests::Flags)

bool FStageShowCueTimingTest::RunTest(const FString& Parameters)
{
	using namespace StageCueTrackingTests;
	const FGameplayTag Dimmer = StageCraftTags::Attribute_Dimmer;
	const FGameplayTag Pan = StageCraftTags::Attribute_Pan;

	FScopedWorld Scope;
	ALightingFixtureActor* A = Scope.SpawnMover();
	ALightingFixtureActor* B = Scope.SpawnMover();
	UShowControlSubsystem* Show = Scope.World->GetSubsystem<UShowControlSubsystem>();
	UStageProgrammerSubsystem* Programmer = Scope.World->GetSubsystem<UStageProgrammerSubsystem>();
	if (!TestNotNull(TEXT("A"), A) || !TestNotNull(TEXT("B"), B) || !TestNotNull(TEXT("Show"), Show) || !TestNotNull(TEXT("Programmer"), Programmer))
	{
		return false;
	}
	const int32 IdA = A->GetFixtureId();
	const int32 IdB = B->GetFixtureId();
	const float HomePan = A->GetHomeAttributes().FindRef(Pan);

	// 1: everything at 0 (blocked). 2: A up with a delay; pan on its own (Position) time. 3: A down on an Out time, the rest tracks.
	// 4: Follow, B up at once. 5: Wait 0.5 s, A pan across. 6: blocked, B only (A goes home).
	FShowCue Cue2 = MakeCue(2.f, 2.f, { Entry(IdA, { { Dimmer, 1.f }, { Pan, 90.f } }) });
	Cue2.Timing.Delay = 1.f;
	Cue2.Timing.FeatureTimes.Add(EStagePaletteKind::Position, 4.f);
	FShowCue Cue3 = MakeCue(3.f, 5.f, { Entry(IdA, { { Dimmer, 0.2f } }) });
	Cue3.Timing.bHasOutTime = true;
	Cue3.Timing.OutTime = 1.f;
	FShowCue Cue4 = MakeCue(4.f, 0.f, { Entry(IdB, { { Dimmer, 1.f } }) });
	Cue4.Timing.Trigger = EStageCueTrigger::Follow;
	FShowCue Cue5 = MakeCue(5.f, 0.f, { Entry(IdA, { { Pan, -90.f } }) });
	Cue5.Timing.Trigger = EStageCueTrigger::Wait;
	Cue5.Timing.WaitTime = 0.5f;
	Show->SetCues({ MakeCue(1.f, 0.f, { Entry(IdA, { { Dimmer, 0.f }, { Pan, 0.f } }), Entry(IdB, { { Dimmer, 0.f } }) }, /*bBlock*/ true),
		Cue2, Cue3, Cue4, Cue5, MakeCue(6.f, 0.f, { Entry(IdB, { { Dimmer, 0.5f } }) }, /*bBlock*/ true) });

	TestTrue(TEXT("Go to cue 1"), Show->GoToCue(1.f));
	TestFalse(TEXT("Cue 1 is instant"), Show->IsFading());

	// Delay, In time and a feature time.
	TestTrue(TEXT("Go to cue 2"), Show->GoToCue(2.f));
	Show->Tick(0.5f);
	TestEqual(TEXT("0.5 s: inside the delay, dimmer still 0"), A->GetAttribute(Dimmer), 0.f, 1e-4f);
	Show->Tick(1.f);
	TestEqual(TEXT("1.5 s: dimmer a quarter in (2 s In)"), A->GetAttribute(Dimmer), 0.25f, 1e-3f);
	TestEqual(TEXT("1.5 s: pan an eighth in (4 s Position)"), A->GetAttribute(Pan), 11.25f, 1e-2f);
	Show->Tick(1.5f);
	TestEqual(TEXT("3 s: dimmer there"), A->GetAttribute(Dimmer), 1.f, 1e-3f);
	TestEqual(TEXT("3 s: pan half way"), A->GetAttribute(Pan), 45.f, 1e-2f);
	TestTrue(TEXT("Still fading for the pan"), Show->IsFading());
	Show->Tick(2.5f);
	TestFalse(TEXT("Done after the longest move"), Show->IsFading());
	TestEqual(TEXT("Pan there"), A->GetAttribute(Pan), 90.f, 1e-2f);

	// Out time for intensity going down; the pan tracks; then the Follow and Wait chain.
	TestTrue(TEXT("Go to cue 3"), Show->GoToCue(3.f));
	TestTrue(TEXT("Cue 4 (Follow) is armed"), Show->IsAutoGoArmed());
	Show->Tick(0.5f);
	TestEqual(TEXT("0.5 s: dimmer half way down on the 1 s Out time"), A->GetAttribute(Dimmer), 0.6f, 1e-3f);
	TestEqual(TEXT("Pan tracks from cue 2"), A->GetAttribute(Pan), 90.f, 1e-2f);
	TestEqual(TEXT("B not started"), B->GetAttribute(Dimmer), 0.f, 1e-4f);
	Show->Tick(0.6f);
	TestEqual(TEXT("Cue 3 done"), A->GetAttribute(Dimmer), 0.2f, 1e-3f);
	TestEqual(TEXT("Cue 4 followed in the same tick"), Show->GetActiveCueNumber(), 4.f);
	TestEqual(TEXT("B up"), B->GetAttribute(Dimmer), 1.f, 1e-4f);
	TestTrue(TEXT("Cue 5 (Wait) is armed"), Show->IsAutoGoArmed());
	Show->Tick(0.3f);
	TestEqual(TEXT("0.3 s: cue 5 still waiting"), A->GetAttribute(Pan), 90.f, 1e-2f);
	Show->Tick(0.3f);
	TestEqual(TEXT("0.6 s: cue 5 played"), A->GetAttribute(Pan), -90.f, 1e-2f);
	TestFalse(TEXT("Cue 6 has trigger Go: nothing armed"), Show->IsAutoGoArmed());

	// Back runs no chain, and plays the earlier cue's tracked look.
	TestTrue(TEXT("Back to cue 4"), Show->Back());
	TestFalse(TEXT("Back arms nothing"), Show->IsAutoGoArmed());
	TestEqual(TEXT("Back: pan as tracked into cue 4"), A->GetAttribute(Pan), 90.f, 1e-2f);

	// A blocked cue sends what it does not hold home.
	TestTrue(TEXT("Go to cue 6"), Show->GoToCue(6.f));
	TestEqual(TEXT("Cue 6: A pan home"), A->GetAttribute(Pan), HomePan, 1e-2f);
	TestEqual(TEXT("Cue 6: B at its value"), B->GetAttribute(Dimmer), 0.5f, 1e-4f);

	// A manual Go cancels an armed chain.
	TestTrue(TEXT("Go to cue 3 again"), Show->GoToCue(3.f));
	TestTrue(TEXT("Armed"), Show->IsAutoGoArmed());
	TestTrue(TEXT("Go to cue 1"), Show->GoToCue(1.f));
	TestFalse(TEXT("Cue 2 has trigger Go: nothing armed after cue 1"), Show->IsAutoGoArmed());

	// Store modes.
	TestFalse(TEXT("Tracking store with an empty programmer is refused"), Show->StoreCue(2.5f, FText::GetEmpty(), 1.f, EStageCueStoreMode::Track));
	Programmer->SetValues(IdA, { { Pan, 30.f } });
	TestTrue(TEXT("Track store of 2.5"), Show->StoreCue(2.5f, FText::FromString(TEXT("Pan")), 1.f, EStageCueStoreMode::Track));
	const FShowCue* Cue25 = Show->FindCue(2.5f);
	TestTrue(TEXT("2.5 holds only A's pan"), Cue25 && !Cue25->bBlock && Cue25->Fixtures.Num() == 1 && Cue25->Fixtures[0].Attributes.Num() == 1);
	Programmer->ClearAll();
	Programmer->SetValues(IdA, { { Dimmer, 0.7f } });
	TestTrue(TEXT("Cue Only store into cue 2"), Show->StoreCue(2.f, FText::GetEmpty(), 2.f, EStageCueStoreMode::CueOnly));
	StageCueTracking::FFixtureStates State;
	Show->GetCueState(2.f, State);
	TestEqual(TEXT("Cue 2 now dims A to 0.7"), ValueIn(State, IdA, Dimmer), 0.7f);
	Show->GetCueState(2.5f, State);
	TestEqual(TEXT("Cue 2.5 keeps the old 1.0"), ValueIn(State, IdA, Dimmer), 1.f);
	TestEqual(TEXT("Cue 2 kept its label and timing"), Show->FindCue(2.f)->Timing.Delay, 1.f);
	Programmer->ClearAll();

	// Block keeps the look; Look stores a blocked snapshot of every fixture.
	Show->GetCueState(3.f, State);
	const float TrackedPan = ValueIn(State, IdA, Pan);
	TestTrue(TEXT("Block cue 3"), Show->SetCueBlocked(3.f, true));
	Show->GetCueState(3.f, State);
	TestTrue(TEXT("Blocked cue 3 shows the same look"), Show->FindCue(3.f)->bBlock && ValueIn(State, IdA, Pan) == TrackedPan);
	TestTrue(TEXT("Look store of cue 7"), Show->StoreCue(7.f, FText::GetEmpty(), 0.f, EStageCueStoreMode::Look));
	TestTrue(TEXT("Cue 7 is a blocked snapshot of both fixtures"), Show->FindCue(7.f)->bBlock && Show->FindCue(7.f)->Fixtures.Num() == 2);
	TestTrue(TEXT("Timing update"), Show->UpdateCueTiming(7.f, Cue5.Timing));
	TestTrue(TEXT("Cue 7 waits now"), Show->FindCue(7.f)->Timing.Trigger == EStageCueTrigger::Wait);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageSceneCueTimingDataTest, "StageCraft.Scene.CueTimingData", StageCueTrackingTests::Flags)

bool FStageSceneCueTimingDataTest::RunTest(const FString& Parameters)
{
	using namespace StageCueTrackingTests;
	FStageSceneDocument Document;
	Document.Name = TEXT("CueTiming");
	FShowCue Cue = MakeCue(1.5f, 3.f, { Entry(2, { { StageCraftTags::Attribute_Dimmer, 0.5f } }) });
	Cue.Timing.Delay = 0.25f;
	Cue.Timing.bHasOutTime = true;
	Cue.Timing.OutTime = 7.f;
	Cue.Timing.FeatureTimes.Add(EStagePaletteKind::Color, 1.5f);
	Cue.Timing.Trigger = EStageCueTrigger::Wait;
	Cue.Timing.WaitTime = 4.f;
	Cue.Timing.Curve = EStageCueCurve::SCurve;
	FShowCue Blocked = MakeCue(2.f, 1.f, {}, /*bBlock*/ true);
	Document.Cues = { Cue, Blocked };

	FStageSceneDocument Parsed;
	FStageSceneParseReport Report;
	TestTrue(TEXT("Round trip parses"), FStageSceneStore::Parse(FStageSceneStore::Serialize(Document), Parsed, Report) == EStageSceneResult::Success);
	TestEqual(TEXT("Nothing dropped"), Report.InvalidCueEntries, 0);
	TestTrue(TEXT("Two cues"), Parsed.Cues.Num() == 2);
	if (Parsed.Cues.Num() == 2)
	{
		TestTrue(TEXT("Timing kept"), Parsed.Cues[0].Timing.Equals(Cue.Timing) && !Parsed.Cues[0].bBlock);
		TestTrue(TEXT("Block kept, no Out time invented"), Parsed.Cues[1].bBlock && !Parsed.Cues[1].Timing.bHasOutTime);
	}

	// Hostile v8 timing: bad values are dropped and counted, the cue keeps its defaults for them.
	const FString Hostile = FString::Printf(TEXT("{\"format\":\"StageCraftScene\",\"version\":%d,\"name\":\"H\",\"items\":[],\"cues\":[")
		TEXT("{\"number\":1,\"fade\":2,\"delay\":-1,\"out\":\"x\",\"wait\":1e9,\"times\":{\"All\":2,\"Laser\":1,\"Color\":-2,\"Position\":3},")
		TEXT("\"trigger\":\"Sometimes\",\"curve\":\"Bouncy\",\"block\":\"yes\",\"fixtures\":[]}]}"), FStageSceneStore::FormatVersion);
	TestTrue(TEXT("A hostile file still parses"), FStageSceneStore::Parse(Hostile, Parsed = FStageSceneDocument(), Report = FStageSceneParseReport()) == EStageSceneResult::Success);
	if (TestEqual(TEXT("The cue loads"), Parsed.Cues.Num(), 1))
	{
		const FShowCue& Loaded = Parsed.Cues[0];
		TestTrue(TEXT("Defaults where values were bad; the good feature time kept"), Loaded.Timing.Delay == 0.f && !Loaded.Timing.bHasOutTime
			&& Loaded.Timing.WaitTime == 0.f && Loaded.Timing.Trigger == EStageCueTrigger::Go && Loaded.Timing.Curve == EStageCueCurve::Linear
			&& !Loaded.bBlock && Loaded.Timing.FeatureTimes.Num() == 1 && Loaded.Timing.FeatureTimes.FindRef(EStagePaletteKind::Position) == 3.f);
	}
	TestEqual(TEXT("Each bad value counted"), Report.InvalidCueEntries, 9);

	// Cues of version 7 and older were snapshots: they load blocked.
	const FString V7 = TEXT("{\"format\":\"StageCraftScene\",\"version\":7,\"name\":\"Old\",\"items\":[],\"cues\":[{\"number\":1,\"fade\":2,\"delay\":5,\"fixtures\":[]}]}");
	TestTrue(TEXT("Version 7 parses"), FStageSceneStore::Parse(V7, Parsed = FStageSceneDocument(), Report = FStageSceneParseReport()) == EStageSceneResult::Success);
	TestTrue(TEXT("Version 7 cue: blocked, no timing read"), Parsed.Cues.Num() == 1 && Parsed.Cues[0].bBlock && Parsed.Cues[0].Timing.Delay == 0.f);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
