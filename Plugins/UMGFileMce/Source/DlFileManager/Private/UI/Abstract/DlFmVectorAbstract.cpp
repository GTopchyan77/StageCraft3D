// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.


#include "UI/Abstract/DlFmVectorAbstract.h"

void UDlFmVectorAbstract::SetValue_NoBroadcast(const FVector& NewValue)
{
	CurValue=NewValue;
}

void UDlFmVectorAbstract::SetValue(const FVector& NewValue)
{
	if(CurValue!=NewValue)
	{
		CurValue=NewValue;
		OnVectorValueChanged.Broadcast(CurValue);
	}
}

