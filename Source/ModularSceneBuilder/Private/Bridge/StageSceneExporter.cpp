// Copyright Epic Games, Inc. All Rights Reserved.

#include "Bridge/StageSceneExporter.h"

#include "Components/StaticMeshComponent.h"
#include "Data/BaseItemData.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Exporters/GLTFExporter.h"
#include "HAL/FileManager.h"
#include "Marketplace/StageMarketplacePreview.h"
#include "Materials/Material.h"
#include "MeshDescription.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ModularSceneBuilder.h"
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

	/** Geometry only: lights would arrive in other apps at unrelated intensities, and material baking needs the editor (ADR 0022 §1). */
	UGLTFExportOptions* MakeOptions()
	{
		UGLTFExportOptions* Options = NewObject<UGLTFExportOptions>(GetTransientPackage(), NAME_None, RF_Transient);
		Options->bExportLights = false;
		Options->bExportCameras = false;
		Options->bExportHiddenInGame = false;
		Options->bExportLevelSequences = false;
		Options->bExportAnimationSequences = false;
		Options->bIncludeCopyrightNotice = false;
		Options->BakeMaterialInputs = EGLTFMaterialBakeMode::Disabled;
		return Options;
	}
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

FStageBridgeResultInfo FStageSceneExporter::ExportDocument(const FStageSceneDocument& Scene, const TMap<FSoftObjectPath, UBaseItemData*>& Catalog, const FString& Path)
{
	using namespace StageSceneExporterImpl;

	// Without a renderer the exporter cannot read mesh data that lives on the GPU only (cooked meshes without CPU access).
	if (!FApp::CanEverRender() || !GEngine)
	{
		return FStageBridgeResultInfo::Fail(EStageBridgeResult::Unavailable, LOCTEXT("NoRenderer", "Exporting needs a renderer; it is not available in this mode."));
	}
	const double StartSeconds = FPlatformTime::Seconds();

	// A world like the marketplace preview's (ADR 0021 §12.3): GamePreview, actors initialized, play never begun, so nothing in it
	// registers with the stage, the show or the DMX output.
	UWorld* World = UWorld::CreateWorld(EWorldType::GamePreview, /*bInformEngineOfWorld*/ false, TEXT("StageCraftExportWorld"));
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::GamePreview);
	Context.SetCurrentWorld(World);
	World->InitializeActorsForPlay(FURL());
	ON_SCOPE_EXIT
	{
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(/*bInformEngineOfWorld*/ false);
	};

	TArray<TObjectPtr<AActor>> Actors;
	FBox Bounds(ForceInit);
	const FStageMarketplacePreviewStats Stats = StageMarketplacePreview::SpawnScene(*World, Scene, Catalog, Actors, Bounds);

	FStageBridgeResultInfo Result;
	Result.SceneName = Scene.Name;
	Result.ExportedItems = Stats.Placed;
	Result.SkippedItems = Stats.NotInLibrary;
	if (Stats.Placed == 0)
	{
		Result.Code = EStageBridgeResult::NothingToExport;
		Result.Message = FText::Format(LOCTEXT("NothingToExport", "\"{0}\" has nothing that can be exported (no items from your Library)."), FText::FromString(Scene.Name));
		return Result;
	}

	int32 Converted = 0;
	TSet<AActor*> Selected;
	for (AActor* Actor : Actors)
	{
		Converted += ConvertProceduralMeshes(*Actor);
		Selected.Add(Actor);
	}

	// Written next to the target and moved into place: a reader (the add-on, the user's file browser) never sees a partial file.
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree*/ true);
	const FString TempPath = FPaths::Combine(FPaths::GetPath(Path), FPaths::GetBaseFilename(Path) + TEXT(".writing") + FileExtension);
	FGLTFExportMessages Messages;
	const bool bExported = UGLTFExporter::ExportToGLTF(World, TempPath, MakeOptions(), Selected, Messages);
	LogMessages(Messages.Errors, ELogVerbosity::Error, TEXT("errors"));
	LogMessages(Messages.Warnings, ELogVerbosity::Warning, TEXT("warnings"));

	if (!bExported || !IFileManager::Get().FileExists(*TempPath))
	{
		IFileManager::Get().Delete(*TempPath, /*RequireExists*/ false, /*EvenReadOnly*/ false, /*Quiet*/ true);
		Result.Code = EStageBridgeResult::ExportFailed;
		Result.Message = FText::Format(LOCTEXT("ExportFailed", "\"{0}\" could not be exported: {1}"), FText::FromString(Scene.Name),
			Messages.Errors.IsEmpty() ? LOCTEXT("SeeLog", "see the log for details.") : FText::FromString(Messages.Errors[0]));
		return Result;
	}
	if (!IFileManager::Get().Move(*Path, *TempPath, /*Replace*/ true, /*EvenIfReadOnly*/ false, /*Attributes*/ false, /*bDoNotRetryOrError*/ true))
	{
		IFileManager::Get().Delete(*TempPath, /*RequireExists*/ false, /*EvenReadOnly*/ false, /*Quiet*/ true);
		Result.Code = EStageBridgeResult::WriteFailed;
		Result.Message = FText::Format(LOCTEXT("WriteFailed", "Could not write {0}."), FText::FromString(Path));
		return Result;
	}

	const int64 Bytes = IFileManager::Get().FileSize(*Path);
	UE_LOG(LogStageCraft, Log, TEXT("Scene export: \"%s\" -> %s: %d items (%d procedural parts converted, %d left out), %lld bytes, %.2f s."),
		*Scene.Name, *Path, Stats.Placed, Converted, Stats.NotInLibrary, Bytes, FPlatformTime::Seconds() - StartSeconds);

	Result.Code = EStageBridgeResult::Success;
	Result.FilePath = Path;
	Result.Message = Stats.NotInLibrary > 0
		? FText::Format(LOCTEXT("ExportedSkipped", "Exported \"{0}\": {1} items ({2} left out: not in your Library or not owned)."), FText::FromString(Scene.Name), FText::AsNumber(Stats.Placed), FText::AsNumber(Stats.NotInLibrary))
		: FText::Format(LOCTEXT("Exported", "Exported \"{0}\": {1} items."), FText::FromString(Scene.Name), FText::AsNumber(Stats.Placed));
	return Result;
}

