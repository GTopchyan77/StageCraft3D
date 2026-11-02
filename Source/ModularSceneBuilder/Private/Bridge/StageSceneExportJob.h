// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Bridge/StageBridgeTypes.h"
#include "Bridge/StageSceneExporter.h"
#include "Engine/TimerHandle.h"
#include "Marketplace/StageMarketplacePreview.h"
#include "Marketplace/StagePackage.h"
#include "Scene/StageSceneTypes.h"
#include "Tasks/Task.h"
#include "UObject/GCObject.h"

/** What a finished export job reports. */
struct FStageSceneExportOutcome
{
	FStageBridgeResultInfo Result;
	/** Lower-case hex SHA-1 of the written file, when asked for (a send); computed on a worker. */
	FString Sha1;
	FStageSceneExportTimings Timings;
	/** Frames the job spanned, and the longest single game-thread slice it took (milliseconds). */
	int32 Frames = 0;
	double LongestSliceMs = 0.0;
};

DECLARE_DELEGATE_OneParam(FOnStageSceneExportFinished, const FStageSceneExportOutcome& /*Outcome*/);

/** What to export and how. */
struct FStageSceneExportJobParams
{
	/** A saved scene of the scenes folder; ignored when Document is set. */
	FString SceneName;
	/** A scene already in memory (benchmarks, tests); the scenes folder is not read and no export rights are needed (not marketplace). */
	TOptional<FStageSceneDocument> Document;
	/** Absolute, ending in .glb. */
	FString Path;
	/** Also hash the written file (a send tells the add-on the SHA-1). */
	bool bComputeSha1 = false;
	/** Game-thread time the job may take per frame for the work it can split (spawning, baking). At least one item per frame. */
	double FrameBudgetMs = 4.0;
};

/**
 * The asynchronous form of FStageSceneExporter::ExportSavedScene (Docs/ADR/0022-dcc-bridge.md §12, STATE.md #55): the same steps
 * and rules, spread over frames so the user interface keeps drawing during an export.
 *
 *   worker      read and parse the scene; read the library manifest for a marketplace stage's rights
 *   game        decide the rights (profile), collect the catalog                      } one frame
 *   game        create the private GamePreview world                                  } one frame
 *   game        spawn the items, a time budget per frame                              } several frames
 *   game        bake procedural parts, a time budget per frame                        } several frames
 *   game        the engine's glTF conversion and write (UObjects: game thread only)   } one frame: the remaining spike
 *   game        destroy the world                                                     } one frame
 *   worker      move the file into place, its size, its SHA-1
 *   game        the callback
 *
 * Owned by its starter through the TSharedPtr from Start; dropping it (or Cancel) abandons the export without a callback, destroys
 * the world, and lets a running worker step finish into nothing (it holds only strings and bytes, never UObjects). Holds its world
 * and actors for the garbage collector (FGCObject). Steps once per frame on the game instance's timer manager, only while it runs. Game thread only.
 */
class FStageSceneExportJob : public FGCObject, public TSharedFromThis<FStageSceneExportJob>
{
public:
	/**
	 * Starts an export for the profile and item library of GameInstance. Returns null with OutRefusal for a request refused
	 * before any work (bad name or path, no renderer, no library). OnFinished is called exactly once unless the job is cancelled.
	 */
	static TSharedPtr<FStageSceneExportJob> Start(class UGameInstance& GameInstance, FStageSceneExportJobParams Params, FOnStageSceneExportFinished OnFinished,
		FStageBridgeResultInfo& OutRefusal);

	virtual ~FStageSceneExportJob() override;

	/** Ends the job now without a callback. Idempotent. */
	void Cancel();

	bool IsRunning() const { return Stage != EStage::Done; }

	//~ Begin FGCObject Interface
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override { return TEXT("FStageSceneExportJob"); }
	//~ End FGCObject Interface

private:
	enum class EStage : uint8
	{
		Loading,
		CreatingWorld,
		Spawning,
		Converting,
		Exporting,
		TearingDown,
		Finalizing,
		Done,
	};

	/** What the loading worker returns: data only. */
	struct FLoaded
	{
		EStageSceneResult SceneResult = EStageSceneResult::NotFound;
		FStageSceneDocument Scene;
		bool bHasLibraryManifest = false;
		FStagePackageManifest LibraryManifest;
		double LoadMs = 0.0;
		double RightsMs = 0.0;
	};

	/** What the finalizing worker returns. */
	struct FFinalized
	{
		bool bMoved = false;
		int64 Bytes = 0;
		FString Sha1;
		double FinalizeMs = 0.0;
		double HashMs = 0.0;
	};

	FStageSceneExportJob(class UGameInstance& GameInstance, FStageSceneExportJobParams InParams, FOnStageSceneExportFinished InOnFinished);

	/** One frame of the job; schedules the next while it runs. */
	void Tick();
	void ScheduleTick();
	/** Runs the current stage within this frame's slice (spawning and baking stop at SliceEndSeconds). */
	void Step(double SliceEndSeconds);
	void StepLoaded();
	void StepCreateWorld();
	void StepSpawn(double SliceEndSeconds);
	void StepConvert(double SliceEndSeconds);
	void StepExport();
	void StepTeardown();
	void StepFinalized();

	void DestroyWorld();
	/** Ends the job with Failure (the scene name kept); the world is destroyed. */
	void Fail(const FStageBridgeResultInfo& Failure);

	TWeakObjectPtr<class UGameInstance> GameInstance;
	FStageSceneExportJobParams Params;
	FOnStageSceneExportFinished OnFinished;
	FStageSceneExportOutcome Outcome;
	EStage Stage = EStage::Loading;

	UE::Tasks::TTask<FLoaded> LoadTask;
	UE::Tasks::TTask<FFinalized> FinalizeTask;
	FStageSceneDocument Scene;
	FString TempPath;

	/** The items the profile may export, by catalog path (the form SpawnItem takes); CatalogItems holds them for the collector. */
	TMap<FSoftObjectPath, class UBaseItemData*> Catalog;
	TArray<TObjectPtr<class UBaseItemData>> CatalogItems;
	TObjectPtr<class UWorld> World = nullptr;
	TArray<TObjectPtr<class AActor>> Actors;
	FBox Bounds = FBox(ForceInit);
	FStageMarketplacePreviewStats Stats;
	int32 NextItem = 0;
	int32 NextActor = 0;
	int32 Converted = 0;
	/** The glTF step failed: teardown ends the job with Outcome.Result instead of finalizing. */
	bool bExportFailed = false;

	/** The next frame's step, on the game instance's timer manager (so the job lives and ticks with its game instance). */
	FTimerHandle StepTimer;
};
