// Copyright Epic Games, Inc. All Rights Reserved.

#include "Import/StageMeshImportSubsystem.h"

#include "Async/Async.h"
#include "Data/StageImportedItemData.h"
#include "Engine/AssetManager.h"
#include "Engine/StaticMesh.h"
#include "Engine/StreamableManager.h"
#include "HAL/FileManager.h"
#include "Import/StageInterchangeGameUtilities.h"
#include "Import/StageModelThumbnailRenderer.h"
#include "InterchangeGenericAssetsPipeline.h"
#include "InterchangeGenericAssetsPipelineSharedSettings.h"
#include "InterchangeGenericAnimationPipeline.h"
#include "InterchangeGenericMaterialPipeline.h"
#include "InterchangeGenericMeshPipeline.h"
#include "InterchangeManager.h"
#include "InterchangeResult.h"
#include "InterchangeResultsContainer.h"
#include "InterchangeSourceData.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ModularSceneBuilder.h"
#include "Subsystems/StageItemSubsystem.h"
#include "Tasks/Task.h"
#include "Workspace/StageLayoutStore.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageMeshImportSubsystem)

#define LOCTEXT_NAMESPACE "StageMeshImport"

/** The import in flight. The Interchange result keeps the imported objects alive until they are collected on the game thread. */
struct FStageActiveMeshImport
{
	TSharedPtr<UE::Interchange::FImportResult, ESPMode::ThreadSafe> Result;
	FString SourceFile;
	EStageMeshImportOrigin Origin = EStageMeshImportOrigin::User;
	uint64 Serial = 0;
};

namespace StageMeshImportPrivate
{
	FText FileLabel(const FString& File)
	{
		return FText::FromString(FPaths::GetCleanFilename(File));
	}

	FText ExtensionList(const TArray<FString>& Extensions)
	{
		return FText::FromString(FString::JoinBy(Extensions, TEXT(", "), [](const FString& Extension) { return TEXT(".") + Extension; }));
	}

	/** The first error the importer reported, or empty. */
	FText FirstError(const UInterchangeResultsContainer* Results)
	{
		if (Results)
		{
			for (const UInterchangeResult* Entry : Results->GetResults())
			{
				if (Entry && Entry->GetResultType() == EInterchangeResultType::Error)
				{
					return Entry->GetText();
				}
			}
		}
		return FText::GetEmpty();
	}
}

void UStageMeshImportSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	ItemSubsystem = Collection.InitializeDependency<UStageItemSubsystem>();

	UseRuntimeImportInGame();
	if (!ThumbnailMaterial.IsNull())
	{
		// Streamed now so the first thumbnail does not load it on the game thread; until it arrives the default material is used.
		ThumbnailMaterialHandle = UAssetManager::GetStreamableManager().RequestAsyncLoad(ThumbnailMaterial.ToSoftObjectPath());
	}
	LoadImportList();
}

void UStageMeshImportSubsystem::Deinitialize()
{
	// A late Interchange result finds no import with its serial and is dropped (FinishActiveImport).
	Active.Reset();
	Queue.Reset();
	ThumbnailRenderer.Reset();
	ThumbnailMaterialHandle.Reset();
	OnImportStarted.Clear();
	OnImportFinished.Clear();

	// The list is a few hundred bytes; waiting keeps the last import remembered when the app quits right after it.
	ImportListPipe.WaitUntilEmpty();

	Super::Deinitialize();
}

TArray<FStageMeshImportOutcome> UStageMeshImportSubsystem::RequestImport(const TArray<FString>& Files)
{
	TArray<FStageMeshImportOutcome> Refusals;
	for (const FString& File : Files)
	{
		const FString Normalized = StageMeshImport::NormalizeSourcePath(File);
		FStageMeshImportOutcome Refusal;
		if (ValidateRequest(Normalized, EStageMeshImportOrigin::User, Refusal))
		{
			Enqueue(Normalized, EStageMeshImportOrigin::User);
		}
		else
		{
			UE_LOG(LogStageCraft, Log, TEXT("Mesh import: refused %s (%s)."), *Normalized, *UEnum::GetValueAsString(Refusal.Result));
			Refusals.Add(MoveTemp(Refusal));
		}
	}
	StartNextImport();
	return Refusals;
}

TArray<FString> UStageMeshImportSubsystem::GetSupportedExtensions() const
{
	return StageMeshImport::FilterSupportedExtensions(
		UInterchangeManager::GetInterchangeManager().GetSupportedAssetTypeFormats(EInterchangeTranslatorAssetType::Meshes));
}

bool UStageMeshImportSubsystem::CanImportAny(const TArray<FString>& Files) const
{
	const TArray<FString> Supported = GetSupportedExtensions();
	return Files.ContainsByPredicate([&Supported](const FString& File) { return StageMeshImport::HasSupportedExtension(File, Supported); });
}

