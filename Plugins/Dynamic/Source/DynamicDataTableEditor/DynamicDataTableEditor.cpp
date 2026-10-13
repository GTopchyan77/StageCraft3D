// Copyright Epic Games, Inc. All Rights Reserved.

#include "DynamicDataTableEditor.h"

#include "AssetRegistry/AssetData.h"
#include "Containers/Map.h"
#include "CoreGlobals.h"
#include "DynamicDataTableEditorModule.h"
#include "DynamicDatatable/DynamicDataTable/DynamicDataTableUtils.h"
#include "DetailsViewArgs.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "DynamicDatatable/DynamicDataTable/DynamicDataTable.h"
#include "StructUtils/UserDefinedStruct.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Commands/GenericCommands.h"
#include "Framework/Commands/UIAction.h"
#include "Framework/Commands/UICommandList.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Layout/Overscroll.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/MultiBox/MultiBoxExtender.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Framework/Text/TextLayout.h"
#include "Framework/Views/ITypedTableView.h"
#include "HAL/PlatformApplicationMisc.h"
#include "IDetailsView.h"
#include "IDocumentation.h"
#include "Internationalization/Internationalization.h"
#include "Layout/BasicLayoutWidgetSlot.h"
#include "Layout/Margin.h"
#include "Layout/Visibility.h"
#include "Math/ColorList.h"
#include "Math/UnrealMathSSE.h"
#include "Math/Vector2D.h"
#include "Misc/AssertionMacros.h"
#include "Misc/Attribute.h"
#include "Misc/FeedbackContext.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "PropertyEditorModule.h"
#include "Rendering/SlateRenderer.h"
#include "SDynamicDataTableListViewRow.h"
#include "SRowEditor.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SlotBase.h"
#include "SourceCodeNavigation.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateTypes.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Templates/Casts.h"
#include "Templates/TypeHash.h"
#include "Textures/SlateIcon.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "UObject/Class.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ObjectPtr.h"
#include "UObject/PropertyPortFlags.h"
#include "UObject/TopLevelAssetPath.h"
#include "UObject/UObjectBaseUtility.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SHyperlink.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBar.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SToolTip.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"

class ITableRow;
class STableViewBase;
class SWidget;

#define LOCTEXT_NAMESPACE "DynamicDataTableEditor"

const FName FDynamicDataTableEditor::DynamicDataTableTabId("DynamicDataTableEditor_DynamicDataTable");
const FName FDynamicDataTableEditor::DynamicDataTableDetailsTabId("DynamicDataTableEditor_DynamicDataTableDetails");
const FName FDynamicDataTableEditor::RowEditorTabId("DynamicDataTableEditor_RowEditor");
const FName FDynamicDataTableEditor::RowNameColumnId("RowName");
const FName FDynamicDataTableEditor::RowNumberColumnId("RowNumber");
const FName FDynamicDataTableEditor::RowDragDropColumnId("RowDragDrop");

class SDynamicDataTableModeSeparator : public SBorder
{
public:
	SLATE_BEGIN_ARGS(SDynamicDataTableModeSeparator) {}
	SLATE_END_ARGS()

		void Construct(const FArguments& InArg)
	{
		SBorder::Construct(
			SBorder::FArguments()
			.BorderImage(FAppStyle::GetBrush("BlueprintEditor.PipelineSeparator"))
			.Padding(0.0f)
		);
	}

	// SWidget interface
	virtual FVector2D ComputeDesiredSize(float) const override
	{
		const float Height = 20.0f;
		const float Thickness = 16.0f;
		return FVector2D(Thickness, Height);
	}
	// End of SWidget interface
};


void FDynamicDataTableEditor::RegisterTabSpawners(const TSharedRef<class FTabManager>& InTabManager)
{
	WorkspaceMenuCategory = InTabManager->AddLocalWorkspaceMenuCategory(LOCTEXT("WorkspaceMenu_Data Table Editor", "Data Table Editor"));

	FAssetEditorToolkit::RegisterTabSpawners(InTabManager);

	CreateAndRegisterDynamicDataTableTab(InTabManager);
	CreateAndRegisterDynamicDataTableDetailsTab(InTabManager);
	CreateAndRegisterRowEditorTab(InTabManager);
}

void FDynamicDataTableEditor::UnregisterTabSpawners(const TSharedRef<class FTabManager>& InTabManager)
{
	FAssetEditorToolkit::UnregisterTabSpawners(InTabManager);

	InTabManager->UnregisterTabSpawner(DynamicDataTableTabId);
	InTabManager->UnregisterTabSpawner(DynamicDataTableDetailsTabId);
	InTabManager->UnregisterTabSpawner(RowEditorTabId);

	DynamicDataTableTabWidget.Reset();
	RowEditorTabWidget.Reset();
}

void FDynamicDataTableEditor::CreateAndRegisterDynamicDataTableTab(const TSharedRef<class FTabManager>& InTabManager)
{
	DynamicDataTableTabWidget = CreateContentBox();

	InTabManager->RegisterTabSpawner(DynamicDataTableTabId, FOnSpawnTab::CreateSP(this, &FDynamicDataTableEditor::SpawnTab_DynamicDataTable))
		.SetDisplayName(LOCTEXT("DynamicDataTableTab", "Data Table"))
		.SetGroup(WorkspaceMenuCategory.ToSharedRef());
}

void FDynamicDataTableEditor::CreateAndRegisterDynamicDataTableDetailsTab(const TSharedRef<class FTabManager>& InTabManager)
{
	FPropertyEditorModule & EditModule = FModuleManager::Get().GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
	FDetailsViewArgs DetailsViewArgs;
	DetailsViewArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;
	DetailsViewArgs.bHideSelectionTip = true;
	PropertyView = EditModule.CreateDetailView(DetailsViewArgs);

	InTabManager->RegisterTabSpawner(DynamicDataTableDetailsTabId, FOnSpawnTab::CreateSP(this, &FDynamicDataTableEditor::SpawnTab_DynamicDataTableDetails))
		.SetDisplayName(LOCTEXT("DynamicDataTableDetailsTab", "Data Table Details"))
		.SetGroup(WorkspaceMenuCategory.ToSharedRef());
}

void FDynamicDataTableEditor::CreateAndRegisterRowEditorTab(const TSharedRef<class FTabManager>& InTabManager)
{
	RowEditorTabWidget = CreateRowEditorBox();

	InTabManager->RegisterTabSpawner(RowEditorTabId, FOnSpawnTab::CreateSP(this, &FDynamicDataTableEditor::SpawnTab_RowEditor))
		.SetDisplayName(LOCTEXT("RowEditorTab", "Row Editor"))
		.SetGroup(WorkspaceMenuCategory.ToSharedRef());
}

FDynamicDataTableEditor::FDynamicDataTableEditor()
	: RowNameColumnWidth(0)
	, RowNumberColumnWidth(0)
	, HighlightedVisibleRowIndex(INDEX_NONE)
	, SortMode(EColumnSortMode::Ascending)
{
}

