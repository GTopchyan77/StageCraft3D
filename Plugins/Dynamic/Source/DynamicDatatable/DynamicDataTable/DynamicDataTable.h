// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if UE_ENABLE_INCLUDE_ORDER_DEPRECATED_IN_5_4
#include "CoreMinimal.h"
#endif //UE_ENABLE_INCLUDE_ORDER_DEPRECATED_IN_5_4
#include "UObject/ObjectMacros.h"
#include "UObject/Object.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/PropertyPortFlags.h"
#include "DynamicDataTableUtils.h"
#include "DynamicDatatable/Public/DynamicDatatableBPLibrary.h"
#include "DynamicDataTable.generated.h"

class Error;
class UDynamicDataTable;
struct FDynamicDataTableEditorUtils;
class UGameplayTagTableManager;
class FDynamicDataTableImporterCSV;
class FDynamicDataTableImporterJSON;
template <class CharType> struct TPrettyJsonPrintPolicy;

// forward declare JSON writer
template <class CharType>
struct TPrettyJsonPrintPolicy;
template <class CharType, class PrintPolicy>
class TJsonWriter;


/**
 * Base class for all table row structs to inherit from.
 */
USTRUCT(BlueprintInternalUseOnly)
struct FDynamicTableRowBase
{
	GENERATED_USTRUCT_BODY()

	FDynamicTableRowBase() { }
	virtual ~FDynamicTableRowBase() { }

	/** 
	 * Can be overridden by subclasses; Called whenever the owning data table is imported or re-imported.
	 * Allows for custom fix-ups, parsing, etc. after initial data is read in.
	 * 
	 * @param InDynamicDataTable					The data table that owns this row
	 * @param InRowName						The name of the row we're performing fix-up on
	 * @param OutCollectedImportProblems	List of problems accumulated during import; Can be added to via this method
	 */
	virtual void OnPostDataImport(const UDynamicDataTable* InDynamicDataTable, const FName InRowName, TArray<FString>& OutCollectedImportProblems) {}

	/**
	 * Can be overridden by subclasses; Called on every row when the owning data table is modified
	 * Allows for custom fix-ups, parsing, etc for user changes
	 * This will be called in addition to OnPostDataImport when importing
	 *
	 * @param InDynamicDataTable					The data table that owns this row
	 * @param InRowName						The name of the row we're performing fix-up on
	 */
	virtual void OnDynamicDataTableChanged(const UDynamicDataTable* InDynamicDataTable, const FName InRowName) {}
};


/**
 * Dynamic Data Table Asset
 */
