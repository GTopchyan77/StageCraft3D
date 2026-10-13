// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Script.h"
#include "Blueprint/BlueprintExceptionInfo.h"
#include "UObject/ObjectMacros.h"
#include "UObject/UnrealType.h"
#include "UObject/ScriptMacros.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "DynamicDataTable2.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "DynamicDatatable/DynamicDataTable/DynamicDataTable.h"
#include "Editor/DataTableEditor/Public/IDataTableEditor.h"
#include "UObject/Class.h" // for FStructUtils
#include "UObject/SavePackage.h"
#if WITH_EDITOR
#include "Editor/EditorEngine.h"
#include "Editor/DataTableEditor/Private/DataTableEditor.h"
#include "Editor/DataTableEditor/Public/IDataTableEditor.h"
#endif
#include "DynamicDataTableFunctionLibrary.generated.h"

class UCurveTable;

/** Enum used to indicate success or failure of EvaluateCurveTableRow. */
UENUM()
namespace EDynEvaluateCurveTableResult
{
    enum Type
    {
        /** Found the row successfully. */
        RowFound,
        /** Failed to find the row. */
        RowNotFound,
    };
}

UCLASS()
class DYNAMICDATATABLEEDITOR_API UDynamicDataTableFunctionLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_UCLASS_BODY()

	//UFUNCTION(BlueprintCallable, Category = "DynamicDataTable", meta = (ExpandEnumAsExecs="OutResult", DynamicDataTablePin="CurveTable"))
	static void EvaluateCurveTableRow(UCurveTable* CurveTable, FName RowName, float InXY, TEnumAsByte<EDynEvaluateCurveTableResult::Type>& OutResult, float& OutXY,const FString& ContextString);
    
	// Returns whether or not Table contains a row named RowName
  	//UFUNCTION(BlueprintCallable, Category = "DynamicDataTable")
 	static bool DoesDynamicDataTableRowExist(UDynamicDataTable* Table, FName RowName);
    
	//UFUNCTION(BlueprintCallable, Category = "DynamicDataTable")
	static void GetDynamicDataTableRowNames(UDynamicDataTable* Table, TArray<FName>& OutRowNames);

	/** Export from the DynamicDataTable all the row for one column. Export it as string. The row name is not included. */
	//UFUNCTION(BlueprintCallable, Category = "DynamicDataTable")
	static TArray<FString> GetDynamicDataTableColumnAsString(const UDynamicDataTable* DynamicDataTable, FName PropertyName);

    /** Get a Row from a DynamicDataTable given a RowName */
    //UFUNCTION(BlueprintCallable, CustomThunk, Category = "DynamicDataTable", meta=(CustomStructureParam = "OutRow", BlueprintInternalUseOnly="true"))
    static bool GetDynamicDataTableRowFromName(UDynamicDataTable* Table, FName RowName, FDynamicTableRowBase& OutRow);
    
	static bool Generic_GetDynamicDataTableRowFromName(const UDynamicDataTable* Table, FName RowName, void* OutRowPtr);

    /** Based on UDynamicDataTableFunctionLibrary::GetDynamicDataTableRow */
    DECLARE_FUNCTION(execGetDynamicDataTableRowFromName)
    {
        P_GET_OBJECT(UDynamicDataTable, Table);
        P_GET_PROPERTY(FNameProperty, RowName);
        
        Stack.StepCompiledIn<FStructProperty>(NULL);
        void* OutRowPtr = Stack.MostRecentPropertyAddress;

		P_FINISH;
		bool bSuccess = false;
		
		FStructProperty* StructProp = CastField<FStructProperty>(Stack.MostRecentProperty);
		if (!Table)
		{
			FBlueprintExceptionInfo ExceptionInfo = FBlueprintExceptionInfo(
				EBlueprintExceptionType::AccessViolation,
				NSLOCTEXT("GetDynamicDataTableRow", "MissingTableInput", "Failed to resolve the table input. Be sure the DynamicDataTable is valid.")
			);
			FBlueprintCoreDelegates::ThrowScriptException(P_THIS, Stack, ExceptionInfo);
		}
		else if(StructProp && OutRowPtr)
		{
			UScriptStruct* OutputType = StructProp->Struct;
			const UScriptStruct* TableType  = Table->GetRowStruct();
		
			const bool bCompatible = (OutputType == TableType) || 
				(OutputType->IsChildOf(TableType) && FStructUtils::TheSameLayout(OutputType, TableType));
			if (bCompatible)
			{
				P_NATIVE_BEGIN;
				bSuccess = Generic_GetDynamicDataTableRowFromName(Table, RowName, OutRowPtr);
				P_NATIVE_END;
			}
			else
			{
				FBlueprintExceptionInfo ExceptionInfo(
					EBlueprintExceptionType::AccessViolation,
					NSLOCTEXT("GetDynamicDataTableRow", "IncompatibleProperty", "Incompatible output parameter; the data table's type is not the same as the return type.")
					);
				FBlueprintCoreDelegates::ThrowScriptException(P_THIS, Stack, ExceptionInfo);
			}
		}
		else
		{
			FBlueprintExceptionInfo ExceptionInfo(
				EBlueprintExceptionType::AccessViolation,
				NSLOCTEXT("GetDynamicDataTableRow", "MissingOutputProperty", "Failed to resolve the output parameter for GetDynamicDataTableRow.")
			);
			FBlueprintCoreDelegates::ThrowScriptException(P_THIS, Stack, ExceptionInfo);
		}
		*(bool*)RESULT_PARAM = bSuccess;
    }

