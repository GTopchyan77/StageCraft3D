// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.


#include "UI/Abstract/Imp/DlFmVector_Edit.h"

#include "ModifyU/UI/DlFmFloat_EditSimple.h"


void UDlFmVector_Edit::NativeConstruct()
{
	Super::NativeConstruct();
	Float_X->OnValueCommitted.AddUniqueDynamic(this,&UDlFmVector_Edit::OnValueChanged_X);
	Float_Y->OnValueCommitted.AddUniqueDynamic(this,&UDlFmVector_Edit::OnValueChanged_Y);
	Float_Z->OnValueCommitted.AddUniqueDynamic(this,&UDlFmVector_Edit::OnValueChanged_Z);
}

void UDlFmVector_Edit::SetValue_NoBroadcast(const FVector& NewValue)
{
	Super::SetValue_NoBroadcast(NewValue);
	Float_X->SetValue_NoBroadcast(CurValue.X);
	Float_Y->SetValue_NoBroadcast(CurValue.Y);
	Float_Z->SetValue_NoBroadcast(CurValue.Z);
}

void UDlFmVector_Edit::SetValue(const FVector& NewValue)
{
	Super::SetValue(NewValue);
	Float_X->SetValue_NoBroadcast(CurValue.X);
	Float_Y->SetValue_NoBroadcast(CurValue.Y);
	Float_Z->SetValue_NoBroadcast(CurValue.Z);
}

void UDlFmVector_Edit::OnValueChanged_X(float Val,ETextCommit::Type CommitMethod)
{
	if(CurValue.X!=Val)
	{
		CurValue.X=Val;
		OnValueChangedEvent.Broadcast(CurValue);
	}
}

void UDlFmVector_Edit::OnValueChanged_Y(float Val,ETextCommit::Type CommitMethod)
{
	if(CurValue.Y!=Val)
	{
		CurValue.Y=Val;
		OnValueChangedEvent.Broadcast(CurValue);
	}
}

void UDlFmVector_Edit::OnValueChanged_Z(float Val,ETextCommit::Type CommitMethod)
{
	if(CurValue.Z!=Val)
	{
		CurValue.Z=Val;
		OnValueChangedEvent.Broadcast(CurValue);
	}
}
