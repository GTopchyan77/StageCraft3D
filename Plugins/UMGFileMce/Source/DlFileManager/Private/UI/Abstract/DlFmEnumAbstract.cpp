// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.


#include "UI/Abstract/DlFmEnumAbstract.h"

uint8 UDlFmEnumAbstract::SetCurIndex(const uint8 NewValue)
{
	uint8 TmpV;
	if (TotalNum > 0)
	{
		TmpV = ((NewValue % TotalNum)>=0)?(NewValue % TotalNum):((NewValue % TotalNum)+TotalNum);
	}
	else
	{
		TmpV = INDEX_NONE;
	}
	if (TmpV != CurIndex)
	{
		OnValueChanged.Broadcast(TmpV);
		OnValueChangedFromTo.Broadcast(CurIndex, TmpV);
		CurIndex = TmpV;
	}
	return CurIndex;
}

uint8 UDlFmEnumAbstract::SetCurIndex_NoBroadcast(const uint8 NewValue)
{
	uint8 TmpV;
	if (TotalNum > 0)
	{
		TmpV = NewValue % TotalNum;
	}
	else
	{
		TmpV = INDEX_NONE;
	}
	CurIndex = TmpV;
	return CurIndex;
}

uint8 UDlFmEnumAbstract::GetCurIndex() const
{
	return CurIndex;
}

void UDlFmEnumAbstract::SetTotalNum(const uint8 NewTotal)
{
	TotalNum = NewTotal;
	SetCurIndex(CurIndex);
}

void UDlFmEnumAbstract::SetTotalNum_NoBroadcast(const uint8 NewTotal)
{
	TotalNum = NewTotal;
	SetCurIndex_NoBroadcast(CurIndex);
}

uint8 UDlFmEnumAbstract::SetCurIndex_Next()
{
	return SetCurIndex(CurIndex+1);
}

uint8 UDlFmEnumAbstract::SetCurIndex_Previous()
{
	return SetCurIndex(CurIndex-1);
}

bool UDlFmEnumAbstract::IsFirst() const
{
	return CurIndex==0;
}

bool UDlFmEnumAbstract::IsLast() const
{
	return CurIndex==(TotalNum-1);
}
