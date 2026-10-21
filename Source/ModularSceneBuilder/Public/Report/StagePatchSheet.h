// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/DateTime.h"

/** One fixture on the patch sheet, read from the stage on the game thread by the caller. */
struct FStagePatchSheetRow
{
	int32 FixtureId = 0;
	/** Instance label shown on stage. */
	FString Label;
	/** Catalog display name, e.g. "Spot 300". */
	FString FixtureType;
	/** DMX mode name; may be empty. */
	FString Mode;
	/** 0 = unpatched. */
	int32 Universe = 0;
	/** 1..512 when patched. */
	int32 Address = 0;
	/** Channels used. */
	int32 Footprint = 0;
	FVector LocationMeters = FVector::ZeroVector;
	/** Its channel range overlaps another fixture in the same universe; computed by the caller from the show's patch. */
	bool bConflict = false;
};

/** The DMX patch sheet of a stage: every lighting fixture with its address, footprint and position, for the lighting crew. */
struct FStagePatchSheet
{
	FString SceneName;
	FDateTime GeneratedAt;
	/** Sorted by fixture ID, then label (natural order). */
	TArray<FStagePatchSheetRow> Rows;
	int32 PatchedCount = 0;
	int32 UnpatchedCount = 0;
	int32 ConflictCount = 0;
	/** Universes used by patched fixtures, ascending, each once. */
	TArray<int32> Universes;

	bool IsEmpty() const { return Rows.IsEmpty(); }
};

/**
 * Pure model and export of the DMX patch sheet (CSV for spreadsheets, PDF for print). Deterministic for the same rows in any
 * order; covered by the StageCraft.Report.PatchSheet and StageCraft.Report.Pdf tests. Any thread.
 */
namespace StagePatchSheet
{
	/**
	 * Sanitizes and sorts Rows (fixture ID, then label in natural order, then universe and address) and counts them.
	 * Defensive against bad data, so the paperwork never prints an address that cannot exist:
	 * - a negative footprint counts as 0 channels;
	 * - a row with a universe below 1 or an address outside 1..512 is unpatched (universe and address 0), and an unpatched
	 *   row is never a conflict, because it occupies no channels;
	 * - a non-finite location component reads 0.
	 */
	MODULARSCENEBUILDER_API FStagePatchSheet Build(TArray<FStagePatchSheetRow> Rows, const FString& SceneName, const FDateTime& GeneratedAt);

	/** Has a universe and a start address within 1..512. */
	MODULARSCENEBUILDER_API bool IsPatched(const FStagePatchSheetRow& Row);

	/** Last channel used (Address + Footprint - 1); 0 when unpatched or the footprint is 0. May exceed 512 when a fixture overruns its universe. */
	MODULARSCENEBUILDER_API int32 GetEndAddress(const FStagePatchSheetRow& Row);

	/** "Universe.Address" with three address digits, e.g. "1.001"; "-" when unpatched (ASCII, so it prints in the PDF). */
	MODULARSCENEBUILDER_API FString FormatAddress(const FStagePatchSheetRow& Row);

	/** The end address in the same "1.016" form; "-" when there is none. */
	MODULARSCENEBUILDER_API FString FormatEndAddress(const FStagePatchSheetRow& Row);

	/**
	 * The sheet as CSV with the bill of materials' conventions (UTF-8 byte order mark, CRLF, "." decimals, every text field
	 * through StageBillOfMaterials::EscapeCsvField): a title block, a blank line, a header row and one row per fixture.
	 * Keeps full Unicode, unlike the PDF.
	 */
	MODULARSCENEBUILDER_API FString ToCsv(const FStagePatchSheet& Sheet);

	/**
	 * The sheet as a print-ready A4 landscape PDF in black and grey (it must photocopy): title block, summary, the fixture
	 * table with its header repeated on every page, and "Page n of m" footers. Characters outside Windows-1252 print as '?'
	 * (see FStagePdfDocument). An empty sheet is one page saying the stage has no lighting fixtures.
	 */
	MODULARSCENEBUILDER_API TArray<uint8> ToPdf(const FStagePatchSheet& Sheet);

	/** Fixture rows that fit on one PDF page. */
	MODULARSCENEBUILDER_API int32 GetRowsPerPage();

	/** "PatchSheet_<Scene>_<yyyyMMdd-HHmmss>" without extension, sanitized like StageBillOfMaterials::MakeFileName. */
	MODULARSCENEBUILDER_API FString MakeFileStem(const FString& SceneName, const FDateTime& GeneratedAt);
}
