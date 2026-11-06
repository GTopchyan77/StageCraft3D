// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for show diagnostics and "why is it dark" (Docs/ADR/0030 §5, STATE.md #73): the mix explanation layer by layer against
// StageShowMix::ResolveAttribute, the fixture explanation's causes, every diagnostic code on a snapshot and on a built world, the
// push to the Problems list, and the assistant's questions in English and Armenian.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Show.Explain+StageCraft.Show.Diagnostics+StageCraft.Assistant.Diagnostics; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Algo/AnyOf.h"
#include "Actors/LightingFixtureActor.h"
#include "AI/StageAICommandParser.h"
#include "AI/StageAICommandSchema.h"
#include "AI/StageAICommandSubsystem.h"
#include "Containers/Ticker.h"
#include "Data/LightingFixtureData.h"
#include "Data/StageParameterTypes.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/AutomationTest.h"
#include "Show/StageFixtureExplain.h"
#include "Show/StageShowDiagnostics.h"
#include "Show/StageShowMix.h"
#include "Subsystems/ShowControlSubsystem.h"
#include "Subsystems/StageProgrammerSubsystem.h"
#include "Subsystems/StageShowDiagnosticsSubsystem.h"
#include "UI/StageUIAnchors.h"
#include "Workspace/StageWorkspaceTypes.h"

namespace StageDiagnosticsTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	/** A game world that has begun play, destroyed with the scope. Fixtures register with its show control on BeginPlay. */
	struct FScopedWorld
	{
		UWorld* World = nullptr;

		FScopedWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("StageDiagnosticsTestWorld"));
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

	FStageDMXPatch MakePatch(int32 Universe, int32 Address)
	{
		FStageDMXPatch Patch;
		Patch.Universe = Universe;
		Patch.Address = Address;
		return Patch;
	}

	bool HasArmenian(const FString& Text)
	{
		return Text.Len() > 0 && Algo::AnyOf(Text, [](TCHAR Char) { return Char >= 0x0531 && Char <= 0x0587; });
	}

	const FStageDiagnostic* FindCode(TConstArrayView<FStageDiagnostic> Found, EStageDiagnosticCode Code)
	{
		return Found.FindByPredicate([Code](const FStageDiagnostic& Diagnostic) { return Diagnostic.Code == Code; });
	}

	/** A fixture that is lit in the cue list, on screen and on the wire. */
	FStageFixtureFacts LitFacts()
	{
		FStageFixtureFacts Facts;
		Facts.FixtureId = 12;
		Facts.bExists = true;
		Facts.Name = TEXT("Spot 575");
		Facts.Patch = MakePatch(1, 1);
		Facts.OutputState = EStageDMXOutputState::Sending;
		Facts.Dimmer.Layer = StageShowMix::ELayer::CueList;
		Facts.Dimmer.StaticValue = 0.8f;
		Facts.Dimmer.Value = 0.8f;
		Facts.Dimmer.bFound = true;
		Facts.Dimmer.bIntensity = true;
		Facts.OutputDimmer = 0.8f;
		Facts.ActiveCue = 4.f;
		Facts.DimmerCue = 4.f;
		return Facts;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageShowExplainTest, "StageCraft.Show.Explain", StageDiagnosticsTests::Flags)

