// Copyright Epic Games, Inc. All Rights Reserved.

#include "Report/StageReportPdfComposer.h"

namespace
{
	// Points, top-left origin (FStagePdfDocument). A4 landscape with 0.5 in margins, the patch sheet's geometry.
	constexpr double PageWidth = FStagePdfDocument::A4LandscapeWidth;
	constexpr double PageHeight = FStagePdfDocument::A4LandscapeHeight;
	constexpr double Margin = 36.0;
	constexpr double ContentLeft = Margin;
	constexpr double ContentRight = PageWidth - Margin;
	constexpr double ContentWidth = ContentRight - ContentLeft;
	constexpr double TitleBaseline = 50.0;
	constexpr double SceneBaseline = 66.0;
	constexpr double SummaryBaseline = 80.0;
	constexpr double TitleRuleY = 88.0;
	constexpr double BodyTop = 98.0;
	constexpr double FooterBaseline = PageHeight - Margin;
	constexpr double FooterRuleY = FooterBaseline - 11.0;
	constexpr double BodyBottom = FooterRuleY - 6.0;

	constexpr double HeadingHeight = 22.0;
	constexpr double NoteHeight = 13.0;
	constexpr double TableHeaderHeight = 16.0;
	constexpr double RowHeight = 14.0;
	constexpr double TableGap = 10.0;
	constexpr double CellPadding = 4.0;

	constexpr double TitleTextSize = 16.0;
	constexpr double HeadingTextSize = 11.0;
	constexpr double InfoTextSize = 9.0;
	constexpr double TableTextSize = 8.5;
	constexpr double FooterTextSize = 7.5;

	const FStagePdfColor ReportInk = FStagePdfColor::Grey(0.0);
	const FStagePdfColor ReportMutedInk = FStagePdfColor::Grey(0.35);
	const FStagePdfColor ReportHeaderFill = FStagePdfColor::Grey(0.86);
	const FStagePdfColor ReportStripeFill = FStagePdfColor::Grey(0.95);
	const FStagePdfColor ReportRuleInk = FStagePdfColor::Grey(0.55);

	const TCHAR* const Separator = TEXT(" · ");

	const FStagePdfColor& GetToneColor(EStageReportTone Tone)
	{
		return Tone == EStageReportTone::Muted ? ReportMutedInk : ReportInk;
	}
}

FStageReportPdfComposer::FStageReportPdfComposer(const FString& InReportTitle, const FString& InFooterName, const FString& InSceneName, const FDateTime& InGeneratedAt)
	: Doc(PageWidth, PageHeight)
	, ReportTitle(InReportTitle)
	, FooterName(InFooterName)
	, SceneName(InSceneName)
	, GeneratedAt(InGeneratedAt)
{
	Doc.SetInfo(InFooterName + TEXT(" - ") + GetDisplaySceneName(), TEXT("StageCraft 3D"), InGeneratedAt);
}

void FStageReportPdfComposer::SetSummary(const FString& InSummary)
{
	Summary = InSummary;
}

FString FStageReportPdfComposer::GetDisplaySceneName() const
{
	return SceneName.TrimStartAndEnd().IsEmpty() ? FString(TEXT("Untitled")) : SceneName;
}

void FStageReportPdfComposer::StartPage()
{
	Doc.AddPage();
	CursorY = BodyTop;
	BodyRowsSinceHeader = 0;
}

void FStageReportPdfComposer::EnsureRoom(double Height)
{
	if (Doc.GetPageCount() > 0 && CursorY + Height <= BodyBottom)
	{
		return;
	}

	StartPage();
	if (bTableOpen)
	{
		DrawTableHeader();
	}
}

void FStageReportPdfComposer::AddHeading(const FString& Text)
{
	EndTable();
	// Keep with next: a heading never ends a page without its table header and first row.
	EnsureRoom(HeadingHeight + TableHeaderHeight + RowHeight);
	Doc.Text(ContentLeft, CursorY + 15.0, FStagePdfDocument::FitText(Text, EStagePdfFont::Bold, HeadingTextSize, ContentWidth), EStagePdfFont::Bold, HeadingTextSize, ReportInk);
	CursorY += HeadingHeight;
}

