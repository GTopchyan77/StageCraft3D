// Copyright Epic Games, Inc. All Rights Reserved.

#include "DynamicDataTableEditorUtils.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectIterator.h"
#include "Styling/SlateTypes.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "Styling/AppStyle.h"
#include "StructUtils/UserDefinedStruct.h"
#include "Misc/StringUtility.h"
#include "ScopedTransaction.h"
//#include "K2Node_GetDynamicDataTableRow.h"
#include "Input/Reply.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Input/SComboBox.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "DetailWidgetRow.h"
#include "Editor.h"

#define LOCTEXT_NAMESPACE "DynamicDataTableEditorUtils"

/** Combobox that allows selecting a struct row for a data table. Based off of SSearchableComboBox */
class SDynamicDataTableStructComboBox : public SComboButton
{
public:
	/** Type of list used for showing menu options. */
	typedef SListView< TSharedPtr<FString> > SComboListType;
	/** Delegate type used to generate widgets that represent Options */
	typedef typename TSlateDelegates< TSharedPtr<FString> >::FOnGenerateWidget FOnGenerateWidget;
	typedef typename TSlateDelegates< TSharedPtr<FString> >::FOnSelectionChanged FOnSelectionChanged;
	DECLARE_DELEGATE_OneParam(FOnFillComboBoxStrings, TArray<TSharedPtr<FString>>&);

	SLATE_BEGIN_ARGS(SDynamicDataTableStructComboBox)
		: _Content()
		, _ComboBoxStyle(&FCoreStyle::Get().GetWidgetStyle< FComboBoxStyle >("ComboBox"))
		, _ButtonStyle(nullptr)
		, _ItemStyle(&FAppStyle::Get().GetWidgetStyle<FTableRowStyle>("ComboBox.Row"))
		, _ContentPadding(_ComboBoxStyle->ContentPadding)
		, _ForegroundColor(FSlateColor::UseStyle())
		, _OnStructSelected()
		, _InitiallySelectedItem(nullptr)
		, _Method()
		, _MaxListHeight(450.0f)
		, _HasDownArrow(true)
	{}

	/** Slot for this button's content (optional) */
	SLATE_DEFAULT_SLOT(FArguments, Content)

	SLATE_STYLE_ARGUMENT(FComboBoxStyle, ComboBoxStyle)

	/** The visual style of the button part of the combo box (overrides ComboBoxStyle) */
	SLATE_STYLE_ARGUMENT(FButtonStyle, ButtonStyle)

	SLATE_STYLE_ARGUMENT(FTableRowStyle, ItemStyle)

	SLATE_ATTRIBUTE(FMargin, ContentPadding)
	SLATE_ATTRIBUTE(FSlateColor, ForegroundColor)

	SLATE_EVENT(FDynamicDataTableEditorUtils::FOnDynamicDataTableStructSelected, OnStructSelected)
	
	/** The custom scrollbar to use in the ListView */
	SLATE_ARGUMENT(TSharedPtr<SScrollBar>, CustomScrollbar)

	/** The option that should be selected when the combo box is first created */
	SLATE_ARGUMENT(TSharedPtr<FString>, InitiallySelectedItem)

	SLATE_ARGUMENT(TOptional<EPopupMethod>, Method)

	/** The max height of the combo box menu */
	SLATE_ARGUMENT(float, MaxListHeight)

	/**
	 * When false, the down arrow is not generated and it is up to the API consumer
	 * to make their own visual hint that this is a drop down.
	 */
	SLATE_ARGUMENT(bool, HasDownArrow)

	SLATE_END_ARGS()

	/**
	 * Construct the widget from a declaration
	 *
	 * @param InArgs   Declaration from which to construct the combo box
	 */
	void Construct(const FArguments& InArgs);

	void ClearSelection();

	void SetSelectedItem(TSharedPtr<FString> InSelectedItem);

	/** @return the item currently selected by the combo box. */
	TSharedPtr<FString> GetSelectedItem();

	/**
	 * Requests a list refresh after updating options
	 * Call SetSelectedItem to update the selected item if required
	 * @see SetSelectedItem
	 */
	void RefreshOptions();

	/** Returns the asset data for a specific string, or null if not found */
	const FAssetData* FindAssetDataForString(TSharedPtr<FString> StringOption) const;

	/** Returns struct from AssetData, possibly loading it */
	UScriptStruct* GetOrLoadStruct(const FAssetData* AssetData);

private:

	/** Generate a row for the InItem in the combo box's list (passed in as OwnerTable). Do this by calling the user-specified OnGenerateWidget */
	TSharedRef<ITableRow> GenerateMenuItemRow(TSharedPtr<FString> InItem, const TSharedRef<STableViewBase>& OwnerTable);

	/** Called if the menu is closed */
	void OnMenuOpenChanged(bool bOpen);

	/** Invoked when the selection in the list changes */
	void OnSelectionChanged_Internal(TSharedPtr<FString> ProposedSelection, ESelectInfo::Type SelectInfo);

	/** Invoked when the search text changes */
	void OnSearchTextChanged(const FText& ChangedText);

	/** Text to display inside box */
	FText GetSelectedText() const;

	/** Show tooltip text for a specific option */
	FText GetTooltipText(TSharedPtr<FString> StringOption);

	/** Handle clicking on the content menu */
	virtual FReply OnButtonClicked() override;

