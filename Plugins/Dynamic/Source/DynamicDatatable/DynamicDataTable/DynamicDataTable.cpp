// Copyright Epic Games, Inc. All Rights Reserved.

#include "DynamicDataTable.h"
#include "AssetRegistry/AssetData.h"
#include "Internationalization/TextPackageNamespaceUtil.h"
#include "Internationalization/StabilizeLocalizationKeys.h"
#include "Serialization/PropertyLocalizationDataGathering.h"
#include "Serialization/ObjectWriter.h"
#include "Serialization/ObjectReader.h"
#include "Serialization/StructuredArchive.h"
#include "UObject/LinkerLoad.h"
#include "DynamicDataTableCSV.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "DynamicDataTableJSON.h"
#include "EditorFramework/AssetImportData.h"
#include "StructUtils/UserDefinedStruct.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(DynamicDataTable)

namespace
{
#if WITH_EDITORONLY_DATA
	void GatherDynamicDataTableForLocalization(const UObject* const Object, FPropertyLocalizationDataGatherer& PropertyLocalizationDataGatherer, const EPropertyLocalizationGathererTextFlags GatherTextFlags)
	{
		const UDynamicDataTable* const DynamicDataTable = CastChecked<UDynamicDataTable>(Object);

		PropertyLocalizationDataGatherer.GatherLocalizationDataFromObject(DynamicDataTable, GatherTextFlags);

		const FString PathToObject = DynamicDataTable->GetPathName();
		for (const auto& Pair : DynamicDataTable->GetRowMap())
		{
			const FString PathToRow = PathToObject + TEXT(".") + Pair.Key.ToString();
			PropertyLocalizationDataGatherer.GatherLocalizationDataFromStructWithCallbacks(PathToRow, DynamicDataTable->RowStruct, Pair.Value, nullptr, GatherTextFlags);
		}
	}
#endif // WITH_EDITORONLY_DATA
}

UDynamicDataTable::FScopedDynamicDataTableChange::FScopedDynamicDataTableChange(UDynamicDataTable* InTable)
	: Table(InTable)
{
	FScopeLock Lock(&CriticalSection);
	int32& Count = ScopeCount.FindOrAdd(Table);
	++Count;
}

UDynamicDataTable::FScopedDynamicDataTableChange::~FScopedDynamicDataTableChange()
{
	FScopeLock Lock(&CriticalSection);
	int32& Count = ScopeCount.FindChecked(Table);
	--Count;
	if (Count == 0)
	{
		Table->HandleDynamicDataTableChanged();
		ScopeCount.Remove(Table);
	}
}

TMap<UDynamicDataTable*, int32> UDynamicDataTable::FScopedDynamicDataTableChange::ScopeCount;
FCriticalSection UDynamicDataTable::FScopedDynamicDataTableChange::CriticalSection;

#define DynamicDataTable_CHANGE_SCOPE()	UDynamicDataTable::FScopedDynamicDataTableChange ActiveScope(this);

UDynamicDataTable::UDynamicDataTable(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bIgnoreExtraFields = false;
	bIgnoreMissingFields = false;
	bStripFromClientBuilds = false;

#if WITH_EDITORONLY_DATA
	{ static const FAutoRegisterLocalizationDataGatheringCallback AutomaticRegistrationOfLocalizationGatherer(UDynamicDataTable::StaticClass(), &GatherDynamicDataTableForLocalization); }
#endif
}

#if WITH_EDITOR
void UDynamicDataTable::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

#if WITH_EDITORONLY_DATA
	HandleDynamicDataTableChanged();
#endif
}
#endif

void UDynamicDataTable::LoadStructData(FStructuredArchiveSlot Slot)
{
	UScriptStruct* LoadUsingStruct = RowStruct;
	if (!LoadUsingStruct)
	{
		if (!HasAnyFlags(RF_ClassDefaultObject) && GetOutermost() != GetTransientPackage())
		{
			UE_LOG(LogDynamicDataTable, Error, TEXT("Missing RowStruct while loading DynamicDataTable '%s'!"), *GetPathName());
		}
		LoadUsingStruct = FDynamicTableRowBase::StaticStruct();
	}

	int32 NumRows;
	FStructuredArchiveArray Array = Slot.EnterArray(NumRows);

	DynamicDataTable_CHANGE_SCOPE();

	RowMap.Reserve(NumRows);
	for (int32 RowIdx = 0; RowIdx < NumRows; RowIdx++)
	{
		FStructuredArchiveRecord RowRecord = Array.EnterElement().EnterRecord();

		// Load row name
		FName RowName;
		RowRecord << SA_VALUE(TEXT("Name"), RowName);

		// Load row data
		uint8* RowData = (uint8*)FMemory::Malloc(LoadUsingStruct->GetStructureSize());

		// And be sure to call DestroyScriptStruct later
		LoadUsingStruct->InitializeStruct(RowData);

		LoadUsingStruct->SerializeItem(RowRecord.EnterField(TEXT("Value")), RowData, nullptr);

		// Add to map
		RowMap.Add(RowName, RowData);
	}
	RefreshDynTable();
}