FDynamicDataTableEditor::~FDynamicDataTableEditor()
{
	GEditor->UnregisterForUndo(this);

	UDynamicDataTable* Table = GetEditableDynamicDataTable();
	if (Table)
	{
		SaveLayoutData();
	}
}

void FDynamicDataTableEditor::PostUndo(bool bSuccess)
{
	HandleUndoRedo();
}

void FDynamicDataTableEditor::PostRedo(bool bSuccess)
{
	HandleUndoRedo();
}

void FDynamicDataTableEditor::HandleUndoRedo()
{
	const UDynamicDataTable* Table = GetDynamicDataTable();
	if (Table)
	{
		HandlePostChange();
		CallbackOnDynamicDataTableUndoRedo.ExecuteIfBound();
	}
}

void FDynamicDataTableEditor::PreChange(const class UUserDefinedStruct* Struct, FStructureEditorUtils::EStructureEditorChangeInfo Info)
{
}

void FDynamicDataTableEditor::PostChange(const class UUserDefinedStruct* Struct, FStructureEditorUtils::EStructureEditorChangeInfo Info)
{
	const UDynamicDataTable* Table = GetDynamicDataTable();
	if (Struct && Table && (Table->GetRowStruct() == Struct))
	{
		HandlePostChange();
	}
}

void FDynamicDataTableEditor::SelectionChange(const UDynamicDataTable* Changed, FName RowName)
{
	const UDynamicDataTable* Table = GetDynamicDataTable();
	if (Changed == Table)
	{
		const bool bSelectionChanged = HighlightedRowName != RowName;
		SetHighlightedRow(RowName);

		if (bSelectionChanged)
		{
			CallbackOnRowHighlighted.ExecuteIfBound(HighlightedRowName);
		}
	}
}

void FDynamicDataTableEditor::PreChange(const UDynamicDataTable* Changed, FDynamicDataTableEditorUtils::EDynamicDataTableChangeInfo Info)
{
}

void FDynamicDataTableEditor::PostChange(const UDynamicDataTable* Changed, FDynamicDataTableEditorUtils::EDynamicDataTableChangeInfo Info)
{
	UDynamicDataTable* Table = GetEditableDynamicDataTable();
	if (Changed == Table)
	{
		// Don't need to notify the DynamicDataTable about changes, that's handled before this
		HandlePostChange();
	}
}

const UDynamicDataTable* FDynamicDataTableEditor::GetDynamicDataTable() const
{
	return Cast<const UDynamicDataTable>(GetEditingObject());
}

void FDynamicDataTableEditor::HandlePostChange()
{
	RefreshDynTable();
	// We need to cache and restore the selection here as RefreshCachedDynamicDataTable will re-create the list view items
	const FName CachedSelection = HighlightedRowName;
	HighlightedRowName = NAME_None;
	RefreshCachedDynamicDataTable(CachedSelection, true/*bUpdateEvenIfValid*/);
}

void FDynamicDataTableEditor::InitDynamicDataTableEditor( const EToolkitMode::Type Mode, const TSharedPtr< class IToolkitHost >& InitToolkitHost, UDynamicDataTable* Table )
{
	TSharedRef<FTabManager::FLayout> StandaloneDefaultLayout = FTabManager::NewLayout( "Standalone_DynamicDataTableEditor_Layout_v6" )
	->AddArea
	(
		FTabManager::NewPrimaryArea()->SetOrientation(Orient_Vertical)
		->Split
		(
			FTabManager::NewStack()
			->AddTab(DynamicDataTableTabId, ETabState::OpenedTab)
			->AddTab(DynamicDataTableDetailsTabId, ETabState::OpenedTab)
			->SetForegroundTab(DynamicDataTableTabId)
		)
		->Split
		(
			FTabManager::NewStack()
			->AddTab(RowEditorTabId, ETabState::OpenedTab)
		)
	);

	const bool bCreateDefaultStandaloneMenu = true;
	const bool bCreateDefaultToolbar = true;
	FAssetEditorToolkit::InitAssetEditor( Mode, InitToolkitHost, FDynamicDataTableEditorModule::DynamicDataTableEditorAppIdentifier, StandaloneDefaultLayout, bCreateDefaultStandaloneMenu, bCreateDefaultToolbar, Table );

	FDynamicDataTableEditorModule& DynamicDataTableEditorModule = FModuleManager::LoadModuleChecked<FDynamicDataTableEditorModule>( "DynamicDataTableEditor" );
	AddMenuExtender(DynamicDataTableEditorModule.GetMenuExtensibilityManager()->GetAllExtenders(GetToolkitCommands(), GetEditingObjects()));
	
	TSharedPtr<FExtender> ToolbarExtender = DynamicDataTableEditorModule.GetToolBarExtensibilityManager()->GetAllExtenders(GetToolkitCommands(), GetEditingObjects());
	ExtendToolbar(ToolbarExtender);
	
	AddToolbarExtender(ToolbarExtender);

	RegenerateMenusAndToolbars();

	// Support undo/redo
	GEditor->RegisterForUndo(this);
	RefreshDynTable();
	
	// @todo toolkit world centric editing
	/*// Setup our tool's layout
	if( IsWorldCentricAssetEditor() )
	{
		const FString TabInitializationPayload(TEXT(""));		// NOTE: Payload not currently used for table properties
		SpawnToolkitTab( DynamicDataTableTabId, TabInitializationPayload, EToolkitTabSpot::Details );
	}*/

	// asset editor commands here
	ToolkitCommands->MapAction(FGenericCommands::Get().Copy, FExecuteAction::CreateSP(this, &FDynamicDataTableEditor::CopySelectedRow), FCanExecuteAction::CreateSP(this, &FDynamicDataTableEditor::CanEditTable));
	ToolkitCommands->MapAction(FGenericCommands::Get().Paste, FExecuteAction::CreateSP(this, &FDynamicDataTableEditor::PasteOnSelectedRow), FCanExecuteAction::CreateSP(this, &FDynamicDataTableEditor::CanEditTable));
	ToolkitCommands->MapAction(FGenericCommands::Get().Duplicate, FExecuteAction::CreateSP(this, &FDynamicDataTableEditor::DuplicateSelectedRow), FCanExecuteAction::CreateSP(this, &FDynamicDataTableEditor::CanEditTable));
	ToolkitCommands->MapAction(FGenericCommands::Get().Rename, FExecuteAction::CreateSP(this, &FDynamicDataTableEditor::RenameSelectedRowCommand), FCanExecuteAction::CreateSP(this, &FDynamicDataTableEditor::CanEditTable));
	ToolkitCommands->MapAction(FGenericCommands::Get().Delete, FExecuteAction::CreateSP(this, &FDynamicDataTableEditor::DeleteSelectedRow), FCanExecuteAction::CreateSP(this, &FDynamicDataTableEditor::CanEditTable));
}

bool FDynamicDataTableEditor::CanEditRows() const
{
	return true;
}

FName FDynamicDataTableEditor::GetToolkitFName() const
{
	return FName("DynamicDataTableEditor");
}

FString FDynamicDataTableEditor::GetDocumentationLink() const
{
	return FString(TEXT("Gameplay/DataDriven"));
}

