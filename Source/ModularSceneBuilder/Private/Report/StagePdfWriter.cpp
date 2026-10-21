// Copyright Epic Games, Inc. All Rights Reserved.

#include "Report/StagePdfWriter.h"

#include "Misc/DateTime.h"

namespace StagePdfWriter
{
	namespace
	{
		// Advance widths in 1/1000 em from Adobe's Helvetica and Helvetica-Bold AFM files, for WinAnsi codes 32..126.
		// Under WinAnsiEncoding code 39 is quotesingle and 96 is grave (StandardEncoding would map them to the curly quotes).
		constexpr int16 HelveticaWidths[95] = {
			278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278,		// 32..47   space ! " # $ % & ' ( ) * + , - . /
			556, 556, 556, 556, 556, 556, 556, 556, 556, 556, 278, 278, 584, 584, 584, 556,		// 48..63   0..9 : ; < = > ?
			1015, 667, 667, 722, 722, 667, 611, 778, 722, 278, 500, 667, 556, 833, 722, 778,	// 64..79   @ A..O
			667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 278, 278, 278, 469, 556,		// 80..95   P..Z [ \ ] ^ _
			333, 556, 556, 500, 556, 556, 278, 556, 556, 222, 222, 500, 222, 833, 556, 556,		// 96..111  ` a..o
			556, 556, 333, 500, 278, 556, 500, 722, 500, 500, 500, 334, 260, 334, 584			// 112..126 p..z { | } ~
		};

		constexpr int16 HelveticaBoldWidths[95] = {
			278, 333, 474, 556, 556, 889, 722, 238, 333, 333, 389, 584, 278, 333, 278, 278,
			556, 556, 556, 556, 556, 556, 556, 556, 556, 556, 333, 333, 584, 584, 584, 611,
			975, 722, 722, 722, 722, 667, 611, 778, 722, 278, 556, 722, 611, 833, 722, 778,
			667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 333, 278, 333, 584, 556,
			333, 556, 611, 556, 611, 556, 333, 611, 611, 278, 278, 556, 278, 889, 611, 611,
			611, 611, 389, 556, 333, 611, 556, 778, 556, 556, 500, 389, 280, 389, 584
		};

		/**
		 * Widths of WinAnsi codes 128..255: exact for the punctuation reports use, and for accented letters the widest glyph of
		 * their family, so truncation errs towards fitting rather than overflowing a cell.
		 */
		int32 GetUpperCodeUnits(uint8 Code, bool bBold)
		{
			switch (Code)
			{
			case 0x85: return 1000;					// ellipsis
			case 0x91: case 0x92: return bBold ? 278 : 222;	// quoteleft, quoteright
			case 0x93: case 0x94: return bBold ? 500 : 333;	// quotedblleft, quotedblright
			case 0x95: return 350;					// bullet
			case 0x97: return 1000;					// emdash
			case 0xA0: return 278;					// no-break space
			case 0xB7: return 278;					// periodcentered
			case 0xC6: return 1000;					// AE
			case 0xD7: return 584;					// multiply
			case 0xDF: return 611;					// germandbls
			case 0xE6: return 889;					// ae
			case 0xF7: return 584;					// divide
			default: break;
			}

			if (Code >= 0xCC && Code <= 0xCF) { return 278; }	// accented I
			if (Code >= 0xEC && Code <= 0xEF) { return 278; }	// accented i
			if (Code >= 0xC0 && Code <= 0xDE) { return 778; }	// accented capitals: at most the width of O
			if (Code >= 0xE0) { return bBold ? 611 : 556; }		// accented lowercase
			return 556;											// euro, en dash and the rest
		}

		/** Marks the low half of a UTF-16 surrogate pair: no glyph, because its high half already became one '?'. */
		constexpr uint8 NoGlyph = 0;

