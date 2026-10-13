// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.

#include "UI/DlFmUICommon.h"

#include "Internationalization/Culture.h"


void FDlFmNumberFormat::InitFormat(FNumberFormattingOptions& Target) const
{
	Target.AlwaysSign = bAlwaysSign;
	Target.UseGrouping = bUseGrouping;
	Target.RoundingMode = RoundingMode;
	Target.MinimumIntegralDigits = MinimumIntegralDigits;
	Target.MaximumIntegralDigits = MaximumIntegralDigits;
	Target.MinimumFractionalDigits = MinimumFractionalDigits;
	Target.MaximumFractionalDigits = MaximumFractionalDigits;
}


FText FDlFmNumberFormat::Float2Text(const float& Val, const FNumberFormattingOptions& NumFormat)
{
	if (FMath::IsNaN(Val))
	{
		return FText::FromName(TEXT("N/A"));
	}else if(FMath::IsNearlyZero(Val))
	{
		return FText::AsNumber(0,&NumFormat);
	}
	else
	{
		return FText::AsNumber(Val, &NumFormat);
	}
}

FText FDlFmNumberFormat::Int2Text(const int32& Val, const FNumberFormattingOptions& NumFormat)
{
	return FText::AsNumber(Val,&NumFormat);
}

float FDlFmNumberFormat::Text2Float(const FText& Val)
{
	return FCString::Atof(*Val.ToString());
}

int32 FDlFmNumberFormat::Text2Int(const FText& Val)
{
	return FCString::Atoi(*Val.ToString());
}




