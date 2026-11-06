// Copyright Epic Games, Inc. All Rights Reserved.

#include "Bridge/StageSceneExportJob.h"

#include "Data/BaseItemData.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Exporters/GLTFExporter.h"
#include "HAL/FileManager.h"
#include "Marketplace/StageMarketplaceSubsystem.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "ModularSceneBuilder.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Scene/StageSceneStore.h"
#include "Subsystems/StageItemSubsystem.h"
#include "Subsystems/StageProfileSubsystem.h"
#include "Tasks/Task.h"
#include "TimerManager.h"

#define LOCTEXT_NAMESPACE "StageSceneExporter"

namespace StageSceneExportJobImpl
{
	double MillisecondsSince(double StartSeconds)
	{
		return (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	}

	/** A FPlatformTime value FrameBudgetMs from now; the budget is clamped so a bad value cannot stall or starve the job. */
	double SliceEnd(double StartSeconds, double FrameBudgetMs)
	{
		constexpr double MinBudgetMs = 1.0;
		constexpr double MaxBudgetMs = 50.0;
		return StartSeconds + FMath::Clamp(FrameBudgetMs, MinBudgetMs, MaxBudgetMs) / 1000.0;
	}
}

TSharedPtr<FStageSceneExportJob> FStageSceneExportJob::Start(UGameInstance& GameInstance, FStageSceneExportJobParams Params, FOnStageSceneExportFinished OnFinished,
	FStageBridgeResultInfo& OutRefusal)
{
	// The refusals that need no work, worded as the synchronous exporter words them.
	const bool bPathValid = !FPaths::IsRelative(Params.Path) && Params.Path.EndsWith(FStageSceneExporter::FileExtension, ESearchCase::IgnoreCase);
	OutRefusal = Params.Document.IsSet()
		? (bPathValid ? FStageBridgeResultInfo() : FStageSceneExporter::ValidateRequest(FString(), Params.Path))
		: FStageSceneExporter::ValidateRequest(Params.SceneName, Params.Path);
	if (OutRefusal.IsSuccess())
	{
		OutRefusal = FStageSceneExporter::CheckRenderer();
	}
	const UStageItemSubsystem* Items = GameInstance.GetSubsystem<UStageItemSubsystem>();
	const UStageProfileSubsystem* Profile = GameInstance.GetSubsystem<UStageProfileSubsystem>();
	if (OutRefusal.IsSuccess() && (!Items || !Profile))
	{
		OutRefusal = FStageBridgeResultInfo::Fail(EStageBridgeResult::Unavailable, LOCTEXT("Unavailable", "The item Library is not available."));
	}
	if (!OutRefusal.IsSuccess())
	{
		return nullptr;
	}

	TSharedPtr<FStageSceneExportJob> Job = MakeShareable(new FStageSceneExportJob(GameInstance, MoveTemp(Params), MoveTemp(OnFinished)));
	// The catalog is decided now, from the profile as it is when the user asked (the same rule as placing an item).
	Job->Catalog = FStageSceneExporter::CollectExportableItems(*Items, *Profile);
	for (const TPair<FSoftObjectPath, UBaseItemData*>& Entry : Job->Catalog)
	{
		Job->CatalogItems.Add(Entry.Value);
	}

	if (Job->Params.Document.IsSet())
	{
		FLoaded Loaded;
		Loaded.SceneResult = EStageSceneResult::Success;
		Loaded.Scene = Job->Params.Document.GetValue();
		Job->LoadTask = UE::Tasks::MakeCompletedTask<FLoaded>(MoveTemp(Loaded));
	}
	else
	{
		// File work on a worker: only strings go in, only data comes out (Rules §29).
		Job->LoadTask = UE::Tasks::Launch(UE_SOURCE_LOCATION, [SceneName = Job->Params.SceneName, Directory = FStageSceneStore::GetDefaultDirectory()]()
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(StageSceneExportJob_Load);
			FLoaded Loaded;
			double Start = FPlatformTime::Seconds();
			FStageSceneParseReport Report;
			Loaded.SceneResult = FStageSceneStore(Directory).Load(SceneName, Loaded.Scene, Report);
			Loaded.InvalidItems = Report.InvalidItems;
			Loaded.LoadMs = StageSceneExportJobImpl::MillisecondsSince(Start);
			if (Loaded.SceneResult == EStageSceneResult::Success && Loaded.Scene.IsFromMarketplace())
			{
				Start = FPlatformTime::Seconds();
				Loaded.bHasLibraryManifest = UStageMarketplaceSubsystem::ReadLibraryManifest(Loaded.Scene.OriginListingId, Loaded.LibraryManifest);
				Loaded.RightsMs = StageSceneExportJobImpl::MillisecondsSince(Start);
			}
			return Loaded;
		});
	}
	Job->ScheduleTick();
	return Job;
}

