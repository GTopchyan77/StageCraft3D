// Copyright Epic Games, Inc. All Rights Reserved.

#include "Show/StagePatchSchedule.h"

#include "Show/StageDMXPatching.h"

#define LOCTEXT_NAMESPACE "StagePatchSchedule"

namespace
{
	constexpr int32 MaxFixtureId = 99999;

	/** Splits Line into fields at Delimiter, outside double quotes ("" inside quotes is one quote). False for an unclosed quote. */
	bool SplitFields(FStringView Line, TCHAR Delimiter, TArray<FString>& OutFields)
	{
		OutFields.Reset();
		FString Field;
		bool bQuoted = false;
		for (int32 Index = 0; Index < Line.Len(); ++Index)
		{
			const TCHAR Char = Line[Index];
			if (bQuoted)
			{
				if (Char == TEXT('"'))
				{
					if (Index + 1 < Line.Len() && Line[Index + 1] == TEXT('"'))
					{
						Field.AppendChar(TEXT('"'));
						++Index;
					}
					else
					{
						bQuoted = false;
					}
				}
				else
				{
					Field.AppendChar(Char);
				}
			}
			else if (Char == TEXT('"'))
			{
				bQuoted = true;
			}
			else if (Char == Delimiter)
			{
				OutFields.Add(MoveTemp(Field));
				Field.Reset();
			}
			else
			{
				Field.AppendChar(Char);
			}
		}
		OutFields.Add(MoveTemp(Field));
		return !bQuoted;
	}

	/** Trimmed, without the patch sheet's leading apostrophe (its guard against spreadsheet formulas). */
	FString CleanField(const FString& Field)
	{
		FString Clean = Field.TrimStartAndEnd();
		if (Clean.StartsWith(TEXT("'")))
		{
			Clean.RightChopInline(1);
			Clean.TrimStartInline();
		}
		return Clean;
	}

	/** Digits only (an optional leading +), at most 6 of them, within Min..Max. */
	bool ParseNumber(const FString& Field, int32 Min, int32 Max, int32& OutValue)
	{
		FStringView Digits(Field);
		if (Digits.StartsWith(TEXT('+')))
		{
			Digits.RightChopInline(1);
		}
		if (Digits.IsEmpty() || Digits.Len() > 6)
		{
			return false;
		}
		int32 Value = 0;
		for (const TCHAR Char : Digits)
		{
			if (Char < TEXT('0') || Char > TEXT('9'))
			{
				return false;
			}
			Value = Value * 10 + (Char - TEXT('0'));
		}
		if (Value < Min || Value > Max)
		{
			return false;
		}
		OutValue = Value;
		return true;
	}

	bool ParseFlag(const FString& Field, bool& OutValue)
	{
		if (Field.IsEmpty() || Field.Equals(TEXT("No"), ESearchCase::IgnoreCase) || Field.Equals(TEXT("False"), ESearchCase::IgnoreCase)
			|| Field == TEXT("0") || Field.Equals(TEXT("Off"), ESearchCase::IgnoreCase))
		{
			OutValue = false;
			return true;
		}
		if (Field.Equals(TEXT("Yes"), ESearchCase::IgnoreCase) || Field.Equals(TEXT("True"), ESearchCase::IgnoreCase)
			|| Field == TEXT("1") || Field.Equals(TEXT("On"), ESearchCase::IgnoreCase))
		{
			OutValue = true;
			return true;
		}
		return false;
	}

	/** Where each known column is in the header row; INDEX_NONE when the file does not have it. */
	struct FColumns
	{
		int32 FixtureId = INDEX_NONE;
		int32 Universe = INDEX_NONE;
		int32 Address = INDEX_NONE;
		int32 InvertPan = INDEX_NONE;
		int32 InvertTilt = INDEX_NONE;
		int32 SwapPanTilt = INDEX_NONE;
	};

	FColumns FindColumns(const TArray<FString>& Header)
	{
		FColumns Columns;
		for (int32 Index = 0; Index < Header.Num(); ++Index)
		{
			const FString Name = CleanField(Header[Index]);
			int32* Slot = Name.Equals(TEXT("Fixture ID"), ESearchCase::IgnoreCase) ? &Columns.FixtureId
				: Name.Equals(TEXT("Universe"), ESearchCase::IgnoreCase) ? &Columns.Universe
				: Name.Equals(TEXT("Address"), ESearchCase::IgnoreCase) ? &Columns.Address
				: Name.Equals(TEXT("Invert Pan"), ESearchCase::IgnoreCase) ? &Columns.InvertPan
				: Name.Equals(TEXT("Invert Tilt"), ESearchCase::IgnoreCase) ? &Columns.InvertTilt
				: Name.Equals(TEXT("Swap Pan/Tilt"), ESearchCase::IgnoreCase) ? &Columns.SwapPanTilt
				: nullptr;
			if (Slot && *Slot == INDEX_NONE)
			{
				*Slot = Index;
			}
		}
		return Columns;
	}

	FString FieldAt(const TArray<FString>& Fields, int32 Column)
	{
		return Fields.IsValidIndex(Column) ? CleanField(Fields[Column]) : FString();
	}

