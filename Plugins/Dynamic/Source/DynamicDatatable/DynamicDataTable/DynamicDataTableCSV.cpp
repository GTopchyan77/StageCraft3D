// Copyright Epic Games, Inc. All Rights Reserved.

#include "DynamicDataTableCSV.h"
#include "UObject/UnrealType.h"
#include "DynamicDataTable.h"
#include "Serialization/Csv/CsvParser.h"
#include "StructUtils/UserDefinedStruct.h"

#if WITH_EDITOR

FDynamicDataTableExporterCSV::FDynamicDataTableExporterCSV(const EDynamicDataTableExportFlags InDTExportFlags, FString& OutExportText)
	: DTExportFlags(InDTExportFlags)
	, ExportedText(OutExportText)
{
}

FDynamicDataTableExporterCSV::~FDynamicDataTableExporterCSV()
{
}

bool FDynamicDataTableExporterCSV::WriteTable(const UDynamicDataTable& InDynamicDataTable)
{
	if (!InDynamicDataTable.RowStruct)
	{
		return false;
	}

	// Write the header (column titles)
	FString ImportKeyField;
	if (!InDynamicDataTable.ImportKeyField.IsEmpty())
	{
		// Write actual name if we have it
		ImportKeyField = InDynamicDataTable.ImportKeyField;
		ExportedText += ImportKeyField;
	}
	else
	{
	ExportedText += TEXT("---");
	}

	FProperty* SkipProperty = nullptr;
	for (TFieldIterator<FProperty> It(InDynamicDataTable.RowStruct); It; ++It)
	{
		FProperty* BaseProp = *It;
		check(BaseProp);

		FString ColumnHeader = DynamicDataTableUtils::GetPropertyExportName(BaseProp, DTExportFlags);
		
		if (ColumnHeader == ImportKeyField)
		{
			// Don't write header again if this is the name field, and save for skipping later
			SkipProperty = BaseProp;
			continue;
		}
		
		ExportedText += TEXT(",");
		ExportedText += ColumnHeader;
	}
	ExportedText += TEXT("\n");

	// Write each row
	for (auto RowIt = InDynamicDataTable.GetRowMap().CreateConstIterator(); RowIt; ++RowIt)
	{
		FName RowName = RowIt.Key();
		ExportedText += RowName.ToString();

		uint8* RowData = RowIt.Value();
		WriteRow(InDynamicDataTable.RowStruct, RowData, SkipProperty);

		ExportedText += TEXT("\n");
	}

	return true;
}

bool FDynamicDataTableExporterCSV::WriteRow(const UScriptStruct* InRowStruct, const void* InRowData, const FProperty* SkipProperty)
{
	if (!InRowStruct)
	{
		return false;
	}

	for (TFieldIterator<FProperty> It(InRowStruct); It; ++It)
	{
		FProperty* BaseProp = *It;
		check(BaseProp);

		if (BaseProp == SkipProperty)
		{
			continue;
		}

		const void* Data = BaseProp->ContainerPtrToValuePtr<void>(InRowData, 0);
		WriteStructEntry(InRowData, BaseProp, Data);
	}

	return true;
}

bool FDynamicDataTableExporterCSV::WriteStructEntry(const void* InRowData, FProperty* InProperty, const void* InPropertyData)
{
	ExportedText += TEXT(",");

	const FString PropertyValue = DynamicDataTableUtils::GetPropertyValueAsString(InProperty, (uint8*)InRowData, DTExportFlags);
	ExportedText += TEXT("\"");
	ExportedText += PropertyValue.Replace(TEXT("\""), TEXT("\"\""));
	ExportedText += TEXT("\"");

	return true;
}


#endif // WITH_EDITOR


FDynamicDataTableImporterCSV::FDynamicDataTableImporterCSV(UDynamicDataTable& InDynamicDataTable, FString InCSVData, TArray<FString>& OutProblems)
	: DynamicDataTable(&InDynamicDataTable)
	, CSVData(MoveTemp(InCSVData))
	, ImportProblems(OutProblems)
{
}

FDynamicDataTableImporterCSV::~FDynamicDataTableImporterCSV()
{
}

