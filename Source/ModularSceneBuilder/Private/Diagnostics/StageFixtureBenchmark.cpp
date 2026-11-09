// Copyright Epic Games, Inc. All Rights Reserved.

// Fixture stress benchmark (Docs/ADR/0012-production-reports-and-fixture-benchmark.md §3): spawns N lighting fixtures in the
// running game world, records two cues, and measures frame, game thread, render thread, GPU and show-control tick time while
// idle and while crossfading continuously; then removes everything it added. Development builds only (compiled out of Shipping).
//
//   StageCraft.Bench.Fixtures 50 200 500 [Seconds=10] [Fade=2] [Settle=3] [Zoom=1] [Item=<catalog asset name>] [GM=1]
//   (GM below 1 measures fades mixed in full every frame, as with a playback up or the Grand Master down; Docs/ADR/0026 §6.)
//   (Effects=1 runs a circle and a dimmer sine on every bench fixture through both phases, Effects=2 adds a rainbow, Effects=3 stores them in the two cues instead (circle and dimmer sine in A, a bigger
//   circle, dimmer sine and rainbow in B), so every crossfade fades, morphs and starts or stops effects; Docs/ADR/0027 §5, §7.)
//   (DMXOut=1 also sends every bench universe as Art-Net and sACN unicast to 127.0.0.1 on unused ports; Docs/ADR/0019 §9.7.)
//   (Audio=1, with Effects, makes those effects follow the test signal at 128 BPM: the circle synced, the dimmer sine modulated by the bass,
//   the rainbow synced to the bar; needs the audio source off; Docs/ADR/0029 §8.)
//   (Crowd=N also stands an audience area of N people (at most 20000) under the fixtures for every run: the GPU cost of the crowd of
//   Docs/ADR/0031 §6 measured with the show. Set the camera first, e.g. StageCraft.View.Camera, so the crowd is in view.)
//   (Zoom=0 keeps the zoom fixed: a cone change rebuilds the engine's light proxy, so zoom fades are measured apart.)
//   StageCraft.Bench.Cancel
//
// Results go to the log ("StageCraft.Bench:") and to Saved/StageCraft/Benchmarks/FixtureBench_<time>.csv.
// Scripts/Bench/RunFixtureBench.ps1 runs it in a Standalone window.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Actors/StageAudienceActor.h"
#include "Actors/LightingFixtureActor.h"
#include "Components/SpawnSystemComponent.h"
#include "Containers/Ticker.h"
#include "Data/StageAudienceData.h"
#include "Data/LightingFixtureData.h"
#include "Data/StageParameterTypes.h"
#include "Diagnostics/StageFrameStats.h"
#include "DynamicRHI.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Fixtures/StageFixtureLibrarySubsystem.h"
#include "Import/StageMeshImportSubsystem.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "IO/StageCraftPaths.h"
#include "Misc/Paths.h"
#include "ProfilingDebugging/MiscTrace.h"
#include "ModularSceneBuilder.h"
#include "Player/ModularPlayerController.h"
#include "RenderTimer.h"
#include "Show/StageNetworkDMXBridge.h"
#include "Subsystems/ShowControlSubsystem.h"
#include "Subsystems/StageAudioReactiveSubsystem.h"
#include "Subsystems/StageDMXPatchSubsystem.h"
#include "UObject/StrongObjectPtr.h"
#include "Subsystems/StageProgrammerSubsystem.h"
#include "Subsystems/StageItemSubsystem.h"
#include "Subsystems/StageSessionSubsystem.h"

namespace StageFixtureBenchmark
{
	/** A crossfade frame is dropped when it is longer than the 60 Hz budget and this many times the idle median. */
	constexpr double FrameBudgetMs = 1000.0 / 60.0;
	constexpr double DropFactor = 1.5;
	/** Cue numbers far above any normal show, removed again with the rest of the run. */
	constexpr float CueA = 9001.f;
	constexpr float CueB = 9002.f;
	/** DMXOut=1 sends here on 127.0.0.1: ports no DMX tool listens on, so a visualiser on this PC never sees bench frames. */
	constexpr uint16 BenchArtNetPort = 26454;
	constexpr uint16 BenchSACNPort = 25568;
	/** Grid of fixtures: 1 m apart at 6 m, the height of a typical truss. */
	constexpr double GridSpacingCm = 100.0;
	constexpr double GridHeightCm = 600.0;

	struct FSettings
	{
		TArray<int32> Counts;
		double PhaseSeconds = 10.0;
		double FadeSeconds = 2.0;
		double SettleSeconds = 3.0;
		/** Whether the two looks differ in zoom. A cone change rebuilds the engine's light proxy, so zoom fades cost far more than the rest. */
		bool bFadeZoom = true;
		FString ItemName;
		/**
		 * The Grand Master during the run (Docs/ADR/0026 §6). Below 1 every fade frame is mixed in full (HTP, LTP, masters) instead of
		 * written straight onto the fixtures, so GM=0.9 measures that path. Put back to what it was afterwards.
		 */
		double GrandMaster = 1.0;
		/** Effects on every bench fixture during both phases (Docs/ADR/0027 §7): 0 none, 1 circle + dimmer sine, 2 also a rainbow, 3 held by the cues. */
		int32 Effects = 0;
		/**
		 * DMXOut=1 sends every bench universe as Art-Net and sACN unicast to 127.0.0.1 on unused ports while measuring (Docs/ADR/0019
		 * §9.7): the show renders each changed universe and the sender thread sends it. Refused while the user has DMX IN or OUT on.
		 */
		bool bDMXOut = false;
		/**
		 * Audio=1 (with Effects 1..3) makes the bench effects follow the music (Docs/ADR/0029 §8): the circle syncs to every two beats,
		 * the dimmer sine is modulated by the bass, the rainbow syncs to the bar, while the test signal plays at 128 BPM. Refused
		 * unless the audio source is off; it is switched off again at the end.
		 */
		bool bAudio = false;
		/** Crowd=N: an audience area of N people (density 4 per m²) under the fixtures for every run; 0 for none. */
		int32 Crowd = 0;
	};