		/** Windows-1252 byte for a character, '?' for anything the standard fonts cannot show. */
		uint8 ToWinAnsi(TCHAR Character)
		{
			const uint32 Code = static_cast<uint32>(Character);
			if ((Code >= 32 && Code <= 126) || (Code >= 0xA0 && Code <= 0xFF))
			{
				return static_cast<uint8>(Code);
			}
			switch (Code)
			{
			case 0x2013: return 0x96;	// en dash
			case 0x2014: return 0x97;	// em dash
			case 0x2018: return 0x91;
			case 0x2019: return 0x92;
			case 0x201C: return 0x93;
			case 0x201D: return 0x94;
			case 0x2022: return 0x95;	// bullet
			case 0x2026: return 0x85;	// ellipsis
			case 0x20AC: return 0x80;	// euro
			default: break;
			}
			return Code >= 0xDC00 && Code <= 0xDFFF ? NoGlyph : static_cast<uint8>('?');
		}

		int32 GetGlyphUnits(uint8 Code, EStagePdfFont Font)
		{
			const bool bBold = Font == EStagePdfFont::Bold;
			if (Code >= 32 && Code <= 126)
			{
				return bBold ? HelveticaBoldWidths[Code - 32] : HelveticaWidths[Code - 32];
			}
			return Code >= 128 ? GetUpperCodeUnits(Code, bBold) : 0;
		}

		int64 MeasureUnits(const FString& String, EStagePdfFont Font)
		{
			int64 Units = 0;
			for (const TCHAR Character : String)
			{
				Units += GetGlyphUnits(ToWinAnsi(Character), Font);
			}
			return Units;
		}

		/** Printf is locale independent ("." decimals); trailing zeros are dropped to keep streams short. */
		FString FormatNumber(double Value)
		{
			FString Out = FString::Printf(TEXT("%.2f"), FMath::IsFinite(Value) ? Value : 0.0);
			while (Out.EndsWith(TEXT("0")))
			{
				Out = Out.LeftChop(1);
			}
			if (Out.EndsWith(TEXT(".")))
			{
				Out = Out.LeftChop(1);
			}
			return Out == TEXT("-0") ? FString(TEXT("0")) : Out;
		}

		FString FormatColor(const FStagePdfColor& Color)
		{
			auto Channel = [](double Value) { return FormatNumber(FMath::IsFinite(Value) ? FMath::Clamp(Value, 0.0, 1.0) : 0.0); };
			return FString::Printf(TEXT("%s %s %s"), *Channel(Color.R), *Channel(Color.G), *Channel(Color.B));
		}

		/** A PDF literal string body: WinAnsi bytes, with \ ( ) escaped and bytes >= 128 as \ddd so the stream stays ASCII. */
		FString EscapeString(const FString& String)
		{
			FString Out;
			Out.Reserve(String.Len() + 8);
			for (const TCHAR Character : String)
			{
				const uint8 Code = ToWinAnsi(Character);
				if (Code == NoGlyph)
				{
					continue;
				}
				if (Code == '\\' || Code == '(' || Code == ')')
				{
					Out.AppendChar(TEXT('\\'));
					Out.AppendChar(static_cast<TCHAR>(Code));
				}
				else if (Code >= 128)
				{
					Out.AppendChar(TEXT('\\'));
					Out.AppendChar(static_cast<TCHAR>(TEXT('0') + ((Code >> 6) & 7)));
					Out.AppendChar(static_cast<TCHAR>(TEXT('0') + ((Code >> 3) & 7)));
					Out.AppendChar(static_cast<TCHAR>(TEXT('0') + (Code & 7)));
				}
				else
				{
					Out.AppendChar(static_cast<TCHAR>(Code));
				}
			}
			return Out;
		}

		/** A PDF text string as UTF-16BE hex with a byte order mark: the only encoding that keeps every character in /Info. */
		FString MakeUnicodeTextString(const FString& String)
		{
			FString Out = TEXT("<FEFF");
			for (const TCHAR Character : String)
			{
				uint32 Code = static_cast<uint32>(Character);
				if (Code > 0xFFFF)
				{
					// 32-bit TCHAR platforms: split into a surrogate pair.
					Code -= 0x10000;
					Out += FString::Printf(TEXT("%04X%04X"), 0xD800 + (Code >> 10), 0xDC00 + (Code & 0x3FF));
				}
				else
				{
					Out += FString::Printf(TEXT("%04X"), Code);
				}
			}
			Out += TEXT(">");
			return Out;
		}

