// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.


#include "UI/Abstract/Imp/DlFmFloat_ShowWithBar.h"

#include "Components/ProgressBar.h"
#include "ModifyU/UI/DlFmFloat_ShowSimple.h"

void UDlFmFloat_ShowWithBar::NativeConstruct()
{
	Super::NativeConstruct();
	bEditable = false;
	bMinLimited=true;
	bMaxLimited=true;
	Float_Number->SetValue(CurValue);
	Bar_Percent->SetPercent(
		FMath::GetMappedRangeValueClamped(
			FVector2D(CurRange.Min, CurRange.Max),
			FVector2D(0, 1),
			CurValue
		)
	);
}

float UDlFmFloat_ShowWithBar::SetValue(float NewValue)
{
	Super::SetValue(NewValue);
	Float_Number->SetValue(CurValue);
	Bar_Percent->SetPercent(
		FMath::GetMappedRangeValueClamped(
			FVector2D(CurRange.Min, CurRange.Max),
			FVector2D(0, 1),
			CurValue
		)
	);
	return CurValue;
}

float UDlFmFloat_ShowWithBar::SetValue_NoBroadcast(float NewValue)
{
	Super::SetValue_NoBroadcast(NewValue);
	Float_Number->SetValue(CurValue);
	Bar_Percent->SetPercent(
		FMath::GetMappedRangeValueClamped(
			FVector2D(CurRange.Min, CurRange.Max),
			FVector2D(0, 1),
			CurValue
		)
	);
	return CurValue;
}
