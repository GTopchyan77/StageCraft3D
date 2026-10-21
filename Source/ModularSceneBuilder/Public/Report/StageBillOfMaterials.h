// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "UObject/SoftObjectPath.h"

/** One placed item's contribution to the bill of materials, as plain values copied from the item and its catalog entry. */
struct FStageBomSource
{
	/** Catalog identity: placements of the same catalog item merge into one line. */
	FSoftObjectPath ItemPath;
	FText ItemName;
	FText Manufacturer;
	/** The catalog category (any depth); lines are grouped by its discipline. */
	FGameplayTag Category;
	/** Per catalog unit (per segment for spans). */
	double UnitWeightKg = 0.0;
	double UnitPowerWatts = 0.0;
	/** Catalog units this placement consists of (AModularBaseActor::GetQuantity). */
	int32 Quantity = 1;
};

/** One catalog item on the stage: how many units, in how many placements, and what they weigh and draw. */
struct FStageBomLine
{
	FSoftObjectPath ItemPath;
	FText ItemName;
	FText Manufacturer;
	/** Units to pick from the warehouse: pieces, or segments for spans. */
	int32 Quantity = 0;
	/** Placed items (runs for spans); equal to Quantity for single pieces. */
	int32 Placements = 0;
	double UnitWeightKg = 0.0;
	double UnitPowerWatts = 0.0;
	double TotalWeightKg = 0.0;
	double TotalPowerWatts = 0.0;
};

/** The lines of one discipline with its subtotals. */
struct FStageBomGroup
{
	/** Top-level category (StageOutliner::GetGroupCategory); invalid for "Other". */
	FGameplayTag Category;
	FText Label;
	TArray<FStageBomLine> Lines;
	int32 Quantity = 0;
	int32 Placements = 0;
	double TotalWeightKg = 0.0;
	double TotalPowerWatts = 0.0;
};

/** The production report of a stage: every catalog item with quantities, combined weight and power, by discipline. */
struct FStageBillOfMaterials
{
	TArray<FStageBomGroup> Groups;
	int32 LineCount = 0;
	int32 Quantity = 0;
	int32 Placements = 0;
	double TotalWeightKg = 0.0;
	double TotalPowerWatts = 0.0;

	bool IsEmpty() const { return Groups.IsEmpty(); }
};

/**
 * Pure aggregation and export of the bill of materials (Docs/ADR/0005-spans-outliner-and-bill-of-materials.md §5).
 * Deterministic for the same sources in any order; covered by the StageCraft.Report tests. Any thread.
 */
namespace StageBillOfMaterials
{
	/**
	 * Merges Sources by catalog item and groups the lines by discipline, in the outliner's order; lines in natural name order.
	 * Totals are computed from the sorted lines, so the same stage gives the same numbers whatever order its items registered in.
	 * Defensive against bad data: a quantity below 1 counts as 1 (every placement is at least one unit), and a non-finite or
	 * negative weight or power counts as 0, so no total can become negative or NaN.
	 */
	MODULARSCENEBUILDER_API FStageBillOfMaterials Build(TConstArrayView<FStageBomSource> Sources);

	/**
	 * The report as CSV (RFC 4180, comma-separated, "." decimals, CRLF lines) for spreadsheets and rental quotes: a title block
	 * (scene, date), a header row, one row per line, a subtotal per discipline and a grand total. Starts with a UTF-8 byte order
	 * mark so spreadsheet programs read non-ASCII names correctly.
	 */
	MODULARSCENEBUILDER_API FString ToCsv(const FStageBillOfMaterials& Report, const FString& SceneName, const FDateTime& GeneratedAt);

	/**
	 * The report as a print-ready A4 landscape PDF in black and grey (Docs/ADR/0012-production-reports-and-fixture-benchmark.md
	 * §2.2): a heading per discipline with its lines and a subtotal row, then the grand total; the title block repeats on every
	 * page with the totals. Names outside Windows-1252 print as '?' (FStagePdfDocument); the CSV keeps them. An empty report is
	 * one page saying the stage is empty.
	 */
	MODULARSCENEBUILDER_API TArray<uint8> ToPdf(const FStageBillOfMaterials& Report, const FString& SceneName, const FDateTime& GeneratedAt);

	/**
	 * One CSV field: quoted when it holds a comma, quote or line break (quotes doubled). Text that a spreadsheet would run as a
	 * formula (starting with =, +, -, @, tab or carriage return) is prefixed with an apostrophe, so a catalog name can never execute.
	 */
	MODULARSCENEBUILDER_API FString EscapeCsvField(const FString& Field);

	/** "BOM_<Scene>_<yyyyMMdd-HHmmss>.csv"; an empty or unusable scene name reads "Untitled". */
	MODULARSCENEBUILDER_API FString MakeFileName(const FString& SceneName, const FDateTime& GeneratedAt);
}
