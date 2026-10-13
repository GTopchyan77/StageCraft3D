// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.


#include "UI/Abstract/DlFmFloatAbstract.h"

void UDlFmFloatAbstract::SetEditable(bool bAbleEditV)
{
	bEditable=bAbleEditV;
}

void UDlFmFloatAbstract::SetLimitMaxValue(bool bUseV)
{
	bMaxLimited=bUseV;
}

void UDlFmFloatAbstract::SetLimitMinValue(bool bUseV)
{
	bMinLimited=bUseV;
}

void UDlFmFloatAbstract::SetLimitMaxMinValue(bool bUseV)
{
	bMinLimited=bUseV;
	bMaxLimited=bUseV;
}

void UDlFmFloatAbstract::SetMaxValue(float NewMax)
{
	const float TmpV=(NewMax>=CurRange.Min)?NewMax:CurRange.Min;
	if(TmpV!=CurRange.Max)
	{
		CurRange.Max=TmpV;
		OnRangeChanged.Broadcast(CurRange.Min,CurRange.Max);
	}
}

void UDlFmFloatAbstract::SetMinValue(float NewMin)
{
	const float TmpV=(NewMin<=CurRange.Max)?NewMin:CurRange.Max;
	if(TmpV!=CurRange.Min)
	{
		CurRange.Min=TmpV;
		OnRangeChanged.Broadcast(CurRange.Min,CurRange.Max);
	}
}

void UDlFmFloatAbstract::SetArrange_Interval(FFloatInterval NewArrange)
{
	CurRange=NewArrange;
}

void UDlFmFloatAbstract::SetArrange_2Number(float NewMin, float NewMax)
{
	CurRange.Max=NewMax;
	CurRange.Min=NewMin;
}

float UDlFmFloatAbstract::SetValue(float NewValue)
{
	float TmpV=NewValue;
	if(bMinLimited)
	{
		TmpV=FMath::Max(TmpV,CurRange.Min);
	}
	if(bMaxLimited)
	{
		TmpV=FMath::Min(TmpV,CurRange.Max);
	}
	if(TmpV!=CurValue)
	{
		CurValue=TmpV;
		OnValueChanged.Broadcast(CurValue);
	}
	return CurValue;
}

void UDlFmFloatAbstract::SetChangeStep(float NewStep)
{
	ChangeStep=NewStep;
}

float UDlFmFloatAbstract::SetValue_Add()
{
	return SetValue(CurValue+ChangeStep);
}


float UDlFmFloatAbstract::SetValue_Sub()
{
	return SetValue(CurValue-ChangeStep);
}


float UDlFmFloatAbstract::SetValue_NoBroadcast(float NewValue)
{
	float TmpV=NewValue;
	if(bMinLimited)
	{
		TmpV=FMath::Max(TmpV,CurRange.Min);
	}
	if(bMaxLimited)
	{
		TmpV=FMath::Min(TmpV,CurRange.Max);
	}
	CurValue=TmpV;
	return CurValue;
}

float UDlFmFloatAbstract::GetValue() const
{
	return CurValue;
}
