// Copyright Epic Games, Inc. All Rights Reserved.

#include "SDynamicDataTableListViewRow.h"

#include "AssetRegistry/AssetData.h"
#include "Containers/Map.h"
#include "DynamicDataTableEditor.h"
#include "DynamicDatatable/DynamicDataTable/DynamicDataTableUtils.h"
#include "Delegates/Delegate.h"
#include "Editor.h"
#include "DynamicDatatable/DynamicDataTable/DynamicDataTable.h"
#include "Framework/Application/MenuStack.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Commands/GenericCommands.h"
#include "Framework/Commands/UIAction.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformCrt.h"
#include "HAL/PlatformMisc.h"
#include "Input/Events.h"
#include "Internationalization/Internationalization.h"
#include "Layout/Children.h"
#include "Layout/Margin.h"
#include "Layout/WidgetPath.h"
#include "Math/UnrealMathSSE.h"
#include "Misc/AssertionMacros.h"
#include "Misc/Attribute.h"
#include "Misc/MessageDialog.h"
#include "SlotBase.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/ISlateStyle.h"
#include "Templates/Casts.h"
#include "Textures/SlateIcon.h"
#include "UObject/UnrealNames.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/SInlineEditableTextBlock.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"

class STableViewBase;
class SWidget;
struct FGeometry;
struct FSlateBrush;

#define LOCTEXT_NAMESPACE "SDynamicDataTableListViewRowName"

void SDynamicDataTableListViewRow::Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& InOwnerTableView)
{
	RowDataPtr = InArgs._RowDataPtr;
	CurrentName = MakeShareable(new FName(RowDataPtr->RowId));
	DynamicDataTableEditor = InArgs._DynamicDataTableEditor;
	IsEditable = InArgs._IsEditable;
	SMultiColumnTableRow<FDynamicDataTableEditorRowListViewDataPtr>::Construct(
		FSuperRowType::FArguments()
		.Style(FAppStyle::Get(), "DataTableEditor.CellListViewRow")
		.OnDrop(this, &SDynamicDataTableListViewRow::OnRowDrop)
		.OnDragEnter(this, &SDynamicDataTableListViewRow::OnRowDragEnter)
		.OnDragLeave(this, &SDynamicDataTableListViewRow::OnRowDragLeave),
		InOwnerTableView
	);

	SetBorderImage(TAttribute<const FSlateBrush*>(this, &SDynamicDataTableListViewRow::GetBorder));

}

FReply SDynamicDataTableListViewRow::OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (IsEditable && MouseEvent.GetEffectingButton() == EKeys::RightMouseButton && RowDataPtr.IsValid() && FEditorDelegates::OnOpenReferenceViewer.IsBound() && DynamicDataTableEditor.IsValid())
	{
		FDynamicDataTableEditorUtils::SelectRow(DynamicDataTableEditor.Pin()->GetDynamicDataTable(), RowDataPtr->RowId);

		TSharedRef<SWidget> MenuWidget = MakeRowActionsMenu();

		FWidgetPath WidgetPath = MouseEvent.GetEventPath() != nullptr ? *MouseEvent.GetEventPath() : FWidgetPath();
		FSlateApplication::Get().PushMenu(AsShared(), WidgetPath, MenuWidget, MouseEvent.GetScreenSpacePosition(), FPopupTransitionEffect::ContextMenu);
		return FReply::Handled();
	}

	return STableRow::OnMouseButtonUp(MyGeometry, MouseEvent);
}

void SDynamicDataTableListViewRow::OnSearchForReferences()
{
	if (DynamicDataTableEditor.IsValid() && RowDataPtr.IsValid())
	{
		if (FDynamicDataTableEditor* DynamicDataTableEditorPtr = DynamicDataTableEditor.Pin().Get())
		{
			UDynamicDataTable* SourceDynamicDataTable = const_cast<UDynamicDataTable*>(DynamicDataTableEditorPtr->GetDynamicDataTable());

			TArray<FAssetIdentifier> AssetIdentifiers;
			AssetIdentifiers.Add(FAssetIdentifier(SourceDynamicDataTable, RowDataPtr->RowId));

			FEditorDelegates::OnOpenReferenceViewer.Broadcast(AssetIdentifiers, FReferenceViewerParams());
		}
	}
}

