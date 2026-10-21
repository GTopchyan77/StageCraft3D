// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Report/StageBillOfMaterialsSubsystem.h"
#include "StageCableScheduleSubsystem.generated.h"

/**
 * Exports the stage's cable schedule and power distribution plan as a print-ready PDF and a CSV beside it, to
 * Saved/StageCraft/Reports (Docs/ADR/0012-production-reports-and-fixture-benchmark.md §2.3).
 *
 * Holds no plan: the loads and DMX patch are read on the game thread from UStageSessionSubsystem (placed items, catalog power)
 * and the fixtures' patch when an export starts, with the supply from UStagePowerDistributionSettings. The plan is built,
 * rendered and written (both files or neither) on a worker (UE::Tasks); the result returns on the game thread to a weakly held
 * subsystem. One export at a time; a result that arrives after the world went away is dropped. Requested through
 * AModularPlayerController::RequestExportCableSchedule. No tick.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageCableScheduleSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem Interface
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	/**
	 * Starts writing the cable schedule. Returns false with OutReason when it cannot start (already exporting, no stage session,
	 * nothing on stage that draws power or takes DMX); otherwise the result arrives on OnExportFinished. Game thread.
	 */
	bool StartExport(FText& OutReason);

	UFUNCTION(BlueprintPure, Category = "StageCraft|Report")
	bool IsExporting() const { return bExporting; }

	UFUNCTION(BlueprintPure, Category = "StageCraft|Report")
	const FStageReportExportResult& GetLastExport() const { return LastExport; }

	/** Whether the stage has anything to plan: equipment that draws power, or a lighting fixture. Game thread. */
	bool HasPlannableEquipment() const;

	/** FilePath of the result is the PDF; the CSV has the same name with ".csv". */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Report")
	FOnStageReportExportFinished OnExportFinished;

protected:
	//~ Begin UWorldSubsystem Interface
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	//~ End UWorldSubsystem Interface

private:
	/** Every placed item's power draw and every lighting fixture's patch, as plain values. Game thread. */
	void GatherInputs(TArray<struct FStagePowerLoad>& OutLoads, TArray<struct FStageDmxNode>& OutNodes) const;

	void FinishExport(uint64 Serial, bool bWritten, const FString& PdfPath, int32 LoadCount, int32 CircuitCount, int32 WarningCount);

	FStageReportExportResult LastExport;
	bool bExporting = false;

	/** Identifies the running export; bumped on Deinitialize so a late result from a finished world is ignored. */
	uint64 ExportSerial = 0;
};
