// Copyright(c) 2022 Dragon Li Software Limited. All Rights Reserved.


#include "DlFm_Item_Logic.h"

#include "DlFmMain_Logic.h"
#include "DlFm_Item_Data.h"
#include "Components/EditableText.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "UI/Fragment/DlFmFilteredEditableText.h"
#include "UI/Fragment/DlFmFilteredToggleButton.h"


void UDlFm_Item_Logic::NativeConstruct()
{
	Super::NativeConstruct();

	Button_Background->OnCheckStateChanged.AddUniqueDynamic(
		this, &UDlFm_Item_Logic::OnInternalClicked_Background);
	Text_ItemName->OnFilteredTextCommitted.AddUniqueDynamic(this, &UDlFm_Item_Logic::OnInternalCommit_Name);
	if (IsValid(Toggle_Selectable_InMultiMode))
	{
		Toggle_Selectable_InMultiMode->OnCheckStateChanged.AddUniqueDynamic(
			this, &UDlFm_Item_Logic::OnInternalChanged_Select);
	}
	Button_Background->SetClickMethod(EButtonClickMethod::PreciseClick);
	Button_Background->SetTouchMethod(EButtonTouchMethod::PreciseTap);
}

void UDlFm_Item_Logic::NativeOnListItemObjectSet(UObject* ListItemObject)
{
	CurrentItem = Cast<UDlFm_Item_Data>(ListItemObject);
	if (CurrentItem != nullptr)
	{
		SetStatus(*(CurrentItem->Item), CurrentItem->FixedIndex);
		SetModeStyle(*(CurrentItem->Item), CurrentItem->Env->GetCore().SelectMode,
		             CurrentItem->Env->GetCore().ExploreMode);
	}
}

void UDlFm_Item_Logic::SetStatus(const FDlUI_FileExplorer_ItemInfo& Item, const int32& FixedIndexVal)
{
	Text_ItemName->SetText(FText::FromString(Item.CleanName));
	Text_ItemName_Show->SetText(FText::FromString(Item.CleanName));
	Internal_SetNameEditable(false);
	if (IsValid(Text_ItemPath))
	{
		Text_ItemPath->SetText(FText::FromString(FPaths::GetPath(Item.Name)));
	}

	if (IsValid(Text_Extension))
	{
		if (Item.bIsDirectory)
		{
			Text_Extension->SetVisibility(ESlateVisibility::Hidden);
		}
		else
		{
			Text_Extension->SetText(FText::FromString(Item.Extension));
		}
	}

	if (IsValid(Image_Icon))
	{
		SetIconBrush(Item,Image_Icon);
	}

	if (IsValid(Text_AccessDate))
	{
		Text_AccessDate->SetText(FText::AsDate(Item.AccessTime));
	}

	if (IsValid(Text_CreationDate))
	{
		Text_CreationDate->SetText(FText::AsDate(Item.CreationTime));
	}

	if (IsValid(Text_ModificationDate))
	{
		Text_ModificationDate->SetText(FText::AsDate(Item.ModificationTime));
	}

	if (IsValid(WShow_ReadOnly))
	{
		if (Item.bIsReadOnly)
		{
			WShow_ReadOnly->SetVisibility(ESlateVisibility::Visible);
		}
		else
		{
			WShow_ReadOnly->SetVisibility(ESlateVisibility::Hidden);
		}
	}

	if (IsValid(Text_Size))
	{
		if (Item.bIsDirectory)
		{
			Text_Size->SetVisibility(ESlateVisibility::Hidden);
		}
		else
		{
			Text_Size->SetVisibility(ESlateVisibility::Visible);
			Text_Size->SetText(FText::AsMemory(Item.FileSize));
		}
	}

	if (IsValid(Button_Function))
	{
		Button_Function->OnFilteredClicked.AddUniqueDynamic(
			this, &UDlFm_Item_Logic::OnInternalClicked_FunctionButton);
	}
}

