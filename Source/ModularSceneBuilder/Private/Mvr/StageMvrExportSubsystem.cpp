// Copyright Epic Games, Inc. All Rights Reserved.

#include "Mvr/StageMvrExportSubsystem.h"

#include "Async/Async.h"
#include "Engine/World.h"
#include "Fixtures/StageFixtureLibraryRules.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "IO/StageAtomicFile.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ModularSceneBuilder.h"
#include "Mvr/StageMvrExportGather.h"
#include "Mvr/StageMvrWriter.h"
#include "Tasks/Task.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageMvrExportSubsystem)

#define LOCTEXT_NAMESPACE "StageMvrExport"

bool UStageMvrExportSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

bool UStageMvrExportSubsystem::IsExportPath(const FString& Path)
{
	return !Path.IsEmpty() && !FPaths::IsRelative(Path) && Path.EndsWith(TEXT(".mvr"), ESearchCase::IgnoreCase)
		&& !FPaths::GetBaseFilename(Path).IsEmpty();
}

bool UStageMvrExportSubsystem::StartExport(const FString& Path, FText& OutReason)
{
	if (bExporting)
	{
		OutReason = LOCTEXT("Busy", "An MVR export is already being written.");
		return false;
	}
	if (!IsExportPath(Path))
	{
		OutReason = LOCTEXT("BadPath", "Choose a full path for an .mvr file.");
		return false;
	}
	const UWorld* World = GetWorld();
	if (!World)
	{
		OutReason = LOCTEXT("NoWorld", "There is no stage to export.");
		return false;
	}

	const double GatherStart = FPlatformTime::Seconds();
	StageMvrExportGather::FGathered Gathered = StageMvrExportGather::Gather(*World);
	const double GatherMs = (FPlatformTime::Seconds() - GatherStart) * 1000.0;
	if (Gathered.Scene.Fixtures.IsEmpty() && Gathered.Scene.Objects.IsEmpty())
	{
		OutReason = LOCTEXT("Empty", "The stage is empty: there is nothing to export.");
		return false;
	}

	FCounts Counts;
	Counts.Fixtures = Gathered.Fixtures;
	Counts.Trusses = Gathered.Trusses;
	Counts.OtherItems = Gathered.OtherItems;
	Counts.Skipped = Gathered.Skipped;
	bExporting = true;
	const uint64 Serial = ++ExportSerial;
	UE_LOG(LogStageCraft, Log, TEXT("MVR export: %d fixtures, %d trusses, %d other items, %d GDTF types to %s (read in %.2f ms)."),
		Counts.Fixtures, Counts.Trusses, Counts.OtherItems, Gathered.GdtfPaths.Num(), *Path, GatherMs);

	UE::Tasks::Launch(UE_SOURCE_LOCATION, [WeakThis = TWeakObjectPtr<UStageMvrExportSubsystem>(this), Serial, Path, Counts, Gathered = MoveTemp(Gathered)]()
	{
		const double Start = FPlatformTime::Seconds();
		TMap<FString, TArray<uint8>> GdtfFiles;
		for (const TPair<FString, FString>& File : Gathered.GdtfPaths)
		{
			const int64 Size = IFileManager::Get().FileSize(*File.Value);
			TArray<uint8> Bytes;
			if (Size > 0 && Size <= StageFixtureLibrary::MaxFileSizeBytes && FFileHelper::LoadFileToArray(Bytes, *File.Value))
			{
				GdtfFiles.Add(File.Key, MoveTemp(Bytes));
			}
		}
		StageMvrExport::FWriteResult Written = StageMvrExport::Write(Gathered.Scene, GdtfFiles);
		GdtfFiles.Empty();
		const FString Error = Written.Error;
		bool bWriteFailed = false;
		if (Written.IsValid())
		{
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree*/ true);
			if (!StageAtomicFile::WriteBytes(Path, Written.Bytes))
			{
				bWriteFailed = true;
			}
		}
		UE_LOG(LogStageCraft, Log, TEXT("MVR export: %lld bytes, %d GDTF files, %d geometry files built and written in %.1f ms."),
			int64(Written.Bytes.Num()), Written.GdtfFiles, Written.GeometryFiles, (FPlatformTime::Seconds() - Start) * 1000.0);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Serial, Path, Error, bWriteFailed, Notes = MoveTemp(Written.Notes), Counts]()
		{
			if (UStageMvrExportSubsystem* Self = WeakThis.Get())
			{
				Self->FinishExport(Serial, Path, Error, bWriteFailed, Notes, Counts);
			}
		});
	});
	return true;
}

void UStageMvrExportSubsystem::FinishExport(uint64 Serial, const FString& Path, const FString& Error, bool bWriteFailed, const TArray<FString>& Notes, FCounts Counts)
{
	if (Serial != ExportSerial)
	{
		return;
	}
	bExporting = false;
	LastExport.bSucceeded = Error.IsEmpty() && !bWriteFailed;
	LastExport.FilePath = Path;
	const FText FileName = FText::FromString(FPaths::GetCleanFilename(Path));
	if (bWriteFailed)
	{
		LastExport.Message = FText::Format(LOCTEXT("WriteFailed", "Could not write {0}. Check that the folder is writable and the file is not open in another program."), FText::FromString(Path));
	}
	else if (!Error.IsEmpty())
	{
		LastExport.Message = FText::Format(LOCTEXT("Refused", "Could not export {0}: {1}"), FileName, FText::FromString(Error));
	}
	else
	{
		FText Message = FText::Format(LOCTEXT("Done", "Exported {0}: {1} {1}|plural(one=fixture,other=fixtures), {2} {2}|plural(one=truss,other=trusses), {3} other {3}|plural(one=item,other=items)."),
			FileName, Counts.Fixtures, Counts.Trusses, Counts.OtherItems);
		if (!Notes.IsEmpty())
		{
			Message = FText::Format(LOCTEXT("DoneNotes", "{0} {1}{2}"), Message, FText::FromString(Notes[0]),
				Notes.Num() > 1 ? FText::Format(LOCTEXT("MoreNotes", " ({0} more in the log)"), Notes.Num() - 1) : FText::GetEmpty());
		}
		LastExport.Message = Message;
	}
	for (const FString& Note : Notes)
	{
		UE_LOG(LogStageCraft, Log, TEXT("MVR export: %s"), *Note);
	}
	UE_CLOG(LastExport.bSucceeded, LogStageCraft, Log, TEXT("MVR export: wrote %s (%d items left out: nothing to show)."), *Path, Counts.Skipped);
	UE_CLOG(!LastExport.bSucceeded, LogStageCraft, Warning, TEXT("MVR export: %s"), *LastExport.Message.ToString());
	OnExportFinished.Broadcast(LastExport);
}

#undef LOCTEXT_NAMESPACE