	/** The item style to use. */
	const FTableRowStyle* ItemStyle;

private:
	/** Delegate that is invoked when the selected item in the combo box changes */
	FDynamicDataTableEditorUtils::FOnDynamicDataTableStructSelected OnStructSelected;
	/** The padding around each menu row */
	FMargin MenuRowPadding;
	/** The item currently selected in the combo box */
	TSharedPtr<FString> SelectedItem;
	/** The search field used for the combox box's contents */
	TSharedPtr< SEditableTextBox > SearchField;
	/** The ListView that we pop up; visualized the available options. */
	TSharedPtr< SComboListType > ComboListView;
	/** The Scrollbar used in the ListView. */
	TSharedPtr< SScrollBar > CustomScrollbar;

	/** List of names to show in combo box, there is a 1:1 mapping to PossibleStructs */
	TArray< TSharedPtr<FString> > CurrentOptions;
	/** List of AssetData representing rows */
	TArray<FAssetData> PossibleStructs;
};

void SDynamicDataTableStructComboBox::Construct(const FArguments& InArgs)
{
	check(InArgs._ComboBoxStyle);

	ItemStyle = InArgs._ItemStyle;

	MenuRowPadding = InArgs._ComboBoxStyle->MenuRowPadding;

	// Work out which values we should use based on whether we were given an override, or should use the style's version
	const FComboButtonStyle& OurComboButtonStyle = InArgs._ComboBoxStyle->ComboButtonStyle;
	const FButtonStyle* const OurButtonStyle = InArgs._ButtonStyle ? InArgs._ButtonStyle : &OurComboButtonStyle.ButtonStyle;

	this->OnStructSelected = InArgs._OnStructSelected;

	CustomScrollbar = InArgs._CustomScrollbar;

	TSharedRef<SWidget> ComboBoxMenuContent =
		SNew(SBox)
		.MaxDesiredHeight(InArgs._MaxListHeight)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SAssignNew(this->SearchField, SEditableTextBox)
				.HintText(LOCTEXT("Search", "Search"))
				.OnTextChanged(this, &SDynamicDataTableStructComboBox::OnSearchTextChanged)
			]

			+ SVerticalBox::Slot()
			[
				SAssignNew(this->ComboListView, SComboListType)
				.ListItemsSource(&CurrentOptions)
				.OnGenerateRow(this, &SDynamicDataTableStructComboBox::GenerateMenuItemRow)
				.OnSelectionChanged(this, &SDynamicDataTableStructComboBox::OnSelectionChanged_Internal)
				.SelectionMode(ESelectionMode::Single)
				.ExternalScrollbar(InArgs._CustomScrollbar)
			]
		];

	// Set up content
	TSharedPtr<SWidget> ButtonContent = InArgs._Content.Widget;
	if (InArgs._Content.Widget == SNullWidget::NullWidget)
	{
		SAssignNew(ButtonContent, STextBlock)
			.Text(this, &SDynamicDataTableStructComboBox::GetSelectedText);
	}

	SComboButton::Construct(SComboButton::FArguments()
		.ComboButtonStyle(&OurComboButtonStyle)
		.ButtonStyle(OurButtonStyle)
		.Method(InArgs._Method)
		.ButtonContent()
		[
			ButtonContent.ToSharedRef()
		]
		.MenuContent()
		[
			ComboBoxMenuContent
		]
		.HasDownArrow(InArgs._HasDownArrow)
		.ContentPadding(InArgs._ContentPadding)
		.ForegroundColor(InArgs._ForegroundColor)
		.OnMenuOpenChanged(this, &SDynamicDataTableStructComboBox::OnMenuOpenChanged)
		.IsFocusable(true)
		);


	// Better to select search field so you can type right away
	SetMenuContentWidgetToFocus(SearchField);

	// Refresh options now
	RefreshOptions();

	// Need to establish the selected item at point of construction so its available for querying
	// NB: If you need a selection to fire use SetItemSelection rather than setting an IntiallySelectedItem
	SelectedItem = InArgs._InitiallySelectedItem;
	if (TListTypeTraits<TSharedPtr<FString>>::IsPtrValid(SelectedItem))
	{
		ComboListView->Private_SetItemSelection(SelectedItem, true);
	}
}

void SDynamicDataTableStructComboBox::ClearSelection()
{
	ComboListView->ClearSelection();
}

void SDynamicDataTableStructComboBox::SetSelectedItem(TSharedPtr<FString> InSelectedItem)
{
	if (TListTypeTraits<TSharedPtr<FString>>::IsPtrValid(InSelectedItem))
	{
		ComboListView->SetSelection(InSelectedItem);
	}
	else
	{
		ComboListView->ClearSelection();
	}
}

TSharedPtr<FString> SDynamicDataTableStructComboBox::GetSelectedItem()
{
	return SelectedItem;
}

FText SDynamicDataTableStructComboBox::GetSelectedText() const
{
	if (SelectedItem.IsValid())
	{
		return FText::FromString(*SelectedItem);
	}

	return FText::GetEmpty();
}

FText SDynamicDataTableStructComboBox::GetTooltipText(TSharedPtr<FString> StringOption)
{
	const FAssetData* FoundAsset = FindAssetDataForString(StringOption);

	if (FoundAsset)
	{
		return FText::FromString(FoundAsset->PackageName.ToString());
	}
	return FText::GetEmpty();
}

