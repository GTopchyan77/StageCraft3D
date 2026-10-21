// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

// Shared checks for the report tests (patch sheet, bill of materials, cable schedule): the PDF file structure and text helpers.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

namespace StageReportTestUtils
{
	/** One TCHAR per byte, so string offsets equal file offsets. */
	inline FString BytesToLatin1(const TArray<uint8>& Bytes)
	{
		FString Out;
		Out.Reserve(Bytes.Num());
		for (const uint8 Byte : Bytes)
		{
			Out.AppendChar(static_cast<TCHAR>(Byte));
		}
		return Out;
	}

	inline int32 CountOccurrences(const FString& Haystack, const TCHAR* Needle)
	{
		const int32 NeedleLen = FCString::Strlen(Needle);
		int32 Count = 0;
		int32 From = Haystack.Find(Needle, ESearchCase::CaseSensitive, ESearchDir::FromStart, 0);
		while (From != INDEX_NONE)
		{
			++Count;
			From = Haystack.Find(Needle, ESearchCase::CaseSensitive, ESearchDir::FromStart, From + NeedleLen);
		}
		return Count;
	}

	/** Header, trailer, every cross-reference offset and every stream /Length of a PDF; true when all hold. */
	inline bool VerifyPdfStructure(FAutomationTestBase& Test, const TArray<uint8>& Pdf, const FString& What)
	{
		auto Label = [&What](const TCHAR* Check) { return What + Check; };
		const FString Text = BytesToLatin1(Pdf);
		bool bOk = Test.TestTrue(*Label(TEXT(": starts with %PDF-1.4")), Text.StartsWith(TEXT("%PDF-1.4\n"), ESearchCase::CaseSensitive));
		bOk &= Test.TestTrue(*Label(TEXT(": ends with %%EOF")), Text.TrimEnd().EndsWith(TEXT("%%EOF"), ESearchCase::CaseSensitive));

		// Everything after the binary comment line is ASCII: text bytes >= 128 must be octal escapes.
		const int32 SecondLineEnd = Text.Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Text.Find(TEXT("\n")) + 1);
		bool bAscii = SecondLineEnd != INDEX_NONE;
		for (int32 Index = SecondLineEnd + 1; bAscii && Index < Pdf.Num(); ++Index)
		{
			bAscii = Pdf[Index] < 128;
		}
		bOk &= Test.TestTrue(*Label(TEXT(": body is pure ASCII")), bAscii);

		const int32 StartXrefPos = Text.Find(TEXT("startxref"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		if (!Test.TestTrue(*Label(TEXT(": has startxref")), StartXrefPos != INDEX_NONE))
		{
			return false;
		}
		const int32 XrefOffset = FCString::Atoi(*Text.Mid(StartXrefPos + 9, 16).TrimStart());
		if (!Test.TestEqual(*Label(TEXT(": startxref points at xref")), Text.Mid(XrefOffset, 5), FString(TEXT("xref\n"))))
		{
			return false;
		}

		const int32 SubsectionStart = XrefOffset + 5;
		const int32 SubsectionEnd = Text.Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, SubsectionStart);
		FString FirstId;
		FString CountText;
		Text.Mid(SubsectionStart, SubsectionEnd - SubsectionStart).Split(TEXT(" "), &FirstId, &CountText);
		const int32 ObjectCount = FCString::Atoi(*CountText);
		bOk &= Test.TestEqual(*Label(TEXT(": xref starts at object 0")), FirstId, FString(TEXT("0")));
		bOk &= Test.TestTrue(*Label(TEXT(": trailer /Size matches")), Text.Contains(FString::Printf(TEXT("/Size %d "), ObjectCount)));

		const int32 EntriesStart = SubsectionEnd + 1;
		for (int32 Id = 0; Id < ObjectCount; ++Id)
		{
			const FString Entry = Text.Mid(EntriesStart + Id * 20, 20);
			if (Id == 0)
			{
				bOk &= Test.TestEqual(*Label(TEXT(": free entry 0")), Entry, FString(TEXT("0000000000 65535 f\r\n")));
				continue;
			}
			bOk &= Test.TestTrue(*Label(*FString::Printf(TEXT(": entry %d is 20 bytes, in use"), Id)), Entry.Len() == 20 && Entry[17] == TEXT('n') && Entry.EndsWith(TEXT("\r\n")));
			const int32 Offset = FCString::Atoi(*Entry.Left(10));
			bOk &= Test.TestTrue(*Label(*FString::Printf(TEXT(": object %d at its offset"), Id)),
				Text.Mid(Offset, 16).StartsWith(FString::Printf(TEXT("%d 0 obj"), Id), ESearchCase::CaseSensitive));
		}

		// Every stream holds exactly /Length bytes before its end-of-line and "endstream".
		int32 LengthPos = Text.Find(TEXT("<< /Length "), ESearchCase::CaseSensitive);
		while (LengthPos != INDEX_NONE)
		{
			const int32 Length = FCString::Atoi(*Text.Mid(LengthPos + 11, 12));
			const int32 DataStart = Text.Find(TEXT("stream\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, LengthPos) + 7;
			bOk &= Test.TestEqual(*Label(TEXT(": stream /Length")), Text.Mid(DataStart + Length, 10), FString(TEXT("\nendstream")));
			LengthPos = Text.Find(TEXT("<< /Length "), ESearchCase::CaseSensitive, ESearchDir::FromStart, DataStart);
		}
		return bOk;
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