void UDynamicDataTable::SaveStructData(FStructuredArchiveSlot Slot)
{
	UScriptStruct* SaveUsingStruct = RowStruct;
	if (!SaveUsingStruct)
	{
		if (!HasAnyFlags(RF_ClassDefaultObject) && GetOutermost() != GetTransientPackage())
		{
			UE_LOG(LogDynamicDataTable, Error, TEXT("Missing RowStruct while saving DynamicDataTable '%s'!"), *GetPathName());
		}
		SaveUsingStruct = FDynamicTableRowBase::StaticStruct();
	}

	int32 NumRows = RowMap.Num();
	FStructuredArchiveArray Array = Slot.EnterArray(NumRows);

	// Now iterate over rows in the map
	for (auto RowIt = RowMap.CreateIterator(); RowIt; ++RowIt)
	{
		// Save out name
		FName RowName = RowIt.Key();
		FStructuredArchiveRecord Row = Array.EnterElement().EnterRecord();
		Row << SA_VALUE(TEXT("Name"), RowName);

		// Save out data
		uint8* RowData = RowIt.Value();

		SaveUsingStruct->SerializeItem(Row.EnterField(TEXT("Value")), RowData, nullptr);
	}
}

void UDynamicDataTable::GetPreloadDependencies(TArray<UObject*>& OutDeps)
{
	Super::GetPreloadDependencies(OutDeps);
	OutDeps.Add(RowStruct);
}

void UDynamicDataTable::OnPostDataImported(TArray<FString>& OutCollectedImportProblems)
{
	if (RowStruct)
	{
		const bool bIsNativeRowStruct = RowStruct->IsChildOf(FDynamicTableRowBase::StaticStruct());

		FString DynamicDataTableTextNamespace = GetName();
#if USE_STABLE_LOCALIZATION_KEYS
		if (GIsEditor)
	{
			DynamicDataTableTextNamespace = TextNamespaceUtil::BuildFullNamespace(DynamicDataTableTextNamespace, TextNamespaceUtil::EnsurePackageNamespace(this), /*bAlwaysApplyPackageNamespace*/true);
		}
#endif

		for (const TPair<FName, uint8*>& TableRowPair : RowMap)
		{
			if (bIsNativeRowStruct)
			{
				FDynamicTableRowBase* CurRow = reinterpret_cast<FDynamicTableRowBase*>(TableRowPair.Value);
				CurRow->OnPostDataImport(this, TableRowPair.Key, OutCollectedImportProblems);
			}

#if WITH_EDITOR
			// Perform automatic fix-up on any text properties that have been imported from a raw string to assign them deterministic keys
			// We do this after OnPostDataImport has been run on the row, as that function may perform custom fix-up logic that will fix the keys differently than the default logic
			StabilizeLocalizationKeys::StabilizeLocalizationKeysForStruct(RowStruct, TableRowPair.Value, DynamicDataTableTextNamespace, TableRowPair.Key.ToString());
#endif
		}
	}
	RefreshDynTable();
	// Don't need to call HandleDynamicDataTableChanged because it gets called by the scope and post edit callbacks
	// If you need to handle an import-specific problem, register with FDynamicDataTableEditorUtils
}

void UDynamicDataTable::HandleDynamicDataTableChanged(FName ChangedRowName)
{
	if (!IsValidChecked(this) || IsUnreachable() || HasAnyFlags(RF_BeginDestroyed))
	{
		// This gets called during destruction, don't broadcast callbacks
		return;
	}

	// Do the row fixup before global callback
	if (RowStruct)
	{
		const bool bIsNativeRowStruct = RowStruct->IsChildOf(FDynamicTableRowBase::StaticStruct());

		if (bIsNativeRowStruct)
		{
			for (const TPair<FName, uint8*>& TableRowPair : RowMap)
			{
				if (ChangedRowName != NAME_None && ChangedRowName != TableRowPair.Key)
				{
					continue;
				}

				FDynamicTableRowBase* CurRow = reinterpret_cast<FDynamicTableRowBase*>(TableRowPair.Value);
				CurRow->OnDynamicDataTableChanged(this, TableRowPair.Key);
			}
		}
	}
	RefreshDynTable();
	OnDynamicDataTableChanged().Broadcast();
}

void UDynamicDataTable::Serialize(FStructuredArchiveRecord Record)
{
	FArchive& BaseArchive = Record.GetUnderlyingArchive();

#if WITH_EDITORONLY_DATA
	// Make sure and update RowStructName before calling the parent Serialize (which will save the properties)
	if (BaseArchive.IsSaving() && RowStruct)
	{
		RowStructName_DEPRECATED = RowStruct->GetFName();
		RowStructPathName = RowStruct->GetStructPathName();
	}
#endif	// WITH_EDITORONLY_DATA

	Super::Serialize(Record); // When loading, this should load our RowStruct!	

	if (RowStruct && RowStruct->HasAnyFlags(RF_NeedLoad))
	{
		auto RowStructLinker = RowStruct->GetLinker();
		if (RowStructLinker)
		{
			RowStructLinker->Preload(RowStruct);
		}
	}

	if(BaseArchive.IsLoading())
	{
		DynamicDataTable_CHANGE_SCOPE();
		EmptyTable();
		LoadStructData(Record.EnterField(TEXT("Data")));
	}
	else if(BaseArchive.IsSaving())
	{
		SaveStructData(Record.EnterField(TEXT("Data")));
	}
}

