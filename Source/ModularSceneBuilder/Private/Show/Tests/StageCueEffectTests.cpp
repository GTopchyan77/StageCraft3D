// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for effects in cues and playbacks (Docs/ADR/0027-effects-engine.md §5): tracking (start, replace, stop, block), Track and
// Cue Only stores, the runner's fades, morph and fader scale, effects composing on one attribute, and on real fixtures the cue list,
// playbacks and the programmer as sources (stores, Go, Back, Go To, restore, stops, take-over, Look, Block, External); the scene
// format v10 "effects", "effectStops" and "programmerEffectStops". The controller requests and panels are exercised by the scripted
// -game run (STATE.md #64).
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Show.EffectTracking; Automation RunTests StageCraft.Show.CueEffects; Automation RunTests StageCraft.Scene.CueEffectData; Quit" -unattended -nullrhi

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
#include "Show/StageEffectRunner.h"
#include "Show/StageEffects.h"
#include "Show/StageShowMix.h"
#include "Subsystems/ShowControlSubsystem.h"
#include "Subsystems/StageProgrammerSubsystem.h"

namespace StageCueEffectTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;
	constexpr float Tolerance = 1e-3f;

	struct FScopedWorld
	{
		UWorld* World = nullptr;

		FScopedWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("StageCueEffectTestWorld"));
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

	FStageEffect MakeEffect(EStageEffectTarget Target, EStageEffectWaveform Waveform, float RateHz, const TCHAR* Name)
	{
		FStageEffect Effect;
		Effect.Name = FText::FromString(Name);
		Effect.Target = Target;
		Effect.Waveform = Waveform;
		Effect.RateHz = RateHz;
		Effect.Spread = 0.f;
		return Effect;
	}

	FStageEffectEntry MakeEntry(const FGuid& Id, const FStageEffect& Effect, TArray<int32> FixtureIds)
	{
		FStageEffectEntry Entry;
		Entry.Id = Id;
		Entry.Effect = Effect;
		Entry.FixtureIds = MoveTemp(FixtureIds);
		return Entry;
	}

	FShowCue MakeCue(float Number, float FadeTime, TArray<FStageEffectEntry> Effects, TArray<FGuid> Stops = {}, bool bBlock = false)
	{
		FShowCue Cue;
		Cue.Number = Number;
		Cue.FadeTime = FadeTime;
		Cue.Effects = MoveTemp(Effects);
		Cue.EffectStops = MoveTemp(Stops);
		Cue.bBlock = bBlock;
		return Cue;
	}

	TArray<FGuid> IdsOf(TConstArrayView<FStageEffectEntry> Entries)
	{
		TArray<FGuid> Ids;
		for (const FStageEffectEntry& Entry : Entries)
		{
			Ids.Add(Entry.Id);
		}
		return Ids;
	}

	/** The running effect with Id, or null. */
	const FStageRunningEffect* FindRunning(const UShowControlSubsystem& Show, const FGuid& Id, TArray<FStageRunningEffect>& Storage)
	{
		Show.GetRunningEffects(Storage);
		return Storage.FindByPredicate([&Id](const FStageRunningEffect& Effect) { return Effect.Entry.Id == Id; });
	}

	ALightingFixtureActor* FindNone(int32)
	{
		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageShowEffectTrackingTest, "StageCraft.Show.EffectTracking", StageCueEffectTests::Flags)

