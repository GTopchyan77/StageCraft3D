// Copyright Epic Games, Inc. All Rights Reserved.

#include "Bridge/StageSceneExporter.h"

#include "Actors/ModularBaseActor.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/LightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Data/BaseItemData.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Exporters/GLTFExporter.h"
#include "HAL/FileManager.h"
#include "Marketplace/StageMarketplacePreview.h"
#include "Marketplace/StageMarketplaceSubsystem.h"
#include "Materials/Material.h"
#include "MeshDescription.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ModularSceneBuilder.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Options/GLTFExportOptions.h"
#include "ProceduralMeshComponent.h"
#include "ProceduralMeshConversion.h"
#include "Scene/StageSceneStore.h"
#include "Subsystems/StageItemSubsystem.h"
#include "Subsystems/StageProfileSubsystem.h"

#define LOCTEXT_NAMESPACE "StageSceneExporter"

namespace StageSceneExporterImpl
{
	/** The exporter's messages are logged, at most this many per kind, so a large scene cannot flood the log. */
	constexpr int32 MaxLoggedMessages = 10;

	void LogMessages(const TArray<FString>& Messages, ELogVerbosity::Type Verbosity, const TCHAR* Kind)
	{
		for (int32 Index = 0; Index < FMath::Min(Messages.Num(), MaxLoggedMessages); ++Index)
		{
			if (Verbosity == ELogVerbosity::Error)
			{
				UE_LOG(LogStageCraft, Error, TEXT("Scene export: glTF %s: %s"), Kind, *Messages[Index]);
			}
			else
			{
				UE_LOG(LogStageCraft, Warning, TEXT("Scene export: glTF %s: %s"), Kind, *Messages[Index]);
			}
		}
		if (Messages.Num() > MaxLoggedMessages)
		{
			UE_LOG(LogStageCraft, Warning, TEXT("Scene export: %d more glTF %s not shown."), Messages.Num() - MaxLoggedMessages, Kind);
		}
	}

	/**
	 * Geometry and the fixtures' lights (KHR_lights_punctual; ADR 0022 §2 as amended in §13). Gobos and light functions have no glTF
	 * form and stay behind. Material baking needs the editor (ADR 0022 §1).
	 */
	UGLTFExportOptions* MakeOptions()
	{
		UGLTFExportOptions* Options = NewObject<UGLTFExportOptions>(GetTransientPackage(), NAME_None, RF_Transient);
		Options->bExportLights = true;
		Options->bExportCameras = false;
		Options->bExportHiddenInGame = false;
		Options->bExportLevelSequences = false;
		Options->bExportAnimationSequences = false;
		Options->bIncludeCopyrightNotice = false;
		Options->BakeMaterialInputs = EGLTFMaterialBakeMode::Disabled;
		return Options;
	}

	/** Adds the scope's wall time to a timings field. */
	struct FPhaseTimer
	{
		double& Ms;
		const double Start = FPlatformTime::Seconds();
		explicit FPhaseTimer(double& InMs) : Ms(InMs) {}
		~FPhaseTimer() { Ms += (FPlatformTime::Seconds() - Start) * 1000.0; }
	};
}

FString FStageSceneExportTimings::ToString() const
{
	return FString::Printf(TEXT("load %.1f, rights %.1f, world %.1f, spawn %.1f, convert %.1f, glTF %.1f, finalize %.1f, teardown %.1f, hash %.1f; total %.1f ms"),
		LoadMs, RightsMs, WorldMs, SpawnMs, ConvertMs, GltfMs, FinalizeMs, TeardownMs, HashMs, GetTotalMs());
}

TMap<FSoftObjectPath, UBaseItemData*> FStageSceneExporter::CollectExportableItems(const UStageItemSubsystem& Items, const UStageProfileSubsystem& Profile)
{
	TMap<FSoftObjectPath, UBaseItemData*> Catalog;
	for (UBaseItemData* Item : Items.GetCatalog())
	{
		// The same ownership rule as placing the item: free, or the exact entitlement is owned.
		if (Item && (!Item->RequiredEntitlement.IsValid() || Profile.HasEntitlement(Item->RequiredEntitlement)))
		{
			Catalog.Add(FSoftObjectPath(Item), Item);
		}
	}
	return Catalog;
}