void FDynamicDataTableEditor::OnAddClicked()
{
	UDynamicDataTable* Table = GetEditableDynamicDataTable();

	if (Table)
	{		
		FName NewName = DynamicDataTableUtils::MakeValidName(TEXT("NewRow"));
		while (Table->GetRowMap().Contains(NewName))
		{
			NewName.SetNumber(NewName.GetNumber() + 1);
		}

		FDynamicDataTableEditorUtils::AddRow(Table, NewName);
		FDynamicDataTableEditorUtils::SelectRow(Table, NewName);

		SetDefaultSort();
	}
}

void FDynamicDataTableEditor::OnRemoveClicked()
{
	DeleteSelectedRow();
}

FReply FDynamicDataTableEditor::OnMoveRowClicked(FDynamicDataTableEditorUtils::ERowMoveDirection MoveDirection)
{
	UDynamicDataTable* Table = GetEditableDynamicDataTable();

	if (Table)
	{
		FDynamicDataTableEditorUtils::MoveRow(Table, HighlightedRowName, MoveDirection);
	}
	return FReply::Handled();
}

FReply FDynamicDataTableEditor::OnMoveToExtentClicked(FDynamicDataTableEditorUtils::ERowMoveDirection MoveDirection)
{
	UDynamicDataTable* Table = GetEditableDynamicDataTable();

	if (Table)
	{
		// We move by the row map size, as FDynamicDataTableEditorUtils::MoveRow will automatically clamp this as appropriate
		FDynamicDataTableEditorUtils::MoveRow(Table, HighlightedRowName, MoveDirection, Table->GetRowMap().Num());
		FDynamicDataTableEditorUtils::SelectRow(Table, HighlightedRowName);

		SetDefaultSort();
	}
	return FReply::Handled();
}

void FDynamicDataTableEditor::OnCopyClicked()
{
	UDynamicDataTable* Table = GetEditableDynamicDataTable();
	if (Table)
	{
		CopySelectedRow();
	}
}

void FDynamicDataTableEditor::OnPasteClicked()
{
	UDynamicDataTable* Table = GetEditableDynamicDataTable();
	if (Table)
	{
		PasteOnSelectedRow();
	}
}

void FDynamicDataTableEditor::OnDuplicateClicked()
{
	UDynamicDataTable* Table = GetEditableDynamicDataTable();
	if (Table)
	{
		DuplicateSelectedRow();
	}
}

bool FDynamicDataTableEditor::CanEditTable() const
{
	return HighlightedRowName != NAME_None;
}

void FDynamicDataTableEditor::SetDefaultSort()
{
	SortMode = EColumnSortMode::Ascending;
	SortByColumn = FDynamicDataTableEditor::RowNumberColumnId;
}

EColumnSortMode::Type FDynamicDataTableEditor::GetColumnSortMode(const FName ColumnId) const
{
	if (SortByColumn != ColumnId)
	{
		return EColumnSortMode::None;
	}

	return SortMode;
}

void FDynamicDataTableEditor::OnColumnSortModeChanged(const EColumnSortPriority::Type SortPriority, const FName& ColumnId, const EColumnSortMode::Type InSortMode)
{
	int32 ColumnIndex;

	SortMode = InSortMode;
	SortByColumn = ColumnId;

	for (ColumnIndex = 0; ColumnIndex < AvailableColumns.Num(); ++ColumnIndex)
	{
		if (AvailableColumns[ColumnIndex]->ColumnId == ColumnId)
		{
			break;
		}
	}

	if (AvailableColumns.IsValidIndex(ColumnIndex))
	{
		if (InSortMode == EColumnSortMode::Ascending)
		{
			VisibleRows.Sort([ColumnIndex](const FDynamicDataTableEditorRowListViewDataPtr& first, const FDynamicDataTableEditorRowListViewDataPtr& second)
			{					
				int32 Result = (first->CellData[ColumnIndex].ToString()).Compare(second->CellData[ColumnIndex].ToString());

				if (!Result)
				{
					return first->RowNum < second->RowNum;

				}

				return Result < 0;
			});

		}
		else if (InSortMode == EColumnSortMode::Descending)
		{
			VisibleRows.Sort([ColumnIndex](const FDynamicDataTableEditorRowListViewDataPtr& first, const FDynamicDataTableEditorRowListViewDataPtr& second)
			{
				int32 Result = (first->CellData[ColumnIndex].ToString()).Compare(second->CellData[ColumnIndex].ToString());

				if (!Result)
				{
					return first->RowNum > second->RowNum;
				}

				return Result > 0;
			});
		}
	}

	CellsListView->RequestListRefresh();
}

void FDynamicDataTableEditor::OnColumnNumberSortModeChanged(const EColumnSortPriority::Type SortPriority, const FName& ColumnId, const EColumnSortMode::Type InSortMode)
{
	SortMode = InSortMode;
	SortByColumn = ColumnId;

	if (InSortMode == EColumnSortMode::Ascending)
	{
		VisibleRows.Sort([](const FDynamicDataTableEditorRowListViewDataPtr& first, const FDynamicDataTableEditorRowListViewDataPtr& second)
		{
			return first->RowNum < second->RowNum;
		});
	}
	else if (InSortMode == EColumnSortMode::Descending)
	{
		VisibleRows.Sort([](const FDynamicDataTableEditorRowListViewDataPtr& first, const FDynamicDataTableEditorRowListViewDataPtr& second)
		{
			return first->RowNum > second->RowNum;
		});
	}

	CellsListView->RequestListRefresh();
}

void FDynamicDataTableEditor::OnColumnNameSortModeChanged(const EColumnSortPriority::Type SortPriority, const FName& ColumnId, const EColumnSortMode::Type InSortMode)
{
	SortMode = InSortMode;
	SortByColumn = ColumnId;

	if (InSortMode == EColumnSortMode::Ascending)
	{
		VisibleRows.Sort([](const FDynamicDataTableEditorRowListViewDataPtr& first, const FDynamicDataTableEditorRowListViewDataPtr& second)
		{
			return (first->DisplayName).ToString() < (second->DisplayName).ToString();
		});
	}
	else if (InSortMode == EColumnSortMode::Descending)
	{
		VisibleRows.Sort([](const FDynamicDataTableEditorRowListViewDataPtr& first, const FDynamicDataTableEditorRowListViewDataPtr& second)
		{
			return (first->DisplayName).ToString() > (second->DisplayName).ToString();
		});
	}

	CellsListView->RequestListRefresh();
}

void FDynamicDataTableEditor::OnEditDynamicDataTableStructClicked()
{
	const UDynamicDataTable* DynamicDataTable = GetDynamicDataTable();
	if (DynamicDataTable)
	{
		const UScriptStruct* ScriptStruct = DynamicDataTable->GetRowStruct();

		if (ScriptStruct)
		{
			GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(ScriptStruct->GetPathName());
			if (FSourceCodeNavigation::CanNavigateToStruct(ScriptStruct))
			{
				FSourceCodeNavigation::NavigateToStruct(ScriptStruct);
			}
		}
	}
}

