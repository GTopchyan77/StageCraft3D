// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.


#include "UI/Abstract/Imp/DlFmEnum_OneLine.h"

#include "Components/TextBlock.h"
#include "ModifyU/UI/DlFmButton.h"

void UDlFmEnum_OneLine::NativeConstruct()
{
	Super::NativeConstruct();
	Button_Previous->OnFilteredPressed.AddUniqueDynamic(this, &UDlFmEnum_OneLine::OnPress_Previous);
	Button_Next->OnFilteredPressed.AddUniqueDynamic(this, &UDlFmEnum_OneLine::OnPress_Next);
	TotalNum = ItemTextList.Num();
	Text_Show->SetText(ItemTextList[CurIndex]);
}

uint8 UDlFmEnum_OneLine::SetCurIndex(const uint8 NewValue)
{
	Super::SetCurIndex(NewValue);
	Text_Show->SetText(ItemTextList[CurIndex]);
	return CurIndex;
}

uint8 UDlFmEnum_OneLine::SetCurIndex_NoBroadcast(const uint8 NewValue)
{
	Super::SetCurIndex_NoBroadcast(NewValue);
	Text_Show->SetText(ItemTextList[CurIndex]);
	return CurIndex;
}

void UDlFmEnum_OneLine::SetTotalNum(const uint8 NewTotal)
{
	if (NewTotal < ItemTextList.Num())
	{
		Super::SetTotalNum(NewTotal);
		Text_Show->SetText(ItemTextList[CurIndex]);
	}
}

void UDlFmEnum_OneLine::SetTotalNum_NoBroadcast(const uint8 NewTotal)
{
	if (NewTotal < ItemTextList.Num())
	{
		Super::SetTotalNum_NoBroadcast(NewTotal);
		Text_Show->SetText(ItemTextList[CurIndex]);
	}
}

uint8 UDlFmEnum_OneLine::SetCurIndex_Next()
{
	Super::SetCurIndex_Next();
	Text_Show->SetText(ItemTextList[CurIndex]);
	return CurIndex;
}

uint8 UDlFmEnum_OneLine::SetCurIndex_Previous()
{
	Super::SetCurIndex_Previous();
	Text_Show->SetText(ItemTextList[CurIndex]);
	return CurIndex;
}

void UDlFmEnum_OneLine::InitFromEnum(const UEnum* Val, const uint8 TotalNumV)
{
	if(Val!=nullptr)
	{
		ItemTextList.SetNum(TotalNumV);
		for (uint8 TmpI=0;TmpI<TotalNumV;TmpI++)
		{
			ItemTextList[TmpI] = Val->GetDisplayNameTextByValue(TmpI);
		}
		TotalNum=TotalNumV;
	}
}

void UDlFmEnum_OneLine::OnPress_Previous()
{
	SetCurIndex_Previous();
}

void UDlFmEnum_OneLine::OnPress_Next()
{
	SetCurIndex_Next();
}
