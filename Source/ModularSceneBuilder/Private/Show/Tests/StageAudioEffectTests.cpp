// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for audio in the effects engine (Docs/ADR/0029-audio-analysis-and-audio-reactive-effects.md §4, §9): the pure maths and the
// runner's Sync, Modulate and Trigger from recorded frame sequences (exact values, fallbacks, Off unchanged), the rules, the show
// reading the music only while an audio effect runs and following the test signal on real fixtures, External holding, and the scene
// format v11 "audio" object. Nothing is recorded: the music is the generated test signal, and the user's settings are never touched.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Show.Audio; Automation RunTests StageCraft.Scene.EffectAudioData; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Actors/LightingFixtureActor.h"
#include "Async/TaskGraphInterfaces.h"
#include "Data/LightingFixtureData.h"
#include "Data/StageParameterTypes.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Scene/StageSceneStore.h"
#include "Settings/StageCraftUserSettings.h"
#include "Show/StageEffectRunner.h"
#include "Show/StageEffects.h"
#include "Subsystems/ShowControlSubsystem.h"
#include "Subsystems/StageAudioReactiveSubsystem.h"
#include "Subsystems/StageProgrammerSubsystem.h"

namespace StageAudioEffectTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;
	constexpr float Tolerance = 1e-4f;
	constexpr float Frame = 1.f / 60.f;

	ALightingFixtureActor* FindNone(int32)
	{
		return nullptr;
	}

	FStageEffect MakeEffect(EStageEffectTarget Target, EStageEffectWaveform Waveform, float RateHz, EStageEffectAudioMode Mode)
	{
		FStageEffect Effect;
		Effect.Name = FText::FromString(TEXT("Test"));
		Effect.Target = Target;
		Effect.Waveform = Waveform;
		Effect.RateHz = RateHz;
		Effect.Spread = 360.f;
		Effect.Audio.Mode = Mode;
		return Effect;
	}

	FStageEffectRunner::FSourcedEntry MakeSourced(const FGuid& Id, const FStageEffect& Effect, TArray<int32> FixtureIds)
	{
		FStageEffectRunner::FSourcedEntry Sourced;
		Sourced.Entry.Id = Id;
		Sourced.Entry.Effect = Effect;
		Sourced.Entry.FixtureIds = MoveTemp(FixtureIds);
		Sourced.Source = EStageEffectSource::Programmer;
		return Sourced;
	}

	/** One recorded analyser frame. */
	StageEffects::FAudioInput MakeInput(double Beats, float BPM, float Confidence = 1.f, bool bSignal = true, float Band = 1.f, int64 Onsets = 0)
	{
		StageEffects::FAudioInput Input;
		for (float& Value : Input.Bands)
		{
			Value = Band;
		}
		Input.Beats = Beats;
		Input.BPM = BPM;
		Input.Confidence = Confidence;
		Input.bSignal = bSignal;
		Input.OnsetCount = Onsets;
		return Input;
	}

	/** Runs Advance with the main loop's other work: the task graph (source results) and the engine's audio devices. */
	void PumpFrame(UShowControlSubsystem* Show, float DeltaSeconds)
	{
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		if (Show)
		{
			Show->Tick(DeltaSeconds);
		}
	}

	bool PumpUntil(double Seconds, TFunctionRef<bool()> Done)
	{
		const double End = FPlatformTime::Seconds() + Seconds;
		while (FPlatformTime::Seconds() < End)
		{
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			if (Done())
			{
				return true;
			}
			FPlatformProcess::Sleep(0.01f);
		}
		return Done();
	}

	struct FScopedWorld
	{
		UWorld* World = nullptr;

		FScopedWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("StageAudioEffectTestWorld"));
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageShowAudioEffectsTest, "StageCraft.Show.AudioEffects", StageAudioEffectTests::Flags)