bool FDynamicDataTableImporterCSV::ReadTable()
{
	if (CSVData.IsEmpty())
	{
		ImportProblems.Add(TEXT("Input data is empty."));
		return false;
	}

	// Check we have a RowStruct specified
	if (!DynamicDataTable->RowStruct)
	{
		ImportProblems.Add(TEXT("No RowStruct specified."));
		return false;
	}

	const FCsvParser Parser(CSVData);
	const auto& Rows = Parser.GetRows();

	// Must have at least a header row (column names)
	if(Rows.Num() < 1)
	{
		ImportProblems.Add(TEXT("Too few rows (there must be at least a header row)."));
		return false;
	}

	// Find property for each column
	int32 KeyColumn = 0;
	if (!DynamicDataTable->ImportKeyField.IsEmpty())
	{
		// Search for key column by name
		for (int32 ColIdx = 0; ColIdx < Rows[0].Num(); ++ColIdx)
		{
			const TCHAR* ColumnValue = Rows[0][ColIdx];
			if (DynamicDataTable->ImportKeyField == FString(ColumnValue))
			{
				KeyColumn = ColIdx;
				break;
			}
		}
	}
	
	TArray<FProperty*> ColumnProps = DynamicDataTable->GetTablePropertyArray(Rows[0], DynamicDataTable->RowStruct, ImportProblems, KeyColumn);

	// Empty existing data
	DynamicDataTable->EmptyTable();

	// Iterate over rows
	for(int32 RowIdx=1; RowIdx<Rows.Num(); RowIdx++)
	{
		const TArray<const TCHAR*>& Cells = Rows[RowIdx];

		// Need at least the key column
		if (Cells.Num() <= KeyColumn)
		{
			ImportProblems.Add(FString::Printf(TEXT("Row '%d' has too few cells."), RowIdx));
			continue;
		}

		// Need enough columns in the properties!
		if( ColumnProps.Num() < Cells.Num() )
		{
			ImportProblems.Add(FString::Printf(TEXT("Row '%d' has more cells than properties, is there a malformed string?"), RowIdx));
			continue;
		}

		// Get row name
		FName RowName = DynamicDataTableUtils::MakeValidName(Cells[KeyColumn]);

		// Check its not 'none'
		if(RowName == NAME_None)
		{
			if (!DynamicDataTable->ImportKeyField.IsEmpty())
			{
				ImportProblems.Add(FString::Printf(TEXT("Row '%d' missing key field '%s'."), RowIdx, *DynamicDataTable->ImportKeyField));
			}
			else
			{
			ImportProblems.Add(FString::Printf(TEXT("Row '%d' missing a name."), RowIdx));
			}

			continue;
		}

		// Check its not a duplicate
		if(!DynamicDataTable->AllowDuplicateRowsOnImport() && DynamicDataTable->GetRowMap().Find(RowName) != nullptr)
		{
			ImportProblems.Add(FString::Printf(TEXT("Duplicate row name '%s'."), *RowName.ToString()));
			continue;
		}

		// Allocate data to store information, using UScriptStruct to know its size
		uint8* RowData = (uint8*)FMemory::Malloc(DynamicDataTable->RowStruct->GetStructureSize());
		DynamicDataTable->RowStruct->InitializeStruct(RowData);
		// And be sure to call DestroyScriptStruct later

		// Add to row map
		DynamicDataTable->AddRowInternal(RowName, RowData);

		// Now iterate over cells (skipping first cell unless we had an explicit name)
		for(int32 CellIdx = 0; CellIdx < Cells.Num(); CellIdx++)
		{
			if (CellIdx == KeyColumn)
		{
				continue;
			}

			// Try and assign string to data using the column property
			FProperty* ColumnProp = ColumnProps[CellIdx];
			const FString CellValue = Cells[CellIdx];
			FString Error = DynamicDataTableUtils::AssignStringToProperty(CellValue, ColumnProp, RowData);

			// If we failed, output a problem string
			if(Error.Len() > 0)
			{
				FString ColumnName = (ColumnProp != nullptr) 
					? DynamicDataTableUtils::GetPropertyExportName(ColumnProp)
					: FString(TEXT("NONE"));
				ImportProblems.Add(FString::Printf(TEXT("Problem assigning string '%s' to property '%s' on row '%s' : %s"), *CellValue, *ColumnName, *RowName.ToString(), *Error));
			}
		}

		// Problem if we didn't have enough cells on this row
		if(Cells.Num() < ColumnProps.Num())
		{
			ImportProblems.Add(FString::Printf(TEXT("Too few cells on row '%s'."), *RowName.ToString()));			
		}
	}

	DynamicDataTable->Modify(true);

	return true;
}

