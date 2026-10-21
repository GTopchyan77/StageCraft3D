// Copyright Epic Games, Inc. All Rights Reserved.

#include "Report/StagePatchSheet.h"

#include "Misc/ComparisonUtility.h"
#include "Misc/DateTime.h"
#include "Report/StageBillOfMaterials.h"
#include "Report/StagePdfWriter.h"

namespace StagePatchSheet
{
	namespace
	{
		const TCHAR* const LineEnd = TEXT("\r\n");
		constexpr int32 DmxUniverseSize = 512;

		// PDF layout in points, top-left origin (FStagePdfDocument). A4 landscape with 0.5 in margins.
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
		constexpr double TableHeaderTop = 96.0;
		constexpr double TableHeaderHeight = 16.0;
		constexpr double BodyTop = TableHeaderTop + TableHeaderHeight;
		constexpr double RowHeight = 14.0;
		constexpr double FooterBaseline = PageHeight - Margin;
		constexpr double FooterRuleY = FooterBaseline - 11.0;
		constexpr double BodyBottom = FooterRuleY - 6.0;
		constexpr int32 RowsPerPage = static_cast<int32>((BodyBottom - BodyTop) / RowHeight);
		constexpr double CellPadding = 4.0;
		constexpr double TableTextSize = 8.5;
		constexpr double InfoTextSize = 9.0;
		constexpr double FooterTextSize = 7.5;

		// Black and greys only: the sheet is printed and photocopied at venues.
		const FStagePdfColor Ink = FStagePdfColor::Grey(0.0);
		const FStagePdfColor MutedInk = FStagePdfColor::Grey(0.35);
		const FStagePdfColor HeaderFill = FStagePdfColor::Grey(0.86);
		const FStagePdfColor StripeFill = FStagePdfColor::Grey(0.95);
		const FStagePdfColor RuleInk = FStagePdfColor::Grey(0.55);

		enum class EColumnAlign : uint8
		{
			Left,
			Right
		};

		struct FColumn
		{
			const TCHAR* Title;
			double Width;
			EColumnAlign Align;
		};

		enum EColumnIndex : int32
		{
			Col_Id, Col_Label, Col_Type, Col_Mode, Col_Universe, Col_Address, Col_End, Col_Channels, Col_X, Col_Y, Col_Z, Col_Notes, Col_Count
		};

		constexpr FColumn Columns[Col_Count] = {
			{ TEXT("ID"), 40.0, EColumnAlign::Right },
			{ TEXT("Label"), 150.0, EColumnAlign::Left },
			{ TEXT("Fixture Type"), 130.0, EColumnAlign::Left },
			{ TEXT("Mode"), 90.0, EColumnAlign::Left },
			{ TEXT("Univ."), 38.0, EColumnAlign::Right },
			{ TEXT("Addr."), 38.0, EColumnAlign::Right },
			{ TEXT("End"), 38.0, EColumnAlign::Right },
			{ TEXT("Ch."), 34.0, EColumnAlign::Right },
			{ TEXT("X (m)"), 50.0, EColumnAlign::Right },
			{ TEXT("Y (m)"), 50.0, EColumnAlign::Right },
			{ TEXT("Z (m)"), 50.0, EColumnAlign::Right },
			{ TEXT("Notes"), 62.0, EColumnAlign::Left },
		};

		constexpr double SumColumnWidths()
		{
			double Sum = 0.0;
			for (const FColumn& Column : Columns)
			{
				Sum += Column.Width;
			}
			return Sum;
		}
		static_assert(SumColumnWidths() == ContentWidth, "Patch sheet columns must span the printable width exactly");

		double SanitizeCoordinate(double Value)
		{
			return FMath::IsFinite(Value) ? Value : 0.0;
		}

		void SanitizeRow(FStagePatchSheetRow& Row)
		{
			Row.Footprint = FMath::Max(Row.Footprint, 0);
			if (Row.Universe < 1 || Row.Address < 1 || Row.Address > DmxUniverseSize)
			{
				Row.Universe = 0;
				Row.Address = 0;
				Row.bConflict = false;
			}
			Row.LocationMeters = FVector(SanitizeCoordinate(Row.LocationMeters.X), SanitizeCoordinate(Row.LocationMeters.Y), SanitizeCoordinate(Row.LocationMeters.Z));
		}