	FText FormatPatch(const FStageDMXPatch& Patch)
	{
		return FText::FromString(FString::Printf(TEXT("%d.%03d"), Patch.Universe, Patch.Address));
	}
}

FStagePatchScheduleParse StagePatchSchedule::Parse(FStringView Text)
{
	FStagePatchScheduleParse Result;
	if (Text.Len() > MaxFileCharacters)
	{
		Result.Errors.Add({ 0, FText::Format(LOCTEXT("TooLarge", "The file is larger than {0} characters; a fixture schedule is far smaller."), MaxFileCharacters) });
		return Result;
	}
	if (Text.StartsWith(TCHAR(0xFEFF)))
	{
		Text.RightChopInline(1);
	}

	TCHAR Delimiter = TEXT(',');
	FColumns Columns;
	TArray<FString> Fields;
	int32 Line = 0;
	int32 Start = 0;
	while (Start <= Text.Len())
	{
		int32 End = Start;
		while (End < Text.Len() && Text[End] != TEXT('\n') && Text[End] != TEXT('\r'))
		{
			++End;
		}
		const FStringView Current = Text.Mid(Start, End - Start);
		// CRLF counts as one line break.
		Start = End + (End + 1 < Text.Len() && Text[End] == TEXT('\r') && Text[End + 1] == TEXT('\n') ? 2 : 1);
		++Line;

		FString Trimmed(Current);
		Trimmed.TrimStartAndEndInline();
		if (Trimmed.IsEmpty())
		{
			continue;
		}
		bool bHasNul = false;
		for (const TCHAR Char : Current)
		{
			bHasNul |= Char == TCHAR(0);
		}
		if (bHasNul)
		{
			if (Result.bHeaderFound)
			{
				Result.Errors.Add({ Line, LOCTEXT("BadCharacters", "The row contains characters that are not text.") });
			}
			continue;
		}

		if (!Result.bHeaderFound)
		{
			// The header row decides the separator: spreadsheets in many locales write semicolons.
			int32 Commas = 0;
			int32 Semicolons = 0;
			for (const TCHAR Char : Current)
			{
				Commas += Char == TEXT(',');
				Semicolons += Char == TEXT(';');
			}
			const TCHAR Candidate = Semicolons > Commas ? TEXT(';') : TEXT(',');
			if (!SplitFields(Current, Candidate, Fields))
			{
				continue;
			}
			Columns = FindColumns(Fields);
			if (Columns.FixtureId == INDEX_NONE)
			{
				continue; // The sheet's title and summary lines.
			}
			Delimiter = Candidate;
			if (Columns.Universe == INDEX_NONE || Columns.Address == INDEX_NONE)
			{
				Result.Errors.Add({ Line, LOCTEXT("MissingColumns", "The header row has no Universe or no Address column.") });
				return Result;
			}
			Result.bHeaderFound = true;
			continue;
		}

		if (Result.Rows.Num() + Result.Errors.Num() >= MaxRows)
		{
			Result.Errors.Add({ Line, FText::Format(LOCTEXT("TooManyRows", "More than {0} rows; the rest of the file was not read."), MaxRows) });
			break;
		}
		if (!SplitFields(Current, Delimiter, Fields))
		{
			Result.Errors.Add({ Line, LOCTEXT("OpenQuote", "A quoted field is not closed.") });
			continue;
		}
		if (Fields.ContainsByPredicate([](const FString& Field) { return Field.Len() > MaxFieldCharacters; }))
		{
			Result.Errors.Add({ Line, FText::Format(LOCTEXT("LongField", "A field is longer than {0} characters."), MaxFieldCharacters) });
			continue;
		}

		FStagePatchScheduleRow Row;
		Row.Line = Line;
		if (!ParseNumber(FieldAt(Fields, Columns.FixtureId), 1, MaxFixtureId, Row.FixtureId))
		{
			Result.Errors.Add({ Line, FText::Format(LOCTEXT("BadFixtureId", "\"{0}\" is not a fixture ID (1-{1})."),
				FText::FromString(FieldAt(Fields, Columns.FixtureId).Left(32)), MaxFixtureId) });
			continue;
		}
		const FString Universe = FieldAt(Fields, Columns.Universe);
		const FString Address = FieldAt(Fields, Columns.Address);
		if (Universe.IsEmpty() != Address.IsEmpty())
		{
			Result.Errors.Add({ Line, FText::Format(LOCTEXT("HalfPatch", "Fixture {0} has a universe or an address but not both."), Row.FixtureId) });
			continue;
		}
		if (!Universe.IsEmpty()
			&& (!ParseNumber(Universe, 1, StageDMXPatching::MaxUniverse, Row.Patch.Universe) || !ParseNumber(Address, 1, StageDMXPatching::UniverseSize, Row.Patch.Address)))
		{
			Result.Errors.Add({ Line, FText::Format(LOCTEXT("BadPatch", "Fixture {0}: \"{1}.{2}\" is not a universe (1-{3}) and an address (1-512)."), Row.FixtureId,
				FText::FromString(Universe.Left(16)), FText::FromString(Address.Left(16)), StageDMXPatching::MaxUniverse) });
			continue;
		}

		bool bBadFlag = false;
		const auto ReadFlag = [&Fields, &bBadFlag](int32 Column, TOptional<bool>& OutFlag)
		{
			bool bValue = false;
			if (Column == INDEX_NONE)
			{
				return;
			}
			if (ParseFlag(FieldAt(Fields, Column), bValue))
			{
				OutFlag = bValue;
			}
			else
			{
				bBadFlag = true;
			}
		};
		ReadFlag(Columns.InvertPan, Row.bInvertPan);
		ReadFlag(Columns.InvertTilt, Row.bInvertTilt);
		ReadFlag(Columns.SwapPanTilt, Row.bSwapPanTilt);
		if (bBadFlag)
		{
			Result.Errors.Add({ Line, FText::Format(LOCTEXT("BadFlag", "Fixture {0}: an invert or swap column is not Yes or No."), Row.FixtureId) });
			continue;
		}
		Result.Rows.Add(MoveTemp(Row));
	}

	if (!Result.bHeaderFound && Result.Errors.IsEmpty())
	{
		Result.Errors.Add({ 0, LOCTEXT("NoHeader", "No header row with a \"Fixture ID\" column was found. Export the patch sheet to see the layout.") });
	}
	return Result;
}

