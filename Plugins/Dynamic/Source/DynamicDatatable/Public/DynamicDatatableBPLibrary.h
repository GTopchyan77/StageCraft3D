// Copyright Vyrulent 2023 All Rights Reserved.

#pragma once

#pragma warning( disable : 4996) //deprication incoming
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Engine/DataTable.h"
#include "rapidjson/reader.h"
#include "StructUtils/InstancedStruct.h"
#include "StructUtils/UserDefinedStruct.h"
#include "Misc/App.h"
#include "UObject/Package.h"
#include "CoreUObjectClasses.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Class.h"
//#include "Engine.h"
#include "DynamicDatatableBPLibrary.generated.h"

class UDynamicDataTable;
//typedef UDynamicDatatableBPLibrary DynamicDatatable;

UENUM(BlueprintType)
enum class OverwriteRowBehavior : uint8 {
	/** Overwrites the existing row */
	E_Overwrite     UMETA(DisplayName = "Overwrite Row"),
	/** Increments the row name like: rowname -> rowname_0 -> rowname_1. */
    E_Increment     UMETA(DisplayName = "Increment Row Name"),
    /** Skips adding the new row. */
	E_Skip     UMETA(DisplayName = "Skip Row")
};

UENUM(BlueprintType)
enum class GetRowExec : uint8 {
	/** Found the row. */
	Found     UMETA(DisplayName = "Found"),
	/** Row was not found. */
    NotFound     UMETA(DisplayName = "Not Found")
};

USTRUCT(BlueprintType)
struct FDynamicTable {
	GENERATED_BODY()

	/** All row data. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "DynamicDatatable")
	TMap<FName,FInstancedStruct> Map = {};
	
	/** All rows in order. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "DynamicDatatable")
	TArray<FName> RowNames = {};
	
	/** Lookup for the source table of each row. Useful for compositing different tables. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "DynamicDatatable")
	TMap<FName,UDataTable*> Sources = {};

	TMap<UDataTable*,TArray<FName>> SourceRowNames = {};

	/** Cached name set when the table gets cached. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "DynamicDatatable")
	FName CacheName = "";
	
	/** Must be put last in the struct. */
	UPROPERTY(SaveGame)
	UScriptStruct* Type = nullptr;

	FORCEINLINE void Clear() {
		Map = {};
		RowNames = {};
		Sources = {};
		SourceRowNames = {};
		//Type = nullptr;
	}
	
};


USTRUCT(BlueprintType)
struct FTableDiffResult {
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DynamicDatatable")
	bool SameType = false;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DynamicDatatable")
	TArray<FName> RowsNotInTable1 = {};
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DynamicDatatable")
	TArray<FName> RowsNotInTable2 = {};
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DynamicDatatable")
	TArray<FName> SharedRows = {};
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DynamicDatatable")
	TArray<FName> IdenticalRows = {};
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DynamicDatatable")
	TArray<FName> DifferentRows = {};

};

USTRUCT(BlueprintType)
struct FDynSaver {
	GENERATED_BODY()
	
	UPROPERTY()
	TMap<FName,FString> CachedStrings = {};
	
};

UDELEGATE(BlueprintCallable)
DECLARE_DYNAMIC_DELEGATE_TwoParams(FAsyncSaveCsvDelegate, FName, ID, bool, out);

UDELEGATE(BlueprintCallable)
DECLARE_DYNAMIC_DELEGATE_TwoParams(FAsyncLoadCsvDelegate, FName, ID, bool, out);

UCLASS()
class DYNAMICDATATABLE_API UDynamicDatatableBPLibrary : public UBlueprintFunctionLibrary {
	GENERATED_UCLASS_BODY()
	
	/** 
		*  Adds a row to the Dynamic Table
		*  An easy way to set the wildcard is to create a "Make" node for your struct and connect it.
	*/
	UFUNCTION(BlueprintCallable, CustomThunk, meta=(CustomStructureParam = "Row"), Category = "DynamicDatatable|Adding")
	static void AddRowToTable( UPARAM(ref) FDynamicTable& Table, FName RowName, UPARAM(ref) const int32& Row, UDataTable* Source);
	
	DECLARE_FUNCTION(execAddRowToTable) {
		P_GET_STRUCT_REF(FDynamicTable,Table);
		P_GET_PROPERTY(FNameProperty,RowName);
		Stack.Step(Stack.Object, NULL);
		FStructProperty* structProperty = CastField<FStructProperty>(Stack.MostRecentProperty);
		void* StructPtr = Stack.MostRecentPropertyAddress;
		UScriptStruct* scriptStruct = structProperty->Struct;
		//void* StructInMap = FMemory::Malloc(scriptStruct->GetStructureSize());
		//FMemory::Memcpy(StructInMap, StructPtr, scriptStruct->GetStructureSize());
		if (RowName.IsNone()) {
			RowName = FName("Row_"+FString::FromInt(Table.RowNames.Num()+1));
		}
		P_GET_OBJECT(UDataTable,Source);
		if (Table.RowNames.Contains(RowName)) {//update existing
			//void* StructInMap;
			//Table.Map.Emplace(RowName,StructInMap);
			//Table.Map[RowName] = FMemory::Malloc(scriptStruct->GetStructureSize());
			//scriptStruct->InitializeStruct(Table.Map[RowName]);
			//scriptStruct->CopyScriptStruct(Table.Map[RowName], StructPtr);
			//FString strdata = "";
			//StructToString(scriptStruct,StructPtr,strdata);
			//Table.Map.Emplace(RowName, MoveTemp(strdata));
			auto& elem = Table.Map.Emplace(RowName,FInstancedStruct());
			elem.InitializeAs(scriptStruct, (uint8*)StructPtr);
			if (Source!=nullptr) {
				if (!Table.SourceRowNames.Contains(Source)) {
					TArray<FName> blank = {};
					Table.SourceRowNames.Emplace(Source,blank);
				}
				Table.SourceRowNames[Source].AddUnique(RowName);
			}
			Table.Sources.Emplace(RowName,Source);
		}
		else {
			//void* StructInMap;
            //Table.Map.Emplace(RowName,StructInMap);
            //Table.Map[RowName] = FMemory::Malloc(scriptStruct->GetStructureSize());
            //scriptStruct->InitializeStruct(Table.Map[RowName]);
            //scriptStruct->CopyScriptStruct(Table.Map[RowName], StructPtr);
			//FString strdata = "";
			//StructToString(scriptStruct,StructPtr,strdata);
			//Table.Map.Emplace(RowName, MoveTemp(strdata));
			auto& elem = Table.Map.Emplace(RowName,FInstancedStruct());
			elem.InitializeAs(scriptStruct, (uint8*)StructPtr);
            Table.RowNames.Emplace(RowName);
			if (Source!=nullptr) {
				if (!Table.SourceRowNames.Contains(Source)) {
					TArray<FName> blank = {};
					Table.SourceRowNames.Emplace(Source,blank);
				}
				Table.SourceRowNames[Source].Emplace(RowName);
			}
			Table.Sources.Emplace(RowName,Source);
		}
		Table.Type = scriptStruct;
		
		P_FINISH;
	}

	/** 
		*  Gets a Row from the Dynamic Table
		*  You can use any struct for Row, if it's different than the table then only properties with the same name and datatype get filled.
		*  An easy way to set the wildcard is to create a "Break" node for your struct and connect it.
	*/
	UFUNCTION(BlueprintCallable, CustomThunk, meta=(CustomStructureParam = "Row", ExpandEnumAsExecs = "Success"), Category = "DynamicDatatable|Getting")
	static void GetRowFromTable( UPARAM(ref) FDynamicTable& Table, FName RowName, GetRowExec& Success, int32& Row, UDataTable*& Source, uint8& SourceByteIndex);