void SDynamicDataTableStructComboBox::RefreshOptions()
{
	if (PossibleStructs.Num() == 0)
	{
		FDynamicDataTableEditorUtils::GetPossibleStructAssetData(PossibleStructs);

		CurrentOptions.Reset();
		for (const FAssetData& FoundStruct : PossibleStructs)
		{
			CurrentOptions.Add(MakeShareable(new FString(FoundStruct.AssetName.ToString())));
		}
	}

	if (!ComboListView->IsPendingRefresh())
	{
		ComboListView->RequestListRefresh();
	}
}

const FAssetData* SDynamicDataTableStructComboBox::FindAssetDataForString(TSharedPtr<FString> StringOption) const
{
	check(CurrentOptions.Num() == PossibleStructs.Num());
	for (int32 i = 0; i < CurrentOptions.Num(); i++)
	{
		if (StringOption == CurrentOptions[i])
		{
			return &PossibleStructs[i];
		}
	}
	return nullptr;
}

UScriptStruct* SDynamicDataTableStructComboBox::GetOrLoadStruct(const FAssetData* AssetData)
{
	if (!AssetData)
	{
		return nullptr;
	}

	return Cast<UScriptStruct>(AssetData->GetAsset());
}

TSharedRef<ITableRow> SDynamicDataTableStructComboBox::GenerateMenuItemRow(TSharedPtr<FString> InItem, const TSharedRef<STableViewBase>& OwnerTable)
{
	FString SearchToken = SearchField->GetText().ToString().ToLower();
	EVisibility WidgetVisibility = EVisibility::Visible;
	if (!SearchToken.IsEmpty())
	{
		if (InItem->ToLower().Find(SearchToken) < 0)
		{
			WidgetVisibility = EVisibility::Collapsed;
		}
	}
	
	TAttribute<FText> OnGetToolTip = TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateSP(this, &SDynamicDataTableStructComboBox::GetTooltipText, InItem));

	return SNew(SComboRow<TSharedPtr<FString>>, OwnerTable)
		.Style(ItemStyle)
		.Visibility(WidgetVisibility)
		.Padding(MenuRowPadding)
		[
			SNew(STextBlock)
			.Text(FText::FromString(*InItem))
			.ToolTipText(OnGetToolTip)
		];
}

void SDynamicDataTableStructComboBox::OnMenuOpenChanged(bool bOpen)
{
	if (bOpen == false)
	{
		if (TListTypeTraits<TSharedPtr<FString>>::IsPtrValid(SelectedItem))
		{
			// Ensure the ListView selection is set back to the last committed selection
			ComboListView->SetSelection(SelectedItem, ESelectInfo::OnNavigation);
			ComboListView->RequestScrollIntoView(SelectedItem, 0);
		}

		// Set focus back to ComboBox for users focusing the ListView that just closed
		TSharedRef<SWidget> ThisRef = AsShared();
		FSlateApplication::Get().ForEachUser([&ThisRef](FSlateUser& User) {
			if (User.HasFocusedDescendants(ThisRef))
			{
				User.SetFocus(ThisRef, EFocusCause::SetDirectly);
			}
		});
	}
}

void SDynamicDataTableStructComboBox::OnSelectionChanged_Internal(TSharedPtr<FString> ProposedSelection, ESelectInfo::Type SelectInfo)
{
	// Ensure that the proposed selection is different
	if (SelectInfo != ESelectInfo::OnNavigation)
	{
		// Ensure that the proposed selection is different from selected
		if (ProposedSelection != SelectedItem)
		{
			SelectedItem = ProposedSelection;
			
			UScriptStruct* SelectedStruct = GetOrLoadStruct(FindAssetDataForString(SelectedItem));

			OnStructSelected.ExecuteIfBound(SelectedStruct);

		}
		// close combo even if user reselected item
		this->SetIsOpen(false);
	}
}

void SDynamicDataTableStructComboBox::OnSearchTextChanged(const FText& ChangedText)
{
	FString SearchToken = ChangedText.ToString().ToLower();
	for (int32 i = 0; i < CurrentOptions.Num(); i++)
	{
		TSharedPtr<ITableRow> Row = ComboListView->WidgetFromItem(CurrentOptions[i]);
		if (Row)
		{
			if (SearchToken.IsEmpty())
			{
				Row->AsWidget()->SetVisibility(EVisibility::Visible);
			}
			else if (CurrentOptions[i]->ToLower().Find(SearchToken) >= 0)
			{
				Row->AsWidget()->SetVisibility(EVisibility::Visible);
			}
			else
			{
				Row->AsWidget()->SetVisibility(EVisibility::Collapsed);
			}
		}
	}

	ComboListView->RequestListRefresh();

	SelectedItem = TSharedPtr< FString >();
}

FReply SDynamicDataTableStructComboBox::OnButtonClicked()
{
	// if user clicked to close the combo menu
	if (this->IsOpen())
	{
		// Re-select first selected item, just in case it was selected by navigation previously
		TArray<TSharedPtr<FString>> SelectedItems = ComboListView->GetSelectedItems();
		if (SelectedItems.Num() > 0)
		{
			OnSelectionChanged_Internal(SelectedItems[0], ESelectInfo::Direct);
		}
	}
	else
	{
		SearchField->SetText(FText::GetEmpty());
		RefreshOptions();
	}

	return SComboButton::OnButtonClicked();
}