FStageActorExportContent FStageSceneExporter::InspectActor(const AActor& Actor)
{
	FStageActorExportContent Content;
	// The glTF exporter's own filter (FGLTFComponentConverter): only components hidden in game, or a hidden owner, are left out.
	if (Actor.IsHidden())
	{
		return Content;
	}
	TInlineComponentArray<USceneComponent*> Components(&Actor);
	for (const USceneComponent* Component : Components)
	{
		if (!Component || Component->bHiddenInGame)
		{
			continue;
		}
		if (const UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(Component))
		{
			Content.bHasGeometry |= Instanced->GetStaticMesh() != nullptr && Instanced->GetInstanceCount() > 0;
		}
		else if (const UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Component))
		{
			Content.bHasGeometry |= Mesh->GetStaticMesh() != nullptr;
		}
		else if (Component->IsA<ULightComponent>())
		{
			++Content.Lights;
		}
	}
	return Content;
}

void FStageSceneExporter::AddActorContent(const AActor& Actor, FStageSceneExportCounts& Counts)
{
	const FStageActorExportContent Content = InspectActor(Actor);
	Counts.Lights += Content.Lights;
	if (!Content.bHasGeometry)
	{
		// Kept in the file (its lights may still count), but named: a part that did not load is a content problem to fix.
		++Counts.WithoutGeometry;
		const AModularBaseActor* Item = Cast<AModularBaseActor>(&Actor);
		const UBaseItemData* Data = Item ? Item->GetItemData() : nullptr;
		UE_LOG(LogStageCraft, Warning, TEXT("Scene export: %s (%s) produced no geometry."), *Actor.GetName(), *GetNameSafe(Data));
	}
}

int32 FStageSceneExporter::ConvertProceduralMeshes(AActor& Actor)
{
	TInlineComponentArray<UProceduralMeshComponent*> Procedurals(&Actor);
	int32 Converted = 0;
	for (UProceduralMeshComponent* Procedural : Procedurals)
	{
		if (!Procedural || !Procedural->IsVisible() || Procedural->GetNumSections() == 0)
		{
			continue;
		}
		FMeshDescription Description = BuildMeshDescription(Procedural);
		if (Description.Triangles().Num() == 0)
		{
			continue;
		}

		UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);
		// Material slots in the order BuildMeshDescription creates its polygon groups: the first use of each section material.
		TArray<UMaterialInterface*> Materials;
		for (int32 Section = 0; Section < Procedural->GetNumSections(); ++Section)
		{
			UMaterialInterface* Material = Procedural->GetMaterial(Section);
			if (!Material)
			{
				Material = UMaterial::GetDefaultMaterial(MD_Surface);
			}
			if (!Materials.Contains(Material))
			{
				Materials.Add(Material);
				Mesh->GetStaticMaterials().Add(FStaticMaterial(Material, Material->GetFName()));
			}
		}

		UStaticMesh::FBuildMeshDescriptionsParams Params;
		Params.bMarkPackageDirty = false;
		Params.bCommitMeshDescription = false;
		// The fast path is the only one in builds without the editor; CPU access lets the exporter read the vertices directly.
		Params.bFastBuild = true;
		Params.bAllowCpuAccess = true;
		if (!Mesh->BuildFromMeshDescriptions({ &Description }, Params))
		{
			UE_LOG(LogStageCraft, Warning, TEXT("Scene export: could not convert %s of %s; it is left out."), *Procedural->GetName(), *Actor.GetName());
			continue;
		}

		UStaticMeshComponent* Copy = NewObject<UStaticMeshComponent>(&Actor, NAME_None, RF_Transient);
		Copy->SetStaticMesh(Mesh);
		if (USceneComponent* Parent = Procedural->GetAttachParent())
		{
			Copy->SetupAttachment(Parent, Procedural->GetAttachSocketName());
			Copy->SetRelativeTransform(Procedural->GetRelativeTransform());
		}
		else
		{
			Copy->SetupAttachment(Procedural);
		}
		Copy->RegisterComponent();
		Actor.AddInstanceComponent(Copy);
		++Converted;
	}
	return Converted;
}