		FString FormatMeters(double Value)
		{
			// Printf is locale independent: "." decimals in both the CSV and the PDF.
			return FString::Printf(TEXT("%.2f"), Value);
		}

		FString GetDisplaySceneName(const FString& SceneName)
		{
			return SceneName.TrimStartAndEnd().IsEmpty() ? FString(TEXT("Untitled")) : SceneName;
		}

		FString JoinUniverses(const TArray<int32>& Universes)
		{
			FString Out;
			for (int32 Index = 0; Index < Universes.Num(); ++Index)
			{
				Out += (Index == 0 ? TEXT("") : TEXT(", ")) + FString::FromInt(Universes[Index]);
			}
			return Out;
		}

		FString CountNoun(int32 Count, const TCHAR* Singular, const TCHAR* Plural)
		{
			return FString::Printf(TEXT("%d %s"), Count, Count == 1 ? Singular : Plural);
		}

		void AppendRow(FString& Out, std::initializer_list<FString> Fields)
		{
			bool bFirst = true;
			for (const FString& Field : Fields)
			{
				if (!bFirst)
				{
					Out += TEXT(",");
				}
				Out += Field;
				bFirst = false;
			}
			Out += LineEnd;
		}

		int32 GetPageCount(int32 RowCount)
		{
			return FMath::Max(1, FMath::DivideAndRoundUp(RowCount, RowsPerPage));
		}

		double GetColumnLeft(int32 ColumnIndex)
		{
			double Left = ContentLeft;
			for (int32 Index = 0; Index < ColumnIndex; ++Index)
			{
				Left += Columns[Index].Width;
			}
			return Left;
		}

		/** One cell's text, truncated to the column and aligned inside its padding. */
		void DrawCell(FStagePdfDocument& Doc, int32 ColumnIndex, double Baseline, const FString& Value, EStagePdfFont Font, const FStagePdfColor& Color)
		{
			const FColumn& Column = Columns[ColumnIndex];
			const double Left = GetColumnLeft(ColumnIndex);
			const FString Fitted = FStagePdfDocument::FitText(Value, Font, TableTextSize, Column.Width - 2.0 * CellPadding);
			if (Column.Align == EColumnAlign::Right)
			{
				Doc.TextRight(Left + Column.Width - CellPadding, Baseline, Fitted, Font, TableTextSize, Color);
			}
			else
			{
				Doc.Text(Left + CellPadding, Baseline, Fitted, Font, TableTextSize, Color);
			}
		}

