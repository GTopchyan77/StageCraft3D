// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/DateTime.h"

/** The two fonts reports use: PDF standard Type 1 fonts, never embedded (every PDF reader ships them), WinAnsi encoded. */
enum class EStagePdfFont : uint8
{
	/** Helvetica */
	Regular,
	/** Helvetica-Bold */
	Bold
};

/** A DeviceRGB colour, each channel 0..1 (clamped when written). Default is black. */
struct FStagePdfColor
{
	double R = 0.0;
	double G = 0.0;
	double B = 0.0;

	static FStagePdfColor Grey(double Level) { return { Level, Level, Level }; }
};

/**
 * A minimal PDF 1.4 writer for simple tabular reports (text, filled and stroked rectangles, lines), with no dependencies
 * beyond Core. Plain value type, no UObjects: any thread, one document per instance.
 *
 * Coordinates are in points (1/72 inch) with the origin at the TOP-LEFT of the page and Y growing downwards, like the UI;
 * the writer converts to PDF's bottom-left origin internally.
 *
 * Text is written in WinAnsiEncoding (Windows-1252), the encoding of the standard fonts: Latin-1 letters, the euro sign,
 * dashes, curly quotes, bullet and ellipsis print correctly; every other character (Cyrillic, Armenian, CJK, emoji, control
 * characters, tabs and line breaks) prints as '?'. Reports that must keep full Unicode should offer a CSV alongside the PDF.
 * The document info (title, producer) is written as UTF-16 and keeps every character.
 */
class MODULARSCENEBUILDER_API FStagePdfDocument
{
public:
	/** ISO A4 in landscape, in points. */
	static constexpr double A4LandscapeWidth = 842.0;
	static constexpr double A4LandscapeHeight = 595.0;

	/** Every page of the document has this size, in points. */
	FStagePdfDocument(double InPageWidth, double InPageHeight);

	double GetPageWidth() const { return PageWidth; }
	double GetPageHeight() const { return PageHeight; }
	int32 GetPageCount() const { return Pages.Num(); }

	/** The /Info dictionary: title and producer shown by readers, and the creation date (D:YYYYMMDDHHmmSS, no time zone). */
	void SetInfo(const FString& InTitle, const FString& InProducer, const FDateTime& InCreatedAt);

	/** Starts a new page and makes it the one drawn on. Returns its index. Drawing before the first AddPage starts page 0. */
	int32 AddPage();

	/**
	 * Makes an existing page the one drawn on, e.g. to draw "Page n of m" footers once the page count is known. Returns false
	 * and changes nothing when Index is not a page.
	 */
	bool SetCurrentPage(int32 Index);

	/** Text starting at X with its baseline at Y. Nothing is drawn for empty text or a size that is not positive. */
	void Text(double X, double Y, const FString& String, EStagePdfFont Font, double Size, const FStagePdfColor& Color = FStagePdfColor());

	/** Text ending at X (right-aligned), baseline at Y. */
	void TextRight(double X, double Y, const FString& String, EStagePdfFont Font, double Size, const FStagePdfColor& Color = FStagePdfColor());

	/** A filled rectangle whose top-left corner is (X, Y). Nothing is drawn for a non-positive width or height. */
	void FillRect(double X, double Y, double Width, double Height, const FStagePdfColor& Color);

	/** A rectangle outline whose top-left corner is (X, Y). */
	void StrokeRect(double X, double Y, double Width, double Height, double LineWidth, const FStagePdfColor& Color);

	/** A straight line with butt caps. */
	void Line(double X1, double Y1, double X2, double Y2, double LineWidth, const FStagePdfColor& Color);

	/** Advance width of String in points, from the real Helvetica / Helvetica-Bold metrics of the characters as encoded. */
	static double MeasureText(const FString& String, EStagePdfFont Font, double Size);

	/** String unchanged when it fits MaxWidth, otherwise its longest prefix that fits followed by "...". Empty when not even "..." fits. */
	static FString FitText(const FString& String, EStagePdfFont Font, double Size, double MaxWidth);

	/**
	 * The complete PDF file: header, objects, cross-reference table and trailer. Content streams are uncompressed and pure
	 * ASCII. A document without pages is saved with one empty page, because a PDF must contain at least one.
	 */
	TArray<uint8> Save() const;

private:
	FString& GetCurrentPage();
	double ToPdfY(double Y) const { return PageHeight - Y; }

	double PageWidth = A4LandscapeWidth;
	double PageHeight = A4LandscapeHeight;

	FString Title;
	FString Producer;
	FDateTime CreatedAt;
	bool bHasInfo = false;

	/** One content stream per page, ASCII only (text bytes >= 128 are written as octal escapes). */
	TArray<FString> Pages;

	/** The page drawn on; INDEX_NONE until the first page exists. */
	int32 CurrentPage = INDEX_NONE;
};
