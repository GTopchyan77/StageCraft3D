// Copyright Epic Games, Inc. All Rights Reserved.

#include "Report/StagePatchSheetSubsystem.h"

#include "Actors/LightingFixtureActor.h"
#include "Async/Async.h"
#include "Data/LightingFixtureData.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "ModularSceneBuilder.h"
#include "Report/StagePatchSheet.h"
#include "Report/StageReportFiles.h"
#include "Subsystems/ShowControlSubsystem.h"
#include "Subsystems/StageDMXPatchSubsystem.h"
#include "Subsystems/StageSessionSubsystem.h"
#include "Tasks/Task.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StagePatchSheetSubsystem)

#define LOCTEXT_NAMESPACE "StagePatchSheet"

bool UStagePatchSheetSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// Same worlds as the show control it reports on.
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UStagePatchSheetSubsystem::Deinitialize()
{
	// A result still on its way belongs to a world that is gone.
	++ExportSerial;
	bExporting = false;
	OnExportFinished.Clear();
	Super::Deinitialize();
}

TArray<FStagePatchSheetRow> UStagePatchSheetSubsystem::GatherRows(const UShowControlSubsystem& ShowControl) const
{
	const TArray<ALightingFixtureActor*> Fixtures = ShowControl.GetFixtures();
	const UStageDMXPatchSubsystem* Patching = UWorld::GetSubsystem<UStageDMXPatchSubsystem>(ShowControl.GetWorld());
	const TArray<int32> Conflicts = Patching ? Patching->FindPatchConflicts() : TArray<int32>();

	TArray<FStagePatchSheetRow> Rows;
	Rows.Reserve(Fixtures.Num());
	for (const ALightingFixtureActor* Fixture : Fixtures)
	{
		const ULightingFixtureData* Data = Fixture->GetFixtureData();
		const FStageDMXPatch Patch = Fixture->GetPatch();

		FStagePatchSheetRow& Row = Rows.AddDefaulted_GetRef();
		Row.FixtureId = Fixture->GetFixtureId();
		Row.Label = Fixture->GetInstanceLabel().ToString();
		Row.FixtureType = Data ? Data->DisplayName.ToString() : FString();
		Row.Mode = Data ? Data->DMXModeName.ToString() : FString();
		Row.Universe = Patch.Universe;
		Row.Address = Patch.Address;
		Row.Footprint = Fixture->GetDMXFootprint();
		// Unreal units are centimetres; crews work in metres.
		Row.LocationMeters = Fixture->GetActorLocation() / 100.0;
		Row.bConflict = Conflicts.Contains(Row.FixtureId);
	}
	return Rows;
}

bool UStagePatchSheetSubsystem::StartExport(FText& OutReason)
{
	if (bExporting)
	{
		OutReason = LOCTEXT("ExportBusy", "The patch sheet is already being exported.");
		return false;
	}

	const UShowControlSubsystem* ShowControl = UWorld::GetSubsystem<UShowControlSubsystem>(GetWorld());
	if (!ShowControl)
	{
		OutReason = LOCTEXT("NoShowControl", "The patch sheet is not available in this level.");
		return false;
	}

	TArray<FStagePatchSheetRow> Rows = GatherRows(*ShowControl);
	if (Rows.IsEmpty())
	{
		OutReason = LOCTEXT("ExportEmpty", "There are no lighting fixtures on the stage: there is no patch sheet to export.");
		return false;
	}

	const UStageSessionSubsystem* Session = UWorld::GetSubsystem<UStageSessionSubsystem>(GetWorld());
	const FString SceneName = Session ? Session->GetSceneName() : FString();
	const FDateTime Now = FDateTime::Now();
	const FString Directory = StageReportFiles::GetOutputDirectory();
	const FString Stem = StagePatchSheet::MakeFileStem(SceneName, Now);
	const int32 FixtureCount = Rows.Num();

	bExporting = true;
	const uint64 Serial = ++ExportSerial;
	UE_LOG(LogStageCraft, Log, TEXT("Patch sheet: exporting %d fixtures to %s/%s (.pdf, .csv)."), FixtureCount, *Directory, *Stem);

	// The rows are plain values gathered above; the worker renders and writes from its own copy and touches no UObject.
	UE::Tasks::Launch(UE_SOURCE_LOCATION, [WeakThis = TWeakObjectPtr<UStagePatchSheetSubsystem>(this), Serial, Rows = MoveTemp(Rows), SceneName, Now, Directory, Stem, FixtureCount]() mutable
	{
		const FStagePatchSheet Sheet = StagePatchSheet::Build(MoveTemp(Rows), SceneName, Now);
		const TArray<uint8> Pdf = StagePatchSheet::ToPdf(Sheet);
		const FString Csv = StagePatchSheet::ToCsv(Sheet);

		IFileManager::Get().MakeDirectory(*Directory, /*Tree*/ true);
		const FString BasePath = StageReportFiles::MakeUniqueStemPath(Directory, Stem, { TEXT(".pdf"), TEXT(".csv") });
		const FString PdfPath = BasePath + TEXT(".pdf");
		const bool bWritten = StageReportFiles::WritePdfAndCsv(BasePath, Pdf, Csv);

		AsyncTask(ENamedThreads::GameThread, [WeakThis, Serial, bWritten, PdfPath, FixtureCount]()
		{
			if (UStagePatchSheetSubsystem* Self = WeakThis.Get())
			{
				Self->FinishExport(Serial, bWritten, PdfPath, FixtureCount);
			}
		});
	});
	return true;
}

void UStagePatchSheetSubsystem::FinishExport(uint64 Serial, bool bWritten, const FString& PdfPath, int32 FixtureCount)
{
	if (Serial != ExportSerial)
	{
		return;
	}

	bExporting = false;
	LastExport.bSucceeded = bWritten;
	LastExport.FilePath = PdfPath;
	LastExport.Message = bWritten
		? FText::Format(LOCTEXT("ExportDone", "Exported the patch sheet {0} and .csv ({1} {1}|plural(one=fixture,other=fixtures))."),
			FText::FromString(FPaths::GetCleanFilename(PdfPath)), FixtureCount)
		: FText::Format(LOCTEXT("ExportFailed", "Could not write {0}. Check that the folder is writable and the file is not open in another program."),
			FText::FromString(PdfPath));

	UE_CLOG(bWritten, LogStageCraft, Log, TEXT("Patch sheet: wrote %s and its CSV."), *PdfPath);
	UE_CLOG(!bWritten, LogStageCraft, Warning, TEXT("Patch sheet: could not write %s."), *PdfPath);
	OnExportFinished.Broadcast(LastExport);
}

#undef LOCTEXT_NAMESPACE