		void DrawPageFrame(FStagePdfDocument& Doc, const FStagePatchSheet& Sheet, int32 PageIndex, int32 PageCount)
		{
			const FString SceneName = GetDisplaySceneName(Sheet.SceneName);
			const FString Separator = TEXT(" · ");

			Doc.Text(ContentLeft, TitleBaseline, TEXT("DMX PATCH SHEET"), EStagePdfFont::Bold, 16.0, Ink);
			Doc.TextRight(ContentRight, TitleBaseline, TEXT("StageCraft 3D"), EStagePdfFont::Bold, 10.0, MutedInk);

			const FString Generated = TEXT("Generated: ") + Sheet.GeneratedAt.ToString(TEXT("%Y-%m-%d %H:%M"));
			const double GeneratedWidth = FStagePdfDocument::MeasureText(Generated, EStagePdfFont::Regular, InfoTextSize);
			Doc.Text(ContentLeft, SceneBaseline, FStagePdfDocument::FitText(TEXT("Scene: ") + SceneName, EStagePdfFont::Regular, InfoTextSize, ContentWidth - GeneratedWidth - 24.0),
				EStagePdfFont::Regular, InfoTextSize, Ink);
			Doc.TextRight(ContentRight, SceneBaseline, Generated, EStagePdfFont::Regular, InfoTextSize, Ink);

			const FString Summary = CountNoun(Sheet.Rows.Num(), TEXT("fixture"), TEXT("fixtures"))
				+ Separator + FString::Printf(TEXT("%d patched"), Sheet.PatchedCount)
				+ Separator + FString::Printf(TEXT("%d unpatched"), Sheet.UnpatchedCount)
				+ Separator + CountNoun(Sheet.ConflictCount, TEXT("conflict"), TEXT("conflicts"))
				+ Separator + TEXT("Universes: ") + (Sheet.Universes.IsEmpty() ? FString(TEXT("none")) : JoinUniverses(Sheet.Universes));
			Doc.Text(ContentLeft, SummaryBaseline, FStagePdfDocument::FitText(Summary, EStagePdfFont::Regular, InfoTextSize, ContentWidth), EStagePdfFont::Regular, InfoTextSize, Ink);
			Doc.Line(ContentLeft, TitleRuleY, ContentRight, TitleRuleY, 0.75, Ink);

			const FString PageLabel = FString::Printf(TEXT("Page %d of %d"), PageIndex + 1, PageCount);
			const double PageLabelWidth = FStagePdfDocument::MeasureText(PageLabel, EStagePdfFont::Regular, FooterTextSize);
			Doc.Line(ContentLeft, FooterRuleY, ContentRight, FooterRuleY, 0.5, RuleInk);
			Doc.Text(ContentLeft, FooterBaseline, FStagePdfDocument::FitText(SceneName + Separator + TEXT("DMX patch sheet"), EStagePdfFont::Regular, FooterTextSize, ContentWidth - PageLabelWidth - 24.0),
				EStagePdfFont::Regular, FooterTextSize, MutedInk);
			Doc.TextRight(ContentRight, FooterBaseline, PageLabel, EStagePdfFont::Regular, FooterTextSize, MutedInk);
		}

		void DrawTableHeader(FStagePdfDocument& Doc)
		{
			Doc.FillRect(ContentLeft, TableHeaderTop, ContentWidth, TableHeaderHeight, HeaderFill);
			const double Baseline = TableHeaderTop + 11.0;
			for (int32 Index = 0; Index < Col_Count; ++Index)
			{
				DrawCell(Doc, Index, Baseline, Columns[Index].Title, EStagePdfFont::Bold, Ink);
			}
			Doc.Line(ContentLeft, BodyTop, ContentRight, BodyTop, 0.5, Ink);
		}

		void DrawFixtureRow(FStagePdfDocument& Doc, const FStagePatchSheetRow& Row, double Top, bool bStriped)
		{
			if (bStriped)
			{
				Doc.FillRect(ContentLeft, Top, ContentWidth, RowHeight, StripeFill);
			}

			const double Baseline = Top + 10.0;
			const bool bPatched = IsPatched(Row);
			DrawCell(Doc, Col_Id, Baseline, FString::FromInt(Row.FixtureId), EStagePdfFont::Regular, Ink);
			DrawCell(Doc, Col_Label, Baseline, Row.Label, EStagePdfFont::Regular, Ink);
			DrawCell(Doc, Col_Type, Baseline, Row.FixtureType, EStagePdfFont::Regular, Ink);
			DrawCell(Doc, Col_Mode, Baseline, Row.Mode, EStagePdfFont::Regular, Ink);
			DrawCell(Doc, Col_Universe, Baseline, bPatched ? FString::FromInt(Row.Universe) : FString(TEXT("-")), EStagePdfFont::Regular, Ink);
			DrawCell(Doc, Col_Address, Baseline, FormatAddress(Row), EStagePdfFont::Regular, Ink);
			DrawCell(Doc, Col_End, Baseline, FormatEndAddress(Row), EStagePdfFont::Regular, Ink);
			DrawCell(Doc, Col_Channels, Baseline, FString::FromInt(Row.Footprint), EStagePdfFont::Regular, Ink);
			DrawCell(Doc, Col_X, Baseline, FormatMeters(Row.LocationMeters.X), EStagePdfFont::Regular, Ink);
			DrawCell(Doc, Col_Y, Baseline, FormatMeters(Row.LocationMeters.Y), EStagePdfFont::Regular, Ink);
			DrawCell(Doc, Col_Z, Baseline, FormatMeters(Row.LocationMeters.Z), EStagePdfFont::Regular, Ink);

			if (Row.bConflict)
			{
				DrawCell(Doc, Col_Notes, Baseline, TEXT("CONFLICT"), EStagePdfFont::Bold, Ink);
			}
			else if (!bPatched)
			{
				DrawCell(Doc, Col_Notes, Baseline, TEXT("Unpatched"), EStagePdfFont::Regular, MutedInk);
			}
		}
	}