	DECLARE_FUNCTION(execGetRowFromTable) {
		P_GET_STRUCT_REF(FDynamicTable,Table);
		P_GET_PROPERTY(FNameProperty,RowName);
		P_GET_ENUM_REF(GetRowExec,Success);
		//auto* Row = Table.Map.Find(RowName);
		Stack.Step(Stack.Object, NULL);
		FStructProperty* structProperty = CastField<FStructProperty>(Stack.MostRecentProperty);
		void* StructPtr = Stack.MostRecentPropertyAddress;
		UScriptStruct* scriptStruct = structProperty->Struct;
		P_GET_OBJECT_REF(UDataTable,Source);
		P_GET_PROPERTY_REF(FByteProperty,SourceByteIndex);
		
		P_FINISH;
		P_NATIVE_BEGIN;
		if (Table.Map.Contains(RowName)) {
			//StringToStruct(Table.Map[RowName],scriptStruct,StructPtr);
			//scriptStruct = Table.Type;
			//StructPtr         = Table.Map[RowName].GetMutableMemory();
			scriptStruct->CopyScriptStruct(StructPtr,Table.Map.Find(RowName)->GetMutableMemory());
			/*if (Table.Type!=scriptStruct) {
				FString str = "", name = "";
				TMap<FString,FProperty*> Props = {};
				int x = 0, sourceIndex = 0;
				for (TFieldIterator<FProperty> It(Table.Type); It; ++It) {
					FProperty* Property = *It;
					if (Property!=nullptr) {
						Props.Emplace(Property->GetAuthoredName().TrimStartAndEnd(), Property);
					}
				}
				for (TFieldIterator<FProperty> It(scriptStruct); It; ++It) {
					str = "";
					FProperty* Property = *It;
					if (Property!=nullptr) {
						name = Property->GetAuthoredName().TrimStartAndEnd();
						if (Props.Contains(name)) {
							if (Props[name]->SameType(Property)) {
								Props[name]->ExportTextItem_Direct(str, Props[name]->ContainerPtrToValuePtr<void>(Table.Map[RowName]), nullptr, nullptr, EPropertyPortFlags::PPF_ExternalEditor);
								Property->ImportText_Direct(*str, Property->ContainerPtrToValuePtr<void>(StructPtr), nullptr, EPropertyPortFlags::PPF_ExternalEditor, nullptr);
							}
						}
					}
				}
			}
			else {structProperty->CopyCompleteValue((void*)StructPtr, Table.Map[RowName]);}*/
			Success = GetRowExec::Found;
		}
		else {Success = GetRowExec::NotFound;}
		//P_GET_PROPERTY_REF(FIntProperty,Index);
		//Index = Table.RowNames.Find(RowName);
		if (Table.Sources.Contains(RowName)) {
			Source = Table.Sources[RowName];
		}
		else {
			Source = nullptr;
		}
		int x = 0;
		SourceByteIndex = GetSourceIndex(Table,Source,x);
		
		P_NATIVE_END;
	}
	
	/** 
		*  Gets a Row from the Dynamic Table
		*  Pure node for convenience, this node will be called every time it's accessed. Use small structs or set the result to a variable first.
		*  You can use any struct for Row, if it's different than the table then only properties with the same name and datatype get filled.
		*  An easy way to set the wildcard is to create a "Break" node for your struct and connect it.
	*/
	UFUNCTION(BlueprintPure, CustomThunk, meta=(DisplayName = "Get Row From Table (Pure)", CompactNodeTitle = "GET", CustomStructureParam = "Row"), Category = "DynamicDatatable|Getting")
	static void GetRowFromTable_Pure( UPARAM(ref) FDynamicTable& Table, FName RowName, int32& Row, bool& Exists, int& Index);

	DECLARE_FUNCTION(execGetRowFromTable_Pure) {
		P_GET_STRUCT_REF(FDynamicTable,Table);
		P_GET_PROPERTY(FNameProperty,RowName);
		//auto* Row = Table.Map.Find(Index);
		Stack.Step(Stack.Object, NULL);
		FStructProperty* structProperty = CastField<FStructProperty>(Stack.MostRecentProperty);
		void* StructPtr = Stack.MostRecentPropertyAddress;
		UScriptStruct* scriptStruct = structProperty->Struct;
		P_GET_PROPERTY_REF(FBoolProperty,Exists);
		P_GET_PROPERTY_REF(FIntProperty,Index);
		Exists = false;
		if (Table.Map.Contains(RowName)) {
			Exists = true;
			//StringToStruct(Table.Map[RowName],scriptStruct,StructPtr);
			//scriptStruct = Table.Type;
			//StructPtr         = Table.Map[RowName].GetMutableMemory();
			scriptStruct->CopyScriptStruct(StructPtr,Table.Map.Find(RowName)->GetMutableMemory());
			/*if (Table.Type!=scriptStruct) {
				FString str = "", name = "";
				TMap<FString,FProperty*> Props = {};
				int x = 0, sourceIndex = 0;
				for (TFieldIterator<FProperty> It(Table.Type); It; ++It) {
					FProperty* Property = *It;
					if (Property!=nullptr) {
						Props.Emplace(Property->GetAuthoredName().TrimStartAndEnd(), Property);
					}
				}
				for (TFieldIterator<FProperty> It(scriptStruct); It; ++It) {
					str = "";
					FProperty* Property = *It;
					if (Property!=nullptr) {
						name = Property->GetAuthoredName().TrimStartAndEnd();
						if (Props.Contains(name)) {
							if (Props[name]->SameType(Property)) {
								Props[name]->ExportTextItem_Direct(str, Props[name]->ContainerPtrToValuePtr<void>(Table.Map[RowName]), nullptr, nullptr, EPropertyPortFlags::PPF_ExternalEditor);
								Property->ImportText_Direct(*str, Property->ContainerPtrToValuePtr<void>(StructPtr), nullptr, EPropertyPortFlags::PPF_ExternalEditor, nullptr);
							}
						}
					}
				}
			}
			else {structProperty->CopyCompleteValue((void*)StructPtr, *Table.Map.Find(RowName));}*/
		}
		Index = Table.RowNames.Find(RowName);
		
		P_FINISH;
	}

	/** 
		*  Deletes a row from the Dynamic Table
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|Utility", meta=(Keywords="Remove"))
	static void DeleteTableRow( UPARAM(ref) FDynamicTable& Table, FName RowName);

	/** 
		*  Deletes a row from the Dynamic Table
		*  Index corresponds to the RowNames Array
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|Utility", meta=(Keywords="Remove"))
	static void DeleteTableRowAtIndex( UPARAM(ref) FDynamicTable& Table, int Index);

	/** 
		*  Completely clears the Dynamic Table
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|Utility")
	static void ClearTable( UPARAM(ref) FDynamicTable& Table);

	/** 
		*  Checks whether the Dynamic Table row exists
		*  Returns true if RowName is found
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|Getting")
	static bool TableRowExists( UPARAM(ref) FDynamicTable& Table, FName RowName);

	/** 
		*  Gets the array of row names from the Dynamic Table
		*  You can also break the Table struct and use RowNames instead
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|Getting")
	static TArray<FName> GetRowNamesFromTable( UPARAM(ref) FDynamicTable& Table);

	/** 
		*  Appends all the rows in the Datatable to the Dynamic Table
		*  Tracks which Source Datatable each row came from
		*  Supports Composite Data Tables (adds from all of its Parent Tables)
		*  Ignores existing entries
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|Adding")
	static void AddRowsFromDataTable( UPARAM(ref) FDynamicTable& Table, UDataTable* Source, bool Overwrite = false);
	
	/** 
		* Returns the number of rows in the Dynamic Table 
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|Getting", meta=(Keywords = "count num length"))
	static int GetSizeOfTable( UPARAM(ref) FDynamicTable& Table);

	/** 
		*  Returns the number of rows in the Dynamic Table that came from the Source Datatable
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|Getting", meta=(Keywords = "count num length"))
	static int GetSizeFromSourceTable( UPARAM(ref) FDynamicTable& Table, UDataTable* Source);

	/** 
		*  Returns an array of row names that came from the Source Datatable
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|Getting")
	static TArray<FName> GetRowNamesFromSourceTable( UPARAM(ref) FDynamicTable& Table, UDataTable* Source);

	/** 
		*  Creates a CSV (Comma Separated Values) string which can be used to fill other tables
		*  Modeled after the UE5 Datatable export result
		*  Warning, may lose some data depending on the types
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|Utility")
	static FString ConvertTableToCsvString( UPARAM(ref) FDynamicTable& Table);

	/** 
		*  Fills the Dynamic Table using a CSV (Comma Separated Values) string
		*  Make sure the Table already has a Type (use Set Table Type or Create Dynamic Table)
		*  Warning, may lose some data depending on the types
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|Adding")
	static void AddRowsFromCsvString( UPARAM(ref) FDynamicTable& Table, UPARAM(ref) FString& Csv);

	/** 
    	*  Creates a new Dynamic Table
    	*  BaseStruct is the struct Type used for the table (use a dummy "Make" struct node for the wildcard)
    	*  This uses the struct's structural components, not the values, so a dummy or a struct in use are both fine.
    */
    UFUNCTION(BlueprintCallable, CustomThunk, meta=(CustomStructureParam = "BaseStruct", Keywords = "New Dynamic Data Table"), Category = "DynamicDatatable")
	static FDynamicTable CreateDynamicTable( UPARAM(ref) int32& BaseStruct);
	