void FStageReportPdfComposer::AddNote(const FString& Text, EStageReportTone Tone)
{
	EndTable();
	EnsureRoom(NoteHeight);
	const EStagePdfFont Font = Tone == EStageReportTone::Strong ? EStagePdfFont::Bold : EStagePdfFont::Regular;
	Doc.Text(ContentLeft, CursorY + 10.0, FStagePdfDocument::FitText(Text, Font, InfoTextSize, ContentWidth), Font, InfoTextSize, GetToneColor(Tone));
	CursorY += NoteHeight;
}

void FStageReportPdfComposer::BeginTable(TArray<FStageReportColumn> InColumns)
{
	EndTable();
	Columns = MoveTemp(InColumns);

	double TotalWeight = 0.0;
	for (FStageReportColumn& Column : Columns)
	{
		Column.Weight = FMath::IsFinite(Column.Weight) && Column.Weight > 0.0 ? Column.Weight : 1.0;
		TotalWeight += Column.Weight;
	}

	ColumnLefts.Reset(Columns.Num());
	ColumnWidths.Reset(Columns.Num());
	double Left = ContentLeft;
	for (const FStageReportColumn& Column : Columns)
	{
		const double Width = ContentWidth * Column.Weight / TotalWeight;
		ColumnLefts.Add(Left);
		ColumnWidths.Add(Width);
		Left += Width;
	}

	if (Columns.IsEmpty())
	{
		return;
	}
	// Room first, with the table still closed: a page break here must not draw the header a second time.
	EnsureRoom(TableHeaderHeight + RowHeight);
	bTableOpen = true;
	DrawTableHeader();
}

void FStageReportPdfComposer::DrawTableHeader()
{
	Doc.FillRect(ContentLeft, CursorY, ContentWidth, TableHeaderHeight, ReportHeaderFill);
	const double Baseline = CursorY + 11.0;
	for (int32 Index = 0; Index < Columns.Num(); ++Index)
	{
		DrawCell(Index, Baseline, FStageReportCell(Columns[Index].Title), EStagePdfFont::Bold);
	}
	CursorY += TableHeaderHeight;
	Doc.Line(ContentLeft, CursorY, ContentRight, CursorY, 0.5, ReportInk);
	BodyRowsSinceHeader = 0;
}

void FStageReportPdfComposer::DrawCell(int32 ColumnIndex, double Baseline, const FStageReportCell& Cell, EStagePdfFont Font)
{
	if (Cell.Text.IsEmpty())
	{
		return;
	}

	const double Left = ColumnLefts[ColumnIndex];
	const double Width = ColumnWidths[ColumnIndex];
	const FString Fitted = FStagePdfDocument::FitText(Cell.Text, Font, TableTextSize, Width - 2.0 * CellPadding);
	if (Columns[ColumnIndex].Align == EStageReportAlign::Right)
	{
		Doc.TextRight(Left + Width - CellPadding, Baseline, Fitted, Font, TableTextSize, GetToneColor(Cell.Tone));
	}
	else
	{
		Doc.Text(Left + CellPadding, Baseline, Fitted, Font, TableTextSize, GetToneColor(Cell.Tone));
	}
}