void FDynamicDataTableEditor::ExtendToolbar(TSharedPtr<FExtender> Extender)
{
	Extender->AddToolBarExtension(
		"Asset",
		EExtensionHook::After,
		GetToolkitCommands(),
		FToolBarExtensionDelegate::CreateSP(this, &FDynamicDataTableEditor::FillToolbar)
	);

}

void FDynamicDataTableEditor::FillToolbar(FToolBarBuilder& ToolbarBuilder)
{
	ToolbarBuilder.BeginSection("DynamicDataTableCommands");
	{
		ToolbarBuilder.AddToolBarButton(
			FUIAction(
				FExecuteAction::CreateSP(this, &FDynamicDataTableEditor::Reimport_Execute),
				FCanExecuteAction::CreateSP(this, &FDynamicDataTableEditor::CanReimport)),
			NAME_None,
			LOCTEXT("ReimportText", "Reimport"),
			LOCTEXT("ReimportTooltip", "Reimport this DynamicDataTable"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Import"));

		ToolbarBuilder.AddSeparator();

		ToolbarBuilder.AddToolBarButton(
			FUIAction(FExecuteAction::CreateSP(this, &FDynamicDataTableEditor::OnAddClicked)),
			NAME_None,
			LOCTEXT("AddIconText", "Add"),
			LOCTEXT("AddRowToolTip", "Add a new row to the Data Table"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Plus"));
		ToolbarBuilder.AddToolBarButton(
			FUIAction(
				FExecuteAction::CreateSP(this, &FDynamicDataTableEditor::OnCopyClicked),
				FCanExecuteAction::CreateSP(this, &FDynamicDataTableEditor::CanEditTable)),
			NAME_None,
			LOCTEXT("CopyIconText", "Copy"),
			LOCTEXT("CopyToolTip", "Copy the currently selected row"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Copy"));
		ToolbarBuilder.AddToolBarButton(
			FUIAction(
				FExecuteAction::CreateSP(this, &FDynamicDataTableEditor::OnPasteClicked),
				FCanExecuteAction::CreateSP(this, &FDynamicDataTableEditor::CanEditTable)),
			NAME_None,
			LOCTEXT("PasteIconText", "Paste"),
			LOCTEXT("PasteToolTip", "Paste on the currently selected row"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Paste"));
		ToolbarBuilder.AddToolBarButton(
			FUIAction(
				FExecuteAction::CreateSP(this, &FDynamicDataTableEditor::OnDuplicateClicked),
				FCanExecuteAction::CreateSP(this, &FDynamicDataTableEditor::CanEditTable)),
			NAME_None,
			LOCTEXT("DuplicateIconText", "Duplicate"),
			LOCTEXT("DuplicateToolTip", "Duplicate the currently selected row"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Duplicate"));
		ToolbarBuilder.AddToolBarButton(
			FUIAction(
				FExecuteAction::CreateSP(this, &FDynamicDataTableEditor::OnRemoveClicked),
				FCanExecuteAction::CreateSP(this, &FDynamicDataTableEditor::CanEditTable)),
			NAME_None,
			LOCTEXT("RemoveRowIconText", "Remove"),
			LOCTEXT("RemoveRowToolTip", "Remove the currently selected row from the Data Table"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Delete"));
	}
	ToolbarBuilder.EndSection();

}

UDynamicDataTable* FDynamicDataTableEditor::GetEditableDynamicDataTable() const
{
	return Cast<UDynamicDataTable>(GetEditingObject());
}

FText FDynamicDataTableEditor::GetBaseToolkitName() const
{
	return LOCTEXT( "AppLabel", "DynamicDataTable Editor" );
}

FString FDynamicDataTableEditor::GetWorldCentricTabPrefix() const
{
	return LOCTEXT("WorldCentricTabPrefix", "DynamicDataTable ").ToString();
}

FLinearColor FDynamicDataTableEditor::GetWorldCentricTabColorScale() const
{
	return FLinearColor( 0.0f, 0.0f, 0.2f, 0.5f );
}

FSlateColor FDynamicDataTableEditor::GetRowTextColor(FName RowName) const
{
	if (RowName == HighlightedRowName)
	{
		return FSlateColor(FColorList::Orange);
	}
	return FSlateColor::UseForeground();
}

FText FDynamicDataTableEditor::GetCellText(FDynamicDataTableEditorRowListViewDataPtr InRowDataPointer, int32 ColumnIndex) const
{
	if (InRowDataPointer.IsValid() && ColumnIndex < InRowDataPointer->CellData.Num())
	{
		return InRowDataPointer->CellData[ColumnIndex];
	}

	return FText();
}

FText FDynamicDataTableEditor::GetCellToolTipText(FDynamicDataTableEditorRowListViewDataPtr InRowDataPointer, int32 ColumnIndex) const
{
	FText TooltipText;

	if (ColumnIndex < AvailableColumns.Num())
	{
		TooltipText = AvailableColumns[ColumnIndex]->DisplayName;
	}

	if (InRowDataPointer.IsValid() && ColumnIndex < InRowDataPointer->CellData.Num())
	{
		TooltipText = FText::Format(LOCTEXT("ColumnRowNameFmt", "{0}: {1}"), TooltipText, InRowDataPointer->CellData[ColumnIndex]);
	}

	return TooltipText;
}

float FDynamicDataTableEditor::GetRowNumberColumnWidth() const
{
	return RowNumberColumnWidth;
}

void FDynamicDataTableEditor::RefreshRowNumberColumnWidth()
{

	TSharedRef<FSlateFontMeasure> FontMeasure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
	const FTextBlockStyle& CellTextStyle = FAppStyle::GetWidgetStyle<FTextBlockStyle>("DataTableEditor.CellText");
	const float CellPadding = 10.0f;

	for (const FDynamicDataTableEditorRowListViewDataPtr& RowData : AvailableRows)
	{
		const float RowNumberWidth = (float)FontMeasure->Measure(FString::FromInt(RowData->RowNum), CellTextStyle.Font).X + CellPadding;
		RowNumberColumnWidth = FMath::Max(RowNumberColumnWidth, RowNumberWidth);
	}

}

void FDynamicDataTableEditor::OnRowNumberColumnResized(const float NewWidth)
{
	RowNumberColumnWidth = NewWidth;
}

float FDynamicDataTableEditor::GetRowNameColumnWidth() const
{
	return RowNameColumnWidth;
}

void FDynamicDataTableEditor::RefreshRowNameColumnWidth()
{
	
	TSharedRef<FSlateFontMeasure> FontMeasure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
	const FTextBlockStyle& CellTextStyle = FAppStyle::GetWidgetStyle<FTextBlockStyle>("DataTableEditor.CellText");
	static const float CellPadding = 10.0f;

	for (const FDynamicDataTableEditorRowListViewDataPtr& RowData : AvailableRows)
	{
		const float RowNameWidth = (float)FontMeasure->Measure(RowData->DisplayName, CellTextStyle.Font).X + CellPadding;
		RowNameColumnWidth = FMath::Max(RowNameColumnWidth, RowNameWidth);
	}
	
}

float FDynamicDataTableEditor::GetColumnWidth(const int32 ColumnIndex) const
{
	if (ColumnWidths.IsValidIndex(ColumnIndex))
	{
		return ColumnWidths[ColumnIndex].CurrentWidth;
	}
	return 0.0f;
}

void FDynamicDataTableEditor::OnColumnResized(const float NewWidth, const int32 ColumnIndex)
{
	if (ColumnWidths.IsValidIndex(ColumnIndex))
	{
		FColumnWidth& ColumnWidth = ColumnWidths[ColumnIndex];
		ColumnWidth.bIsAutoSized = false;
		ColumnWidth.CurrentWidth = NewWidth;

		// Update the persistent column widths in the layout data
		{
			if (!LayoutData.IsValid())
			{
				LayoutData = MakeShareable(new FJsonObject());
			}

			TSharedPtr<FJsonObject> LayoutColumnWidths;
			if (!LayoutData->HasField(TEXT("ColumnWidths")))
			{
				LayoutColumnWidths = MakeShareable(new FJsonObject());
				LayoutData->SetObjectField(TEXT("ColumnWidths"), LayoutColumnWidths);
			}
			else
			{
				LayoutColumnWidths = LayoutData->GetObjectField(TEXT("ColumnWidths"));
			}

			const FString& ColumnName = AvailableColumns[ColumnIndex]->ColumnId.ToString();
			LayoutColumnWidths->SetNumberField(ColumnName, NewWidth);
		}
	}
}

void FDynamicDataTableEditor::OnRowNameColumnResized(const float NewWidth)
{
	RowNameColumnWidth = NewWidth;
}

void FDynamicDataTableEditor::LoadLayoutData()
{
	LayoutData.Reset();

	const UDynamicDataTable* Table = GetDynamicDataTable();
	if (!Table)
	{
		return;
	}

	const FString LayoutDataFilename = FPaths::ProjectSavedDir() / TEXT("AssetData") / TEXT("DynamicDataTableEditorLayout") / Table->GetName() + TEXT(".json");

	FString JsonText;
	if (FFileHelper::LoadFileToString(JsonText, *LayoutDataFilename))
	{
		TSharedRef< TJsonReader<TCHAR> > JsonReader = TJsonReaderFactory<TCHAR>::Create(JsonText);
		FJsonSerializer::Deserialize(JsonReader, LayoutData);
	}
}

void FDynamicDataTableEditor::SaveLayoutData()
{
	const UDynamicDataTable* Table = GetDynamicDataTable();
	if (!Table || !LayoutData.IsValid())
	{
		return;
	}

	const FString LayoutDataFilename = FPaths::ProjectSavedDir() / TEXT("AssetData") / TEXT("DynamicDataTableEditorLayout") / Table->GetName() + TEXT(".json");

	FString JsonText;
	TSharedRef< TJsonWriter< TCHAR, TPrettyJsonPrintPolicy<TCHAR> > > JsonWriter = TJsonWriterFactory< TCHAR, TPrettyJsonPrintPolicy<TCHAR> >::Create(&JsonText);
	if (FJsonSerializer::Serialize(LayoutData.ToSharedRef(), JsonWriter))
	{
		FFileHelper::SaveStringToFile(JsonText, *LayoutDataFilename);
	}
}

TSharedRef<ITableRow> FDynamicDataTableEditor::MakeRowWidget(FDynamicDataTableEditorRowListViewDataPtr InRowDataPtr, const TSharedRef<STableViewBase>& OwnerTable)
{
	return
		SNew(SDynamicDataTableListViewRow, OwnerTable)
		.DynamicDataTableEditor(SharedThis(this))
		.RowDataPtr(InRowDataPtr)
		.IsEditable(CanEditRows());
}

TSharedRef<SWidget> FDynamicDataTableEditor::MakeCellWidget(FDynamicDataTableEditorRowListViewDataPtr InRowDataPtr, const int32 InRowIndex, const FName& InColumnId)
{
	int32 ColumnIndex = 0;
	for (; ColumnIndex < AvailableColumns.Num(); ++ColumnIndex)
	{
		const FDynamicDataTableEditorColumnHeaderDataPtr& ColumnData = AvailableColumns[ColumnIndex];
		if (ColumnData->ColumnId == InColumnId)
		{
			break;
		}
	}

	// Valid column ID?
	if (AvailableColumns.IsValidIndex(ColumnIndex) && InRowDataPtr->CellData.IsValidIndex(ColumnIndex))
	{
		return SNew(SBox)
			.Padding(FMargin(4, 2, 4, 2))
			[
				SNew(STextBlock)
				.TextStyle(FAppStyle::Get(), "DataTableEditor.CellText")
				.ColorAndOpacity(this, &FDynamicDataTableEditor::GetRowTextColor, InRowDataPtr->RowId)
				.Text(this, &FDynamicDataTableEditor::GetCellText, InRowDataPtr, ColumnIndex)
				.HighlightText(this, &FDynamicDataTableEditor::GetFilterText)
				.ToolTipText(this, &FDynamicDataTableEditor::GetCellToolTipText, InRowDataPtr, ColumnIndex)
			];
	}

	return SNullWidget::NullWidget;
}

void FDynamicDataTableEditor::OnRowSelectionChanged(FDynamicDataTableEditorRowListViewDataPtr InNewSelection, ESelectInfo::Type InSelectInfo)
{
	const bool bSelectionChanged = !InNewSelection.IsValid() || InNewSelection->RowId != HighlightedRowName;
	const FName NewRowName = (InNewSelection.IsValid()) ? InNewSelection->RowId : NAME_None;

	SetHighlightedRow(NewRowName);

	if (bSelectionChanged)
	{
		CallbackOnRowHighlighted.ExecuteIfBound(HighlightedRowName);
	}
}

void FDynamicDataTableEditor::CopySelectedRow()
{
	UDynamicDataTable* TablePtr = Cast<UDynamicDataTable>(GetEditingObject());
	uint8* RowPtr = TablePtr ? TablePtr->GetRowMap().FindRef(HighlightedRowName) : nullptr;

	if (!RowPtr || !TablePtr->RowStruct)
		return;

	FString ClipboardValue;
	TablePtr->RowStruct->ExportText(ClipboardValue, RowPtr, RowPtr, TablePtr, PPF_Copy, nullptr);

	FPlatformApplicationMisc::ClipboardCopy(*ClipboardValue);
}

void FDynamicDataTableEditor::PasteOnSelectedRow()
{
	UDynamicDataTable* TablePtr = Cast<UDynamicDataTable>(GetEditingObject());
	uint8* RowPtr = TablePtr ? TablePtr->GetRowMap().FindRef(HighlightedRowName) : nullptr;

	if (!RowPtr || !TablePtr->RowStruct)
		return;

	const FScopedTransaction Transaction(LOCTEXT("PasteDynamicDataTableRow", "Paste Data Table Row"));
	TablePtr->Modify();

	FString ClipboardValue;
	FPlatformApplicationMisc::ClipboardPaste(ClipboardValue);

	FDynamicDataTableEditorUtils::BroadcastPreChange(TablePtr, FDynamicDataTableEditorUtils::EDynamicDataTableChangeInfo::RowData);

	const TCHAR* Result = TablePtr->RowStruct->ImportText(*ClipboardValue, RowPtr, TablePtr, PPF_Copy, GWarn, GetPathNameSafe(TablePtr->RowStruct));

	TablePtr->HandleDynamicDataTableChanged(HighlightedRowName);
	TablePtr->MarkPackageDirty();

	FDynamicDataTableEditorUtils::BroadcastPostChange(TablePtr, FDynamicDataTableEditorUtils::EDynamicDataTableChangeInfo::RowData);

	if (Result == nullptr)
	{
		FNotificationInfo Info(LOCTEXT("FailedPaste", "Failed to paste row"));
		FSlateNotificationManager::Get().AddNotification(Info);
	}
}

void FDynamicDataTableEditor::DuplicateSelectedRow()
{
	UDynamicDataTable* TablePtr = Cast<UDynamicDataTable>(GetEditingObject());
	FName NewName = HighlightedRowName;

	if (NewName == NAME_None || TablePtr == nullptr)
	{
		return;
	}

	const TArray<FName> ExistingNames = TablePtr->GetRowNames();
	while (ExistingNames.Contains(NewName))
	{
		NewName.SetNumber(NewName.GetNumber() + 1);
	}

	FDynamicDataTableEditorUtils::DuplicateRow(TablePtr, HighlightedRowName, NewName);
	FDynamicDataTableEditorUtils::SelectRow(TablePtr, NewName);
}

void FDynamicDataTableEditor::RenameSelectedRowCommand()
{
	UDynamicDataTable* TablePtr = Cast<UDynamicDataTable>(GetEditingObject());
	FName NewName = HighlightedRowName; 

	if (NewName == NAME_None || TablePtr == nullptr)
	{
		return;
	}

	if (VisibleRows.IsValidIndex(HighlightedVisibleRowIndex))
	{
		TSharedPtr< SDynamicDataTableListViewRow > RowWidget = StaticCastSharedPtr< SDynamicDataTableListViewRow >(CellsListView->WidgetFromItem(VisibleRows[HighlightedVisibleRowIndex]));
		RowWidget->SetRowForRename();
	}
}

void FDynamicDataTableEditor::DeleteSelectedRow()
{
	if (UDynamicDataTable* Table = GetEditableDynamicDataTable())
	{
		// We must perform this before removing the row
		const int32 RowToRemoveIndex = VisibleRows.IndexOfByPredicate([&](const FDynamicDataTableEditorRowListViewDataPtr& InRowName) -> bool
		{
			return InRowName->RowId == HighlightedRowName;
		});
		// Remove row
		if (FDynamicDataTableEditorUtils::RemoveRow(Table, HighlightedRowName))
		{
			// Try and keep the same row index selected
			const int32 RowIndexToSelect = FMath::Clamp(RowToRemoveIndex, 0, VisibleRows.Num() - 1);
			if (VisibleRows.IsValidIndex(RowIndexToSelect))
			{
				FDynamicDataTableEditorUtils::SelectRow(Table, VisibleRows[RowIndexToSelect]->RowId);
			}
			// Refresh list. Otherwise, the removed row would still appear in the screen until the next list refresh. An
			// analog of CellsListView->RequestListRefresh() also occurs inside FDynamicDataTableEditorUtils::SelectRow
			else
			{
				CellsListView->RequestListRefresh();
			}
		}
	}
}

FText FDynamicDataTableEditor::GetFilterText() const
{
	return ActiveFilterText;
}

void FDynamicDataTableEditor::OnFilterTextChanged(const FText& InFilterText)
{
	ActiveFilterText = InFilterText;
	UpdateVisibleRows();
}

void FDynamicDataTableEditor::OnFilterTextCommitted(const FText& NewText, ETextCommit::Type CommitInfo)
{
	if (CommitInfo == ETextCommit::OnCleared)
	{
		SearchBoxWidget->SetText(FText::GetEmpty());
		OnFilterTextChanged(FText::GetEmpty());
	}
}

void FDynamicDataTableEditor::PostRegenerateMenusAndToolbars()
{
	const UDynamicDataTable* DynamicDataTable = GetDynamicDataTable();

	if (DynamicDataTable)
	{
		const UUserDefinedStruct* UDS = Cast<const UUserDefinedStruct>(DynamicDataTable->GetRowStruct());

		// build and attach the menu overlay
		TSharedRef<SHorizontalBox> MenuOverlayBox = SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.ShadowOffset(FVector2D::UnitVector)
				.Text(LOCTEXT("DynamicDataTableEditor_RowStructType", "Row Type: "))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SHyperlink)
				.Style(FAppStyle::Get(), "Common.GotoNativeCodeHyperlink")
				.OnNavigate(this, &FDynamicDataTableEditor::OnEditDynamicDataTableStructClicked)
				.Text(FText::FromName(DynamicDataTable->GetRowStructPathName().GetAssetName()))
				.ToolTipText(LOCTEXT("DynamicDataTableRowToolTip", "Open the struct used for each row in this data table"))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SButton)
				.VAlign(VAlign_Center)
				.ButtonStyle(FAppStyle::Get(), "HoverHintOnly")
				.OnClicked(this, &FDynamicDataTableEditor::OnFindRowInContentBrowserClicked)
				.Visibility(UDS ? EVisibility::Visible : EVisibility::Collapsed)
				.ToolTipText(LOCTEXT("FindRowInCBToolTip", "Find struct in Content Browser"))
				.ContentPadding(4.0f)
				.ForegroundColor(FSlateColor::UseForeground())
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush("Icons.Search"))
				]
			];
	
		SetMenuOverlay(MenuOverlayBox);
	}
}

FReply FDynamicDataTableEditor::OnFindRowInContentBrowserClicked()
{
	const UDynamicDataTable* DynamicDataTable = GetDynamicDataTable();
	if(DynamicDataTable)
	{
		TArray<FAssetData> ObjectsToSync;
		ObjectsToSync.Add(FAssetData(DynamicDataTable->GetRowStruct()));
		GEditor->SyncBrowserToObjects(ObjectsToSync);
	}

	return FReply::Handled();
}

void FDynamicDataTableEditor::RefreshCachedDynamicDataTable(const FName InCachedSelection, const bool bUpdateEvenIfValid)
{
	UDynamicDataTable* Table = GetEditableDynamicDataTable();
	TArray<FDynamicDataTableEditorColumnHeaderDataPtr> PreviousColumns = AvailableColumns;

	FDynamicDataTableEditorUtils::CacheDynamicDataTableForEditing(Table, AvailableColumns, AvailableRows);

	// Update the desired width of the row names and numbers column
	// This prevents it growing or shrinking as you scroll the list view
	RefreshRowNumberColumnWidth();
	RefreshRowNameColumnWidth();

	// Setup the default auto-sized columns
	ColumnWidths.SetNum(AvailableColumns.Num());
	for (int32 ColumnIndex = 0; ColumnIndex < AvailableColumns.Num(); ++ColumnIndex)
	{
		const FDynamicDataTableEditorColumnHeaderDataPtr& ColumnData = AvailableColumns[ColumnIndex];
		FColumnWidth& ColumnWidth = ColumnWidths[ColumnIndex];
		ColumnWidth.CurrentWidth = FMath::Clamp(ColumnData->DesiredColumnWidth, 10.0f, 400.0f); // Clamp auto-sized columns to a reasonable limit
	}

	// Load the persistent column widths from the layout data
	{
		const TSharedPtr<FJsonObject>* LayoutColumnWidths = nullptr;
		if (LayoutData.IsValid() && LayoutData->TryGetObjectField(TEXT("ColumnWidths"), LayoutColumnWidths))
		{
			for(int32 ColumnIndex = 0; ColumnIndex < AvailableColumns.Num(); ++ColumnIndex)
			{
				const FDynamicDataTableEditorColumnHeaderDataPtr& ColumnData = AvailableColumns[ColumnIndex];

				double LayoutColumnWidth = 0.0f;
				if ((*LayoutColumnWidths)->TryGetNumberField(ColumnData->ColumnId.ToString(), LayoutColumnWidth))
				{
					FColumnWidth& ColumnWidth = ColumnWidths[ColumnIndex];
					ColumnWidth.bIsAutoSized = false;
					ColumnWidth.CurrentWidth = static_cast<float>(LayoutColumnWidth);
				}
			}
		}
	}

	if (PreviousColumns != AvailableColumns)
	{
		ColumnNamesHeaderRow->ClearColumns();

		if (CanEditRows())
		{
			ColumnNamesHeaderRow->AddColumn(
				SHeaderRow::Column(RowDragDropColumnId)
				[
					SNew(SBox)
					.VAlign(VAlign_Fill)
				.HAlign(HAlign_Fill)
				.ToolTip(IDocumentation::Get()->CreateToolTip(
					LOCTEXT("DynamicDataTableRowHandleTooltip", "Drag Drop Handles"),
					nullptr,
					*FDynamicDataTableEditorUtils::VariableTypesTooltipDocLink,
					TEXT("DynamicDataTableRowHandle")))
				[
					SNew(STextBlock)
					.Text(FText::GetEmpty())
				]
				]
			);
		}	

		ColumnNamesHeaderRow->AddColumn(
			SHeaderRow::Column(RowNumberColumnId)
			.SortMode(this, &FDynamicDataTableEditor::GetColumnSortMode, RowNumberColumnId)
			.OnSort(this, &FDynamicDataTableEditor::OnColumnNumberSortModeChanged)
			.ManualWidth(this, &FDynamicDataTableEditor::GetRowNumberColumnWidth)
			.OnWidthChanged(this, &FDynamicDataTableEditor::OnRowNumberColumnResized)
			[
				SNew(SBox)
				.VAlign(VAlign_Fill)
				.HAlign(HAlign_Fill)
				.ToolTip(IDocumentation::Get()->CreateToolTip(
				LOCTEXT("DynamicDataTableRowIndexTooltip", "Row Index"),
				nullptr,
				*FDynamicDataTableEditorUtils::VariableTypesTooltipDocLink,
				TEXT("DynamicDataTableRowIndex")))
				[
					SNew(STextBlock)
					.Text(FText::GetEmpty())
				]
			]

		);

		ColumnNamesHeaderRow->AddColumn(
			SHeaderRow::Column(RowNameColumnId)
			.DefaultLabel(LOCTEXT("DynamicDataTableRowName", "Row Name"))
			.ManualWidth(this, &FDynamicDataTableEditor::GetRowNameColumnWidth)
			.OnWidthChanged(this, &FDynamicDataTableEditor::OnRowNameColumnResized)
			.SortMode(this, &FDynamicDataTableEditor::GetColumnSortMode, RowNameColumnId)
			.OnSort(this, &FDynamicDataTableEditor::OnColumnNameSortModeChanged)
		);

		for (int32 ColumnIndex = 0; ColumnIndex < AvailableColumns.Num(); ++ColumnIndex)
		{
			const FDynamicDataTableEditorColumnHeaderDataPtr& ColumnData = AvailableColumns[ColumnIndex];

			ColumnNamesHeaderRow->AddColumn(
				SHeaderRow::Column(ColumnData->ColumnId)
				.DefaultLabel(ColumnData->DisplayName)
				.ManualWidth(TAttribute<float>::Create(TAttribute<float>::FGetter::CreateSP(this, &FDynamicDataTableEditor::GetColumnWidth, ColumnIndex)))
				.OnWidthChanged(this, &FDynamicDataTableEditor::OnColumnResized, ColumnIndex)
				.SortMode(this, &FDynamicDataTableEditor::GetColumnSortMode, ColumnData->ColumnId)
				.OnSort(this, &FDynamicDataTableEditor::OnColumnSortModeChanged)
				[
					SNew(SBox)
					.Padding(FMargin(0, 4, 0, 4))
					.VAlign(VAlign_Fill)
					.ToolTip(IDocumentation::Get()->CreateToolTip(FDynamicDataTableEditorUtils::GetRowTypeInfoTooltipText(ColumnData), nullptr, *FDynamicDataTableEditorUtils::VariableTypesTooltipDocLink, FDynamicDataTableEditorUtils::GetRowTypeTooltipDocExcerptName(ColumnData)))
					[
						SNew(STextBlock)
						.Justification(ETextJustify::Center)
						.Text(ColumnData->DisplayName)
					]
				]
			);
		}
	}

	UpdateVisibleRows(InCachedSelection, bUpdateEvenIfValid);

	if (PropertyView.IsValid())
	{
		PropertyView->SetObject(Table);
	}
}

void FDynamicDataTableEditor::UpdateVisibleRows(const FName InCachedSelection, const bool bUpdateEvenIfValid)
{
	if (ActiveFilterText.IsEmptyOrWhitespace())
	{
		VisibleRows = AvailableRows;
	}
	else
	{
		VisibleRows.Empty(AvailableRows.Num());

		const FString& ActiveFilterString = ActiveFilterText.ToString();
		for (const FDynamicDataTableEditorRowListViewDataPtr& RowData : AvailableRows)
		{
			bool bPassesFilter = false;

			if (RowData->DisplayName.ToString().Contains(ActiveFilterString))
			{
				bPassesFilter = true;
			}
			else
			{
				for (const FText& CellText : RowData->CellData)
				{
					if (CellText.ToString().Contains(ActiveFilterString))
					{
						bPassesFilter = true;
						break;
					}
				}
			}

			if (bPassesFilter)
			{
				VisibleRows.Add(RowData);
			}
		}
	}

	CellsListView->RequestListRefresh();
	RestoreCachedSelection(InCachedSelection, bUpdateEvenIfValid);
}

void FDynamicDataTableEditor::RestoreCachedSelection(const FName InCachedSelection, const bool bUpdateEvenIfValid)
{
	// Validate the requested selection to see if it matches a known row
	bool bSelectedRowIsValid = false;
	if (!InCachedSelection.IsNone())
	{
		bSelectedRowIsValid = VisibleRows.ContainsByPredicate([&InCachedSelection](const FDynamicDataTableEditorRowListViewDataPtr& RowData) -> bool
		{
			return RowData->RowId == InCachedSelection;
		});
	}

	// Apply the new selection (if required)
	if (!bSelectedRowIsValid)
	{
		SetHighlightedRow((VisibleRows.Num() > 0) ? VisibleRows[0]->RowId : NAME_None);
		CallbackOnRowHighlighted.ExecuteIfBound(HighlightedRowName);
	}
	else if (bUpdateEvenIfValid)
	{
		SetHighlightedRow(InCachedSelection);
		CallbackOnRowHighlighted.ExecuteIfBound(HighlightedRowName);
	}
}

TSharedRef<SVerticalBox> FDynamicDataTableEditor::CreateContentBox()
{
	TSharedRef<SScrollBar> HorizontalScrollBar = SNew(SScrollBar)
		.Orientation(Orient_Horizontal)
		.Thickness(FVector2D(12.0f, 12.0f));

	TSharedRef<SScrollBar> VerticalScrollBar = SNew(SScrollBar)
		.Orientation(Orient_Vertical)
		.Thickness(FVector2D(12.0f, 12.0f));

	ColumnNamesHeaderRow = SNew(SHeaderRow);

	CellsListView = SNew(SListView<FDynamicDataTableEditorRowListViewDataPtr>)
		.ListItemsSource(&VisibleRows)
		.HeaderRow(ColumnNamesHeaderRow)
		.OnGenerateRow(this, &FDynamicDataTableEditor::MakeRowWidget)
		.OnSelectionChanged(this, &FDynamicDataTableEditor::OnRowSelectionChanged)
		.ExternalScrollbar(VerticalScrollBar)
		.ConsumeMouseWheel(EConsumeMouseWheel::Always)
		.SelectionMode(ESelectionMode::Single)
		.AllowOverscroll(EAllowOverscroll::No);

	LoadLayoutData();
	RefreshCachedDynamicDataTable();

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			[
				SAssignNew(SearchBoxWidget, SSearchBox)
				.InitialText(this, &FDynamicDataTableEditor::GetFilterText)
				.OnTextChanged(this, &FDynamicDataTableEditor::OnFilterTextChanged)
				.OnTextCommitted(this, &FDynamicDataTableEditor::OnFilterTextCommitted)
			]
		]
		+SVerticalBox::Slot()
		[
			SNew(SHorizontalBox)
			+SHorizontalBox::Slot()
			[
				SNew(SScrollBox)
				.Orientation(Orient_Horizontal)
				.ExternalScrollbar(HorizontalScrollBar)
				+SScrollBox::Slot()
				[
					CellsListView.ToSharedRef()
				]
			]
			+SHorizontalBox::Slot()
			.AutoWidth()
			[
				VerticalScrollBar
			]
		]
		+SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SHorizontalBox)
			+SHorizontalBox::Slot()
			[
				HorizontalScrollBar
			]
		];
}

TSharedRef<SWidget> FDynamicDataTableEditor::CreateRowEditorBox()
{
	UDynamicDataTable* Table = Cast<UDynamicDataTable>(GetEditingObject());

	// Support undo/redo
	if (Table)
	{
		Table->SetFlags(RF_Transactional);
	}

	auto RowEditor = SNew(SRowEditor, Table);
	RowEditor->RowSelectedCallback.BindSP(this, &FDynamicDataTableEditor::SetHighlightedRow);
	CallbackOnRowHighlighted.BindSP(RowEditor, &SRowEditor::SelectRow);
	CallbackOnDynamicDataTableUndoRedo.BindSP(RowEditor, &SRowEditor::HandleUndoRedo);
	return RowEditor;
}

TSharedRef<SRowEditor> FDynamicDataTableEditor::CreateRowEditor(UDynamicDataTable* Table)
{
	return SNew(SRowEditor, Table);
}

TSharedRef<SDockTab> FDynamicDataTableEditor::SpawnTab_RowEditor(const FSpawnTabArgs& Args)
{
	check(Args.GetTabId().TabType == RowEditorTabId);

	return SNew(SDockTab)
		.Label(LOCTEXT("RowEditorTitle", "Row Editor"))
		.TabColorScale(GetTabColorScale())
		[
			SNew(SBorder)
			.Padding(2)
			.VAlign(VAlign_Top)
			.HAlign(HAlign_Fill)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			[
				RowEditorTabWidget.ToSharedRef()
			]
		];
}


TSharedRef<SDockTab> FDynamicDataTableEditor::SpawnTab_DynamicDataTable( const FSpawnTabArgs& Args )
{
	check( Args.GetTabId().TabType == DynamicDataTableTabId );

	UDynamicDataTable* Table = Cast<UDynamicDataTable>(GetEditingObject());

	// Support undo/redo
	if (Table)
	{
		Table->SetFlags(RF_Transactional);
	}

	return SNew(SDockTab)
		.Label( LOCTEXT("DynamicDataTableTitle", "Data Table") )
		.TabColorScale( GetTabColorScale() )
		[
			SNew(SBorder)
			.Padding(2)
			.BorderImage( FAppStyle::GetBrush( "ToolPanel.GroupBorder" ) )
			[
				DynamicDataTableTabWidget.ToSharedRef()
			]
		];
}

TSharedRef<SDockTab> FDynamicDataTableEditor::SpawnTab_DynamicDataTableDetails(const FSpawnTabArgs& Args)
{
	check(Args.GetTabId().TabType == DynamicDataTableDetailsTabId);

	PropertyView->SetObject(GetEditableDynamicDataTable());

	return SNew(SDockTab)
		.Label(LOCTEXT("DynamicDataTableDetails", "Data Table Details"))
		.TabColorScale(GetTabColorScale())
		[
			SNew(SBorder)
			.Padding(2)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			[
				PropertyView.ToSharedRef()
			]
		];
}

void FDynamicDataTableEditor::SetHighlightedRow(FName Name)
{
	if (Name == HighlightedRowName)
	{
		return;
	}

	if (Name.IsNone())
	{
		HighlightedRowName = NAME_None;
		CellsListView->ClearSelection();
		HighlightedVisibleRowIndex = INDEX_NONE;
	}
	else
	{
		HighlightedRowName = Name;

		FDynamicDataTableEditorRowListViewDataPtr* NewSelectionPtr = NULL;
		for (HighlightedVisibleRowIndex = 0; HighlightedVisibleRowIndex < VisibleRows.Num(); ++HighlightedVisibleRowIndex)
		{
			if (VisibleRows[HighlightedVisibleRowIndex]->RowId == Name)
			{
				NewSelectionPtr = &(VisibleRows[HighlightedVisibleRowIndex]);

				break;
			}
		}


		// Synchronize the list views
		if (NewSelectionPtr)
		{
			CellsListView->SetSelection(*NewSelectionPtr);

			CellsListView->RequestScrollIntoView(*NewSelectionPtr);
		}
		else
		{
			CellsListView->ClearSelection();
		}
	}
}

void FDynamicDataTableEditor::RefreshDynTable() {
	UDynamicDataTable* Source = GetEditableDynamicDataTable();
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
		Table.Map = {};
		Table.RowNames = {};
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

#undef LOCTEXT_NAMESPACE