// --- Shared decisions and wording ---

FStageBridgeResultInfo FStageSceneExporter::ValidateRequest(const FString& SceneName, const FString& Path)
{
	if (!FStageSceneStore::IsValidSceneName(SceneName) || FPaths::IsRelative(Path) || !Path.EndsWith(FileExtension, ESearchCase::IgnoreCase))
	{
		return FStageBridgeResultInfo::Fail(EStageBridgeResult::InvalidRequest, LOCTEXT("InvalidRequest", "Pick a saved scene and a .glb file to export to."));
	}
	return FStageBridgeResultInfo();
}

FStageBridgeResultInfo FStageSceneExporter::CheckRenderer()
{
	// Without a renderer the exporter cannot read mesh data that lives on the GPU only (cooked meshes without CPU access).
	if (!FApp::CanEverRender() || !GEngine)
	{
		return FStageBridgeResultInfo::Fail(EStageBridgeResult::Unavailable, LOCTEXT("NoRenderer", "Exporting needs a renderer; it is not available in this mode."));
	}
	return FStageBridgeResultInfo();
}

FStageBridgeResultInfo FStageSceneExporter::DescribeLoadFailure(EStageSceneResult LoadResult, const FString& SceneName)
{
	if (LoadResult == EStageSceneResult::NotFound || LoadResult == EStageSceneResult::InvalidName)
	{
		return FStageBridgeResultInfo::Fail(EStageBridgeResult::NotFound, FText::Format(LOCTEXT("NotFound", "There is no saved scene \"{0}\"."), FText::FromString(SceneName)));
	}
	return FStageBridgeResultInfo::Fail(EStageBridgeResult::Unreadable, FText::Format(LOCTEXT("Unreadable", "Scene \"{0}\" could not be read."), FText::FromString(SceneName)));
}

FStageBridgeResultInfo FStageSceneExporter::DescribeNoRights(const FString& SceneName, const FText& Reason)
{
	return FStageBridgeResultInfo::Fail(EStageBridgeResult::NotOriginal, Reason.IsEmpty()
		? FText::Format(LOCTEXT("NotOriginal", "\"{0}\" came from the Community Marketplace and cannot be exported."), FText::FromString(SceneName))
		: Reason);
}

FStageBridgeResultInfo FStageSceneExporter::DescribeNothingToExport(const FString& SceneName)
{
	FStageBridgeResultInfo Result = FStageBridgeResultInfo::Fail(EStageBridgeResult::NothingToExport,
		FText::Format(LOCTEXT("NothingToExport", "\"{0}\" has nothing that can be exported (no items from your Library)."), FText::FromString(SceneName)));
	Result.SceneName = SceneName;
	return Result;
}

FStageBridgeResultInfo FStageSceneExporter::DescribeExportFailed(const FString& SceneName, const TArray<FString>& Errors)
{
	FStageBridgeResultInfo Result = FStageBridgeResultInfo::Fail(EStageBridgeResult::ExportFailed,
		FText::Format(LOCTEXT("ExportFailed", "\"{0}\" could not be exported: {1}"), FText::FromString(SceneName),
			Errors.IsEmpty() ? LOCTEXT("SeeLog", "see the log for details.") : FText::FromString(Errors[0])));
	Result.SceneName = SceneName;
	return Result;
}

FStageBridgeResultInfo FStageSceneExporter::DescribeWriteFailed(const FString& Path)
{
	return FStageBridgeResultInfo::Fail(EStageBridgeResult::WriteFailed, FText::Format(LOCTEXT("WriteFailed", "Could not write {0}."), FText::FromString(Path)));
}