void SDynamicDataTableListViewRow::OnInsertNewRow(EDynamicDataTableRowInsertionPosition InsertPosition)
{
	if (DynamicDataTableEditor.IsValid() && RowDataPtr.IsValid())
	{
		if (FDynamicDataTableEditor* DynamicDataTableEditorPtr = DynamicDataTableEditor.Pin().Get())
		{
			UDynamicDataTable* SourceDynamicDataTable = const_cast<UDynamicDataTable*>(DynamicDataTableEditorPtr->GetDynamicDataTable());

			if (SourceDynamicDataTable)
			{
				FName NewName = DynamicDataTableUtils::MakeValidName(TEXT("NewRow"));
				while (SourceDynamicDataTable->GetRowMap().Contains(NewName))
				{
					NewName.SetNumber(NewName.GetNumber() + 1);
				}

				if (InsertPosition == EDynamicDataTableRowInsertionPosition::Bottom)
				{
					FDynamicDataTableEditorUtils::AddRow(SourceDynamicDataTable, NewName);
				}
				else
				{
					FDynamicDataTableEditorUtils::AddRowAboveOrBelowSelection(SourceDynamicDataTable, *CurrentName, NewName, InsertPosition);
				}
				FDynamicDataTableEditorUtils::SelectRow(SourceDynamicDataTable, NewName);

				DynamicDataTableEditorPtr->SetDefaultSort();
			}
		}
	}
}

FReply SDynamicDataTableListViewRow::OnRowDrop(const FDragDropEvent& DragDropEvent)
{
	bIsHoveredDragTarget = false;

	TSharedPtr<FDynamicDataTableRowDragDropOp> DynamicDataTableDropOp = DragDropEvent.GetOperationAs< FDynamicDataTableRowDragDropOp >();
	TSharedPtr<SDynamicDataTableListViewRow> RowPtr = nullptr;
	if (DynamicDataTableDropOp.IsValid() && DynamicDataTableDropOp->Row.IsValid())
	{
		RowPtr = DynamicDataTableDropOp->Row.Pin();
	}
	if (!RowPtr.IsValid())
	{
		return FReply::Unhandled();
	}

	int32 JumpCount = (RowPtr->RowDataPtr)->RowNum - RowDataPtr->RowNum;

	if (!JumpCount)
	{
		return FReply::Handled();
	}

	FDynamicDataTableEditorUtils::ERowMoveDirection Direction = JumpCount > 0 ? FDynamicDataTableEditorUtils::ERowMoveDirection::Up : FDynamicDataTableEditorUtils::ERowMoveDirection::Down;

	if (FDynamicDataTableEditor* DynamicDataTableEditorPtr = DynamicDataTableEditor.Pin().Get())
	{
		UDynamicDataTable* SourceDynamicDataTable = const_cast<UDynamicDataTable*>(DynamicDataTableEditorPtr->GetDynamicDataTable());

		if (SourceDynamicDataTable)
		{
			FName& RowId = (RowPtr->RowDataPtr)->RowId;

			FDynamicDataTableEditorUtils::MoveRow(SourceDynamicDataTable, RowId, Direction, FMath::Abs<int32>(JumpCount));
			
			FDynamicDataTableEditorUtils::SelectRow(SourceDynamicDataTable, RowId);

			DynamicDataTableEditorPtr->SortMode = EColumnSortMode::Ascending;
			DynamicDataTableEditorPtr->SortByColumn = FDynamicDataTableEditor::RowNumberColumnId;

			return FReply::Handled();
		}
	}

	return FReply::Unhandled();
}

