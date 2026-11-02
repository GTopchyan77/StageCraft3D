// Copyright Epic Games, Inc. All Rights Reserved.

// StageCraft.Bench.Export: measures what exporting a stage to .glb costs the game thread, the old way (FStageSceneExporter, one
// call in one frame) against FStageSceneExportJob (spread over frames), on the same scene in the same run (STATE.md #55, Rules §22).
//
//   StageCraft.Bench.Export [Scene=<saved scene>] [Copies=<n>] [Runs=<n>] [Budget=<ms>]
//
// The saved scene is read once and its items repeated Copies times side by side, in memory only (nothing is saved). Each run does a
// synchronous export, waits, then a job export, and records every frame's wall time while each export runs. Written to the log
// ("StageCraft.Bench.Export:" lines) and to Saved/StageCraft/Benchmarks/ExportBench_<time>.csv; the .glb files are deleted.
// Needs a renderer (-game). Development builds only.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Bridge/StageSceneExportJob.h"
#include "Bridge/StageSceneExporter.h"
#include "Containers/Ticker.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "IO/StageCraftPaths.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "ModularSceneBuilder.h"
#include "Scene/StageSceneStore.h"
#include "Subsystems/StageItemSubsystem.h"
#include "Subsystems/StageProfileSubsystem.h"

namespace StageExportBenchmark
{
	/** Frames to let settle between two exports, so one export's frames are not counted in the other's. */
	constexpr int32 SettleFrames = 30;
	/** Distance between two copies of the scene, so they do not overlap (cm). */
	constexpr double CopySpacing = 3000.0;

	struct FSample
	{
		int32 Run = 0;
		FString Mode;
		double WallMs = 0.0;
		double WorstFrameMs = 0.0;
		double P95FrameMs = 0.0;
		int32 Frames = 0;
		double LongestSliceMs = 0.0;
		FStageSceneExportTimings Timings;
		bool bSuccess = false;
	};

	double Percentile(TArray<double> Values, double Fraction)
	{
		if (Values.IsEmpty())
		{
			return 0.0;
		}
		Values.Sort();
		return Values[FMath::Clamp(FMath::CeilToInt(Fraction * Values.Num()) - 1, 0, Values.Num() - 1)];
	}

	/** One benchmark in progress; lives as long as its ticker. */
	class FBench : public TSharedFromThis<FBench>
	{
	public:
		FBench(UGameInstance& InGameInstance, FStageSceneDocument InScene, int32 InRuns, double InBudgetMs)
			: GameInstance(&InGameInstance), Scene(MoveTemp(InScene)), Runs(InRuns), BudgetMs(InBudgetMs)
		{
		}