UCLASS(MinimalAPI, BlueprintType, AutoExpandCategories = "DynamicDataTable,ImportOptions", Meta = (LoadBehavior = "LazyOnDemand"))
class UDynamicDataTable
	: public UObject
{
	GENERATED_UCLASS_BODY()

	DYNAMICDATATABLE_API virtual ~UDynamicDataTable() {};

	DECLARE_MULTICAST_DELEGATE(FOnDynamicDataTableChanged);
	DECLARE_MULTICAST_DELEGATE(FOnDynamicDataTableImport);
	
	friend FDynamicDataTableEditorUtils;
	friend UGameplayTagTableManager;
	friend FDynamicDataTableImporterCSV;
	friend FDynamicDataTableImporterJSON;

	/** Structure to use for each row of the table, must inherit from FDynamicTableRowBase */
	UPROPERTY(VisibleAnywhere, Category=DynamicDataTable, meta=(DisplayThumbnail="false"))
	TObjectPtr<UScriptStruct>			RowStruct;

protected:
	/** Map of name of row to row data structure. */
	TMap<FName, uint8*>		RowMap;

	// TODO: remove this, it is temporarily here to allow DynamicDataTableEditorUtils to compile until I get around to updating functions like RemoveRow and RenameRow
	virtual TMap<FName, uint8*>& GetNonConstRowMap() { return RowMap; }

	/** Called to add rows to the data table */
	DYNAMICDATATABLE_API virtual void AddRowInternal(FName RowName, uint8* RowDataPtr);

	/** Deletes the row memory */
	DYNAMICDATATABLE_API virtual void RemoveRowInternal(FName RowName);
public:

	virtual const TMap<FName, uint8*>& GetRowMap() const { return RowMap; }
	virtual const TMap<FName, uint8*>& GetRowMap() { return RowMap; }

	const UScriptStruct* GetRowStruct() const { return RowStruct; }

	/** Returns true if it is valid to import multiple table rows with the same name; returns false otherwise */
	virtual bool AllowDuplicateRowsOnImport() const { return false; }

	/** The Internal Table for use with the plugin. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=DynamicDataTable)
	FDynamicTable DynamicTable;
	
	/** Set to true to not cook this data table into client builds. Useful for sensitive tables that only servers should know about. */
	UPROPERTY(EditAnywhere, Category=DynamicDataTable)
	uint8 bStripFromClientBuilds : 1;

	/** Set to true to ignore extra fields in the import data, if false it will warn about them */
	UPROPERTY(EditAnywhere, Category=ImportOptions)
	uint8 bIgnoreExtraFields : 1;

	/** Set to true to ignore any fields that are expected but missing, if false it will warn about them */
	UPROPERTY(EditAnywhere, Category = ImportOptions)
	uint8 bIgnoreMissingFields : 1;

	/** Explicit field in import data to use as key. If this is empty it uses Name for JSON and the first field found for CSV */
	UPROPERTY(EditAnywhere, Category=ImportOptions)
	FString ImportKeyField;
	
#if WITH_EDITOR
	DYNAMICDATATABLE_API virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	//DYNAMICDATATABLE_API virtual void PostLoadAssetRegistryTags(const FAssetData& InAssetData, TArray<FAssetRegistryTag>& OutTagsAndValuesToUpdate) const override;
#endif // WITH_EDITOR

	//~ Begin UObject Interface.
	DYNAMICDATATABLE_API virtual void FinishDestroy() override;
	DYNAMICDATATABLE_API virtual void Serialize(FStructuredArchiveRecord Record) override;
	DYNAMICDATATABLE_API static void AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector);
	DYNAMICDATATABLE_API virtual void GetPreloadDependencies(TArray<UObject*>& OutDeps) override;
	DYNAMICDATATABLE_API virtual void GetResourceSizeEx(FResourceSizeEx& CumulativeResourceSize) override;
	virtual bool NeedsLoadForClient() const override { return bStripFromClientBuilds ? false : Super::NeedsLoadForClient(); }
	virtual bool NeedsLoadForEditorGame() const override { return bStripFromClientBuilds ? false : Super::NeedsLoadForEditorGame(); }
#if WITH_EDITORONLY_DATA
	UE_DEPRECATED(5.1, "Class names are now represented by path names. Please use GetRowStructPathName.")
	DYNAMICDATATABLE_API FName GetRowStructName() const;
	DYNAMICDATATABLE_API FTopLevelAssetPath GetRowStructPathName() const;
	DYNAMICDATATABLE_API virtual void GetAssetRegistryTags(FAssetRegistryTagsContext Context) const override;
	UE_DEPRECATED(5.4, "Implement the version that takes FAssetRegistryTagsContext instead.")
	DYNAMICDATATABLE_API virtual void GetAssetRegistryTags(TArray<FAssetRegistryTag>& OutTags) const override;
	DYNAMICDATATABLE_API virtual void PostInitProperties() override;
	DYNAMICDATATABLE_API virtual void PostLoad() override;
	//~ End UObject Interface

	/** The file this data table was imported from, may be empty */
	UPROPERTY(VisibleAnywhere, Instanced, Category=ImportSource)
	TObjectPtr<class UAssetImportData> AssetImportData;

	/** The filename imported to create this object. Relative to this object's package, BaseDir() or absolute */
	UPROPERTY()
	FString ImportPath_DEPRECATED;

	/** The name of the RowStruct we were using when we were last saved */
	UPROPERTY()
	FName RowStructName_DEPRECATED;

	/** The name of the RowStruct we were using when we were last saved */
	UPROPERTY()
	FTopLevelAssetPath RowStructPathName;

protected:
	/** When RowStruct is being modified, row data is stored serialized with tags */
	UPROPERTY(Transient)
	TArray<uint8> RowsSerializedWithTags;

	UPROPERTY(Transient)
	TSet<TObjectPtr<UObject>> TemporarilyReferencedObjects;
#endif	// WITH_EDITORONLY_DATA

private:
	/** A multicast delegate that is called any time the data table changes. */
	FOnDynamicDataTableChanged OnDynamicDataTableChangedDelegate;