FReply SDynamicDataTableListViewRow::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	FKey Key = InKeyEvent.GetKey();

	if (Key == EKeys::Escape && InlineEditableText->HasKeyboardFocus())
	{
		// Clear focus
		return FReply::Handled().SetUserFocus(SharedThis(this), EFocusCause::Cleared);
	}

	return FReply::Unhandled();
}

void SDynamicDataTableListViewRow::OnRowRenamed(const FText& Text, ETextCommit::Type CommitType)
{
	UDynamicDataTable* DynamicDataTable = Cast<UDynamicDataTable>(DynamicDataTableEditor.Pin()->GetEditingObject());

	if (!GetCurrentNameAsText().EqualTo(Text) && DynamicDataTable)
	{
		if (FDynamicDataTableEditor* DynamicDataTableEditorPtr = DynamicDataTableEditor.Pin().Get())
		{

			const TArray<FName> RowNames = DynamicDataTable->GetRowNames();

			if (Text.IsEmptyOrWhitespace() || !FName::IsValidXName(Text.ToString(), INVALID_NAME_CHARACTERS))
			{
				// Only pop up the error dialog if the rename was caused by the user's action
				if ((CommitType == ETextCommit::OnEnter) || (CommitType == ETextCommit::OnUserMovedFocus))
				{
					// popup an error dialog here
					const FText Message = FText::Format(LOCTEXT("InvalidRowName", "'{0}' is not a valid row name"), Text);
					FMessageDialog::Open(EAppMsgType::Ok, Message);
				}
				return;
			}
			const FName NewName = DynamicDataTableUtils::MakeValidName(Text.ToString());
			if (NewName == NAME_None)
			{
				// popup an error dialog here
				const FText Message = FText::Format(LOCTEXT("InvalidRowName", "'{0}' is not a valid row name"), Text);
				FMessageDialog::Open(EAppMsgType::Ok, Message);

				return;
			}
			for (const FName& Name : RowNames)
			{
				if (Name.IsValid() && (Name == NewName))
				{
					//the name already exists
					// popup an error dialog here
					const FText Message = FText::Format(LOCTEXT("DuplicateRowName", "'{0}' is already used as a row name in this table"), Text);
					FMessageDialog::Open(EAppMsgType::Ok, Message);
					return;

				}
			}

			const FName OldName = GetCurrentName();
			FDynamicDataTableEditorUtils::RenameRow(DynamicDataTable, OldName, NewName);
			FDynamicDataTableEditorUtils::SelectRow(DynamicDataTable, NewName);

			DynamicDataTableEditorPtr->SetDefaultSort();

			*CurrentName = NewName;
		}
	}
}

TSharedRef<SWidget> SDynamicDataTableListViewRow::GenerateWidgetForColumn(const FName& ColumnName)
{
	TSharedPtr<FDynamicDataTableEditor> DynamicDataTableEditorPtr = DynamicDataTableEditor.Pin();
	return (DynamicDataTableEditorPtr.IsValid())
		? MakeCellWidget(IndexInList, ColumnName)
		: SNullWidget::NullWidget;
}

