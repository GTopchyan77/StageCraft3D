// Copyright Vyrulent 2023 All Rights Reserved.

#pragma once

#include "DynamicDatatableBPLibrary.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Engine/DataTable.h"
#include "DynamicDataTablePluginBPLibraryEditor.generated.h"

//typedef UDynamicDataTablePluginBPLibraryEditor DynamicDatatablePluginEditor;

UCLASS()
class DYNAMICDATATABLEPLUGINEDITOR_API UDynamicDataTablePluginBPLibraryEditor : public UBlueprintFunctionLibrary {
	GENERATED_UCLASS_BODY()
	
	/** 
		*  Do not call directly, right click on a data table in the content browser and use the function under "Scripted Asset Actions"
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|EditorOnly")
	static void CreateEnumFromTableRows_EDITOR( UPARAM(ref) FDynamicTable& Table, FString Path, FString EnumName);

	
	/** 
		*  Do not call directly, right click on a data table in the content browser and use the function under "Scripted Asset Actions"
	*/
	UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|EditorOnly")
	static void CreateEnumFromTableSources_EDITOR( UPARAM(ref) FDynamicTable& Table, FString Path, FString EnumName, bool RemovePrefix, bool RemoveSuffix);

	///** 
	//	*  Saves a copy of the table as a Data Table.
	//*/
	//UFUNCTION(BlueprintCallable, Category = "DynamicDatatable|EditorOnly")
	//static void ConvertDynamicTableAssetToDataTableAsset_EDITOR(UDynamicDataTable* Table, FString Path, FString AssetName);
	
};