	DECLARE_FUNCTION(execCreateDynamicTable) {
		Stack.Step(Stack.Object, NULL);
		FStructProperty* structProperty = CastField<FStructProperty>(Stack.MostRecentProperty);
		void* StructPtr = Stack.MostRecentPropertyAddress;
		UScriptStruct* scriptStruct = structProperty->Struct;
		
		P_FINISH;
		
		P_NATIVE_BEGIN;
		FDynamicTable Table;
		Table.Type = scriptStruct;
		*(FDynamicTable*)RESULT_PARAM = Table;
		P_NATIVE_END;
	}

	/** 
        *  Sets the type for a Dynamic Table
        *  If you change the type with data in the table, it may cause undetermined behavior
        *  Type is used for things like properly importing CSV info
    */
    UFUNCTION(BlueprintCallable, CustomThunk, meta=(CustomStructureParam = "BaseStruct", Keywords = "struct"), Category = "DynamicDatatable|Setting")
    static void SetTableType( UPARAM(ref) FDynamicTable& Table, UPARAM(ref) int32& BaseStruct);
	
	DECLARE_FUNCTION(execSetTableType) {
		P_GET_STRUCT_REF(FDynamicTable, Table);
		Stack.Step(Stack.Object, NULL);
		FStructProperty* structProperty = CastField<FStructProperty>(Stack.MostRecentProperty);
		void* StructPtr = Stack.MostRecentPropertyAddress;
		UScriptStruct* scriptStruct = structProperty->Struct;
		Table.Type = scriptStruct;

		P_FINISH;
	}

    /** 
        *  Returns true if the Dynamic Table is the same Type as the BaseStruct
    */
    UFUNCTION(BlueprintCallable, CustomThunk, meta=(CustomStructureParam = "BaseStruct", Keywords = "struct"), Category = "DynamicDatatable")
    static bool IsTableType( UPARAM(ref) FDynamicTable& Table, UPARAM(ref) int32& BaseStruct);
	
	DECLARE_FUNCTION(execIsTableType) {
		P_GET_STRUCT_REF(FDynamicTable, Table);
		Stack.Step(Stack.Object, NULL);
		FStructProperty* structProperty = CastField<FStructProperty>(Stack.MostRecentProperty);
		void* StructPtr = Stack.MostRecentPropertyAddress;
		UScriptStruct* scriptStruct = structProperty->Struct;
		if (Table.Type==nullptr) {
			*(bool*) RESULT_PARAM = false;
		}
		*(bool*) RESULT_PARAM = (Table.Type->GetStructCPPName() == scriptStruct->GetStructCPPName());
		P_FINISH;
	}

	/** 
		*  Returns true if the table has any Type
		*  Use Set Table Type or Create Dynamic Table to remedy this
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable", meta=(Keywords = "struct"))
	static bool TableHasAnyType( UPARAM(ref) FDynamicTable& Table);
	
    /** 
		*  Gets a Row from the Dynamic Table
		*  Index corresponds to the RowNames Array
		*  You can use any struct for Row, if it's different than the table then only properties with the same name and datatype get filled.
		*  An easy way to set the wildcard is to create a "Break" node for your struct and connect it.
	*/
	UFUNCTION(BlueprintCallable, CustomThunk, meta=(CustomStructureParam = "Row"), Category = "DynamicDatatable|Getting")
	static void GetRowFromTableAtIndex( UPARAM(ref) FDynamicTable& Table, int Index, int32& Row, FName& RowName, UDataTable*& Source);

	DECLARE_FUNCTION(execGetRowFromTableAtIndex) {
		P_GET_STRUCT_REF(FDynamicTable,Table);
		P_GET_PROPERTY(FIntProperty,index);
		//auto* Row = Table.Map.Find(Index);
		Stack.Step(Stack.Object, NULL);
		FStructProperty* structProperty = CastField<FStructProperty>(Stack.MostRecentProperty);
		void* StructPtr = Stack.MostRecentPropertyAddress;
		UScriptStruct* scriptStruct = structProperty->Struct;
		P_GET_PROPERTY_REF(FNameProperty,RowName);
		P_GET_OBJECT_REF(UDataTable,Source);
		if (index>=0&&index<Table.RowNames.Num()) {
			RowName = Table.RowNames[index];
			if (Table.Map.Contains(RowName)) {
				//StringToStruct(Table.Map[RowName],scriptStruct,StructPtr);
				//scriptStruct = Table.Type;
				//StructPtr         = Table.Map[RowName].GetMutableMemory();
				scriptStruct->CopyScriptStruct(StructPtr,Table.Map.Find(RowName)->GetMutableMemory());
				/*if (Table.Type!=scriptStruct) {
					FString str = "", name = "";
					TMap<FString,FProperty*> Props = {};
					int x = 0, sourceIndex = 0;
					for (TFieldIterator<FProperty> It(Table.Type); It; ++It) {
						FProperty* Property = *It;
						if (Property!=nullptr) {
							Props.Emplace(Property->GetAuthoredName().TrimStartAndEnd(), Property);
						}
					}
					for (TFieldIterator<FProperty> It(scriptStruct); It; ++It) {
						str = "";
						FProperty* Property = *It;
						if (Property!=nullptr) {
							name = Property->GetAuthoredName().TrimStartAndEnd();
							if (Props.Contains(name)) {
								if (Props[name]->SameType(Property)) {
									Props[name]->ExportTextItem_Direct(str, Props[name]->ContainerPtrToValuePtr<void>(Table.Map[RowName]), nullptr, nullptr, EPropertyPortFlags::PPF_ExternalEditor);
									Property->ImportText_Direct(*str, Property->ContainerPtrToValuePtr<void>(StructPtr), nullptr, EPropertyPortFlags::PPF_ExternalEditor, nullptr);
								}
							}
						}
					}
				}
				else {structProperty->CopyCompleteValue((void*)StructPtr, *Table.Map.Find(RowName));}*/
			}
			if (Table.Sources.Contains(RowName)) {
				Source = Table.Sources[RowName];
			}
			else {
				Source = nullptr;
			}
		}
		else {Source = nullptr;}
			
		P_FINISH;
	}
		
	/** 
		*  Gets a Row from the Dynamic Table
		*  Index corresponds to the RowNames Array
		*  Pure for convenience, this node will be called every time it's accessed. Use for small structs or set the result to a variable.
		*  An easy way to set the wildcard is to create a "Break" node for your struct and connect it.
	*/
	UFUNCTION(BlueprintPure, CustomThunk, meta=(DisplayName = "Get Row From Table At Index (Pure)", CompactNodeTitle = "GET", CustomStructureParam = "Row"), Category = "DynamicDatatable|Getting")
	static void GetRowFromTableAtIndex_Pure( UPARAM(ref) FDynamicTable& Table, int Index, int32& Row, bool& Exists, FName& RowName);

	DECLARE_FUNCTION(execGetRowFromTableAtIndex_Pure) {
		P_GET_STRUCT_REF(FDynamicTable,Table);
		P_GET_PROPERTY(FIntProperty,index);
		//auto* Row = Table.Map.Find(Index);
		Stack.Step(Stack.Object, NULL);
		FStructProperty* structProperty = CastField<FStructProperty>(Stack.MostRecentProperty);
		void* StructPtr = Stack.MostRecentPropertyAddress;
		UScriptStruct* scriptStruct = structProperty->Struct;
		P_GET_PROPERTY_REF(FBoolProperty,Exists);
		P_GET_PROPERTY_REF(FNameProperty,RowName);
		Exists = false;
		if (index>=0&&index<Table.RowNames.Num()) {
			RowName = Table.RowNames[index];
			if (Table.Map.Contains(RowName)) {
				Exists = true;
				//StringToStruct(Table.Map[RowName],scriptStruct,StructPtr);
				//scriptStruct = Table.Type;
				//StructPtr         = Table.Map[RowName].GetMutableMemory();
				scriptStruct->CopyScriptStruct(StructPtr,Table.Map.Find(RowName)->GetMutableMemory());
				/*if (Table.Type!=scriptStruct) {
					FString str = "", name = "";
					TMap<FString,FProperty*> Props = {};
					int x = 0, sourceIndex = 0;
					for (TFieldIterator<FProperty> It(Table.Type); It; ++It) {
						FProperty* Property = *It;
						if (Property!=nullptr) {
							Props.Emplace(Property->GetAuthoredName().TrimStartAndEnd(), Property);
						}
					}
					for (TFieldIterator<FProperty> It(scriptStruct); It; ++It) {
						str = "";
						FProperty* Property = *It;
						if (Property!=nullptr) {
							name = Property->GetAuthoredName().TrimStartAndEnd();
							if (Props.Contains(name)) {
								if (Props[name]->SameType(Property)) {
									Props[name]->ExportTextItem_Direct(str, Props[name]->ContainerPtrToValuePtr<void>(Table.Map[RowName]), nullptr, nullptr, EPropertyPortFlags::PPF_ExternalEditor);
									Property->ImportText_Direct(*str, Property->ContainerPtrToValuePtr<void>(StructPtr), nullptr, EPropertyPortFlags::PPF_ExternalEditor, nullptr);
								}
							}
						}
					}
				}
				else {structProperty->CopyCompleteValue((void*)StructPtr, *Table.Map.Find(RowName));}*/
			}
		}
			
		P_FINISH;
	}