public:
	/** Gets a multicast delegate that is called any time the data table changes. */
	FOnDynamicDataTableChanged& OnDynamicDataTableChanged() { return OnDynamicDataTableChangedDelegate; }

	/** 
	 * Call whenever the data of a table has changed, this calls the OnDynamicDataTableChanged() delegate and per-row callbacks.
	 * If ChangedRowName is not none, only that row was changed, otherwise assume all rows have changed
	 */
	DYNAMICDATATABLE_API void HandleDynamicDataTableChanged(FName ChangedRowName = NAME_None);
	
	/** Get all of the rows in the table, regardless of name */
	template <class T>
	void GetAllRows(const TCHAR* ContextString, OUT TArray<T*>& OutRowArray) const
	{
		if (RowStruct == nullptr)
		{
			UE_LOG(LogDynamicDataTable, Error, TEXT("UDynamicDataTable::GetAllRows : DynamicDataTable '%s' has no RowStruct specified (%s)."), *GetPathName(), ContextString);
		}
		else if (!RowStruct->IsChildOf(T::StaticStruct()))
		{
			UE_LOG(LogDynamicDataTable, Error, TEXT("UDynamicDataTable::GetAllRows : Incorrect type specified for DynamicDataTable '%s' (%s)."), *GetPathName(), ContextString);
		}
		else
		{
			OutRowArray.Reserve(OutRowArray.Num() + GetRowMap().Num());
			for (TMap<FName, uint8*>::TConstIterator RowMapIter(GetRowMap().CreateConstIterator()); RowMapIter; ++RowMapIter)
			{
				OutRowArray.Add(reinterpret_cast<T*>(RowMapIter.Value()));
			}
		}	
	}

	template <class T>
	void GetAllRows(const FString& ContextString, OUT TArray<T*>& OutRowArray) const
	{
		GetAllRows<T>(*ContextString, OutRowArray);
	}

	/** Function to find the row of a table given its name. */
	template <class T>
	T* FindRow(FName RowName, const TCHAR* ContextString, bool bWarnIfRowMissing = true) const
	{
		if(RowStruct == nullptr)
		{
			UE_LOG(LogDynamicDataTable, Error, TEXT("UDynamicDataTable::FindRow : '%s' specified no row for DynamicDataTable '%s'."), ContextString, *GetPathName());
			return nullptr;
		}

		if(!RowStruct->IsChildOf(T::StaticStruct()))
		{
			UE_CLOG(bWarnIfRowMissing, LogDynamicDataTable, Error, TEXT("UDynamicDataTable::FindRow : '%s' specified incorrect type for DynamicDataTable '%s'."), ContextString, *GetPathName());
			return nullptr;
		}

		if(RowName.IsNone())
		{
			UE_CLOG(bWarnIfRowMissing, LogDynamicDataTable, Warning, TEXT("UDynamicDataTable::FindRow : '%s' requested invalid row 'None' from DynamicDataTable '%s'."), ContextString, *GetPathName());
			return nullptr;
		}

		uint8* const* RowDataPtr = GetRowMap().Find(RowName);
		if (RowDataPtr == nullptr)
		{
			if (bWarnIfRowMissing)
			{
				UE_LOG(LogDynamicDataTable, Warning, TEXT("UDynamicDataTable::FindRow : '%s' requested row '%s' not in DynamicDataTable '%s'."), ContextString, *RowName.ToString(), *GetPathName());
			}
			return nullptr;
		}

		uint8* RowData = *RowDataPtr;
		check(RowData);

		return reinterpret_cast<T*>(RowData);
	}

	template <class T>
	T* FindRow(FName RowName, const FString& ContextString, bool bWarnIfRowMissing = true) const
	{
		return FindRow<T>(RowName, *ContextString, bWarnIfRowMissing);
	}

	/** Perform some operation for every row. */
	template <class T>
	void ForeachRow(const TCHAR* ContextString, TFunctionRef<void (const FName& Key, const T& Value)> Predicate) const
	{
		if (RowStruct == nullptr)
		{
			UE_LOG(LogDynamicDataTable, Error, TEXT("UDynamicDataTable::ForeachRow : DynamicDataTable '%s' has no RowStruct specified (%s)."), *GetPathName(), ContextString);
		}
		else if (!RowStruct->IsChildOf(T::StaticStruct()))
		{
			UE_LOG(LogDynamicDataTable, Error, TEXT("UDynamicDataTable::ForeachRow : Incorrect type specified for DynamicDataTable '%s' (%s)."), *GetPathName(), ContextString);
		}
		else
		{
			for (TMap<FName, uint8*>::TConstIterator RowMapIter(GetRowMap().CreateConstIterator()); RowMapIter; ++RowMapIter)
			{
				T* Entry = reinterpret_cast<T*>(RowMapIter.Value());
				Predicate(RowMapIter.Key(), *Entry);
			}
		}
	}

	template <class T>
	void ForeachRow(const FString& ContextString, TFunctionRef<void (const FName& Key, const T& Value)> Predicate) const
	{
		ForeachRow<T>(*ContextString, Predicate);
	}

	/** Returns the column property where PropertyName matches the name of the column property. Returns nullptr if no match is found or the match is not a supported table property */
	DYNAMICDATATABLE_API FProperty* FindTableProperty(const FName& PropertyName) const;

	/** High performance version with no type safety */
	uint8* FindRowUnchecked(FName RowName) const
	{
		if(RowStruct == nullptr)
		{
			return nullptr;
		}

		// If is RowName is none, it won't find anything in the map
		uint8* const* RowDataPtr = GetRowMap().Find(RowName);

		if(RowDataPtr == nullptr)
		{
			return nullptr;
		}

		return *RowDataPtr;
	}

	/** Empty the table info (will not clear RowStruct) */
	DYNAMICDATATABLE_API virtual void EmptyTable();

	DYNAMICDATATABLE_API TArray<FName> GetRowNames() const;

	/** Removes a single row from the DynamicDataTable by name. Just returns if row is not found. */
	DYNAMICDATATABLE_API virtual void RemoveRow(FName RowName);

	/** Copies RowData into table. That is: create Row if not found and copy data into the RowMap based on RowData. This is a "copy in" operation, so changing the passed in RowData after the fact does nothing. */
	DYNAMICDATATABLE_API virtual void AddRow(FName RowName, const FDynamicTableRowBase& RowData);