void FStageReportPdfComposer::AddRow(TConstArrayView<FStageReportCell> Cells, EStageReportRowStyle Style)
{
	if (!bTableOpen)
	{
		return;
	}

	EnsureRoom(RowHeight);
	switch (Style)
	{
	case EStageReportRowStyle::Body:
		if (BodyRowsSinceHeader % 2 == 1)
		{
			Doc.FillRect(ContentLeft, CursorY, ContentWidth, RowHeight, ReportStripeFill);
		}
		++BodyRowsSinceHeader;
		break;
	case EStageReportRowStyle::Subtotal:
		Doc.Line(ContentLeft, CursorY, ContentRight, CursorY, 0.5, ReportRuleInk);
		BodyRowsSinceHeader = 0;
		break;
	case EStageReportRowStyle::Total:
		Doc.FillRect(ContentLeft, CursorY, ContentWidth, RowHeight, ReportHeaderFill);
		Doc.Line(ContentLeft, CursorY, ContentRight, CursorY, 0.75, ReportInk);
		BodyRowsSinceHeader = 0;
		break;
	}

	const double Baseline = CursorY + 10.0;
	for (int32 Index = 0; Index < Columns.Num() && Index < Cells.Num(); ++Index)
	{
		const bool bBold = Style != EStageReportRowStyle::Body || Cells[Index].Tone == EStageReportTone::Strong;
		DrawCell(Index, Baseline, Cells[Index], bBold ? EStagePdfFont::Bold : EStagePdfFont::Regular);
	}
	CursorY += RowHeight;
}

void FStageReportPdfComposer::EndTable()
{
	if (!bTableOpen)
	{
		return;
	}
	bTableOpen = false;
	Doc.Line(ContentLeft, CursorY, ContentRight, CursorY, 0.5, ReportRuleInk);
	CursorY += TableGap;
}

void FStageReportPdfComposer::DrawFrame(int32 PageIndex, int32 PageCount)
{
	const FString DisplayScene = GetDisplaySceneName();

	Doc.Text(ContentLeft, TitleBaseline, ReportTitle, EStagePdfFont::Bold, TitleTextSize, ReportInk);
	Doc.TextRight(ContentRight, TitleBaseline, TEXT("StageCraft 3D"), EStagePdfFont::Bold, 10.0, ReportMutedInk);

	const FString Generated = TEXT("Generated: ") + GeneratedAt.ToString(TEXT("%Y-%m-%d %H:%M"));
	const double GeneratedWidth = FStagePdfDocument::MeasureText(Generated, EStagePdfFont::Regular, InfoTextSize);
	Doc.Text(ContentLeft, SceneBaseline, FStagePdfDocument::FitText(TEXT("Scene: ") + DisplayScene, EStagePdfFont::Regular, InfoTextSize, ContentWidth - GeneratedWidth - 24.0),
		EStagePdfFont::Regular, InfoTextSize, ReportInk);
	Doc.TextRight(ContentRight, SceneBaseline, Generated, EStagePdfFont::Regular, InfoTextSize, ReportInk);
	Doc.Text(ContentLeft, SummaryBaseline, FStagePdfDocument::FitText(Summary, EStagePdfFont::Regular, InfoTextSize, ContentWidth), EStagePdfFont::Regular, InfoTextSize, ReportInk);
	Doc.Line(ContentLeft, TitleRuleY, ContentRight, TitleRuleY, 0.75, ReportInk);

	const FString PageLabel = FString::Printf(TEXT("Page %d of %d"), PageIndex + 1, PageCount);
	const double PageLabelWidth = FStagePdfDocument::MeasureText(PageLabel, EStagePdfFont::Regular, FooterTextSize);
	Doc.Line(ContentLeft, FooterRuleY, ContentRight, FooterRuleY, 0.5, ReportRuleInk);
	Doc.Text(ContentLeft, FooterBaseline, FStagePdfDocument::FitText(DisplayScene + Separator + FooterName, EStagePdfFont::Regular, FooterTextSize, ContentWidth - PageLabelWidth - 24.0),
		EStagePdfFont::Regular, FooterTextSize, ReportMutedInk);
	Doc.TextRight(ContentRight, FooterBaseline, PageLabel, EStagePdfFont::Regular, FooterTextSize, ReportMutedInk);
}

TArray<uint8> FStageReportPdfComposer::Finish()
{
	EndTable();
	if (Doc.GetPageCount() == 0)
	{
		StartPage();
	}

	const int32 PageCount = Doc.GetPageCount();
	for (int32 PageIndex = 0; PageIndex < PageCount; ++PageIndex)
	{
		Doc.SetCurrentPage(PageIndex);
		DrawFrame(PageIndex, PageCount);
	}
	return Doc.Save();
}
