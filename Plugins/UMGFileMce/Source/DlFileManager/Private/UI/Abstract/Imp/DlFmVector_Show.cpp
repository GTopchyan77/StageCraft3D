// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.


#include "UI/Abstract/Imp/DlFmVector_Show.h"

#include "Components/TextBlock.h"
#include "ModifyU/UI/DlFmFloat_ShowSimple.h"

void UDlFmVector_Show::SetValue(const FVector& NewValue)
{
	Super::SetValue(NewValue);
	Float_X->SetValue(CurValue.X);
	Float_Y->SetValue(CurValue.Y);
	Float_Z->SetValue(CurValue.Z);
}

