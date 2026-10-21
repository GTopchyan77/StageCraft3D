// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Report/StageBillOfMaterials.h"
#include "StageBillOfMaterialsSubsystem.generated.h"

/** Outcome of writing the bill of materials to disk. */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageReportExportResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Report")
	bool bSucceeded = false;

	/** The file written (success) or attempted (failure); for a PDF + CSV pair, the PDF (the CSV has the same name with ".csv"). */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Report")
	FString FilePath;

	/** User-facing summary, e.g. "Exported BOM_Arena_20261013-141500.csv (12 lines)." */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Report")
	FText Message;
};

/** The report changed (items added, removed, resized, swapped). Views re-read GetReport on the next tick. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnStageBillOfMaterialsChanged);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStageReportExportFinished, const FStageReportExportResult&, Result);

/**
 * The production report of the stage in this world (Docs/ADR/0005-spans-outliner-and-bill-of-materials.md §5): every catalog item
 * with its quantity, combined weight and power draw, grouped by discipline, and its export as PDF + CSV to Saved/StageCraft/Reports.
 *
 * Derived state only. The source of truth is UStageSessionSubsystem's placed items; the report is a cache that is marked stale on
 * the session's OnItemListChanged and rebuilt on the next GetReport, so a burst of edits (a scene load) costs one rebuild. Nothing
 * ticks, polls or iterates the world. Read access for everyone; exporting is requested through the controller (RequestExportBillOfMaterials).
 *
 * Export runs on a worker (UE::Tasks): the PDF and CSV are built on the game thread from the current report, then written atomically
 * off it; the result returns on the game thread to a weakly held subsystem. One export at a time; results that arrive after the
 * world went away are dropped.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageBillOfMaterialsSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem Interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	/** The current report, rebuilt first if the stage changed since the last call. Game thread only. */
	const FStageBillOfMaterials& GetReport() const;

	/**
	 * Writes the current report as PDF and CSV (Docs/ADR/0012-production-reports-and-fixture-benchmark.md §2.2). Returns false with OutReason when it cannot start (already exporting, empty stage);
	 * otherwise the result arrives on OnExportFinished.
	 */
	bool StartExport(FText& OutReason);

	UFUNCTION(BlueprintPure, Category = "StageCraft|Report")
	bool IsExporting() const { return bExporting; }

	UFUNCTION(BlueprintPure, Category = "StageCraft|Report")
	const FStageReportExportResult& GetLastExport() const { return LastExport; }

	/** Saved/StageCraft/Reports, absolute. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Report")
	FString GetOutputDirectory() const;

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Report")
	FOnStageBillOfMaterialsChanged OnReportChanged;

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Report")
	FOnStageReportExportFinished OnExportFinished;

protected:
	//~ Begin UWorldSubsystem Interface
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	//~ End UWorldSubsystem Interface

private:
	UFUNCTION()
	void HandleItemListChanged();

	void FinishExport(uint64 Serial, bool bWritten, const FString& FilePath, int32 LineCount);

	TWeakObjectPtr<class UStageSessionSubsystem> Session;

	/** Derived from the session's items; rebuilt lazily by GetReport while bReportStale. */
	mutable FStageBillOfMaterials CachedReport;
	mutable bool bReportStale = true;

	FStageReportExportResult LastExport;
	bool bExporting = false;

	/** Identifies the running export; bumped on Deinitialize so a late result from a finished world is ignored. */
	uint64 ExportSerial = 0;
};