	/** 
		*  Returns an array of all rows in the Dynamic Table
		*  An easy way to set the wildcard is to create a "Break" node for your struct and connect it to a foreach loop and then to this node.
	*/
	UFUNCTION(BlueprintCallable, CustomThunk, meta=(ArrayParm = "Rows"), Category = "DynamicDatatable|Getting")
	static void GetAllRowsFromTable( UPARAM(ref) FDynamicTable& Table, TArray<int32>& Rows);

	DECLARE_FUNCTION(execGetAllRowsFromTable) {
		P_GET_STRUCT_REF(FDynamicTable,Table);
		Stack.StepCompiledIn<FArrayProperty>(NULL);
		void* ArrayPtr = Stack.MostRecentPropertyAddress;
		FArrayProperty* ArrayProperty = (FArrayProperty*)(Stack.MostRecentProperty);
		FScriptArrayHelper ArrayHelper(ArrayProperty, ArrayPtr);
		FStructProperty* structProperty = CastField<FStructProperty>(ArrayProperty->Inner);
		FName RowName = TEXT("");
		int i = 0, l = Table.RowNames.Num();
		for (i=0; i<l; ++i) {
			ArrayHelper.AddValue();
			RowName = Table.RowNames[i];
			if (Table.Map.Contains(RowName)) {
				//StringToStruct(Table.Map[RowName],structProperty->Struct,(void*)ArrayHelper.GetRawPtr(i));
				structProperty->CopyCompleteValue((void*)ArrayHelper.GetRawPtr(i), Table.Map.Find(RowName)->GetMutableMemory());
				//structProperty->CopyCompleteValue((void*)ArrayHelper.GetRawPtr(i), *Table.Map.Find(RowName));
			}
		}
		P_FINISH;
	}
	
	/** 
		*  Returns an array of all rows in the Dynamic Table that match the input RowNames
		*  It will always return in the same order as the input RowNames
		*  An easy way to set the wildcard is to create a "Break" node for your struct and connect it to a foreach loop and then to this node.
	*/
	UFUNCTION(BlueprintCallable, CustomThunk, meta=(ArrayParm = "Rows"), Category = "DynamicDatatable|Getting")
	static void GetAllRowsFromTableRowNames( UPARAM(ref) FDynamicTable& Table, TArray<int32>& Rows, UPARAM(ref) TArray<FName>& RowNames);

	DECLARE_FUNCTION(execGetAllRowsFromTableRowNames) {
		P_GET_STRUCT_REF(FDynamicTable,Table);
		Stack.StepCompiledIn<FArrayProperty>(NULL);
		void* ArrayPtr = Stack.MostRecentPropertyAddress;
		FArrayProperty* ArrayProperty = (FArrayProperty*)(Stack.MostRecentProperty);
		FScriptArrayHelper ArrayHelper(ArrayProperty, ArrayPtr);
		FStructProperty* structProperty = CastField<FStructProperty>(ArrayProperty->Inner);
		TArray<FString> RowKeys;
		Stack.StepCompiledIn<FArrayProperty>(NULL);
		void* KeysPtr = Stack.MostRecentPropertyAddress;
		FArrayProperty* NamesProperty = (FArrayProperty*)(Stack.MostRecentProperty);
		FScriptArrayHelper NamesHelper(NamesProperty, KeysPtr);
		FName RowName = TEXT("");
		int i = 0, l = NamesHelper.Num();
		for (i=0; i<l; ++i) {
			ArrayHelper.AddValue();
			RowName = CastField<FNameProperty>(NamesProperty->Inner)->GetPropertyValue(NamesHelper.GetRawPtr(i));
			if (Table.Map.Contains(RowName)) {
				//StringToStruct(Table.Map[RowName],structProperty->Struct,(void*)ArrayHelper.GetRawPtr(i));
				structProperty->CopyCompleteValue((void*)ArrayHelper.GetRawPtr(i), Table.Map.Find(RowName)->GetMutableMemory());
				//structProperty->CopyCompleteValue((void*)ArrayHelper.GetRawPtr(i), *Table.Map.Find(RowName));
			}
		}
		P_FINISH;
	}

	/** 
		*  Returns an array of row names that came from the Source Datatable
		*  Uses GetName() internally for the check, Get Object Name should work for blueprints
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|Getting")
	static TArray<FName> GetRowNamesFromSourceTableName( UPARAM(ref) FDynamicTable& Table, FString SourceName);

	/** 
		*  Returns an array of the Data Tables that have been added to the Dynamic Table
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|Getting")
	static TArray<UDataTable*> GetSourceTables( UPARAM(ref) FDynamicTable& Table);
	
	/** 
		*  Appends two Dynamic Tables, TableB gets added to the end of TableA
		*  OverwriteExisting determines the behavior when encountering rownames that already exist
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|Adding")
	static void AppendTable( UPARAM(ref) FDynamicTable& TableA, UPARAM(ref) FDynamicTable& TableB, OverwriteRowBehavior OverwriteExisting = OverwriteRowBehavior::E_Overwrite);

	static FName IncrementRowNum(FName Row);

	/** 
		*  Writes a string to the file specified. Please include a file extension, e.g. savedtext.txt
		*  Path is a /Game/ relative path, eg. /Game/Saved/SaveGames/ functions like 'Project Saved Dir' return paths like this.
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|Utility")
	static bool WriteStringToFile(FString Path, FString FileName, UPARAM(ref) FString& SaveText, bool Overwrite = true);

	/** 
		* Reads a string from the file specified. Please include a file extension, e.g. savedtext.txt
		*  Path is a /Game/ relative path, eg. /Game/Saved/SaveGames/ functions like 'Project Saved Dir' return paths like this.
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|Utility")
	static bool ReadStringFromFile(FString Path, FString FileName, FString& OutStr);

	/** 
		*  Saves a table as a Csv file, to be used to import later.
		*  Not guaranteed to be compatible with spreadsheet software.
		*  This may lose some information.
		*  Path is a /Game/ relative path, eg. /Game/Saved/SaveGames/ functions like 'Project Saved Dir' return paths like this.
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|Files")
	static bool SaveTableAsCsvFile( UPARAM(ref) FDynamicTable& Table, FString Path, FString FileName);

	/** 
		*  Loads a table from a Csv file, Table must already have a Base Type.
		*  This may lose some information.
		*  Path is a /Game/ relative path, eg. /Game/Saved/SaveGames/ functions like 'Project Saved Dir' return paths like this.
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|Files")
	static void LoadTableFromCsvFile( UPARAM(ref) FDynamicTable& Table, FString Path, FString FileName);

	/** 
		*  Returns the Csv string from the file.
		*  Path is a /Game/ relative path, eg. /Game/Saved/SaveGames/ functions like 'Project Saved Dir' return paths like this.
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|Files")
	static FString LoadCsvStringFromFile(FString Path, FString FileName);

	/** 
		*  Saves the table to a file with the extension .dyntable
		*  Keeps all information in the table, but its rows may still lose some information (Csv warning applies).
		*  Tolerates (some) changes to the structure between saving/loading.
		*  Path is a /Game/ relative path, eg. /Game/Saved/SaveGames/ functions like 'Project Saved Dir' return paths like this.
	*/
	UFUNCTION(BlueprintCallable, meta = (WorldContext = "WorldContextObject"), Category = "DynamicDatatable|Files")
	static bool SaveTable(UObject* WorldContextObject, UPARAM(ref) FDynamicTable& Table, FString Path, FString FileName);

