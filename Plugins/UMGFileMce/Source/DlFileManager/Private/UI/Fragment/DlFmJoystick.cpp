// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.


#include "UI/Fragment/DlFmJoystick.h"


void UDlFmJoystick::SynchronizeProperties()
{
	if (MyDlFmJoystick.IsValid())
	{
		MyDlFmJoystick->Config = Config;
		MyDlFmJoystick->GetDraft().PlayerHandle.OnHandle0Changed.AddUniqueDynamic(this,&UDlFmJoystick::DelegateBridget_PlayerHandleChanged);
	}
	Super::SynchronizeProperties();
}

void UDlFmJoystick::ReleaseSlateResources(bool bReleaseChildren)
{
	Super::ReleaseSlateResources(bReleaseChildren);
	MyDlFmJoystick.Reset();
}

void UDlFmJoystick::SetCtrlSrc_PlayerHandle(EDlFmJoystick_ControlSource Val)
{
	if(MyDlFmJoystick.IsValid())
	{
		MyDlFmJoystick->SetCtrlSrc_PlayerHandle(Val);
	}
}

void UDlFmJoystick::SetCtrlSrc_AIHandle(EDlFmJoystick_ControlSource Val)
{
	if(MyDlFmJoystick.IsValid())
	{
		MyDlFmJoystick->SetCtrlSrc_AIHandle(Val);
	}
}

void UDlFmJoystick::SmoothedTo_AIHandle(const FVector2D& Val)
{
	if(MyDlFmJoystick.IsValid())
	{
		MyDlFmJoystick->SmoothedTo_AIHandle(Val);
	}
}

void UDlFmJoystick::Set_PercentUnClamp_NoBroadcast_PlayerHandle(const FVector2D& Val)
{
	if(MyDlFmJoystick.IsValid())
	{
		MyDlFmJoystick->Set_PercentUnClamp_NoBroadcast_PlayerHandle(Val);
	}
}

FVector2D UDlFmJoystick::Get_Percent()
{
	if(MyDlFmJoystick.IsValid())
	{
		return MyDlFmJoystick->GetDraft().PlayerHandle.CurVal;
	}
	return FVector2D::ZeroVector;
}

void UDlFmJoystick::SetConfig(FDlFmJoystick_Config& Val)
{
	if (MyDlFmJoystick.IsValid())
	{
		Config=Val;
		MyDlFmJoystick->Config = Config;
	}
}

void UDlFmJoystick::RefreshConfig()
{
	if (MyDlFmJoystick.IsValid())
	{
		MyDlFmJoystick->Config = Config;
	}
}


#if WITH_EDITOR
const FText UDlFmJoystick::GetPaletteCategory()
{
	return FText::FromName(TEXT("DlFmJoystick"));
}
#endif

void UDlFmJoystick::DelegateBridget_PlayerHandleChanged(const FVector2D& Val)
{
	OnPlayerHandleChanged.Broadcast(Val);
}

TSharedRef<SWidget> UDlFmJoystick::RebuildWidget()
{
	MyDlFmJoystick = SNew(SDlFmJoystick);
	return MyDlFmJoystick.ToSharedRef();
}
