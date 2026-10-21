// Copyright Epic Games, Inc. All Rights Reserved.

#include "Report/StageCableScheduleSubsystem.h"

#include "Actors/LightingFixtureActor.h"
#include "Actors/ModularBaseActor.h"
#include "Async/Async.h"
#include "Data/BaseItemData.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "ModularSceneBuilder.h"
#include "Report/StageCableSchedule.h"
#include "Report/StagePowerDistributionSettings.h"
#include "Report/StageReportFiles.h"
#include "Subsystems/StageSessionSubsystem.h"
#include "Tasks/Task.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageCableScheduleSubsystem)

#define LOCTEXT_NAMESPACE "StageCableSchedule"

bool UStageCableScheduleSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// Same worlds as the session it reports on.
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UStageCableScheduleSubsystem::Deinitialize()
{
	// A result still on its way belongs to a world that is gone.
	++ExportSerial;
	bExporting = false;
	OnExportFinished.Clear();
	Super::Deinitialize();
}

void UStageCableScheduleSubsystem::GatherInputs(TArray<FStagePowerLoad>& OutLoads, TArray<FStageDmxNode>& OutNodes) const
{
	const UStageSessionSubsystem* Session = UWorld::GetSubsystem<UStageSessionSubsystem>(GetWorld());
	if (!Session)
	{
		return;
	}

	const TArray<AModularBaseActor*> Items = Session->GetPlacedItems();
	OutLoads.Reserve(Items.Num());
	for (const AModularBaseActor* Item : Items)
	{
		const UBaseItemData* Data = Item ? Item->GetItemData() : nullptr;
		if (!Data || Item->IsActorBeingDestroyed())
		{
			continue;
		}

		// The one place a fixture differs from other equipment here: it has an ID and a DMX patch.
		const ALightingFixtureActor* Fixture = Cast<ALightingFixtureActor>(Item);
		const double UnitWatts = Data->Specs.PowerDrawWatts;
		if (UnitWatts > 0.0 || Fixture)
		{
			FStagePowerLoad& Load = OutLoads.AddDefaulted_GetRef();
			Load.Label = Item->GetInstanceLabel().ToString();
			Load.ItemType = Data->DisplayName.IsEmpty() ? Data->GetName() : Data->DisplayName.ToString();
			Load.FixtureId = Fixture ? Fixture->GetFixtureId() : 0;
			Load.Watts = UnitWatts * FMath::Max(Item->GetQuantity(), 1);
		}

		if (Fixture)
		{
			const FStageDMXPatch Patch = Fixture->GetPatch();
			FStageDmxNode& Node = OutNodes.AddDefaulted_GetRef();
			Node.FixtureId = Fixture->GetFixtureId();
			Node.Label = Item->GetInstanceLabel().ToString();
			Node.Universe = Patch.Universe;
			Node.Address = Patch.Address;
			Node.Footprint = Fixture->GetDMXFootprint();
			// Unreal units are centimetres; crews work in metres.
			Node.LocationMeters = Fixture->GetActorLocation() / 100.0;
		}
	}
}

bool UStageCableScheduleSubsystem::HasPlannableEquipment() const
{
	TArray<FStagePowerLoad> Loads;
	TArray<FStageDmxNode> Nodes;
	GatherInputs(Loads, Nodes);
	return !Loads.IsEmpty() || !Nodes.IsEmpty();
}