const FString FDynamicDataTableEditorUtils::VariableTypesTooltipDocLink = TEXT("Shared/Editor/Blueprint/VariableTypes");

TSharedRef<SWidget> FDynamicDataTableEditorUtils::MakeRowStructureComboBox(FOnDynamicDataTableStructSelected OnSelected)
{
	TSharedRef<SDynamicDataTableStructComboBox> ComboBox = SNew(SDynamicDataTableStructComboBox)
		.OnStructSelected(OnSelected);

	return ComboBox;
}

FDynamicDataTableEditorUtils::FDynamicDataTableEditorManager& FDynamicDataTableEditorUtils::FDynamicDataTableEditorManager::Get()
{
	static TSharedRef< FDynamicDataTableEditorManager > EditorManager(new FDynamicDataTableEditorManager());
	return *EditorManager;
}

bool FDynamicDataTableEditorUtils::RemoveRow(UDynamicDataTable* DynamicDataTable, FName Name)
{
	bool bResult = false;
	if (DynamicDataTable && DynamicDataTable->RowStruct)
	{
		const FScopedTransaction Transaction(LOCTEXT("RemoveDynamicDataTableRow", "Remove Data Table Row"));

		BroadcastPreChange(DynamicDataTable, EDynamicDataTableChangeInfo::RowList);
		DynamicDataTable->Modify();
		uint8* RowData = nullptr;
		const bool bRemoved = DynamicDataTable->GetNonConstRowMap().RemoveAndCopyValue(Name, RowData);
		if (bRemoved && RowData)
		{
			DynamicDataTable->RowStruct->DestroyStruct(RowData);
			FMemory::Free(RowData);
			bResult = true;

			// Compact the map so that a subsequent add goes at the end of the table
			DynamicDataTable->GetNonConstRowMap().CompactStable();
		}
		BroadcastPostChange(DynamicDataTable, EDynamicDataTableChangeInfo::RowList);
	}
	return bResult;
}

uint8* FDynamicDataTableEditorUtils::AddRow(UDynamicDataTable* DynamicDataTable, FName RowName)
{
	if (!DynamicDataTable || (RowName == NAME_None) || (DynamicDataTable->GetRowMap().Find(RowName) != nullptr) || !DynamicDataTable->RowStruct)
	{
		return nullptr;
	}

	const FScopedTransaction Transaction(LOCTEXT("AddDynamicDataTableRow", "Add Data Table Row"));

	BroadcastPreChange(DynamicDataTable, EDynamicDataTableChangeInfo::RowList);
	DynamicDataTable->Modify();
	// Allocate data to store information, using UScriptStruct to know its size
	uint8* RowData = (uint8*)FMemory::Malloc(DynamicDataTable->RowStruct->GetStructureSize());
	DynamicDataTable->RowStruct->InitializeStruct(RowData);
	// And be sure to call DestroyScriptStruct later

	// Add to row map
	DynamicDataTable->AddRowInternal(RowName, RowData);
	BroadcastPostChange(DynamicDataTable, EDynamicDataTableChangeInfo::RowList);
	return RowData;
}

uint8* FDynamicDataTableEditorUtils::AddRowAboveOrBelowSelection(UDynamicDataTable* DynamicDataTable, const FName& RowName, const FName& NewRowName, EDynamicDataTableRowInsertionPosition InsertPosition)
{
	if (!DynamicDataTable || (NewRowName == NAME_None) || (DynamicDataTable->GetRowMap().Find(NewRowName) != nullptr) || !DynamicDataTable->RowStruct)
	{
		return nullptr;
	}

	const FScopedTransaction Transaction(LOCTEXT("AddDynamicDataTableRowAboveBelow", "Add Data Table Row Above or Below"));

	TArray<FName> OrderedRowNames;
	DynamicDataTable->GetRowMap().GenerateKeyArray(OrderedRowNames);

	int32 CurrentRowIndex = OrderedRowNames.IndexOfByKey(RowName);
	if (CurrentRowIndex == INDEX_NONE)
	{
		return nullptr;
	}

	if (InsertPosition == EDynamicDataTableRowInsertionPosition::Below)
	{
		CurrentRowIndex += 1;
	}

	OrderedRowNames.Insert(NewRowName, CurrentRowIndex);
	
	// Build a name -> index map as the KeySort will hit this a lot
	TMap<FName, int32> NamesToNewIndex;
	for (int32 NameIndex = 0; NameIndex < OrderedRowNames.Num(); ++NameIndex)
	{
		NamesToNewIndex.Add(OrderedRowNames[NameIndex], NameIndex);
	}
	
	
	BroadcastPreChange(DynamicDataTable, EDynamicDataTableChangeInfo::RowList);
	
	DynamicDataTable->Modify();
	
	// Allocate data to store information, using UScriptStruct to know its size
	uint8* RowData = (uint8*)FMemory::Malloc(DynamicDataTable->RowStruct->GetStructureSize());

	// And be sure to call DestroyScriptStruct later
	DynamicDataTable->RowStruct->InitializeStruct(RowData);

	// Add to row map
	DynamicDataTable->AddRowInternal(NewRowName, RowData);

	// Re-sort the map keys to match the new order
	DynamicDataTable->GetNonConstRowMap().KeySort([&NamesToNewIndex](const FName& One, const FName& Two) -> bool
	{
		const int32 OneIndex = NamesToNewIndex.FindRef(One);
		const int32 TwoIndex = NamesToNewIndex.FindRef(Two);
		return OneIndex < TwoIndex;
	});

	BroadcastPostChange(DynamicDataTable, EDynamicDataTableChangeInfo::RowList);

	return RowData;
}