FString UStageMeshImportSubsystem::GetImportListPath()
{
	return FPaths::ProjectSavedDir() / TEXT("StageCraft") / TEXT("Imports.json");
}

bool UStageMeshImportSubsystem::ValidateRequest(const FString& NormalizedFile, EStageMeshImportOrigin Origin, FStageMeshImportOutcome& OutRefusal) const
{
	using namespace StageMeshImportPrivate;
	OutRefusal.SourceFile = NormalizedFile;
	OutRefusal.Origin = Origin;

	const TArray<FString> SupportedExtensions = GetSupportedExtensions();
	if (SupportedExtensions.IsEmpty())
	{
		OutRefusal.Result = EStageMeshImportResult::NotAllowed;
		OutRefusal.Message = LOCTEXT("NoImporter", "Model import is not available in this build.");
		return false;
	}
	if (!StageMeshImport::HasSupportedExtension(NormalizedFile, SupportedExtensions))
	{
		OutRefusal.Result = EStageMeshImportResult::UnsupportedFormat;
		OutRefusal.Message = FText::Format(LOCTEXT("Unsupported", "{0} cannot be imported. Supported formats: {1}."),
			FileLabel(NormalizedFile), ExtensionList(SupportedExtensions));
		return false;
	}
	if (!FPaths::FileExists(NormalizedFile))
	{
		OutRefusal.Result = EStageMeshImportResult::FileNotFound;
		OutRefusal.Message = FText::Format(LOCTEXT("NotFound", "{0} was not found at {1}."), FileLabel(NormalizedFile), FText::FromString(FPaths::GetPath(NormalizedFile)));
		return false;
	}
	if (IFileManager::Get().FileSize(*NormalizedFile) > StageMeshImport::MaxFileSizeBytes)
	{
		OutRefusal.Result = EStageMeshImportResult::FileTooLarge;
		OutRefusal.Message = FText::Format(LOCTEXT("TooLarge", "{0} is larger than {1} MB and was not imported."),
			FileLabel(NormalizedFile), FText::AsNumber(StageMeshImport::MaxFileSizeBytes / (1024 * 1024)));
		return false;
	}
	if (IsKnownFile(NormalizedFile))
	{
		OutRefusal.Result = EStageMeshImportResult::AlreadyImported;
		OutRefusal.Message = FText::Format(LOCTEXT("AlreadyImported", "{0} is already in the Library."), FileLabel(NormalizedFile));
		return false;
	}
	return true;
}

bool UStageMeshImportSubsystem::IsKnownFile(const FString& NormalizedFile) const
{
	auto Same = [&NormalizedFile](const FString& Other) { return Other.Equals(NormalizedFile, ESearchCase::IgnoreCase); };

	return ImportedItems.ContainsByPredicate([&Same](const UStageImportedItemData* Item) { return Item && Same(Item->GetSourceFile()); })
		|| Queue.ContainsByPredicate([&Same](const FQueuedImport& Queued) { return Same(Queued.SourceFile); })
		|| (Active.IsValid() && Same(Active->SourceFile));
}

void UStageMeshImportSubsystem::Enqueue(const FString& NormalizedFile, EStageMeshImportOrigin Origin)
{
	FQueuedImport& Entry = Queue.AddDefaulted_GetRef();
	Entry.SourceFile = NormalizedFile;
	Entry.Origin = Origin;
}

void UStageMeshImportSubsystem::UseRuntimeImportInGame()
{
#if WITH_EDITOR
	// Only a game process running on editor binaries needs this (see UStageInterchangeGameUtilities). Packaged builds already
	// import as runtime, and in the editor (PIE included) the editor plugin's utilities answer correctly.
	UInterchangeManager& Manager = UInterchangeManager::GetInterchangeManager();
	const UInterchangeEditorUtilitiesBase* Current = Manager.GetEditorUtilities();
	if (IsRunningGame() && !GIsEditor && (!Current || !Current->IsRuntimeOrPIE()))
	{
		Manager.SetEditorUtilities(UStageInterchangeGameUtilities::StaticClass());
		UE_LOG(LogStageCraft, Log, TEXT("Mesh import: standalone game on editor binaries; Interchange set to runtime import."));
	}
#endif
}

