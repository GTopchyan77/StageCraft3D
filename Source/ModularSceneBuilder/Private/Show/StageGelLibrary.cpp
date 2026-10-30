// Copyright Epic Games, Inc. All Rights Reserved.

#include "Show/StageGelLibrary.h"

#include "Engine/DataTable.h"
#include "ModularSceneBuilder.h"
#include "Show/StageLightingSettings.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageGelLibrary)

#define LOCTEXT_NAMESPACE "StageCraftGelLibrary"

namespace
{
	/** Lower case letters and digits only, so "Lee 106", "lee-106" and "LEE106" compare equal. */
	FString Normalize(const FString& Text)
	{
		FString Result;
		Result.Reserve(Text.Len());
		for (const TCHAR Character : Text)
		{
			if (FChar::IsAlnum(Character))
			{
				Result.AppendChar(FChar::ToLower(Character));
			}
		}
		return Result;
	}

	/** "007" and "7" are the same catalogue number. */
	FString TrimLeadingZeros(const FString& Number)
	{
		int32 First = 0;
		while (First < Number.Len() - 1 && Number[First] == TEXT('0'))
		{
			++First;
		}
		return Number.Mid(First);
	}

	bool IsAllDigits(const FString& Text)
	{
		if (Text.IsEmpty())
		{
			return false;
		}
		for (const TCHAR Character : Text)
		{
			if (!FChar::IsDigit(Character))
			{
				return false;
			}
		}
		return true;
	}

	/** Words that name what a gel is rather than which one: "primary red gel", "lee 106 filter". */
	FString StripGenericWords(const FString& Query)
	{
		TArray<FString> Words;
		Query.ParseIntoArrayWS(Words);
		static const TCHAR* const Generic[] = { TEXT("gel"), TEXT("gels"), TEXT("filter"), TEXT("color"), TEXT("colour") };
		Words.RemoveAll([](const FString& Word)
		{
			for (const TCHAR* GenericWord : Generic)
			{
				if (Word.Equals(GenericWord, ESearchCase::IgnoreCase))
				{
					return true;
				}
			}
			return false;
		});
		return FString::Join(Words, TEXT(" "));
	}

	/** Splits "lee106" / "l106" / "rosco27" / "roscolux27" / "r27" into brand and number. */
	bool ParseBrandAndNumber(const FString& Normalized, TOptional<EStageGelBrand>& OutBrand, FString& OutNumber)
	{
		static const TPair<const TCHAR*, EStageGelBrand> Prefixes[] = {
			{ TEXT("roscolux"), EStageGelBrand::Rosco }, { TEXT("rosco"), EStageGelBrand::Rosco }, { TEXT("lee"), EStageGelBrand::Lee },
			{ TEXT("r"), EStageGelBrand::Rosco }, { TEXT("l"), EStageGelBrand::Lee },
		};
		if (IsAllDigits(Normalized))
		{
			OutBrand.Reset();
			OutNumber = TrimLeadingZeros(Normalized);
			return true;
		}
		for (const TPair<const TCHAR*, EStageGelBrand>& Prefix : Prefixes)
		{
			if (Normalized.StartsWith(Prefix.Key) && IsAllDigits(Normalized.Mid(FCString::Strlen(Prefix.Key))))
			{
				OutBrand = Prefix.Value;
				OutNumber = TrimLeadingZeros(Normalized.Mid(FCString::Strlen(Prefix.Key)));
				return true;
			}
		}
		return false;
	}

	const TCHAR* GetBrandLetter(EStageGelBrand Brand)
	{
		return Brand == EStageGelBrand::Rosco ? TEXT("R") : TEXT("L");
	}
}

FString FStageGel::GetCode() const
{
	return FString(GetBrandLetter(Brand)) + Number;
}

FText FStageGel::GetLabel() const
{
	return FText::Format(LOCTEXT("GelLabel", "{0} {1} {2}"),
		Brand == EStageGelBrand::Rosco ? LOCTEXT("Rosco", "Rosco") : LOCTEXT("Lee", "Lee"), FText::FromString(Number), DisplayName);
}

