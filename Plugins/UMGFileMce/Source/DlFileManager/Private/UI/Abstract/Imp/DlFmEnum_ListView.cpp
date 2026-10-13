// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.


#include "UI/Abstract/Imp/DlFmEnum_ListView.h"

void UDlFmEnum_ListView_ItemWidget::NativeConstruct()
{
	Super::NativeConstruct();
	Button_Select->OnPressed.AddUniqueDynamic(this,&UDlFmEnum_ListView_ItemWidget::OnSelect_Pressed);
}

void UDlFmEnum_ListView_ItemWidget::NativeOnListItemObjectSet(UObject* ListItemObject)
{
	IUserObjectListEntry::NativeOnListItemObjectSet(ListItemObject);

	UDlFmEnum_ListView_ItemData* TmpData = Cast<UDlFmEnum_ListView_ItemData>(ListItemObject);
	if(IsValid( TmpData))
	{
		DataPtr=TmpData;
		FButtonStyle CacheButtonStyle;
		if(DataPtr->Parent->GetCurIndex()==DataPtr->SelfIndex)
		{
			CacheButtonStyle.SetNormal(DataPtr->Parent->WhenTrue);
			CacheButtonStyle.SetHovered(DataPtr->Parent->WhenHoveredTrue);
			CacheButtonStyle.SetPressed(DataPtr->Parent->WhenFalse);
			CacheButtonStyle.SetDisabled(DataPtr->Parent->WhenDisabledTrue);
		}
		else
		{
			
			CacheButtonStyle.SetNormal(DataPtr->Parent->WhenFalse);
			CacheButtonStyle.SetHovered(DataPtr->Parent->WhenHoveredFalse);
			CacheButtonStyle.SetPressed(DataPtr->Parent->WhenTrue);
			CacheButtonStyle.SetDisabled(DataPtr->Parent->WhenDisabledFalse);
		}
	    Button_Select->SetStyle(CacheButtonStyle);

		SetVisibility(DataPtr->Visible);

	}
}

void UDlFmEnum_ListView_ItemWidget::OnSelect_Pressed()
{
	if( IsValid(DataPtr))
	{
		DataPtr->Parent->SetCurIndex(DataPtr->SelfIndex);
	}
}

void UDlFmEnum_ListView::NativeConstruct()
{
	Super::NativeConstruct();
	MainList->SetSelectionMode(ESelectionMode::None);
}

uint8 UDlFmEnum_ListView::SetCurIndex(const uint8 NewValue)
{
	const uint8 TmpV = Super::SetCurIndex(NewValue);
	MainList->RequestRefresh();
	return TmpV;
}

uint8 UDlFmEnum_ListView::SetCurIndex_NoBroadcast(const uint8 NewValue)
{
	const uint8 TmpV = Super::SetCurIndex_NoBroadcast(NewValue);
	MainList->RequestRefresh();
	return TmpV;
}

void UDlFmEnum_ListView::SetTotalNum(const uint8 NewTotal)
{
	Super::SetTotalNum(NewTotal);
	Set_ItemPoolNum(TotalNum);
}

void UDlFmEnum_ListView::SetTotalNum_NoBroadcast(const uint8 NewTotal)
{
	Super::SetTotalNum_NoBroadcast(NewTotal);
	Set_ItemPoolNum(TotalNum);
}

uint8 UDlFmEnum_ListView::SetCurIndex_Next()
{
	const uint8 TmpV = Super::SetCurIndex_Next();
	MainList->RequestRefresh();
	return TmpV;
}

uint8 UDlFmEnum_ListView::SetCurIndex_Previous()
{
	const uint8 TmpV = Super::SetCurIndex_Previous();
	MainList->RequestRefresh();
	return TmpV;
}

void UDlFmEnum_ListView::SetItemVisible(const uint8 ItemIndexV, const ESlateVisibility VisibleV) const
{
	Super::SetItemVisible(ItemIndexV, VisibleV);
	ItemPool[ItemIndexV]->Visible=VisibleV;
	MainList->RequestRefresh();
}

void UDlFmEnum_ListView::Set_ItemPoolNum(const uint8 NewNum)
{
	if(NewNum>ItemPool.Num())
	{
		for (int32 TmpIndex = ItemPool.Num(); TmpIndex < NewNum; TmpIndex++)
		{
			UDlFmEnum_ListView_ItemData* TmpNewItem = NewObject<UDlFmEnum_ListView_ItemData>(this);
			TmpNewItem->Parent=this;
			TmpNewItem->SelfIndex=TmpIndex;
			ItemPool.Emplace(TmpNewItem);
		}
		MainList->SetListItems(ItemPool);
		MainList->RequestRefresh();
	}
	else if(NewNum<ItemPool.Num())
	{
		for (int32 TmpIndex = ItemPool.Num(); TmpIndex >= NewNum; TmpIndex--)
		{
			ItemPool[TmpIndex]->MarkAsGarbage();
		}
		ItemPool.SetNum(NewNum);
		MainList->SetListItems(ItemPool);
		MainList->RequestRefresh();
	}
}