void UDynamicDataTable::AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector)
{	
	UDynamicDataTable* This = CastChecked<UDynamicDataTable>(InThis);

	// Need to emit references for referenced rows (unless there's no properties that reference UObjects)
	if(This->RowStruct != nullptr && This->RowStruct->RefLink != nullptr)
	{
		// Now iterate over rows in the map
		for ( auto RowIt = This->RowMap.CreateIterator(); RowIt; ++RowIt )
		{
			uint8* RowData = RowIt.Value();

			if (RowData)
			{
				FVerySlowReferenceCollectorArchiveScope CollectorScope(Collector.GetVerySlowReferenceCollectorArchive(), This);
				// Serialize all of the properties to make sure they get in the collector
				This->RowStruct->SerializeBin(CollectorScope.GetArchive(), RowData);
			}
		}
	}

	Super::AddReferencedObjects( This, Collector );
}

void UDynamicDataTable::GetResourceSizeEx(FResourceSizeEx& CumulativeResourceSize)
{
	Super::GetResourceSizeEx(CumulativeResourceSize);

	CumulativeResourceSize.AddDedicatedSystemMemoryBytes(RowMap.GetAllocatedSize());
	if (RowStruct)
	{
		CumulativeResourceSize.AddDedicatedSystemMemoryBytes(RowMap.Num() * RowStruct->GetStructureSize());
	}
}

void UDynamicDataTable::FinishDestroy()
{
	Super::FinishDestroy();
	if(!IsTemplate())
	{
		EmptyTable(); // Free memory when UObject goes away
	}
}

#if WITH_EDITORONLY_DATA
FName UDynamicDataTable::GetRowStructName() const
{
	return (RowStruct) ? RowStruct->GetFName() : RowStructName_DEPRECATED;
}

FTopLevelAssetPath UDynamicDataTable::GetRowStructPathName() const
{
	return (RowStruct) ? RowStruct->GetStructPathName() : RowStructPathName;
}

void UDynamicDataTable::GetAssetRegistryTags(TArray<FAssetRegistryTag>& OutTags) const
{
	PRAGMA_DISABLE_DEPRECATION_WARNINGS;
	Super::GetAssetRegistryTags(OutTags);
	PRAGMA_ENABLE_DEPRECATION_WARNINGS;
}

void UDynamicDataTable::GetAssetRegistryTags(FAssetRegistryTagsContext Context) const
{
	if (AssetImportData)
	{
		Context.AddTag( FAssetRegistryTag(SourceFileTagName(), AssetImportData->GetSourceData().ToJson(), FAssetRegistryTag::TT_Hidden) );
	}

	// Add the row structure tag
	{
		static const FName RowStructureTag = "RowStructure";
		Context.AddTag( FAssetRegistryTag(RowStructureTag, GetRowStructPathName().ToString(), FAssetRegistryTag::TT_Alphabetical) );
	}

	Super::GetAssetRegistryTags(Context);
}

void UDynamicDataTable::PostInitProperties()
{
	if (!HasAnyFlags(RF_ClassDefaultObject))
	{
		AssetImportData = NewObject<UAssetImportData>(this, TEXT("AssetImportData"));
	}

	Super::PostInitProperties();
}

void UDynamicDataTable::PostLoad()
{
	Super::PostLoad();
	if (!ImportPath_DEPRECATED.IsEmpty() && AssetImportData)
	{
		FAssetImportInfo Info;
		Info.Insert(FAssetImportInfo::FSourceFile(ImportPath_DEPRECATED));
		AssetImportData->SourceData = MoveTemp(Info);
	}
	if (!RowStructName_DEPRECATED.IsNone())
	{
		UStruct* SavedRowStruct = RowStruct;
		if (!SavedRowStruct)
		{
			SavedRowStruct = FindFirstObjectSafe<UStruct>(*RowStructName_DEPRECATED.ToString());
		}
		if (SavedRowStruct)
		{
			RowStructPathName = SavedRowStruct->GetStructPathName();
		}
		else
		{
			UE_LOG(LogDynamicDataTable, Error, TEXT("Unable to resolved RowStruct PathName from serialized short name '%s'!"), *RowStructName_DEPRECATED.ToString());
		}
	}
	RefreshDynTable();
}
#endif // WITH_EDITORONLY_DATA

#if WITH_EDITOR
/*void UDynamicDataTable::PostLoadAssetRegistryTags(const FAssetData& InAssetData, TArray<FAssetRegistryTag>& OutTagsAndValuesToUpdate) const
{
	Super::PostLoadAssetRegistryTags(InAssetData, OutTagsAndValuesToUpdate);

	static const FName RowStructureTag(TEXT("RowStructure"));
	FString TagValue = InAssetData.GetTagValueRef<FString>(RowStructureTag);
	if (!TagValue.IsEmpty() && FPackageName::IsShortPackageName(TagValue))
	{
		FTopLevelAssetPath PathName = UClass::TryConvertShortTypeNameToPathName<UField>(TagValue, ELogVerbosity::Warning, TEXT("UDynamicDataTable::PostLoadAssetRegistryTags"));
		if (!PathName.IsNull())
		{
			OutTagsAndValuesToUpdate.Add(FAssetRegistryTag(RowStructureTag, PathName.ToString(), FAssetRegistryTag::TT_Alphabetical));
		}
	}
}*/
#endif // WITH_EDITOR