uint8* FDynamicDataTableEditorUtils::DuplicateRow(UDynamicDataTable* DynamicDataTable, FName SourceRowName, FName RowName)
{
	if (!DynamicDataTable || (SourceRowName == NAME_None) || !DynamicDataTable->RowMap.Contains(SourceRowName) || DynamicDataTable->RowMap.Contains(RowName) || !DynamicDataTable->RowStruct)
	{
		return NULL;
	}

	const FScopedTransaction Transaction(LOCTEXT("DuplicateDynamicDataTableRow", "Duplicate Data Table Row"));

	BroadcastPreChange(DynamicDataTable, EDynamicDataTableChangeInfo::RowList);
	DynamicDataTable->Modify();

	// Allocate data to store information, using UScriptStruct to know its size
	uint8* OldRowData = *DynamicDataTable->RowMap.Find(SourceRowName);
	uint8* NewRowData = (uint8*)FMemory::Malloc(DynamicDataTable->RowStruct->GetStructureSize());

	DynamicDataTable->RowStruct->InitializeStruct(NewRowData);
	DynamicDataTable->RowStruct->CopyScriptStruct(NewRowData, OldRowData);

	// Add to row map
	DynamicDataTable->RowMap.Add(RowName, NewRowData);
	BroadcastPostChange(DynamicDataTable, EDynamicDataTableChangeInfo::RowList);
	return NewRowData;
}

bool FDynamicDataTableEditorUtils::RenameRow(UDynamicDataTable* DynamicDataTable, FName OldName, FName NewName)
{
	bool bResult = false;
	if (DynamicDataTable)
	{
		const FScopedTransaction Transaction(LOCTEXT("RenameDynamicDataTableRow", "Rename Data Table Row"));

		BroadcastPreChange(DynamicDataTable, EDynamicDataTableChangeInfo::RowList);
		DynamicDataTable->Modify();

		uint8* RowData = nullptr;
		const bool bValidnewName = (NewName != NAME_None) && !DynamicDataTable->GetRowMap().Find(NewName);
		const bool bRemoved = bValidnewName && DynamicDataTable->GetNonConstRowMap().RemoveAndCopyValue(OldName, RowData);
		if (bRemoved)
		{
			DynamicDataTable->GetNonConstRowMap().FindOrAdd(NewName) = RowData;
			bResult = true;
		}
		BroadcastPostChange(DynamicDataTable, EDynamicDataTableChangeInfo::RowList);
	}
	return bResult;
}

bool FDynamicDataTableEditorUtils::MoveRow(UDynamicDataTable* DynamicDataTable, FName RowName, ERowMoveDirection Direction, int32 NumRowsToMoveBy)
{
	if (!DynamicDataTable)
	{
		return false;
	}
	
	// Our maps are ordered which is why we can get away with this
	// If we ever change our map implementation, we'll need to preserve this order information in a separate array and 
	// make sure that order dependent code (such as exporting and the data table viewer) use that when dealing with rows
	// This may also require making RowMap private and fixing up all the existing code that references it directly
	TArray<FName> OrderedRowNames;
	DynamicDataTable->GetRowMap().GenerateKeyArray(OrderedRowNames);

	const int32 CurrentRowIndex = OrderedRowNames.IndexOfByKey(RowName);
	if (CurrentRowIndex == INDEX_NONE)
	{
		return false;
	}
	
	// Calculate our new row index, clamped to the available rows
	int32 NewRowIndex = INDEX_NONE;
	switch(Direction)
	{
	case ERowMoveDirection::Up:
		NewRowIndex = FMath::Clamp(CurrentRowIndex - NumRowsToMoveBy, 0, OrderedRowNames.Num() - 1);
		break;

	case ERowMoveDirection::Down:
		NewRowIndex = FMath::Clamp(CurrentRowIndex + NumRowsToMoveBy, 0, OrderedRowNames.Num() - 1);
		break;

	default:
		break;
	}

	if (NewRowIndex == INDEX_NONE)
	{
		return false;
	}

	if (CurrentRowIndex == NewRowIndex)
	{
		// Nothing to do, but not an error
		return true;
	}

	// Swap the order around as requested
	OrderedRowNames.RemoveAt(CurrentRowIndex, 1, false);
	OrderedRowNames.Insert(RowName, NewRowIndex);

	// Build a name -> index map as the KeySort will hit this a lot
	TMap<FName, int32> NamesToNewIndex;
	for (int32 NameIndex = 0; NameIndex < OrderedRowNames.Num(); ++NameIndex)
	{
		NamesToNewIndex.Add(OrderedRowNames[NameIndex], NameIndex);
	}

	const FScopedTransaction Transaction(LOCTEXT("MoveDynamicDataTableRow", "Move Data Table Row"));

	BroadcastPreChange(DynamicDataTable, EDynamicDataTableChangeInfo::RowList);
	DynamicDataTable->Modify();

	// Re-sort the map keys to match the new order
	DynamicDataTable->GetNonConstRowMap().KeySort([&NamesToNewIndex](const FName& One, const FName& Two) -> bool
	{
		const int32 OneIndex = NamesToNewIndex.FindRef(One);
		const int32 TwoIndex = NamesToNewIndex.FindRef(Two);
		return OneIndex < TwoIndex;
	});

	BroadcastPostChange(DynamicDataTable, EDynamicDataTableChangeInfo::RowList);

	return true;
}