#if WITH_EDITOR
	DYNAMICDATATABLE_API virtual void CleanBeforeStructChange();
	DYNAMICDATATABLE_API virtual void RestoreAfterStructChange();

	/** Output entire contents of table as a string */
	DYNAMICDATATABLE_API FString GetTableAsString(const EDynamicDataTableExportFlags InDTExportFlags = EDynamicDataTableExportFlags::None) const;

	/** Output entire contents of table as CSV */
	DYNAMICDATATABLE_API FString GetTableAsCSV(const EDynamicDataTableExportFlags InDTExportFlags = EDynamicDataTableExportFlags::None) const;

	/** Output entire contents of table as JSON */
	DYNAMICDATATABLE_API FString GetTableAsJSON(const EDynamicDataTableExportFlags InDTExportFlags = EDynamicDataTableExportFlags::None) const;

	/** Output entire contents of table as JSON */
	template<typename CharType = TCHAR>
	DYNAMICDATATABLE_API bool WriteTableAsJSON(const TSharedRef< TJsonWriter<CharType, TPrettyJsonPrintPolicy<CharType> > >& JsonWriter, const EDynamicDataTableExportFlags InDTExportFlags = EDynamicDataTableExportFlags::None) const;

	/** Output entire contents of table as a JSON Object*/
	template<typename CharType = TCHAR>
	DYNAMICDATATABLE_API bool WriteTableAsJSONObject(const TSharedRef< TJsonWriter<CharType, TPrettyJsonPrintPolicy<CharType> > >& JsonWriter, const EDynamicDataTableExportFlags InDTExportFlags = EDynamicDataTableExportFlags::None) const;

	/** Output the fields from a particular row (use RowMap to get RowData) to an existing JsonWriter */
	template<typename CharType = TCHAR>
	DYNAMICDATATABLE_API bool WriteRowAsJSON(const TSharedRef< TJsonWriter<CharType, TPrettyJsonPrintPolicy<CharType> > >& JsonWriter, const void* RowData, const EDynamicDataTableExportFlags InDTExportFlags = EDynamicDataTableExportFlags::None) const;

	/** Copies all the import options from another table, this does not copy row dawta */
	DYNAMICDATATABLE_API bool CopyImportOptions(UDynamicDataTable* SourceTable);