UScriptStruct& UDynamicDataTable::GetEmptyUsingStruct() const
{
	UScriptStruct* EmptyUsingStruct = RowStruct;
	if (!EmptyUsingStruct)
	{
		if (!HasAnyFlags(RF_ClassDefaultObject) && GetOutermost() != GetTransientPackage())
		{
			UE_LOG(LogDynamicDataTable, Error, TEXT("Missing RowStruct while emptying DynamicDataTable '%s'!"), *GetPathName());
		}
		EmptyUsingStruct = FDynamicTableRowBase::StaticStruct();
	}

	return *EmptyUsingStruct;
}

void UDynamicDataTable::EmptyTable()
{
	DynamicDataTable_CHANGE_SCOPE();

	UScriptStruct& EmptyUsingStruct = GetEmptyUsingStruct();

	// Iterate over all rows in table and free mem
	for (auto RowIt = RowMap.CreateIterator(); RowIt; ++RowIt)
	{
		uint8* RowData = RowIt.Value();
		EmptyUsingStruct.DestroyStruct(RowData);
		FMemory::Free(RowData);
	}

	// Finally empty the map
	RowMap.Empty();
}

void UDynamicDataTable::RemoveRow(FName RowName)
{
	DynamicDataTable_CHANGE_SCOPE();

	RemoveRowInternal(RowName);
}

void UDynamicDataTable::RemoveRowInternal(FName RowName)
{
	UScriptStruct& EmptyUsingStruct = GetEmptyUsingStruct();

	uint8* RowData = nullptr;
	RowMap.RemoveAndCopyValue(RowName, RowData);
		
	if (RowData)
	{
		EmptyUsingStruct.DestroyStruct(RowData);
		FMemory::Free(RowData);
	}
}

void UDynamicDataTable::AddRow(FName RowName, const FDynamicTableRowBase& RowData)
{
	DynamicDataTable_CHANGE_SCOPE();

	UScriptStruct& EmptyUsingStruct = GetEmptyUsingStruct();

	// We want to delete the row memory even for child classes that override remove
	RemoveRowInternal(RowName);
		
	uint8* NewRawRowData = (uint8*)FMemory::Malloc(EmptyUsingStruct.GetStructureSize());
	
	EmptyUsingStruct.InitializeStruct(NewRawRowData);
	EmptyUsingStruct.CopyScriptStruct(NewRawRowData, &RowData);

	// Add to map
	AddRowInternal(RowName, NewRawRowData);
}

void UDynamicDataTable::AddRowInternal(FName RowName, uint8* RowData)
{
	RowMap.Add(RowName, RowData);
}

/** Returns the column property where PropertyName matches the name of the column property. Returns NULL if no match is found or the match is not a supported table property */
FProperty* UDynamicDataTable::FindTableProperty(const FName& PropertyName) const
{
	FProperty* Property = nullptr;

	if (RowStruct)
	{
		Property = RowStruct->FindPropertyByName(PropertyName);
		if (Property == nullptr && RowStruct->IsA<UUserDefinedStruct>())
		{
			const FString PropertyNameStr = PropertyName.ToString();

			for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
			{
				if (PropertyNameStr == RowStruct->GetAuthoredNameForField(*It))
				{
					Property = *It;
					break;
				}
			}
		}
		if (!DynamicDataTableUtils::IsSupportedTableProperty(Property))
		{
			Property = nullptr;
		}
	}

	return Property;
}

#if WITH_EDITOR

void UDynamicDataTable::CleanBeforeStructChange()
{
	if (RowsSerializedWithTags.Num() > 0)
	{
		// This is part of an undo, so restore that value instead of calculating a new one
		EmptyTable();
	}
	else
	{
		RowsSerializedWithTags.Reset();
		TemporarilyReferencedObjects.Empty();
		{
			class FRawStructWriter : public FObjectWriter
			{
				TSet<TObjectPtr<UObject>>& TemporarilyReferencedObjects;
			public:
				FRawStructWriter(TArray<uint8>& InBytes, TSet<TObjectPtr<UObject>>& InTemporarilyReferencedObjects)
					: FObjectWriter(InBytes), TemporarilyReferencedObjects(InTemporarilyReferencedObjects) {}
				virtual FArchive& operator<<(class UObject*& Res) override
				{
					FObjectWriter::operator<<(Res);
					TemporarilyReferencedObjects.Add(Res);
					return *this;
				}
			};

			FRawStructWriter MemoryWriter(RowsSerializedWithTags, TemporarilyReferencedObjects);
			SaveStructData(FStructuredArchiveFromArchive(MemoryWriter).GetSlot());
		}

		EmptyTable();
		Modify();
	}
}

