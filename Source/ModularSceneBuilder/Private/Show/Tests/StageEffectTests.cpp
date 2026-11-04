// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for the effects engine (Docs/ADR/0027-effects-engine.md): the waveforms, the spread over fixtures (wings, blocks, direction,
// fan), the rules, the effect layer of the output mix, effects running from the programmer on real fixtures (tick, static look kept
// for recording, highlight and masters, clock kept through edits, undo, renumbered and removed fixtures, External DMX) and the
// scene format v9 "programmerEffects". The Effects panel is exercised by the scripted -game run (STATE.md #63).
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Show.Effect; Automation RunTests StageCraft.Scene.EffectData; Quit" -unattended -nullrhi

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
#include "Show/StageEffects.h"
#include "Show/StageShowMix.h"
#include "Subsystems/ShowControlSubsystem.h"
#include "Subsystems/StageProgrammerSubsystem.h"

namespace StageEffectTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;
	constexpr float Tolerance = 1e-4f;

	struct FScopedWorld
	{
		UWorld* World = nullptr;

		FScopedWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("StageEffectTestWorld"));
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

	/** Forwards programmer effect steps to the world's programmer; nothing else occurs in these tests. */
	class FEffectsEditor final : public IStageItemEditor
	{
	public:
		explicit FEffectsEditor(UWorld& InWorld) : World(InWorld) {}

		virtual bool CaptureItem(const FGuid&, FStageItemSnapshot&) const override { return false; }
		virtual EStageCommandResult RestoreItem(const FStageItemSnapshot&) override { return EStageCommandResult::Invalid; }
		virtual EStageCommandResult RemoveItem(const FGuid&) override { return EStageCommandResult::Invalid; }
		virtual EStageCommandResult SetItemTransform(const FGuid&, const FTransform&) override { return EStageCommandResult::Invalid; }
		virtual EStageCommandResult SetItemParameter(const FGuid&, const FGameplayTag&, const FStageParameterValue&) override { return EStageCommandResult::Invalid; }

		virtual EStageCommandResult SetProgrammerEffects(const FStageProgrammerEffects& Effects) override
		{
			World.GetSubsystem<UStageProgrammerSubsystem>()->SetEffectState(Effects);
			return EStageCommandResult::Succeeded;
		}

	private:
		UWorld& World;
	};

	FStageEffect MakeEffect(EStageEffectTarget Target, EStageEffectWaveform Waveform, float RateHz, float Spread)
	{
		FStageEffect Effect;
		Effect.Name = FText::FromString(TEXT("Test"));
		Effect.Target = Target;
		Effect.Waveform = Waveform;
		Effect.RateHz = RateHz;
		Effect.Spread = Spread;
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

	float Unit(EStageEffectWaveform Waveform, double Position, float Duty = 0.5f)
	{
		return StageEffects::SampleWave(Waveform, Position, Duty, 0).Unit;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageShowEffectWavesTest, "StageCraft.Show.EffectWaves", StageEffectTests::Flags)

bool FStageShowEffectWavesTest::RunTest(const FString& Parameters)
{
	using namespace StageEffectTests;
	using EWave = EStageEffectWaveform;

	TestEqual(TEXT("Sine starts in the middle"), Unit(EWave::Sine, 0.0), 0.5f, Tolerance);
	TestEqual(TEXT("Sine top at a quarter"), Unit(EWave::Sine, 0.25), 1.f, Tolerance);
	TestEqual(TEXT("Sine bottom at three quarters"), Unit(EWave::Sine, 0.75), 0.f, Tolerance);
	TestEqual(TEXT("Cosine starts at the top"), Unit(EWave::Cosine, 0.0), 1.f, Tolerance);
	TestEqual(TEXT("Square on inside the duty"), Unit(EWave::Square, 0.1, 0.25f), 1.f);
	TestEqual(TEXT("Square off after the duty"), Unit(EWave::Square, 0.3, 0.25f), 0.f);
	TestEqual(TEXT("Square duty 0 is always off"), Unit(EWave::Square, 0.0, 0.f), 0.f);
	TestEqual(TEXT("Ramp up"), Unit(EWave::RampUp, 0.25), 0.25f, Tolerance);
	TestEqual(TEXT("Ramp down"), Unit(EWave::RampDown, 0.25), 0.75f, Tolerance);
	TestEqual(TEXT("Triangle rises"), Unit(EWave::Triangle, 0.25), 0.5f, Tolerance);
	TestEqual(TEXT("Triangle top at half"), Unit(EWave::Triangle, 0.5), 1.f, Tolerance);
	TestEqual(TEXT("Only the fraction counts"), Unit(EWave::Sine, 7.25), 1.f, Tolerance);
	TestEqual(TEXT("Negative positions wrap"), Unit(EWave::Sine, -0.25), 0.f, Tolerance);

	// Random: one level per cycle, the same on every run, in range, and different from cycle to cycle and fixture to fixture.
	const float First = StageEffects::SampleWave(EWave::Random, 3.1, 0.5f, 4).Unit;
	TestEqual(TEXT("Random holds within a cycle"), StageEffects::SampleWave(EWave::Random, 3.9, 0.5f, 4).Unit, First);
	TestEqual(TEXT("Random repeats (seeded)"), StageEffects::SampleWave(EWave::Random, 3.5, 0.5f, 4).Unit, First);
	TSet<int32> Levels;
	bool bInRange = true;
	for (int32 Cycle = 0; Cycle < 40; ++Cycle)
	{
		const StageEffects::FSample Sample = StageEffects::SampleWave(EWave::Random, Cycle + 0.5, 0.5f, 4);
		bInRange &= Sample.Unit >= 0.f && Sample.Unit <= 1.f && Sample.Pan >= -1.f && Sample.Pan <= 1.f && Sample.Tilt >= -1.f && Sample.Tilt <= 1.f;
		Levels.Add(FMath::RoundToInt(Sample.Unit * 1000.f));
	}
	TestTrue(TEXT("Random stays in range"), bInRange);
	TestTrue(TEXT("Random changes from cycle to cycle"), Levels.Num() >= 30);
	TestNotEqual(TEXT("Random differs per fixture step"), StageEffects::SampleWave(EWave::Random, 3.1, 0.5f, 5).Unit, First);

	// Circle: pan and tilt a quarter apart, on the unit circle; Figure 8: tilt at twice the rate.
	bool bRound = true;
	for (double Position = 0.0; Position < 1.0; Position += 0.07)
	{
		const StageEffects::FSample Sample = StageEffects::SampleWave(EWave::Circle, Position, 0.5f, 0);
		bRound &= FMath::IsNearlyEqual(Sample.Pan * Sample.Pan + Sample.Tilt * Sample.Tilt, 1.f, 1e-3f);
	}
	TestTrue(TEXT("Circle stays on the circle"), bRound);
	const StageEffects::FSample Start = StageEffects::SampleWave(EWave::Circle, 0.0, 0.5f, 0);
	TestTrue(TEXT("Circle starts at the top"), FMath::IsNearlyEqual(Start.Pan, 0.f, Tolerance) && FMath::IsNearlyEqual(Start.Tilt, 1.f, Tolerance));
	const StageEffects::FSample Eight = StageEffects::SampleWave(EWave::Figure8, 0.125, 0.5f, 0);
	TestTrue(TEXT("Figure 8: tilt peaks at an eighth"), FMath::IsNearlyEqual(Eight.Tilt, 1.f, Tolerance) && Eight.Pan > 0.7f);
	TestEqual(TEXT("Single-axis waves give pan and tilt alike"), StageEffects::SampleWave(EWave::Sine, 0.25, 0.5f, 0).Tilt, 1.f, Tolerance);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageShowEffectSpreadTest, "StageCraft.Show.EffectSpread", StageEffectTests::Flags)

bool FStageShowEffectSpreadTest::RunTest(const FString& Parameters)
{
	using namespace StageEffectTests;
	FStageEffect Effect = MakeEffect(EStageEffectTarget::Intensity, EStageEffectWaveform::Sine, 1.f, 360.f);
	const auto Offsets = [&Effect](int32 Count)
	{
		TArray<float> Result;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Result.Add(static_cast<float>(StageEffects::GetFixtureOffset(Effect, Index, Count)));
		}
		return Result;
	};
	TestTrue(TEXT("Spread 360 over 4: a quarter apart"), Offsets(4) == TArray<float>({ 0.f, 0.25f, 0.5f, 0.75f }));
	Effect.Wings = 2;
	TestTrue(TEXT("Two wings mirror: out from the ends"), Offsets(4) == TArray<float>({ 0.f, 0.5f, 0.5f, 0.f }));
	Effect.Wings = 1;
	Effect.Blocks = 2;
	TestTrue(TEXT("Blocks of two move together"), Offsets(4) == TArray<float>({ 0.f, 0.f, 0.5f, 0.5f }));
	Effect.Blocks = 1;
	Effect.Phase = 90.f;
	TestEqual(TEXT("Phase shifts every fixture"), static_cast<float>(StageEffects::GetFixtureOffset(Effect, 0, 4)), 0.25f, Tolerance);
	Effect.Phase = 0.f;
	Effect.Spread = 0.f;
	TestTrue(TEXT("Spread 0: all in step"), Offsets(3) == TArray<float>({ 0.f, 0.f, 0.f }));
	TestEqual(TEXT("A single fixture is step 0"), static_cast<float>(StageEffects::GetFixtureOffset(Effect, 5, 1)), 0.f);
	Effect.Spread = 360.f;
	TestEqual(TEXT("An index past the end counts as the last"), static_cast<float>(StageEffects::GetFixtureOffset(Effect, 9, 4)), 0.75f, Tolerance);

	// Direction: forward the pattern runs first to last, reverse last to first; bounce runs out and back.
	FStageEffect Chase = MakeEffect(EStageEffectTarget::Intensity, EStageEffectWaveform::Square, 1.f, 360.f);
	Chase.Duty = 0.25f;
	TestEqual(TEXT("Forward: the first fixture is on first"), StageEffects::Evaluate(Chase, 0, 4, 0.1).Unit, 1.f);
	TestEqual(TEXT("Forward: the second is on a quarter later"), StageEffects::Evaluate(Chase, 1, 4, 0.35).Unit, 1.f);
	TestEqual(TEXT("Forward: the last is still off then"), StageEffects::Evaluate(Chase, 3, 4, 0.35).Unit, 0.f);
	Chase.Direction = EStageEffectDirection::Reverse;
	TestEqual(TEXT("Reverse: the last is on a quarter in"), StageEffects::Evaluate(Chase, 3, 4, 0.35).Unit, 1.f);
	TestEqual(TEXT("Reverse: the second is off then"), StageEffects::Evaluate(Chase, 1, 4, 0.35).Unit, 0.f);
	FStageEffect Bounce = MakeEffect(EStageEffectTarget::Intensity, EStageEffectWaveform::Sine, 1.f, 0.f);
	Bounce.Direction = EStageEffectDirection::Bounce;
	TestEqual(TEXT("Bounce: out like forward"), StageEffects::Evaluate(Bounce, 0, 1, 0.25).Unit, 1.f, Tolerance);
	TestEqual(TEXT("Bounce: back through the same values"), StageEffects::Evaluate(Bounce, 0, 1, 1.75).Unit, 1.f, Tolerance);
	TestEqual(TEXT("Bounce: no jump at the turn"), StageEffects::Evaluate(Bounce, 0, 1, 0.999).Unit, StageEffects::Evaluate(Bounce, 0, 1, 1.001).Unit, 1e-2f);

	// Size fan and levels.
	Effect.SizeFan = EStageFanMode::V;
	TestTrue(TEXT("Fan V: the ends at full size, the middle none"), StageEffects::GetSizeFactor(Effect, 0, 3) == 1.f
		&& StageEffects::GetSizeFactor(Effect, 1, 3) == 0.f && StageEffects::GetSizeFactor(Effect, 2, 3) == 1.f);
	Effect.Low = 0.2f;
	Effect.High = 0.8f;
	TestEqual(TEXT("Level: halfway between Low and High"), StageEffects::GetLevel(Effect, 0.5f, 1.f), 0.5f, Tolerance);
	TestEqual(TEXT("Level: no size stays at Low"), StageEffects::GetLevel(Effect, 1.f, 0.f), 0.2f, Tolerance);
	Effect.ColorMode = EStageEffectColorMode::Rainbow;
	TestTrue(TEXT("Rainbow starts red"), StageEffects::GetColor(Effect, 0.f).Equals(FLinearColor(1.f, 0.f, 0.f), 1e-3f));
	TestTrue(TEXT("Rainbow at a third is green"), StageEffects::GetColor(Effect, 1.f / 3.f).Equals(FLinearColor(0.f, 1.f, 0.f), 1e-3f));
	Effect.ColorMode = EStageEffectColorMode::Crossfade;
	Effect.ColorA = FLinearColor(1.f, 0.f, 0.f);
	Effect.ColorB = FLinearColor(0.f, 0.f, 1.f);
	TestTrue(TEXT("Crossfade halfway"), StageEffects::GetColor(Effect, 0.5f).Equals(FLinearColor(0.5f, 0.f, 0.5f), 1e-3f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageShowEffectRulesTest, "StageCraft.Show.EffectRules", StageEffectTests::Flags)

bool FStageShowEffectRulesTest::RunTest(const FString& Parameters)
{
	using namespace StageEffectTests;
	FStageEffect Wild = MakeEffect(EStageEffectTarget::Beam, EStageEffectWaveform::Sine, 100.f, 1.0e9f);
	Wild.Attribute = StageCraftTags::Attribute_Pan;
	Wild.Size = std::numeric_limits<float>::quiet_NaN();
	Wild.Low = -3.f;
	Wild.Wings = 0;
	Wild.Blocks = -5;
	Wild.Waveform = static_cast<EStageEffectWaveform>(200);
	Wild.ColorA = FLinearColor(5.f, -1.f, 0.5f, 0.2f);
	Wild.Name = FText::FromString(FString::ChrN(200, TEXT('x')));
	const FStageEffect Safe = StageEffectRules::SanitizeEffect(Wild);
	TestEqual(TEXT("Rate capped"), Safe.RateHz, StageEffectRules::MaxRateHz);
	TestEqual(TEXT("Spread capped"), Safe.Spread, StageEffectRules::MaxPhaseDegrees);
	TestEqual(TEXT("NaN size falls back"), Safe.Size, FStageEffect().Size);
	TestEqual(TEXT("Low clamped"), Safe.Low, 0.f);
	TestTrue(TEXT("Wings and blocks at least 1"), Safe.Wings == 1 && Safe.Blocks == 1);
	TestTrue(TEXT("Unknown waveform falls back"), Safe.Waveform == FStageEffect().Waveform);
	TestTrue(TEXT("Colour clamped and opaque"), Safe.ColorA.Equals(FLinearColor(1.f, 0.f, 0.5f, 1.f)));
	TestTrue(TEXT("A Beam effect cannot move pan: Zoom instead"), Safe.Attribute == StageCraftTags::Attribute_Zoom);
	TestEqual(TEXT("Name cut"), Safe.Name.ToString().Len(), StageEffectRules::MaxNameLength);
	FStageEffect Intensity = MakeEffect(EStageEffectTarget::Intensity, EStageEffectWaveform::Sine, 1.f, 0.f);
	Intensity.Attribute = StageCraftTags::Attribute_Iris;
	TestFalse(TEXT("Only Beam keeps an attribute"), StageEffectRules::SanitizeEffect(Intensity).Attribute.IsValid());
	TestTrue(TEXT("Iris and Zoom are beam attributes"), StageEffectRules::IsBeamAttribute(StageCraftTags::Attribute_Iris) && StageEffectRules::IsBeamAttribute(StageCraftTags::Attribute_Zoom));
	TestFalse(TEXT("Dimmer and colour are not"), StageEffectRules::IsBeamAttribute(StageCraftTags::Attribute_Dimmer) || StageEffectRules::IsBeamAttribute(StageCraftTags::Attribute_ColorR));
	TestEqual(TEXT("120 BPM is 2 Hz"), StageEffectRules::BPMToHz(120.f), 2.f);
	TestEqual(TEXT("0.5 Hz is 30 BPM"), StageEffectRules::HzToBPM(0.5f), 30.f);

	const FGuid Shared = FGuid::NewGuid();
	TArray<FStageEffectEntry> Entries;
	Entries.Add(MakeEntry(FGuid(), Intensity, { 1 }));
	Entries.Add(MakeEntry(Shared, Intensity, { 1, -2, 1, 0, 3 }));
	Entries.Add(MakeEntry(Shared, Intensity, { 4 }));
	Entries.Add(MakeEntry(FGuid::NewGuid(), Intensity, { -1 }));
	const int32 Dropped = StageEffectRules::SanitizeEntries(Entries);
	TestEqual(TEXT("One entry survives"), Entries.Num(), 1);
	TestTrue(TEXT("... with its unique positive fixtures in order"), Entries.Num() == 1 && Entries[0].FixtureIds == TArray<int32>({ 1, 3 }));
	TestEqual(TEXT("Every drop counted (3 entries, 3 fixture IDs, 1 left empty)"), Dropped, 7);
	TArray<FStageEffectEntry> Many;
	for (int32 Index = 0; Index < StageEffectRules::MaxEffects + 3; ++Index)
	{
		Many.Add(MakeEntry(FGuid::NewGuid(), Intensity, { 1 }));
	}
	StageEffectRules::SanitizeEntries(Many);
	TestEqual(TEXT("At most MaxEffects"), Many.Num(), StageEffectRules::MaxEffects);

	TSet<FString> Names;
	bool bAllValid = true;
	for (const FStageEffect& Quick : StageEffectRules::MakeQuickEffects())
	{
		bAllValid &= StageEffectRules::SanitizeEffect(Quick).Equals(Quick);
		Names.Add(Quick.Name.ToString());
	}
	TestTrue(TEXT("Quick effects are valid as they are"), bAllValid);
	TestEqual(TEXT("Quick effects are named apart"), Names.Num(), StageEffectRules::MakeQuickEffects().Num());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageShowEffectMixTest, "StageCraft.Show.EffectMix", StageEffectTests::Flags)

bool FStageShowEffectMixTest::RunTest(const FString& Parameters)
{
	using namespace StageShowMix;
	const FGameplayTag Dimmer = StageCraftTags::Attribute_Dimmer;
	const FGameplayTag Pan = StageCraftTags::Attribute_Pan;
	FAttributeMap Home;
	Home.Add(Dimmer, 0.f);
	Home.Add(Pan, 0.f);
	FAttributeMap Programmer;
	Programmer.Add(Pan, 30.f);
	FEffectMap Effects;
	Effects.Add(Pan, { 10.f, true, 1.f });
	Effects.Add(Dimmer, { 0.8f, false, 1.f });

	FFixtureLayers Layers;
	Layers.Home = &Home;
	Layers.Programmer = &Programmer;
	Layers.Effects = &Effects;
	float Value = -1.f;
	TestTrue(TEXT("Relative: added to the programmer's pan"), ResolveAttribute(Pan, Layers, Value) && FMath::IsNearlyEqual(Value, 40.f));
	TestTrue(TEXT("Absolute: replaces the dimmer"), ResolveAttribute(Dimmer, Layers, Value) && FMath::IsNearlyEqual(Value, 0.8f));
	Effects[Dimmer].Weight = 0.5f;
	TestTrue(TEXT("Half faded in: halfway from the static value"), ResolveAttribute(Dimmer, Layers, Value) && FMath::IsNearlyEqual(Value, 0.4f));
	Effects[Pan].Weight = 0.5f;
	TestTrue(TEXT("Relative half faded in: half the movement"), ResolveAttribute(Pan, Layers, Value) && FMath::IsNearlyEqual(Value, 35.f));
	Effects[Dimmer].Weight = 1.f;
	Layers.IntensityScale = 0.5f;
	TestTrue(TEXT("The masters scale an intensity effect"), ResolveAttribute(Dimmer, Layers, Value) && FMath::IsNearlyEqual(Value, 0.4f));
	Layers.IntensityScale = 1.f;
	FAttributeMap Highlight;
	Highlight.Add(Dimmer, 1.f);
	Layers.Highlight = &Highlight;
	TestTrue(TEXT("The highlight beats an effect"), ResolveAttribute(Dimmer, Layers, Value) && FMath::IsNearlyEqual(Value, 1.f));
	Layers.Highlight = nullptr;
	FFixtureLayers Bare;
	Bare.Effects = &Effects;
	TestFalse(TEXT("A relative effect needs a static value to move around"), ResolveAttribute(Pan, Bare, Value));
	TestTrue(TEXT("An absolute effect stands alone"), ResolveAttribute(Dimmer, Bare, Value) && FMath::IsNearlyEqual(Value, 0.8f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageShowEffectsTest, "StageCraft.Show.Effects", StageEffectTests::Flags)

bool FStageShowEffectsTest::RunTest(const FString& Parameters)
{
	using namespace StageEffectTests;
	const FGameplayTag Dimmer = StageCraftTags::Attribute_Dimmer;
	const FGameplayTag Pan = StageCraftTags::Attribute_Pan;
	const FGameplayTag Tilt = StageCraftTags::Attribute_Tilt;

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
	const float TiltHome = A->GetAttribute(Tilt);
	TestFalse(TEXT("Idle: no tick"), Show->IsTickable());

	// A circle around a programmer position: relative to it, from the effect's own clock.
	TMap<FGameplayTag, float> Held;
	Held.Add(Pan, 10.f);
	Programmer->SetValues(A->GetFixtureId(), Held);
	FStageEffect Circle = MakeEffect(EStageEffectTarget::Position, EStageEffectWaveform::Circle, 1.f, 0.f);
	Circle.Size = 20.f;
	const FGuid CircleId = Programmer->AddEffect(Circle, Ids);
	TestTrue(TEXT("Started"), CircleId.IsValid() && Show->AreEffectsLive() && Show->IsTickable());
	TestEqual(TEXT("Clock 0: pan at the programmer's 10"), A->GetAttribute(Pan), 10.f, 1e-3f);
	TestEqual(TEXT("Clock 0: tilt at the top of the circle"), A->GetAttribute(Tilt), TiltHome + 20.f, 1e-3f);
	Show->Tick(0.25f);
	TestEqual(TEXT("A quarter cycle: pan out by the size"), A->GetAttribute(Pan), 30.f, 1e-2f);
	TestEqual(TEXT("... tilt back in the middle"), A->GetAttribute(Tilt), TiltHome, 1e-2f);
	TestEqual(TEXT("Recording takes the static look, not the effect"), Show->GetRecordableValues(*A).FindRef(Pan), 10.f, 1e-3f);

	// A dimmer chase on its own clock: one fixture on at a time, in selection order.
	FStageEffect Chase = MakeEffect(EStageEffectTarget::Intensity, EStageEffectWaveform::Square, 1.f, 360.f);
	Chase.Duty = 1.f / 3.f - 0.01f;
	const FGuid ChaseId = Programmer->AddEffect(Chase, Ids);
	TestTrue(TEXT("Chase step 1: A on, B and C off"), A->GetAttribute(Dimmer) == 1.f && B->GetAttribute(Dimmer) == 0.f && C->GetAttribute(Dimmer) == 0.f);
	TestEqual(TEXT("The circle's clock carried on"), static_cast<float>(Show->GetEffectCycles(CircleId)), 0.25f, 1e-4f);
	Show->Tick(0.4f);
	TestTrue(TEXT("Chase step 2: B on"), A->GetAttribute(Dimmer) == 0.f && B->GetAttribute(Dimmer) == 1.f && C->GetAttribute(Dimmer) == 0.f);
	Show->SetGrandMaster(0.5f);
	TestEqual(TEXT("The Grand Master scales the chase"), B->GetAttribute(Dimmer), 0.5f, 1e-3f);
	Show->SetGrandMaster(1.f);
	Programmer->SetHighlightedFixtures({ A->GetFixtureId() });
	TestEqual(TEXT("The highlight beats the chase"), A->GetAttribute(Dimmer), 1.f);
	Programmer->SetHighlightedFixtures({});
	TestEqual(TEXT("Highlight off: the chase again"), A->GetAttribute(Dimmer), 0.f);

	// An edit keeps the clock; undo and redo restore the effects exactly.
	const FStageProgrammerEffects BeforeEdit = Programmer->GetEffectState();
	const float ClockBefore = static_cast<float>(Show->GetEffectCycles(ChaseId));
	FStageEffect Faster = Chase;
	Faster.RateHz = 2.f;
	TestTrue(TEXT("Edit"), Programmer->UpdateEffect(ChaseId, Faster));
	TestFalse(TEXT("The same edit again changes nothing"), Programmer->UpdateEffect(ChaseId, Faster));
	TestEqual(TEXT("The edit kept the clock"), static_cast<float>(Show->GetEffectCycles(ChaseId)), ClockBefore, 1e-5f);
	FEffectsEditor Editor(*Scoped.World);
	FStageProgrammerEffectsCommand Step(FText::FromString(TEXT("Edit")), BeforeEdit, Programmer->GetEffectState());
	TestTrue(TEXT("Undo"), Step.Undo(Editor) == EStageCommandResult::Succeeded && Programmer->GetEffects()[1].Effect.RateHz == 1.f);
	TestTrue(TEXT("Redo"), Step.Redo(Editor) == EStageCommandResult::Succeeded && Programmer->GetEffects()[1].Effect.RateHz == 2.f);

	// A renumbered fixture leaves its effects; a destroyed one is simply gone.
	TestTrue(TEXT("Renumber C"), C->SetFixtureId(999));
	Show->Tick(0.05f);
	const float CPan = C->GetAttribute(Pan);
	Show->Tick(0.2f);
	TestEqual(TEXT("C no longer moves"), C->GetAttribute(Pan), CPan, 1e-4f);
	B->Destroy();
	Show->Tick(0.1f);
	TestTrue(TEXT("Still running on A"), Show->AreEffectsLive());

	// External DMX: effects hold still and are not written; back to Internal they run again.
	Show->SetControlSource(EStageControlSource::External);
	TestFalse(TEXT("External: not live"), Show->AreEffectsLive());
	TestFalse(TEXT("External: no tick"), Show->IsTickable());
	Show->SetControlSource(EStageControlSource::Internal);
	TestTrue(TEXT("Internal: live again"), Show->AreEffectsLive());

	// Stop: the fixture goes back to its static look and the show stops ticking.
	TestTrue(TEXT("Stop the chase"), Programmer->RemoveEffect(ChaseId));
	TestFalse(TEXT("Stop it again"), Programmer->RemoveEffect(ChaseId));
	Programmer->SetEffects({});
	TestEqual(TEXT("Stopped: pan back to the programmer's"), A->GetAttribute(Pan), 10.f, 1e-3f);
	TestEqual(TEXT("Stopped: tilt back home"), A->GetAttribute(Tilt), TiltHome, 1e-3f);
	TestEqual(TEXT("Stopped: dimmer back home"), A->GetAttribute(Dimmer), A->GetHomeAttributes().FindRef(Dimmer), 1e-3f);
	TestFalse(TEXT("Stopped: no tick"), Show->IsTickable());
	TestFalse(TEXT("No fixtures: nothing starts"), Programmer->AddEffect(Chase, { -1, 0 }).IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageSceneEffectDataTest, "StageCraft.Scene.EffectData", StageEffectTests::Flags)

bool FStageSceneEffectDataTest::RunTest(const FString& Parameters)
{
	using namespace StageEffectTests;
	FStageEffect Circle = MakeEffect(EStageEffectTarget::Position, EStageEffectWaveform::Figure8, 0.3f, 90.f);
	Circle.Name = FText::FromString(TEXT("Eight"));
	Circle.Axis = EStageEffectAxis::Tilt;
	Circle.Size = 33.f;
	Circle.Phase = 45.f;
	Circle.Wings = 2;
	Circle.Blocks = 3;
	Circle.Direction = EStageEffectDirection::Bounce;
	Circle.SizeFan = EStageFanMode::InvertedV;
	FStageEffect Iris = MakeEffect(EStageEffectTarget::Beam, EStageEffectWaveform::Random, 4.f, 360.f);
	Iris.Attribute = StageCraftTags::Attribute_Iris;
	Iris.Low = 0.1f;
	Iris.High = 0.7f;
	Iris.Duty = 0.3f;
	Iris.ColorMode = EStageEffectColorMode::Rainbow;
	Iris.ColorA = FLinearColor(0.f, 0.5f, 1.f);

	FStageSceneDocument Document;
	Document.Name = TEXT("Effects");
	Document.ProgrammerEffects.Add(MakeEntry(FGuid::NewGuid(), Circle, { 3, 1, 2 }));
	Document.ProgrammerEffects.Add(MakeEntry(FGuid::NewGuid(), Iris, { 7 }));
	FStageSceneDocument Parsed;
	FStageSceneParseReport Report;
	TestTrue(TEXT("Round trip parses"), FStageSceneStore::Parse(FStageSceneStore::Serialize(Document), Parsed, Report) == EStageSceneResult::Success);
	TestEqual(TEXT("Nothing dropped"), Report.InvalidShowEntries, 0);
	TestTrue(TEXT("Both effects back, exactly"), UStageProgrammerSubsystem::AreEffectsEqual(Parsed.ProgrammerEffects, Document.ProgrammerEffects));

	// Hostile v9 effects: bad entries are dropped, bad settings fall back, each counted; the rest loads.
	const FString GoodId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
	const FString Hostile = FString::Printf(TEXT("{\"format\":\"StageCraftScene\",\"version\":%d,\"name\":\"H\",\"items\":[],\"programmerEffects\":[")
		TEXT("{\"id\":\"nope\",\"fixtures\":[1]},7,")
		TEXT("{\"id\":\"%s\",\"fixtures\":[1,-4,\"x\",1,2],\"target\":\"Lasers\",\"waveform\":\"Wobble\",\"rate\":1e9,\"size\":\"big\",\"wings\":0,")
		TEXT("\"colorA\":[1,2],\"attribute\":\"StageCraft.Attribute.Dimmer\"},")
		TEXT("{\"id\":\"%s\",\"fixtures\":[5]},")
		TEXT("{\"id\":\"%s\",\"fixtures\":[]}]}"),
		FStageSceneStore::FormatVersion, *GoodId, *GoodId, *FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens));
	TestTrue(TEXT("A hostile file still parses"), FStageSceneStore::Parse(Hostile, Parsed = FStageSceneDocument(), Report = FStageSceneParseReport()) == EStageSceneResult::Success);
	TestEqual(TEXT("One effect survives"), Parsed.ProgrammerEffects.Num(), 1);
	if (Parsed.ProgrammerEffects.Num() == 1)
	{
		const FStageEffectEntry& Survivor = Parsed.ProgrammerEffects[0];
		TestTrue(TEXT("... on fixtures 1 and 2"), Survivor.FixtureIds == TArray<int32>({ 1, 2 }));
		TestTrue(TEXT("... with defaults for the bad settings"), Survivor.Effect.Target == FStageEffect().Target && Survivor.Effect.Waveform == FStageEffect().Waveform
			&& Survivor.Effect.RateHz == FStageEffect().RateHz && Survivor.Effect.Size == FStageEffect().Size && Survivor.Effect.Wings == 1);
	}
	TestTrue(TEXT("The drops are counted"), Report.InvalidShowEntries >= 12);

	const FString V8 = TEXT("{\"format\":\"StageCraftScene\",\"version\":8,\"name\":\"Old\",\"items\":[],\"programmerEffects\":[{\"id\":\"x\"}]}");
	TestTrue(TEXT("Version 8 parses"), FStageSceneStore::Parse(V8, Parsed = FStageSceneDocument(), Report = FStageSceneParseReport()) == EStageSceneResult::Success);
	TestTrue(TEXT("Version 8: no effects, nothing read"), Parsed.ProgrammerEffects.IsEmpty() && Report.InvalidShowEntries == 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