	FStagePatchSheet Build(TArray<FStagePatchSheetRow> Rows, const FString& SceneName, const FDateTime& GeneratedAt)
	{
		FStagePatchSheet Sheet;
		Sheet.SceneName = SceneName;
		Sheet.GeneratedAt = GeneratedAt;

		for (FStagePatchSheetRow& Row : Rows)
		{
			SanitizeRow(Row);
		}

		Rows.StableSort([](const FStagePatchSheetRow& A, const FStagePatchSheetRow& B)
		{
			if (A.FixtureId != B.FixtureId)
			{
				return A.FixtureId < B.FixtureId;
			}
			const int32 ByLabel = UE::ComparisonUtility::CompareNaturalOrder(A.Label, B.Label);
			if (ByLabel != 0)
			{
				return ByLabel < 0;
			}
			return A.Universe != B.Universe ? A.Universe < B.Universe : A.Address < B.Address;
		});

		for (const FStagePatchSheetRow& Row : Rows)
		{
			if (IsPatched(Row))
			{
				++Sheet.PatchedCount;
				Sheet.Universes.AddUnique(Row.Universe);
			}
			else
			{
				++Sheet.UnpatchedCount;
			}
			Sheet.ConflictCount += Row.bConflict ? 1 : 0;
		}
		Sheet.Universes.Sort();
		Sheet.Rows = MoveTemp(Rows);
		return Sheet;
	}

	bool IsPatched(const FStagePatchSheetRow& Row)
	{
		return Row.Universe >= 1 && Row.Address >= 1 && Row.Address <= DmxUniverseSize;
	}

	int32 GetEndAddress(const FStagePatchSheetRow& Row)
	{
		return IsPatched(Row) && Row.Footprint > 0 ? Row.Address + Row.Footprint - 1 : 0;
	}

	FString FormatAddress(const FStagePatchSheetRow& Row)
	{
		return IsPatched(Row) ? FString::Printf(TEXT("%d.%03d"), Row.Universe, Row.Address) : FString(TEXT("-"));
	}

	FString FormatEndAddress(const FStagePatchSheetRow& Row)
	{
		const int32 EndAddress = GetEndAddress(Row);
		return EndAddress > 0 ? FString::Printf(TEXT("%d.%03d"), Row.Universe, EndAddress) : FString(TEXT("-"));
	}