#endif
	/** 
	 *	Create table from CSV style comma-separated string. 
	 *	RowStruct must be defined before calling this function. 
	 *	@return	Set of problems encountered while processing input
	 */
	DYNAMICDATATABLE_API TArray<FString> CreateTableFromCSVString(const FString& InString);

	/** 
	*	Create table from JSON style string. 
	*	RowStruct must be defined before calling this function. 
	*	@return	Set of problems encountered while processing input
	*/
	DYNAMICDATATABLE_API TArray<FString> CreateTableFromJSONString(const FString& InString);

	/** Get array of UProperties that corresponds to columns in the table */
	TArray<FProperty*> GetTablePropertyArray(const TArray<const TCHAR*>& Cells, UStruct* RowStruct, TArray<FString>& OutProblems, int32 KeyColumn = 0);
	
	/** 
	 *	Create table from another Data Table
	 *	@return	Set of problems encountered while processing input
	 */
	DYNAMICDATATABLE_API TArray<FString> CreateTableFromOtherTable(const UDynamicDataTable* InTable);

	/**
	 *	Create table from a raw data map with a given script struct
	 *	@return	Set of problems encountered while processing input
	 */
	DYNAMICDATATABLE_API TArray<FString> CreateTableFromRawData(TMap<FName, const uint8*>& DataMap, UScriptStruct* InRowStruct);

	DYNAMICDATATABLE_API void RefreshDynTable();

#if WITH_EDITOR
	/** Get an array of all the column titles, using the friendly display name from the property */
	DYNAMICDATATABLE_API TArray<FString> GetColumnTitles() const;

	/** Get an array of all the column titles, using the unique name from the property */
	DYNAMICDATATABLE_API TArray<FString> GetUniqueColumnTitles() const;

	/** Get array for each row in the table. The first row is the titles*/
	DYNAMICDATATABLE_API TArray< TArray<FString> > GetTableData(const EDynamicDataTableExportFlags InDTExportFlags = EDynamicDataTableExportFlags::None) const;
#endif //WITH_EDITOR

	//~ End UDynamicDataTable Interface

protected:
	void SaveStructData(FStructuredArchiveSlot Slot);
	void LoadStructData(FStructuredArchiveSlot Slot);

	/**
	 * Called whenever new data is imported into the data table via CreateTableFrom*; Alerts each imported row and gives the
	 * row struct a chance to operate on the imported data
	 * 
	 * @param OutCollectedImportProblems	[OUT] Array of strings of import problems
	 */
	void OnPostDataImported(OUT TArray<FString>& OutCollectedImportProblems);

	UScriptStruct& GetEmptyUsingStruct() const;

	/** Used to trigger the data table changed delegate. This allows us to trigger the delegate only once from more complex changes */
	struct FScopedDynamicDataTableChange
	{
		FScopedDynamicDataTableChange(UDynamicDataTable* InTable);
		~FScopedDynamicDataTableChange();

	private:
		UDynamicDataTable* Table;

		static TMap<UDynamicDataTable*, int32> ScopeCount;
		static FCriticalSection CriticalSection;
	};
};


/** Handle to a particular row in a table*/
USTRUCT(BlueprintType)
struct DYNAMICDATATABLE_API FDynamicDataTableRowHandle
{
	GENERATED_USTRUCT_BODY()

	FDynamicDataTableRowHandle()
		: DynamicDataTable(nullptr)
		, RowName(NAME_None)
	{

	}

	/** Pointer to table we want a row from */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=DynamicDataTableRowHandle)
	TObjectPtr<const UDynamicDataTable>	DynamicDataTable;

	/** Name of row in the table that we want */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=DynamicDataTableRowHandle)
	FName				RowName;

	/** Returns true if this handle is specifically pointing to nothing */
	bool IsNull() const
	{
		return DynamicDataTable == nullptr && RowName.IsNone();
	}

	/** Get the row straight from the row handle */
	template <class T>
	T* GetRow(const TCHAR* ContextString) const
	{
		if(DynamicDataTable == nullptr)
		{
			if (RowName != NAME_None)
			{
				UE_LOG(LogDynamicDataTable, Warning, TEXT("FDynamicDataTableRowHandle::GetRow : No DynamicDataTable for row %s (%s)."), *RowName.ToString(), ContextString);
			}
			return nullptr;
		}

		return DynamicDataTable->FindRow<T>(RowName, ContextString);
	}

	template <class T>
	T* GetRow(const FString& ContextString) const
	{
		return GetRow<T>(*ContextString);
	}

	FString ToDebugString(bool bUseFullPath = false) const
	{
		if (DynamicDataTable == nullptr)
		{
			return FString::Printf(TEXT("No Data Table Specified, Row: %s"), *RowName.ToString());
		}

		return FString::Printf(TEXT("Table: %s, Row: %s"), bUseFullPath ? *DynamicDataTable->GetPathName() : *DynamicDataTable->GetName(), *RowName.ToString());
	}

	bool operator==(FDynamicDataTableRowHandle const& Other) const;
	bool operator!=(FDynamicDataTableRowHandle const& Other) const;
	void PostSerialize(const FArchive& Ar);
};