void UDynamicDataTable::RestoreAfterStructChange()
{
	DynamicDataTable_CHANGE_SCOPE();

	EmptyTable();
	{
		class FRawStructReader : public FObjectReader
		{
		public:
			FRawStructReader(TArray<uint8>& InBytes) : FObjectReader(InBytes) {}
			virtual FArchive& operator<<(class UObject*& Res) override
			{
				UObject* Object = nullptr;
				FObjectReader::operator<<(Object);
				FWeakObjectPtr WeakObjectPtr = Object;
				Res = WeakObjectPtr.Get();
				return *this;
			}
		};

		FRawStructReader MemoryReader(RowsSerializedWithTags);
		LoadStructData(FStructuredArchiveFromArchive(MemoryReader).GetSlot());
	}
	TemporarilyReferencedObjects.Empty();
	RowsSerializedWithTags.Empty();
}

FString UDynamicDataTable::GetTableAsString(const EDynamicDataTableExportFlags InDTExportFlags) const
{
	FString Result;

	if (RowStruct != nullptr)
	{
		Result += FString::Printf(TEXT("Using RowStruct: %s\n\n"), *RowStruct->GetPathName());

		// First build array of properties
		TArray<FProperty*> StructProps;
		for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
		{
			FProperty* Prop = *It;
			check(Prop != nullptr);
			StructProps.Add(Prop);
		}

		// First row, column titles, taken from properties
		Result += TEXT("---");
		for (int32 PropIdx=0; PropIdx<StructProps.Num(); PropIdx++)
		{
			Result += TEXT(",");
			Result += StructProps[PropIdx]->GetName();
		}
		Result += TEXT("\n");

		// Now iterate over rows
		for ( auto RowIt = RowMap.CreateConstIterator(); RowIt; ++RowIt )
		{
			FName RowName = RowIt.Key();
			Result += RowName.ToString();

			uint8* RowData = RowIt.Value();
			for(int32 PropIdx=0; PropIdx<StructProps.Num(); PropIdx++)
			{
				Result += TEXT(",");
				Result += DynamicDataTableUtils::GetPropertyValueAsString(StructProps[PropIdx], RowData, InDTExportFlags);
			}
			Result += TEXT("\n");			
		}
	}
	else
	{
		Result += FString(TEXT("Missing RowStruct!\n"));
	}
	return Result;
}

FString UDynamicDataTable::GetTableAsCSV(const EDynamicDataTableExportFlags InDTExportFlags) const
{
	FString Result;
	if (!FDynamicDataTableExporterCSV(InDTExportFlags, Result).WriteTable(*this))
	{
		Result = TEXT("Missing RowStruct!\n");
	}
	return Result;
}

FString UDynamicDataTable::GetTableAsJSON(const EDynamicDataTableExportFlags InDTExportFlags) const
{
	FString Result;
	if (!FDynamicDataTableExporterJSON(InDTExportFlags, Result).WriteTable(*this))
	{
		Result = TEXT("Missing RowStruct!\n");
	}
	return Result;
}

template<typename CharType>
bool UDynamicDataTable::WriteRowAsJSON(const TSharedRef< TJsonWriter<CharType, TPrettyJsonPrintPolicy<CharType> > >& JsonWriter, const void* RowData, const EDynamicDataTableExportFlags InDTExportFlags) const
{
	return TDynamicDataTableExporterJSON<CharType>(InDTExportFlags, JsonWriter).WriteRow(RowStruct, RowData);
}

template DYNAMICDATATABLE_API bool UDynamicDataTable::WriteRowAsJSON<TCHAR>(const TSharedRef< TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR> > >& JsonWriter, const void* RowData, const EDynamicDataTableExportFlags InDTExportFlags) const;
template DYNAMICDATATABLE_API bool UDynamicDataTable::WriteRowAsJSON<ANSICHAR>(const TSharedRef< TJsonWriter<ANSICHAR, TPrettyJsonPrintPolicy<ANSICHAR> > >& JsonWriter, const void* RowData, const EDynamicDataTableExportFlags InDTExportFlags) const;

bool UDynamicDataTable::CopyImportOptions(UDynamicDataTable* SourceTable)
{
	// Only safe to call on an empty table
	if (!SourceTable || !ensure(RowMap.Num() == 0))
	{
		return false;
	}

	bStripFromClientBuilds = SourceTable->bStripFromClientBuilds;
	bIgnoreExtraFields = SourceTable->bIgnoreExtraFields;
	bIgnoreMissingFields = SourceTable->bIgnoreMissingFields;
	ImportKeyField = SourceTable->ImportKeyField;
	RowStruct = SourceTable->RowStruct;

	if (RowStruct)
	{
		RowStructName_DEPRECATED = RowStruct->GetFName();
		RowStructPathName = RowStruct->GetStructPathName();
	}

	if (SourceTable->AssetImportData)
	{
		AssetImportData->SourceData = SourceTable->AssetImportData->SourceData;
	}

	return true;
}

template<typename CharType>
bool UDynamicDataTable::WriteTableAsJSON(const TSharedRef< TJsonWriter<CharType, TPrettyJsonPrintPolicy<CharType> > >& JsonWriter, const EDynamicDataTableExportFlags InDTExportFlags) const
{
	return TDynamicDataTableExporterJSON<CharType>(InDTExportFlags, JsonWriter).WriteTable(*this);
}