bool FDynamicDataTableEditorUtils::SelectRow(const UDynamicDataTable* DynamicDataTable, FName RowName)
{
	for (auto Listener : FDynamicDataTableEditorManager::Get().GetListeners())
	{
		static_cast<INotifyOnDynamicDataTableChanged*>(Listener)->SelectionChange(DynamicDataTable, RowName);
	}
	return true;
}

bool FDynamicDataTableEditorUtils::DiffersFromDefault(UDynamicDataTable* DynamicDataTable, FName RowName)
{
	bool bDiffers = false;

	if (DynamicDataTable && DynamicDataTable->GetRowMap().Contains(RowName))
	{
		uint8* RowData = DynamicDataTable->GetRowMap()[RowName];

		if (const UUserDefinedStruct* UDStruct = Cast<const UUserDefinedStruct>(DynamicDataTable->RowStruct))
		{
			return !UDStruct->CompareScriptStruct(RowData, UDStruct->GetDefaultInstance(), PPF_None);
		}
	}

	return bDiffers;
}

bool FDynamicDataTableEditorUtils::ResetToDefault(UDynamicDataTable* DynamicDataTable, FName RowName)
{
	bool bResult = false;

	if (DynamicDataTable && DynamicDataTable->GetRowMap().Contains(RowName))
	{
		const FScopedTransaction Transaction(LOCTEXT("ResetDynamicDataTableRowToDefault", "Reset Data Table Row to Default Values"));

		BroadcastPreChange(DynamicDataTable, EDynamicDataTableChangeInfo::RowData);
		DynamicDataTable->Modify();

		uint8* RowData = DynamicDataTable->GetRowMap()[RowName];

		if (const UUserDefinedStruct* UDStruct = Cast<const UUserDefinedStruct>(DynamicDataTable->RowStruct))
		{
			UDStruct->InitializeDefaultValue(RowData);
			bResult = true;
		}

		BroadcastPostChange(DynamicDataTable, EDynamicDataTableChangeInfo::RowData);
	}

	return bResult;
}

void FDynamicDataTableEditorUtils::BroadcastPreChange(UDynamicDataTable* DynamicDataTable, EDynamicDataTableChangeInfo Info)
{
	FDynamicDataTableEditorManager::Get().PreChange(DynamicDataTable, Info);
}

void FDynamicDataTableEditorUtils::BroadcastPostChange(UDynamicDataTable* DynamicDataTable, EDynamicDataTableChangeInfo Info)
{
	if (DynamicDataTable && (EDynamicDataTableChangeInfo::RowList == Info))
	{
		//for (TObjectIterator<UK2Node_GetDynamicDataTableRow> It(RF_Transient | RF_ClassDefaultObject, /** bIncludeDerivedClasses */ true, /** InternalExcludeFlags */ EInternalObjectFlags::Garbage); It; ++It)
		//{
		//	It->OnDynamicDataTableRowListChanged(DynamicDataTable);
		//}
	}
	FDynamicDataTableEditorManager::Get().PostChange(DynamicDataTable, Info);
	DynamicDataTable->OnDynamicDataTableChanged().Broadcast();
}

void FDynamicDataTableEditorUtils::CacheDynamicDataTableForEditing(const UDynamicDataTable* DynamicDataTable, TArray<FDynamicDataTableEditorColumnHeaderDataPtr>& OutAvailableColumns, TArray<FDynamicDataTableEditorRowListViewDataPtr>& OutAvailableRows)
{
	if (!DynamicDataTable || !DynamicDataTable->RowStruct)
	{
		OutAvailableColumns.Empty();
		OutAvailableRows.Empty();
		return;
	}

	CacheDataForEditing(DynamicDataTable->RowStruct, DynamicDataTable->GetRowMap(), OutAvailableColumns, OutAvailableRows);
}