TSharedRef<SWidget> SDynamicDataTableListViewRow::MakeCellWidget(const int32 InRowIndex, const FName& InColumnId)
{
	const FName RowDragDropColumnId("RowDragDrop");

	int32 ColumnIndex = 0;

	FDynamicDataTableEditor* DynamicDataTableEdit = DynamicDataTableEditor.Pin().Get();
	TArray<FDynamicDataTableEditorColumnHeaderDataPtr>& AvailableColumns = DynamicDataTableEdit->AvailableColumns;

	if (InColumnId.IsEqual(RowDragDropColumnId))
	{
		return SNew(SDynamicDataTableRowHandle)
			.Content()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.Padding(5.0f, 1.0f)
				[
					SNew(SImage)
					.Image(FCoreStyle::Get().GetBrush("VerticalBoxDragIndicatorShort"))
				]
			]
			.ParentRow(SharedThis(this));
	}

	const FName RowNumberColumnId("RowNumber");

	if (InColumnId.IsEqual(RowNumberColumnId))
	{
		return SNew(SBox)
			.Padding(FMargin(4, 2, 4, 2))
			[
				SNew(STextBlock)
				.TextStyle(FAppStyle::Get(), "DataTableEditor.CellText")
				.Text(FText::FromString(FString::FromInt(RowDataPtr->RowNum)))
				.ColorAndOpacity(DynamicDataTableEdit, &FDynamicDataTableEditor::GetRowTextColor, RowDataPtr->RowId)
				.HighlightText(DynamicDataTableEdit, &FDynamicDataTableEditor::GetFilterText)
			];
	}

	const FName RowNameColumnId("RowName");

	if (InColumnId.IsEqual(RowNameColumnId))
	{
		return SNew(SBox)
			.Padding(FMargin(4, 2, 4, 2))
			[
				SAssignNew(InlineEditableText, SInlineEditableTextBlock)
				.Text(RowDataPtr->DisplayName)
				.OnTextCommitted(this, &SDynamicDataTableListViewRow::OnRowRenamed)
				.HighlightText(DynamicDataTableEdit, &FDynamicDataTableEditor::GetFilterText)
				.ColorAndOpacity(DynamicDataTableEdit, &FDynamicDataTableEditor::GetRowTextColor, RowDataPtr->RowId)
				.IsReadOnly(!IsEditable)
			];
	}

	for (; ColumnIndex < AvailableColumns.Num(); ++ColumnIndex)
	{
		const FDynamicDataTableEditorColumnHeaderDataPtr& ColumnData = AvailableColumns[ColumnIndex];
		if (ColumnData->ColumnId == InColumnId)
		{
			break;
		}
	}
	 
	// Valid column ID?
	if (AvailableColumns.IsValidIndex(ColumnIndex) && RowDataPtr->CellData.IsValidIndex(ColumnIndex))
	{
		return SNew(SBox)
			.Padding(FMargin(4, 2, 4, 2))
			[
				SNew(STextBlock)
				.TextStyle(FAppStyle::Get(), "DataTableEditor.CellText")
				.ColorAndOpacity(DynamicDataTableEdit, &FDynamicDataTableEditor::GetRowTextColor, RowDataPtr->RowId)
				.Text(DynamicDataTableEdit, &FDynamicDataTableEditor::GetCellText, RowDataPtr, ColumnIndex)
				.HighlightText(DynamicDataTableEdit, &FDynamicDataTableEditor::GetFilterText)
				.ToolTipText(DynamicDataTableEdit, &FDynamicDataTableEditor::GetCellToolTipText, RowDataPtr, ColumnIndex)
			];
	}

	return SNullWidget::NullWidget;
}

FName SDynamicDataTableListViewRow::GetCurrentName() const
{
	return CurrentName.IsValid() ? *CurrentName : NAME_None;

}

uint32 SDynamicDataTableListViewRow::GetCurrentIndex() const
{
	return RowDataPtr.IsValid() ? RowDataPtr->RowNum : -1;
}

const TArray<FText>& SDynamicDataTableListViewRow::GetCellValues() const
{
	check(RowDataPtr)
	return RowDataPtr->CellData;
}

FReply SDynamicDataTableListViewRow::OnMouseButtonDoubleClick(const FGeometry& InMyGeometry, const FPointerEvent& InMouseEvent)
{
	if (InlineEditableText->IsHovered())
	{
		InlineEditableText->EnterEditingMode();
	}

	return FReply::Handled();
}

void SDynamicDataTableListViewRow::SetRowForRename()
{
	InlineEditableText->EnterEditingMode();
}

const FDynamicDataTableEditorRowListViewDataPtr& SDynamicDataTableListViewRow::GetRowDataPtr() const
{
	return RowDataPtr;
}

FText SDynamicDataTableListViewRow::GetCurrentNameAsText() const
{
	return FText::FromName(GetCurrentName());
}

