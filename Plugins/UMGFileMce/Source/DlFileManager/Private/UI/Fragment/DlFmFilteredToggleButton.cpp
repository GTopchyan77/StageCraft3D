// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.


#include "UI/Fragment/DlFmFilteredToggleButton.h"

UDlFmFilteredToggleButton::UDlFmFilteredToggleButton(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	OnFilteredClicked.AddUniqueDynamic(this, &UDlFmFilteredToggleButton::DlFilteredClick_UpperHandle);

	CurrentBool = Config.InitBool;
	CacheButtonStyle = GetStyle();
	UpdateStyle();
}

void UDlFmFilteredToggleButton::DlFilteredClick_UpperHandle()
{
	
	CurrentBool = !CurrentBool;
	UpdateStyle();
	//lastChangeTime = GetWorld()->RealTimeSeconds;
	OnCheckStateChanged.Broadcast(CurrentBool);
	
}


void UDlFmFilteredToggleButton::SetIsChecked(bool InIsChecked)
{
	if(InIsChecked!=CurrentBool)
	{
		CurrentBool = InIsChecked;
		UpdateStyle();
		OnCheckStateChanged.Broadcast(CurrentBool);		
	}
}

void UDlFmFilteredToggleButton::SetIsChecked_NoBroadcast(bool InIsChecked)
{
	if(InIsChecked!=CurrentBool)
	{
		CurrentBool = InIsChecked;
		UpdateStyle();	
	}
}

bool UDlFmFilteredToggleButton::IsChecked() const
{
	return CurrentBool;
}

void UDlFmFilteredToggleButton::SynchronizeProperties()
{
	CurrentBool = Config.InitBool;
	Super::SynchronizeProperties();
	UpdateStyle();
}

void UDlFmFilteredToggleButton::UpdateStyle()
{
	if (CurrentBool==true)
	{
		CacheButtonStyle.SetNormal(Config.WhenTrue);
		CacheButtonStyle.SetHovered(Config.WhenHoveredTrue);
		CacheButtonStyle.SetPressed(Config.WhenFalse);
		CacheButtonStyle.SetDisabled(Config.WhenDisabledTrue);
	}
	else
	{
		CacheButtonStyle.SetNormal(Config.WhenFalse);
		CacheButtonStyle.SetHovered(Config.WhenHoveredFalse);
		CacheButtonStyle.SetPressed(Config.WhenTrue);
		CacheButtonStyle.SetDisabled(Config.WhenDisabledFalse);
	}
	SetStyle(CacheButtonStyle);
}