void UDlFm_Item_Logic::SetModeStyle(const FDlUI_FileExplorer_ItemInfo& Info,
                                           FDlUI_FileExplorer_Lib::ESelectMode SelectMode,
                                           FDlUI_FileExplorer_Lib::EExploreMode ExploreMode)
{
	SetVisibility(Info.bShouldShow ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);

	switch (SelectMode)
	{
	case FDlUI_FileExplorer_Lib::ESelectMode::Single:
		{
			Internal_SetNameEditable(false);
			if (IsValid(Toggle_Selectable_InMultiMode))
			{
				Toggle_Selectable_InMultiMode->SetVisibility(ESlateVisibility::Hidden);
			}
			if (IsValid(Button_Function))
			{
				Button_Function->SetVisibility(ESlateVisibility::Visible);
			}
			Button_Background->SetIsChecked_NoBroadcast(Info.bSelected);
		}
		break;
	case FDlUI_FileExplorer_Lib::ESelectMode::Multiply:
		{
			Internal_SetNameEditable(false);
			if (IsValid(Toggle_Selectable_InMultiMode))
			{
				Toggle_Selectable_InMultiMode->SetVisibility(ESlateVisibility::Visible);
				Toggle_Selectable_InMultiMode->SetIsChecked_NoBroadcast(Info.bSelected);
			}
			if (IsValid(Button_Function))
			{
				Button_Function->SetVisibility(ESlateVisibility::Hidden);
			}
			Button_Background->SetIsChecked_NoBroadcast(Info.bSelected);
		}
		break;
	default:
		if (IsValid(Toggle_Selectable_InMultiMode))
		{
			Toggle_Selectable_InMultiMode->SetVisibility(ESlateVisibility::Hidden);
		}
		if (IsValid(Button_Function))
		{
			Button_Function->SetVisibility(ESlateVisibility::Visible);
		}
		Button_Background->SetIsChecked_NoBroadcast(Info.bSelected);
		break;
	}

	switch (ExploreMode)
	{
	case FDlUI_FileExplorer_Lib::EExploreMode::Browse:
		if (IsValid(Text_ItemPath))
		{
			Text_ItemPath->SetVisibility(ESlateVisibility::Collapsed);
		}
		break;
	case FDlUI_FileExplorer_Lib::EExploreMode::Search:
		if (IsValid(Text_ItemPath))
		{
			Text_ItemPath->SetVisibility(ESlateVisibility::Visible);
		}
		break;
	default:
		if (IsValid(Text_ItemPath))
		{
			Text_ItemPath->SetVisibility(ESlateVisibility::Collapsed);
		}
		break;
	}
	if (Info.SpecialAction == FDlUI_FileExplorer_Lib::ESpecialAction::Rename)
	{
		Internal_SetNameEditable(true);
		Text_ItemName->SetFocus();
	}
	else
	{
		Internal_SetNameEditable(false);
	}
}

void UDlFm_Item_Logic::SetIconBrush_Implementation(const FDlUI_FileExplorer_ItemInfo& Item,UImage* Target_Image_Icon)
{
	if (Item.bIsDirectory)
	{
		Target_Image_Icon->SetBrush(IconBrush_Directory);
	}
	else
	{
		Target_Image_Icon->SetBrush(IconBrush_NormalFile);
		for (TTuple<FString, FSlateBrush> OneItem : IconBrush_SpecialFile)
		{
			if (Item.Name.EndsWith(OneItem.Key, ESearchCase::IgnoreCase))
			{
				Target_Image_Icon->SetBrush(OneItem.Value);
				break;
			}
		}
	}
}

void UDlFm_Item_Logic::OnInternalClicked_Background(bool Val)
{
	if(IsValid(CurrentItem))
	{
		CurrentItem->Env->OnInternal_ItemClicked(CurrentItem->FixedIndex,CurrentItem->Item->ListSegment);
	}
}

void UDlFm_Item_Logic::OnInternalCommit_Name(const FText& Text, ETextCommit::Type CommitMethod)
{
	if(IsValid(CurrentItem))
	{
		CurrentItem->Env->OnInternal_ItemRenameCommitted(CurrentItem->FixedIndex,CurrentItem->Item->ListSegment,Text);
	}
	Internal_SetNameEditable(false);
}

void UDlFm_Item_Logic::OnInternalChanged_Select(bool Val)
{
	if(IsValid(CurrentItem))
	{
		CurrentItem->Env->OnInternal_ItemClicked(CurrentItem->FixedIndex,CurrentItem->Item->ListSegment);
	}
}

void UDlFm_Item_Logic::OnInternalClicked_FunctionButton()
{
	if(IsValid(CurrentItem))
	{
		CurrentItem->Env->OnInternal_ItemFunctionButtonClicked(CurrentItem->FixedIndex,CurrentItem->Item->ListSegment);
	}
}

inline void UDlFm_Item_Logic::Internal_SetNameEditable(const bool Val) const
{
	if (Val)
	{
		Text_ItemName->SetVisibility(ESlateVisibility::Visible);
		Text_ItemName_Show->SetVisibility(ESlateVisibility::Hidden);
	}
	else
	{
		Text_ItemName->SetVisibility(ESlateVisibility::Hidden);
		Text_ItemName_Show->SetVisibility(ESlateVisibility::Visible);
	}
}