	/** 
		*  Loads the table from a file with the extension .dyntable
		*  Tolerates (some) changes to the structure between saving/loading.
		*  Path is a /Game/ relative path, eg. /Game/Saved/SaveGames/ functions like 'Project Saved Dir' return paths like this.
	*/
	UFUNCTION(BlueprintCallable, meta = (WorldContext = "WorldContextObject"), Category = "DynamicDatatable|Files")
	static FDynamicTable LoadTable(UObject* WorldContextObject, FString SaveDir, FString FileName, bool& Success);
	
	/** 
		*  Rebuilds internal source mapping of RowNames and SourceRowNames
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|Utility")
	static void RecreateMissingData( UPARAM(ref) FDynamicTable& Table);
	
	/** 
		*  Solves math equations. Supports: +,-,*,/,^,(,) and variables
		*  Variables Map should be set up like: VariableName, Value
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|Utility")
	static double EvaluateMath(FString Input, UPARAM(ref) TMap<FName,double>& Variables);

	/** 
		*  Solves math equations. Supports: +,-,*,/,^,(,)
		*  Use EvaluateMath instead if you need variables.
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|Utility")
	static double EvaluateMathNoVariables(FString Input);

	static double EvaluateMathSimple( UPARAM(ref) const FString& input);

	static double ApplyOperation(double leftOperand, double rightOperand, char op);

	static bool IsOp(char op);

	/** 
		*  Returns the property (column) of the row structure.
		*  PropertyName should be the same as what you set the variable name to be when you made the structure.
		*  This is akin to getting a column for a single row, aka a Cell.
		*  Returns as string for any variable type, in the Unreal Csv-friendly format (without escaped quotes).
		*  Recommended: use "DT Get Property" instead (auto converts pins, pulls props directly, no string conversion)
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|Getting", meta=(Keywords="column cell"))
	static FString GetPropertyOfTableRowAsString( UPARAM(ref) FDynamicTable& Table, FName RowName, FName PropertyName);

	/** 
		*  Sets the property at PropertyName for the struct at RowName in the Table.
		*  PropertyName should be the same as what you set the variable name to be when you made the structure.
		*  This is akin to setting a column for one row, aka a Cell.
		*  Use as string for any variable type, refer to the output of GetPropertyOfTableRowAsString for formatting a specific variable type if doing it manually.
		*  Recommended: use "DT Set Property" instead (auto converts pins, sets props directly, no string conversion)
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|Setting", meta=(Keywords="column cell"))
	static bool SetPropertyOfTableRowAsString( UPARAM(ref) FDynamicTable& Table, FName RowName, FName PropertyName, FString Value);

	/** 
		*  Increments an existing value in the table row. Handles floats, ints, and strings/names.
		*  Any Float type - adds Value, treats it as a double.
		*  Any Int type - adds Value, treats it as a int64.
		*  Any String type - adds Value to the number at the end of the string, appends _0 if there's no number already.
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|Setting", meta=(Keywords="column cell"))
	static bool IncrementPropertyOfTableRow( UPARAM(ref) FDynamicTable& Table, FName RowName, FName PropertyName, int Value = 1);
	
	/** 
		*  Saves a table as a Csv file on another thread.
		*  Not guaranteed to be compatible with spreadsheet software.
		*  This may lose some information.
		*  Use AsyncID to track, if you have multiple running at once.
		*  Calls the connected event when finished (drag out the Completed pin and create an event)
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|Async")
	static void Async_SaveTableAsCsvFile( UPARAM(ref) FDynamicTable& Table, FName AsyncID, FString Path, FString FileName, FAsyncSaveCsvDelegate Completed);

	/** 
		*  Fills a table from a Csv file on another thread, Table must already have a Base Type.
		*  This may lose some information.
		*  Use AsyncID to track, if you have multiple running at once.
		*  Calls the connected event when finished (drag out the Completed pin and create an event)
		*  Path is a /Game/ relative path, eg. /Game/Saved/SaveGames/ functions like 'Project Saved Dir' return paths like this.
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|Async")
	static void Async_LoadTableFromCsvFile( UPARAM(ref) FDynamicTable& Table, FName AsyncID, FString Path, FString FileName, FAsyncLoadCsvDelegate Completed);

	/** 
		*  Saves the table to a file with the extension .dyntable on another thread.
		*  Keeps all information in the table, but its rows may still lose some information (Csv warning applies).
		*  Tolerates (some) changes to the structure between saving/loading.
		*  Use AsyncID to track, if you have multiple running at once.
		*  Calls the connected event when finished (drag out the Completed pin and create an event)
		*  Path is a /Game/ relative path, eg. /Game/Saved/SaveGames/ functions like 'Project Saved Dir' return paths like this.
	*/
	UFUNCTION(BlueprintCallable, meta = (WorldContext = "WorldContextObject"), Category = "DynamicDatatable|Async")
	static void Async_SaveTable(UObject* WorldContextObject,  UPARAM(ref) FDynamicTable& Table, FName AsyncID, FString Path, FString FileName, FAsyncSaveCsvDelegate Completed);

	/** 
		*  Loads the table from a file with the extension .dyntable on another thread.
		*  Tolerates (some) changes to the structure between saving/loading.
		*  Use AsyncID to track, if you have multiple running at once.
		*  Calls the connected event when finished (drag out the Completed pin and create an event)
	*/
	UFUNCTION(BlueprintCallable, meta = (WorldContext = "WorldContextObject"), Category = "DynamicDatatable|Async")
	static void Async_LoadTable(UObject* WorldContextObject, UPARAM(ref) FDynamicTable& Table, FName AsyncID, FString Path, FString FileName, FAsyncLoadCsvDelegate Completed);

	/** 
		*  Returns information about the differences between the table rows.
		*  Uses memcmp internally, compares every row for differences.
		*  StopEarly stops as soon as a difference is reached. If set, it will not return complete data.
		*  Does not check for equal row data if the base struct Types are different.
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|Utility")
	static FTableDiffResult DiffTables( UPARAM(ref) FDynamicTable& Table1, UPARAM(ref) FDynamicTable& Table2, bool StopEarly, bool& DiffRowNames, bool& SharedRowsIdentical);

	/** 
		*  converts a Table Diff Result to a String
	*/
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Table Diff Result To String", CompactNodeTitle = "->", BlueprintAutocast), Category = "DynamicDatatable|Utility")
	static FString Conv_TableDiffResultToString(FTableDiffResult Diff);

	/** 
		*  Gets the index of the source, from the order they were added to the table.
		*  Returns as int and byte.
	*/
	UFUNCTION(BlueprintPure, meta = (CompactNodeTitle = "->"), Category = "DynamicDatatable|Utility")
	static uint8 GetSourceIndex( UPARAM(ref) FDynamicTable& Table, UDataTable* Source, int& Index);

	/** 
		*  Makes a 2D indexed row name, reversible with Make XY From Row Name
		*  Useful for inventories
	*/
	UFUNCTION(BlueprintPure, Category = "DynamicDatatable|Utility")
	static FName MakeIndexedRowNameFromXY(FName Base, int X, int Y);

	/** 
		*  Makes a 2D index from a row name, reversible with Make Row Name From XY
		*  Useful for inventories
	*/
	UFUNCTION(BlueprintPure, Category = "DynamicDatatable|Utility")
	static void MakeXYFromIndexedRowName(FName RowName, int& X, int& Y);

	
	/** 
		*  Saves the cached tables to disk.
		*  Use Store Cached Table to cache tables.
		*  Use Retrieve Cached Table to get a cached table.
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|Caching")
	static void SaveCachedTables(FString Path, FString FileName);

	/** 
		*  Loads the cached tables from disk.
		*  Use Store Cached Table to cache tables.
		*  Use Retrieve Cached Table to get a cached table.
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|Caching")
	static void LoadCachedTables(FString Path, FString FileName);

	/** 
		*  Retrieves a table from the cache.
		*  Tables are stored for the lifetime of the program.
		*  Use the Save and Load Cached Tables functions to keep them permanently.
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|Caching")
	static FDynamicTable RetrieveCachedTable(FName TableName, bool& Exists);

	/** 
		*  Stores a table in the cache.
		*  Tables are stored for the lifetime of the program.
		*  Use the Save and Load Cached Tables functions to keep them permanently.
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|Caching")
	static void StoreCachedTable(FName TableName, UPARAM(ref) FDynamicTable& Table);

	/** 
		*  Removes a table from the cache.
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|Caching")
	static void RemoveCachedTable(FName TableName);

	/** 
		*  Removes a table from the cache, if it exists.
		*  Only works if the table was ever stored or retrieved from the cache.
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|Caching")
	static void RemoveTableFromCache( UPARAM(ref) FDynamicTable& Table);

	/** 
		*  Checks whether a table exists in the cache.
	*/
	UFUNCTION(BlueprintPure, Category = "DynamicDatatable|Caching")
	static bool DoesCachedTableExist(FName TableName);