void FStageSceneExportJob::ScheduleTick()
{
	if (UGameInstance* Owner = GameInstance.Get())
	{
		StepTimer = Owner->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateSP(this, &FStageSceneExportJob::Tick));
	}
}

FStageSceneExportJob::FStageSceneExportJob(UGameInstance& InGameInstance, FStageSceneExportJobParams InParams, FOnStageSceneExportFinished InOnFinished)
	: GameInstance(&InGameInstance)
	, Params(MoveTemp(InParams))
	, OnFinished(MoveTemp(InOnFinished))
{
	Outcome.Result.SceneName = Params.Document.IsSet() ? Params.Document->Name : Params.SceneName;
}

FStageSceneExportJob::~FStageSceneExportJob()
{
	Cancel();
}

void FStageSceneExportJob::Cancel()
{
	OnFinished.Unbind();
	if (UGameInstance* Owner = GameInstance.Get())
	{
		Owner->GetTimerManager().ClearTimer(StepTimer);
	}
	StepTimer.Invalidate();
	if (Stage != EStage::Done && Stage != EStage::Finalizing && !TempPath.IsEmpty())
	{
		IFileManager::Get().Delete(*TempPath, /*RequireExists*/ false, /*EvenReadOnly*/ false, /*Quiet*/ true);
	}
	DestroyWorld();
	Stage = EStage::Done;
}

void FStageSceneExportJob::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObject(World);
	Collector.AddReferencedObjects(Actors);
	Collector.AddReferencedObjects(CatalogItems);
}

void FStageSceneExportJob::Tick()
{
	// The callback may drop the owner's reference; this one keeps the job alive until the tick returns.
	const TSharedRef<FStageSceneExportJob> KeepAlive = AsShared();
	StepTimer.Invalidate();
	if (Stage == EStage::Done)
	{
		return;
	}

	const double Start = FPlatformTime::Seconds();
	Step(StageSceneExportJobImpl::SliceEnd(Start, Params.FrameBudgetMs));
	++Outcome.Frames;
	Outcome.LongestSliceMs = FMath::Max(Outcome.LongestSliceMs, StageSceneExportJobImpl::MillisecondsSince(Start));

	if (Stage != EStage::Done)
	{
		ScheduleTick();
		return;
	}
	UE_LOG(LogStageCraft, Log, TEXT("Scene export job \"%s\": %s in %d frames, longest slice %.1f ms; %s."), *Outcome.Result.SceneName,
		*UEnum::GetValueAsString(Outcome.Result.Code), Outcome.Frames, Outcome.LongestSliceMs, *Outcome.Timings.ToString());
	FOnStageSceneExportFinished Callback = MoveTemp(OnFinished);
	OnFinished.Unbind();
	Callback.ExecuteIfBound(Outcome);
}

void FStageSceneExportJob::Step(double SliceEndSeconds)
{
	if (!GameInstance.IsValid())
	{
		Cancel();
		return;
	}
	switch (Stage)
	{
	case EStage::Loading:
		if (LoadTask.IsCompleted())
		{
			StepLoaded();
		}
		return;
	case EStage::CreatingWorld:
		StepCreateWorld();
		return;
	case EStage::Spawning:
		StepSpawn(SliceEndSeconds);
		return;
	case EStage::Converting:
		StepConvert(SliceEndSeconds);
		return;
	case EStage::Exporting:
		StepExport();
		return;
	case EStage::TearingDown:
		StepTeardown();
		return;
	case EStage::Finalizing:
		if (FinalizeTask.IsCompleted())
		{
			StepFinalized();
		}
		return;
	case EStage::Done:
	default:
		return;
	}
}