FStageBridgeResultInfo FStageSceneExporter::ExportSavedScene(UGameInstance& GameInstance, const FStageSceneStore& Scenes, const FString& SceneName, const FString& Path)
{
	if (!FStageSceneStore::IsValidSceneName(SceneName) || FPaths::IsRelative(Path) || !Path.EndsWith(FileExtension, ESearchCase::IgnoreCase))
	{
		return FStageBridgeResultInfo::Fail(EStageBridgeResult::InvalidRequest, LOCTEXT("InvalidRequest", "Pick a saved scene and a .glb file to export to."));
	}

	FStageSceneDocument Scene;
	FStageSceneParseReport Report;
	switch (Scenes.Load(SceneName, Scene, Report))
	{
	case EStageSceneResult::Success:
		break;
	case EStageSceneResult::NotFound:
	case EStageSceneResult::InvalidName:
		return FStageBridgeResultInfo::Fail(EStageBridgeResult::NotFound, FText::Format(LOCTEXT("NotFound", "There is no saved scene \"{0}\"."), FText::FromString(SceneName)));
	default:
		return FStageBridgeResultInfo::Fail(EStageBridgeResult::Unreadable, FText::Format(LOCTEXT("Unreadable", "Scene \"{0}\" could not be read."), FText::FromString(SceneName)));
	}

	// Deny by default: a bought stage stays inside StageCraft, under any name it was saved as.
	if (Scene.IsFromMarketplace())
	{
		return FStageBridgeResultInfo::Fail(EStageBridgeResult::NotOriginal,
			FText::Format(LOCTEXT("NotOriginal", "\"{0}\" came from the Community Marketplace; only your own stages can be exported."), FText::FromString(SceneName)));
	}

	const UStageItemSubsystem* Items = GameInstance.GetSubsystem<UStageItemSubsystem>();
	const UStageProfileSubsystem* Profile = GameInstance.GetSubsystem<UStageProfileSubsystem>();
	if (!Items || !Profile)
	{
		return FStageBridgeResultInfo::Fail(EStageBridgeResult::Unavailable, LOCTEXT("Unavailable", "The item Library is not available."));
	}

	FStageBridgeResultInfo Result = ExportDocument(Scene, CollectExportableItems(*Items, *Profile), Path);
	Result.SceneName = SceneName;
	return Result;
}

#undef LOCTEXT_NAMESPACE