bool UStageCableScheduleSubsystem::StartExport(FText& OutReason)
{
	if (bExporting)
	{
		OutReason = LOCTEXT("ExportBusy", "The cable schedule is already being exported.");
		return false;
	}

	const UStageSessionSubsystem* Session = UWorld::GetSubsystem<UStageSessionSubsystem>(GetWorld());
	if (!Session)
	{
		OutReason = LOCTEXT("NoSession", "The cable schedule is not available in this level.");
		return false;
	}

	TArray<FStagePowerLoad> Loads;
	TArray<FStageDmxNode> Nodes;
	GatherInputs(Loads, Nodes);
	if (Loads.IsEmpty() && Nodes.IsEmpty())
	{
		OutReason = LOCTEXT("ExportEmpty", "Nothing on the stage draws power or takes DMX: there is no cable schedule to export.");
		return false;
	}

	const FStagePowerDistribution Supply = GetDefault<UStagePowerDistributionSettings>()->ToDistribution();
	const FString SceneName = Session->GetSceneName();
	const FDateTime Now = FDateTime::Now();
	const FString Directory = StageReportFiles::GetOutputDirectory();
	const FString Stem = StageCableSchedule::MakeFileStem(SceneName, Now);

	bExporting = true;
	const uint64 Serial = ++ExportSerial;
	UE_LOG(LogStageCraft, Log, TEXT("Cable schedule: exporting %d loads and %d fixtures (%d-phase %.0f V, %.0f A circuits at %.0f %%) to %s/%s (.pdf, .csv)."),
		Loads.Num(), Nodes.Num(), Supply.PhaseCount, Supply.PhaseVoltage, Supply.CircuitRatingAmps, Supply.UsableShare * 100.0, *Directory, *Stem);

	// The inputs are plain values gathered above; the worker plans, renders and writes from its own copies and touches no UObject.
	UE::Tasks::Launch(UE_SOURCE_LOCATION, [WeakThis = TWeakObjectPtr<UStageCableScheduleSubsystem>(this), Serial, Loads = MoveTemp(Loads), Nodes = MoveTemp(Nodes),
		Supply, SceneName, Now, Directory, Stem]() mutable
	{
		const FStageCableSchedule Schedule = StageCableSchedule::Build(MoveTemp(Loads), MoveTemp(Nodes), Supply, SceneName, Now);
		const TArray<uint8> Pdf = StageCableSchedule::ToPdf(Schedule);
		const FString Csv = StageCableSchedule::ToCsv(Schedule);

		IFileManager::Get().MakeDirectory(*Directory, /*Tree*/ true);
		const FString BasePath = StageReportFiles::MakeUniqueStemPath(Directory, Stem, { TEXT(".pdf"), TEXT(".csv") });
		const FString PdfPath = BasePath + TEXT(".pdf");
		const bool bWritten = StageReportFiles::WritePdfAndCsv(BasePath, Pdf, Csv);
		const int32 LoadCount = Schedule.Loads.Num();
		const int32 CircuitCount = Schedule.Circuits.Num();
		const int32 WarningCount = StageCableSchedule::GetWarnings(Schedule).Num();

		AsyncTask(ENamedThreads::GameThread, [WeakThis, Serial, bWritten, PdfPath, LoadCount, CircuitCount, WarningCount]()
		{
			if (UStageCableScheduleSubsystem* Self = WeakThis.Get())
			{
				Self->FinishExport(Serial, bWritten, PdfPath, LoadCount, CircuitCount, WarningCount);
			}
		});
	});
	return true;
}

void UStageCableScheduleSubsystem::FinishExport(uint64 Serial, bool bWritten, const FString& PdfPath, int32 LoadCount, int32 CircuitCount, int32 WarningCount)
{
	if (Serial != ExportSerial)
	{
		return;
	}

	bExporting = false;
	LastExport.bSucceeded = bWritten;
	LastExport.FilePath = PdfPath;
	if (!bWritten)
	{
		LastExport.Message = FText::Format(LOCTEXT("ExportFailed", "Could not write {0}. Check that the folder is writable and the file is not open in another program."),
			FText::FromString(PdfPath));
	}
	else if (WarningCount > 0)
	{
		LastExport.Message = FText::Format(LOCTEXT("ExportDoneWarnings", "Exported the cable schedule {0} and .csv ({1} {1}|plural(one=load,other=loads) on {2} {2}|plural(one=circuit,other=circuits)); {3} {3}|plural(one=warning,other=warnings) on page 1."),
			FText::FromString(FPaths::GetCleanFilename(PdfPath)), LoadCount, CircuitCount, WarningCount);
	}
	else
	{
		LastExport.Message = FText::Format(LOCTEXT("ExportDone", "Exported the cable schedule {0} and .csv ({1} {1}|plural(one=load,other=loads) on {2} {2}|plural(one=circuit,other=circuits))."),
			FText::FromString(FPaths::GetCleanFilename(PdfPath)), LoadCount, CircuitCount);
	}

	UE_CLOG(bWritten, LogStageCraft, Log, TEXT("Cable schedule: wrote %s and its CSV (%d loads, %d circuits, %d warnings)."), *PdfPath, LoadCount, CircuitCount, WarningCount);
	UE_CLOG(!bWritten, LogStageCraft, Warning, TEXT("Cable schedule: could not write %s."), *PdfPath);
	OnExportFinished.Broadcast(LastExport);
}

#undef LOCTEXT_NAMESPACE