template DYNAMICDATATABLE_API bool UDynamicDataTable::WriteTableAsJSON<TCHAR>(const TSharedRef< TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR> > >& JsonWriter, const EDynamicDataTableExportFlags InDTExportFlags) const;
template DYNAMICDATATABLE_API bool UDynamicDataTable::WriteTableAsJSON<ANSICHAR>(const TSharedRef< TJsonWriter<ANSICHAR, TPrettyJsonPrintPolicy<ANSICHAR> > >& JsonWriter, const EDynamicDataTableExportFlags InDTExportFlags) const;

template<typename CharType>
bool UDynamicDataTable::WriteTableAsJSONObject(const TSharedRef< TJsonWriter<CharType, TPrettyJsonPrintPolicy<CharType> > >& JsonWriter, const EDynamicDataTableExportFlags InDTExportFlags) const
{
	return TDynamicDataTableExporterJSON<CharType>(InDTExportFlags, JsonWriter).WriteTableAsObject(*this);
}

template DYNAMICDATATABLE_API bool UDynamicDataTable::WriteTableAsJSONObject<TCHAR>(const TSharedRef< TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR> > >& JsonWriter, const EDynamicDataTableExportFlags InDTExportFlags) const;
template DYNAMICDATATABLE_API bool UDynamicDataTable::WriteTableAsJSONObject<ANSICHAR>(const TSharedRef< TJsonWriter<ANSICHAR, TPrettyJsonPrintPolicy<ANSICHAR> > >& JsonWriter, const EDynamicDataTableExportFlags InDTExportFlags) const;
#endif

TArray<FProperty*> UDynamicDataTable::GetTablePropertyArray(const TArray<const TCHAR*>& Cells, UStruct* InRowStruct, TArray<FString>& OutProblems, int32 KeyColumn)
{
	TArray<FProperty*> ColumnProps;

	// Get list of all expected properties from the struct
	TArray<FName> ExpectedPropNames = DynamicDataTableUtils::GetStructPropertyNames(InRowStruct);

	// Need at least 2 columns, first column will contain row names
	if(Cells.Num() > 1)
	{
		ColumnProps.AddZeroed( Cells.Num() );

		// Skip first column depending on option
		TArray<FString> TempPropertyImportNames;
		for (int32 ColIdx = 0; ColIdx < Cells.Num(); ++ColIdx)
		{
			if (ColIdx == KeyColumn)
			{
				continue;
			}

			const TCHAR* ColumnValue = Cells[ColIdx];

			FName PropName = DynamicDataTableUtils::MakeValidName(ColumnValue);
			if(PropName == NAME_None)
			{
				OutProblems.Add(FString::Printf(TEXT("Missing name for column %d."), ColIdx));
			}
			else
			{
				FProperty* ColumnProp = FindFProperty<FProperty>(InRowStruct, PropName);

				for (TFieldIterator<FProperty> It(InRowStruct); It && !ColumnProp; ++It)
				{
					DynamicDataTableUtils::GetPropertyImportNames(*It, TempPropertyImportNames);
					ColumnProp = TempPropertyImportNames.Contains(ColumnValue) ? *It : nullptr;
				}

				// Didn't find a property with this name, problem..
				if(ColumnProp == nullptr)
				{
					if (!bIgnoreExtraFields)
					{
						OutProblems.Add(FString::Printf(TEXT("Cannot find Property for column '%s' in struct '%s'."), *PropName.ToString(), *InRowStruct->GetName()));
					}
				}
				// Found one!
				else
				{
					// Check we don't have this property already
					if(ColumnProps.Contains(ColumnProp))
					{
						OutProblems.Add(FString::Printf(TEXT("Duplicate column '%s'."), *ColumnProp->GetName()));
					}
					// Check we support this property type
					else if( !DynamicDataTableUtils::IsSupportedTableProperty(ColumnProp) )
					{
						OutProblems.Add(FString::Printf(TEXT("Unsupported Property type for struct member '%s'."), *ColumnProp->GetName()));
					}
					// Looks good, add to array
					else
					{
						ColumnProps[ColIdx] = ColumnProp;
					}

					// Track that we found this one
					ExpectedPropNames.Remove(ColumnProp->GetFName());
				}
			}
		}
	}

	if (!bIgnoreMissingFields)
	{
		// Generate warning for any properties in struct we are not filling in
		for (int32 PropIdx = 0; PropIdx < ExpectedPropNames.Num(); PropIdx++)
		{
			const FProperty* const ColumnProp = FindFProperty<FProperty>(InRowStruct, ExpectedPropNames[PropIdx]);

#if WITH_EDITOR
			// If the structure has specified the property as optional for import (gameplay code likely doing a custom fix-up or parse of that property),
			// then avoid warning about it
			static const FName DynamicDataTableImportOptionalMetadataKey(TEXT("DynamicDataTableImportOptional"));
			if (ColumnProp->HasMetaData(DynamicDataTableImportOptionalMetadataKey))
			{
				continue;
			}
#endif // WITH_EDITOR

			const FString DisplayName = DynamicDataTableUtils::GetPropertyExportName(ColumnProp);
			OutProblems.Add(FString::Printf(TEXT("Expected column '%s' not found in input."), *DisplayName));
		}
	}

	return ColumnProps;
}