void SDynamicDataTableRowHandle::Construct(const FArguments& InArgs)
{
	ParentRow = InArgs._ParentRow;

	ChildSlot
		[
			InArgs._Content.Widget
		];
}

FReply SDynamicDataTableRowHandle::OnDragDetected(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (MouseEvent.IsMouseButtonDown(EKeys::LeftMouseButton))
	{
		TSharedPtr<FDragDropOperation> DragDropOp = CreateDragDropOperation(ParentRow.Pin());
		if (DragDropOp.IsValid())
		{
			return FReply::Handled().BeginDragDrop(DragDropOp.ToSharedRef());
		}
	}

	return FReply::Unhandled();

}

TSharedPtr<FDynamicDataTableRowDragDropOp> SDynamicDataTableRowHandle::CreateDragDropOperation(TSharedPtr<SDynamicDataTableListViewRow> InRow)
{
	TSharedPtr<FDynamicDataTableRowDragDropOp> Operation = MakeShareable(new FDynamicDataTableRowDragDropOp(InRow));

	return Operation;
}

void SDynamicDataTableListViewRow::SetIsDragDrop(bool bInIsDragDrop)
{
	bIsDragDropObject = bInIsDragDrop;
}


void SDynamicDataTableListViewRow::OnRowDragEnter(const FDragDropEvent& DragDropEvent)
{
	bIsHoveredDragTarget = true;
}

void SDynamicDataTableListViewRow::OnRowDragLeave(const FDragDropEvent& DragDropEvent)
{
	bIsHoveredDragTarget = false;
}


const FSlateBrush* SDynamicDataTableListViewRow::GetBorder() const
{
	if (bIsDragDropObject)
	{
		return FAppStyle::GetBrush("DataTableEditor.DragDropObject");
	}
	else if (bIsHoveredDragTarget)
	{
		return FAppStyle::GetBrush("DataTableEditor.DragDropHoveredTarget");
	}
	else
	{
		return STableRow::GetBorder();
	}
}

void SDynamicDataTableListViewRow::OnMoveToExtentClicked(FDynamicDataTableEditorUtils::ERowMoveDirection MoveDirection)
{
	if (DynamicDataTableEditor.IsValid())
	{
		TSharedPtr<FDynamicDataTableEditor> DynamicDataTableEditorPtr = DynamicDataTableEditor.Pin();
		DynamicDataTableEditorPtr->OnMoveToExtentClicked(MoveDirection);
	}
}


