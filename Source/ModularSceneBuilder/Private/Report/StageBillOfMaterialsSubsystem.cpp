// Copyright Epic Games, Inc. All Rights Reserved.

#include "Report/StageBillOfMaterialsSubsystem.h"

#include "Actors/ModularBaseActor.h"
#include "Async/Async.h"
#include "Data/BaseItemData.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "ModularSceneBuilder.h"
#include "Report/StageReportFiles.h"
#include "Subsystems/StageSessionSubsystem.h"
#include "Tasks/Task.h"
#include "Workspace/StageLayoutStore.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageBillOfMaterialsSubsystem)

#define LOCTEXT_NAMESPACE "StageBillOfMaterials"

bool UStageBillOfMaterialsSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// Same worlds as the session it reports on.
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UStageBillOfMaterialsSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	UStageSessionSubsystem* SessionSubsystem = Collection.InitializeDependency<UStageSessionSubsystem>();
	Session = SessionSubsystem;
	if (SessionSubsystem)
	{
		SessionSubsystem->OnItemListChanged.AddUniqueDynamic(this, &ThisClass::HandleItemListChanged);
	}
	else
	{
		UE_LOG(LogStageCraft, Warning, TEXT("Bill of materials: no stage session in %s; the report stays empty."), *GetNameSafe(GetWorld()));
	}
}

void UStageBillOfMaterialsSubsystem::Deinitialize()
{
	if (UStageSessionSubsystem* SessionSubsystem = Session.Get())
	{
		SessionSubsystem->OnItemListChanged.RemoveDynamic(this, &ThisClass::HandleItemListChanged);
	}
	Session.Reset();

	// A write still running finishes on its worker; its result no longer matches and is dropped.
	++ExportSerial;
	bExporting = false;
	OnReportChanged.Clear();
	OnExportFinished.Clear();
	Super::Deinitialize();
}

void UStageBillOfMaterialsSubsystem::HandleItemListChanged()
{
	bReportStale = true;
	OnReportChanged.Broadcast();
}

const FStageBillOfMaterials& UStageBillOfMaterialsSubsystem::GetReport() const
{
	if (!bReportStale)
	{
		return CachedReport;
	}

	TArray<FStageBomSource> Sources;
	if (const UStageSessionSubsystem* SessionSubsystem = Session.Get())
	{
		const TArray<AModularBaseActor*> Items = SessionSubsystem->GetPlacedItems();
		Sources.Reserve(Items.Num());
		for (const AModularBaseActor* Item : Items)
		{
			const UBaseItemData* Data = Item ? Item->GetItemData() : nullptr;
			if (!Data || Item->IsActorBeingDestroyed())
			{
				continue;
			}

			FStageBomSource& Source = Sources.AddDefaulted_GetRef();
			Source.ItemPath = FSoftObjectPath(Data);
			Source.ItemName = Data->DisplayName.IsEmpty() ? FText::FromName(Data->GetFName()) : Data->DisplayName;
			Source.Manufacturer = Data->Specs.Manufacturer;
			Source.Category = Data->CategoryTag;
			Source.UnitWeightKg = Data->Specs.WeightKg;
			Source.UnitPowerWatts = Data->Specs.PowerDrawWatts;
			Source.Quantity = Item->GetQuantity();
		}
	}

	CachedReport = StageBillOfMaterials::Build(Sources);
	bReportStale = false;
	return CachedReport;
}

FString UStageBillOfMaterialsSubsystem::GetOutputDirectory() const
{
	return StageReportFiles::GetOutputDirectory();
}

bool UStageBillOfMaterialsSubsystem::StartExport(FText& OutReason)
{
	if (bExporting)
	{
		OutReason = LOCTEXT("ExportBusy", "The bill of materials is already being exported.");
		return false;
	}

	const FStageBillOfMaterials& Report = GetReport();
	if (Report.IsEmpty())
	{
		OutReason = LOCTEXT("ExportEmpty", "The stage is empty: there is no bill of materials to export.");
		return false;
	}

	const UStageSessionSubsystem* SessionSubsystem = Session.Get();
	const FString SceneName = SessionSubsystem ? SessionSubsystem->GetSceneName() : FString();
	const FDateTime Now = FDateTime::Now();
	FString Csv = StageBillOfMaterials::ToCsv(Report, SceneName, Now);
	const FString Directory = GetOutputDirectory();
	const FString Path = Directory / StageBillOfMaterials::MakeFileName(SceneName, Now);
	const int32 LineCount = Report.LineCount;

	bExporting = true;
	const uint64 Serial = ++ExportSerial;
	UE_LOG(LogStageCraft, Log, TEXT("Bill of materials: exporting %d lines (%d units, %.1f kg, %.0f W) to %s."),
		Report.LineCount, Report.Quantity, Report.TotalWeightKg, Report.TotalPowerWatts, *Path);

	// The text is complete before the task starts; the worker only touches the file system and its own copies.
	UE::Tasks::Launch(UE_SOURCE_LOCATION, [WeakThis = TWeakObjectPtr<UStageBillOfMaterialsSubsystem>(this), Serial, Csv = MoveTemp(Csv), Directory, Path, LineCount]()
	{
		IFileManager::Get().MakeDirectory(*Directory, /*Tree*/ true);
		const FString FinalPath = StageReportFiles::MakeUniqueStemPath(Directory, FPaths::GetBaseFilename(Path), { TEXT(".csv") }) + TEXT(".csv");
		const bool bWritten = FStageLayoutStore::WriteFileAtomic(FinalPath, Csv);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Serial, bWritten, FinalPath, LineCount]()
		{
			if (UStageBillOfMaterialsSubsystem* Self = WeakThis.Get())
			{
				Self->FinishExport(Serial, bWritten, FinalPath, LineCount);
			}
		});
	});
	return true;
}

void UStageBillOfMaterialsSubsystem::FinishExport(uint64 Serial, bool bWritten, const FString& FilePath, int32 LineCount)
{
	if (Serial != ExportSerial)
	{
		return;
	}

	bExporting = false;
	LastExport.bSucceeded = bWritten;
	LastExport.FilePath = FilePath;
	LastExport.Message = bWritten
		? FText::Format(LOCTEXT("ExportDone", "Exported {0} ({1} lines)."), FText::FromString(FPaths::GetCleanFilename(FilePath)), FText::AsNumber(LineCount))
		: FText::Format(LOCTEXT("ExportFailed", "Could not write {0}. Check that the folder is writable and the file is not open in another program."),
			FText::FromString(FilePath));

	UE_CLOG(bWritten, LogStageCraft, Log, TEXT("Bill of materials: wrote %s."), *FilePath);
	UE_CLOG(!bWritten, LogStageCraft, Warning, TEXT("Bill of materials: could not write %s."), *FilePath);
	OnExportFinished.Broadcast(LastExport);
}

#undef LOCTEXT_NAMESPACE
