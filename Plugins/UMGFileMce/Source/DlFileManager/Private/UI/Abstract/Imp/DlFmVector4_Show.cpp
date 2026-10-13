// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.


#include "UI/Abstract/Imp/DlFmVector4_Show.h"

#include "ModifyU/UI/DlFmFloat_ShowSimple.h"


void UDlFmVector4_Show::SetValue(const FVector4& NewValue)
{
	Super::SetValue(NewValue);
	Float_X->SetValue(CurValue.X);
	Float_Y->SetValue(CurValue.Y);
	Float_Z->SetValue(CurValue.Z);
	Float_W->SetValue(CurValue.W);
}

