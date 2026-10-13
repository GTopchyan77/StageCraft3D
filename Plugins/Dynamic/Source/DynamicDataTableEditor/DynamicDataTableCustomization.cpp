// Copyright Epic Games, Inc. All Rights Reserved.

#include "DynamicDataTableCustomization.h"

#include "AssetRegistry/AssetData.h"
#include "Containers/Map.h"
#include "DynamicDataTableEditorUtils.h"
#include "Delegates/Delegate.h"
#include "DetailWidgetRow.h"
#include "Editor.h"
#include "DynamicDatatable/DynamicDataTable/DynamicDataTable.h"
#include "Fonts/SlateFontInfo.h"
#include "Framework/Commands/UIAction.h"
#include "HAL/Platform.h"
#include "HAL/PlatformCrt.h"
#include "IDetailChildrenBuilder.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/Text.h"
#include "Misc/Attribute.h"
#include "PropertyCustomizationHelpers.h"
#include "PropertyEditorModule.h"
#include "PropertyHandle.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/Object.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Text/STextBlock.h"

class SToolTip;

#define LOCTEXT_NAMESPACE "FDynamicDataTableCustomizationLayout"

void FDynamicDataTableCustomizationLayout::CustomizeHeader(TSharedRef<class IPropertyHandle> InStructPropertyHandle, class FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& StructCustomizationUtils)
{
	this->StructPropertyHandle = InStructPropertyHandle;

	if (StructPropertyHandle->HasMetaData(TEXT("RowType")))
	{
		const FString& RowType = StructPropertyHandle->GetMetaData(TEXT("RowType"));
		RowTypeFilter = FName(*RowType);
		RowFilterStruct = UClass::TryFindTypeSlow<UScriptStruct>(RowType);
	}

	FSimpleDelegate OnDynamicDataTableChangedDelegate = FSimpleDelegate::CreateSP(this, &FDynamicDataTableCustomizationLayout::OnDynamicDataTableChanged);
	StructPropertyHandle->SetOnPropertyValueChanged(OnDynamicDataTableChangedDelegate);
	
	HeaderRow
	.NameContent()
	[
		InStructPropertyHandle->CreatePropertyNameWidget()
	];

	FDynamicDataTableEditorUtils::AddSearchForReferencesContextMenu(HeaderRow, FExecuteAction::CreateSP(this, &FDynamicDataTableCustomizationLayout::OnSearchForReferences));
}

void FDynamicDataTableCustomizationLayout::CustomizeChildren(TSharedRef<class IPropertyHandle> InStructPropertyHandle, class IDetailChildrenBuilder& StructBuilder, IPropertyTypeCustomizationUtils& StructCustomizationUtils)
{
	/** Get all the existing property handles */
	DynamicDataTablePropertyHandle = InStructPropertyHandle->GetChildHandle("DynamicDataTable");
	RowNamePropertyHandle = InStructPropertyHandle->GetChildHandle("RowName");

	if (DynamicDataTablePropertyHandle->IsValidHandle() && RowNamePropertyHandle->IsValidHandle())
	{
		/** Setup Change callback */
		FSimpleDelegate OnDynamicDataTableChangedDelegate = FSimpleDelegate::CreateSP(this, &FDynamicDataTableCustomizationLayout::OnDynamicDataTableChanged);
		DynamicDataTablePropertyHandle->SetOnPropertyValueChanged(OnDynamicDataTableChangedDelegate);

		/** Construct a asset picker widget with a custom filter */
		StructBuilder.AddCustomRow(LOCTEXT("DynamicDataTable_TableName", "Data Table"))
			.NameContent()
			[
				SNew(STextBlock)
				.Text(LOCTEXT("DynamicDataTable_TableName", "Data Table"))
				.Font(StructCustomizationUtils.GetRegularFont())
			]
			.ValueContent()
			.MaxDesiredWidth(0.0f) // don't constrain the combo button width
			[
				SNew(SObjectPropertyEntryBox)
				.PropertyHandle(DynamicDataTablePropertyHandle)
				.AllowedClass(UDynamicDataTable::StaticClass())
				.OnShouldFilterAsset(this, &FDynamicDataTableCustomizationLayout::ShouldFilterAsset)
			];

		FPropertyComboBoxArgs ComboArgs(RowNamePropertyHandle, 
			FOnGetPropertyComboBoxStrings::CreateSP(this, &FDynamicDataTableCustomizationLayout::OnGetRowStrings), 
			FOnGetPropertyComboBoxValue::CreateSP(this, &FDynamicDataTableCustomizationLayout::OnGetRowValueString));
		ComboArgs.ShowSearchForItemCount = 1;

		/** Construct a combo box widget to select from a list of valid options */
		StructBuilder.AddCustomRow(LOCTEXT("DynamicDataTable_RowName", "Row Name"))
			.NameContent()
			[
				SNew(STextBlock)
				.Text(LOCTEXT("DynamicDataTable_RowName", "Row Name"))
				.Font(StructCustomizationUtils.GetRegularFont())
			]
			.ValueContent()
			.MaxDesiredWidth(0.0f) // don't constrain the combo button width
			[
				PropertyCustomizationHelpers::MakePropertyComboBox(ComboArgs)
			];
	}
}