FStageBridgeResultInfo FStageSceneExporter::DescribeExported(const FString& SceneName, const FStageSceneExportCounts& Counts, const FString& Path)
{
	FStageBridgeResultInfo Result;
	Result.SceneName = SceneName;
	Result.FilePath = Path;
	Result.ExportedItems = Counts.Exported;
	Result.SkippedItems = Counts.NotInLibrary;
	Result.HiddenItems = Counts.Hidden;
	Result.InvalidItems = Counts.InvalidInFile;
	Result.ItemsWithoutGeometry = Counts.WithoutGeometry;
	Result.ExportedLights = Counts.Lights;

	const FText Exported = Counts.Lights > 0
		? FText::Format(LOCTEXT("ExportedWithLights", "Exported \"{0}\": {1} items and {2} lights."), FText::FromString(SceneName), FText::AsNumber(Counts.Exported), FText::AsNumber(Counts.Lights))
		: FText::Format(LOCTEXT("Exported", "Exported \"{0}\": {1} items."), FText::FromString(SceneName), FText::AsNumber(Counts.Exported));
	const FText NoGeometry = Counts.WithoutGeometry > 0
		? FText::Format(LOCTEXT("NoGeometry", " {0} of them have no geometry (a part that did not load; see the log)."), FText::AsNumber(Counts.WithoutGeometry))
		: FText::GetEmpty();
	Result.Message = FText::Format(LOCTEXT("ExportedSummary", "{0}{1}{2}"), Exported, DescribeLeftOut(Result), NoGeometry);
	return Result;
}

FText FStageSceneExporter::DescribeLeftOut(const FStageBridgeResultInfo& Result)
{
	TArray<FText> Parts;
	if (Result.HiddenItems > 0)
	{
		Parts.Add(FText::Format(LOCTEXT("LeftOutHidden", "{0} hidden on stage"), FText::AsNumber(Result.HiddenItems)));
	}
	if (Result.SkippedItems > 0)
	{
		Parts.Add(FText::Format(LOCTEXT("LeftOutNotOwned", "{0} not owned or not in your Library"), FText::AsNumber(Result.SkippedItems)));
	}
	if (Result.InvalidItems > 0)
	{
		Parts.Add(FText::Format(LOCTEXT("LeftOutDamaged", "{0} damaged in the scene file"), FText::AsNumber(Result.InvalidItems)));
	}
	return Parts.IsEmpty() ? FText::GetEmpty() : FText::Format(LOCTEXT("LeftOut", " Left out: {0}."), FText::Join(LOCTEXT("LeftOutSeparator", ", "), Parts));
}

UGLTFExportOptions* FStageSceneExporter::MakeExportOptions()
{
	return StageSceneExporterImpl::MakeOptions();
}

void FStageSceneExporter::LogExportMessages(const FGLTFExportMessages& Messages)
{
	StageSceneExporterImpl::LogMessages(Messages.Errors, ELogVerbosity::Error, TEXT("errors"));
	StageSceneExporterImpl::LogMessages(Messages.Warnings, ELogVerbosity::Warning, TEXT("warnings"));
}

UWorld* FStageSceneExporter::CreateExportWorld()
{
	FWorldInitializationValues Values;
	Values.InitializeScenes(true)
		.AllowAudioPlayback(false)
		.RequiresHitProxies(false)
		.CreatePhysicsScene(false)
		.CreateNavigation(false)
		.CreateAISystem(false)
		.ShouldSimulatePhysics(false)
		.EnableTraceCollision(false)
		.SetTransactional(false)
		.CreateFXSystem(false)
		.CreateWorldPartition(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::GamePreview, /*bInformEngineOfWorld*/ false, TEXT("StageCraftExportWorld"), /*InWorldPackage*/ nullptr,
		/*bAddToRoot*/ true, ERHIFeatureLevel::Num, &Values);
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::GamePreview);
	Context.SetCurrentWorld(World);
	World->InitializeActorsForPlay(FURL());
	return World;
}