UInterchangePipelineBase* UStageMeshImportSubsystem::MakePipeline(const FString& SourceFile)
{
	UInterchangeGenericAssetsPipeline* Assets = NewObject<UInterchangeGenericAssetsPipeline>(this);

	// A stage prop is one rigid object: every mesh in the file becomes one static mesh, skinned meshes included.
	Assets->CommonMeshesProperties->ForceAllMeshAsType = EInterchangeForceMeshType::IFMT_StaticMesh;
	Assets->CommonSkeletalMeshesAndAnimationsProperties->bImportOnlyAnimations = false;
	Assets->MeshPipeline->bImportStaticMeshes = true;
	Assets->MeshPipeline->bImportSkeletalMeshes = false;
	Assets->MeshPipeline->bImportGeometryCaches = false;
	Assets->MeshPipeline->CombineStaticMeshesBehavior = EInterchangeCombineStaticMeshesBehavior::All;
	// Nanite cannot be built in a packaged game; disabling it keeps the editor and packaged builds producing the same mesh.
	Assets->MeshPipeline->bBuildNanite = false;
	Assets->AnimationPipeline->bImportAnimations = false;

	// New materials cannot be compiled in a packaged game; instances of Interchange's parent materials can.
	Assets->MaterialPipeline->bImportMaterials = true;
	Assets->MaterialPipeline->MaterialImport = EInterchangeMaterialImportOption::ImportAsMaterialInstances;

	const StageMeshImport::FImportOffset Offset = StageMeshImport::GetImportOffset(StageMeshImport::GetExtension(SourceFile));
	Assets->ImportOffsetRotation = Offset.Rotation;
	Assets->ImportOffsetUniformScale = static_cast<float>(Offset.UniformScale);
	return Assets;
}

void UStageMeshImportSubsystem::StartNextImport()
{
	if (Active.IsValid() || Queue.IsEmpty())
	{
		return;
	}

	const FQueuedImport Request = Queue[0];
	Queue.RemoveAt(0);

	const FString ContentPath = StageMeshImport::MakeItemPackagePath(StageMeshImport::MakeItemName(Request.SourceFile)) + TEXT("/Assets");
	FImportAssetParameters Parameters;
	Parameters.bIsAutomated = true;
	Parameters.bReplaceExisting = true;
	Pipeline = MakePipeline(Request.SourceFile);
	Parameters.OverridePipelines.Add(FSoftObjectPath(Pipeline));

	Active = MakeShared<FStageActiveMeshImport>();
	Active->SourceFile = Request.SourceFile;
	Active->Origin = Request.Origin;
	Active->Serial = ++ImportSerial;

	UE_LOG(LogStageCraft, Log, TEXT("Mesh import: importing %s into %s (%d more queued)."), *Request.SourceFile, *ContentPath, Queue.Num());
	OnImportStarted.Broadcast(Request.SourceFile);

	UInterchangeManager& Manager = UInterchangeManager::GetInterchangeManager();
	UInterchangeSourceData* Source = UInterchangeManager::CreateSourceData(Request.SourceFile);
	Active->Result = Manager.ImportAssetAsync(ContentPath, Source, Parameters);

	// Interchange may finish on another thread, or at once inside OnDone for a file it cannot read. Either way the result is
	// collected on the next game-thread tick, outside this call.
	Active->Result->OnDone([WeakThis = TWeakObjectPtr<UStageMeshImportSubsystem>(this), Serial = Active->Serial](UE::Interchange::FImportResult&)
	{
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Serial]()
		{
			if (UStageMeshImportSubsystem* Self = WeakThis.Get())
			{
				Self->FinishActiveImport(Serial);
			}
		});
	});
}

void UStageMeshImportSubsystem::FinishActiveImport(uint64 Serial)
{
	if (!Active.IsValid() || Active->Serial != Serial)
	{
		return;
	}

	const FStageMeshImportOutcome Outcome = CollectActiveImport();
	Active.Reset();

	UE_LOG(LogStageCraft, Log, TEXT("Mesh import: %s %s: %s"), *Outcome.SourceFile, *UEnum::GetValueAsString(Outcome.Result), *Outcome.Message.ToString());
	OnImportFinished.Broadcast(Outcome);
	StartNextImport();
}

FStageMeshImportOutcome UStageMeshImportSubsystem::CollectActiveImport()
{
	using namespace StageMeshImportPrivate;

	FStageMeshImportOutcome Outcome;
	Outcome.SourceFile = Active->SourceFile;
	Outcome.Origin = Active->Origin;

	UStaticMesh* Mesh = nullptr;
	for (UObject* Object : Active->Result->GetImportedObjects())
	{
		if (!Object)
		{
			continue;
		}
		ImportedAssets.Add(Object);
		if (UStaticMesh* Candidate = Cast<UStaticMesh>(Object); Candidate && !Mesh)
		{
			Mesh = Candidate;
		}
	}

	if (!Mesh || !Mesh->GetBoundingBox().IsValid)
	{
		const FText Error = FirstError(Active->Result->GetResults());
		Outcome.Result = Error.IsEmpty() ? EStageMeshImportResult::NoGeometry : EStageMeshImportResult::ImporterFailed;
		Outcome.Message = Error.IsEmpty()
			? FText::Format(LOCTEXT("NoGeometry", "{0} contains no model geometry to import."), FileLabel(Outcome.SourceFile))
			: FText::Format(LOCTEXT("ImporterFailed", "{0} could not be imported: {1}"), FileLabel(Outcome.SourceFile), Error);
		return Outcome;
	}

	Outcome.Item = RegisterItem(*Mesh, Outcome.SourceFile);
	Outcome.Result = EStageMeshImportResult::Success;
	Outcome.Message = FText::Format(LOCTEXT("Imported", "Imported {0} ({1}) into the Library."), Outcome.Item->DisplayName, FText::FromString(Outcome.Item->GetSourceFormat()));
	RememberFile(Outcome.SourceFile);
	return Outcome;
}