bool FStageShowEffectTrackingTest::RunTest(const FString& Parameters)
{
	using namespace StageCueEffectTests;
	const FGuid IdA = FGuid::NewGuid();
	const FGuid IdB = FGuid::NewGuid();
	const FGuid IdC = FGuid::NewGuid();
	const FStageEffect Circle = MakeEffect(EStageEffectTarget::Position, EStageEffectWaveform::Circle, 1.f, TEXT("Circle"));
	FStageEffect Bigger = Circle;
	Bigger.Size = 60.f;
	const FStageEffect Chase = MakeEffect(EStageEffectTarget::Intensity, EStageEffectWaveform::Square, 2.f, TEXT("Chase"));

	// Tracking: start, track, stop, replace (moves last), block.
	TArray<FShowCue> Cues;
	Cues.Add(MakeCue(1.f, 0.f, { MakeEntry(IdA, Circle, { 1, 2 }) }));
	Cues.Add(MakeCue(2.f, 0.f, { MakeEntry(IdB, Chase, { 1 }) }));
	Cues.Add(MakeCue(3.f, 0.f, {}, { IdA }));
	Cues.Add(MakeCue(4.f, 0.f, { MakeEntry(IdA, Bigger, { 2, 1 }) }));
	Cues.Add(MakeCue(5.f, 0.f, {}, {}, /*bBlock*/ true));
	Cues.Add(MakeCue(6.f, 0.f, { MakeEntry(IdC, Chase, { 3 }) }));
	TArray<FStageEffectEntry> At;
	StageCueTracking::EffectsAt(Cues, 0, At);
	TestTrue(TEXT("Cue 1 starts A"), IdsOf(At) == TArray<FGuid>({ IdA }));
	StageCueTracking::EffectsAt(Cues, 1, At);
	TestTrue(TEXT("Cue 2: A tracks, B starts"), IdsOf(At) == TArray<FGuid>({ IdA, IdB }));
	StageCueTracking::EffectsAt(Cues, 2, At);
	TestTrue(TEXT("Cue 3 stops A"), IdsOf(At) == TArray<FGuid>({ IdB }));
	StageCueTracking::EffectsAt(Cues, 3, At);
	TestTrue(TEXT("Cue 4 starts A again, latest last"), IdsOf(At) == TArray<FGuid>({ IdB, IdA }) && At[1].Effect.Size == 60.f && At[1].FixtureIds == TArray<int32>({ 2, 1 }));
	StageCueTracking::EffectsAt(Cues, 4, At);
	TestTrue(TEXT("A blocked cue starts from none"), At.IsEmpty());
	StageCueTracking::EffectsAt(Cues, 5, At);
	TestTrue(TEXT("After the block only its own"), IdsOf(At) == TArray<FGuid>({ IdC }));
	StageCueTracking::EffectsAt(Cues, 9, At);
	TestTrue(TEXT("Out of range: none"), At.IsEmpty());
	Cues[1].Effects.Add(MakeEntry(IdA, Bigger, { 1 }));
	StageCueTracking::EffectsAt(Cues, 1, At);
	TestTrue(TEXT("A later entry of the same id replaces it in place of order"), IdsOf(At) == TArray<FGuid>({ IdB, IdA }) && At[1].Effect.Size == 60.f);
	Cues[1].Effects.Pop();

	// Merge: an effect removes a stop of its id, a stop removes the effect.
	FShowCue Merge = MakeCue(7.f, 0.f, { MakeEntry(IdB, Chase, { 1 }) }, { IdA });
	StageCueTracking::MergeEffectsInto(Merge, { MakeEntry(IdA, Circle, { 4 }), MakeEntry(IdB, Bigger, { 1 }) }, {});
	TestTrue(TEXT("Merge: A started, its stop gone, B replaced"), IdsOf(Merge.Effects) == TArray<FGuid>({ IdB, IdA }) && Merge.EffectStops.IsEmpty()
		&& Merge.Effects[0].Effect.Size == 60.f);
	StageCueTracking::MergeEffectsInto(Merge, {}, { IdB, IdB });
	TestTrue(TEXT("Merge a stop: B gone, one stop"), IdsOf(Merge.Effects) == TArray<FGuid>({ IdA }) && Merge.EffectStops == TArray<FGuid>({ IdB }));

	// Cue Only: the next cue keeps what it had. A changed effect goes back to its old version, a new one stops, a stopped one restarts.
	TArray<FShowCue> Only;
	Only.Add(MakeCue(1.f, 0.f, { MakeEntry(IdA, Circle, { 1 }) }));
	Only.Add(MakeCue(2.f, 0.f, {}));
	Only.Add(MakeCue(3.f, 0.f, {}));
	TArray<FStageEffectEntry> Before;
	StageCueTracking::EffectsAt(Only, 0, Before);
	const TArray<FStageEffectEntry> Stored = { MakeEntry(IdA, Bigger, { 1 }), MakeEntry(IdB, Chase, { 2 }) };
	StageCueTracking::MergeEffectsInto(Only[0], Stored, {});
	TestEqual(TEXT("Cue Only writes two entries"), StageCueTracking::KeepNextCueEffects(Only, 0, Before, Stored, {}), 2);
	TestTrue(TEXT("... A as it was, B stopped"), Only[1].Effects.Num() == 1 && Only[1].Effects[0].Effect.Size == Circle.Size && Only[1].EffectStops == TArray<FGuid>({ IdB }));
	StageCueTracking::EffectsAt(Only, 1, At);
	TestTrue(TEXT("Cue 2 looks as before the store"), At.Num() == 1 && At[0].Id == IdA && At[0].Effect.Size == Circle.Size);
	TestEqual(TEXT("The next cue naming the ids itself: nothing"), StageCueTracking::KeepNextCueEffects(Only, 0, Before, Stored, {}), 0);
	StageCueTracking::EffectsAt(Only, 1, Before);
	TestEqual(TEXT("A stored stop: the next cue restarts the effect"), StageCueTracking::KeepNextCueEffects(Only, 1, Before, {}, { IdA }), 1);
	TestTrue(TEXT("... A in cue 3"), Only[2].Effects.Num() == 1 && Only[2].Effects[0].Id == IdA);
	Only[2] = MakeCue(3.f, 0.f, {}, {}, /*bBlock*/ true);
	TestEqual(TEXT("A blocked next cue takes nothing"), StageCueTracking::KeepNextCueEffects(Only, 1, Before, Stored, {}), 0);
	TestEqual(TEXT("No next cue: nothing"), StageCueTracking::KeepNextCueEffects(Only, 2, Before, Stored, {}), 0);

	// Rules: stops must be valid, unique, not an effect of the same cue, at most MaxEffects.
	FShowCue Dirty = MakeCue(1.f, 0.f, { MakeEntry(IdA, Circle, { 1, 1, -2 }), MakeEntry(FGuid(), Circle, { 1 }) }, { FGuid(), IdB, IdB, IdA });
	TestEqual(TEXT("SanitizeCueEffects counts every drop"), StageCueRules::SanitizeCueEffects(Dirty), 6);
	TestTrue(TEXT("... A on fixture 1, stop B"), Dirty.Effects.Num() == 1 && Dirty.Effects[0].FixtureIds == TArray<int32>({ 1 }) && Dirty.EffectStops == TArray<FGuid>({ IdB }));
	TArray<FGuid> Many;
	for (int32 Index = 0; Index < StageEffectRules::MaxEffects + 3; ++Index)
	{
		Many.Add(FGuid::NewGuid());
	}
	TestEqual(TEXT("Stops past MaxEffects are dropped"), StageEffectRules::SanitizeStops(Many, {}), 3);
	TArray<FShowCue> Listed = { MakeCue(1.f, 0.f, {}, { IdA, IdA }) };
	StageCueRules::SanitizeCueList(Listed);
	TestEqual(TEXT("SanitizeCueList repairs cue effects too"), Listed[0].EffectStops.Num(), 1);

	// Feature timing: an effect fades with its feature's time, a stopping intensity effect with the Out time.
	FShowCue Timed = MakeCue(1.f, 2.f, {});
	Timed.Timing.Delay = 0.5f;
	Timed.Timing.FeatureTimes.Add(EStagePaletteKind::Position, 3.f);
	Timed.Timing.bHasOutTime = true;
	Timed.Timing.OutTime = 5.f;
	float Delay = 0.f;
	float Duration = 0.f;
	StageCueTiming::GetFeatureTiming(Timed, EStagePaletteKind::Position, false, Delay, Duration);
	TestTrue(TEXT("Position: its feature time and the delay"), Delay == 0.5f && Duration == 3.f);
	StageCueTiming::GetFeatureTiming(Timed, EStagePaletteKind::Intensity, true, Delay, Duration);
	TestEqual(TEXT("Intensity going down: the Out time"), Duration, 5.f);
	StageCueTiming::GetFeatureTiming(Timed, EStagePaletteKind::Color, true, Delay, Duration);
	TestEqual(TEXT("Colour: the In time"), Duration, 2.f);

	// Composition: one effect over another by its weight.
	StageShowMix::FEffectValue Into{ 1.f, false, 1.f };
	StageShowMix::ComposeEffect(Into, { 0.f, false, 0.25f });
	float Value = 0.4f;
	TestTrue(TEXT("Absolute: 1 then 0 at a quarter"), FMath::IsNearlyEqual(FMath::Lerp(FMath::Lerp(Value, 1.f, 1.f), 0.f, 0.25f), FMath::Lerp(Value, Into.Value, Into.Weight), Tolerance));
	Into = { 0.8f, false, 0.5f };
	StageShowMix::ComposeEffect(Into, { 0.2f, false, 0.5f });
	TestTrue(TEXT("Absolute halves: the same as applying both in turn"), FMath::IsNearlyEqual(FMath::Lerp(FMath::Lerp(Value, 0.8f, 0.5f), 0.2f, 0.5f), FMath::Lerp(Value, Into.Value, Into.Weight), Tolerance));
	Into = { 10.f, true, 1.f };
	StageShowMix::ComposeEffect(Into, { 30.f, true, 0.25f });
	TestEqual(TEXT("Relative: offsets blend"), Into.Value, 10.f * 0.75f + 30.f * 0.25f, Tolerance);
	StageShowMix::ComposeEffect(Into, { 5.f, true, 1.f });
	TestEqual(TEXT("At weight 1 the later wins"), Into.Value, 5.f);
	StageShowMix::ComposeEffect(Into, { 0.5f, false, 0.1f });
	TestTrue(TEXT("Relative and absolute do not mix: the later replaces"), !Into.bRelative && Into.Value == 0.5f);

	// The runner's fades without fixtures (weights and clocks only).
	FStageEffectRunner Runner;
	TArray<int32> Released;
	FShowCue Fade2 = MakeCue(1.f, 2.f, {});
	const FStageEffectRunner::FSourcedEntry SourcedA{ MakeEntry(IdA, Circle, { 1, 2 }), EStageEffectSource::Cue, INDEX_NONE, 1.f };
	TestTrue(TEXT("Start at once: changed"), Runner.SetEffects({ SourcedA }, &FindNone, nullptr));
	TestEqual(TEXT("... at full"), Runner.GetWeight(IdA), 1.f);
	TestFalse(TEXT("The same again: nothing changed"), Runner.SetEffects({ SourcedA }, &FindNone, nullptr));
	FStageEffectRunner::FSourcedEntry Scaled = SourcedA;
	Scaled.IntensityScale = 0.5f;
	TestFalse(TEXT("Only the scale: not a change"), Runner.SetEffects({ Scaled }, &FindNone, nullptr));
	TestTrue(TEXT("A cue stops it: fading out"), Runner.SetEffects({}, &FindNone, &Fade2) && Runner.IsRunning());
	Runner.Advance(1.f, Released);
	TestEqual(TEXT("Half way out"), Runner.GetWeight(IdA), 0.5f, Tolerance);
	TArray<FStageRunningEffect> Running;
	Runner.GetRunning(Running);
	TestTrue(TEXT("... listed as stopping"), Running.Num() == 1 && Running[0].bStopping);
	Runner.Advance(1.1f, Released);
	TestTrue(TEXT("Faded out: dropped, its fixtures released"), !Runner.IsRunning() && Released == TArray<int32>({ 1, 2 }));
	Runner.SetEffects({ SourcedA }, &FindNone, &Fade2);
	TestEqual(TEXT("A cue starts it: from 0"), Runner.GetWeight(IdA), 0.f);
	Runner.Advance(1.f, Released);
	Runner.SetEffects({ SourcedA }, &FindNone, nullptr);
	TestEqual(TEXT("An unrelated change keeps the fade"), Runner.GetWeight(IdA), 0.5f, Tolerance);
	Runner.SetEffects({}, &FindNone, &Fade2);
	Runner.SetEffects({ SourcedA }, &FindNone, &Fade2);
	TestEqual(TEXT("Stopped and back: up from where it was"), Runner.GetWeight(IdA), 0.5f, Tolerance);
	FStageEffectRunner::FSourcedEntry Changed = SourcedA;
	Changed.Entry.Effect = Bigger;
	Runner.SetEffects({ Changed }, &FindNone, nullptr);
	TestEqual(TEXT("A change at once snaps to full"), Runner.GetWeight(IdA), 1.f);
	FShowCue Delayed = MakeCue(1.f, 1.f, {});
	Delayed.Timing.Delay = 1.f;
	Delayed.Timing.Curve = EStageCueCurve::SCurve;
	Runner.SetEffects({}, &FindNone, nullptr);
	Runner.SetEffects({ SourcedA }, &FindNone, &Delayed);
	Runner.Advance(0.5f, Released);
	TestEqual(TEXT("Still in its delay"), Runner.GetWeight(IdA), 0.f);
	Runner.Advance(0.75f, Released);
	TestEqual(TEXT("S-curve a quarter through"), Runner.GetWeight(IdA), 0.15625f, Tolerance);
	// A morph blends the rate: 1 Hz to 3 Hz over 2 s.
	FStageEffectRunner::FSourcedEntry Faster = SourcedA;
	Faster.Entry.Effect.RateHz = 3.f;
	Runner.Advance(1.f, Released);
	TestEqual(TEXT("Faded in"), Runner.GetWeight(IdA), 1.f);
	const double Clock = Runner.GetCycles(IdA);
	Runner.SetEffects({ Faster }, &FindNone, &Fade2);
	TestEqual(TEXT("A morph keeps the weight"), Runner.GetWeight(IdA), 1.f);
	Runner.Advance(0.5f, Released);
	Runner.Advance(0.5f, Released);
	TestEqual(TEXT("The rate blends (0.5 s at 1 Hz, 0.5 s at 1.5 Hz)"), static_cast<float>(Runner.GetCycles(IdA) - Clock), 1.25f, Tolerance);
	TestTrue(TEXT("Zero time: changes at once"), Runner.SetEffects({}, &FindNone, &Only[0]) && !Runner.IsRunning());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageShowCueEffectsTest, "StageCraft.Show.CueEffects", StageCueEffectTests::Flags)