	FString ToCsv(const FStagePatchSheet& Sheet)
	{
		using StageBillOfMaterials::EscapeCsvField;

		FString Out;
		Out.Reserve(256 + Sheet.Rows.Num() * 96);

		// U+FEFF: written as the UTF-8 byte order mark, which spreadsheet programs need to read non-ASCII names correctly.
		Out.AppendChar(TCHAR(0xFEFF));
		AppendRow(Out, { EscapeCsvField(TEXT("StageCraft 3D DMX Patch Sheet")) });
		AppendRow(Out, { TEXT("Scene"), EscapeCsvField(GetDisplaySceneName(Sheet.SceneName)) });
		AppendRow(Out, { TEXT("Generated"), Sheet.GeneratedAt.ToString(TEXT("%Y-%m-%d %H:%M")) });
		AppendRow(Out, { TEXT("Fixtures"), FString::FromInt(Sheet.Rows.Num()) });
		AppendRow(Out, { TEXT("Universes"), EscapeCsvField(Sheet.Universes.IsEmpty() ? FString(TEXT("None")) : JoinUniverses(Sheet.Universes)) });
		AppendRow(Out, { TEXT("Conflicts"), FString::FromInt(Sheet.ConflictCount) });
		Out += LineEnd;

		AppendRow(Out, { TEXT("Fixture ID"), TEXT("Label"), TEXT("Fixture Type"), TEXT("Mode"), TEXT("Universe"), TEXT("Address"),
			TEXT("End Address"), TEXT("Channels"), TEXT("X (m)"), TEXT("Y (m)"), TEXT("Z (m)"), TEXT("Notes") });

		for (const FStagePatchSheetRow& Row : Sheet.Rows)
		{
			const bool bPatched = IsPatched(Row);
			const int32 EndAddress = GetEndAddress(Row);
			const FString Notes = Row.bConflict ? TEXT("Patch conflict") : (bPatched ? TEXT("") : TEXT("Unpatched"));

			// Address columns stay plain numbers (empty when unpatched) so spreadsheets can sort and filter them.
			AppendRow(Out, { FString::FromInt(Row.FixtureId), EscapeCsvField(Row.Label), EscapeCsvField(Row.FixtureType), EscapeCsvField(Row.Mode),
				bPatched ? FString::FromInt(Row.Universe) : FString(), bPatched ? FString::FromInt(Row.Address) : FString(),
				EndAddress > 0 ? FString::FromInt(EndAddress) : FString(), FString::FromInt(Row.Footprint),
				FormatMeters(Row.LocationMeters.X), FormatMeters(Row.LocationMeters.Y), FormatMeters(Row.LocationMeters.Z), EscapeCsvField(Notes) });
		}
		return Out;
	}

	TArray<uint8> ToPdf(const FStagePatchSheet& Sheet)
	{
		FStagePdfDocument Doc(PageWidth, PageHeight);
		Doc.SetInfo(TEXT("DMX Patch Sheet - ") + GetDisplaySceneName(Sheet.SceneName), TEXT("StageCraft 3D"), Sheet.GeneratedAt);

		const int32 PageCount = GetPageCount(Sheet.Rows.Num());
		for (int32 PageIndex = 0; PageIndex < PageCount; ++PageIndex)
		{
			Doc.AddPage();
			DrawPageFrame(Doc, Sheet, PageIndex, PageCount);

			if (Sheet.Rows.IsEmpty())
			{
				Doc.Text(ContentLeft, TableHeaderTop + 14.0, TEXT("No lighting fixtures on this stage."), EStagePdfFont::Regular, 10.0, Ink);
				continue;
			}

			DrawTableHeader(Doc);
			const int32 First = PageIndex * RowsPerPage;
			const int32 Last = FMath::Min(First + RowsPerPage, Sheet.Rows.Num());
			for (int32 Index = First; Index < Last; ++Index)
			{
				const int32 RowOnPage = Index - First;
				DrawFixtureRow(Doc, Sheet.Rows[Index], BodyTop + RowOnPage * RowHeight, RowOnPage % 2 == 1);
			}
			const double TableBottom = BodyTop + (Last - First) * RowHeight;
			Doc.Line(ContentLeft, TableBottom, ContentRight, TableBottom, 0.5, RuleInk);
		}
		return Doc.Save();
	}

	int32 GetRowsPerPage()
	{
		return RowsPerPage;
	}

	FString MakeFileStem(const FString& SceneName, const FDateTime& GeneratedAt)
	{
		// Same sanitizing as StageBillOfMaterials::MakeFileName: safe on every file system, recognisable to the user.
		FString Safe;
		Safe.Reserve(SceneName.Len());
		for (const TCHAR Character : SceneName.TrimStartAndEnd())
		{
			Safe.AppendChar(FChar::IsAlnum(Character) || Character == TEXT('-') || Character == TEXT('_') ? Character : TEXT('_'));
		}
		if (Safe.IsEmpty())
		{
			Safe = TEXT("Untitled");
		}
		return FString::Printf(TEXT("PatchSheet_%s_%s"), *Safe, *GeneratedAt.ToString(TEXT("%Y%m%d-%H%M%S")));
	}
}
