// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.

#include "UI/Compound/DlFmRemapList_Lib.h"

#include "Components/ComboBoxString.h"
#include "Components/TextBlock.h"
#include "ModifyU/UI/DlFmBorder.h"
#include "UI/Compound/DlFmRemapList.h"


void UDlFmRemapList_OneData::RefreshWidgetSelect() const
{
	if(IsValid(Widget))
	{
		if(Widget->SelfData==this)
		{
			Widget->SetUI_SelectIndex(Parent->GetRemap()[SelfIndex]);
		}
	}
}

void UDlFmRemapList_OneWidget::OnSelectChanged_ValueName(FString SelectedItem, ESelectInfo::Type SelectionType)
{
	if (IsValid(SelfData))
	{
		const int32 SelectIndex = Combo_ValueName->GetSelectedIndex();
		if (SelectIndex == 0)
		{
			SelfData->Parent->SetRemap(SelfData->SelfIndex,INDEX_NONE);
			SetStyle_SelectNone(true);
		}
		else
		{
			SelfData->Parent->SetRemap(SelfData->SelfIndex,Combo_ValueName->GetSelectedIndex() - 1);
			SetStyle_SelectNone(false);
		}
		SelfData->Parent->Refresh_MatchStatus();
	}
}

void UDlFmRemapList_OneWidget::NativeConstruct()
{
	Super::NativeConstruct();
	Combo_ValueName->OnSelectionChanged.AddUniqueDynamic(
		this, &UDlFmRemapList_OneWidget::OnSelectChanged_ValueName);
}

void UDlFmRemapList_OneWidget::NativeOnListItemObjectSet(UObject* ListItemObject)
{
	IUserObjectListEntry::NativeOnListItemObjectSet(ListItemObject);
	
	SelfData = Cast<UDlFmRemapList_OneData>(ListItemObject);
	if (SelfData != nullptr)
	{
		SelfData->Widget=this;
		if (IsValid(SelfData->Parent))
		{
			Text_KeyName->SetText(FText::FromString(
				SelfData->Parent->GetKey_ItemName(SelfData->SelfIndex)));
			
			if (Ver_ValueList.Observe_TriggerUpdate(SelfData->Parent->Ver_ValueList))
			{
				Combo_ValueName->ClearOptions();
				Combo_ValueName->AddOption(TEXT("None"));
				for (int32 TmpI = 0; TmpI < SelfData->Parent->GetValueList_Num(); TmpI++)
				{
					Combo_ValueName->AddOption(SelfData->Parent->GetValue_ItemName(TmpI));
				}
			}

			SetUI_SelectIndex(SelfData->Parent->GetRemap()[SelfData->SelfIndex]);
		}
	}
}

void UDlFmRemapList_OneWidget::SetUI_SelectIndex(const int32 Index) const
{
	if (Index == INDEX_NONE)
	{
		Combo_ValueName->SetSelectedIndex(0);
		SetStyle_SelectNone(true);
	}
	else
	{
		Combo_ValueName->SetSelectedIndex(Index + 1);
		SetStyle_SelectNone(false);
	}
}

void UDlFmRemapList_OneWidget::SetStyle_SelectNone(bool Val) const
{
	if(IsValid(Border_IsNone))
	{
		if(Val)
		{
			Border_IsNone->SetBrush(Brush_IsNone);
		}
		else
		{
			Border_IsNone->SetBrush(Brush_NotNone);
		}
	}
}
