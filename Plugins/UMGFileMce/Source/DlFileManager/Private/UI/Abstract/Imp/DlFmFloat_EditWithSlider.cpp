// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.


#include "UI/Abstract/Imp/DlFmFloat_EditWithSlider.h"

#include "Components/EditableText.h"
#include "Components/Slider.h"
#include "ModifyU/UI/DlFmFloat_EditSimple.h"

void UDlFmFloat_EditWithSlider::OnCommit_Number(float Val, ETextCommit::Type CommitMethod)
{
	SetValue(Val);
}

void UDlFmFloat_EditWithSlider::OnValueChanged_Percent(float NewVal)
{
	SetValue(NewVal);
}

void UDlFmFloat_EditWithSlider::NativeConstruct()
{
	Super::NativeConstruct();
	Float_Number->OnValueCommitted.AddUniqueDynamic(this,&UDlFmFloat_EditWithSlider::OnCommit_Number);
	Slider_Percent->OnValueChanged.AddUniqueDynamic(this,&UDlFmFloat_EditWithSlider::OnValueChanged_Percent);
}

void UDlFmFloat_EditWithSlider::SetEditable(bool bAbleEditV)
{
	Super::SetEditable(bAbleEditV);
	Float_Number->SetIsEnabled(bAbleEditV);
	Slider_Percent->SetIsEnabled(bAbleEditV);
	//Float_Number->SetIsReadOnly(bAbleEditV);
	//Slider_Percent->SetLocked(bAbleEditV);
}

float UDlFmFloat_EditWithSlider::SetValue(float NewValue)
{
	Super::SetValue(NewValue);
	Float_Number->SetValue(CurValue);
	if(FMath::IsNaN(NewValue))
	{
		Slider_Percent->SetValue(CurRange.Max);
	}
	else
	{
		Slider_Percent->SetValue(NewValue);
	}
	return CurValue;
}

float UDlFmFloat_EditWithSlider::SetValue_NoBroadcast(float NewValue)
{
	Super::SetValue_NoBroadcast(NewValue);
	Float_Number->SetValue(CurValue);
	Slider_Percent->SetValue(NewValue);
	return CurValue;
}

void UDlFmFloat_EditWithSlider::SetMaxValue(float NewMax)
{
	Super::SetMaxValue(NewMax);
	Slider_Percent->SetMaxValue(CurRange.Max);
}

void UDlFmFloat_EditWithSlider::SetMinValue(float NewMin)
{
	Super::SetMinValue(NewMin);
	Slider_Percent->SetMinValue(CurRange.Min);
}

void UDlFmFloat_EditWithSlider::SetArrange_Interval(FFloatInterval NewArrange)
{
	Super::SetArrange_Interval(NewArrange);
	Slider_Percent->SetMinValue(CurRange.Min);
	Slider_Percent->SetMaxValue(CurRange.Max);
	
}

void UDlFmFloat_EditWithSlider::SetArrange_2Number(float NewMin, float NewMax)
{
	Super::SetArrange_2Number(NewMin, NewMax);
	Slider_Percent->SetMinValue(CurRange.Min);
	Slider_Percent->SetMaxValue(CurRange.Max);
}