	/** 
		*  Checks whether a table exists in the cache.
		*  Only works if the table was ever stored or retrieved from the cache.
	*/
	UFUNCTION(BlueprintPure, Category = "DynamicDatatable|Caching")
	static bool IsTableCached( UPARAM(ref) FDynamicTable& Table);

	/** 
		*  Diffs against the cached version.
		*  Uses memcmp internally, compares every row for differences and stops early if it finds one.
		*  You should do speed tests with your setup, there are likely better ways to handle this, but provided for convenience.
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|Caching")
	static bool IsCachedTableUpToDate( UPARAM(ref) FDynamicTable& Table);

	/** 
		*  Returns the CachedTables Map variable.
		*  Potentially massive amount of data being copied, only use for debugging or behind loading screens.
		*  Use Get Cached Table Names instead.
		*  Much better to only get the tables you need.
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|Caching")
	static TMap<FName,FDynamicTable> GetEntireCachedTablesMap();

	/** 
		*  Returns the Table Names of all tables that have been cached.
		*  If this is empty and you expected something, you may not be saving and loading.
		*  The cache only exists for the lifetime of the program.
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|Caching")
	static TArray<FName> GetCachedTableNames();

	/** 
		*  Compares two structs for equality. Uses memcmp.
	*/
	UFUNCTION(BlueprintPure, CustomThunk, meta=(CompactNodeTitle="==", CustomStructureParam = "StructA,StructB"), Category = "DynamicDatatable|Utility")
	static bool StructEqualsStruct( UPARAM(ref) int32& StructA, UPARAM(ref) int32& StructB);
	
	DECLARE_FUNCTION(execStructEqualsStruct) {
		Stack.Step(Stack.Object, NULL);
		FStructProperty* structPropertyA = CastField<FStructProperty>(Stack.MostRecentProperty);
		bool stop = false;
		if (!structPropertyA) {stop = true;}
		if (!stop) {
			void* StructPtrA = Stack.MostRecentPropertyAddress;
			UScriptStruct* scriptStructA = structPropertyA->Struct;
			Stack.Step(Stack.Object, NULL);
			FStructProperty* structPropertyB = CastField<FStructProperty>(Stack.MostRecentProperty);
			if (!structPropertyB) {stop = true;}
			if (!stop) {
				void* StructPtrB = Stack.MostRecentPropertyAddress;
				UScriptStruct* scriptStructB = structPropertyB->Struct;
				if (StructPtrA&&StructPtrB) {
					*(bool*) RESULT_PARAM = (memcmp(StructPtrA, StructPtrB, std::min(scriptStructA->GetStructureSize(),scriptStructB->GetStructureSize())) == 0);
				}
				else {stop = true;}
			}
		}
		if (stop) {
			*(bool*) RESULT_PARAM = false;
		}
		P_FINISH;
	}
	
	static FString SaveTableString( UPARAM(ref) FDynamicTable& Table);
	
	static void LoadTableString( UPARAM(ref) FDynamicTable& Table, UPARAM(ref) FString& String);

	/** 
		*  Converts one struct to another.
		*  Structs must have some similarly named properties of the same type for it to work.
	*/
	UFUNCTION(BlueprintPure, CustomThunk, meta=(CompactNodeTitle="->", CustomStructureParam = "StructA,StructB"), Category = "DynamicDatatable|Utility")
	static void ReinterpretStruct( UPARAM(ref) int32& StructA, int32& StructB);
	
	DECLARE_FUNCTION(execReinterpretStruct) {
		Stack.Step(Stack.Object, NULL);
		FStructProperty* structPropertyA = CastField<FStructProperty>(Stack.MostRecentProperty);
		bool stop = false;
		if (!structPropertyA) {stop = true;}
		if (!stop) {
			void* StructPtrA = Stack.MostRecentPropertyAddress;
			UScriptStruct* scriptStructA = structPropertyA->Struct;
			Stack.Step(Stack.Object, NULL);
			FStructProperty* structPropertyB = CastField<FStructProperty>(Stack.MostRecentProperty);
			if (!structPropertyB) {stop = true;}
			if (!stop) {
				void* StructPtrB = Stack.MostRecentPropertyAddress;
				UScriptStruct* scriptStructB = structPropertyB->Struct;
				if (StructPtrA&&StructPtrB) {
					FString str = "", name = "";
					TMap<FString,FProperty*> Props = {};
					int x = 0, sourceIndex = 0;
					for (TFieldIterator<FProperty> It(scriptStructA); It; ++It) {
						FProperty* Property = *It;
						if (Property!=nullptr) {
							Props.Emplace(Property->GetAuthoredName().TrimStartAndEnd(), Property);
						}
					}
					for (TFieldIterator<FProperty> It(scriptStructB); It; ++It) {
						str = "";
						FProperty* Property = *It;
						if (Property!=nullptr) {
							name = Property->GetAuthoredName().TrimStartAndEnd();
							if (Props.Contains(name)) {
								if (Props[name]->SameType(Property)) {
									Props[name]->ExportTextItem_Direct(str, Props[name]->ContainerPtrToValuePtr<void>(StructPtrA), nullptr, nullptr, EPropertyPortFlags::PPF_ExternalEditor);
									Property->ImportText_Direct(*str, Property->ContainerPtrToValuePtr<void>(StructPtrB), nullptr, EPropertyPortFlags::PPF_ExternalEditor, nullptr);
								}
							}
						}
					}
				}
			}
		}
		P_FINISH;
	}

	/* Todo: Doesn't work
		*  Returns the property (column) of the row structure.
		*  PropertyName should be the same as what you set the variable name to be when you made the structure.
		*  This is akin to getting a column for a single row, aka a Cell.
		*  Output must be the same datatype as the property.
	*
	UFUNCTION(BlueprintCallable, CustomThunk, meta=(CustomStructureParam = "RowProperty", Keywords="column cell"), Category = "DynamicDatatable|Getting")
	static void GetWildcardProperty( UPARAM(ref) FDynamicTable& Table, FName RowName, FName PropertyName, int32& RowProperty, bool& Found);

	DECLARE_FUNCTION(execGetWildcardProperty) {
		P_GET_STRUCT_REF(FDynamicTable,Table);
		P_GET_PROPERTY(FNameProperty,RowName);
		P_GET_PROPERTY(FNameProperty,PropertyName);
		Stack.Step(Stack.Object, NULL);
		FProperty* Prop = CastField<FProperty>(Stack.MostRecentProperty);
		FString name = PropertyName.ToString(), str = "";
		if (Prop!=nullptr) {
			void* PropPtr = Stack.MostRecentPropertyAddress;
			P_GET_UBOOL_REF(Found);
			if (Table.Map.Contains(RowName)) {
				for (TFieldIterator<FProperty> It(Table.Type); It; ++It) {
					FProperty* Property = *It;
					if (Property!=nullptr) {
						if (Property->GetAuthoredName().TrimStartAndEnd()==name) {
							if (Property->SameType(Prop)) {
								Property->CopyCompleteValue((void*)PropPtr, *Table.Map.Find(RowName));
								Found = true;
								goto didfind;
							}
						}
					}
				}
			}
			Found = false;
			didfind:;
		}
		{UE_LOG(LogTemp, Log, TEXT("%s"), *str);}
	}*/

	/** 
		*  Returns an array of each property (column) name, in order.
	*/
	UFUNCTION(BlueprintCallable, CustomThunk, meta=(CustomStructureParam = "Struct", Keywords = "column"), Category = "DynamicDatatable|Utility")
	static TArray<FName> GetPropertyNamesFromStruct( UPARAM(ref) int32& Struct);
	