FStagePatchSchedulePlan StagePatchSchedule::Plan(TConstArrayView<FStagePatchScheduleRow> Rows, TConstArrayView<FStagePatchScheduleFixture> Fixtures)
{
	FStagePatchSchedulePlan Result;
	TMap<int32, int32> FixtureIndex;
	FixtureIndex.Reserve(Fixtures.Num());
	for (int32 Index = 0; Index < Fixtures.Num(); ++Index)
	{
		FixtureIndex.Add(Fixtures[Index].FixtureId, Index);
	}
	TMap<int32, int32> RowsPerFixture;
	for (const FStagePatchScheduleRow& Row : Rows)
	{
		++RowsPerFixture.FindOrAdd(Row.FixtureId);
	}

	TMap<int32, FStageDMXPatch> NewPatches;
	for (const FStagePatchScheduleRow& Row : Rows)
	{
		if (RowsPerFixture[Row.FixtureId] > 1)
		{
			Result.Rejected.Add({ Row.Line, FText::Format(LOCTEXT("Duplicate", "Fixture {0} is on more than one row."), Row.FixtureId) });
			continue;
		}
		const int32* Index = FixtureIndex.Find(Row.FixtureId);
		if (!Index)
		{
			Result.Rejected.Add({ Row.Line, FText::Format(LOCTEXT("Unknown", "There is no fixture {0} on the stage."), Row.FixtureId) });
			continue;
		}
		const FStagePatchScheduleFixture& Fixture = Fixtures[*Index];
		if (Row.Patch.IsPatched() && Fixture.Footprint <= 0)
		{
			Result.Rejected.Add({ Row.Line, FText::Format(LOCTEXT("NoDMX", "Fixture {0} takes no DMX."), Row.FixtureId) });
			continue;
		}
		if (Row.Patch.IsPatched() && !StageDMXPatching::Fits(Row.Patch.Address, Fixture.Footprint))
		{
			Result.Rejected.Add({ Row.Line, FText::Format(LOCTEXT("DoesNotFit", "Fixture {0}'s {1} channels do not fit at {2}."), Row.FixtureId,
				Fixture.Footprint, FormatPatch(Row.Patch)) });
			continue;
		}

		FStagePatchScheduleChange Change;
		Change.FixtureId = Row.FixtureId;
		Change.Patch = Row.Patch;
		Change.Options = Fixture.Options;
		Change.bOptionsFromFile = Row.bInvertPan.IsSet() || Row.bInvertTilt.IsSet() || Row.bSwapPanTilt.IsSet();
		Change.Options.bInvertPan = Row.bInvertPan.Get(Change.Options.bInvertPan);
		Change.Options.bInvertTilt = Row.bInvertTilt.Get(Change.Options.bInvertTilt);
		Change.Options.bSwapPanTilt = Row.bSwapPanTilt.Get(Change.Options.bSwapPanTilt);
		if (Change.Patch == Fixture.Patch && Change.Options == Fixture.Options)
		{
			++Result.Unchanged;
			continue;
		}
		NewPatches.Add(Change.FixtureId, Change.Patch);
		Result.Changes.Add(Change);
	}

	// Overlaps are allowed (as everywhere in the patch) but reported, over the stage as it would be.
	TArray<FStageDMXPatchSlot> Slots;
	Slots.Reserve(Fixtures.Num());
	for (const FStagePatchScheduleFixture& Fixture : Fixtures)
	{
		const FStageDMXPatch* NewPatch = NewPatches.Find(Fixture.FixtureId);
		Slots.Add({ Fixture.FixtureId, NewPatch ? *NewPatch : Fixture.Patch, Fixture.Footprint });
	}
	Result.Conflicts = StageDMXPatching::FindConflicts(Slots);
	return Result;
}

#undef LOCTEXT_NAMESPACE