template<>
struct TStructOpsTypeTraits< FDynamicDataTableRowHandle > : public TStructOpsTypeTraitsBase2< FDynamicDataTableRowHandle >
{
	enum
	{
		WithPostSerialize = true,
	};
};

/** Handle to a particular set of rows in a table */
USTRUCT(BlueprintType)
struct DYNAMICDATATABLE_API FDynamicDataTableCategoryHandle
{
	GENERATED_USTRUCT_BODY()

	/** Pointer to table we want a row from */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=DynamicDataTableCategoryHandle)
	TObjectPtr<const class UDynamicDataTable>	DynamicDataTable = nullptr;

	/** Name of column in the table that we want */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=DynamicDataTableCategoryHandle)
	FName				ColumnName;

	/** Contents of rows in the table that we want */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=DynamicDataTableCategoryHandle)
	FName				RowContents;

	/** Returns true if this handle is specifically pointing to nothing */
	bool IsNull() const
	{
		return DynamicDataTable == nullptr && ColumnName == NAME_None && RowContents == NAME_None;
	}

	/** Searches DynamicDataTable for all rows that contain entries with RowContents in the column named ColumnName and returns them. */
	template <class T>
	void GetRows(TArray<T*>& OutRows, const FString& ContextString) const
	{
		OutRows.Empty();
		if (DynamicDataTable == nullptr)
		{
			if (RowContents != NAME_None)
			{
				UE_LOG(LogDynamicDataTable, Warning, TEXT("FDynamicDataTableCategoryHandle::GetRows : No DynamicDataTable for row %s (%s)."), *RowContents.ToString(), *ContextString);
			}

			return;
		}

		if (ColumnName == NAME_None)
		{
			if (RowContents != NAME_None)
			{
				UE_LOG(LogDynamicDataTable, Warning, TEXT("FDynamicDataTableCategoryHandle::GetRows : No Column selected for row %s (%s)."), *RowContents.ToString(), *ContextString);
			}

			return;
		}

		// Find the property that matches the desired column (ColumnName)
		FProperty* Property = DynamicDataTable->FindTableProperty(ColumnName);
		if (Property == nullptr)
		{
			return;
		}

		// check each row to see if the value in the Property element is the one we're looking for (RowContents). If it is, add the row to OutRows
		uint8* RowContentsAsBinary = (uint8*)FMemory_Alloca(Property->GetSize());
		Property->InitializeValue(RowContentsAsBinary);
		if (Property->ImportText_Direct(*RowContents.ToString(), RowContentsAsBinary, nullptr, PPF_None) == nullptr)
		{
			Property->DestroyValue(RowContentsAsBinary);
			return;
		}

		for (auto RowIt = DynamicDataTable->GetRowMap().CreateConstIterator(); RowIt; ++RowIt)
		{
			uint8* RowData = RowIt.Value();

			if (Property->Identical(Property->ContainerPtrToValuePtr<void>(RowData, 0), RowContentsAsBinary, PPF_None))
			{
				OutRows.Add((T*)RowData);
			}
		}
		Property->DestroyValue(RowContentsAsBinary);

		return;
	}

	bool operator==(FDynamicDataTableCategoryHandle const& Other) const;
	bool operator!=(FDynamicDataTableCategoryHandle const& Other) const;
};


/** Macro to call GetRow with a correct error info. Assumed to be called from within a UObject */
#define GETROW_REPORTERROR(Handle, Template) Handle.GetRow<Template>(FString::Printf(TEXT("%s.%s"), *GetPathName(), TEXT(#Handle)))
#define GETROWOBJECT_REPORTERROR(Object, Handle, Template) Handle.GetRow<Template>(FString::Printf(TEXT("%s.%s"), *Object->GetPathName(), TEXT(#Handle)))
