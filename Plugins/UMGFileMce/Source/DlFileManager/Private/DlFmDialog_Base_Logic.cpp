// Copyright(c) 2022 Dragon Li Software Limited. All Rights Reserved.


#include "DlFmDialog_Base_Logic.h"

#include "Components/TextBlock.h"
#include "ModifyU/UI/DlFmButton.h"

//Set visibility before broadcast, because booking functions may need to change visibility again 
void UDlFmDialog_Base_Logic::DoWork_Close()
{
	SetVisibility(ESlateVisibility::Hidden);
	OnDialogClicked.Broadcast(EDlFm_DialogEventType::Click_Close);
}

void UDlFmDialog_Base_Logic::DoWork_ChooseYes()
{
	if (bAutoClose)
	{
		SetVisibility(ESlateVisibility::Hidden);
	}
	OnDialogClicked.Broadcast(EDlFm_DialogEventType::Click_Yes);
}

void UDlFmDialog_Base_Logic::DoWork_ChooseNo()
{
	if (bAutoClose)
	{
		SetVisibility(ESlateVisibility::Hidden);
	}
	OnDialogClicked.Broadcast(EDlFm_DialogEventType::Click_No);
}

void UDlFmDialog_Base_Logic::DoWork_ChooseYesForAll()
{
	if (bAutoClose)
	{
		SetVisibility(ESlateVisibility::Hidden);
	}
	OnDialogClicked.Broadcast(EDlFm_DialogEventType::Click_YesForAll);
}

void UDlFmDialog_Base_Logic::DoWork_ChooseNoForAll()
{
	if (bAutoClose)
	{
		SetVisibility(ESlateVisibility::Hidden);
	}
	OnDialogClicked.Broadcast(EDlFm_DialogEventType::Click_NoForAll);
}

void UDlFmDialog_Base_Logic::DoWork_ChooseCancel()
{
	if (bAutoClose)
	{
		SetVisibility(ESlateVisibility::Hidden);
	}
	OnDialogClicked.Broadcast(EDlFm_DialogEventType::Click_Cancel);
}

void UDlFmDialog_Base_Logic::DoWork_SelectNewerModified()
{
	if (bAutoClose)
	{
		SetVisibility(ESlateVisibility::Hidden);
	}
	OnDialogClicked.Broadcast(EDlFm_DialogEventType::Click_SelectNewerModified);
}

void UDlFmDialog_Base_Logic::DoWork_SelectNewerModified_ForAll()
{
	if (bAutoClose)
	{
		SetVisibility(ESlateVisibility::Hidden);
	}
	OnDialogClicked.Broadcast(EDlFm_DialogEventType::Click_SelectNewerModified_ForAll);
}

void UDlFmDialog_Base_Logic::DoWork_SelectLarger()
{
	if (bAutoClose)
	{
		SetVisibility(ESlateVisibility::Hidden);
	}
	OnDialogClicked.Broadcast(EDlFm_DialogEventType::Click_SelectLarger);
}

void UDlFmDialog_Base_Logic::DoWork_SelectLarger_ForAll()
{
	if (bAutoClose)
	{
		SetVisibility(ESlateVisibility::Hidden);
	}
	OnDialogClicked.Broadcast(EDlFm_DialogEventType::Click_SelectLarger_ForAll);
}

void UDlFmDialog_Base_Logic::NativeConstruct()
{
	Super::NativeConstruct();
	if (IsValid(Button_Close))
	{
		Button_Close->OnFilteredClicked.
		              AddUniqueDynamic(this, &UDlFmDialog_Base_Logic::OnInternalClick_Close);
	}
	if (IsValid(Button_Yes))
	{
		Button_Yes->OnFilteredClicked.AddUniqueDynamic(this, &UDlFmDialog_Base_Logic::OnInternalClick_Yes);
	}
	if (IsValid(Button_No))
	{
		Button_No->OnFilteredClicked.AddUniqueDynamic(this, &UDlFmDialog_Base_Logic::OnInternalClick_No);
	}
	if (IsValid(Button_YesForAll))
	{
		Button_YesForAll->OnFilteredClicked.AddUniqueDynamic(
			this, &UDlFmDialog_Base_Logic::OnInternalClick_YesForAll);
	}
	if (IsValid(Button_NoForAll))
	{
		Button_NoForAll->OnFilteredClicked.AddUniqueDynamic(
			this, &UDlFmDialog_Base_Logic::OnInternalClick_NoForAll);
	}
	if (IsValid(Button_Cancel))
	{
		Button_Cancel->OnFilteredClicked.AddUniqueDynamic(
			this, &UDlFmDialog_Base_Logic::OnInternalClick_Cancel);
	}
	if (IsValid(Button_SelectNewerModified))
	{
		Button_SelectNewerModified->OnFilteredClicked.AddUniqueDynamic(
			this, &UDlFmDialog_Base_Logic::OnInternalClick_SelectNewerModified);
	}
	if (IsValid(Button_SelectNewerModified_ForAll))
	{
		Button_SelectNewerModified_ForAll->OnFilteredClicked.AddUniqueDynamic(
			this, &UDlFmDialog_Base_Logic::OnInternalClick_SelectNewerModified_ForAll);
	}
	if (IsValid(Button_SelectLarger))
	{
		Button_SelectLarger->OnFilteredClicked.AddUniqueDynamic(
			this, &UDlFmDialog_Base_Logic::OnInternalClick_SelectLarger);
	}
	if (IsValid(Button_SelectLarger_ForAll))
	{
		Button_SelectLarger_ForAll->OnFilteredClicked.AddUniqueDynamic(
			this, &UDlFmDialog_Base_Logic::OnInternalClick_SelectLarger_ForAll);
	}
}

void UDlFmDialog_Base_Logic::OnInternalClick_Close()
{
	DoWork_Close();
}

void UDlFmDialog_Base_Logic::OnInternalClick_Yes()
{
	DoWork_ChooseYes();
}

void UDlFmDialog_Base_Logic::OnInternalClick_No()
{
	DoWork_ChooseNo();
}

void UDlFmDialog_Base_Logic::OnInternalClick_YesForAll()
{
	DoWork_ChooseYesForAll();
}

void UDlFmDialog_Base_Logic::OnInternalClick_NoForAll()
{
	DoWork_ChooseNoForAll();
}

void UDlFmDialog_Base_Logic::OnInternalClick_Cancel()
{
	DoWork_ChooseCancel();
}

void UDlFmDialog_Base_Logic::OnInternalClick_SelectNewerModified()
{
	DoWork_SelectNewerModified();
}

void UDlFmDialog_Base_Logic::OnInternalClick_SelectNewerModified_ForAll()
{
	DoWork_SelectNewerModified_ForAll();
}

void UDlFmDialog_Base_Logic::OnInternalClick_SelectLarger()
{
	DoWork_SelectLarger();
}

void UDlFmDialog_Base_Logic::OnInternalClick_SelectLarger_ForAll()
{
	DoWork_SelectLarger_ForAll();
}
