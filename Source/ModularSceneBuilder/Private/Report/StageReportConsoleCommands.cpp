// Copyright Epic Games, Inc. All Rights Reserved.

// Development console commands for the bill of materials (Docs/ADR/0005-spans-outliner-and-bill-of-materials.md §5), the patch
// sheet (Docs/ADR/0011-cue-list-and-patch-sheet-export.md §3) and the cable schedule (Docs/ADR/0012 §2.3): log the
// report the panel shows, and export it through the controller's request. Compiled out of Shipping.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "ModularSceneBuilder.h"
#include "Player/ModularPlayerController.h"
#include "Report/StageBillOfMaterialsSubsystem.h"

namespace StageReportCommands
{
	void Status(const TArray<FString>& Args, UWorld* World)
	{
		const UStageBillOfMaterialsSubsystem* Report = UWorld::GetSubsystem<UStageBillOfMaterialsSubsystem>(World);
		if (!Report)
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Report: no bill of materials in this world."));
			return;
		}

		const FStageBillOfMaterials& Current = Report->GetReport();
		for (const FStageBomGroup& Group : Current.Groups)
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Report: [%s] %d units in %d placements, %.2f kg, %.1f W"), *Group.Label.ToString(),
				Group.Quantity, Group.Placements, Group.TotalWeightKg, Group.TotalPowerWatts);
			for (const FStageBomLine& Line : Group.Lines)
			{
				UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Report:     %s: %d x (%.2f kg, %.1f W) in %d placements = %.2f kg, %.1f W"), *Line.ItemName.ToString(),
					Line.Quantity, Line.UnitWeightKg, Line.UnitPowerWatts, Line.Placements, Line.TotalWeightKg, Line.TotalPowerWatts);
			}
		}
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Report: TOTAL %d lines, %d units, %d placements, %.2f kg, %.1f W; last export: %s"), Current.LineCount,
			Current.Quantity, Current.Placements, Current.TotalWeightKg, Current.TotalPowerWatts,
			Report->GetLastExport().Message.IsEmpty() ? TEXT("none") : *Report->GetLastExport().Message.ToString());
	}

	void Export(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = World ? Cast<AModularPlayerController>(World->GetFirstPlayerController()) : nullptr;
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Report.Export: %s"), Controller && Controller->RequestExportBillOfMaterials() ? TEXT("started") : TEXT("refused"));
	}

	void ExportPatch(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = World ? Cast<AModularPlayerController>(World->GetFirstPlayerController()) : nullptr;
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Report.ExportPatch: %s"), Controller && Controller->RequestExportPatchSheet() ? TEXT("started") : TEXT("refused"));
	}

	void ExportCables(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = World ? Cast<AModularPlayerController>(World->GetFirstPlayerController()) : nullptr;
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Report.ExportCables: %s"), Controller && Controller->RequestExportCableSchedule() ? TEXT("started") : TEXT("refused"));
	}

	FAutoConsoleCommandWithWorldAndArgs ExportCablesCommand(TEXT("StageCraft.Report.ExportCables"), TEXT("Exports the cable schedule and power distribution plan as PDF + CSV (File > Production)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ExportCables));
	FAutoConsoleCommandWithWorldAndArgs ExportPatchCommand(TEXT("StageCraft.Report.ExportPatch"), TEXT("Exports the DMX patch sheet as PDF + CSV (the Patch panel's EXPORT PDF)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ExportPatch));
	FAutoConsoleCommandWithWorldAndArgs StatusCommand(TEXT("StageCraft.Report.Status"), TEXT("Logs the bill of materials: lines by discipline, subtotals and totals."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Status));
	FAutoConsoleCommandWithWorldAndArgs ExportCommand(TEXT("StageCraft.Report.Export"), TEXT("Exports the bill of materials as PDF + CSV (the panel's EXPORT PDF)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Export));
}

#endif // !UE_BUILD_SHIPPING
