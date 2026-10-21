// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/DateTime.h"
#include "Report/StagePdfWriter.h"

enum class EStageReportAlign : uint8
{
	Left,
	Right
};

/** How a cell's text is drawn: Normal ink, Muted grey (secondary information), or Strong (bold, for warnings and flags). */
enum class EStageReportTone : uint8
{
	Normal,
	Muted,
	Strong
};

/** Body rows are striped; Subtotal rows are bold under a thin rule; a Total row is bold on a grey band under a black rule. */
enum class EStageReportRowStyle : uint8
{
	Body,
	Subtotal,
	Total
};

struct FStageReportColumn
{
	FString Title;
	/** Relative width: the table's weights are scaled so the columns span the printable width exactly. */
	double Weight = 1.0;
	EStageReportAlign Align = EStageReportAlign::Left;
};

struct FStageReportCell
{
	FStageReportCell() = default;
	FStageReportCell(FString InText, EStageReportTone InTone = EStageReportTone::Normal) : Text(MoveTemp(InText)), Tone(InTone) {}
	FStageReportCell(const TCHAR* InText, EStageReportTone InTone = EStageReportTone::Normal) : Text(InText), Tone(InTone) {}

	FString Text;
	EStageReportTone Tone = EStageReportTone::Normal;
};

/**
 * Lays out a printable report (A4 landscape, black and greys so it photocopies) on an FStagePdfDocument, flowing content
 * down the page and breaking pages as needed (Docs/ADR/0012-production-reports-and-fixture-benchmark.md §2.1):
 *  - every page gets the title block (report title, scene, date, summary line) and a footer with "Page n of m", drawn by
 *    Finish once the page count is known;
 *  - tables repeat their header row after a page break; a heading always keeps its table header and first row with it.
 *
 * Plain value type with no UObjects: any thread, one report per instance. Text outside Windows-1252 prints as '?', as for
 * every FStagePdfDocument; reports pair the PDF with a CSV that keeps full Unicode.
 */
class FStageReportPdfComposer
{
public:
	/** ReportTitle is printed large on every page (e.g. "BILL OF MATERIALS"); FooterName follows the scene in the footer. */
	FStageReportPdfComposer(const FString& InReportTitle, const FString& InFooterName, const FString& InSceneName, const FDateTime& InGeneratedAt);

	/** One line under the scene name on every page (counts and totals). Truncated with "..." when too long. */
	void SetSummary(const FString& InSummary);

	/** A bold section heading. Ends any open table. */
	void AddHeading(const FString& Text);

	/** One line of text (truncated to the page width). Ends any open table. */
	void AddNote(const FString& Text, EStageReportTone Tone = EStageReportTone::Normal);

	/** Starts a table and draws its header. Columns with a non-positive weight count as weight 1. */
	void BeginTable(TArray<FStageReportColumn> InColumns);

	/** One row of the open table; missing cells are blank and extra cells are ignored. Does nothing without an open table. */
	void AddRow(TConstArrayView<FStageReportCell> Cells, EStageReportRowStyle Style = EStageReportRowStyle::Body);

	/** Closes the open table with a rule and leaves a gap below it. */
	void EndTable();

	/** Draws the title blocks and footers on every page and returns the PDF file. The composer is spent afterwards. */
	TArray<uint8> Finish();

	/** Pages so far (at least 1 after Finish). */
	int32 GetPageCount() const { return Doc.GetPageCount(); }

private:
	void StartPage();
	/** Starts a new page when Height does not fit above the footer; repeats the open table's header there. */
	void EnsureRoom(double Height);
	void DrawTableHeader();
	void DrawCell(int32 ColumnIndex, double Baseline, const FStageReportCell& Cell, EStagePdfFont Font);
	void DrawFrame(int32 PageIndex, int32 PageCount);
	FString GetDisplaySceneName() const;

	FStagePdfDocument Doc;
	FString ReportTitle;
	FString FooterName;
	FString SceneName;
	FDateTime GeneratedAt;
	FString Summary;

	/** Top of the next thing drawn on the current page; below the footer area means a new page is needed. */
	double CursorY = 0.0;

	TArray<FStageReportColumn> Columns;
	/** Points, parallel to Columns. */
	TArray<double> ColumnLefts;
	TArray<double> ColumnWidths;
	bool bTableOpen = false;
	int32 BodyRowsSinceHeader = 0;
};
