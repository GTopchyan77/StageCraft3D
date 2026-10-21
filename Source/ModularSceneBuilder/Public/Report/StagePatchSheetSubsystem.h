// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Report/StageBillOfMaterialsSubsystem.h"
#include "StagePatchSheetSubsystem.generated.h"

/**
 * Exports the stage's DMX patch sheet as a print-ready PDF and a CSV beside it, to Saved/StageCraft/Reports
 * (Docs/ADR/0011-cue-list-and-patch-sheet-export.md §3.2).
 *
 * Holds no patch state: the rows are read on the game thread from UShowControlSubsystem (fixtures, patch, conflicts) when an
 * export starts; both documents are rendered and written atomically on a worker (UE::Tasks), and the result returns on the
 * game thread to a weakly held subsystem. One export at a time; a result that arrives after the world went away is dropped.
 * Requested through AModularPlayerController::RequestExportPatchSheet. No tick.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStagePatchSheetSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem Interface
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	/**
	 * Starts writing the patch sheet. Returns false with OutReason when it cannot start (already exporting, no show control,
	 * no fixtures); otherwise the result arrives on OnExportFinished. Game thread.
	 */
	bool StartExport(FText& OutReason);

	UFUNCTION(BlueprintPure, Category = "StageCraft|Report")
	bool IsExporting() const { return bExporting; }

	UFUNCTION(BlueprintPure, Category = "StageCraft|Report")
	const FStageReportExportResult& GetLastExport() const { return LastExport; }

	/** FilePath of the result is the PDF; the CSV has the same name with ".csv". */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Report")
	FOnStageReportExportFinished OnExportFinished;

protected:
	//~ Begin UWorldSubsystem Interface
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	//~ End UWorldSubsystem Interface

private:
	/** The rows of every registered fixture, as the patch sheet prints them. Game thread. */
	TArray<struct FStagePatchSheetRow> GatherRows(const class UShowControlSubsystem& ShowControl) const;

	void FinishExport(uint64 Serial, bool bWritten, const FString& PdfPath, int32 FixtureCount);

	FStageReportExportResult LastExport;
	bool bExporting = false;

	/** Identifies the running export; bumped on Deinitialize so a late result from a finished world is ignored. */
	uint64 ExportSerial = 0;
};