void FStageSceneExporter::DestroyExportWorld(UWorld* World)
{
	if (!World)
	{
		return;
	}
	if (GEngine)
	{
		GEngine->DestroyWorldContext(World);
	}
	// Left to the engine's periodic collection. Each destroyed world kept until then makes the next one slower to create (14 -> 49
	// ms over ten back-to-back exports), but forcing a full purge here measured worse: a 390 ms frame for the first purge and tens of
	// ms in the frame after each later one (STATE.md #55).
	World->DestroyWorld(/*bInformEngineOfWorld*/ false);
}

FString FStageSceneExporter::GetTempPath(const FString& Path)
{
	// Written next to the target and moved into place: a reader (the add-on, the user's file browser) never sees a partial file.
	return FPaths::Combine(FPaths::GetPath(Path), FPaths::GetBaseFilename(Path) + TEXT(".writing") + FileExtension);
}

// --- The synchronous export (tests, and the measured baseline of StageCraft.Bench.Export) ---

FStageBridgeResultInfo FStageSceneExporter::ExportDocument(const FStageSceneDocument& Scene, const TMap<FSoftObjectPath, UBaseItemData*>& Catalog, const FString& Path,
	FStageSceneExportTimings* OutTimings, int32 InvalidItemsInFile)
{
	using namespace StageSceneExporterImpl;
	TRACE_CPUPROFILER_EVENT_SCOPE(StageSceneExport_Document);

	const FStageBridgeResultInfo Renderer = CheckRenderer();
	if (!Renderer.IsSuccess())
	{
		return Renderer;
	}
	FStageSceneExportTimings LocalTimings;
	FStageSceneExportTimings& Timings = OutTimings ? *OutTimings : LocalTimings;

	// A world like the marketplace preview's (ADR 0021 §12.3): GamePreview, actors initialized, play never begun, so nothing in it
	// registers with the stage, the show or the DMX output.
	UWorld* World = nullptr;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(StageSceneExport_World);
		FPhaseTimer Phase(Timings.WorldMs);
		World = CreateExportWorld();
	}
	ON_SCOPE_EXIT
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(StageSceneExport_Teardown);
		FPhaseTimer Phase(Timings.TeardownMs);
		DestroyExportWorld(World);
	};

	TArray<TObjectPtr<AActor>> Actors;
	FBox Bounds(ForceInit);
	FStageMarketplacePreviewStats Stats;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(StageSceneExport_Spawn);
		FPhaseTimer Phase(Timings.SpawnMs);
		Stats = StageMarketplacePreview::SpawnScene(*World, Scene, Catalog, Actors, Bounds, EStageHiddenItems::Skip);
	}
	if (Stats.Placed == 0)
	{
		return DescribeNothingToExport(Scene.Name);
	}

	int32 Converted = 0;
	TSet<AActor*> Selected;
	FStageSceneExportCounts Counts;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(StageSceneExport_Convert);
		FPhaseTimer Phase(Timings.ConvertMs);
		for (AActor* Actor : Actors)
		{
			Converted += ConvertProceduralMeshes(*Actor);
			Selected.Add(Actor);
			AddActorContent(*Actor, Counts);
		}
	}
	Counts.Exported = Stats.Placed;
	Counts.NotInLibrary = Stats.NotInLibrary;
	Counts.Hidden = Stats.Hidden;
	Counts.InvalidInFile = InvalidItemsInFile;

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree*/ true);
	const FString TempPath = GetTempPath(Path);
	FGLTFExportMessages Messages;
	bool bExported = false;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(StageSceneExport_Gltf);
		FPhaseTimer Phase(Timings.GltfMs);
		bExported = UGLTFExporter::ExportToGLTF(World, TempPath, MakeOptions(), Selected, Messages);
	}
	LogExportMessages(Messages);
	if (!bExported || !IFileManager::Get().FileExists(*TempPath))
	{
		IFileManager::Get().Delete(*TempPath, /*RequireExists*/ false, /*EvenReadOnly*/ false, /*Quiet*/ true);
		return DescribeExportFailed(Scene.Name, Messages.Errors);
	}

	int64 Bytes = 0;
	{
		FPhaseTimer Phase(Timings.FinalizeMs);
		if (!IFileManager::Get().Move(*Path, *TempPath, /*Replace*/ true, /*EvenIfReadOnly*/ false, /*Attributes*/ false, /*bDoNotRetryOrError*/ true))
		{
			IFileManager::Get().Delete(*TempPath, /*RequireExists*/ false, /*EvenReadOnly*/ false, /*Quiet*/ true);
			return DescribeWriteFailed(Path);
		}
		Bytes = IFileManager::Get().FileSize(*Path);
	}

	// Teardown runs after this line (scope exit) and is not in this total.
	UE_LOG(LogStageCraft, Log, TEXT("Scene export: \"%s\" -> %s: %d items, %d lights (%d procedural parts converted; left out: %d unavailable, %d hidden, %d damaged), %lld bytes; %s."),
		*Scene.Name, *Path, Counts.Exported, Counts.Lights, Converted, Counts.NotInLibrary, Counts.Hidden, Counts.InvalidInFile, Bytes, *Timings.ToString());
	return DescribeExported(Scene.Name, Counts, Path);
}