bool FStageShowAudioEffectsTest::RunTest(const FString& Parameters)
{
	using namespace StageAudioEffectTests;

	// --- Rules ---
	FStageEffect Wild = MakeEffect(EStageEffectTarget::Intensity, EStageEffectWaveform::Sine, 1.f, static_cast<EStageEffectAudioMode>(77));
	Wild.Audio.Band = static_cast<EStageAudioBand>(9);
	Wild.Audio.Amount = std::numeric_limits<float>::quiet_NaN();
	Wild.Audio.Modulates = static_cast<EStageEffectAudioTarget>(5);
	Wild.Audio.BeatsPerCycle = static_cast<EStageEffectBeatDivision>(40);
	Wild.Audio.Response = static_cast<EStageEffectAudioResponse>(8);
	const FStageEffectAudio Safe = StageEffectRules::SanitizeEffect(Wild).Audio;
	TestTrue(TEXT("Unknown audio settings fall back to the defaults"), Safe.Equals(FStageEffectAudio()));
	Wild.Audio = FStageEffectAudio();
	Wild.Audio.Amount = -2.f;
	TestEqual(TEXT("Amount clamped"), StageEffectRules::SanitizeEffect(Wild).Audio.Amount, 0.f);
	FStageEffect Synced = Wild;
	Synced.Audio.Mode = EStageEffectAudioMode::Sync;
	TestFalse(TEXT("Equals sees the audio settings"), Synced.Equals(Wild));
	TestTrue(TEXT("Sync and Trigger take the clock; Modulate does not"), StageEffectRules::IsClockFromAudio(Synced.Audio)
		&& !StageEffectRules::IsClockFromAudio(FStageEffectAudio()));
	const TArray<FStageEffect> Quick = StageEffectRules::MakeAudioQuickEffects();
	TestEqual(TEXT("Six audio quick effects"), Quick.Num(), 6);
	for (const FStageEffect& Effect : Quick)
	{
		TestTrue(*FString::Printf(TEXT("%s follows the music and is valid"), *Effect.Name.ToString()),
			Effect.UsesAudio() && StageEffectRules::SanitizeEffect(Effect).Equals(Effect));
	}
	TestEqual(TEXT("Quarter"), StageEffectRules::GetBeatsPerCycle(EStageEffectBeatDivision::Quarter), 0.25f);
	TestEqual(TEXT("Eight"), StageEffectRules::GetBeatsPerCycle(EStageEffectBeatDivision::Eight), 8.f);

	// --- Pure maths ---
	FStageEffect Level = MakeEffect(EStageEffectTarget::Intensity, EStageEffectWaveform::Sine, 1.f, EStageEffectAudioMode::Modulate);
	Level.Low = 0.2f;
	Level.Audio.Amount = 0.5f;
	FStageEffect Modulated = Level;
	StageEffects::ApplyModulation(Modulated, 0.4f);
	TestEqual(TEXT("Modulate Level: High = Low + (High - Low) x (1 - A + A x band)"), Modulated.High, 0.2f + 0.8f * 0.7f, Tolerance);
	TestEqual(TEXT("... Low unchanged"), Modulated.Low, 0.2f);
	FStageEffect Sized = Level;
	Sized.Audio.Modulates = EStageEffectAudioTarget::Size;
	StageEffects::ApplyModulation(Sized, 0.4f);
	TestEqual(TEXT("Modulate Size"), Sized.Size, 30.f * 0.7f, Tolerance);
	FStageEffect Rated = Level;
	Rated.Audio.Modulates = EStageEffectAudioTarget::Rate;
	TestEqual(TEXT("Modulate Rate: RateHz x (1 + A x band)"), StageEffects::GetModulatedRate(Rated, 2.f, 0.4f), 2.4f, Tolerance);
	FStageEffect Plain = Level;
	Plain.Audio.Mode = EStageEffectAudioMode::Off;
	FStageEffect PlainCopy = Plain;
	StageEffects::ApplyModulation(PlainCopy, 0.f);
	TestTrue(TEXT("Off: nothing modulated"), PlainCopy.Equals(Plain) && StageEffects::GetModulatedRate(Plain, 2.f, 0.f) == 2.f);
	TestEqual(TEXT("No signal reads the band as 1"), StageEffects::GetModulationBand(Level.Audio, MakeInput(0.0, 0.f, 0.f, false, 0.f)), 1.f);
	TestEqual(TEXT("A signal reads the band"), StageEffects::GetModulationBand(Level.Audio, MakeInput(0.0, 0.f, 0.f, true, 0.3f)), 0.3f);
	TestEqual(TEXT("Smoothing: a rise is at once"), StageEffects::SmoothBand(0.2f, 0.9f, Frame, EStageEffectAudioResponse::Slow), 0.9f);
	TestEqual(TEXT("Smoothing: Fast falls by e after 30 ms"), StageEffects::SmoothBand(1.f, 0.f, 0.03f, EStageEffectAudioResponse::Fast), FMath::Exp(-1.f), Tolerance);
	TestEqual(TEXT("Smoothing: Slow falls by e after 400 ms"), StageEffects::SmoothBand(1.f, 0.f, 0.4f, EStageEffectAudioResponse::Slow), FMath::Exp(-1.f), Tolerance);
	TestEqual(TEXT("Cycle error the short way forward"), StageEffects::GetCycleError(0.95, 2.05), 0.1, 1e-9);
	TestEqual(TEXT("Cycle error the short way back"), StageEffects::GetCycleError(0.05, 0.95), -0.1, 1e-9);
	FStageEffectAudio Beats;
	TestEqual(TEXT("Trigger events: beats"), StageEffects::GetTriggerEvents(Beats, MakeInput(3.7, 120.f)), int64(3));
	Beats.BeatsPerCycle = EStageEffectBeatDivision::Quarter;
	TestEqual(TEXT("... quarters"), StageEffects::GetTriggerEvents(Beats, MakeInput(3.7, 120.f)), int64(14));
	Beats.BeatsPerCycle = EStageEffectBeatDivision::Eight;
	TestEqual(TEXT("... every eight beats"), StageEffects::GetTriggerEvents(Beats, MakeInput(17.0, 120.f)), int64(2));
	Beats.Band = EStageAudioBand::Level;
	TestEqual(TEXT("... onsets with the Level band"), StageEffects::GetTriggerEvents(Beats, MakeInput(17.0, 120.f, 1.f, true, 1.f, 42)), int64(42));
	FStageEffect Chase = MakeEffect(EStageEffectTarget::Intensity, EStageEffectWaveform::Square, 1.f, EStageEffectAudioMode::Trigger);
	TestEqual(TEXT("Trigger step: one fixture of eight"), StageEffects::GetTriggerStep(Chase, 8), 0.125, 1e-9);
	Chase.Wings = 2;
	TestEqual(TEXT("... of four steps with two wings"), StageEffects::GetTriggerStep(Chase, 8), 0.25, 1e-9);
	Chase.Waveform = EStageEffectWaveform::Random;
	TestEqual(TEXT("... a whole cycle for Random (a new level each time)"), StageEffects::GetTriggerStep(Chase, 8), 1.0, 1e-9);
	FStageAudioFeatures Features;
	Features.BeatCount = 10;
	Features.BeatPhase = 0.75f;
	Features.BPM = 120.f;
	Features.Timestamp = 100.0;
	Features.bSignal = true;
	Features.Bass = 0.6f;
	const StageEffects::FAudioInput FromFeatures = StageEffects::FAudioInput::FromFeatures(Features, 100.25);
	TestEqual(TEXT("Beats carried forward past the beat (10.75 + 0.5)"), FromFeatures.Beats, 11.25, 1e-9);
	TestEqual(TEXT("Bands copied"), FromFeatures.GetBand(EStageAudioBand::Bass), 0.6f);
	TestEqual(TEXT("Carried at most a second"), StageEffects::FAudioInput::FromFeatures(Features, 200.0).Beats, 12.75, 1e-9);

	// --- The runner: Off is unchanged by any music ---
	const TArray<int32> Four = { 1, 2, 3, 4 };
	FStageEffectRunner WithMusic;
	FStageEffectRunner Without;
	const FGuid OffId = FGuid::NewGuid();
	FStageEffect Off = MakeEffect(EStageEffectTarget::Position, EStageEffectWaveform::Circle, 0.37f, EStageEffectAudioMode::Off);
	WithMusic.SetEffects({ MakeSourced(OffId, Off, Four) }, &FindNone, nullptr);
	Without.SetEffects({ MakeSourced(OffId, Off, Four) }, &FindNone, nullptr);
	TestFalse(TEXT("Off: the show need not read the music"), WithMusic.NeedsAudio());
	TArray<int32> Released;
	for (int32 Index = 0; Index < 200; ++Index)
	{
		const StageEffects::FAudioInput Input = MakeInput(Index * 0.03, 120.f, 0.9f, true, 0.1f * (Index % 10), Index);
		WithMusic.Advance(Frame, Released, &Input);
		Without.Advance(Frame, Released);
	}
	TestTrue(TEXT("Off: the same clock to the bit with or without music"), WithMusic.GetCycles(OffId) == Without.GetCycles(OffId));

	// --- Sync ---
	const FGuid SyncId = FGuid::NewGuid();
	FStageEffect Sync = MakeEffect(EStageEffectTarget::Position, EStageEffectWaveform::Sine, 0.5f, EStageEffectAudioMode::Sync);
	FStageEffectRunner Runner;
	Runner.SetEffects({ MakeSourced(SyncId, Sync, Four) }, &FindNone, nullptr);
	TestTrue(TEXT("Sync: the show reads the music"), Runner.NeedsAudio());
	double Beat = 10.3;
	double Previous = Runner.GetCycles(SyncId);
	double LargestPull = 0.0;
	for (int32 Index = 0; Index < 60; ++Index)
	{
		Beat += 2.0 * Frame;
		const StageEffects::FAudioInput Input = MakeInput(Beat, 120.f);
		Runner.Advance(Frame, Released, &Input);
		const double Moved = Runner.GetCycles(SyncId) - Previous;
		LargestPull = FMath::Max(LargestPull, FMath::Abs(Moved - 2.0 * Frame));
		Previous = Runner.GetCycles(SyncId);
	}
	TestTrue(TEXT("Sync: never pulled more than 5 % of a cycle a frame"), LargestPull <= StageEffects::SyncMaxPullCycles + 1e-9);
	TestEqual(TEXT("Sync: locked to the beat within a second"), StageEffects::GetCycleError(Runner.GetCycles(SyncId), Beat), 0.0, 1e-6);
	// 120 -> 128: the clock bends to the new tempo instead of jumping.
	LargestPull = 0.0;
	for (int32 Index = 0; Index < 120; ++Index)
	{
		Beat += 128.0 / 60.0 * Frame;
		const StageEffects::FAudioInput Input = MakeInput(Beat, 128.f);
		Runner.Advance(Frame, Released, &Input);
		const double Moved = Runner.GetCycles(SyncId) - Previous;
		LargestPull = FMath::Max(LargestPull, FMath::Abs(Moved - 128.0 / 60.0 * Frame));
		Previous = Runner.GetCycles(SyncId);
	}
	TestTrue(TEXT("Tempo change: no jump"), LargestPull <= StageEffects::SyncMaxPullCycles + 1e-9);
	TestEqual(TEXT("Tempo change: locked again"), StageEffects::GetCycleError(Runner.GetCycles(SyncId), Beat), 0.0, 1e-6);
	// Low confidence: carries on at the last good tempo, not pulled.
	const StageEffects::FAudioInput Unsure = MakeInput(Beat + 0.4, 90.f, 0.2f);
	Runner.Advance(0.5f, Released, &Unsure);
	TestEqual(TEXT("Low confidence: the last good tempo"), Runner.GetCycles(SyncId) - Previous, 128.0 / 60.0 * 0.5, 1e-6);
	Previous = Runner.GetCycles(SyncId);
	Runner.Advance(0.5f, Released);
	TestEqual(TEXT("No music at all: the last good tempo"), Runner.GetCycles(SyncId) - Previous, 128.0 / 60.0 * 0.5, 1e-6);
	FStageEffectRunner Never;
	Never.SetEffects({ MakeSourced(SyncId, Sync, Four) }, &FindNone, nullptr);
	Never.Advance(1.f, Released);
	TestEqual(TEXT("Never had a tempo: its own rate"), Never.GetCycles(SyncId), 0.5, 1e-9);
	FStageEffect Bar = Sync;
	Bar.Audio.BeatsPerCycle = EStageEffectBeatDivision::Four;
	FStageEffectRunner BarRunner;
	BarRunner.SetEffects({ MakeSourced(SyncId, Bar, Four) }, &FindNone, nullptr);
	const StageEffects::FAudioInput OnBar = MakeInput(0.0, 120.f, 1.f);
	BarRunner.Advance(Frame, Released, &OnBar);
	const double BarStart = BarRunner.GetCycles(SyncId);
	const StageEffects::FAudioInput Unsure2 = MakeInput(0.0, 120.f, 0.f);
	BarRunner.Advance(1.f, Released, &Unsure2);
	TestEqual(TEXT("Four beats a cycle: 120 BPM is half a cycle a second"), BarRunner.GetCycles(SyncId) - BarStart, 0.5, 1e-6);

	// --- Trigger ---
	const FGuid TriggerId = FGuid::NewGuid();
	FStageEffect Step = MakeEffect(EStageEffectTarget::Intensity, EStageEffectWaveform::Square, 3.f, EStageEffectAudioMode::Trigger);
	FStageEffectRunner Stepper;
	Stepper.SetEffects({ MakeSourced(TriggerId, Step, Four) }, &FindNone, nullptr);
	const auto StepAt = [&](const StageEffects::FAudioInput& Input) { Stepper.Advance(Frame, Released, &Input); return Stepper.GetCycles(TriggerId); };
	TestEqual(TEXT("Trigger: the first frame primes, no step"), StepAt(MakeInput(5.2, 120.f)), 0.0, 1e-9);
	TestEqual(TEXT("... no beat yet: holds (its own rate is unused)"), StepAt(MakeInput(5.9, 120.f)), 0.0, 1e-9);
	TestEqual(TEXT("... a beat: one fixture on"), StepAt(MakeInput(6.1, 120.f)), 0.25, 1e-9);
	TestEqual(TEXT("... a jump of fourteen beats: four steps at most"), StepAt(MakeInput(20.0, 120.f)), 1.25, 1e-9);
	TestEqual(TEXT("... no tempo: holds"), StepAt(MakeInput(21.0, 120.f, 0.1f)), 1.25, 1e-9);
	TestEqual(TEXT("... back: primes again, no catching up"), StepAt(MakeInput(30.0, 120.f)), 1.25, 1e-9);
	TestEqual(TEXT("... and steps on the next beat"), StepAt(MakeInput(31.0, 120.f)), 1.5, 1e-9);
	TestEqual(TEXT("... a source restart (count went back) primes"), StepAt(MakeInput(0.5, 120.f)), 1.5, 1e-9);
	FStageEffect OnOnsets = Step;
	OnOnsets.Audio.Band = EStageAudioBand::Level;
	OnOnsets.Waveform = EStageEffectWaveform::Random;
	FStageEffectRunner Hits;
	Hits.SetEffects({ MakeSourced(TriggerId, OnOnsets, Four) }, &FindNone, nullptr);
	const auto HitAt = [&](const StageEffects::FAudioInput& Input) { Hits.Advance(Frame, Released, &Input); return Hits.GetCycles(TriggerId); };
	HitAt(MakeInput(0.0, 0.f, 0.f, true, 1.f, 3));
	TestEqual(TEXT("Onsets need no tempo: a hit is a new random level"), HitAt(MakeInput(0.0, 0.f, 0.f, true, 1.f, 4)), 1.0, 1e-9);
	TestEqual(TEXT("... no signal: holds"), HitAt(MakeInput(0.0, 0.f, 0.f, false, 1.f, 9)), 1.0, 1e-9);

	// --- Modulate Rate (Level and Size are checked on fixtures below) ---
	const FGuid RateId = FGuid::NewGuid();
	FStageEffect Speed = MakeEffect(EStageEffectTarget::Position, EStageEffectWaveform::Sine, 1.f, EStageEffectAudioMode::Modulate);
	Speed.Audio.Modulates = EStageEffectAudioTarget::Rate;
	FStageEffectRunner Speeder;
	Speeder.SetEffects({ MakeSourced(RateId, Speed, Four) }, &FindNone, nullptr);
	const StageEffects::FAudioInput Loud = MakeInput(0.0, 0.f, 0.f, true, 1.f);
	Speeder.Advance(0.5f, Released, &Loud);
	TestEqual(TEXT("Modulate Rate at full band: twice as fast"), Speeder.GetCycles(RateId), 1.0, 1e-6);
	Speeder.Advance(0.5f, Released);
	TestEqual(TEXT("... no music reads 1: still twice"), Speeder.GetCycles(RateId), 2.0, 1e-6);
	Speeder.SetEffects({}, &FindNone, nullptr);
	TestFalse(TEXT("Stopped: the show stops reading the music"), Speeder.NeedsAudio());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageShowAudioOnFixturesTest, "StageCraft.Show.AudioOnFixtures", StageAudioEffectTests::Flags)