bool FStageShowCueEffectsTest::RunTest(const FString& Parameters)
{
	using namespace StageCueEffectTests;
	const FGameplayTag Dimmer = StageCraftTags::Attribute_Dimmer;
	const FGameplayTag Pan = StageCraftTags::Attribute_Pan;

	FScopedWorld Scoped;
	UShowControlSubsystem* Show = Scoped.World->GetSubsystem<UShowControlSubsystem>();
	UStageProgrammerSubsystem* Programmer = Scoped.World->GetSubsystem<UStageProgrammerSubsystem>();
	if (!TestNotNull(TEXT("Show control"), Show) || !TestNotNull(TEXT("Programmer"), Programmer))
	{
		return false;
	}
	ALightingFixtureActor* A = Scoped.SpawnMover();
	ALightingFixtureActor* B = Scoped.SpawnMover();
	ALightingFixtureActor* C = Scoped.SpawnMover();
	const TArray<int32> Ids = { A->GetFixtureId(), B->GetFixtureId(), C->GetFixtureId() };
	const float PanHome = A->GetAttribute(Pan);
	const float DimmerHome = A->GetAttribute(Dimmer);
	TArray<FStageRunningEffect> Storage;

	// A programmer effect stored in a cue (Track) runs on from the cue without a jump once the programmer is cleared.
	FStageEffect Circle = MakeEffect(EStageEffectTarget::Position, EStageEffectWaveform::Circle, 1.f, TEXT("Circle"));
	Circle.Size = 20.f;
	const FGuid CircleId = Programmer->AddEffect(Circle, Ids);
	Show->Tick(0.25f);
	const float PanStored = A->GetAttribute(Pan);
	TestTrue(TEXT("Track store with effects only"), Show->StoreCue(1.f, FText::FromString(TEXT("One")), 2.f, EStageCueStoreMode::Track));
	const FShowCue* One = Show->FindCue(1.f);
	TestTrue(TEXT("Cue 1 holds the effect under its id"), One && One->Effects.Num() == 1 && One->Effects[0].Id == CircleId && One->Fixtures.IsEmpty());
	TestTrue(TEXT("Restore cue 1"), Show->RestoreActiveCue(1.f));
	Programmer->SetEffectState(FStageProgrammerEffects());
	const FStageRunningEffect* Running = FindRunning(*Show, CircleId, Storage);
	TestTrue(TEXT("Now the cue runs it"), Running && Running->Source == EStageEffectSource::Cue);
	TestEqual(TEXT("... on the same clock"), static_cast<float>(Show->GetEffectCycles(CircleId)), 0.25f, 1e-4f);
	TestEqual(TEXT("... without a jump"), A->GetAttribute(Pan), PanStored, Tolerance);

	// A cue list: 1 starts the circle, 2 makes it bigger (morph), 3 stops it and starts a chase, 4 is blocked with an Out time.
	// A quarter hertz, so one second in the circle is at its widest pan.
	FStageEffect Slow = Circle;
	Slow.RateHz = 0.25f;
	FStageEffect Bigger = Slow;
	Bigger.Size = 40.f;
	FStageEffect Chase = MakeEffect(EStageEffectTarget::Intensity, EStageEffectWaveform::Square, 0.5f, TEXT("Chase"));
	Chase.Duty = 1.f; // Always on: its value is High (1) at every point, so the weight is all that moves.
	const FGuid ChaseId = FGuid::NewGuid();
	TArray<FShowCue> List;
	List.Add(MakeCue(1.f, 2.f, { MakeEntry(CircleId, Slow, Ids) }));
	List.Add(MakeCue(2.f, 2.f, { MakeEntry(CircleId, Bigger, Ids) }));
	List.Add(MakeCue(3.f, 1.f, { MakeEntry(ChaseId, Chase, Ids) }, { CircleId }));
	FShowCue Four = MakeCue(4.f, 0.f, {}, {}, /*bBlock*/ true);
	Four.Timing.bHasOutTime = true;
	Four.Timing.OutTime = 4.f;
	List.Add(Four);
	Show->SetCues(List);
	TestFalse(TEXT("A loaded list starts with no cue effects"), Show->AreEffectsLive());

	TestTrue(TEXT("Go to 1"), Show->GoToCue(1.f));
	TestEqual(TEXT("Fades in from 0"), Show->GetEffectWeight(CircleId), 0.f);
	TestEqual(TEXT("... the pan where it was"), A->GetAttribute(Pan), PanHome, Tolerance);
	Show->Tick(1.f);
	TestEqual(TEXT("Half way in"), Show->GetEffectWeight(CircleId), 0.5f, Tolerance);
	const float Cycles1 = static_cast<float>(Show->GetEffectCycles(CircleId));
	const StageEffects::FSample Sample1 = StageEffects::Evaluate(Slow, 0, Ids.Num(), Cycles1);
	TestEqual(TEXT("... the circle at half its size"), A->GetAttribute(Pan), PanHome + 0.5f * 20.f * Sample1.Pan, Tolerance);
	Show->Tick(1.1f);
	TestEqual(TEXT("In"), Show->GetEffectWeight(CircleId), 1.f);

	TestTrue(TEXT("Go to 2"), Show->GoToCue(2.f));
	Show->Tick(1.f);
	const float Cycles2 = static_cast<float>(Show->GetEffectCycles(CircleId));
	const StageEffects::FSample Sample2 = StageEffects::Evaluate(Slow, 0, Ids.Num(), Cycles2);
	TestEqual(TEXT("Morph half way: size 30, full weight"), A->GetAttribute(Pan), PanHome + 30.f * Sample2.Pan, Tolerance);
	TestEqual(TEXT("... the weight never dipped"), Show->GetEffectWeight(CircleId), 1.f);
	Show->Tick(1.1f);

	TestTrue(TEXT("Go to 3"), Show->GoToCue(3.f));
	Show->Tick(0.5f);
	TestEqual(TEXT("Circle half way out"), Show->GetEffectWeight(CircleId), 0.5f, Tolerance);
	TestEqual(TEXT("Chase half way in"), Show->GetEffectWeight(ChaseId), 0.5f, Tolerance);
	TestEqual(TEXT("... the dimmer half way to the chase"), A->GetAttribute(Dimmer), FMath::Lerp(DimmerHome, 1.f, 0.5f), Tolerance);
	Show->Tick(0.6f);
	TestTrue(TEXT("Circle gone"), !FindRunning(*Show, CircleId, Storage));
	TestEqual(TEXT("... pan back to its static look"), A->GetAttribute(Pan), PanHome, Tolerance);
	TestEqual(TEXT("Chase in"), A->GetAttribute(Dimmer), 1.f, Tolerance);

	TestTrue(TEXT("Go to 4 (blocked, Out 4 s)"), Show->GoToCue(4.f));
	Show->Tick(2.f);
	TestEqual(TEXT("An intensity effect stops over the Out time"), Show->GetEffectWeight(ChaseId), 0.5f, Tolerance);
	Show->Tick(2.1f);
	TestTrue(TEXT("Out: dimmer home, no tick"), FMath::IsNearlyEqual(A->GetAttribute(Dimmer), DimmerHome, Tolerance) && !Show->IsTickable());

	TestTrue(TEXT("Back to 3"), Show->Back());
	Show->Tick(0.5f);
	TestEqual(TEXT("Back fades with cue 3's time"), Show->GetEffectWeight(ChaseId), 0.5f, Tolerance);
	TestTrue(TEXT("Restore cue 2"), Show->RestoreActiveCue(2.f));
	TestTrue(TEXT("Restore: at once"), Show->GetEffectWeight(CircleId) == 1.f && !FindRunning(*Show, ChaseId, Storage));

	// The programmer over cue effects: a stop holds it off, Clear brings it back; taking it keeps its clock.
	TestTrue(TEXT("Stop the circle from the programmer"), Programmer->AddEffectStop(CircleId));
	TestFalse(TEXT("... a second time: nothing"), Programmer->AddEffectStop(CircleId));
	TestTrue(TEXT("... it stopped, pan static"), !FindRunning(*Show, CircleId, Storage) && FMath::IsNearlyEqual(A->GetAttribute(Pan), PanHome, Tolerance));
	TestTrue(TEXT("The show still knows it"), Show->FindShowEffect(CircleId) != nullptr);
	Programmer->SetEffectState(FStageProgrammerEffects());
	TestTrue(TEXT("Clear: back"), FindRunning(*Show, CircleId, Storage) != nullptr);
	Show->Tick(0.3f);
	const double ClockBeforeTake = Show->GetEffectCycles(CircleId);
	TestTrue(TEXT("Take it into the programmer"), Programmer->TakeEffect(*Show->FindShowEffect(CircleId)));
	Running = FindRunning(*Show, CircleId, Storage);
	TestTrue(TEXT("... the programmer runs it, same clock"), Running && Running->Source == EStageEffectSource::Programmer
		&& FMath::IsNearlyEqual(Show->GetEffectCycles(CircleId), ClockBeforeTake, 1e-6));
	FStageEffect Wider = Bigger;
	Wider.Size = 50.f;
	TestTrue(TEXT("Edit it live"), Programmer->UpdateEffect(CircleId, Wider));
	TestTrue(TEXT("Track store writes it back"), Show->StoreCue(2.f, FText::GetEmpty(), 2.f, EStageCueStoreMode::Track));
	TestTrue(TEXT("... cue 2 has the new size"), Show->FindCue(2.f)->Effects.Num() == 1 && Show->FindCue(2.f)->Effects[0].Effect.Size == 50.f);

	// A store of a stop (Track) records it; Cue Only stops a new effect at the next cue.
	Programmer->SetEffectState(FStageProgrammerEffects());
	Programmer->AddEffectStop(CircleId);
	TestTrue(TEXT("Store a stop"), Show->StoreCue(2.5f, FText::GetEmpty(), 0.f, EStageCueStoreMode::Track));
	TestTrue(TEXT("... cue 2.5 stops the circle"), Show->FindCue(2.5f)->EffectStops == TArray<FGuid>({ CircleId }));
	Programmer->SetEffectState(FStageProgrammerEffects());
	const FGuid NewId = Programmer->AddEffect(Chase, { Ids[0] });
	TestTrue(TEXT("Cue Only store"), Show->StoreCue(1.5f, FText::GetEmpty(), 0.f, EStageCueStoreMode::CueOnly));
	TestTrue(TEXT("... cue 1.5 starts it, cue 2 stops it"), Show->FindCue(1.5f)->Effects.Num() == 1 && Show->FindCue(2.f)->EffectStops.Contains(NewId));
	Programmer->SetEffectState(FStageProgrammerEffects());

	// Look stores every running effect; Block writes the inherited effects into the cue.
	TestTrue(TEXT("Restore cue 2"), Show->RestoreActiveCue(2.f));
	TestTrue(TEXT("Look store"), Show->StoreCue(10.f, FText::GetEmpty(), 0.f, EStageCueStoreMode::Look));
	TestTrue(TEXT("... holds the running circle"), Show->FindCue(10.f)->Effects.Num() == 1 && Show->FindCue(10.f)->Effects[0].Id == CircleId);
	TestTrue(TEXT("Block cue 3"), Show->SetCueBlocked(3.f, true));
	const FShowCue* Three = Show->FindCue(3.f);
	TestTrue(TEXT("... it holds the chase it ran and no stops"), Three && Three->bBlock && Three->EffectStops.IsEmpty() && IdsOf(Three->Effects) == TArray<FGuid>({ ChaseId }));

	// A playback: its effects run while it is up, intensity scaled by the fader.
	Show->SetCues({});
	FStageProgrammerEffects PlaybackEffects;
	const FGuid PlaybackId = FGuid::NewGuid();
	PlaybackEffects.Effects.Add(MakeEntry(PlaybackId, Chase, { Ids[1] }));
	TestTrue(TEXT("Record a playback with an effect only"), Show->RecordPlayback(0, {}, PlaybackEffects));
	TestFalse(TEXT("Nothing to record"), Show->RecordPlayback(1, {}, FStageProgrammerEffects()));
	TestFalse(TEXT("Down: not running"), FindRunning(*Show, PlaybackId, Storage) != nullptr);
	Show->SetPlaybackLevel(0, 0.5f);
	Running = FindRunning(*Show, PlaybackId, Storage);
	TestTrue(TEXT("Up: the playback runs it"), Running && Running->Source == EStageEffectSource::Playback && Running->Playback == 0);
	Show->Tick(0.1f);
	TestEqual(TEXT("... scaled by the fader"), B->GetAttribute(Dimmer), 0.5f, Tolerance);
	Show->SetPlaybackLevel(0, 1.f);
	Show->Tick(0.1f);
	TestEqual(TEXT("... at full"), B->GetAttribute(Dimmer), 1.f, Tolerance);
	TestTrue(TEXT("Release"), Show->ReleasePlayback(0));
	TestTrue(TEXT("... gone, dimmer home"), !FindRunning(*Show, PlaybackId, Storage) && FMath::IsNearlyEqual(B->GetAttribute(Dimmer), DimmerHome, Tolerance));
	Show->SetPlaybackFlash(0, true);
	TestTrue(TEXT("Flash runs it"), FindRunning(*Show, PlaybackId, Storage) != nullptr);
	Show->SetPlaybackFlash(0, false);
	TestFalse(TEXT("Flash off: gone"), FindRunning(*Show, PlaybackId, Storage) != nullptr);

	// External DMX: everything holds still.
	Show->SetPlaybackLevel(0, 1.f);
	Show->SetControlSource(EStageControlSource::External);
	TestFalse(TEXT("External: not live"), Show->AreEffectsLive() || Show->IsTickable());
	Show->SetControlSource(EStageControlSource::Internal);
	TestTrue(TEXT("Internal: live again"), Show->AreEffectsLive());
	Show->ReleasePlayback(0);
	TestFalse(TEXT("All stopped: no tick"), Show->IsTickable());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageSceneCueEffectDataTest, "StageCraft.Scene.CueEffectData", StageCueEffectTests::Flags)