		void Begin()
		{
			Directory = FPaths::ConvertRelativePathToFull(FPaths::Combine(StageCraftPaths::DataRoot(), TEXT("Benchmarks"), TEXT("Export")));
			IFileManager::Get().MakeDirectory(*Directory, /*Tree*/ true);
			LastFrameSeconds = FPlatformTime::Seconds();
			Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateSP(this, &FBench::Tick));
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Bench.Export: \"%s\", %d items, %d runs, job budget %.1f ms."), *Scene.Name, Scene.Items.Num(), Runs, BudgetMs);
		}

	private:
		enum class EPhase : uint8 { Settle, Sync, AfterSync, Job, Done };

		bool Tick(float DeltaSeconds)
		{
			const double Now = FPlatformTime::Seconds();
			const double FrameMs = (Now - LastFrameSeconds) * 1000.0;
			LastFrameSeconds = Now;
			UGameInstance* Owner = GameInstance.Get();
			if (!Owner)
			{
				return false;
			}

			switch (Phase)
			{
			case EPhase::Settle:
				// Frames with no export: the baseline the export frames are compared with.
				IdleFrames.Add(FrameMs);
				if (++Waited >= SettleFrames)
				{
					Waited = 0;
					Phase = EPhase::Sync;
				}
				return true;

			case EPhase::Sync:
			{
				// The old path: everything in this one frame. Its cost shows as the next frame's wall time too.
				FSample& Sample = Samples.AddDefaulted_GetRef();
				Sample.Run = Run;
				Sample.Mode = TEXT("sync");
				const UStageItemSubsystem* Items = Owner->GetSubsystem<UStageItemSubsystem>();
				const UStageProfileSubsystem* Profile = Owner->GetSubsystem<UStageProfileSubsystem>();
				const double Start = FPlatformTime::Seconds();
				const FStageBridgeResultInfo Result = Items && Profile
					? FStageSceneExporter::ExportDocument(Scene, FStageSceneExporter::CollectExportableItems(*Items, *Profile), MakePath(TEXT("sync")), &Sample.Timings)
					: FStageBridgeResultInfo::Fail(EStageBridgeResult::Unavailable, FText::GetEmpty());
				Sample.WallMs = (FPlatformTime::Seconds() - Start) * 1000.0;
				Sample.LongestSliceMs = Sample.WallMs;
				Sample.Frames = 1;
				Sample.bSuccess = Result.IsSuccess();
				Phase = EPhase::AfterSync;
				return true;
			}

			case EPhase::AfterSync:
				// The first tick after the export closes the frame that contained it.
				if (Waited == 0)
				{
					Samples.Last().WorstFrameMs = FrameMs;
					Samples.Last().P95FrameMs = FrameMs;
				}
				if (++Waited >= SettleFrames)
				{
					Waited = 0;
					StartJob(*Owner);
				}
				return true;

			case EPhase::Job:
				JobFrames.Add(FrameMs);
				return true;

			case EPhase::Done:
			default:
				return false;
			}
		}

		void StartJob(UGameInstance& Owner)
		{
			FStageSceneExportJobParams Params;
			Params.Document = Scene;
			Params.Path = MakePath(TEXT("job"));
			Params.bComputeSha1 = true;
			Params.FrameBudgetMs = BudgetMs;
			JobFrames.Reset();
			JobStart = FPlatformTime::Seconds();
			Phase = EPhase::Job;
			FStageBridgeResultInfo Refusal;
			Job = FStageSceneExportJob::Start(Owner, MoveTemp(Params), FOnStageSceneExportFinished::CreateSP(this, &FBench::HandleJobFinished), Refusal);
			if (!Job)
			{
				UE_LOG(LogStageCraft, Warning, TEXT("StageCraft.Bench.Export: the job was refused: %s"), *Refusal.Message.ToString());
				Finish();
			}
		}

		void HandleJobFinished(const FStageSceneExportOutcome& Outcome)
		{
			FSample& Sample = Samples.AddDefaulted_GetRef();
			Sample.Run = Run;
			Sample.Mode = TEXT("job");
			Sample.WallMs = (FPlatformTime::Seconds() - JobStart) * 1000.0;
			// The frame the job finished in is still running; its slice is counted as LongestSliceMs.
			Sample.WorstFrameMs = JobFrames.IsEmpty() ? 0.0 : FMath::Max(JobFrames);
			Sample.P95FrameMs = Percentile(JobFrames, 0.95);
			Sample.Frames = Outcome.Frames;
			Sample.LongestSliceMs = Outcome.LongestSliceMs;
			Sample.Timings = Outcome.Timings;
			Sample.bSuccess = Outcome.Result.IsSuccess();
			Job.Reset();

			if (++Run < Runs)
			{
				Phase = EPhase::Settle;
				return;
			}
			Finish();
		}

		FString MakePath(const TCHAR* Mode) const
		{
			return FPaths::Combine(Directory, FString::Printf(TEXT("Bench_%s_%d.glb"), Mode, Run));
		}

		void Finish()
		{
			Phase = EPhase::Done;
			FString Csv = TEXT("run,mode,success,wall_ms,worst_frame_ms,p95_frame_ms,frames,longest_slice_ms,load_ms,rights_ms,world_ms,spawn_ms,convert_ms,gltf_ms,finalize_ms,teardown_ms,hash_ms\n");
			for (const FSample& Sample : Samples)
			{
				const FStageSceneExportTimings& T = Sample.Timings;
				Csv += FString::Printf(TEXT("%d,%s,%d,%.2f,%.2f,%.2f,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n"), Sample.Run, *Sample.Mode, Sample.bSuccess ? 1 : 0,
					Sample.WallMs, Sample.WorstFrameMs, Sample.P95FrameMs, Sample.Frames, Sample.LongestSliceMs,
					T.LoadMs, T.RightsMs, T.WorldMs, T.SpawnMs, T.ConvertMs, T.GltfMs, T.FinalizeMs, T.TeardownMs, T.HashMs);
				UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Bench.Export: run %d %s %s: wall %.1f ms, worst frame %.1f ms, p95 %.1f ms, %d frames, longest slice %.1f ms; %s."),
					Sample.Run, *Sample.Mode, Sample.bSuccess ? TEXT("ok") : TEXT("FAILED"), Sample.WallMs, Sample.WorstFrameMs, Sample.P95FrameMs, Sample.Frames,
					Sample.LongestSliceMs, *T.ToString());
			}
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Bench.Export: frames without an export: median %.1f ms, p95 %.1f ms, worst %.1f ms (%d frames)."),
				Percentile(IdleFrames, 0.5), Percentile(IdleFrames, 0.95), IdleFrames.IsEmpty() ? 0.0 : FMath::Max(IdleFrames), IdleFrames.Num());
			Csv += FString::Printf(TEXT("idle,median,1,%.2f,%.2f,%.2f,%d,0,0,0,0,0,0,0,0,0,0\n"), Percentile(IdleFrames, 0.5),
				IdleFrames.IsEmpty() ? 0.0 : FMath::Max(IdleFrames), Percentile(IdleFrames, 0.95), IdleFrames.Num());
			const FString CsvPath = FPaths::Combine(FPaths::GetPath(Directory), FString::Printf(TEXT("ExportBench_%s.csv"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"))));
			FFileHelper::SaveStringToFile(Csv, *CsvPath);
			IFileManager::Get().DeleteDirectory(*Directory, /*RequireExists*/ false, /*Tree*/ true);
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Bench.Export: done; %s"), *CsvPath);
			FTSTicker::GetCoreTicker().RemoveTicker(Ticker);
			Active.Reset();
		}

	public:
		/** The benchmark running, if any: it keeps itself alive until done. */
		static TSharedPtr<FBench> Active;

	private:
		TWeakObjectPtr<UGameInstance> GameInstance;
		FStageSceneDocument Scene;
		int32 Runs = 1;
		double BudgetMs = 4.0;
		FString Directory;
		EPhase Phase = EPhase::Settle;
		int32 Run = 0;
		int32 Waited = 0;
		double LastFrameSeconds = 0.0;
		double JobStart = 0.0;
		TArray<double> JobFrames;
		TArray<double> IdleFrames;
		TArray<FSample> Samples;
		TSharedPtr<FStageSceneExportJob> Job;
		FTSTicker::FDelegateHandle Ticker;
	};

	TSharedPtr<FBench> FBench::Active;

	void Run(const TArray<FString>& Args, UWorld* World)
	{
		UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		if (!GameInstance)
		{
			UE_LOG(LogStageCraft, Warning, TEXT("StageCraft.Bench.Export: no game instance (run it in -game)."));
			return;
		}
		if (FBench::Active.IsValid())
		{
			UE_LOG(LogStageCraft, Warning, TEXT("StageCraft.Bench.Export: already running."));
			return;
		}
		const FString Line = FString::Join(Args, TEXT(" "));
		FString SceneName = TEXT("R38 Cues");
		int32 Copies = 10;
		int32 Runs = 3;
		double BudgetMs = 4.0;
		FParse::Value(*Line, TEXT("Scene="), SceneName);
		FParse::Value(*Line, TEXT("Copies="), Copies);
		FParse::Value(*Line, TEXT("Runs="), Runs);
		FParse::Value(*Line, TEXT("Budget="), BudgetMs);
		Copies = FMath::Clamp(Copies, 1, 1000);
		Runs = FMath::Clamp(Runs, 1, 20);

		FStageSceneDocument Source;
		FStageSceneParseReport Report;
		if (FStageSceneStore(FStageSceneStore::GetDefaultDirectory()).Load(SceneName, Source, Report) != EStageSceneResult::Success)
		{
			UE_LOG(LogStageCraft, Warning, TEXT("StageCraft.Bench.Export: there is no readable saved scene \"%s\" (Scene=<name>)."), *SceneName);
			return;
		}
		// Only in memory: nothing is saved, and the copy is not a marketplace stage (no rights to check).
		FStageSceneDocument Scene = Source;
		Scene.Name = FString::Printf(TEXT("%s x%d"), *SceneName, Copies);
		Scene.OriginListingId.Invalidate();
		Scene.Items.Reset();
		for (int32 Copy = 0; Copy < Copies; ++Copy)
		{
			for (FStageItemSnapshot Item : Source.Items)
			{
				Item.InstanceId = FGuid::NewGuid();
				Item.Transform.AddToTranslation(FVector(CopySpacing * Copy, 0.0, 0.0));
				Scene.Items.Add(MoveTemp(Item));
			}
		}
		FBench::Active = MakeShared<FBench>(*GameInstance, MoveTemp(Scene), Runs, BudgetMs);
		FBench::Active->Begin();
	}

	FAutoConsoleCommandWithWorldAndArgs Command(
		TEXT("StageCraft.Bench.Export"),
		TEXT("Measures the game-thread cost of exporting a stage to .glb, synchronously and as a job spread over frames. ")
		TEXT("StageCraft.Bench.Export [Scene=<saved scene>] [Copies=<n>] [Runs=<n>] [Budget=<ms>]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Run));
}

#endif // !UE_BUILD_SHIPPING