	/** Per-frame samples of one measured phase, in milliseconds. */
	struct FPhaseSamples
	{
		TArray<double> Frame;
		TArray<double> Game;
		TArray<double> Gpu;
		UShowControlSubsystem::FTickCostStats ShowTick;
	};

	struct FRunResult
	{
		int32 RequestedFixtures = 0;
		int32 SpawnedFixtures = 0;
		int32 CrowdPeople = 0;
		FPhaseSamples Idle;
		FPhaseSamples Crossfade;
		int32 CuesStarted = 0;
	};

	/** The steps of one run; Finished means every count is done or the run was cancelled. */
	enum class EStep : uint8
	{
		Spawn,
		Settle,
		Idle,
		Crossfade,
		Cleanup,
		Finished
	};

	FString JoinCounts(const TArray<int32>& Counts)
	{
		FString Out;
		for (const int32 Count : Counts)
		{
			Out += (Out.IsEmpty() ? TEXT("") : TEXT(" ")) + FString::FromInt(Count);
		}
		return Out;
	}

	/**
	 * One benchmark session over Settings.Counts, driven by the core ticker while it exists. It owns nothing in the world beyond
	 * the fixtures it spawned and the two cues it stored, and removes both in Cleanup (also when cancelled). It holds the world
	 * weakly: if the world goes away mid-run, the run ends without touching it.
	 */
	class FBenchmark
	{
	public:
		FBenchmark(UWorld& InWorld, FSettings InSettings)
			: World(&InWorld)
			, Settings(MoveTemp(InSettings))
		{
		}

		/** Advances the run; false when it has finished and can be destroyed. */
		bool Tick(float DeltaTime)
		{
			if (!World.IsValid())
			{
				UE_LOG(LogStageCraft, Warning, TEXT("StageCraft.Bench: the world went away; the run ended without cleanup."));
				return false;
			}

			StepSeconds += DeltaTime;
			switch (Step)
			{
			case EStep::Spawn:
				SpawnFixtures();
				break;
			case EStep::Settle:
				if (StepSeconds >= Settings.SettleSeconds && !IsWaitingForImports())
				{
					StoreCues();
					BeginPhase(EStep::Idle);
				}
				break;
			case EStep::Idle:
				Sample(Current.Idle, DeltaTime);
				if (StepSeconds >= Settings.PhaseSeconds)
				{
					EndPhase(Current.Idle);
					BeginPhase(EStep::Crossfade);
				}
				break;
			case EStep::Crossfade:
				Sample(Current.Crossfade, DeltaTime);
				KeepFading();
				if (StepSeconds >= Settings.PhaseSeconds)
				{
					EndPhase(Current.Crossfade);
					Results.Add(Current);
					Step = EStep::Cleanup;
				}
				break;
			case EStep::Cleanup:
				Cleanup();
				++CountIndex;
				if (CountIndex < Settings.Counts.Num())
				{
					SetStep(EStep::Spawn);
				}
				else
				{
					CloseBenchOutput();
					StopBenchAudio();
					Report();
					SetStep(EStep::Finished);
				}
				break;
			case EStep::Finished:
				return false;
			}
			return Step != EStep::Finished;
		}

		/** Skips to cleanup; the results gathered so far are still reported. */
		void Cancel()
		{
			if (Step != EStep::Finished)
			{
				UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Bench: cancelled."));
				Cleanup();
				CloseBenchOutput();
				StopBenchAudio();
				Report();
				SetStep(EStep::Finished);
			}
		}