		const TCHAR* GetFontResource(EStagePdfFont Font)
		{
			return Font == EStagePdfFont::Bold ? TEXT("F2") : TEXT("F1");
		}

		/** Every string reaching the file is ASCII by construction (escaped text, hex info), so a byte per character is exact. */
		void AppendAscii(TArray<uint8>& Out, const FString& Ascii)
		{
			Out.Reserve(Out.Num() + Ascii.Len());
			for (const TCHAR Character : Ascii)
			{
				Out.Add(static_cast<uint8>(Character));
			}
		}
	}
}

FStagePdfDocument::FStagePdfDocument(double InPageWidth, double InPageHeight)
	: PageWidth(FMath::IsFinite(InPageWidth) && InPageWidth > 0.0 ? InPageWidth : A4LandscapeWidth)
	, PageHeight(FMath::IsFinite(InPageHeight) && InPageHeight > 0.0 ? InPageHeight : A4LandscapeHeight)
{
}

void FStagePdfDocument::SetInfo(const FString& InTitle, const FString& InProducer, const FDateTime& InCreatedAt)
{
	Title = InTitle;
	Producer = InProducer;
	CreatedAt = InCreatedAt;
	bHasInfo = true;
}

int32 FStagePdfDocument::AddPage()
{
	CurrentPage = Pages.AddDefaulted();
	return CurrentPage;
}

bool FStagePdfDocument::SetCurrentPage(int32 Index)
{
	if (!Pages.IsValidIndex(Index))
	{
		return false;
	}
	CurrentPage = Index;
	return true;
}

FString& FStagePdfDocument::GetCurrentPage()
{
	if (!Pages.IsValidIndex(CurrentPage))
	{
		CurrentPage = Pages.AddDefaulted();
	}
	return Pages[CurrentPage];
}

void FStagePdfDocument::Text(double X, double Y, const FString& String, EStagePdfFont Font, double Size, const FStagePdfColor& Color)
{
	if (String.IsEmpty() || !(Size > 0.0))
	{
		return;
	}
	// Text is filled, so its colour is the non-stroking colour (rg); q/Q keeps every operation's state to itself.
	GetCurrentPage() += FString::Printf(TEXT("q %s rg BT /%s %s Tf %s %s Td (%s) Tj ET Q\n"),
		*StagePdfWriter::FormatColor(Color), StagePdfWriter::GetFontResource(Font), *StagePdfWriter::FormatNumber(Size), *StagePdfWriter::FormatNumber(X), *StagePdfWriter::FormatNumber(ToPdfY(Y)), *StagePdfWriter::EscapeString(String));
}

void FStagePdfDocument::TextRight(double X, double Y, const FString& String, EStagePdfFont Font, double Size, const FStagePdfColor& Color)
{
	Text(X - MeasureText(String, Font, Size), Y, String, Font, Size, Color);
}

void FStagePdfDocument::FillRect(double X, double Y, double Width, double Height, const FStagePdfColor& Color)
{
	if (!(Width > 0.0) || !(Height > 0.0))
	{
		return;
	}
	GetCurrentPage() += FString::Printf(TEXT("q %s rg %s %s %s %s re f Q\n"),
		*StagePdfWriter::FormatColor(Color), *StagePdfWriter::FormatNumber(X), *StagePdfWriter::FormatNumber(ToPdfY(Y + Height)), *StagePdfWriter::FormatNumber(Width), *StagePdfWriter::FormatNumber(Height));
}