bool FStageShowAudioOnFixturesTest::RunTest(const FString& Parameters)
{
	using namespace StageAudioEffectTests;
	const FGameplayTag Dimmer = StageCraftTags::Attribute_Dimmer;

	FScopedWorld Scoped;
	UShowControlSubsystem* Show = Scoped.World->GetSubsystem<UShowControlSubsystem>();
	UStageProgrammerSubsystem* Programmer = Scoped.World->GetSubsystem<UStageProgrammerSubsystem>();
	UStageAudioReactiveSubsystem* Audio = Scoped.World->GetSubsystem<UStageAudioReactiveSubsystem>();
	if (!TestNotNull(TEXT("Show control"), Show) || !TestNotNull(TEXT("Programmer"), Programmer) || !TestNotNull(TEXT("Audio"), Audio))
	{
		return false;
	}
	// The user's settings are never touched; whatever this PC remembered was opened at BeginPlay, so start from Off.
	UStageCraftUserSettings* Settings = NewObject<UStageCraftUserSettings>();
	Settings->SetToDefaults();
	Audio->UseSettingsForTests(Settings);
	FText Error;
	Audio->SetSource(EStageAudioSource::Off, Error);

	ALightingFixtureActor* A = Scoped.SpawnMover();
	ALightingFixtureActor* B = Scoped.SpawnMover();
	const TArray<int32> Both = { A->GetFixtureId(), B->GetFixtureId() };

	// Only an effect that follows the music makes the show read it.
	const FGuid ChaseId = Programmer->AddEffect(MakeEffect(EStageEffectTarget::Intensity, EStageEffectWaveform::Square, 1.f, EStageEffectAudioMode::Off), Both);
	const int64 ReadsBefore = Show->GetAudioReadsForTests();
	Show->Tick(Frame);
	Show->Tick(Frame);
	TestEqual(TEXT("No audio effect: the music is never read"), Show->GetAudioReadsForTests(), ReadsBefore);
	Programmer->SetEffects({});

	// Bass Pulse on B with no music: the band reads 1, so B is fully lit (a show built on audio stays lit when the music stops).
	const TArray<FStageEffect> Quick = StageEffectRules::MakeAudioQuickEffects();
	const FGuid PulseId = Programmer->AddEffect(Quick[0], { B->GetFixtureId() });
	Show->Tick(Frame);
	TestEqual(TEXT("An audio effect: read once per tick"), Show->GetAudioReadsForTests(), ReadsBefore + 1);
	TestEqual(TEXT("No music: Bass Pulse at full"), B->GetAttribute(Dimmer), 1.f, 1e-3f);

	// A synced swing on A while the test signal plays at 128 BPM: the clock follows the tempo, and the pulse follows the kick.
	FStageEffect Swing = Quick[4];
	Swing.Audio.BeatsPerCycle = EStageEffectBeatDivision::One;
	const FGuid SwingId = Programmer->AddEffect(Swing, { A->GetFixtureId() });
	TestTrue(TEXT("TestSignal starts"), Audio->SetSource(EStageAudioSource::TestSignal, Error));
	double Last = FPlatformTime::Seconds();
	const auto TickReal = [&]()
	{
		const double Now = FPlatformTime::Seconds();
		PumpFrame(Show, static_cast<float>(Now - Last));
		Last = Now;
	};
	const bool bTempo = PumpUntil(9.0, [&]() { TickReal(); const FStageAudioFeatures F = Audio->GetFeatures(); return F.bSignal && F.Confidence >= 0.5f && FMath::Abs(F.BPM - 128.f) <= 1.f; });
	TestTrue(TEXT("The analyser found 128 BPM"), bTempo);
	PumpUntil(1.0, [&]() { TickReal(); return false; }); // the pull settles
	const double Start = FPlatformTime::Seconds();
	const double CyclesStart = Show->GetEffectCycles(SwingId);
	double Unwrapped = 0.0;
	double PreviousCycles = CyclesStart;
	float LowestPulse = 1.f;
	float HighestPulse = 0.f;
	PumpUntil(3.0, [&]()
	{
		TickReal();
		const double Cycles = Show->GetEffectCycles(SwingId);
		Unwrapped += Cycles - PreviousCycles;
		PreviousCycles = Cycles;
		LowestPulse = FMath::Min(LowestPulse, B->GetAttribute(Dimmer));
		HighestPulse = FMath::Max(HighestPulse, B->GetAttribute(Dimmer));
		return false;
	});
	const double MeasuredBPM = Unwrapped / (FPlatformTime::Seconds() - Start) * 60.0;
	AddInfo(FString::Printf(TEXT("Synced clock: %.2f BPM; Bass Pulse %.2f .. %.2f"), MeasuredBPM, LowestPulse, HighestPulse));
	TestTrue(TEXT("Sync: the clock runs at the music's tempo"), FMath::Abs(MeasuredBPM - 128.0) <= 2.0);
	TestTrue(TEXT("Modulate: the pulse follows the kick"), HighestPulse >= 0.6f && LowestPulse <= 0.9f * HighestPulse);

	// External DMX: effects hold, the music is not read.
	Show->SetControlSource(EStageControlSource::External);
	const double Held = Show->GetEffectCycles(SwingId);
	const int64 ReadsHeld = Show->GetAudioReadsForTests();
	Show->Tick(0.5f);
	TestTrue(TEXT("External: holds and does not read"), Show->GetEffectCycles(SwingId) == Held && Show->GetAudioReadsForTests() == ReadsHeld);
	Show->SetControlSource(EStageControlSource::Internal);

	Programmer->SetEffects({});
	TestFalse(TEXT("Stopped: no tick"), Show->IsTickable());
	TestTrue(TEXT("Off"), Audio->SetSource(EStageAudioSource::Off, Error));
	Audio->UseSettingsForTests(nullptr);
	TestTrue(TEXT("Pulse id was valid"), PulseId.IsValid() && ChaseId.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageSceneEffectAudioDataTest, "StageCraft.Scene.EffectAudioData", StageAudioEffectTests::Flags)

bool FStageSceneEffectAudioDataTest::RunTest(const FString& Parameters)
{
	using namespace StageAudioEffectTests;
	FStageEffect Synced = MakeEffect(EStageEffectTarget::Position, EStageEffectWaveform::Circle, 0.3f, EStageEffectAudioMode::Sync);
	Synced.Audio.BeatsPerCycle = EStageEffectBeatDivision::Quarter;
	FStageEffect Pulse = MakeEffect(EStageEffectTarget::Intensity, EStageEffectWaveform::Cosine, 0.f, EStageEffectAudioMode::Modulate);
	Pulse.Audio.Band = EStageAudioBand::SubBass;
	Pulse.Audio.Amount = 0.35f;
	Pulse.Audio.Modulates = EStageEffectAudioTarget::Rate;
	Pulse.Audio.Response = EStageEffectAudioResponse::Slow;
	const FStageEffect Plain = MakeEffect(EStageEffectTarget::Color, EStageEffectWaveform::Sine, 0.2f, EStageEffectAudioMode::Off);

	FStageSceneDocument Document;
	Document.Name = TEXT("Audio");
	Document.ProgrammerEffects.Add(MakeSourced(FGuid::NewGuid(), Synced, { 1, 2 }).Entry);
	Document.ProgrammerEffects.Add(MakeSourced(FGuid::NewGuid(), Pulse, { 3 }).Entry);
	Document.ProgrammerEffects.Add(MakeSourced(FGuid::NewGuid(), Plain, { 4 }).Entry);
	const FString Saved = FStageSceneStore::Serialize(Document);
	TestTrue(TEXT("Version 11"), Saved.Contains(TEXT("\"version\": 11")));
	int32 AudioObjects = 0;
	for (int32 At = Saved.Find(TEXT("\"audio\""), ESearchCase::CaseSensitive); At != INDEX_NONE; At = Saved.Find(TEXT("\"audio\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, At + 1))
	{
		++AudioObjects;
	}
	TestEqual(TEXT("Only the two audio effects carry \"audio\""), AudioObjects, 2);
	TestTrue(TEXT("Enums written by name"), Saved.Contains(TEXT("\"beatsPerCycle\": \"1/4\"")) && Saved.Contains(TEXT("\"band\": \"SubBass\"")));
	FStageSceneDocument Parsed;
	FStageSceneParseReport Report;
	TestTrue(TEXT("Round trip parses"), FStageSceneStore::Parse(Saved, Parsed, Report) == EStageSceneResult::Success);
	TestEqual(TEXT("Nothing dropped"), Report.InvalidShowEntries, 0);
	TestTrue(TEXT("Every audio setting back, exactly"), UStageProgrammerSubsystem::AreEffectsEqual(Parsed.ProgrammerEffects, Document.ProgrammerEffects));

	// Hostile audio: unknown names and bad numbers fall back, each counted; a wrong type is counted; ranges are clamped.
	const FString Id1 = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
	const FString Id2 = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
	const FString Id3 = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
	const FString Hostile = FString::Printf(TEXT("{\"format\":\"StageCraftScene\",\"version\":%d,\"name\":\"H\",\"items\":[],\"programmerEffects\":[")
		TEXT("{\"id\":\"%s\",\"fixtures\":[1],\"audio\":{\"mode\":\"Dance\",\"band\":\"Treble\",\"amount\":\"x\",\"beatsPerCycle\":\"3\",\"response\":7}},")
		TEXT("{\"id\":\"%s\",\"fixtures\":[2],\"audio\":[1,2]},")
		TEXT("{\"id\":\"%s\",\"fixtures\":[3],\"audio\":{\"mode\":\"Trigger\",\"amount\":7}}]}"),
		FStageSceneStore::FormatVersion, *Id1, *Id2, *Id3);
	TestTrue(TEXT("A hostile file still parses"), FStageSceneStore::Parse(Hostile, Parsed = FStageSceneDocument(), Report = FStageSceneParseReport()) == EStageSceneResult::Success);
	if (TestEqual(TEXT("Every effect kept"), Parsed.ProgrammerEffects.Num(), 3))
	{
		TestTrue(TEXT("Bad settings: the defaults"), Parsed.ProgrammerEffects[0].Effect.Audio.Equals(FStageEffectAudio()));
		TestFalse(TEXT("Not an object: audio off"), Parsed.ProgrammerEffects[1].Effect.UsesAudio());
		TestTrue(TEXT("Trigger kept, amount clamped"), Parsed.ProgrammerEffects[2].Effect.Audio.Mode == EStageEffectAudioMode::Trigger
			&& Parsed.ProgrammerEffects[2].Effect.Audio.Amount == 1.f);
	}
	TestTrue(TEXT("The bad values are counted"), Report.InvalidShowEntries >= 5);

	// Version 10 effects load with audio off.
	const FString V10 = FString::Printf(TEXT("{\"format\":\"StageCraftScene\",\"version\":10,\"name\":\"Old\",\"items\":[],\"programmerEffects\":[")
		TEXT("{\"id\":\"%s\",\"fixtures\":[1],\"target\":\"Intensity\",\"waveform\":\"Square\"}]}"), *Id1);
	TestTrue(TEXT("Version 10 parses"), FStageSceneStore::Parse(V10, Parsed = FStageSceneDocument(), Report = FStageSceneParseReport()) == EStageSceneResult::Success);
	TestTrue(TEXT("Version 10: audio off"), Parsed.ProgrammerEffects.Num() == 1 && !Parsed.ProgrammerEffects[0].Effect.UsesAudio());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