bool FDynamicDataTableCustomizationLayout::GetCurrentValue(UDynamicDataTable*& OutDynamicDataTable, FName& OutName) const
{
	if (RowNamePropertyHandle.IsValid() && RowNamePropertyHandle->IsValidHandle() && DynamicDataTablePropertyHandle.IsValid() && DynamicDataTablePropertyHandle->IsValidHandle())
	{
		// If either handle is multiple value or failure, fail
		UObject* SourceDynamicDataTable = nullptr;
		if (DynamicDataTablePropertyHandle->GetValue(SourceDynamicDataTable) == FPropertyAccess::Success)
		{
			OutDynamicDataTable = Cast<UDynamicDataTable>(SourceDynamicDataTable);

			if (RowNamePropertyHandle->GetValue(OutName) == FPropertyAccess::Success)
			{
				return true;
			}
		}
	}
	return false;
}

void FDynamicDataTableCustomizationLayout::OnSearchForReferences()
{
	UDynamicDataTable* DynamicDataTable;
	FName RowName;

	if (GetCurrentValue(DynamicDataTable, RowName) && DynamicDataTable)
	{
		TArray<FAssetIdentifier> AssetIdentifiers;
		AssetIdentifiers.Add(FAssetIdentifier(DynamicDataTable, RowName));

		FEditorDelegates::OnOpenReferenceViewer.Broadcast(AssetIdentifiers, FReferenceViewerParams());
	}
}

FString FDynamicDataTableCustomizationLayout::OnGetRowValueString() const
{
	if (!RowNamePropertyHandle.IsValid() || !RowNamePropertyHandle->IsValidHandle())
	{
		return FString();
	}

	FName RowNameValue;
	const FPropertyAccess::Result RowResult = RowNamePropertyHandle->GetValue(RowNameValue);
	if (RowResult == FPropertyAccess::Success)
	{
		if (RowNameValue.IsNone())
		{
			return LOCTEXT("DynamicDataTable_None", "None").ToString();
		}
		return RowNameValue.ToString();
	}
	else if (RowResult == FPropertyAccess::Fail)
	{
		return LOCTEXT("DynamicDataTable_None", "None").ToString();
	}
	else
	{
		return LOCTEXT("MultipleValues", "Multiple Values").ToString();
	}
}

void FDynamicDataTableCustomizationLayout::OnGetRowStrings(TArray< TSharedPtr<FString> >& OutStrings, TArray<TSharedPtr<SToolTip>>& OutToolTips, TArray<bool>& OutRestrictedItems) const
{
	UDynamicDataTable* DynamicDataTable = nullptr;
	FName IgnoredRowName;

	// Ignore return value as we will show rows if table is the same but row names are multiple values
	GetCurrentValue(DynamicDataTable, IgnoredRowName);

	TArray<FName> AllRowNames;
	if (DynamicDataTable != nullptr)
	{
		for (TMap<FName, uint8*>::TConstIterator Iterator(DynamicDataTable->GetRowMap()); Iterator; ++Iterator)
		{
			AllRowNames.Add(Iterator.Key());
		}

		// Sort the names alphabetically.
		AllRowNames.Sort(FNameLexicalLess());
	}

	for (const FName& RowName : AllRowNames)
	{
		OutStrings.Add(MakeShared<FString>(RowName.ToString()));
		OutRestrictedItems.Add(false);
	}
}

void FDynamicDataTableCustomizationLayout::OnDynamicDataTableChanged()
{
	UDynamicDataTable* CurrentTable;
	FName OldName;

	// Clear name on table change if no longer valid
	if (GetCurrentValue(CurrentTable, OldName))
	{
		if (!CurrentTable || !CurrentTable->FindRowUnchecked(OldName))
		{
			RowNamePropertyHandle->SetValue(FName());
		}
	}
}

bool FDynamicDataTableCustomizationLayout::ShouldFilterAsset(const struct FAssetData& AssetData)
{
	if (!RowTypeFilter.IsNone())
	{
		static const FName RowStructureTagName("RowStructure");
		FString RowStructure;
		if (AssetData.GetTagValue<FString>(RowStructureTagName, RowStructure))
		{
			if (RowStructure == RowTypeFilter.ToString())
			{
				return false;
			}

			// This is slow, but at the moment we don't have an alternative to the short struct name search
			UScriptStruct* RowStruct = UClass::TryFindTypeSlow<UScriptStruct>(RowStructure);
			if (RowStruct && RowFilterStruct && RowStruct->IsChildOf(RowFilterStruct))
			{
				return false;
			}
		}
		return true;
	}
	return false;
}

#undef LOCTEXT_NAMESPACE