bool FStageShowExplainTest::RunTest(const FString& Parameters)
{
	using namespace StageDiagnosticsTests;
	using StageShowMix::ELayer;
	const FGameplayTag Dimmer = StageCraftTags::Attribute_Dimmer;
	const FGameplayTag Pan = StageCraftTags::Attribute_Pan;

	// 1. The mix explanation names each layer as the winner, and always gives ResolveAttribute's value.
	{
		const TMap<FGameplayTag, float> Home = { { Dimmer, 0.1f }, { Pan, 5.f } };
		const TMap<FGameplayTag, float> Main = { { Dimmer, 0.4f }, { Pan, 10.f } };
		const TMap<FGameplayTag, float> PlaybackHigh = { { Dimmer, 1.f }, { Pan, 20.f } };
		const TMap<FGameplayTag, float> PlaybackLate = { { Pan, 30.f } };
		const TMap<FGameplayTag, float> Programmer = { { Dimmer, 0.6f } };
		const TMap<FGameplayTag, float> Highlight = { { Dimmer, 1.f } };
		StageShowMix::FEffectMap Effects;
		Effects.Add(Dimmer, { 0.f, false, 1.f });

		struct FCase
		{
			const TCHAR* Name;
			StageShowMix::FFixtureLayers Layers;
			FGameplayTag Attribute;
			ELayer Layer;
			int32 Playback;
			bool bEffect;
			bool bMasters;
		};
		TArray<FCase> Cases;
		const auto Layers = [&](bool bMain, int32 Playbacks, bool bProgrammer, bool bEffect, bool bHighlight, float Scale)
		{
			StageShowMix::FFixtureLayers Out;
			Out.Home = &Home;
			Out.Playback = bMain ? &Main : nullptr;
			if (Playbacks >= 1)
			{
				Out.Playbacks.Add({ &PlaybackHigh, 0.5f, 1, 3 });
			}
			if (Playbacks >= 2)
			{
				Out.Playbacks.Add({ &PlaybackLate, 1.f, 2, 6 });
			}
			Out.Programmer = bProgrammer ? &Programmer : nullptr;
			Out.Effects = bEffect ? &Effects : nullptr;
			Out.Highlight = bHighlight ? &Highlight : nullptr;
			Out.IntensityScale = Scale;
			return Out;
		};
		Cases.Add({ TEXT("Home"), Layers(false, 0, false, false, false, 1.f), Dimmer, ELayer::Home, INDEX_NONE, false, false });
		Cases.Add({ TEXT("Cue list over home"), Layers(true, 0, false, false, false, 1.f), Dimmer, ELayer::CueList, INDEX_NONE, false, false });
		Cases.Add({ TEXT("HTP: playback 4 at 50% (0.5) beats the cue list's 0.4"), Layers(true, 1, false, false, false, 1.f), Dimmer, ELayer::Playback, 3, false, false });
		Cases.Add({ TEXT("LTP: the playback raised last holds pan"), Layers(true, 2, false, false, false, 1.f), Pan, ELayer::Playback, 6, false, false });
		Cases.Add({ TEXT("Programmer over the playbacks"), Layers(true, 2, true, false, false, 1.f), Dimmer, ELayer::Programmer, INDEX_NONE, false, false });
		Cases.Add({ TEXT("An effect over the programmer"), Layers(true, 1, true, true, false, 1.f), Dimmer, ELayer::Programmer, INDEX_NONE, true, false });
		Cases.Add({ TEXT("Highlight hides the effect"), Layers(true, 1, true, true, true, 1.f), Dimmer, ELayer::Highlight, INDEX_NONE, false, false });
		Cases.Add({ TEXT("Masters scale intensity"), Layers(true, 0, false, false, false, 0.5f), Dimmer, ELayer::CueList, INDEX_NONE, false, true });
		Cases.Add({ TEXT("Masters leave pan alone"), Layers(true, 0, false, false, false, 0.5f), Pan, ELayer::CueList, INDEX_NONE, false, false });
		Cases.Add({ TEXT("Blackout over the highlight"), Layers(true, 0, false, false, true, 0.f), Dimmer, ELayer::Highlight, INDEX_NONE, false, true });
		for (const FCase& Case : Cases)
		{
			const StageShowMix::FAttributeExplanation Explained = StageShowMix::ExplainAttribute(Case.Attribute, Case.Layers);
			float Resolved = -1.f;
			const bool bResolved = StageShowMix::ResolveAttribute(Case.Attribute, Case.Layers, Resolved);
			TestEqual(*FString::Printf(TEXT("%s: layer"), Case.Name), static_cast<int32>(Explained.Layer), static_cast<int32>(Case.Layer));
			TestEqual(*FString::Printf(TEXT("%s: playback"), Case.Name), Explained.Playback, Case.Playback);
			TestEqual(*FString::Printf(TEXT("%s: effect"), Case.Name), Explained.bEffect, Case.bEffect);
			TestEqual(*FString::Printf(TEXT("%s: masters"), Case.Name), Explained.bScaledByMasters, Case.bMasters);
			TestEqual(*FString::Printf(TEXT("%s: found as the mix"), Case.Name), Explained.bFound, bResolved);
			TestEqual(*FString::Printf(TEXT("%s: the mix's value"), Case.Name), Explained.Value, Resolved, 1e-5f);
		}
		const StageShowMix::FAttributeExplanation Empty = StageShowMix::ExplainAttribute(Dimmer, StageShowMix::FFixtureLayers());
		TestTrue(TEXT("No layer: not found"), !Empty.bFound && Empty.Layer == ELayer::None);
	}

	// 2. The fixture explanation: the first cause along the chain, and where it is fixed.
	{
		struct FCause
		{
			const TCHAR* Name;
			FStageFixtureFacts Facts;
			EStageDarkCause Dark;
			EStageOutputCause Output;
			EStageDiagnosticPlace Place;
			const TCHAR* Says;
		};
		TArray<FCause> Causes;
		FStageFixtureFacts Facts = LitFacts();
		Causes.Add({ TEXT("Lit"), Facts, EStageDarkCause::None, EStageOutputCause::None, EStageDiagnosticPlace::None, TEXT("is lit") });

		Facts = LitFacts();
		Facts.bExists = false;
		Causes.Add({ TEXT("Not found"), Facts, EStageDarkCause::NotFound, EStageOutputCause::None, EStageDiagnosticPlace::Patch, TEXT("no fixture 12") });

		Facts = LitFacts();
		Facts.bBlackout = true;
		Facts.Dimmer.IntensityScale = 0.f;
		Facts.Dimmer.bScaledByMasters = true;
		Facts.Dimmer.Value = 0.f;
		Facts.OutputDimmer = 0.f;
		Causes.Add({ TEXT("Blackout"), Facts, EStageDarkCause::Blackout, EStageOutputCause::None, EStageDiagnosticPlace::Blackout, TEXT("BLACKOUT is on") });

		Facts.bBlackout = false;
		Facts.GrandMaster = 0.f;
		Causes.Add({ TEXT("Grand Master at 0"), Facts, EStageDarkCause::GrandMaster, EStageOutputCause::None, EStageDiagnosticPlace::GrandMaster, TEXT("Grand Master is at 0") });

		Facts = LitFacts();
		Facts.Dimmer.StaticValue = Facts.Dimmer.Value = Facts.OutputDimmer = 0.f;
		Facts.DimmerCue = 2.f;
		Causes.Add({ TEXT("Tracked dimmer 0"), Facts, EStageDarkCause::Dimmer, EStageOutputCause::None, EStageDiagnosticPlace::Cues, TEXT("from cue 2, tracked into the playing cue 4") });

		Facts = LitFacts();
		Facts.Dimmer.Layer = StageShowMix::ELayer::Playback;
		Facts.Dimmer.Playback = 2;
		Facts.PlaybackName = FText::FromString(TEXT("Wash"));
		Facts.Dimmer.StaticValue = Facts.Dimmer.Value = Facts.OutputDimmer = 0.f;
		Causes.Add({ TEXT("Playback dimmer 0"), Facts, EStageDarkCause::Dimmer, EStageOutputCause::None, EStageDiagnosticPlace::Playback, TEXT("playback 3 \"Wash\"") });

		Facts = LitFacts();
		Facts.Dimmer.Layer = StageShowMix::ELayer::Programmer;
		Facts.Dimmer.StaticValue = Facts.Dimmer.Value = Facts.OutputDimmer = 0.f;
		Causes.Add({ TEXT("Programmer dimmer 0"), Facts, EStageDarkCause::Dimmer, EStageOutputCause::None, EStageDiagnosticPlace::Programmer, TEXT("held in the programmer") });

		Facts = LitFacts();
		Facts.Dimmer.bEffect = true;
		Facts.Dimmer.Value = Facts.OutputDimmer = 0.f;
		Facts.IntensityEffects = { FText::FromString(TEXT("Bass Pulse")) };
		Facts.bAudioEffect = true;
		Facts.bAudioKill = true;
		Causes.Add({ TEXT("Effect"), Facts, EStageDarkCause::Effect, EStageOutputCause::None, EStageDiagnosticPlace::Effects, TEXT("AUDIO KILL is on") });

		Facts = LitFacts();
		Facts.ColorFraction = 0.f;
		Causes.Add({ TEXT("Black colour"), Facts, EStageDarkCause::Color, EStageOutputCause::None, EStageDiagnosticPlace::None, TEXT("colour lets no light out") });

		Facts = LitFacts();
		Facts.bHasIris = true;
		Facts.Iris = 1.f;
		Causes.Add({ TEXT("Iris"), Facts, EStageDarkCause::IrisClosed, EStageOutputCause::None, EStageDiagnosticPlace::None, TEXT("iris is closed") });

		Facts = LitFacts();
		Facts.ControlSource = EStageControlSource::External;
		Facts.Dimmer = StageShowMix::FAttributeExplanation();
		Facts.OutputDimmer = 0.f;
		Causes.Add({ TEXT("External, DMX IN off"), Facts, EStageDarkCause::NoInput, EStageOutputCause::OutputPaused, EStageDiagnosticPlace::DMXIn, TEXT("DMX IN is off") });
		Facts.InputState = EStageDMXInputState::Listening;
		Causes.Add({ TEXT("External, universe silent"), Facts, EStageDarkCause::NoInputSignal, EStageOutputCause::OutputPaused, EStageDiagnosticPlace::DMXIn, TEXT("no console sends universe 1") });
		Facts.Patch = FStageDMXPatch();
		Causes.Add({ TEXT("External, unpatched"), Facts, EStageDarkCause::NoInputSignal, EStageOutputCause::Unpatched, EStageDiagnosticPlace::Patch, TEXT("has no DMX address") });
		Facts.Patch = MakePatch(1, 1);
		Facts.bUniverseLive = true;
		Facts.Dimmer.StaticValue = 0.f;
		Causes.Add({ TEXT("External, the console sends 0"), Facts, EStageDarkCause::Dimmer, EStageOutputCause::OutputPaused, EStageDiagnosticPlace::None, TEXT("from DMX IN (the console") });

		// On the wire only: lit on screen.
		Facts = LitFacts();
		Facts.OutputState = EStageDMXOutputState::Off;
		Causes.Add({ TEXT("DMX OUT off: normal for previs"), Facts, EStageDarkCause::None, EStageOutputCause::OutputOff, EStageDiagnosticPlace::None, TEXT("DMX OUT is off") });
		Facts.OutputState = EStageDMXOutputState::Failed;
		Causes.Add({ TEXT("DMX OUT failed"), Facts, EStageDarkCause::None, EStageOutputCause::OutputFailed, EStageDiagnosticPlace::DMXOut, TEXT("DMX OUT failed") });
		Facts.OutputState = EStageDMXOutputState::Sending;
		Facts.Patch = FStageDMXPatch();
		Causes.Add({ TEXT("Unpatched"), Facts, EStageDarkCause::None, EStageOutputCause::Unpatched, EStageDiagnosticPlace::Patch, TEXT("AUTO PATCH") });
		Facts.Patch = MakePatch(1, 1);
		Facts.bPatchConflict = true;
		Causes.Add({ TEXT("Conflict"), Facts, EStageDarkCause::None, EStageOutputCause::PatchConflict, EStageDiagnosticPlace::Patch, TEXT("address 1.001 overlaps") });
		Facts.bPatchConflict = false;
		Facts.bUniverseMuted = true;
		Causes.Add({ TEXT("Muted"), Facts, EStageDarkCause::None, EStageOutputCause::UniverseMuted, EStageDiagnosticPlace::DMXOut, TEXT("universe 1 is muted") });

		for (const FCause& Cause : Causes)
		{
			const FStageFixtureVerdict Verdict = StageFixtureExplain::Explain(Cause.Facts, EStageAILanguage::English);
			TestEqual(*FString::Printf(TEXT("%s: dark cause"), Cause.Name), static_cast<int32>(Verdict.DarkCause), static_cast<int32>(Cause.Dark));
			TestEqual(*FString::Printf(TEXT("%s: output cause"), Cause.Name), static_cast<int32>(Verdict.OutputCause), static_cast<int32>(Cause.Output));
			TestEqual(*FString::Printf(TEXT("%s: place"), Cause.Name), static_cast<int32>(Verdict.Place), static_cast<int32>(Cause.Place));
			if (!TestTrue(*FString::Printf(TEXT("%s: says \"%s\""), Cause.Name, Cause.Says), Verdict.Text.Contains(Cause.Says)))
			{
				AddInfo(Verdict.Text);
			}
			const FStageFixtureVerdict Armenian = StageFixtureExplain::Explain(Cause.Facts, EStageAILanguage::Armenian);
			TestTrue(*FString::Printf(TEXT("%s: Armenian text"), Cause.Name), HasArmenian(Armenian.Text));
			TestEqual(*FString::Printf(TEXT("%s: same cause in Armenian"), Cause.Name), static_cast<int32>(Armenian.DarkCause), static_cast<int32>(Cause.Dark));
		}
	}

	// 3. On a built show: the explanation follows the real mix.
	{
		FScopedWorld Scope;
		ALightingFixtureActor* A = Scope.SpawnMover();
		ALightingFixtureActor* B = Scope.SpawnMover();
		UShowControlSubsystem* Show = Scope.World->GetSubsystem<UShowControlSubsystem>();
		UStageProgrammerSubsystem* Programmer = Scope.World->GetSubsystem<UStageProgrammerSubsystem>();
		UStageShowDiagnosticsSubsystem* Diagnostics = Scope.World->GetSubsystem<UStageShowDiagnosticsSubsystem>();
		if (!TestNotNull(TEXT("A"), A) || !TestNotNull(TEXT("B"), B) || !TestNotNull(TEXT("Show"), Show) || !TestNotNull(TEXT("Programmer"), Programmer)
			|| !TestNotNull(TEXT("Diagnostics"), Diagnostics))
		{
			return false;
		}
		const int32 IdA = A->GetFixtureId();
		const int32 IdB = B->GetFixtureId();

		// Cue 1 (blocked): A at 0, B full. Cue 2 changes only B's pan, so A's 0 tracks from cue 1.
		FShowCue Cue1;
		Cue1.Number = 1.f;
		Cue1.FadeTime = 0.f;
		Cue1.bBlock = true;
		Cue1.Fixtures = { Entry(IdA, { { Dimmer, 0.f } }), Entry(IdB, { { Dimmer, 1.f } }) };
		FShowCue Cue2;
		Cue2.Number = 2.f;
		Cue2.FadeTime = 0.f;
		Cue2.Fixtures = { Entry(IdB, { { Pan, 30.f } }) };
		Show->SetCues({ Cue1, Cue2 });
		TestTrue(TEXT("Go to cue 2"), Show->GoToCue(2.f));
		Show->FinishFade();

		const StageShowMix::FAttributeExplanation AtCue = Show->ExplainAttribute(IdA, Dimmer);
		TestTrue(TEXT("A's dimmer from the cue list"), AtCue.Layer == ELayer::CueList && AtCue.Value == 0.f);
		TestTrue(TEXT("... set by cue 1"), Show->FindSettingCue(IdA, Dimmer).Get(-1.f) == 1.f);
		FStageFixtureVerdict Verdict = Diagnostics->ExplainFixture(IdA, EStageAILanguage::English);
		TestEqual(TEXT("A is dark by its dimmer"), static_cast<int32>(Verdict.DarkCause), static_cast<int32>(EStageDarkCause::Dimmer));
		TestTrue(TEXT("... tracked from cue 1"), Verdict.Text.Contains(TEXT("from cue 1, tracked into the playing cue 2")));
		TestEqual(TEXT("B is lit"), static_cast<int32>(Diagnostics->ExplainFixture(IdB, EStageAILanguage::English).DarkCause), static_cast<int32>(EStageDarkCause::None));

		// The programmer lifts A; the masters take it down again.
		Programmer->SetValues(IdA, { { Dimmer, 0.7f } });
		TestTrue(TEXT("A held in the programmer"), Show->ExplainAttribute(IdA, Dimmer).Layer == ELayer::Programmer);
		TestEqual(TEXT("A lit by the programmer"), static_cast<int32>(Diagnostics->ExplainFixture(IdA, EStageAILanguage::English).DarkCause), static_cast<int32>(EStageDarkCause::None));
		Show->SetGrandMaster(0.f);
		TestEqual(TEXT("Grand Master at 0"), static_cast<int32>(Diagnostics->ExplainFixture(IdA, EStageAILanguage::English).DarkCause), static_cast<int32>(EStageDarkCause::GrandMaster));
		Show->SetGrandMaster(1.f);
		Show->SetBlackout(true);
		TestEqual(TEXT("Blackout"), static_cast<int32>(Diagnostics->ExplainFixture(IdA, EStageAILanguage::English).DarkCause), static_cast<int32>(EStageDarkCause::Blackout));
		Show->SetBlackout(false);

		// A playback above the cue list (HTP), then the highlight above everything.
		Programmer->ClearAll();
		TestTrue(TEXT("Record playback 1"), Show->RecordPlayback(0, { Entry(IdA, { { Dimmer, 0.9f } }) }));
		TestTrue(TEXT("Raise playback 1"), Show->SetPlaybackLevel(0, 1.f));
		const StageShowMix::FAttributeExplanation FromPlayback = Show->ExplainAttribute(IdA, Dimmer);
		TestTrue(TEXT("A from playback 1"), FromPlayback.Layer == ELayer::Playback && FromPlayback.Playback == 0);
		TestEqual(TEXT("... the value on stage"), FromPlayback.Value, A->GetAttribute(Dimmer), 1e-4f);
		TestTrue(TEXT("... said so"), Diagnostics->ExplainFixture(IdA, EStageAILanguage::English).Text.Contains(TEXT("from playback 1")));
		Programmer->SetHighlightedFixtures({ IdA });
		TestTrue(TEXT("Highlight wins"), Show->ExplainAttribute(IdA, Dimmer).Layer == ELayer::Highlight);
		Programmer->SetHighlightedFixtures({});

		TestEqual(TEXT("Unknown fixture"), static_cast<int32>(Diagnostics->ExplainFixture(9999, EStageAILanguage::English).DarkCause), static_cast<int32>(EStageDarkCause::NotFound));
		TestFalse(TEXT("Unknown fixture: nothing explained"), Show->ExplainAttribute(9999, Dimmer).bFound);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageShowDiagnosticsTest, "StageCraft.Show.Diagnostics", StageDiagnosticsTests::Flags)

bool FStageShowDiagnosticsTest::RunTest(const FString& Parameters)
{
	using namespace StageDiagnosticsTests;
	const FGameplayTag Dimmer = StageCraftTags::Attribute_Dimmer;

	// 1. A clean show has no findings; each code fires on its own fault.
	TArray<FShowCue> Cues;
	FShowCue Cue;
	Cue.Number = 1.f;
	Cue.Fixtures = { Entry(1, { { Dimmer, 1.f } }) };
	Cues.Add(Cue);
	TArray<FStagePlayback> Playbacks;
	Playbacks.SetNum(StagePlaybackRules::Count);
	const auto Clean = [&Cues, &Playbacks]()
	{
		FStageShowSnapshot Snapshot;
		Snapshot.Fixtures = { { 1, MakePatch(1, 1) }, { 2, MakePatch(1, 20) } };
		Snapshot.Cues = Cues;
		Snapshot.Playbacks = Playbacks;
		return Snapshot;
	};
	TestEqual(TEXT("Clean show: nothing"), StageShowDiagnostics::Run(Clean()).Num(), 0);

	struct FFault
	{
		const TCHAR* Name;
		TFunction<void(FStageShowSnapshot&)> Break;
		EStageDiagnosticCode Code;
		EStageDiagnosticSeverity Severity;
	};
	const FGuid GonePalette = FGuid::NewGuid();
	TArray<FShowCue> BadCues = Cues;
	BadCues[0].Fixtures.Add(Entry(99, { { Dimmer, 1.f } }));
	BadCues[0].Fixtures[0].PaletteRefs.Add(Dimmer, GonePalette);
	TArray<FStagePlayback> BadPlaybacks = Playbacks;
	BadPlaybacks[4].Cues = BadCues;

	const TArray<FFault> Faults = {
		{ TEXT("Conflict"), [](FStageShowSnapshot& S) { S.ConflictingFixtureIds = { 2, 1, 2 }; }, EStageDiagnosticCode::PatchConflict, EStageDiagnosticSeverity::Error },
		{ TEXT("Unpatched with DMX OUT"), [](FStageShowSnapshot& S) { S.Fixtures[1].Patch = FStageDMXPatch(); S.OutputState = EStageDMXOutputState::Sending; }, EStageDiagnosticCode::Unpatched, EStageDiagnosticSeverity::Warning },
		{ TEXT("External, DMX IN off"), [](FStageShowSnapshot& S) { S.ControlSource = EStageControlSource::External; }, EStageDiagnosticCode::DMXInOff, EStageDiagnosticSeverity::Error },
		{ TEXT("DMX IN failed"), [](FStageShowSnapshot& S) { S.InputState = EStageDMXInputState::Failed; S.InputError = FText::FromString(TEXT("port 6454 in use")); }, EStageDiagnosticCode::DMXInFailed, EStageDiagnosticSeverity::Error },
		{ TEXT("External, universe silent"), [](FStageShowSnapshot& S) { S.ControlSource = EStageControlSource::External; S.InputState = EStageDMXInputState::Listening; S.LiveUniverses = { 2 }; }, EStageDiagnosticCode::DMXInNoSignal, EStageDiagnosticSeverity::Warning },
		{ TEXT("DMX OUT failed"), [](FStageShowSnapshot& S) { S.OutputState = EStageDMXOutputState::Failed; }, EStageDiagnosticCode::DMXOutFailed, EStageDiagnosticSeverity::Error },
		{ TEXT("Muted universe"), [](FStageShowSnapshot& S) { S.OutputState = EStageDMXOutputState::Sending; S.MutedUniverses = { 1 }; }, EStageDiagnosticCode::UniverseMuted, EStageDiagnosticSeverity::Warning },
		{ TEXT("Output paused"), [](FStageShowSnapshot& S) { S.ControlSource = EStageControlSource::External; S.InputState = EStageDMXInputState::Listening; S.LiveUniverses = { 1 }; S.OutputState = EStageDMXOutputState::Sending; }, EStageDiagnosticCode::OutputPausedUnderExternal, EStageDiagnosticSeverity::Info },
		{ TEXT("Effects held"), [](FStageShowSnapshot& S) { S.ControlSource = EStageControlSource::External; S.InputState = EStageDMXInputState::Listening; S.LiveUniverses = { 1 }; S.bEffectsRunning = true; }, EStageDiagnosticCode::EffectsHeldUnderExternal, EStageDiagnosticSeverity::Info },
		{ TEXT("Missing palette"), [&BadCues](FStageShowSnapshot& S) { S.Cues = BadCues; }, EStageDiagnosticCode::PaletteReferenceMissing, EStageDiagnosticSeverity::Warning },
		{ TEXT("Missing fixture"), [&BadCues](FStageShowSnapshot& S) { S.Cues = BadCues; }, EStageDiagnosticCode::CueNamesMissingFixture, EStageDiagnosticSeverity::Warning },
		{ TEXT("Blackout"), [](FStageShowSnapshot& S) { S.bBlackout = true; }, EStageDiagnosticCode::BlackoutOn, EStageDiagnosticSeverity::Warning },
		{ TEXT("Grand Master at 20%"), [](FStageShowSnapshot& S) { S.GrandMaster = 0.2f; }, EStageDiagnosticCode::GrandMasterLow, EStageDiagnosticSeverity::Warning },
		{ TEXT("Audio failed"), [](FStageShowSnapshot& S) { S.AudioState = EStageAudioSourceState::Failed; }, EStageDiagnosticCode::AudioSourceFailed, EStageDiagnosticSeverity::Error },
		{ TEXT("Audio effects without a source"), [](FStageShowSnapshot& S) { S.bAudioEffectsRunning = S.bEffectsRunning = true; }, EStageDiagnosticCode::AudioNoSource, EStageDiagnosticSeverity::Warning },
		{ TEXT("Audio effects hear nothing"), [](FStageShowSnapshot& S) { S.bAudioEffectsRunning = S.bEffectsRunning = true; S.AudioSource = EStageAudioSource::Input; S.AudioState = EStageAudioSourceState::Running; }, EStageDiagnosticCode::AudioNoSignal, EStageDiagnosticSeverity::Warning },
		{ TEXT("Audio killed"), [](FStageShowSnapshot& S) { S.bAudioEffectsRunning = S.bEffectsRunning = true; S.AudioSource = EStageAudioSource::TestSignal; S.AudioState = EStageAudioSourceState::Running; S.bAudioSignal = true; S.bAudioKill = true; }, EStageDiagnosticCode::AudioKilled, EStageDiagnosticSeverity::Info },
		{ TEXT("Programmer holds"), [](FStageShowSnapshot& S) { S.ProgrammerFixtureIds = { 2, 1 }; }, EStageDiagnosticCode::ProgrammerHolds, EStageDiagnosticSeverity::Info },
	};
	for (const FFault& Fault : Faults)
	{
		FStageShowSnapshot Snapshot = Clean();
		Fault.Break(Snapshot);
		const TArray<FStageDiagnostic> Found = StageShowDiagnostics::Run(Snapshot);
		const FStageDiagnostic* Diagnostic = FindCode(Found, Fault.Code);
		if (!TestNotNull(*FString::Printf(TEXT("%s: fires"), Fault.Name), Diagnostic))
		{
			continue;
		}
		TestEqual(*FString::Printf(TEXT("%s: severity"), Fault.Name), static_cast<int32>(Diagnostic->Severity), static_cast<int32>(Fault.Severity));
		TestTrue(*FString::Printf(TEXT("%s: worst first"), Fault.Name), Found[0].Severity >= Diagnostic->Severity);
		const FString English = StageShowDiagnostics::Describe(*Diagnostic, EStageAILanguage::English).ToString();
		TestFalse(*FString::Printf(TEXT("%s: described"), Fault.Name), English.IsEmpty());
		TestTrue(*FString::Printf(TEXT("%s: described in Armenian"), Fault.Name), HasArmenian(StageShowDiagnostics::Describe(*Diagnostic, EStageAILanguage::Armenian).ToString()));
		TestTrue(*FString::Printf(TEXT("%s: the same every run"), Fault.Name), StageShowDiagnostics::Run(Snapshot) == Found);
		FGameplayTag Panel;
		FName Control;
		if (TestTrue(*FString::Printf(TEXT("%s: has a place"), Fault.Name), StageUIAnchors::GetPlaceTarget(Diagnostic->Place, Panel, Control)))
		{
			TestNotNull(*FString::Printf(TEXT("%s: its place is a real anchor"), Fault.Name), StageUIAnchors::Find(Panel, Control));
		}
	}

	// Subjects and details.
	{
		FStageShowSnapshot Snapshot = Clean();
		Snapshot.ConflictingFixtureIds = { 2, 1, 2 };
		TestEqual(TEXT("Conflict subjects sorted, once each"), StageShowDiagnostics::Run(Snapshot)[0].FixtureIds, TArray<int32>({ 1, 2 }));

		Snapshot = Clean();
		Snapshot.Fixtures[1].Patch = FStageDMXPatch();
		TestNull(TEXT("Unpatched without DMX: a previs stage is not nagged"), FindCode(StageShowDiagnostics::Run(Snapshot), EStageDiagnosticCode::Unpatched));

		Snapshot = Clean();
		Snapshot.Cues = BadCues;
		const TArray<FStageDiagnostic> Found = StageShowDiagnostics::Run(Snapshot);
		const FStageDiagnostic* Missing = FindCode(Found, EStageDiagnosticCode::CueNamesMissingFixture);
		TestTrue(TEXT("Missing fixture 99 in cue 1"), Missing && Missing->FixtureIds == TArray<int32>({ 99 }) && Missing->Cues == TArray<float>({ 1.f }) && Missing->Playback == INDEX_NONE);
		TestTrue(TEXT("... described with both"), Missing && StageShowDiagnostics::Describe(*Missing, EStageAILanguage::English).ToString().Contains(TEXT("Cue 1 names fixtures that are not on stage (99)")));

		Snapshot = Clean();
		Snapshot.Playbacks = BadPlaybacks;
		const TArray<FStageDiagnostic> InPlaybacks = StageShowDiagnostics::Run(Snapshot);
		const FStageDiagnostic* InPlayback = FindCode(InPlaybacks, EStageDiagnosticCode::PaletteReferenceMissing);
		TestTrue(TEXT("A playback's cue: playback 5, its place"), InPlayback && InPlayback->Playback == 4 && InPlayback->Place == EStageDiagnosticPlace::Playback);

		Snapshot = Clean();
		Snapshot.GrandMaster = 0.21f;
		TestNull(TEXT("Grand Master above the low level"), FindCode(StageShowDiagnostics::Run(Snapshot), EStageDiagnosticCode::GrandMasterLow));

		Snapshot = Clean();
		Snapshot.ControlSource = EStageControlSource::External;
		Snapshot.InputState = EStageDMXInputState::Listening;
		Snapshot.LiveUniverses = { 1 };
		Snapshot.bAudioEffectsRunning = Snapshot.bEffectsRunning = true;
		TestNull(TEXT("Under External, audio effects hold: no audio finding"), FindCode(StageShowDiagnostics::Run(Snapshot), EStageDiagnosticCode::AudioNoSource));

		TestEqual(TEXT("Join: at most 8 numbers"), StageShowDiagnostics::JoinNumbers({ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 }, EStageAILanguage::English), FString(TEXT("1, 2, 3, 4, 5, 6, 7, 8 and 2 more")));
	}

	// 2. On a built world: the subsystem reads the live show and pushes changes once per frame.
	{
		FScopedWorld Scope;
		ALightingFixtureActor* A = Scope.SpawnMover();
		ALightingFixtureActor* B = Scope.SpawnMover();
		UShowControlSubsystem* Show = Scope.World->GetSubsystem<UShowControlSubsystem>();
		UStageShowDiagnosticsSubsystem* Diagnostics = Scope.World->GetSubsystem<UStageShowDiagnosticsSubsystem>();
		if (!TestNotNull(TEXT("A"), A) || !TestNotNull(TEXT("B"), B) || !TestNotNull(TEXT("Show"), Show) || !TestNotNull(TEXT("Diagnostics"), Diagnostics))
		{
			return false;
		}
		TestEqual(TEXT("A fresh stage: nothing"), Diagnostics->GetProblems().Num(), 0);

		int32 Broadcasts = 0;
		const FDelegateHandle Handle = Diagnostics->OnProblemsChanged.AddLambda([&Broadcasts]() { ++Broadcasts; });
		A->SetPatch(MakePatch(1, 1));
		B->SetPatch(MakePatch(1, 3));
		Show->SetBlackout(true);
		FTSTicker::GetCoreTicker().Tick(0.f);
		TestEqual(TEXT("Three changes in a frame: one broadcast"), Broadcasts, 1);
		const TArray<FStageDiagnostic>& Problems = Diagnostics->GetProblems();
		TestTrue(TEXT("Overlapping addresses found"), FindCode(Problems, EStageDiagnosticCode::PatchConflict) != nullptr);
		TestTrue(TEXT("Blackout found"), FindCode(Problems, EStageDiagnosticCode::BlackoutOn) != nullptr);
		TestEqual(TEXT("Worst first: the conflict"), static_cast<int32>(Problems[0].Code), static_cast<int32>(EStageDiagnosticCode::PatchConflict));

		FTSTicker::GetCoreTicker().Tick(0.f);
		TestEqual(TEXT("No change: no broadcast"), Broadcasts, 1);
		Show->SetGrandMaster(0.9f);
		FTSTicker::GetCoreTicker().Tick(0.f);
		TestEqual(TEXT("A change that finds nothing new: no broadcast"), Broadcasts, 1);

		FShowCue Named;
		Named.Number = 3.f;
		Named.Fixtures = { Entry(424242, { { Dimmer, 1.f } }) };
		Show->SetCues({ Named });
		Show->SetBlackout(false);
		FTSTicker::GetCoreTicker().Tick(0.f);
		TestEqual(TEXT("New findings: broadcast"), Broadcasts, 2);
		TestTrue(TEXT("Cue 3 names a missing fixture"), FindCode(Diagnostics->GetProblems(), EStageDiagnosticCode::CueNamesMissingFixture) != nullptr);
		TestNull(TEXT("Blackout gone"), FindCode(Diagnostics->GetProblems(), EStageDiagnosticCode::BlackoutOn));
		Diagnostics->OnProblemsChanged.Remove(Handle);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAssistantDiagnosticsTest, "StageCraft.Assistant.Diagnostics", StageDiagnosticsTests::Flags)

bool FStageAssistantDiagnosticsTest::RunTest(const FString& Parameters)
{
	using namespace StageDiagnosticsTests;

	// 1. The grammar, in English and Armenian.
	struct FAsk
	{
		const TCHAR* Prompt;
		EStageAIIntent Intent;
		int32 FixtureId;
	};
	const FAsk Asks[] = {
		{ TEXT("why is fixture 12 dark?"), EStageAIIntent::ExplainFixture, 12 },
		{ TEXT("Why is light 3 not responding"), EStageAIIntent::ExplainFixture, 3 },
		{ TEXT("why is light 3 off?"), EStageAIIntent::ExplainFixture, 3 },
		{ TEXT("explain fixture 7"), EStageAIIntent::ExplainFixture, 7 },
		{ TEXT("why is it dark"), EStageAIIntent::ExplainFixture, INDEX_NONE },
		{ TEXT("any problems?"), EStageAIIntent::ListProblems, INDEX_NONE },
		{ TEXT("what's wrong with my show"), EStageAIIntent::ListProblems, INDEX_NONE },
		{ TEXT("check the show"), EStageAIIntent::ListProblems, INDEX_NONE },
		{ TEXT("run diagnostics"), EStageAIIntent::ListProblems, INDEX_NONE },
		{ TEXT("ինչու է 12 սարքը մութ"), EStageAIIntent::ExplainFixture, 12 },
		{ TEXT("ինչու 5 լույսը չի վառվում"), EStageAIIntent::ExplainFixture, 5 },
		{ TEXT("ինչ խնդիրներ կան"), EStageAIIntent::ListProblems, INDEX_NONE },
		{ TEXT("ինչն է սխալ"), EStageAIIntent::ListProblems, INDEX_NONE },
	};
	for (const FAsk& Ask : Asks)
	{
		const TOptional<FStageAICommand> Command = StageAICommandParser::Parse(Ask.Prompt);
		if (!TestTrue(*FString::Printf(TEXT("\"%s\" parses"), Ask.Prompt), Command.IsSet()))
		{
			continue;
		}
		TestEqual(*FString::Printf(TEXT("\"%s\": intent"), Ask.Prompt), StageAICommandSchema::GetIntentName(Command->Intent), StageAICommandSchema::GetIntentName(Ask.Intent));
		TestEqual(*FString::Printf(TEXT("\"%s\": fixture"), Ask.Prompt), Command->FixtureId.Get(INDEX_NONE), Ask.FixtureId);
		TestTrue(*FString::Printf(TEXT("\"%s\": a question"), Ask.Prompt), Command->IsQuestion());
	}
	const auto IntentOf = [](const TCHAR* Prompt)
	{
		const TOptional<FStageAICommand> Command = StageAICommandParser::Parse(Prompt);
		return Command.IsSet() ? StageAICommandSchema::GetIntentName(Command->Intent) : FString();
	};
	TestNotEqual(TEXT("A why about no fixture is left to the help"), IntentOf(TEXT("why is a fixture dark")), StageAICommandSchema::GetIntentName(EStageAIIntent::ExplainFixture));
	TestNotEqual(TEXT("How to fix: the help"), IntentOf(TEXT("how do I fix a patch problem")), StageAICommandSchema::GetIntentName(EStageAIIntent::ListProblems));
	TestEqual(TEXT("Turning a light off still sets it"), IntentOf(TEXT("turn fixture 3 off")), StageAICommandSchema::GetIntentName(EStageAIIntent::SetFixture));

	// 2. The online tool: fixture_id is a whole number from 1.
	FStageAICommand FromTool;
	FText Error;
	TestTrue(TEXT("Tool: explain_fixture 12"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"explain_fixture\",\"fixture_id\":12}"), FromTool, Error)
		&& FromTool.Intent == EStageAIIntent::ExplainFixture && FromTool.FixtureId.Get(0) == 12);
	TestFalse(TEXT("Tool: fixture_id 0 refused"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"explain_fixture\",\"fixture_id\":0}"), FromTool, Error));
	TestFalse(TEXT("Tool: fixture_id 1.5 refused"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"explain_fixture\",\"fixture_id\":1.5}"), FromTool, Error));
	TestTrue(TEXT("Tool: list_problems"), StageAICommandSchema::CommandFromToolInputJson(TEXT("{\"intent\":\"list_problems\"}"), FromTool, Error) && FromTool.Intent == EStageAIIntent::ListProblems);
	TestTrue(TEXT("The tool definition offers both"), StageAICommandSchema::MakeToolDefinitionJson().Contains(TEXT("explain_fixture")) && StageAICommandSchema::MakeToolDefinitionJson().Contains(TEXT("fixture_id")));

	// 3. Answered from the live stage, read-only, with SHOW ME at the fix.
	FScopedWorld Scope;
	ALightingFixtureActor* A = Scope.SpawnMover();
	UShowControlSubsystem* Show = Scope.World->GetSubsystem<UShowControlSubsystem>();
	UStageAICommandSubsystem* Assistant = UWorld::GetSubsystem<UStageAICommandSubsystem>(Scope.World);
	if (!TestNotNull(TEXT("A"), A) || !TestNotNull(TEXT("Show"), Show) || !TestNotNull(TEXT("Assistant"), Assistant))
	{
		return false;
	}
	int32 Executed = 0;
	const auto Executor = [&Executed]()
	{
		return FStageAICommandExecutor::CreateLambda([&Executed](const FStageAICommand&)
		{
			++Executed;
			return FStageAIActionOutcome::Make(EStageAIOutcome::Succeeded, FText::FromString(TEXT("ran")));
		});
	};
	const auto Last = [Assistant]() { return Assistant->GetMessages().IsEmpty() ? FStageAIMessage() : Assistant->GetMessages().Last(); };
	A->SetAttribute(StageCraftTags::Attribute_Dimmer, 1.f);
	Show->SetBlackout(true);

	Assistant->SubmitPrompt(FString::Printf(TEXT("why is fixture %d dark?"), A->GetFixtureId()), Executor());
	TestTrue(TEXT("Explained: Blackout"), Last().Text.ToString().Contains(TEXT("BLACKOUT is on")));
	TestTrue(TEXT("... SHOW ME the BLACKOUT button"), Last().Reference.Panel == FGameplayTag(StageCraftTags::Panel_Playback) && Last().Reference.Control == TEXT("Blackout"));
	TestTrue(TEXT("... nothing changed"), Executed == 0 && Show->IsBlackout());

	Assistant->SubmitPrompt(FString::Printf(TEXT("ինչու է %d սարքը մութ"), A->GetFixtureId()), Executor());
	TestTrue(TEXT("Armenian: explained in Armenian"), HasArmenian(Last().Text.ToString()) && Last().Text.ToString().Contains(TEXT("BLACKOUT")));

	Assistant->SubmitPrompt(TEXT("why is it dark"), Executor());
	TestTrue(TEXT("No selection: asks which fixture"), Last().Text.ToString().Contains(TEXT("Which fixture")));

	Assistant->SubmitPrompt(TEXT("any problems?"), Executor());
	TestTrue(TEXT("Problems listed"), Last().Text.ToString().Contains(TEXT("BLACKOUT is on")));
	TestTrue(TEXT("... OPEN the Problems panel"), Last().Reference.Panel == FGameplayTag(StageCraftTags::Panel_Problems));
	Show->SetBlackout(false);
	Assistant->SubmitPrompt(TEXT("any problems?"), Executor());
	TestTrue(TEXT("Clean: says so"), Last().Text.ToString().Contains(TEXT("found no problems")));
	TestEqual(TEXT("Never runs anything"), Executed, 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
