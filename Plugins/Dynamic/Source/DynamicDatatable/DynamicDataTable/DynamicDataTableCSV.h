// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UDynamicDataTable;
enum class EDynamicDataTableExportFlags : uint8;

#if WITH_EDITOR

class FDynamicDataTableExporterCSV
{
public:
	FDynamicDataTableExporterCSV(const EDynamicDataTableExportFlags InDTExportFlags, FString& OutExportText);

	~FDynamicDataTableExporterCSV();

	bool WriteTable(const UDynamicDataTable& InDynamicDataTable);

	bool WriteRow(const UScriptStruct* InRowStruct, const void* InRowData, const FProperty* SkipProperty = nullptr);

private:
	bool WriteStructEntry(const void* InRowData, FProperty* InProperty, const void* InPropertyData);

	EDynamicDataTableExportFlags DTExportFlags;
	FString& ExportedText;
};

#endif // WITH_EDITOR


class FDynamicDataTableImporterCSV
{
public:
	FDynamicDataTableImporterCSV(UDynamicDataTable& InDynamicDataTable, FString InCSVData, TArray<FString>& OutProblems);

	~FDynamicDataTableImporterCSV();

	bool ReadTable();

private:
	UDynamicDataTable* DynamicDataTable;
	FString CSVData;
	TArray<FString>& ImportProblems;
};