TArray<FString> UDynamicDataTable::CreateTableFromCSVString(const FString& InString)
{
	DynamicDataTable_CHANGE_SCOPE();

	// Array used to store problems about table creation
	TArray<FString> OutProblems;

	FDynamicDataTableImporterCSV(*this, InString, OutProblems).ReadTable();
	OnPostDataImported(OutProblems);

	return OutProblems;
}

TArray<FString> UDynamicDataTable::CreateTableFromJSONString(const FString& InString)
{
	DynamicDataTable_CHANGE_SCOPE();

	// Array used to store problems about table creation
	TArray<FString> OutProblems;

	FDynamicDataTableImporterJSON(*this, InString, OutProblems).ReadTable();
	OnPostDataImported(OutProblems);

	return OutProblems;
}

TArray<FString> UDynamicDataTable::CreateTableFromOtherTable(const UDynamicDataTable* InTable)
{
	DynamicDataTable_CHANGE_SCOPE();

	// Array used to store problems about table creation
	TArray<FString> OutProblems;

	if (InTable == nullptr)
	{
		OutProblems.Add(TEXT("No input table provided"));
		return OutProblems;
	}

	if (RowStruct && RowMap.Num() > 0)
	{
		EmptyTable();
	}

	RowStruct = InTable->RowStruct;

	// make a local copy of the rowmap so we have a snapshot of it
	TMap<FName, uint8*> InRowMapCopy = InTable->GetRowMap();

	UScriptStruct& EmptyUsingStruct = GetEmptyUsingStruct();
	for (TMap<FName, uint8*>::TConstIterator RowMapIter(InRowMapCopy.CreateConstIterator()); RowMapIter; ++RowMapIter)
	{
		uint8* NewRawRowData = (uint8*)FMemory::Malloc(EmptyUsingStruct.GetStructureSize());
		EmptyUsingStruct.InitializeStruct(NewRawRowData);
		EmptyUsingStruct.CopyScriptStruct(NewRawRowData, RowMapIter.Value());
		RowMap.Add(RowMapIter.Key(), NewRawRowData);
	}

	return OutProblems;
}

TArray<FString> UDynamicDataTable::CreateTableFromRawData(TMap<FName, const uint8*>& DataMap, UScriptStruct* InRowStruct)
{
	DynamicDataTable_CHANGE_SCOPE();

	// Array used to store problems about table creation
	TArray<FString> OutProblems;

	if (InRowStruct == nullptr)
	{
		OutProblems.Add(TEXT("No input struct provided"));
		return OutProblems;
	}

	if (RowStruct && RowMap.Num() > 0)
	{
		EmptyTable();
	}

	RowStruct = InRowStruct;

	UScriptStruct& EmptyUsingStruct = GetEmptyUsingStruct();
	for (TMap<FName, const uint8*>::TConstIterator RowMapIter(DataMap.CreateConstIterator()); RowMapIter; ++RowMapIter)
	{
		uint8* NewRawRowData = static_cast<uint8*>(FMemory::Malloc(EmptyUsingStruct.GetStructureSize()));
		EmptyUsingStruct.InitializeStruct(NewRawRowData);
		EmptyUsingStruct.CopyScriptStruct(NewRawRowData, RowMapIter.Value());
		RowMap.Add(RowMapIter.Key(), NewRawRowData);
	}

	return OutProblems;
}

#if WITH_EDITOR

TArray<FString> UDynamicDataTable::GetColumnTitles() const
{
	TArray<FString> Result;
	Result.Add(TEXT("Name"));
	if (RowStruct)
	{
		for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
		{
			FProperty* Prop = *It;
			check(Prop != nullptr);
			const FString DisplayName = DynamicDataTableUtils::GetPropertyExportName(Prop);
			Result.Add(DisplayName);
		}
	}
	return Result;
}

TArray<FString> UDynamicDataTable::GetUniqueColumnTitles() const
{
	TArray<FString> Result;
	Result.Add(TEXT("Name"));
	if (RowStruct)
	{
		for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
		{
			FProperty* Prop = *It;
			check(Prop != nullptr);
			const FString DisplayName = Prop->GetName();
			Result.Add(DisplayName);
		}
	}
	return Result;
}

TArray< TArray<FString> > UDynamicDataTable::GetTableData(const EDynamicDataTableExportFlags InDTExportFlags) const
{
	 TArray< TArray<FString> > Result;

	 Result.Add(GetColumnTitles());

	 // First build array of properties
	 TArray<FProperty*> StructProps;
	 if (RowStruct)
	 {
	 	for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
	 	{
		 	FProperty* Prop = *It;
			check(Prop != nullptr);
		 	StructProps.Add(Prop);
	 	}
	 }

	 // Now iterate over rows
	 for ( auto RowIt = RowMap.CreateConstIterator(); RowIt; ++RowIt )
	 {
		 TArray<FString> RowResult;
		 FName RowName = RowIt.Key();
		 RowResult.Add(RowName.ToString());

		 uint8* RowData = RowIt.Value();
		 for(int32 PropIdx=0; PropIdx<StructProps.Num(); PropIdx++)
		 {
			 RowResult.Add(DynamicDataTableUtils::GetPropertyValueAsString(StructProps[PropIdx], RowData, InDTExportFlags));
		 }
		 Result.Add(RowResult);
	 }
	 return Result;

}