void FStagePdfDocument::StrokeRect(double X, double Y, double Width, double Height, double LineWidth, const FStagePdfColor& Color)
{
	if (!(Width > 0.0) || !(Height > 0.0) || !(LineWidth > 0.0))
	{
		return;
	}
	GetCurrentPage() += FString::Printf(TEXT("q %s w %s RG %s %s %s %s re S Q\n"),
		*StagePdfWriter::FormatNumber(LineWidth), *StagePdfWriter::FormatColor(Color), *StagePdfWriter::FormatNumber(X), *StagePdfWriter::FormatNumber(ToPdfY(Y + Height)), *StagePdfWriter::FormatNumber(Width), *StagePdfWriter::FormatNumber(Height));
}

void FStagePdfDocument::Line(double X1, double Y1, double X2, double Y2, double LineWidth, const FStagePdfColor& Color)
{
	if (!(LineWidth > 0.0))
	{
		return;
	}
	GetCurrentPage() += FString::Printf(TEXT("q %s w %s RG %s %s m %s %s l S Q\n"),
		*StagePdfWriter::FormatNumber(LineWidth), *StagePdfWriter::FormatColor(Color), *StagePdfWriter::FormatNumber(X1), *StagePdfWriter::FormatNumber(ToPdfY(Y1)), *StagePdfWriter::FormatNumber(X2), *StagePdfWriter::FormatNumber(ToPdfY(Y2)));
}

double FStagePdfDocument::MeasureText(const FString& String, EStagePdfFont Font, double Size)
{
	if (!(Size > 0.0) || !FMath::IsFinite(Size))
	{
		return 0.0;
	}
	return static_cast<double>(StagePdfWriter::MeasureUnits(String, Font)) * Size / 1000.0;
}

FString FStagePdfDocument::FitText(const FString& String, EStagePdfFont Font, double Size, double MaxWidth)
{
	if (MeasureText(String, Font, Size) <= MaxWidth)
	{
		return String;
	}

	// Three periods rather than U+2026 so the marker reads the same in every encoding and font.
	const FString Ellipsis = TEXT("...");
	const double Budget = MaxWidth - MeasureText(Ellipsis, Font, Size);
	if (Budget < 0.0)
	{
		return FString();
	}

	// Summing whole font units (as MeasureText does) keeps the cut consistent with the measurement of the result.
	const double BudgetUnits = Budget * 1000.0 / Size;
	int64 UsedUnits = 0;
	int32 Keep = 0;
	for (; Keep < String.Len(); ++Keep)
	{
		const int32 Units = StagePdfWriter::GetGlyphUnits(StagePdfWriter::ToWinAnsi(String[Keep]), Font);
		if (static_cast<double>(UsedUnits + Units) > BudgetUnits)
		{
			break;
		}
		UsedUnits += Units;
	}
	return String.Left(Keep).TrimEnd() + Ellipsis;
}