void FStageSceneExportJob::StepLoaded()
{
	FLoaded Loaded = MoveTemp(LoadTask.GetResult());
	LoadTask = {};
	Outcome.Timings.LoadMs = Loaded.LoadMs;
	Outcome.Timings.RightsMs = Loaded.RightsMs;
	if (Loaded.SceneResult != EStageSceneResult::Success)
	{
		Fail(FStageSceneExporter::DescribeLoadFailure(Loaded.SceneResult, Params.SceneName));
		return;
	}
	Scene = MoveTemp(Loaded.Scene);
	Counts.InvalidInFile = Loaded.InvalidItems;

	// Deny by default: a marketplace stage leaves only when its publisher allowed it (ADR 0022 §11.1). The file was read on the
	// worker; the decision needs the profile, so it is made here.
	if (Scene.IsFromMarketplace())
	{
		const UStageMarketplaceSubsystem* Marketplace = GameInstance->GetSubsystem<UStageMarketplaceSubsystem>();
		const FStageMarketplaceResultInfo Rights = Marketplace
			? Marketplace->DecideExportRights(Scene.OriginListingId, Loaded.bHasLibraryManifest ? &Loaded.LibraryManifest : nullptr)
			: FStageMarketplaceResultInfo::Fail(EStageMarketplaceResult::NotOriginal, FText::GetEmpty());
		if (!Rights.IsSuccess())
		{
			Fail(FStageSceneExporter::DescribeNoRights(Outcome.Result.SceneName, Rights.Message));
			return;
		}
	}
	Stage = EStage::CreatingWorld;
}

void FStageSceneExportJob::StepCreateWorld()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(StageSceneExportJob_World);
	const double Start = FPlatformTime::Seconds();
	World = FStageSceneExporter::CreateExportWorld();
	Outcome.Timings.WorldMs += StageSceneExportJobImpl::MillisecondsSince(Start);
	Stage = EStage::Spawning;
}

void FStageSceneExportJob::StepSpawn(double SliceEndSeconds)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(StageSceneExportJob_Spawn);
	const double Start = FPlatformTime::Seconds();
	// At least one item per frame, however slow, so the job always moves.
	do
	{
		if (NextItem >= Scene.Items.Num())
		{
			break;
		}
		StageMarketplacePreview::SpawnItem(*World, Scene.Items[NextItem++], Catalog, Actors, Bounds, Stats, EStageHiddenItems::Skip);
	}
	while (FPlatformTime::Seconds() < SliceEndSeconds);
	Outcome.Timings.SpawnMs += StageSceneExportJobImpl::MillisecondsSince(Start);

	if (NextItem < Scene.Items.Num())
	{
		return;
	}
	if (Stats.Placed == 0)
	{
		Fail(FStageSceneExporter::DescribeNothingToExport(Outcome.Result.SceneName));
		return;
	}
	Stage = EStage::Converting;
}

void FStageSceneExportJob::StepConvert(double SliceEndSeconds)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(StageSceneExportJob_Convert);
	const double Start = FPlatformTime::Seconds();
	do
	{
		if (NextActor >= Actors.Num())
		{
			break;
		}
		if (AActor* Actor = Actors[NextActor++])
		{
			Converted += FStageSceneExporter::ConvertProceduralMeshes(*Actor);
			FStageSceneExporter::AddActorContent(*Actor, Counts);
		}
	}
	while (FPlatformTime::Seconds() < SliceEndSeconds);
	Outcome.Timings.ConvertMs += StageSceneExportJobImpl::MillisecondsSince(Start);
	if (NextActor >= Actors.Num())
	{
		Stage = EStage::Exporting;
	}
}

void FStageSceneExportJob::StepExport()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(StageSceneExportJob_Gltf);
	const double Start = FPlatformTime::Seconds();
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Params.Path), /*Tree*/ true);
	TempPath = FStageSceneExporter::GetExportTempPath(Params.Path);
	TSet<AActor*> Selected;
	for (AActor* Actor : Actors)
	{
		Selected.Add(Actor);
	}
	// The engine converts and writes in one call that reads components and world state, so it cannot be split or moved off the
	// game thread: the one frame of the job that takes as long as the scene needs (measured in Timings.GltfMs).
	FGLTFExportMessages Messages;
	const bool bExported = UGLTFExporter::ExportToGLTF(World, TempPath, FStageSceneExporter::MakeExportOptions(), Selected, Messages);
	FStageSceneExporter::LogExportMessages(Messages);
	Outcome.Timings.GltfMs += StageSceneExportJobImpl::MillisecondsSince(Start);
	if (!bExported || !IFileManager::Get().FileExists(*TempPath))
	{
		IFileManager::Get().Delete(*TempPath, /*RequireExists*/ false, /*EvenReadOnly*/ false, /*Quiet*/ true);
		Outcome.Result = FStageSceneExporter::DescribeExportFailed(Outcome.Result.SceneName, Messages.Errors);
		bExportFailed = true;
	}
	Stage = EStage::TearingDown;
}