TSharedRef<SWidget> SDynamicDataTableListViewRow::MakeRowActionsMenu()
{
	FMenuBuilder MenuBuilder(true, DynamicDataTableEditor.Pin()->GetToolkitCommands());

	MenuBuilder.AddMenuEntry(
		LOCTEXT("DynamicDataTableRowMenuActions_InsertNewRow", "Insert New Row"),
		LOCTEXT("DynamicDataTableRowMenuActions_InsertNewRowTooltip", "Insert a new row"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "DataTableEditor.Add"), 
		FUIAction(FExecuteAction::CreateSP(this, &SDynamicDataTableListViewRow::OnInsertNewRow, EDynamicDataTableRowInsertionPosition::Bottom))
	);
	
	MenuBuilder.AddMenuEntry(
		LOCTEXT("DynamicDataTableRowMenuActions_InsertNewRowAbove", "Insert New Row Above"),
		LOCTEXT("DynamicDataTableRowMenuActions_InsertNewRowAboveTooltip", "Insert a new Row above the current selection"),
		FSlateIcon(), 
		FUIAction(FExecuteAction::CreateSP(this, &SDynamicDataTableListViewRow::OnInsertNewRow, EDynamicDataTableRowInsertionPosition::Above))
	);
	
	MenuBuilder.AddMenuEntry(
		LOCTEXT("DynamicDataTableRowMenuActions_InsertNewRowBelow", "Insert New Row Below"),
		LOCTEXT("DynamicDataTableRowMenuActions_InsertNewRowBelowTooltip", "Insert a new Row below the current selection"),
		FSlateIcon(), 
		FUIAction(FExecuteAction::CreateSP(this, &SDynamicDataTableListViewRow::OnInsertNewRow, EDynamicDataTableRowInsertionPosition::Below))
	);

	MenuBuilder.AddMenuEntry(FGenericCommands::Get().Copy);
	MenuBuilder.AddMenuEntry(FGenericCommands::Get().Paste);
	MenuBuilder.AddMenuEntry(FGenericCommands::Get().Duplicate);
	MenuBuilder.AddMenuEntry(FGenericCommands::Get().Rename);
	MenuBuilder.AddMenuEntry(FGenericCommands::Get().Delete);

	MenuBuilder.AddMenuSeparator();

	MenuBuilder.AddMenuEntry(
		LOCTEXT("DynamicDataTableRowMenuActions_MoveToTopAction", "Move Row to Top"),
		LOCTEXT("DynamicDataTableRowMenuActions_MoveToTopActionTooltip", "Move selected Row to the top"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Symbols.DoubleUpArrow"), 
		FUIAction(FExecuteAction::CreateSP(this, &SDynamicDataTableListViewRow::OnMoveToExtentClicked, FDynamicDataTableEditorUtils::ERowMoveDirection::Up))
	);
	
	MenuBuilder.AddMenuEntry(
		LOCTEXT("DynamicDataTableRowMenuActions_MoveToBottom", "Move Row To Bottom"),
		LOCTEXT("DynamicDataTableRowMenuActions_MoveToBottomTooltip", "Move selected Row to the bottom"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Symbols.DoubleDownArrow"), 
		FUIAction(FExecuteAction::CreateSP(this, &SDynamicDataTableListViewRow::OnMoveToExtentClicked, FDynamicDataTableEditorUtils::ERowMoveDirection::Down))
	);
	
	MenuBuilder.AddMenuSeparator();

	MenuBuilder.AddMenuEntry(
		NSLOCTEXT("FDynamicDataTableRowUtils", "FDynamicDataTableRowUtils_SearchForReferences", "Find Row References"),
		NSLOCTEXT("FDynamicDataTableRowUtils", "FDynamicDataTableRowUtils_SearchForReferencesTooltip", "Find assets that reference this Row"),
		FSlateIcon(), 
		FUIAction(FExecuteAction::CreateSP(this, &SDynamicDataTableListViewRow::OnSearchForReferences))
	);

	return MenuBuilder.MakeWidget();
}

FDynamicDataTableRowDragDropOp::FDynamicDataTableRowDragDropOp(TSharedPtr<SDynamicDataTableListViewRow> InRow)
{
	Row = InRow;

	TSharedPtr<SDynamicDataTableListViewRow> RowPtr = nullptr;
	if (Row.IsValid())
	{
		RowPtr = Row.Pin();
		RowPtr->SetIsDragDrop(true);

		DecoratorWidget = SNew(SBorder)
			.Padding(8.f)
			.BorderImage(FAppStyle::GetBrush("Graph.ConnectorFeedback.Border"))
			.Content()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(FText::Format(NSLOCTEXT("DynamicDataTableDragDrop", "PlaceRowHere", "Place Row {0} Here"), FText::AsNumber(InRow->GetCurrentIndex())))
				]
			];

		Construct();
	}
}

void FDynamicDataTableRowDragDropOp::OnDrop(bool bDropWasHandled, const FPointerEvent& MouseEvent)
{
	FDecoratedDragDropOp::OnDrop(bDropWasHandled, MouseEvent);

	TSharedPtr<SDynamicDataTableListViewRow> RowPtr = nullptr;
	if (Row.IsValid())
	{
		RowPtr = Row.Pin();
		RowPtr->SetIsDragDrop(false);
	}
}

#undef LOCTEXT_NAMESPACE
