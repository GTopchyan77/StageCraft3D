// Copyright(c) 2022 Dragon Li Software Limited. All Rights Reserved.


#include "DlFmDialog_Logic.h"

#include "DlFmLib.h"

#include "Components/EditableText.h"
#include "Components/TextBlock.h"
#include "Misc/Paths.h"


void UDlFm_DialogDelete_Logic::SetInfo_ConfirmDelete(const FDlUI_FileExplorer_Lib::FBundleAction& BundleAction)
{
	const FString TmpMsg=FString::Printf(TEXT("Please confirm to delete the %d items."),BundleAction.SelectedBuffer.Num());
	Text_DeleteInfo->SetText(FText::FromString(TmpMsg));
	SetVisibility(ESlateVisibility::Visible);
}

void UDlFm_DialogDelete_Logic::SetInfo_DeleteFailed(
	const FDlUI_FileExplorer_Lib::FBundleAction& BundleAction)
{
	const FString TmpMsg=FString::Printf(TEXT(" %d items are not deleted."),BundleAction.FailedBuffer.Num());
	Text_DeleteInfo->SetText(FText::FromString(TmpMsg));
	SetVisibility(ESlateVisibility::Visible);
}

void UDlFm_DialogNewItem_Logic::SetInfo_StartNewItem(const FString& DefaultDirName)
{
	const FString TmpSanitizedDirName=FPaths::MakeValidFileName(DefaultDirName);
	Text_NewName->SetText(FText::FromString(TmpSanitizedDirName));
	Text_NewName->SetFocus();
	WShow_IllegalNameWarning->SetVisibility(ESlateVisibility::Hidden);
	SetVisibility(ESlateVisibility::Visible);
}

void UDlFm_DialogNewItem_Logic::DoWork_ChooseYes()
{
	const FString TmpDirName=Text_NewName->GetText().ToString();
	const FString TmpSanitizedDirName=FPaths::MakeValidFileName(TmpDirName);
	if(TmpSanitizedDirName.Equals(TmpDirName)==false)
	{
		WShow_IllegalNameWarning->SetVisibility(ESlateVisibility::Visible);
		Text_NewName->SetText(FText::FromString(TmpSanitizedDirName));
	}
	else
	{
		WShow_IllegalNameWarning->SetVisibility(ESlateVisibility::Hidden);
		PlannedNewName=TmpDirName;
		Super::DoWork_ChooseYes();
	}
}

void UDlFm_DialogNewItem_Logic::NativeConstruct()
{
	Super::NativeConstruct();

	WShow_IllegalNameWarning->SetVisibility(ESlateVisibility::Hidden);
}

void UDlFm_DialogPasteOverwrite_Logic::SetInfo_FileAlreadyExist(
	const FDlUI_FileExplorer_Lib::FBundleAction& BundleAction, const int32 Index)
{
	SetVisibility(ESlateVisibility::Visible);
	Text_ItemName_Src->SetText(FText::FromString(BundleAction.FailedBuffer[Index].Name));
	if(IsValid(Text_AccessDate_Src))
	{
		Text_AccessDate_Src->SetText(FText::AsDate(BundleAction.FailedBuffer[Index].AccessTime));
	}
	if(IsValid(Text_CreationDate_Src))
	{
		Text_CreationDate_Src->SetText(FText::AsDate(BundleAction.FailedBuffer[Index].CreationTime));
	}
	if(IsValid(Text_ModificationDate_Src))
	{
		Text_ModificationDate_Src->SetText(FText::AsDate(BundleAction.FailedBuffer[Index].ModificationTime));
	}
	if(IsValid(WShow_ReadOnly_Src))
	{
		WShow_ReadOnly_Src->SetVisibility(BundleAction.FailedBuffer[Index].bIsReadOnly?ESlateVisibility::Visible:ESlateVisibility::Hidden);
	}
	if(IsValid(Text_Size_Src))
	{
		Text_Size_Src->SetText(FText::AsMemory(BundleAction.FailedBuffer[Index].FileSize));
	}

	
	Text_ItemName_Des->SetText(FText::FromString(BundleAction.FailedBuffer_Info[Index].Name));
	if(IsValid(Text_AccessDate_Des))
	{
		Text_AccessDate_Des->SetText(FText::AsDate(BundleAction.FailedBuffer_Info[Index].AccessTime));
	}
	if(IsValid(Text_CreationDate_Des))
	{
		Text_CreationDate_Des->SetText(FText::AsDate(BundleAction.FailedBuffer_Info[Index].CreationTime));
	}
	if(IsValid(Text_ModificationDate_Des))
	{
		Text_ModificationDate_Des->SetText(FText::AsDate(BundleAction.FailedBuffer_Info[Index].ModificationTime));
	}
	if(IsValid(WShow_ReadOnly_Des))
	{
		WShow_ReadOnly_Des->SetVisibility(BundleAction.FailedBuffer_Info[Index].bIsReadOnly?ESlateVisibility::Visible:ESlateVisibility::Hidden);
	}
	if(IsValid(Text_Size_Des))
	{
		Text_Size_Des->SetText(FText::AsMemory(BundleAction.FailedBuffer_Info[Index].FileSize));
	}

	if(IsValid(Text_CounterInfo))
	{
		Text_CounterInfo->SetText(FText::FromString(FString::Printf(TEXT("%d files already exist, Please choose whether overwrite"),(BundleAction.FailedBuffer.Num()-Index))));
	}
}