UStageImportedItemData* UStageMeshImportSubsystem::RegisterItem(UStaticMesh& Mesh, const FString& SourceFile)
{
	const FString ItemName = StageMeshImport::MakeItemName(SourceFile);
	UPackage* Package = CreatePackage(*StageMeshImport::MakeItemPackagePath(ItemName));
	UStageImportedItemData* Item = NewObject<UStageImportedItemData>(Package, *ItemName, RF_Transient);
	Item->InitializeFromImport(Mesh, SourceFile);
	if (!ThumbnailRenderer.IsValid())
	{
		ThumbnailRenderer = MakeShared<FStageModelThumbnailRenderer>();
	}
	Item->SetThumbnail(ThumbnailRenderer->Render(Mesh, ThumbnailMaterial.Get(), *Item));
	ImportedItems.Add(Item);

	if (UStageItemSubsystem* Items = ItemSubsystem.Get())
	{
		Items->AddRuntimeItem(*Item);
	}
	return Item;
}

void UStageMeshImportSubsystem::LoadImportList()
{
	UE::Tasks::Launch(UE_SOURCE_LOCATION, [WeakThis = TWeakObjectPtr<UStageMeshImportSubsystem>(this), Path = GetImportListPath()]()
	{
		FString Json;
		TArray<FString> Files;
		if (FFileHelper::LoadFileToString(Json, *Path) && !StageMeshImport::ReadImportList(Json, Files))
		{
			UE_LOG(LogStageCraft, Warning, TEXT("Mesh import: %s is not a valid import list; no models are restored."), *Path);
		}
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Files = MoveTemp(Files)]() mutable
		{
			if (UStageMeshImportSubsystem* Self = WeakThis.Get())
			{
				Self->HandleImportListLoaded(MoveTemp(Files));
			}
		});
	});
}

void UStageMeshImportSubsystem::HandleImportListLoaded(TArray<FString> Files)
{
	// Imports that finished while the list was being read are kept, after the remembered ones.
	TArray<FString> ImportedMeanwhile = MoveTemp(RememberedFiles);
	RememberedFiles = Files;
	bImportListLoaded = true;
	for (const FString& File : ImportedMeanwhile)
	{
		RememberFile(File);
	}

	UE_LOG(LogStageCraft, Log, TEXT("Mesh import: formats %s; restoring %d model(s) from %s."),
		*FString::Join(GetSupportedExtensions(), TEXT(", ")), Files.Num(), *GetImportListPath());
	for (const FString& File : Files)
	{
		FStageMeshImportOutcome Refusal;
		if (ValidateRequest(File, EStageMeshImportOrigin::Restore, Refusal))
		{
			Enqueue(File, EStageMeshImportOrigin::Restore);
		}
		else if (Refusal.Result != EStageMeshImportResult::AlreadyImported)
		{
			UE_LOG(LogStageCraft, Warning, TEXT("Mesh import: cannot restore %s: %s"), *File, *Refusal.Message.ToString());
			OnImportFinished.Broadcast(Refusal);
		}
	}
	StartNextImport();
}

void UStageMeshImportSubsystem::RememberFile(const FString& SourceFile)
{
	const bool bKnown = RememberedFiles.ContainsByPredicate([&SourceFile](const FString& File) { return File.Equals(SourceFile, ESearchCase::IgnoreCase); });
	if (bKnown || RememberedFiles.Num() >= StageMeshImport::MaxRememberedFiles)
	{
		return;
	}
	RememberedFiles.Add(SourceFile);
	if (bImportListLoaded)
	{
		SaveImportList();
	}
}

void UStageMeshImportSubsystem::SaveImportList()
{
	ImportListPipe.Launch(UE_SOURCE_LOCATION, [Json = StageMeshImport::WriteImportList(RememberedFiles), Path = GetImportListPath()]()
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree*/ true);
		if (!FStageLayoutStore::WriteFileAtomic(Path, Json))
		{
			UE_LOG(LogStageCraft, Warning, TEXT("Mesh import: could not write %s; imported models will not be restored next time."), *Path);
		}
	});
}

#undef LOCTEXT_NAMESPACE
