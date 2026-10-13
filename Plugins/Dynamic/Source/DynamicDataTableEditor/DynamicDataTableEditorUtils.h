// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "DynamicDatatable/DynamicDataTable/DynamicDataTable.h"
#include "Kismet2/ListenerManager.h"
#include "Widgets/SWidget.h"
#include "Framework/Commands/UIAction.h"
#include "AssetRegistry/AssetData.h"

struct FDynamicDataTableEditorColumnHeaderData
{
	/** Unique ID used to identify this column */
	FName ColumnId;

	/** Display name of this column */
	FText DisplayName;

	/** The calculated width of this column taking into account the cell data for each row */
	float DesiredColumnWidth;

	/** The FProperty for the variable in this column */
	const FProperty* Property;
};

struct FDynamicDataTableEditorRowListViewData
{
	/** Unique ID used to identify this row */
	FName RowId;

	/** Display name of this row */
	FText DisplayName;

	/** The calculated height of this row taking into account the cell data for each column */
	float DesiredRowHeight;

	/** Insertion number of the row */
	uint32 RowNum;

	/** Array corresponding to each cell in this row */
	TArray<FText> CellData;
};

typedef TSharedPtr<FDynamicDataTableEditorColumnHeaderData> FDynamicDataTableEditorColumnHeaderDataPtr;
typedef TSharedPtr<FDynamicDataTableEditorRowListViewData>  FDynamicDataTableEditorRowListViewDataPtr;


enum class EDynamicDataTableRowInsertionPosition
{
	Above,
	Below,
	Bottom,
};

struct DYNAMICDATATABLEEDITOR_API FDynamicDataTableEditorUtils
{
	enum class EDynamicDataTableChangeInfo
	{
		/** The data corresponding to a single row has been changed */
		RowData,
		/** The data corresponding to the entire list of rows has been changed */
		RowList,
	};

	enum class ERowMoveDirection
	{
		Up,
		Down,
	};

	class FDynamicDataTableEditorManager : public FListenerManager < UDynamicDataTable, EDynamicDataTableChangeInfo >
	{
		FDynamicDataTableEditorManager() {}
	public:
		DYNAMICDATATABLEEDITOR_API static FDynamicDataTableEditorManager& Get();

		class DYNAMICDATATABLEEDITOR_API ListenerType : public InnerListenerType<FDynamicDataTableEditorManager>
		{
		public:
			virtual void SelectionChange(const UDynamicDataTable* DynamicDataTable, FName RowName) { }
		};
	};

	typedef FDynamicDataTableEditorManager::ListenerType INotifyOnDynamicDataTableChanged;

	static bool RemoveRow(UDynamicDataTable* DynamicDataTable, FName Name);
	static uint8* AddRow(UDynamicDataTable* DynamicDataTable, FName RowName);
	static uint8* DuplicateRow(UDynamicDataTable* DynamicDataTable, FName SourceRowName, FName RowName);
	static bool RenameRow(UDynamicDataTable* DynamicDataTable, FName OldName, FName NewName);
	static bool MoveRow(UDynamicDataTable* DynamicDataTable, FName RowName, ERowMoveDirection Direction, int32 NumRowsToMoveBy = 1);
	static bool SelectRow(const UDynamicDataTable* DynamicDataTable, FName RowName);
	static bool DiffersFromDefault(UDynamicDataTable* DynamicDataTable, FName RowName);
	static bool ResetToDefault(UDynamicDataTable* DynamicDataTable, FName RowName);

	static uint8* AddRowAboveOrBelowSelection(UDynamicDataTable* DynamicDataTable, const FName& RowName, const FName& NewRowName, EDynamicDataTableRowInsertionPosition InsertPosition);

	static void BroadcastPreChange(UDynamicDataTable* DynamicDataTable, EDynamicDataTableChangeInfo Info);
	static void BroadcastPostChange(UDynamicDataTable* DynamicDataTable, EDynamicDataTableChangeInfo Info);

	/** Reads a data table and parses out editable copies of rows and columns */
	static void CacheDynamicDataTableForEditing(const UDynamicDataTable* DynamicDataTable, TArray<FDynamicDataTableEditorColumnHeaderDataPtr>& OutAvailableColumns, TArray<FDynamicDataTableEditorRowListViewDataPtr>& OutAvailableRows);

	/** Generic version that works with any DynamicDataTable-like structure */
	static void CacheDataForEditing(const UScriptStruct* RowStruct, const TMap<FName, uint8*>& RowMap, TArray<FDynamicDataTableEditorColumnHeaderDataPtr>& OutAvailableColumns, TArray<FDynamicDataTableEditorRowListViewDataPtr>& OutAvailableRows);

	/** Returns all script structs that can be used as a data table row. This only includes loaded ones */
	static TArray<UScriptStruct*> GetPossibleStructs();

	/** Fills in an array with all possible DynamicDataTable structs, unloaded and loaded */
	static void GetPossibleStructAssetData(TArray<FAssetData>& StructAssets);
	
	/** Utility function which verifies that the specified struct type is viable for data tables */
	static bool IsValidTableStruct(const UScriptStruct* Struct);

	/** Add a UI action for search for references, useful for customizations */
	static void AddSearchForReferencesContextMenu(class FDetailWidgetRow& RowNameDetailWidget, FExecuteAction SearchForReferencesAction);

	/** Short description for a data or curve handle */
	static FText GetHandleShortDescription(const UObject* TableAsset, FName RowName);

	/** Tooltip text for the data table row type */
	static FText GetRowTypeInfoTooltipText(FDynamicDataTableEditorColumnHeaderDataPtr ColumnHeaderDataPtr);

	/** Doc excerpt name for the data table row type */
	static FString GetRowTypeTooltipDocExcerptName(FDynamicDataTableEditorColumnHeaderDataPtr ColumnHeaderDataPtr);

	/** Link to variable type doc  */
	static const FString VariableTypesTooltipDocLink;

	/** Delegate called when a data table struct is selected */
	DECLARE_DELEGATE_OneParam(FOnDynamicDataTableStructSelected, UScriptStruct*);

	/** Creates a combo box that allows selecting from the list of possible row structures */
	static TSharedRef<SWidget> MakeRowStructureComboBox(FOnDynamicDataTableStructSelected OnSelected);
};
