// Copyright Epic Games, Inc. All Rights Reserved.

#include "Report/StageBillOfMaterials.h"

#include "Misc/ComparisonUtility.h"
#include "Misc/DateTime.h"
#include "Outliner/StageOutlinerModel.h"

namespace StageBillOfMaterials
{
	namespace
	{
		const TCHAR* const LineEnd = TEXT("\r\n");

		double SanitizeUnitValue(double Value)
		{
			return FMath::IsFinite(Value) && Value > 0.0 ? Value : 0.0;
		}

		/** Placements of one catalog item merge on this; items without catalog data (level-authored oddities) merge by name. */
		FString MakeLineKey(const FStageBomSource& Source)
		{
			return Source.ItemPath.IsValid() ? Source.ItemPath.ToString() : TEXT("name:") + Source.ItemName.ToString();
		}

		FString FormatNumber(double Value)
		{
			// Printf is locale independent: always "." decimals, which every spreadsheet imports as a number from CSV.
			return FString::Printf(TEXT("%.2f"), Value);
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
	}

	FStageBillOfMaterials Build(TConstArrayView<FStageBomSource> Sources)
	{
		FStageBillOfMaterials Report;
		TMap<FString, TPair<int32, int32>> LineIndexByKey;		// key -> (group, line)

		for (const FStageBomSource& Source : Sources)
		{
			const FGameplayTag GroupCategory = StageOutliner::GetGroupCategory(Source.Category);
			int32 GroupIndex = Report.Groups.IndexOfByPredicate([&GroupCategory](const FStageBomGroup& Group) { return Group.Category == GroupCategory; });
			if (GroupIndex == INDEX_NONE)
			{
				GroupIndex = Report.Groups.AddDefaulted();
				Report.Groups[GroupIndex].Category = GroupCategory;
				Report.Groups[GroupIndex].Label = StageOutliner::MakeGroupLabel(GroupCategory);
			}

			const FString Key = MakeLineKey(Source);
			FStageBomLine* Line = nullptr;
			if (const TPair<int32, int32>* Existing = LineIndexByKey.Find(Key))
			{
				Line = &Report.Groups[Existing->Key].Lines[Existing->Value];
			}
			else
			{
				TArray<FStageBomLine>& Lines = Report.Groups[GroupIndex].Lines;
				LineIndexByKey.Add(Key, { GroupIndex, Lines.Num() });
				Line = &Lines.AddDefaulted_GetRef();
				Line->ItemPath = Source.ItemPath;
				Line->ItemName = Source.ItemName;
				Line->Manufacturer = Source.Manufacturer;
				Line->UnitWeightKg = SanitizeUnitValue(Source.UnitWeightKg);
				Line->UnitPowerWatts = SanitizeUnitValue(Source.UnitPowerWatts);
			}

			Line->Quantity += FMath::Max(Source.Quantity, 1);
			++Line->Placements;
		}

		Report.Groups.Sort([](const FStageBomGroup& A, const FStageBomGroup& B)
		{
			const int32 OrderA = StageOutliner::GetGroupOrder(A.Category);
			const int32 OrderB = StageOutliner::GetGroupOrder(B.Category);
			return OrderA != OrderB ? OrderA < OrderB : A.Label.CompareTo(B.Label) < 0;
		});

		for (FStageBomGroup& Group : Report.Groups)
		{
			Group.Lines.Sort([](const FStageBomLine& A, const FStageBomLine& B)
			{
				const int32 ByName = UE::ComparisonUtility::CompareNaturalOrder(A.ItemName.ToString(), B.ItemName.ToString());
				return ByName != 0 ? ByName < 0 : A.ItemPath.ToString() < B.ItemPath.ToString();
			});

			for (FStageBomLine& Line : Group.Lines)
			{
				Line.TotalWeightKg = Line.UnitWeightKg * Line.Quantity;
				Line.TotalPowerWatts = Line.UnitPowerWatts * Line.Quantity;
				Group.Quantity += Line.Quantity;
				Group.Placements += Line.Placements;
				Group.TotalWeightKg += Line.TotalWeightKg;
				Group.TotalPowerWatts += Line.TotalPowerWatts;
			}

			Report.LineCount += Group.Lines.Num();
			Report.Quantity += Group.Quantity;
			Report.Placements += Group.Placements;
			Report.TotalWeightKg += Group.TotalWeightKg;
			Report.TotalPowerWatts += Group.TotalPowerWatts;
		}
		return Report;
	}

