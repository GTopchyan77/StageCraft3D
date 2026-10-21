// Copyright Epic Games, Inc. All Rights Reserved.

// Fixture stress benchmark (Docs/ADR/0012-production-reports-and-fixture-benchmark.md §3): spawns N lighting fixtures in the
// running game world, records two cues, and measures frame, game thread, render thread, GPU and show-control tick time while
// idle and while crossfading continuously; then removes everything it added. Development builds only (compiled out of Shipping).
//
//   StageCraft.Bench.Fixtures 50 200 500 [Seconds=10] [Fade=2] [Settle=3] [Zoom=1] [Item=<catalog asset name>]
//   (Zoom=0 keeps the zoom fixed: a cone change rebuilds the engine's light proxy, so zoom fades are measured apart.)
//   StageCraft.Bench.Cancel
//
// Results go to the log ("StageCraft.Bench:") and to Saved/StageCraft/Benchmarks/FixtureBench_<time>.csv.
// Scripts/Bench/RunFixtureBench.ps1 runs it in a Standalone window.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Actors/LightingFixtureActor.h"
#include "Components/SpawnSystemComponent.h"
#include "Containers/Ticker.h"
#include "Data/LightingFixtureData.h"
#include "Data/StageParameterTypes.h"
#include "Diagnostics/StageFrameStats.h"
#include "DynamicRHI.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Import/StageMeshImportSubsystem.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ProfilingDebugging/MiscTrace.h"
#include "ModularSceneBuilder.h"
#include "Player/ModularPlayerController.h"
#include "RenderTimer.h"
#include "Subsystems/ShowControlSubsystem.h"
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
			const int32 Pending = Imports ? Imports->GetPendingCount() : 0;
			if (Pending > 0 && !bLoggedImportWait)
			{
				UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Bench: waiting for %d model import(s) to finish before measuring."), Pending);
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
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Bench: spawned %d of %d fixtures; settling %.0f s."), Current.SpawnedFixtures, Current.RequestedFixtures, Settings.SettleSeconds);
			SetStep(EStep::Settle);
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
			ApplyLook(false);
			Show->StoreCue(CueA, FText::FromString(TEXT("Bench A")), Settings.FadeSeconds);
			ApplyLook(true);
			Show->StoreCue(CueB, FText::FromString(TEXT("Bench B")), Settings.FadeSeconds);
			// Idle starts from a settled look with no fade running.
			Show->GoToCue(CueA);
			Show->FinishFade();
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

			if (Show)
			{
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
			FString Csv = TEXT("Fixtures,Phase,Frames,FrameMeanMs,FrameP50Ms,FrameP95Ms,FrameP99Ms,FrameMaxMs,GameMeanMs,GameP95Ms,GpuMeanMs,GpuP95Ms,ShowTicks,ShowTickMeanMs,ShowTickMaxMs,DroppedFrames,CuesStarted\r\n");
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

					UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Bench: %4d fixtures %-9s %5d frames | frame %s | game %6.2f / p95 %6.2f | GPU %6.2f / p95 %6.2f | show tick %5d x %.3f ms (max %.3f) | dropped %d"),
						Result.SpawnedFixtures, PhaseName, Frame.Count, *Describe(Frame), Game.Mean, Game.P95, Gpu.Mean, Gpu.P95,
						Samples.ShowTick.Ticks, ShowMeanMs, ShowMaxMs, Dropped);
					Csv += FString::Printf(TEXT("%d,%s,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%.4f,%.4f,%d,%d\r\n"),
						Result.SpawnedFixtures, PhaseName, Frame.Count, Frame.Mean, Frame.Median, Frame.P95, Frame.P99, Frame.Max, Game.Mean, Game.P95,
						Gpu.Mean, Gpu.P95, Samples.ShowTick.Ticks, ShowMeanMs, ShowMaxMs, bCrossfade ? Dropped : 0, bCrossfade ? Result.CuesStarted : 0);
				}
			}

			const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("StageCraft") / TEXT("Benchmarks"));
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
		// The user's show before the run, given back by Cleanup.
		TArray<FShowCue> SavedCues;
		float SavedActiveCue = -1.f;
		TMap<TWeakObjectPtr<ALightingFixtureActor>, TMap<FGameplayTag, float>> SavedLooks;
		bool bSessionWasDirty = false;
		bool bLoggedImportWait = false;

		EStep Step = EStep::Spawn;
		double StepSeconds = 0.0;
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
				else
				{
					UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Bench: ignored \"%s\" (Seconds 1..600, Fade 0.1..60, Settle 0..60, Zoom 0|1, Item=<asset name>)."), *Arg);
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
