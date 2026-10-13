// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.


#include "UI/Abstract/Imp/DlFmEnum_Panel.h"
#include "UI/Fragment/DlFmFilteredToggleButton.h"


void UDlFmEnum_Panel_Item::OnToggleChanged_Switch(bool NewV)
{
	if(NewV)
	{
		if(IsValid(Parent))
		{
			Parent->SetCurIndex(SelfIndex);
		}
	}
	else
	{
		Toggle_Switch->SetIsChecked_NoBroadcast(true);
	}
}

void UDlFmEnum_Panel_Item::NativeConstruct()
{
	Super::NativeConstruct();
	Toggle_Switch->OnCheckStateChanged.AddUniqueDynamic(this,&UDlFmEnum_Panel_Item::OnToggleChanged_Switch);
}

void UDlFmEnum_Panel::NativeConstruct()
{
	Super::NativeConstruct();
	
	TArray<UWidget*> TmpAllChild = Panel->GetAllChildren();
	for (UWidget* OneWidget :TmpAllChild)
	{
		UDlFmEnum_Panel_Item* TmpW=Cast<UDlFmEnum_Panel_Item>(OneWidget);
		if(TmpW!=nullptr)
		{
			TmpW->Parent=this;
			TmpW->Toggle_Switch->SetIsChecked_NoBroadcast(false);
			ChildWidgetMap.Add(TmpW->SelfIndex,TmpW);
		}
	}

	for(const TTuple<unsigned char, UDlFmEnum_Panel_Item*> OnePair :ChildWidgetMap)
	{
		OnePair.Value->Toggle_Switch->SetIsChecked_NoBroadcast(true);
		CurIndex=OnePair.Key;
		break;
	}
}

uint8 UDlFmEnum_Panel::SetCurIndex(const uint8 NewValue)
{
	if((ChildWidgetMap.Find(NewValue)!=nullptr)&&(NewValue!=CurIndex))
	{
		const uint8 CacheLast=CurIndex;
		CurIndex=NewValue;
		ChildWidgetMap[CacheLast]->Toggle_Switch->SetIsChecked_NoBroadcast(false);
		ChildWidgetMap[CurIndex]->Toggle_Switch->SetIsChecked_NoBroadcast(true);

		OnValueChanged.Broadcast(CurIndex);
		OnValueChangedFromTo.Broadcast(CacheLast,CurIndex);
	}
	return CurIndex;
}

uint8 UDlFmEnum_Panel::SetCurIndex_NoBroadcast(const uint8 NewValue)
{
	if((ChildWidgetMap.Find(NewValue)!=nullptr)&&(NewValue!=CurIndex))
	{
		const uint8 CacheLast=CurIndex;
		CurIndex=NewValue;
		ChildWidgetMap[CacheLast]->Toggle_Switch->SetIsChecked_NoBroadcast(false);
		ChildWidgetMap[CurIndex]->Toggle_Switch->SetIsChecked_NoBroadcast(true);
	}
	return CurIndex;
}

void UDlFmEnum_Panel::SetItemVisible(uint8 Item, ESlateVisibility Visible) const
{
	UDlFmEnum_Panel_Item* const* Tmp = ChildWidgetMap.Find(Item);
	if(Tmp!=nullptr)
	{
		(*Tmp)->SetVisibility(Visible);
	}
}
