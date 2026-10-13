// Copyright Epic Games, Inc. All Rights Reserved.

#include "DynamicDataTableFunctionLibrary.h"

#include "DynamicDatatable/DynamicDataTable/DynamicDataTable.h"
#include "Engine/CurveTable.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(DynamicDataTableFunctionLibrary)

#if WITH_EDITOR
#include "EditorFramework/AssetImportData.h"
#include "Factories/CSVImportFactory.h"
#include "HAL/FileManager.h"
#endif //WITH_EDITOR

UDynamicDataTableFunctionLibrary::UDynamicDataTableFunctionLibrary(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void UDynamicDataTableFunctionLibrary::EvaluateCurveTableRow(UCurveTable* CurveTable, FName RowName, float InXY, TEnumAsByte<EDynEvaluateCurveTableResult::Type>& OutResult, float& OutXY,const FString& ContextString)
{
	FCurveTableRowHandle Handle;
	Handle.CurveTable = CurveTable;
	Handle.RowName = RowName;
	
	bool found = Handle.Eval(InXY, &OutXY,ContextString);
	
	if (found)
	{
	    OutResult = EDynEvaluateCurveTableResult::RowFound;
	}
	else
	{
	    OutResult = EDynEvaluateCurveTableResult::RowNotFound;
	}
}

bool UDynamicDataTableFunctionLibrary::DoesDynamicDataTableRowExist(UDynamicDataTable* Table, FName RowName)
{
	if (!Table)
	{
		return false;
	}
	else if (Table->RowStruct == nullptr)
	{
		return false;
	}
	return Table->GetRowMap().Find(RowName) != nullptr;
}

TArray<FString> UDynamicDataTableFunctionLibrary::GetDynamicDataTableColumnAsString(const UDynamicDataTable* DynamicDataTable, FName PropertyName)
{
	if (DynamicDataTable && PropertyName != NAME_None)
	{
		EDynamicDataTableExportFlags ExportFlags = EDynamicDataTableExportFlags::None;
		return DynamicDataTableUtils::GetColumnDataAsString(DynamicDataTable, PropertyName, ExportFlags);
	}
	return TArray<FString>();
}

bool UDynamicDataTableFunctionLibrary::Generic_GetDynamicDataTableRowFromName(const UDynamicDataTable* Table, FName RowName, void* OutRowPtr)
{
	bool bFoundRow = false;

	if (OutRowPtr && Table)
	{
		void* RowPtr = Table->FindRowUnchecked(RowName);

		if (RowPtr != nullptr)
		{
			const UScriptStruct* StructType = Table->GetRowStruct();

			if (StructType != nullptr)
			{
				StructType->CopyScriptStruct(OutRowPtr, RowPtr);
				bFoundRow = true;
			}
		}
	}

	return bFoundRow;
}

bool UDynamicDataTableFunctionLibrary::GetDynamicDataTableRowFromName(UDynamicDataTable* Table, FName RowName, FDynamicTableRowBase& OutRow)
{
	// We should never hit this!  stubs to avoid NoExport on the class.
	check(0);
	return false;
}

void UDynamicDataTableFunctionLibrary::GetDynamicDataTableRowNames(UDynamicDataTable* Table, TArray<FName>& OutRowNames)
{
	if (Table)
	{
		OutRowNames = Table->GetRowNames();
	}
	else
	{
		OutRowNames.Empty();
	}
}


#if WITH_EDITOR
bool UDynamicDataTableFunctionLibrary::FillDynamicDataTableFromCSVString(UDynamicDataTable* DynamicDataTable, const FString& InString)
{
	if (!DynamicDataTable)
	{
		UE_LOG(LogDynamicDataTable, Error, TEXT("FillDynamicDataTableFromCSVString - The DynamicDataTable is invalid."));
		return false;
	}

	UCSVImportFactory* ImportFactory = NewObject<UCSVImportFactory>();

	bool bWasCancelled = false;
	const TCHAR* Buffer = *InString;
	UObject* Result = ImportFactory->FactoryCreateText(DynamicDataTable->GetClass()
		, DynamicDataTable->GetOuter()
		, DynamicDataTable->GetFName()
		, DynamicDataTable->GetFlags()
		, nullptr
		, TEXT("csv")
		, Buffer
		, Buffer + InString.Len()
		, nullptr
		, bWasCancelled);

	return Result != nullptr && !bWasCancelled;
}

bool UDynamicDataTableFunctionLibrary::FillDynamicDataTableFromCSVFile(UDynamicDataTable* DynamicDataTable, const FString& InFilePath)
{
	if (!DynamicDataTable)
	{
		UE_LOG(LogDynamicDataTable, Error, TEXT("FillDynamicDataTableFromCSVFile - The DynamicDataTable is invalid."));
		return false;
	}

	if (!IFileManager::Get().FileExists(*InFilePath))
	{
		UE_LOG(LogDynamicDataTable, Error, TEXT("FillDynamicDataTableFromCSVFile - The file '%s' doesn't exist."), *InFilePath);
		return false;
	}

	DynamicDataTable->AssetImportData->Update(InFilePath);
	UCSVImportFactory* ImportFactory = NewObject<UCSVImportFactory>();
	return ImportFactory->ReimportCSV(DynamicDataTable) == EReimportResult::Succeeded;
}

bool UDynamicDataTableFunctionLibrary::FillDynamicDataTableFromJSONString(UDynamicDataTable* DynamicDataTable, const FString& InString)
{
	if (!DynamicDataTable)
	{
		UE_LOG(LogDynamicDataTable, Error, TEXT("FillDynamicDataTableFromJSONString - The DynamicDataTable is invalid."));
		return false;
	}

	UCSVImportFactory* ImportFactory = NewObject<UCSVImportFactory>();

	bool bWasCancelled = false;
	const TCHAR* Buffer = *InString;
	UObject* Result = ImportFactory->FactoryCreateText(DynamicDataTable->GetClass()
		, DynamicDataTable->GetOuter()
		, DynamicDataTable->GetFName()
		, DynamicDataTable->GetFlags()
		, nullptr
		, TEXT("json")
		, Buffer
		, Buffer + InString.Len()
		, nullptr
		, bWasCancelled);

	return Result != nullptr && !bWasCancelled;
}

bool UDynamicDataTableFunctionLibrary::FillDynamicDataTableFromJSONFile(UDynamicDataTable* DynamicDataTable, const FString& InFilePath, UScriptStruct* ImportRowStruct)
{
	if (!DynamicDataTable)
	{
		UE_LOG(LogDynamicDataTable, Error, TEXT("FillDynamicDataTableFromJSONFile - The DynamicDataTable is invalid."));
		return false;
	}

	if (!IFileManager::Get().FileExists(*InFilePath))
	{
		UE_LOG(LogDynamicDataTable, Error, TEXT("FillDynamicDataTableFromJSONFile - The file '%s' doesn't exist."), *InFilePath);
		return false;
	}

	if (!InFilePath.EndsWith(TEXT(".json")))
	{
		UE_LOG(LogDynamicDataTable, Error, TEXT("FillDynamicDataTableFromJSONFile - The file is not a json."));
		return false;
	}

	DynamicDataTable->AssetImportData->Update(InFilePath);
	UCSVImportFactory* ImportFactory = NewObject<UCSVImportFactory>();
	ImportFactory->AutomatedImportSettings.ImportRowStruct = ImportRowStruct;
	return ImportFactory->ReimportCSV(DynamicDataTable) == EReimportResult::Succeeded;
}
#endif //WITH_EDITOR