	FString EscapeCsvField(const FString& Field)
	{
		FString Safe = Field;
		if (!Safe.IsEmpty() && FCString::Strchr(TEXT("=+-@\t\r"), Safe[0]) != nullptr)
		{
			// Spreadsheets run such cells as formulas (CSV injection); the apostrophe makes them plain text.
			Safe.InsertAt(0, TEXT('\''));
		}

		const bool bNeedsQuotes = Safe.Contains(TEXT(",")) || Safe.Contains(TEXT("\"")) || Safe.Contains(TEXT("\n")) || Safe.Contains(TEXT("\r"));
		if (!bNeedsQuotes)
		{
			return Safe;
		}
		return TEXT("\"") + Safe.Replace(TEXT("\""), TEXT("\"\"")) + TEXT("\"");
	}

	FString ToCsv(const FStageBillOfMaterials& Report, const FString& SceneName, const FDateTime& GeneratedAt)
	{
		FString Out;
		Out.Reserve(256 + Report.LineCount * 96);

		// U+FEFF: written as the UTF-8 byte order mark, which spreadsheet programs need to read non-ASCII names correctly.
		Out += TEXT("﻿");
		AppendRow(Out, { EscapeCsvField(TEXT("StageCraft 3D Bill of Materials")) });
		AppendRow(Out, { TEXT("Scene"), EscapeCsvField(SceneName.IsEmpty() ? TEXT("Untitled") : SceneName) });
		AppendRow(Out, { TEXT("Generated"), GeneratedAt.ToString(TEXT("%Y-%m-%d %H:%M")) });
		Out += LineEnd;

		AppendRow(Out, { TEXT("Category"), TEXT("Item"), TEXT("Manufacturer"), TEXT("Quantity"), TEXT("Placements"),
			TEXT("Unit Weight (kg)"), TEXT("Total Weight (kg)"), TEXT("Unit Power (W)"), TEXT("Total Power (W)") });

		for (const FStageBomGroup& Group : Report.Groups)
		{
			const FString Category = EscapeCsvField(Group.Label.ToString());
			for (const FStageBomLine& Line : Group.Lines)
			{
				AppendRow(Out, { Category, EscapeCsvField(Line.ItemName.ToString()), EscapeCsvField(Line.Manufacturer.ToString()),
					FString::FromInt(Line.Quantity), FString::FromInt(Line.Placements),
					FormatNumber(Line.UnitWeightKg), FormatNumber(Line.TotalWeightKg), FormatNumber(Line.UnitPowerWatts), FormatNumber(Line.TotalPowerWatts) });
			}
			AppendRow(Out, { EscapeCsvField(Group.Label.ToString() + TEXT(" subtotal")), FString(), FString(),
				FString::FromInt(Group.Quantity), FString::FromInt(Group.Placements),
				FString(), FormatNumber(Group.TotalWeightKg), FString(), FormatNumber(Group.TotalPowerWatts) });
		}

		AppendRow(Out, { TEXT("TOTAL"), FString(), FString(), FString::FromInt(Report.Quantity), FString::FromInt(Report.Placements),
			FString(), FormatNumber(Report.TotalWeightKg), FString(), FormatNumber(Report.TotalPowerWatts) });
		return Out;
	}

	FString MakeFileName(const FString& SceneName, const FDateTime& GeneratedAt)
	{
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
		return FString::Printf(TEXT("BOM_%s_%s.csv"), *Safe, *GeneratedAt.ToString(TEXT("%Y%m%d-%H%M%S")));
	}
}