void FStageSceneExportJob::StepTeardown()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(StageSceneExportJob_Teardown);
	const double Start = FPlatformTime::Seconds();
	DestroyWorld();
	Outcome.Timings.TeardownMs += StageSceneExportJobImpl::MillisecondsSince(Start);
	if (bExportFailed)
	{
		Stage = EStage::Done;
		return;
	}

	FinalizeTask = UE::Tasks::Launch(UE_SOURCE_LOCATION, [Temp = TempPath, Path = Params.Path, bHash = Params.bComputeSha1]()
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(StageSceneExportJob_Finalize);
		FFinalized Finalized;
		double Start = FPlatformTime::Seconds();
		Finalized.bMoved = IFileManager::Get().Move(*Path, *Temp, /*Replace*/ true, /*EvenIfReadOnly*/ false, /*Attributes*/ false, /*bDoNotRetryOrError*/ true);
		if (!Finalized.bMoved)
		{
			IFileManager::Get().Delete(*Temp, /*RequireExists*/ false, /*EvenReadOnly*/ false, /*Quiet*/ true);
		}
		Finalized.Bytes = Finalized.bMoved ? IFileManager::Get().FileSize(*Path) : 0;
		Finalized.FinalizeMs = StageSceneExportJobImpl::MillisecondsSince(Start);
		if (bHash && Finalized.bMoved)
		{
			Start = FPlatformTime::Seconds();
			TArray<uint8> Bytes;
			if (FFileHelper::LoadFileToArray(Bytes, *Path, FILEREAD_Silent))
			{
				FSHAHash Hash;
				FSHA1::HashBuffer(Bytes.GetData(), Bytes.Num(), Hash.Hash);
				Finalized.Sha1 = Hash.ToString().ToLower();
			}
			Finalized.HashMs = StageSceneExportJobImpl::MillisecondsSince(Start);
		}
		return Finalized;
	});
	Stage = EStage::Finalizing;
}

void FStageSceneExportJob::StepFinalized()
{
	const FFinalized Finalized = FinalizeTask.GetResult();
	FinalizeTask = {};
	Outcome.Timings.FinalizeMs = Finalized.FinalizeMs;
	Outcome.Timings.HashMs = Finalized.HashMs;
	if (!Finalized.bMoved)
	{
		Fail(FStageSceneExporter::DescribeWriteFailed(Params.Path));
		return;
	}
	if (Params.bComputeSha1 && Finalized.Sha1.IsEmpty())
	{
		Fail(FStageSceneExporter::DescribeWriteFailed(Params.Path));
		return;
	}
	Counts.Exported = Stats.Placed;
	Counts.NotInLibrary = Stats.NotInLibrary;
	Counts.Hidden = Stats.Hidden;
	Outcome.Result = FStageSceneExporter::DescribeExported(Outcome.Result.SceneName, Counts, Params.Path);
	Outcome.Sha1 = Finalized.Sha1;
	UE_LOG(LogStageCraft, Log, TEXT("Scene export: \"%s\" -> %s: %d items, %d lights (%d procedural parts converted; left out: %d unavailable, %d hidden, %d damaged), %lld bytes."),
		*Outcome.Result.SceneName, *Params.Path, Counts.Exported, Counts.Lights, Converted, Counts.NotInLibrary, Counts.Hidden, Counts.InvalidInFile, Finalized.Bytes);
	Stage = EStage::Done;
}

void FStageSceneExportJob::DestroyWorld()
{
	FStageSceneExporter::DestroyExportWorld(World);
	World = nullptr;
	Actors.Reset();
}

void FStageSceneExportJob::Fail(const FStageBridgeResultInfo& Failure)
{
	const FString SceneName = Outcome.Result.SceneName;
	Outcome.Result = Failure;
	Outcome.Result.SceneName = SceneName;
	DestroyWorld();
	Stage = EStage::Done;
}

#undef LOCTEXT_NAMESPACE