		/** Captures what the run changes outside its own fixtures and cues; false (with a log line) when it cannot run. */
		bool Prepare()
		{
			UShowControlSubsystem* Show = UWorld::GetSubsystem<UShowControlSubsystem>(World.Get());
			const UStageSessionSubsystem* Session = UWorld::GetSubsystem<UStageSessionSubsystem>(World.Get());
			AModularPlayerController* Controller = Cast<AModularPlayerController>(World->GetFirstPlayerController());
			if (!Show || !Session || !Controller || !Controller->GetSpawnSystem())
			{
				UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Bench: needs a stage world with show control, a session and a StageCraft player controller."));
				return false;
			}
			if (Show->GetControlSource() != EStageControlSource::Internal)
			{
				UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Bench: the control source is External; switch to Internal to play cues."));
				return false;
			}

			FixtureData = FindFixtureData();
			if (!FixtureData.IsValid())
			{
				UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Bench: no lighting fixture in the catalog%s."),
					Settings.ItemName.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" named %s"), *Settings.ItemName));
				return false;
			}
			if (Settings.bAudio && !StartBenchAudio())
			{
				return false;
			}
			if (Settings.bDMXOut && !OpenBenchOutput(*Show))
			{
				StopBenchAudio();
				return false;
			}

			SavedCues = Show->GetCues();
			SavedActiveCue = Show->GetActiveCueNumber();
			for (ALightingFixtureActor* Fixture : Show->GetFixtures())
			{
				SavedLooks.Add(Fixture, Fixture->GetAttributes());
			}
			bSessionWasDirty = Session->IsDirty();
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Bench: %s fixtures of %s, %.0f s idle + %.0f s crossfading (%.1f s fades, zoom %s) each, RHI %s."),
				*JoinCounts(Settings.Counts), *FixtureData->GetName(), Settings.PhaseSeconds, Settings.PhaseSeconds, Settings.FadeSeconds,
				Settings.bFadeZoom ? TEXT("fading") : TEXT("fixed"),
				GDynamicRHI ? GDynamicRHI->GetName() : TEXT("none"));
			return true;
		}

	private:
		/** Audio=1: the test signal as the source. Only from Off, so nothing of the user's is replaced. */
		bool StartBenchAudio()
		{
			UStageAudioReactiveSubsystem* Audio = UWorld::GetSubsystem<UStageAudioReactiveSubsystem>(World.Get());
			if (Settings.Effects <= 0)
			{
				UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Bench: Audio=1 needs Effects=1, 2 or 3 (the effects that follow the music)."));
				return false;
			}
			if (!Audio || Audio->GetState() != EStageAudioSourceState::Off)
			{
				UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Bench: Audio=1 needs the audio source off; switch it off in the Audio panel first."));
				return false;
			}
			FText Error;
			if (!Audio->SetSource(EStageAudioSource::TestSignal, Error))
			{
				UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Bench: Audio=1 could not start the test signal: %s"), *Error.ToString());
				return false;
			}
			bBenchAudio = true;
			return true;
		}

		/** Logs what the analyser found and switches the source off again. */
		void StopBenchAudio()
		{
			if (!bBenchAudio)
			{
				return;
			}
			bBenchAudio = false;
			UStageAudioReactiveSubsystem* Audio = World.IsValid() ? UWorld::GetSubsystem<UStageAudioReactiveSubsystem>(World.Get()) : nullptr;
			if (!Audio)
			{
				return;
			}
			const FStageAudioStatus Status = Audio->GetStatus();
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Bench: audio test signal at %.2f BPM (confidence %.2f), analyser %.2f %% of a core, %lld samples dropped."),
				Status.Features.BPM, Status.Features.Confidence, Status.AnalyzerCpuPercent, Status.DroppedSamples);
			FText Error;
			Audio->SetSource(EStageAudioSource::Off, Error);
		}

		/** Audio=1: Effect made to follow the music the way the bench measures it (Docs/ADR/0029 §8). */
		static void ApplyBenchAudio(FStageEffect& Effect)
		{
			switch (Effect.Target)
			{
			case EStageEffectTarget::Position:
				Effect.Audio.Mode = EStageEffectAudioMode::Sync;
				Effect.Audio.BeatsPerCycle = EStageEffectBeatDivision::Two;
				break;
			case EStageEffectTarget::Intensity:
				Effect.Audio.Mode = EStageEffectAudioMode::Modulate;
				Effect.Audio.Band = EStageAudioBand::Bass;
				break;
			default:
				Effect.Audio.Mode = EStageEffectAudioMode::Sync;
				Effect.Audio.BeatsPerCycle = EStageEffectBeatDivision::Four;
				break;
			}
		}

		/** DMXOut=1: a bench-owned bridge that sends unicast to this PC on ports nothing listens on. The user's own transport is never replaced. */
		bool OpenBenchOutput(UShowControlSubsystem& Show)
		{
			if (Show.GetDMXBridge())
			{
				UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Bench: DMXOut=1 needs DMX IN and OUT off (a transport is installed); switch them off first."));
				return false;
			}
			FStageDMXSenderConfig Config;
			Config.Settings.ArtNet = EStageArtNetOutputMode::Unicast;
			Config.Settings.SACN = EStageSACNOutputMode::Unicast;
			Config.Settings.UnicastAddress = TEXT("127.0.0.1");
			Config.ArtNetPort = BenchArtNetPort;
			Config.SACNPort = BenchSACNPort;
			Config.SourceName = TEXT("StageCraft Bench");
			BenchBridge.Reset(NewObject<UStageNetworkDMXBridge>(&Show));
			FText Error;
			if (!BenchBridge->OpenOutput(Config, Error))
			{
				UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Bench: DMXOut=1 could not open the output: %s"), *Error.ToString());
				BenchBridge.Reset();
				return false;
			}
			Show.SetDMXBridge(BenchBridge.Get());
			return true;
		}

		/** Every patched universe (the bench fixtures were just auto-patched) is sent from now on. */
		void UpdateBenchOutput(UShowControlSubsystem& Show) const
		{
			const UStageDMXPatchSubsystem* Patch = UWorld::GetSubsystem<UStageDMXPatchSubsystem>(World.Get());
			if (BenchBridge && Patch)
			{
				BenchBridge->SetOutputUniverses(Patch->GetPatchedUniverses());
				Show.RefreshDMXOutput();
			}
		}

		/** Logs what the bench output sent and removes it from the show (which closes it). */
		void CloseBenchOutput()
		{
			if (!BenchBridge)
			{
				return;
			}
			const FStageDMXOutputStatus Status = BenchBridge->GetOutputStatus();
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Bench: DMX output sent %lld Art-Net and %lld sACN packets (%lld send errors)."),
				Status.ArtNetPackets, Status.SACNPackets, Status.SendErrors);
			UShowControlSubsystem* Show = World.IsValid() ? UWorld::GetSubsystem<UShowControlSubsystem>(World.Get()) : nullptr;
			if (Show && Show->GetDMXBridge() == BenchBridge.Get())
			{
				Show->SetDMXBridge(nullptr);
			}
			else
			{
				BenchBridge->Stop();
			}
			BenchBridge.Reset();
		}

		void SetStep(EStep NewStep)
		{
			Step = NewStep;
			StepSeconds = 0.0;
		}

		ULightingFixtureData* FindFixtureData() const
		{
			const UGameInstance* GameInstance = World->GetGameInstance();
			const UStageItemSubsystem* Items = GameInstance ? GameInstance->GetSubsystem<UStageItemSubsystem>() : nullptr;
			if (!Items)
			{
				return nullptr;
			}
			for (UBaseItemData* Item : Items->GetCatalog())
			{
				ULightingFixtureData* Fixture = Cast<ULightingFixtureData>(Item);
				if (Fixture && (Settings.ItemName.IsEmpty() || Fixture->GetName() == Settings.ItemName))
				{
					return Fixture;
				}
			}
			return nullptr;
		}

		/**
		 * A model import (the startup re-import of the user's models included) runs Interchange on the game thread and would be
		 * measured as fixture cost, so measuring waits for the queue to empty; logged once per wait.
		 */
		bool IsWaitingForImports()
		{
			const UGameInstance* GameInstance = World->GetGameInstance();
			const UStageMeshImportSubsystem* Imports = GameInstance ? GameInstance->GetSubsystem<UStageMeshImportSubsystem>() : nullptr;
			const UStageFixtureLibrarySubsystem* Fixtures = GameInstance ? GameInstance->GetSubsystem<UStageFixtureLibrarySubsystem>() : nullptr;
			const int32 Pending = (Imports ? Imports->GetPendingCount() : 0) + (Fixtures && Fixtures->HasLoadsInProgress() ? 1 : 0);
			if (Pending > 0 && !bLoggedImportWait)
			{
				UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Bench: waiting for %d model or fixture import(s) to finish before measuring."), Pending);
			}
			bLoggedImportWait = Pending > 0;
			return Pending > 0;
		}

		void SpawnFixtures()
		{
			Current = FRunResult();
			Current.RequestedFixtures = Settings.Counts[CountIndex];

			AModularPlayerController* Controller = Cast<AModularPlayerController>(World->GetFirstPlayerController());
			USpawnSystemComponent* Spawner = Controller ? Controller->GetSpawnSystem() : nullptr;
			const int32 Columns = FMath::Max(1, FMath::CeilToInt32(FMath::Sqrt(static_cast<double>(Current.RequestedFixtures))));
			const double HalfWidth = (Columns - 1) * GridSpacingCm * 0.5;
			for (int32 Index = 0; Spawner && Index < Current.RequestedFixtures; ++Index)
			{
				const FVector Location((Index % Columns) * GridSpacingCm - HalfWidth, (Index / Columns) * GridSpacingCm - HalfWidth, GridHeightCm);
				// Through the same path as a user placement: rules, registration, auto-patch.
				if (ALightingFixtureActor* Fixture = Cast<ALightingFixtureActor>(Spawner->SpawnItem(FixtureData.Get(), FTransform(Location))))
				{
					Spawned.Add(Fixture);
				}
			}
			Current.SpawnedFixtures = Spawned.Num();
			if (Settings.Crowd > 0 && Spawner)
			{
				SpawnCrowd(*Spawner);
			}
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Bench: spawned %d of %d fixtures; settling %.0f s."), Current.SpawnedFixtures, Current.RequestedFixtures, Settings.SettleSeconds);
			SetStep(EStep::Settle);
		}

		/** Crowd=N: one audience area at 4 people per m², 1.6 times as wide as deep, centred under the fixture grid. */
		void SpawnCrowd(USpawnSystemComponent& Spawner)
		{
			const UGameInstance* GameInstance = World->GetGameInstance();
			const UStageItemSubsystem* Items = GameInstance ? GameInstance->GetSubsystem<UStageItemSubsystem>() : nullptr;
			UStageAudienceData* AudienceData = nullptr;
			for (UBaseItemData* Item : Items ? Items->GetCatalog() : TArray<UBaseItemData*>())
			{
				AudienceData = AudienceData ? AudienceData : Cast<UStageAudienceData>(Item);
			}
			if (!AudienceData)
			{
				UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Bench: Crowd needs an audience item in the catalog; measuring without one."));
				return;
			}
			constexpr double Density = 4.0;
			const double DepthCm = FMath::Sqrt(Settings.Crowd / Density / 1.6) * 100.0;
			const double WidthCm = DepthCm * 1.6;
			AStageAudienceActor* Area = Cast<AStageAudienceActor>(Spawner.SpawnItem(AudienceData, FTransform(FVector(-DepthCm * 0.5, 0.0, 0.0))));
			if (!Area)
			{
				UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Bench: the crowd could not be placed; measuring without one."));
				return;
			}
			for (const TPair<FGameplayTag, double>& Setting : { TPair<FGameplayTag, double>(StageCraftTags::Param_Audience_Density, Density),
				TPair<FGameplayTag, double>(StageCraftTags::Param_Audience_Width, WidthCm), TPair<FGameplayTag, double>(StageCraftTags::Param_Audience_Depth, DepthCm) })
			{
				IStageParameterInterface::Execute_SetParameterValue(Area, Setting.Key, FStageParameterValue::MakeFloat(Setting.Value));
			}
			CrowdArea = Area;
			Current.CrowdPeople = Area->GetPeopleCount();
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Bench: crowd of %d people (%.1f x %.1f m)."), Current.CrowdPeople, WidthCm / 100.0, DepthCm / 100.0);
		}

		void ApplyLook(bool bBright)
		{
			TMap<FGameplayTag, float> Look;
			Look.Add(StageCraftTags::Attribute_Dimmer, bBright ? 1.f : 0.2f);
			Look.Add(StageCraftTags::Attribute_ColorR, bBright ? 0.2f : 1.f);
			Look.Add(StageCraftTags::Attribute_ColorG, bBright ? 0.4f : 0.5f);
			Look.Add(StageCraftTags::Attribute_ColorB, bBright ? 1.f : 0.1f);
			Look.Add(StageCraftTags::Attribute_Pan, bBright ? 30.f : -30.f);
			Look.Add(StageCraftTags::Attribute_Tilt, bBright ? 60.f : 20.f);
			Look.Add(StageCraftTags::Attribute_Zoom, bBright && Settings.bFadeZoom ? 35.f : 15.f);
			for (const TWeakObjectPtr<ALightingFixtureActor>& Fixture : Spawned)
			{
				if (ALightingFixtureActor* Live = Fixture.Get())
				{
					Live->SetAttributes(Look);
				}
			}
		}

		void StoreCues()
		{
			UShowControlSubsystem* Show = UWorld::GetSubsystem<UShowControlSubsystem>(World.Get());
			UStageProgrammerSubsystem* Programmer = UWorld::GetSubsystem<UStageProgrammerSubsystem>(World.Get());
			const bool bCueEffects = Settings.Effects == 3 && Programmer;
			ApplyLook(false);
			Show->StoreCue(CueA, FText::FromString(TEXT("Bench A")), Settings.FadeSeconds);
			ApplyLook(true);
			Show->StoreCue(CueB, FText::FromString(TEXT("Bench B")), Settings.FadeSeconds);
			if (bCueEffects)
			{
				// The looks are stored first: a Look store with effects running records the static look under them, and ApplyLook
				// writes the fixtures directly, so both cues would hold the same look and every Go would finish at once. The effects
				// then go into the cues with Track stores, which keep each cue's values.
				StartEffects();
				Show->StoreCue(CueA, FText::GetEmpty(), Settings.FadeSeconds, EStageCueStoreMode::Track);
				// Cue B: the same circle bigger (a morph on every Go) and a rainbow that starts and stops.
				for (const FStageEffectEntry& Entry : TArray<FStageEffectEntry>(Programmer->GetEffects()))
				{
					if (Entry.Effect.Target == EStageEffectTarget::Position)
					{
						FStageEffect Bigger = Entry.Effect;
						Bigger.Size *= 2.f;
						Programmer->UpdateEffect(Entry.Id, Bigger);
					}
				}
				StartEffect(*Programmer, TEXT("Rainbow"));
				Show->StoreCue(CueB, FText::GetEmpty(), Settings.FadeSeconds, EStageCueStoreMode::Track);
				Programmer->SetEffectState(FStageProgrammerEffects()); // From here the cues run them.
			}
			// Idle starts from a settled look with no fade running.
			Show->GoToCue(CueA);
			Show->FinishFade();
			SavedGrandMaster = Show->GetGrandMaster();
			Show->SetGrandMaster(static_cast<float>(Settings.GrandMaster));
			if (!bCueEffects)
			{
				StartEffects();
			}
			UpdateBenchOutput(*Show);
		}

		/** Starts the quick effect Name on every bench fixture. */
		void StartEffect(UStageProgrammerSubsystem& Programmer, const TCHAR* Name) const
		{
			TArray<int32> FixtureIds;
			for (const TWeakObjectPtr<ALightingFixtureActor>& Fixture : Spawned)
			{
				if (const ALightingFixtureActor* Live = Fixture.Get())
				{
					FixtureIds.Add(Live->GetFixtureId());
				}
			}
			const TArray<FStageEffect> Quick = StageEffectRules::MakeQuickEffects();
			if (const FStageEffect* Effect = Quick.FindByPredicate([Name](const FStageEffect& Candidate) { return Candidate.Name.ToString() == Name; }))
			{
				FStageEffect Started = *Effect;
				if (Settings.bAudio)
				{
					ApplyBenchAudio(Started);
				}
				Programmer.AddEffect(Started, FixtureIds);
			}
		}

		/**
		 * Effects=1|2|3: the programmer runs quick effects on every bench fixture (with 3 only while the cues are stored); the user's
		 * own effects and stops come back in Cleanup.
		 */
		void StartEffects()
		{
			UStageProgrammerSubsystem* Programmer = UWorld::GetSubsystem<UStageProgrammerSubsystem>(World.Get());
			if (Settings.Effects <= 0 || !Programmer)
			{
				return;
			}
			SavedEffects = Programmer->GetEffectState();
			Programmer->SetEffectState(FStageProgrammerEffects());
			StartEffect(*Programmer, TEXT("Circle"));
			StartEffect(*Programmer, TEXT("Dimmer Sine"));
			if (Settings.Effects == 2)
			{
				StartEffect(*Programmer, TEXT("Rainbow"));
			}
		}

		/** "Bench 500 crossfade": a timing region in an Unreal Insights trace (-trace=cpu,frame), so a profile can be cut to one phase. */
		FString GetRegionName() const
		{
			return FString::Printf(TEXT("Bench %d %s"), Current.RequestedFixtures, Step == EStep::Crossfade ? TEXT("crossfade") : TEXT("idle"));
		}

		void BeginPhase(EStep Phase)
		{
			SetStep(Phase);
			TRACE_BEGIN_REGION(*GetRegionName());
			UShowControlSubsystem* Show = UWorld::GetSubsystem<UShowControlSubsystem>(World.Get());
			Show->ResetTickCostStats();
			if (Phase == EStep::Crossfade)
			{
				KeepFading();
			}
		}

		void EndPhase(FPhaseSamples& Samples)
		{
			TRACE_END_REGION(*GetRegionName());
			const UShowControlSubsystem* Show = UWorld::GetSubsystem<UShowControlSubsystem>(World.Get());
			Samples.ShowTick = Show->GetTickCostStats();
		}

		/** Continuous crossfades: the next Go as soon as the previous fade completed. */
		void KeepFading()
		{
			UShowControlSubsystem* Show = UWorld::GetSubsystem<UShowControlSubsystem>(World.Get());
			if (!Show->IsFading())
			{
				const bool bToB = FMath::IsNearlyEqual(Show->GetActiveCueNumber(), CueA);
				Show->GoToCue(bToB ? CueB : CueA);
				++Current.CuesStarted;
			}
		}

		void Sample(FPhaseSamples& Samples, float DeltaTime)
		{
			// The stat unit sources: the previous frame's game thread and GPU time. The render thread counter (GRenderThreadTime)
			// is measured per Slate window present and reads 0 in this multi-window workspace, so it is not sampled.
			Samples.Frame.Add(DeltaTime * 1000.0);
			Samples.Game.Add(FPlatformTime::ToMilliseconds(GGameThreadTime));
			Samples.Gpu.Add(FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles(0)));
		}

		/** Removes the run's fixtures and gives the user's show back: cue list, active cue and every fixture's look. */
		void Cleanup()
		{
			UShowControlSubsystem* Show = UWorld::GetSubsystem<UShowControlSubsystem>(World.Get());
			if (Show)
			{
				Show->FinishFade();
			}

			AModularPlayerController* Controller = Cast<AModularPlayerController>(World->GetFirstPlayerController());
			USpawnSystemComponent* Spawner = Controller ? Controller->GetSpawnSystem() : nullptr;
			for (const TWeakObjectPtr<ALightingFixtureActor>& Fixture : Spawned)
			{
				ALightingFixtureActor* Live = Fixture.Get();
				if (Live && !(Spawner && Spawner->TryDeleteActor(Live)))
				{
					Live->Destroy();
				}
			}
			Spawned.Reset();
			if (AStageAudienceActor* Area = CrowdArea.Get())
			{
				if (!(Spawner && Spawner->TryDeleteActor(Area)))
				{
					Area->Destroy();
				}
			}
			CrowdArea.Reset();

			if (Show)
			{
				if (SavedGrandMaster.IsSet())
				{
					Show->SetGrandMaster(SavedGrandMaster.GetValue());
					SavedGrandMaster.Reset();
				}
				if (SavedEffects.IsSet())
				{
					if (UStageProgrammerSubsystem* Programmer = UWorld::GetSubsystem<UStageProgrammerSubsystem>(World.Get()))
					{
						Programmer->SetEffectState(SavedEffects.GetValue());
					}
					SavedEffects.Reset();
				}
				const bool bHadActiveCue = SavedActiveCue >= 0.f && SavedCues.ContainsByPredicate([this](const FShowCue& Cue) { return FMath::IsNearlyEqual(Cue.Number, SavedActiveCue); });
				if (bHadActiveCue)
				{
					// Back to the cue the user was on, so GO continues from there.
					Show->ReplaceCues(SavedCues);
					Show->GoToCue(SavedActiveCue);
					Show->FinishFade();
				}
				else
				{
					// No cue was active: the list as loaded, nothing active, nothing fading.
					Show->SetCues(SavedCues);
				}
			}
			// The bench cues recorded the user's fixtures too; their exact look before the run comes last.
			for (const TPair<TWeakObjectPtr<ALightingFixtureActor>, TMap<FGameplayTag, float>>& Look : SavedLooks)
			{
				if (ALightingFixtureActor* Fixture = Look.Key.Get())
				{
					Fixture->SetAttributes(Look.Value);
				}
			}
			if (UStageSessionSubsystem* Session = UWorld::GetSubsystem<UStageSessionSubsystem>(World.Get()); Session && !bSessionWasDirty)
			{
				Session->MarkClean();
			}
		}

		static FString Describe(const FStageFrameStatsSummary& Stats)
		{
			return FString::Printf(TEXT("mean %6.2f  p50 %6.2f  p95 %6.2f  p99 %6.2f  max %7.2f"), Stats.Mean, Stats.Median, Stats.P95, Stats.P99, Stats.Max);
		}

		void Report() const
		{
			FString Csv = TEXT("Fixtures,Crowd,Phase,Frames,FrameMeanMs,FrameP50Ms,FrameP95Ms,FrameP99Ms,FrameMaxMs,GameMeanMs,GameP95Ms,GpuMeanMs,GpuP95Ms,ShowTicks,ShowTickMeanMs,ShowTickMaxMs,DroppedFrames,CuesStarted\r\n");
			for (const FRunResult& Result : Results)
			{
				const FStageFrameStatsSummary IdleFrame = StageFrameStats::Summarize(Result.Idle.Frame);
				for (int32 PhaseIndex = 0; PhaseIndex < 2; ++PhaseIndex)
				{
					const bool bCrossfade = PhaseIndex == 1;
					const FPhaseSamples& Samples = bCrossfade ? Result.Crossfade : Result.Idle;
					const FStageFrameStatsSummary Frame = StageFrameStats::Summarize(Samples.Frame);
					const FStageFrameStatsSummary Game = StageFrameStats::Summarize(Samples.Game);
					const FStageFrameStatsSummary Gpu = StageFrameStats::Summarize(Samples.Gpu);
					const double ShowMeanMs = Samples.ShowTick.Ticks > 0 ? Samples.ShowTick.TotalSeconds / Samples.ShowTick.Ticks * 1000.0 : 0.0;
					const double ShowMaxMs = Samples.ShowTick.MaxSeconds * 1000.0;
					const int32 Dropped = StageFrameStats::CountDroppedFrames(Samples.Frame, FrameBudgetMs, IdleFrame.Median, DropFactor);
					const TCHAR* PhaseName = bCrossfade ? TEXT("crossfade") : TEXT("idle");

					UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Bench: %4d fixtures %5d people %-9s %5d frames | frame %s | game %6.2f / p95 %6.2f | GPU %6.2f / p95 %6.2f | show tick %5d x %.3f ms (max %.3f) | dropped %d"),
						Result.SpawnedFixtures, Result.CrowdPeople, PhaseName, Frame.Count, *Describe(Frame), Game.Mean, Game.P95, Gpu.Mean, Gpu.P95,
						Samples.ShowTick.Ticks, ShowMeanMs, ShowMaxMs, Dropped);
					Csv += FString::Printf(TEXT("%d,%d,%s,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%.4f,%.4f,%d,%d\r\n"),
						Result.SpawnedFixtures, Result.CrowdPeople, PhaseName, Frame.Count, Frame.Mean, Frame.Median, Frame.P95, Frame.P99, Frame.Max, Game.Mean, Game.P95,
						Gpu.Mean, Gpu.P95, Samples.ShowTick.Ticks, ShowMeanMs, ShowMaxMs, bCrossfade ? Dropped : 0, bCrossfade ? Result.CuesStarted : 0);
				}
			}

			const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::Combine(StageCraftPaths::DataRoot(), TEXT("Benchmarks")));
			const FString Path = Directory / FString::Printf(TEXT("FixtureBench_%s.csv"), *FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S")));
			IFileManager::Get().MakeDirectory(*Directory, /*Tree*/ true);
			const bool bWritten = FFileHelper::SaveStringToFile(Csv, *Path);
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Bench: done (%d runs). Dropped = crossfade frames over max(%.1f ms, %.1f x idle median). %s %s"),
				Results.Num(), FrameBudgetMs, DropFactor, bWritten ? TEXT("Wrote") : TEXT("Could not write"), *Path);
		}

		TWeakObjectPtr<UWorld> World;
		FSettings Settings;
		TWeakObjectPtr<ULightingFixtureData> FixtureData;
		TArray<TWeakObjectPtr<ALightingFixtureActor>> Spawned;
		TWeakObjectPtr<AStageAudienceActor> CrowdArea;
		// The user's show before the run, given back by Cleanup.
		TArray<FShowCue> SavedCues;
		float SavedActiveCue = -1.f;
		TMap<TWeakObjectPtr<ALightingFixtureActor>, TMap<FGameplayTag, float>> SavedLooks;
		bool bSessionWasDirty = false;
		bool bLoggedImportWait = false;

		EStep Step = EStep::Spawn;
		double StepSeconds = 0.0;
		/** The Grand Master before the run, put back when it ends. */
		TOptional<float> SavedGrandMaster;
		/** The programmer's effects and stops before the run (Effects=1|2|3), put back when it ends. */
		TOptional<FStageProgrammerEffects> SavedEffects;
		/** DMXOut=1: the bench's own output, installed for the whole run. Strong: this runner is not a UObject. */
		TStrongObjectPtr<UStageNetworkDMXBridge> BenchBridge;
		/** Audio=1: the bench started the test signal and switches it off at the end. */
		bool bBenchAudio = false;
		int32 CountIndex = 0;
		FRunResult Current;
		TArray<FRunResult> Results;
	};

	/** The running benchmark; at most one. Ticked by the core ticker, which is removed with it. */
	TUniquePtr<FBenchmark> Active;
	FTSTicker::FDelegateHandle TickerHandle;

	bool TickActive(float DeltaTime)
	{
		if (Active && Active->Tick(DeltaTime))
		{
			return true;
		}
		// Returning false removes this ticker; the benchmark is destroyed outside its own Tick.
		Active.Reset();
		TickerHandle.Reset();
		return false;
	}

	bool ParseSettings(const TArray<FString>& Args, FSettings& Out)
	{
		for (const FString& Arg : Args)
		{
			FString Key;
			FString Value;
			if (Arg.Split(TEXT("="), &Key, &Value))
			{
				const double Number = FCString::Atod(*Value);
				if (Key.Equals(TEXT("Seconds"), ESearchCase::IgnoreCase) && Number >= 1.0 && Number <= 600.0)
				{
					Out.PhaseSeconds = Number;
				}
				else if (Key.Equals(TEXT("Fade"), ESearchCase::IgnoreCase) && Number >= 0.1 && Number <= 60.0)
				{
					Out.FadeSeconds = Number;
				}
				else if (Key.Equals(TEXT("Settle"), ESearchCase::IgnoreCase) && Number >= 0.0 && Number <= 60.0)
				{
					Out.SettleSeconds = Number;
				}
				else if (Key.Equals(TEXT("Zoom"), ESearchCase::IgnoreCase) && (Value == TEXT("0") || Value == TEXT("1")))
				{
					Out.bFadeZoom = Value == TEXT("1");
				}
				else if (Key.Equals(TEXT("Item"), ESearchCase::IgnoreCase) && !Value.IsEmpty())
				{
					Out.ItemName = Value;
				}
				else if (Key.Equals(TEXT("GM"), ESearchCase::IgnoreCase) && Number >= 0.0 && Number <= 1.0 && !Value.IsEmpty())
				{
					Out.GrandMaster = Number;
				}
				else if (Key.Equals(TEXT("Effects"), ESearchCase::IgnoreCase) && (Value == TEXT("0") || Value == TEXT("1") || Value == TEXT("2") || Value == TEXT("3")))
				{
					Out.Effects = FCString::Atoi(*Value);
				}
				else if (Key.Equals(TEXT("DMXOut"), ESearchCase::IgnoreCase) && (Value == TEXT("0") || Value == TEXT("1")))
				{
					Out.bDMXOut = Value == TEXT("1");
				}
				else if (Key.Equals(TEXT("Audio"), ESearchCase::IgnoreCase) && (Value == TEXT("0") || Value == TEXT("1")))
				{
					Out.bAudio = Value == TEXT("1");
				}
				else if (Key.Equals(TEXT("Crowd"), ESearchCase::IgnoreCase) && Value.IsNumeric() && Number >= 0.0 && Number <= 20000.0)
				{
					Out.Crowd = FCString::Atoi(*Value);
				}
				else
				{
					UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Bench: ignored \"%s\" (Seconds 1..600, Fade 0.1..60, Settle 0..60, Zoom 0|1, Item=<asset name>, GM 0..1, Effects 0|1|2|3, DMXOut 0|1, Audio 0|1, Crowd 0..20000)."), *Arg);
				}
				continue;
			}

			const int32 Count = FCString::Atoi(*Arg);
			if (Count >= 1 && Count <= 2000 && Arg.IsNumeric())
			{
				Out.Counts.Add(Count);
			}
			else
			{
				UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Bench: ignored \"%s\" (fixture counts are 1..2000)."), *Arg);
			}
		}
		return !Out.Counts.IsEmpty();
	}

	void Start(const TArray<FString>& Args, UWorld* World)
	{
		if (Active)
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Bench: a run is in progress (StageCraft.Bench.Cancel stops it)."));
			return;
		}
		FSettings Settings;
		if (!World || !ParseSettings(Args, Settings))
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Bench.Fixtures <count> [<count>...] [Seconds=10] [Fade=2] [Settle=3] [Zoom=1] [Item=<asset name>]"));
			return;
		}

		TUniquePtr<FBenchmark> Benchmark = MakeUnique<FBenchmark>(*World, MoveTemp(Settings));
		if (!Benchmark->Prepare())
		{
			return;
		}
		Active = MoveTemp(Benchmark);
		TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&TickActive));
	}

	void Cancel(const TArray<FString>& Args, UWorld* World)
	{
		if (Active)
		{
			Active->Cancel();
		}
	}

	FAutoConsoleCommandWithWorldAndArgs StartCommand(TEXT("StageCraft.Bench.Fixtures"),
		TEXT("<count> [<count>...] [Seconds=10] [Fade=2] [Settle=3] [Zoom=1] [Item=<asset>] Spawns N fixtures, measures idle and continuous crossfades, removes them; results in the log and Saved/StageCraft/Benchmarks."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Start));
	FAutoConsoleCommandWithWorldAndArgs CancelCommand(TEXT("StageCraft.Bench.Cancel"), TEXT("Stops the running fixture benchmark, removes its fixtures and cues, and reports what was measured."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Cancel));
}

#endif // !UE_BUILD_SHIPPING