#if WITH_EDITOR
	/** 
	 * Empty and fill a Data Table from CSV string.
	 * @param	CSVString	The Data that representing the contents of a CSV file.
	 * @return	True if the operation succeeds, check the log for errors if it didn't succeed.
	 */
	//UFUNCTION(BlueprintCallable, Category = "Editor Scripting | DynamicDataTable", DisplayName="Fill Data Table from CSV String")
	static bool FillDynamicDataTableFromCSVString(UDynamicDataTable* DynamicDataTable, const FString& CSVString);

	/** 
	 * Empty and fill a Data Table from CSV file.
	 * @param	CSVFilePath	The file path of the CSV file.
	 * @return	True if the operation succeeds, check the log for errors if it didn't succeed.
	 */
	//UFUNCTION(BlueprintCallable, Category = "Editor Scripting | DynamicDataTable", DisplayName = "Fill Data Table from CSV File")
	static bool FillDynamicDataTableFromCSVFile(UDynamicDataTable* DynamicDataTable, const FString& CSVFilePath);

	/** 
	 * Empty and fill a Data Table from JSON string.
	 * @param	JSONString	The Data that representing the contents of a JSON file.
	 * @return	True if the operation succeeds, check the log for errors if it didn't succeed.
	 */
	//UFUNCTION(BlueprintCallable, Category = "Editor Scripting | DynamicDataTable", DisplayName = "Fill Data Table from JSON String")
	static bool FillDynamicDataTableFromJSONString(UDynamicDataTable* DynamicDataTable, const FString& JSONString);


	/** 
	 * Empty and fill a Data Table from JSON file.
	 * @param	JSONFilePath	The file path of the JSON file.
	 * @return	True if the operation succeeds, check the log for errors if it didn't succeed.
	 */
	//UFUNCTION(BlueprintCallable, Category = "Editor Scripting | DynamicDataTable", DisplayName = "Fill Data Table from JSON File")
	static bool FillDynamicDataTableFromJSONFile(UDynamicDataTable* DynamicDataTable, const FString& JSONFilePath, UScriptStruct* ImportRowStruct = nullptr);
#endif //WITH_EDITOR

	

	/** 
		*  Imports CSV formatted text from a file into an existing (non-dynamic) data table and saves it.
		*  OverwriteTable clears the table before adding rows.
		*  Backup important data before using!
	*/
	UFUNCTION(BlueprintCallable, meta = (WorldContext = "WorldContextObject"), Category = "DynamicDatatable|EDITOR")
	static bool EDITOR_AddCsvFromPathToDataTable(FString CsvFilePath, UDataTable* DataTable, bool OverwriteTable = false, bool DoSave = true) {
		#if WITH_EDITOR
				FString CsvContent;
				if (!FFileHelper::LoadFileToString(CsvContent, *CsvFilePath)) {return false;}
				return EDITOR_AddCsvStringToDataTable(CsvContent,DataTable,OverwriteTable,DoSave);
		#endif
		return false;
	}

	/** 
		*  Imports CSV formatted text from a string into an existing (non-dynamic) data table and saves it.
		*  OverwriteTable clears the table before adding rows.
		*  Backup important data before using!
	*/
	UFUNCTION(BlueprintCallable, meta = (WorldContext = "WorldContextObject"), Category = "DynamicDatatable|EDITOR")
	static bool EDITOR_AddCsvStringToDataTable( UPARAM(ref) FString& CsvString, UDataTable* DataTable, bool OverwriteTable = false, bool DoSave = true) {
		bool res = false;
		#if WITH_EDITOR
			if (!DataTable) {return false;}
			UDataTable* dt = DataTable;
			if (!dt) {return false;}
			
			FDataTableEditorUtils::BroadcastPreChange(dt, FDataTableEditorUtils::EDataTableChangeInfo::RowList);
			dt->Modify(true);
			bool oo = dt->bPreserveExistingValues;
			dt->bPreserveExistingValues = !OverwriteTable;
			auto probs = dt->CreateTableFromCSVString(CsvString);
			for (auto& each : probs) {
				UE_LOG(LogTemp, Log, TEXT("EDITOR Add Csv Node: %s"), *each);
			}
			dt->bPreserveExistingValues = oo;
			dt->HandleDataTableChanged(NAME_None);
			dt->MarkPackageDirty();

			FDataTableEditorUtils::BroadcastPostChange(dt, FDataTableEditorUtils::EDataTableChangeInfo::RowList);
			if (DoSave) {
				UPackage* Package = dt->GetOutermost();
				if (Package) {
					FString PackageFileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());

					FSavePackageArgs SaveArgs;
					SaveArgs.TopLevelFlags       = RF_Public | RF_Standalone | RF_ClassDefaultObject;
					SaveArgs.bWarnOfLongFilename = false;
					SaveArgs.Error               = GError; 

					const bool bSuccess = UPackage::SavePackage(Package, dt, *PackageFileName, SaveArgs);
					//UPackage::SavePackage(Package, dt, RF_Public | RF_Standalone | RF_ClassDefaultObject, *PackageFileName, GError, nullptr, false, false, SAVE_None, nullptr);
					res = true;
				}
			}
			else {res = true;}
		#endif
		return res;
	}
	
};