	DECLARE_FUNCTION(execGetPropertyNamesFromStruct) {
		Stack.Step(Stack.Object, NULL);
		FStructProperty* structProperty = CastField<FStructProperty>(Stack.MostRecentProperty);
		TArray<FName> Props = {};
		if (structProperty) {
			void* StructPtr = Stack.MostRecentPropertyAddress;
			UScriptStruct* scriptStruct = structProperty->Struct;
			if (StructPtr) {
				int x = 0, sourceIndex = 0;
				for (TFieldIterator<FProperty> It(scriptStruct); It; ++It) {
					FProperty* Property = *It;
					if (Property!=nullptr) {
						Props.Emplace(FName(Property->GetAuthoredName().TrimStartAndEnd()));
					}
				}
			}
		}
		*(TArray<FName>*) RESULT_PARAM = Props;

		P_FINISH;
	}
	
/* todo: import text doesn't work, not sure what's wrong.
	///
	//	*  Converts a struct into a csv string.
	//	*  This will be formatted like a full table, but it's only the one row.
	//
	UFUNCTION(BlueprintCallable, CustomThunk, meta=(CustomStructureParam = "Struct"), Category = "DynamicDatatable|Utility")
	static FString StructToCsvString( UPARAM(ref) int32& Struct);

	DECLARE_FUNCTION(execStructToCsvString) {
		Stack.Step(Stack.Object, NULL);
		FStructProperty* structProperty = CastField<FStructProperty>(Stack.MostRecentProperty);
		if (structProperty) {
			void* StructPtr = Stack.MostRecentPropertyAddress;
			UScriptStruct* scriptStruct = structProperty->Struct;
			if (StructPtr) {
				FString res = "", FirstLine = "---", Line = "", str = "";
				UObject* Parent = nullptr;
				FString qsingle = "\"", qdouble = "\"\"";
				int i = 0;
				Line = "CsvStruct";
				for (TFieldIterator<FProperty> It(scriptStruct); It; ++It) {
					FProperty* Property = *It;
					if (Property!=nullptr) {
						if (i==0) {FirstLine += ","+Property->GetAuthoredName().TrimStartAndEnd();}
						for (int32 ArrayIndex = 0; ArrayIndex < Property->ArrayDim; ArrayIndex++) {
							//str = ""; Property->ExportText_Direct(str, Property->ContainerPtrToValuePtr<void>(each.Value), nullptr, Parent, EPropertyPortFlags::PPF_ExternalEditor);
							str = ""; Property->ExportTextItem_Direct(str, Property->ContainerPtrToValuePtr<void>(StructPtr), nullptr, Parent, EPropertyPortFlags::PPF_ExternalEditor);
							Line += ",\""+str.Replace(*qsingle,*qdouble)+"\"";
						}
					}
				}
				if (i==0) {res = FirstLine;}
				res += "\n"+MoveTemp(Line); Line = "";
				++i;
				*(FString*) RESULT_PARAM = MoveTemp(res);
			}
		}
		else {*(FString*) RESULT_PARAM = "";}
		
		P_FINISH;
	}

	/// 
	//	*  Converts a csv string into a struct.
	//	*  This must be formatted like a full table, but it's only the one row.
	///
	UFUNCTION(BlueprintCallable, CustomThunk, meta=(CustomStructureParam = "Struct"), Category = "DynamicDatatable|Utility")
	static void StructFromCsvString(int32& Struct, UPARAM(ref) FString& Csv);
	
	DECLARE_FUNCTION(execStructFromCsvString) {
		Stack.Step(Stack.Object, NULL);
		FStructProperty* structProperty = CastField<FStructProperty>(Stack.MostRecentProperty);
		TArray<FName> Props = {};
		P_GET_PROPERTY_REF(FStrProperty,Csv);
		if (structProperty) {
			void* StructPtr = Stack.MostRecentPropertyAddress;
			UScriptStruct* scriptStruct = structProperty->Struct;
			if (StructPtr&&Csv!="") {
				//TArray<FString> PropNames = {}, PropStrs = {};
				TArray<FString> Lines = {}; Csv.ParseIntoArray(Lines,TEXT("\n"),true);
				if (Lines.Num()>1) {
					FString qsingle = "\"", qdouble = "\"\"", lp = "(", rp = ")";
					FString& firstline = Lines[0];
					FString& secondline = Lines[1];
					PropNames = SplitCsvLine(firstline);
					PropStrs = SplitCsvLine(secondline);
					StructPtr = FMemory::Malloc(scriptStruct->GetStructureSize());
					int idx = 0, pnl = PropNames.Num(), psl = PropStrs.Num();
					for (TFieldIterator<FProperty> It(scriptStruct); It; ++It) {
						FProperty* Property = *It;
						if (Property!=nullptr) {
							idx = PropNames.Find(Property->GetAuthoredName().TrimStartAndEnd());
							if (idx>=0&&idx<psl) {
								auto& propstr = PropStrs[idx];
								//if (propstr[0]=='"') {propstr.RightChopInline(1);}
								//if (propstr[propstr.Len()-1]=='"') {propstr.LeftChopInline(1);}
								//propstr = lp+propstr+rp;
								{FString str2 = propstr; UE_LOG(LogTemp, Log, TEXT("%s"), *str2);}
								//PropStrs[idx] = PropStrs[idx].Replace(*qdouble,*qsingle);
								for (int32 ArrayIndex = 0; ArrayIndex < Property->ArrayDim; ArrayIndex++) {
									//str = ""; Property->ExportText_Direct(str, Property->ContainerPtrToValuePtr<void>(each.Value), nullptr, Parent, EPropertyPortFlags::PPF_ExternalEditor);
									Property->ImportText_Direct(*PropStrs[idx], Property->ContainerPtrToValuePtr<void>(StructPtr, ArrayIndex), EPropertyPortFlags::PPF_ExternalEditor, nullptr);
								}
							}
						}
					}
				}//
				// easy hack
				UDataTable* dt = NewObject<UDataTable>();
				dt->RowStruct = scriptStruct;
				dt->CreateTableFromCSVString(Csv);
				FDynamicTable Table;
				AddRowsFromDataTable(Table,dt);
				FName rowname = TEXT("CsvStruct");
				if (Table.Map.Contains(rowname)) {
					StructPtr = Table.Map[rowname]; //*dt->FindRowUnchecked("CsvStruct");
				}
				dt = nullptr;
			}
		}
		
		P_FINISH;
	}*/

	/** 
		*  Splits a single line of a csv file into an array of strings between the commas.
		*  Ignores commas within quotation marks.
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|Utility")
	static TArray<FString> SplitCsvLine( UPARAM(ref) FString& Line);

	/** 
		*  Appends all the rows in the Dynamic Datatable Asset to the Dynamic Table
		*  Overwrites existing entries that have the same name.
		*  DEPRICATED - can still be used but not recommended, migrate to using regular data table assets
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|Depricated")
	static void AddRowsFromDynamicTableAsset( UPARAM(ref) FDynamicTable& Table, UDynamicDataTable* Source);

	/** 
		*  Gets the table the row originated from.
		*  "Pure" Node: It's a direct lookup of a pointer, so it can be used much more cheaply than getting the row.
	*/
	UFUNCTION(BlueprintPure, Category = "DynamicDatatable|Getting")
	static UDataTable* GetSourceFromRow( UPARAM(ref) FDynamicTable& Table, FName RowName);
	
	/** 
		*  Returns a property via the variable name, as a string.
	*/
	UFUNCTION(BlueprintCallable, CustomThunk, meta=(CustomStructureParam = "Struct", Keywords = "variable"), Category = "DynamicDatatable|Utility")
	static FString GetPropertyFromStructAsString( UPARAM(ref) int32& Struct, FString PropertyName, bool& Success);
	
	DECLARE_FUNCTION(execGetPropertyFromStructAsString) {
		Stack.Step(Stack.Object, NULL);
		FStructProperty* structProperty = CastField<FStructProperty>(Stack.MostRecentProperty);
		if (structProperty) {
			void* StructPtr = Stack.MostRecentPropertyAddress;
			UScriptStruct* scriptStruct = structProperty->Struct;
			P_GET_PROPERTY(FStrProperty, PropertyName);
			P_GET_PROPERTY_REF(FBoolProperty, Success);
			Success = false;
			FString res;
			if (StructPtr) {
				int x = 0, sourceIndex = 0;
				for (TFieldIterator<FProperty> It(scriptStruct); It; ++It) {
					FProperty* Property = *It;
					if (Property!=nullptr) {
						FString pname = PropertyName;
						if (pname.Equals(Property->GetAuthoredName().TrimStartAndEnd(),ESearchCase::IgnoreCase)) {
							Property->ExportTextItem_Direct(res, Property->ContainerPtrToValuePtr<void>(StructPtr), nullptr, nullptr, EPropertyPortFlags::PPF_ExternalEditor);
							Success = true;
							goto finish;
						}
					}
				}
			}
			finish:;
			*(FString*) RESULT_PARAM = res;
		}
		else {
			FString res = "";
			*(FString*) RESULT_PARAM = res;
		}

		P_FINISH;
	}
	