FStageGelLibrary FStageGelLibrary::Build(TConstArrayView<FStageGelRow> Rows)
{
	FStageGelLibrary Library;
	TSet<FString> Codes;
	for (const FStageGelRow& Row : Rows)
	{
		FStageGel Gel;
		Gel.Brand = Row.Brand;
		// Kept as printed ("002"); compared without leading zeros.
		Gel.Number = Row.Number.TrimStartAndEnd();
		Gel.DisplayName = Row.DisplayName;

		if (!IsAllDigits(Gel.Number))
		{
			UE_LOG(LogStageCraft, Warning, TEXT("Gel library: skipped a row with catalogue number '%s' (digits only)."), *Row.Number);
			continue;
		}
		const FString Code = FString(GetBrandLetter(Gel.Brand)) + TrimLeadingZeros(Gel.Number);
		if (Codes.Contains(Code))
		{
			UE_LOG(LogStageCraft, Warning, TEXT("Gel library: skipped duplicate gel %s."), *Code);
			continue;
		}
		const FLinearColor Linear = FLinearColor::FromSRGBColor(Row.Swatch);
		const float Peak = FMath::Max3(Linear.R, Linear.G, Linear.B);
		if (!(Peak > UE_SMALL_NUMBER))
		{
			UE_LOG(LogStageCraft, Warning, TEXT("Gel library: skipped gel %s, its swatch is black."), *Code);
			continue;
		}
		Gel.Filter = FLinearColor(Linear.R / Peak, Linear.G / Peak, Linear.B / Peak);
		Codes.Add(Code);
		Library.Gels.Add(MoveTemp(Gel));
	}

	Library.Gels.StableSort([](const FStageGel& A, const FStageGel& B)
	{
		if (A.Brand != B.Brand)
		{
			return A.Brand < B.Brand;
		}
		// Validated as digits, so the numeric order is the swatch book's.
		return FCString::Atoi64(*A.Number) < FCString::Atoi64(*B.Number);
	});
	return Library;
}

const FStageGelLibrary& FStageGelLibrary::Get()
{
	check(IsInGameThread());
	// Gels are shipped content, so the validated list is built once per run; editing the table needs a restart.
	static const FStageGelLibrary Library = []
	{
		const UStageLightingSettings* Settings = GetDefault<UStageLightingSettings>();
		const UDataTable* Table = Settings ? Settings->GelTable.LoadSynchronous() : nullptr;
		if (!Table)
		{
			UE_LOG(LogStageCraft, Warning, TEXT("Gel library: no GelTable set ([/Script/ModularSceneBuilder.StageLightingSettings] in DefaultGame.ini); no gels are offered."));
			return FStageGelLibrary();
		}
		if (Table->GetRowStruct() != FStageGelRow::StaticStruct())
		{
			UE_LOG(LogStageCraft, Warning, TEXT("Gel library: %s does not hold StageGelRow rows; no gels are offered."), *Table->GetPathName());
			return FStageGelLibrary();
		}
		TArray<FStageGelRow> Rows;
		Table->ForeachRow<FStageGelRow>(TEXT("StageGelLibrary"), [&Rows](const FName&, const FStageGelRow& Row) { Rows.Add(Row); });
		FStageGelLibrary Built = Build(Rows);
		UE_LOG(LogStageCraft, Log, TEXT("Gel library: %d gels from %s."), Built.Gels.Num(), *Table->GetPathName());
		return Built;
	}();
	return Library;
}

int32 FStageGelLibrary::FindByCode(const FString& Code) const
{
	return Gels.IndexOfByPredicate([&Code](const FStageGel& Gel) { return Gel.GetCode().Equals(Code, ESearchCase::IgnoreCase); });
}

int32 FStageGelLibrary::Find(const FString& Query) const
{
	const FString Normalized = Normalize(StripGenericWords(Query));
	if (Normalized.IsEmpty())
	{
		return INDEX_NONE;
	}

	TOptional<EStageGelBrand> Brand;
	FString Number;
	if (ParseBrandAndNumber(Normalized, Brand, Number))
	{
		int32 Found = INDEX_NONE;
		for (int32 Index = 0; Index < Gels.Num(); ++Index)
		{
			if (TrimLeadingZeros(Gels[Index].Number) == Number && (!Brand.IsSet() || Gels[Index].Brand == Brand.GetValue()))
			{
				if (Found != INDEX_NONE)
				{
					// A bare number both brands use ("27") is ambiguous; asking is better than guessing.
					return INDEX_NONE;
				}
				Found = Index;
			}
		}
		return Found;
	}

	// By name: an exact name first, then a part of a name that only one gel has.
	int32 Partial = INDEX_NONE;
	int32 PartialMatches = 0;
	for (int32 Index = 0; Index < Gels.Num(); ++Index)
	{
		const FString Name = Normalize(Gels[Index].DisplayName.ToString());
		if (Name == Normalized)
		{
			return Index;
		}
		if (Normalized.Len() >= 3 && Name.Contains(Normalized))
		{
			Partial = Index;
			++PartialMatches;
		}
	}
	return PartialMatches == 1 ? Partial : INDEX_NONE;
}

#undef LOCTEXT_NAMESPACE