void FDynamicDataTableEditorUtils::CacheDataForEditing(const UScriptStruct* RowStruct, const TMap<FName, uint8*>& RowMap, TArray<FDynamicDataTableEditorColumnHeaderDataPtr>& OutAvailableColumns, TArray<FDynamicDataTableEditorRowListViewDataPtr>& OutAvailableRows)
{
	TArray<FDynamicDataTableEditorColumnHeaderDataPtr> OldColumns = OutAvailableColumns;
	TArray<FDynamicDataTableEditorRowListViewDataPtr> OldRows = OutAvailableRows;

	// First build array of properties
	TArray<const FProperty*> StructProps;
	for (TFieldIterator<const FProperty> It(RowStruct); It; ++It)
	{
		const FProperty* Prop = *It;
		check(Prop);
		if (!Prop->HasMetaData(FName(TEXT("HideFromDynamicDataTableEditorColumn"))))
		{
			StructProps.Add(Prop);
		}
	}

	TSharedRef<FSlateFontMeasure> FontMeasure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
	const FTextBlockStyle& CellTextStyle = FAppStyle::GetWidgetStyle<FTextBlockStyle>("DataTableEditor.CellText");
	static const float CellPadding = 10.0f;

	// Populate the column data
	OutAvailableColumns.Reset(StructProps.Num());
	for (int32 Index = 0; Index < StructProps.Num(); ++Index)
	{
		const FProperty* Prop = StructProps[Index];
		const FText PropertyDisplayName = DynamicDataTableUtils::GetPropertyDisplayName(Prop, FName::NameToDisplayString(Prop->GetName(), Prop->IsA<FBoolProperty>()));

		FDynamicDataTableEditorColumnHeaderDataPtr CachedColumnData;
		
		// If at all possible, attempt to reuse previous columns if their data has not changed
		if (Index >= OldColumns.Num() || OldColumns[Index]->ColumnId != Prop->GetFName() || !OldColumns[Index]->DisplayName.EqualTo(PropertyDisplayName))
		{
			CachedColumnData = MakeShareable(new FDynamicDataTableEditorColumnHeaderData());
			CachedColumnData->ColumnId = Prop->GetFName();
			CachedColumnData->DisplayName = PropertyDisplayName;
			CachedColumnData->Property = Prop;
		}
		else
		{
			CachedColumnData = OldColumns[Index];

			// Need to update property hard pointer in case it got reconstructed
			CachedColumnData->Property = Prop;
		}

		CachedColumnData->DesiredColumnWidth = FontMeasure->Measure(CachedColumnData->DisplayName, CellTextStyle.Font).X + CellPadding;

		OutAvailableColumns.Add(CachedColumnData);
	}

	// Populate the row data
	OutAvailableRows.Reset(RowMap.Num());
	int32 Index = 0;
	for (auto RowIt = RowMap.CreateConstIterator(); RowIt; ++RowIt, ++Index)
	{
		FText RowName = FText::FromName(RowIt->Key);
		FDynamicDataTableEditorRowListViewDataPtr CachedRowData;

		// If at all possible, attempt to reuse previous rows if their data has not changed.
		if (Index >= OldRows.Num() || OldRows[Index]->RowId != RowIt->Key || !OldRows[Index]->DisplayName.EqualTo(RowName))
		{
			CachedRowData = MakeShareable(new FDynamicDataTableEditorRowListViewData());
			CachedRowData->RowId = RowIt->Key;
			CachedRowData->DisplayName = RowName;
			CachedRowData->CellData.Reserve(StructProps.Num());
		}
		else
		{
			CachedRowData = OldRows[Index];
			CachedRowData->CellData.Reset(StructProps.Num());
		}

		CachedRowData->DesiredRowHeight = FontMeasure->GetMaxCharacterHeight(CellTextStyle.Font);
		CachedRowData->RowNum = Index + 1;

		// Always rebuild cell data
		{
			const uint8* RowData = RowIt.Value();
			for (int32 ColumnIndex = 0; ColumnIndex < StructProps.Num(); ++ColumnIndex)
			{
				const FProperty* Prop = StructProps[ColumnIndex];
				FDynamicDataTableEditorColumnHeaderDataPtr CachedColumnData = OutAvailableColumns[ColumnIndex];

				const FText CellText = DynamicDataTableUtils::GetPropertyValueAsText(Prop, RowData);
				CachedRowData->CellData.Add(CellText);

				const FVector2D CellTextSize = FontMeasure->Measure(CellText, CellTextStyle.Font);

				CachedRowData->DesiredRowHeight = FMath::Max(CachedRowData->DesiredRowHeight, CellTextSize.Y);

				const float CellWidth = CellTextSize.X + CellPadding;
				CachedColumnData->DesiredColumnWidth = FMath::Max(CachedColumnData->DesiredColumnWidth, CellWidth);
			}
		}

		OutAvailableRows.Add(CachedRowData);
	}
}

TArray<UScriptStruct*> FDynamicDataTableEditorUtils::GetPossibleStructs()
{
	TArray< UScriptStruct* > RowStructs;

	// Make combo of table rowstruct options
	for (TObjectIterator<UScriptStruct> It; It; ++It)
	{
		UScriptStruct* Struct = *It;
		if (IsValidTableStruct(Struct))
		{
			RowStructs.Add(Struct);
		}
	}

	RowStructs.Sort();

	return RowStructs;
}

void FDynamicDataTableEditorUtils::GetPossibleStructAssetData(TArray<FAssetData>& StructAssets)
{
	StructAssets.Reset();

	// Make combo of table rowstruct options
	for (TObjectIterator<UScriptStruct> It; It; ++It)
	{
		UScriptStruct* Struct = *It;
		if (IsValidTableStruct(Struct))
		{
			StructAssets.Add(FAssetData(Struct));
		}
	}

	// Now get unloaded ones
	const FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	TArray<FAssetData> AssetData;
	AssetRegistryModule.Get().GetAssetsByClass(UUserDefinedStruct::StaticClass()->GetClassPathName(), AssetData);

	for (int32 AssetIndex = 0; AssetIndex < AssetData.Num(); ++AssetIndex)
	{
		const FAssetData& Asset = AssetData[AssetIndex];
		if (Asset.IsValid() && !Asset.IsAssetLoaded())
		{
			StructAssets.Add(Asset);
		}
	}

	StructAssets.Sort([](const FAssetData& A, const FAssetData& B) { return A.AssetName.LexicalLess(B.AssetName); });
}