TArray<uint8> FStagePdfDocument::Save() const
{

	TArray<FString> PageContents = Pages;
	if (PageContents.IsEmpty())
	{
		PageContents.AddDefaulted();
	}

	// Fixed objects first, then a (page, content stream) pair per page.
	constexpr int32 CatalogId = 1;
	constexpr int32 PagesId = 2;
	constexpr int32 FontRegularId = 3;
	constexpr int32 FontBoldId = 4;
	constexpr int32 InfoId = 5;
	constexpr int32 FirstPageId = 6;
	const int32 ObjectCount = FirstPageId + PageContents.Num() * 2;		// including the free object 0

	TArray<int32> Offsets;
	Offsets.SetNumZeroed(ObjectCount);

	int32 ContentBytes = 0;
	for (const FString& Content : PageContents)
	{
		ContentBytes += Content.Len();
	}

	TArray<uint8> Out;
	Out.Reserve(1024 + ContentBytes + ObjectCount * 128);

	StagePdfWriter::AppendAscii(Out, TEXT("%PDF-1.4\n"));
	// A comment of bytes >= 128 right after the header tells transfer tools the file is binary (PDF 1.4 §3.4.1).
	const uint8 BinaryComment[] = { '%', 0xE2, 0xE3, 0xCF, 0xD3, '\n' };
	Out.Append(BinaryComment, static_cast<int32>(UE_ARRAY_COUNT(BinaryComment)));

	auto WriteObject = [&Out, &Offsets](int32 Id, const FString& Body)
	{
		Offsets[Id] = Out.Num();
		StagePdfWriter::AppendAscii(Out, FString::Printf(TEXT("%d 0 obj\n"), Id));
		StagePdfWriter::AppendAscii(Out, Body);
		StagePdfWriter::AppendAscii(Out, TEXT("\nendobj\n"));
	};

	WriteObject(CatalogId, FString::Printf(TEXT("<< /Type /Catalog /Pages %d 0 R >>"), PagesId));

	FString Kids;
	for (int32 Index = 0; Index < PageContents.Num(); ++Index)
	{
		Kids += FString::Printf(TEXT("%s%d 0 R"), Index == 0 ? TEXT("") : TEXT(" "), FirstPageId + Index * 2);
	}
	WriteObject(PagesId, FString::Printf(TEXT("<< /Type /Pages /Kids [%s] /Count %d >>"), *Kids, PageContents.Num()));

	WriteObject(FontRegularId, TEXT("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>"));
	WriteObject(FontBoldId, TEXT("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding >>"));

	FString InfoBody = TEXT("<<");
	if (bHasInfo)
	{
		InfoBody += FString::Printf(TEXT(" /Title %s /Producer %s /CreationDate (D:%s)"),
			*StagePdfWriter::MakeUnicodeTextString(Title), *StagePdfWriter::MakeUnicodeTextString(Producer), *CreatedAt.ToString(TEXT("%Y%m%d%H%M%S")));
	}
	InfoBody += TEXT(" >>");
	WriteObject(InfoId, InfoBody);

	const FString MediaBox = FString::Printf(TEXT("[0 0 %s %s]"), *StagePdfWriter::FormatNumber(PageWidth), *StagePdfWriter::FormatNumber(PageHeight));
	for (int32 Index = 0; Index < PageContents.Num(); ++Index)
	{
		const int32 PageId = FirstPageId + Index * 2;
		const int32 ContentId = PageId + 1;
		WriteObject(PageId, FString::Printf(
			TEXT("<< /Type /Page /Parent %d 0 R /MediaBox %s /Resources << /Font << /F1 %d 0 R /F2 %d 0 R >> >> /Contents %d 0 R >>"),
			PagesId, *MediaBox, FontRegularId, FontBoldId, ContentId));

		// /Length counts the bytes between "stream\n" and the end-of-line before "endstream"; content is ASCII, so Len() is exact.
		const FString& Content = PageContents[Index];
		WriteObject(ContentId, FString::Printf(TEXT("<< /Length %d >>\nstream\n"), Content.Len()) + Content + TEXT("\nendstream"));
	}

	// Cross-reference table: every entry is exactly 20 bytes (10-digit offset, 5-digit generation, type, two-byte EOL).
	const int32 XrefOffset = Out.Num();
	StagePdfWriter::AppendAscii(Out, FString::Printf(TEXT("xref\n0 %d\n"), ObjectCount));
	StagePdfWriter::AppendAscii(Out, TEXT("0000000000 65535 f\r\n"));
	for (int32 Id = 1; Id < ObjectCount; ++Id)
	{
		StagePdfWriter::AppendAscii(Out, FString::Printf(TEXT("%010d 00000 n\r\n"), Offsets[Id]));
	}

	StagePdfWriter::AppendAscii(Out, FString::Printf(TEXT("trailer\n<< /Size %d /Root %d 0 R /Info %d 0 R >>\nstartxref\n%d\n"),
		ObjectCount, CatalogId, InfoId, XrefOffset));
	StagePdfWriter::AppendAscii(Out, TEXT("%%EOF\n"));		// literal, not a format string: the file needs both percent signs
	return Out;
}