#endif //WITH_EDITOR

TArray<FName> UDynamicDataTable::GetRowNames() const
{
	TArray<FName> Keys;
	RowMap.GetKeys(Keys);
	return Keys;
}

bool FDynamicDataTableRowHandle::operator==(FDynamicDataTableRowHandle const& Other) const
{
	return DynamicDataTable == Other.DynamicDataTable && RowName == Other.RowName;
}

bool FDynamicDataTableRowHandle::operator != (FDynamicDataTableRowHandle const& Other) const
{
	return DynamicDataTable != Other.DynamicDataTable || RowName != Other.RowName;
}

void FDynamicDataTableRowHandle::PostSerialize(const FArchive& Ar)
{
	if (Ar.IsSaving() && !IsNull() && DynamicDataTable)
	{
		// Note which row we are pointing to for later searching
		Ar.MarkSearchableName(DynamicDataTable, RowName);
	}
}

bool FDynamicDataTableCategoryHandle::operator==(FDynamicDataTableCategoryHandle const& Other) const
{
	return DynamicDataTable == Other.DynamicDataTable && ColumnName == Other.ColumnName && RowContents == Other.RowContents;
}

bool FDynamicDataTableCategoryHandle::operator != (FDynamicDataTableCategoryHandle const& Other) const
{
	return DynamicDataTable != Other.DynamicDataTable || ColumnName != Other.ColumnName || RowContents != Other.RowContents;
}

void UDynamicDataTable::RefreshDynTable() {
	UDynamicDataTable* Source = this;
	if (Source) {
		FDynamicTable& Table = Source->DynamicTable;
		if (Source==nullptr) {return;}
		TArray<UDynamicDataTable*> sources = {};
		/*if (Source->IsA(UCompositeDataTable::StaticClass())) {
			UCompositeDataTable* cdt = Cast<UCompositeDataTable>(Source);
			FArrayProperty* ParentTablesProperty = FindFProperty<FArrayProperty>(cdt->StaticClass(), "ParentTables");
			if (ParentTablesProperty!=nullptr) {
				FScriptArrayHelper ParentTablesHelper(ParentTablesProperty, ParentTablesProperty->ContainerPtrToValuePtr<void>(cdt));
				for (int32 i = 0; i < ParentTablesHelper.Num(); i++) {
					if (FObjectProperty* InnerProp = CastField<FObjectProperty>(ParentTablesProperty->Inner)) {
						UObject* Object = InnerProp->GetObjectPropertyValue(ParentTablesHelper.GetRawPtr(i));
						UDataTable* table = Cast<UDataTable>(Object);
						if (table!=nullptr) {sources.Emplace(table);}
					}
				}
			}
		}
		else {*/
		sources.Emplace(Source);
		//}
		// NOTE (PreciseTransform fix, 2026-09-04): do NOT wipe Table.Map/RowNames here.
		// This function runs unconditionally from PostLoad() on every asset load. Wiping
		// first destroyed any row added via the FInstancedStruct API (AddRowToTable /
		// AddRowsFromDataTable, e.g. MergeDataTableIntoDynamicDataTable), since those rows
		// only ever exist in Table.Map and are absent from the legacy RowMap this loop
		// rebuilds from - every row added that way was silently lost on the next editor
		// restart. The Contains() guard below already makes this loop merge-safe once the
		// wipe is gone; rows already in Table.Map (from normal tagged-property load) are
		// preserved, and anything present only in the legacy RowMap still gets synced in.
		for (UDynamicDataTable* each : sources) {
			TMap<FName,uint8*> data = each->GetRowMap();
			UScriptStruct* rowStruct = each->RowStruct;
			Table.Type = each->RowStruct;
			for (auto rowIt = each->GetRowMap().CreateConstIterator(); rowIt; ++rowIt) {
				FName rowName = rowIt.Key();
				if (!Table.Map.Contains(rowName)) {
					//void* rowStructData = FMemory::Malloc(rowStruct->GetStructureSize());
					//rowStruct->InitializeStruct(rowStructData);
					//rowStruct->CopyScriptStruct(rowStructData, rowIt.Value());
					//Table.Map.Emplace(rowName, rowStructData);
					
					//FString strdata = "";
					//UDynamicDatatableBPLibrary::StructToString(Table.Type,rowIt.Value(),strdata);
					//Table.Map.Emplace(rowName, MoveTemp(strdata));
					
					auto& elem = Table.Map.Emplace(rowName,FInstancedStruct());
					elem.InitializeAs(rowStruct, (uint8*)rowIt.Value());
					Table.RowNames.Emplace(rowName);
					Table.Sources.Emplace(rowName,nullptr);
				}
			}
		}
	}
}