FStageBridgeResultInfo FStageSceneExporter::ExportSavedScene(UGameInstance& GameInstance, const FStageSceneStore& Scenes, const FString& SceneName, const FString& Path,
	FStageSceneExportTimings* OutTimings)
{
	using namespace StageSceneExporterImpl;
	const FStageBridgeResultInfo Request = ValidateRequest(SceneName, Path);
	if (!Request.IsSuccess())
	{
		return Request;
	}
	FStageSceneExportTimings LocalTimings;
	FStageSceneExportTimings& Timings = OutTimings ? *OutTimings : LocalTimings;

	FStageSceneDocument Scene;
	FStageSceneParseReport Report;
	EStageSceneResult Loaded = EStageSceneResult::NotFound;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(StageSceneExport_Load);
		FPhaseTimer Phase(Timings.LoadMs);
		Loaded = Scenes.Load(SceneName, Scene, Report);
	}
	if (Loaded != EStageSceneResult::Success)
	{
		return DescribeLoadFailure(Loaded, SceneName);
	}

	// Deny by default: a marketplace stage, under any name it was saved as, leaves only when its publisher allowed it (ADR 0022 §11.1).
	if (Scene.IsFromMarketplace())
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(StageSceneExport_Rights);
		FPhaseTimer Phase(Timings.RightsMs);
		const UStageMarketplaceSubsystem* Marketplace = GameInstance.GetSubsystem<UStageMarketplaceSubsystem>();
		const FStageMarketplaceResultInfo Rights = Marketplace
			? Marketplace->CheckExportRights(Scene.OriginListingId)
			: FStageMarketplaceResultInfo::Fail(EStageMarketplaceResult::NotOriginal, FText::GetEmpty());
		if (!Rights.IsSuccess())
		{
			return DescribeNoRights(SceneName, Rights.Message);
		}
	}

	const UStageItemSubsystem* Items = GameInstance.GetSubsystem<UStageItemSubsystem>();
	const UStageProfileSubsystem* Profile = GameInstance.GetSubsystem<UStageProfileSubsystem>();
	if (!Items || !Profile)
	{
		return FStageBridgeResultInfo::Fail(EStageBridgeResult::Unavailable, LOCTEXT("Unavailable", "The item Library is not available."));
	}

	FStageBridgeResultInfo Result = ExportDocument(Scene, CollectExportableItems(*Items, *Profile), Path, &Timings, Report.InvalidItems);
	Result.SceneName = SceneName;
	return Result;
}

#undef LOCTEXT_NAMESPACE