bool FStageSceneCueEffectDataTest::RunTest(const FString& Parameters)
{
	using namespace StageCueEffectTests;
	FStageEffect Chase = MakeEffect(EStageEffectTarget::Intensity, EStageEffectWaveform::Square, 2.f, TEXT("Chase"));
	Chase.Spread = 360.f;
	const FGuid IdA = FGuid::NewGuid();
	const FGuid IdB = FGuid::NewGuid();
	const FGuid IdC = FGuid::NewGuid();

	FStageSceneDocument Document;
	Document.Name = TEXT("Cue effects");
	Document.Cues.Add(MakeCue(1.f, 2.f, { MakeEntry(IdA, Chase, { 1, 2, 3 }) }));
	Document.Cues.Add(MakeCue(2.f, 2.f, {}, { IdA }));
	FStagePlayback Playback;
	Playback.Name = FText::FromString(TEXT("FX"));
	Playback.Cues.Add(MakeCue(1.f, 0.f, { MakeEntry(IdB, Chase, { 4 }) }, { IdC }));
	Document.Playbacks.SetNum(StagePlaybackRules::Count);
	Document.Playbacks[2] = Playback;
	Document.ProgrammerEffectStops = { IdC };
	FStageSceneDocument Parsed;
	FStageSceneParseReport Report;
	TestTrue(TEXT("Round trip parses"), FStageSceneStore::Parse(FStageSceneStore::Serialize(Document), Parsed, Report) == EStageSceneResult::Success);
	TestTrue(TEXT("Nothing dropped"), Report.InvalidShowEntries == 0 && Report.InvalidCueEntries == 0);
	TestTrue(TEXT("Cue 1's effect back exactly"), Parsed.Cues.Num() == 2 && StageEffectRules::AreEntriesEqual(Parsed.Cues[0].Effects, Document.Cues[0].Effects));
	TestTrue(TEXT("Cue 2's stop back"), Parsed.Cues.Num() == 2 && Parsed.Cues[1].EffectStops == TArray<FGuid>({ IdA }));
	TestTrue(TEXT("The playback's effect and stop back"), Parsed.Playbacks.IsValidIndex(2) && Parsed.Playbacks[2].Cues.Num() == 1
		&& StageEffectRules::AreEntriesEqual(Parsed.Playbacks[2].Cues[0].Effects, Playback.Cues[0].Effects) && Parsed.Playbacks[2].Cues[0].EffectStops == TArray<FGuid>({ IdC }));
	TestTrue(TEXT("The programmer's stop back"), Parsed.ProgrammerEffectStops == TArray<FGuid>({ IdC }));

	// Hostile v10 cue effects and stops: dropped and counted, the rest loads.
	const FString A = IdA.ToString(EGuidFormats::DigitsWithHyphens);
	const FString B = IdB.ToString(EGuidFormats::DigitsWithHyphens);
	const FString Hostile = FString::Printf(TEXT("{\"format\":\"StageCraftScene\",\"version\":%d,\"name\":\"H\",\"items\":[],\"cues\":[")
		TEXT("{\"number\":1,\"fade\":1,\"fixtures\":[],\"effects\":[{\"id\":\"%s\",\"fixtures\":[1]},{\"id\":\"bad\",\"fixtures\":[1]},3],")
		TEXT("\"effectStops\":[\"%s\",\"%s\",\"%s\",\"nope\",5]}],")
		TEXT("\"programmerEffectStops\":[\"%s\",\"%s\",false]}"),
		FStageSceneStore::FormatVersion, *A, *A, *B, *B, *B, *B);
	TestTrue(TEXT("A hostile file still parses"), FStageSceneStore::Parse(Hostile, Parsed = FStageSceneDocument(), Report = FStageSceneParseReport()) == EStageSceneResult::Success);
	TestTrue(TEXT("Cue 1 keeps effect A and stop B"), Parsed.Cues.Num() == 1 && IdsOf(Parsed.Cues[0].Effects) == TArray<FGuid>({ IdA })
		&& Parsed.Cues[0].EffectStops == TArray<FGuid>({ IdB }));
	TestEqual(TEXT("Cue drops counted (2 effects, a stop of its own effect, a duplicate, 2 unreadable)"), Report.InvalidCueEntries, 6);
	TestTrue(TEXT("The programmer keeps stop B"), Parsed.ProgrammerEffectStops == TArray<FGuid>({ IdB }));
	TestEqual(TEXT("Programmer drops counted"), Report.InvalidShowEntries, 2);

	const FString V9 = FString::Printf(TEXT("{\"format\":\"StageCraftScene\",\"version\":9,\"name\":\"Old\",\"items\":[],\"cues\":[{\"number\":1,\"fixtures\":[],")
		TEXT("\"effects\":[{\"id\":\"%s\",\"fixtures\":[1]}],\"effectStops\":[\"%s\"]}],\"programmerEffectStops\":[\"%s\"]}"), *A, *B, *B);
	TestTrue(TEXT("Version 9 parses"), FStageSceneStore::Parse(V9, Parsed = FStageSceneDocument(), Report = FStageSceneParseReport()) == EStageSceneResult::Success);
	TestTrue(TEXT("Version 9: no cue effects or stops, nothing read"), Parsed.Cues.Num() == 1 && Parsed.Cues[0].Effects.IsEmpty() && Parsed.Cues[0].EffectStops.IsEmpty()
		&& Parsed.ProgrammerEffectStops.IsEmpty() && Report.InvalidCueEntries == 0 && Report.InvalidShowEntries == 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