	/** 
		*  Sets a property via the variable name, as a string.
		*  It may be wise to see how GetPropertyFromStruct returns a certain property type before using this.
	*/
	UFUNCTION(BlueprintCallable, CustomThunk, meta=(CustomStructureParam = "Struct", Keywords = "variable"), Category = "DynamicDatatable|Utility")
	static void SetPropertyInStructAsString( UPARAM(ref) int32& Struct, FString PropertyName, UPARAM(ref) FString& Data, bool& Success);
	
	DECLARE_FUNCTION(execSetPropertyInStructAsString) {
		Stack.Step(Stack.Object, NULL);
		FStructProperty* structProperty = CastField<FStructProperty>(Stack.MostRecentProperty);
		if (structProperty) {
			void* StructPtr = Stack.MostRecentPropertyAddress;
			UScriptStruct* scriptStruct = structProperty->Struct;
			P_GET_PROPERTY(FStrProperty, PropertyName);
			P_GET_PROPERTY_REF(FStrProperty, Data);
			P_GET_PROPERTY_REF(FBoolProperty, Success);
			Success = false;
			if (StructPtr) {
				int x = 0, sourceIndex = 0;
				for (TFieldIterator<FProperty> It(scriptStruct); It; ++It) {
					FProperty* Property = *It;
					if (Property!=nullptr) {
						FString pname = PropertyName;
						if (pname.Equals(Property->GetAuthoredName().TrimStartAndEnd(),ESearchCase::IgnoreCase)) {
							FString str = Data;
							Property->ImportText_Direct(*str,Property->ContainerPtrToValuePtr<void>(StructPtr),nullptr,EPropertyPortFlags::PPF_ExternalEditor,nullptr);
							Success = true;
						}
					}
				}
			}
		}

		P_FINISH;
	}

	/** 
		*  Deletes a dyntable file
		*  This will delete any .dyntable file provided, use with care.
		*  Path is a /Game/ relative path, eg. /Game/Saved/SaveGames/ functions like 'Project Saved Dir' return paths like this.
	*/
	UFUNCTION(BlueprintCallable, meta = (WorldContext = "WorldContextObject"), Category = "DynamicDatatable|Utility")
	static bool DeleteTableFile(UObject* WorldContextObject, FString Path, FString FileName);
	
	/** 
		*  Matches a string to the rows of a table, returns all matching row names.
		*  Works as an "OR" filter, if Prefix, Suffix, OR Search match, it gets added to the result array.
	*/
	UFUNCTION(BlueprintCallable, meta = (WorldContext = "WorldContextObject"), Category = "DynamicDatatable|Getting")
	static TArray<FName> SearchTableRows( UPARAM(ref) FDynamicTable& Table, FString Search, FString Prefix, FString Suffix, bool CaseSensitive);

	UFUNCTION(BlueprintPure, BlueprintInternalUseOnly, CustomThunk, meta=(CustomStructureParam = "Value", BlueprintThreadSafe))
	static bool GetTableRowMember(UPARAM(ref) FDynamicTable& Table, FName RowName, FName MemberName, int32& Value);
	
	DECLARE_FUNCTION(execGetTableRowMember) {
		P_GET_STRUCT_REF(FDynamicTable,Table);
		P_GET_PROPERTY(FNameProperty, RowName);
		P_GET_PROPERTY(FNameProperty, MemberName);
		//Stack.MostRecentProperty = nullptr;
		Stack.Step(Stack.Object, nullptr);
		FProperty* ValProp   = Stack.MostRecentProperty;
		void*      ValPtr    = Stack.MostRecentPropertyAddress;
		if (!ValProp) {
			Stack.bArrayContextFailed = true;
			return;
		}

		P_FINISH;
		P_NATIVE_BEGIN;
		bool res = false;
		if (Table.Map.Contains(RowName)) {
			res = true;
			FProperty* Prop = nullptr;
			for (TFieldIterator<FProperty> It(Table.Type); It; ++It) {
				FProperty* Property = *It;
				if (Property->GetAuthoredName().TrimStartAndEnd()==MemberName.ToString()) {
					Prop = Property; break;
				}
			}
			//ValProp->CopyCompleteValue(ValPtr,Prop->ContainerPtrToValuePtr<void>(Table.Map[RowName].GetMutableMemory()));
			CopyISValue(ValProp,ValPtr,Prop->ContainerPtrToValuePtr<void>(Table.Map[RowName].GetMutableMemory()));
		}
		*(bool*)RESULT_PARAM = res;
		P_NATIVE_END;
	}
	
	static void CopyISValue(const FProperty* Prop, void* Dest, const void* Src) {
	    if (!Prop || !Dest || !Src) {return;}
		if (Prop->HasAllPropertyFlags(CPF_IsPlainOldData) && Prop->ArrayDim <= 1) {
	        const SIZE_T NumBytes = SIZE_T(Prop->ElementSize) * SIZE_T(Prop->ArrayDim);
	        FMemory::Memcpy(Dest, Src, NumBytes);
	        return;
	    }
	    Prop->CopyCompleteValue(Dest, Src);
	}

	/** 
		*  Gets the row name at Index
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|Getting")
	static FName TableGetRowNameFromIndex( UPARAM(ref) FDynamicTable& Table, int Index) {
		if (Index>=0&&Index<Table.RowNames.Num()) {
			return Table.RowNames[Index];
		}
		return NAME_None;
	}

	UFUNCTION(BlueprintCallable, BlueprintInternalUseOnly, CustomThunk, meta=(CustomStructureParam = "Value", BlueprintThreadSafe))
	static bool SetTableRowMember(UPARAM(ref) FDynamicTable& Table, FName RowName, FName MemberName,UPARAM(ref) int32& Value);
	
	DECLARE_FUNCTION(execSetTableRowMember) {
		P_GET_STRUCT_REF(FDynamicTable,Table);
		P_GET_PROPERTY(FNameProperty, RowName);
		P_GET_PROPERTY(FNameProperty, MemberName);
		//Stack.MostRecentProperty = nullptr;
		Stack.Step(Stack.Object, nullptr);
		FProperty* ValProp   = Stack.MostRecentProperty;
		void*      ValPtr    = Stack.MostRecentPropertyAddress;
		if (!ValProp) {
			Stack.bArrayContextFailed = true;
			return;
		}

		P_FINISH;
		P_NATIVE_BEGIN;
		bool res = false;
		if (Table.Map.Contains(RowName)) {
			res = true;
			FProperty* Prop = nullptr;
			for (TFieldIterator<FProperty> It(Table.Type); It; ++It) {
				FProperty* Property = *It;
				if (Property->GetAuthoredName().TrimStartAndEnd()==MemberName.ToString()) {
					Prop = Property; break;
				}
			}
			ValProp->CopyCompleteValue(Prop->ContainerPtrToValuePtr<void>(Table.Map[RowName].GetMutableMemory()),ValPtr);
			//CopyISValue(ValProp,ValPtr,Prop->ContainerPtrToValuePtr<void>(Table.Map[RowName].GetMutableMemory()));
		}
		*(bool*)RESULT_PARAM = res;
		P_NATIVE_END;
	}

	UFUNCTION()
	TArray<FString> GetStroctOptions() const {
		TArray<FString> res = {};
		FString ProjectScriptPrefix = FString::Printf(TEXT("/Script/%s"), FApp::GetProjectName());
		for (TObjectIterator<UScriptStruct> It; It; ++It) {
			UScriptStruct* Struct = *It;
			const FString PkgName  = Struct->GetOutermost()->GetName();
			const bool bBlueprintStruct = Struct->IsA(UUserDefinedStruct::StaticClass());
			const bool bNativeInProject = PkgName.StartsWith(ProjectScriptPrefix);
			if (bBlueprintStruct || bNativeInProject) {
				res.AddUnique(Struct->GetName());
			}
		}
		res.Sort();
		return res;
	}
};