bool FDynamicDataTableEditorUtils::IsValidTableStruct(const UScriptStruct* Struct)
{
	const UScriptStruct* TableRowStruct = FDynamicTableRowBase::StaticStruct();

	// If a child of the table row struct base, but not itself
	const bool bBasedOnTableRowBase = TableRowStruct && Struct->IsChildOf(TableRowStruct) && (Struct != TableRowStruct);
	const bool bUDStruct = Struct->IsA<UUserDefinedStruct>();
	const bool bValidStruct = (Struct->GetOutermost() != GetTransientPackage());

	return (bBasedOnTableRowBase || bUDStruct) && bValidStruct;
}

void FDynamicDataTableEditorUtils::AddSearchForReferencesContextMenu(FDetailWidgetRow& RowNameDetailWidget, FExecuteAction SearchForReferencesAction)
{
	if (SearchForReferencesAction.IsBound() && FEditorDelegates::OnOpenReferenceViewer.IsBound())
	{
		RowNameDetailWidget.AddCustomContextMenuAction(FUIAction(SearchForReferencesAction),
			NSLOCTEXT("FDynamicDataTableRowUtils", "FDynamicDataTableRowUtils_SearchForReferences", "Find Row References"),
			NSLOCTEXT("FDynamicDataTableRowUtils", "FDynamicDataTableRowUtils_SearchForReferencesTooltip", "Find assets that reference this Row"),
			FSlateIcon());
	}
}

FText FDynamicDataTableEditorUtils::GetHandleShortDescription(const UObject* TableAsset, FName RowName)
{
	FText TableNameText = LOCTEXT("Description_None", "None");
	FText RowNameText = TableNameText;
	const int32 MaxChars = 15;
	FString More = TEXT("...");

	if (!TableAsset && RowName.IsNone())
	{
		// Just display None on it's own
		return TableNameText;
	}

	if (TableAsset)
	{
		FString TempString = TableAsset->GetName();

		// Chop off end if needed
		if (TempString.Len() > MaxChars)
		{
			TempString.LeftInline(MaxChars - More.Len());
			TempString.Append(More);
		}

		TableNameText = FText::AsCultureInvariant(TempString);
	}

	if (!RowName.IsNone())
	{
		FString TempString = RowName.ToString();

		// Show right side if too long, usually more important
		if (TempString.Len() > MaxChars)
		{
			TempString.RightInline(MaxChars - More.Len());
			TempString.InsertAt(0, More);
		}

		RowNameText = FText::AsCultureInvariant(TempString);
	}

	return FText::Format(LOCTEXT("HandlePreviewFormat", "{0}[{1}]"), TableNameText, RowNameText);
}

FText FDynamicDataTableEditorUtils::GetRowTypeInfoTooltipText(FDynamicDataTableEditorColumnHeaderDataPtr ColumnHeaderDataPtr)
{
	if (ColumnHeaderDataPtr.IsValid())
	{
		const FProperty* Property = ColumnHeaderDataPtr->Property;
		if (Property)
		{
			const FFieldClass* PropertyClass = Property->GetClass();
			const FStructProperty* StructProp = CastField<const FStructProperty>(Property);
			if (StructProp)
			{
				FString TypeName = FName::NameToDisplayString(Property->GetCPPType(), Property->IsA<FBoolProperty>());
				if (TypeName.Len())
				{
					// If type name starts with F and another capital letter, assume standard naming and remove F in the string shown to the user
					if (TypeName.StartsWith("F", ESearchCase::CaseSensitive) && TypeName.Len() > 1 && FChar::IsUpper(TypeName.GetCharArray()[1]))
					{
						TypeName.RemoveFromStart("F");
					}
					return FText::FromString(TypeName);
				}
			}
			if (PropertyClass)
			{
				return FText::FromString(PropertyClass->GetDescription());
			}
			
		}
	}

	return FText::GetEmpty();
}

FString FDynamicDataTableEditorUtils::GetRowTypeTooltipDocExcerptName(FDynamicDataTableEditorColumnHeaderDataPtr ColumnHeaderDataPtr)
{
	if (ColumnHeaderDataPtr.IsValid())
	{
		const FProperty* Property = ColumnHeaderDataPtr->Property;
		if (Property)
		{
			const FStructProperty* StructProp = CastField<const FStructProperty>(Property);
			if (StructProp)
			{
				if (StructProp->Struct == TBaseStructure<FSoftObjectPath>::Get())
				{
					return "SoftObject";
				}
				if (StructProp->Struct == TBaseStructure<FSoftClassPath>::Get())
				{
					return "SoftClass";
				}
				FString TypeName = FName::NameToDisplayString(Property->GetCPPType(), Property->IsA<FBoolProperty>());
				if (TypeName.Len())
				{
					// If type name starts with F and another capital letter, assume standard naming and remove F to match the doc excerpt name
					if (TypeName.StartsWith("F", ESearchCase::CaseSensitive) && TypeName.Len() > 1 && FChar::IsUpper(TypeName.GetCharArray()[1]))
					{
						TypeName.RemoveFromStart("F");
					}
					return TypeName;
				}
			}
			const FFieldClass* PropertyClass = Property->GetClass();
			if (PropertyClass)
			{
				if (PropertyClass == FStrProperty::StaticClass())
				{
					return "String";
				}
				FString PropertyClassName = PropertyClass->GetName();
				PropertyClassName.RemoveFromEnd("Property");
				return PropertyClassName;
			}
		}
	}

	return "";
}

#undef LOCTEXT_NAMESPACE
