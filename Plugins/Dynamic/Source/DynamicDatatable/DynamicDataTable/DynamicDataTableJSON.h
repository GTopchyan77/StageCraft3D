// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Dom/JsonValue.h"
#include "Dom/JsonObject.h"

class UDynamicDataTable;
enum class EDynamicDataTableExportFlags : uint8;

template <class CharType> struct TPrettyJsonPrintPolicy;

// forward declare JSON writer
template <class CharType>
struct TPrettyJsonPrintPolicy;
template <class CharType, class PrintPolicy>
class TJsonWriter;

namespace DynamicDataTableJSONUtils
{
	/** Returns what string is used as the key/name field for a data table */
	FString GetKeyFieldName(const UDynamicDataTable& InDynamicDataTable);
}

#if WITH_EDITOR

/** 
 * Class to serialize a DynamicDataTable to a TJsonWriter.
 */
template<typename CharType = TCHAR>
class TDynamicDataTableExporterJSON
{
public:
	typedef TJsonWriter<CharType, TPrettyJsonPrintPolicy<CharType>> FDynamicDataTableJsonWriter;

	TDynamicDataTableExporterJSON(const EDynamicDataTableExportFlags InDTExportFlags, TSharedRef<FDynamicDataTableJsonWriter> InJsonWriter);

	~TDynamicDataTableExporterJSON();

	/** Writes the data table out as an array of objects */
	bool WriteTable(const UDynamicDataTable& InDynamicDataTable);

	/** Writes the data table out as a named object with each row being a sub value on that object */
	bool WriteTableAsObject(const UDynamicDataTable& InDynamicDataTable);

	/** Writes out a single row */
	bool WriteRow(const UScriptStruct* InRowStruct, const void* InRowData, const FString* FieldToSkip = nullptr);

	/** Writes the contents of a single row */
	bool WriteStruct(const UScriptStruct* InStruct, const void* InStructData, const FString* FieldToSkip = nullptr);

protected:
	bool WriteStructEntry(const void* InRowData, const FProperty* InProperty, const void* InPropertyData);

	bool WriteContainerEntry(const FProperty* InProperty, const void* InPropertyData, const FString* InIdentifier = nullptr);

	EDynamicDataTableExportFlags DTExportFlags;
	TSharedRef<FDynamicDataTableJsonWriter> JsonWriter;
	bool bJsonWriterNeedsClose;
};

/**
 * TCHAR-specific instantiation of TDynamicDataTableExporterJSON that has a convenience constructor to write output to an FString instead of an external TJsonWriter
 */
class FDynamicDataTableExporterJSON : public TDynamicDataTableExporterJSON<TCHAR>
{
public:
	using TDynamicDataTableExporterJSON<TCHAR>::TDynamicDataTableExporterJSON;
// 
	FDynamicDataTableExporterJSON(const EDynamicDataTableExportFlags InDTExportFlags, FString& OutExportText);
};

#endif // WITH_EDITOR

class FDynamicDataTableImporterJSON
{
public:
	FDynamicDataTableImporterJSON(UDynamicDataTable& InDynamicDataTable, const FString& InJSONData, TArray<FString>& OutProblems);

	~FDynamicDataTableImporterJSON();

	bool ReadTable();

private:
	bool ReadRow(const TSharedRef<FJsonObject>& InParsedTableRowObject, const int32 InRowIdx);

	bool ReadStruct(const TSharedRef<FJsonObject>& InParsedObject, UScriptStruct* InStruct, const FName InRowName, void* InStructData);

	bool ReadStructEntry(const TSharedRef<FJsonValue>& InParsedPropertyValue, const FName InRowName, const FString& InColumnName, const void* InRowData, FProperty* InProperty, void* InPropertyData);

	bool ReadContainerEntry(const TSharedRef<FJsonValue>& InParsedPropertyValue, const FName InRowName, const FString& InColumnName, const int32 InArrayEntryIndex, FProperty* InProperty, void* InPropertyData);

	UDynamicDataTable* DynamicDataTable;
	const FString& JSONData;
	TArray<FString>& ImportProblems;
};
