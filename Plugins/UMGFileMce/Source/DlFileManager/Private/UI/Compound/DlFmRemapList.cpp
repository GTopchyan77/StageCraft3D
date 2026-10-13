// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.


#include "UI/Compound/DlFmRemapList.h"

#include "Components/ListView.h"
#include "ModifyU/UI/DlFmBorder.h"
#include "ModifyU/UI/DlFmButton.h"
#include "UI/Compound/DlFmRemapList_Lib.h"

void UDlFmRemapList::OnClick_ClearSelect()
{
	for (int32& One:Remap_Key2Value)
	{
		One=INDEX_NONE;
	}
	ForceRefreshAllUISelect();
}

void UDlFmRemapList::OnClick_TryMatch()
{
	TryMatch();
	ForceRefreshAllUISelect();
}

void UDlFmRemapList::Refresh_MatchStatus()
{
	if(IsValid(Border_MatchStatus))
	{
		bool bAllMatched=true;
		for (const int32 One :Remap_Key2Value)
		{
			if(One==INDEX_NONE)
			{
				bAllMatched=false;
				break;
			}
		}
		if(bAllMatched)
		{
			Border_MatchStatus->SetBrushColor(Color_AllMatched);
		}
		else
		{
			Border_MatchStatus->SetBrushColor(Color_ExistUnmatched);
		}
	}
}

void UDlFmRemapList::SetRemap(int32 KeyIndex, int32 ValueIndex)
{
	if(KeyIndex<GetKeyList_Num())
	{
		if(((ValueIndex<GetValueList_Num())&&(ValueIndex>=0))||(ValueIndex==INDEX_NONE))
		{
			Remap_Key2Value[KeyIndex]=ValueIndex;
		}
	}
}

void UDlFmRemapList::Refresh_AfterKvListChange()
{
	Ver_ValueList.Update();
	Remap_Key2Value.Init(INDEX_NONE,GetKeyList_Num());
	TryMatch();
	SetVisiblePoolNum(GetKeyList_Num());
	List_KeyValue->SetListItems(Visible_Pool);
	List_KeyValue->RequestRefresh();
	ForceRefreshAllUISelect();
	Refresh_MatchStatus();
}

void UDlFmRemapList::NativeConstruct()
{
	Super::NativeConstruct();
	if(IsValid(Button_ClearSelect))
	{
		Button_ClearSelect->OnClicked.AddUniqueDynamic(this,&UDlFmRemapList::OnClick_ClearSelect);
	}
	if(IsValid(Button_TryMatch))
	{
		Button_TryMatch->OnClicked.AddUniqueDynamic(this,&UDlFmRemapList::OnClick_TryMatch);
	}
}

void UDlFmRemapList::SetVisiblePoolNum(int32 Val)
{
	if(Val>=Data_Pool.Num())
	{
		const int32 OrgDataPoolNum=Data_Pool.Num();
		for (int32 TmpI0=OrgDataPoolNum;TmpI0<Val;TmpI0++)
		{
			UDlFmRemapList_OneData* TmpObj = NewObject<UDlFmRemapList_OneData>(this);
			TmpObj->Parent=this;
			TmpObj->SelfIndex=TmpI0;
			Data_Pool.Emplace(TmpObj);
		}
	}
	Visible_Pool.SetNumUninitialized(Val);
	for (int32 TmpI=0;TmpI<Val;TmpI++)
	{
		Visible_Pool[TmpI]=Data_Pool[TmpI];
	}
}

void UDlFmRemapList::ForceRefreshAllUISelect()
{
	for (const UDlFmRemapList_OneData* One :Visible_Pool)
	{
		One->RefreshWidgetSelect();
	}
}
