// Copyright(c) 2022 Dragon Li Software Limited. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "DlFmDialog_Base_Logic.h"
#include "DlFmDialog_Logic.generated.h"

class UTextBlock;
class UEditableText;
namespace FDlUI_FileExplorer_Lib
{
	struct FBundleAction;
};
/**
 * 
 */
UCLASS()
class DLFILEMANAGER_API UDlFm_DialogDelete_Logic : public UDlFmDialog_Base_Logic
{
	GENERATED_BODY()
public:
	void SetInfo_ConfirmDelete(const FDlUI_FileExplorer_Lib::FBundleAction& BundleAction);
	void SetInfo_DeleteFailed(const FDlUI_FileExplorer_Lib::FBundleAction& BundleAction);
private:
	UPROPERTY(meta = (BindWidget))
	UTextBlock* Text_DeleteInfo;
};

UCLASS()
class DLFILEMANAGER_API UDlFm_DialogNewItem_Logic : public UDlFmDialog_Base_Logic
{
	GENERATED_BODY()
public:
	void SetInfo_StartNewItem(const FString & DefaultDirName);
	virtual void DoWork_ChooseYes() override;
	//Make Dir cannot be done without widget
	FString PlannedNewName;

	virtual void NativeConstruct() override;
private:
	UPROPERTY(meta = (BindWidget))
	UEditableText* Text_NewName;
	UPROPERTY(meta = (BindWidget))
	UWidget* WShow_IllegalNameWarning;
};

UCLASS()
class DLFILEMANAGER_API UDlFm_DialogPasteOverwrite_Logic : public UDlFmDialog_Base_Logic
{
	GENERATED_BODY()
public:
	void SetInfo_FileAlreadyExist(const FDlUI_FileExplorer_Lib::FBundleAction& BundleAction,int32 Index);
	//No status info should be stored within dialog widget, because dialog action should work without widget
	//int32 CurrentIndex;
private:
	
	UPROPERTY(meta = (BindWidgetOptional))
	UTextBlock* Text_CounterInfo;
	
	UPROPERTY(meta = (BindWidget))
	UTextBlock* Text_ItemName_Src;
	UPROPERTY(meta = (BindWidgetOptional))
	UTextBlock* Text_AccessDate_Src;
	UPROPERTY(meta = (BindWidgetOptional))
	UTextBlock* Text_CreationDate_Src;
	UPROPERTY(meta = (BindWidgetOptional))
	UTextBlock* Text_ModificationDate_Src;
	UPROPERTY(meta = (BindWidgetOptional))
	UWidget* WShow_ReadOnly_Src;
	UPROPERTY(meta = (BindWidgetOptional))
	UTextBlock* Text_Size_Src;
	
	UPROPERTY(meta = (BindWidget))
	UTextBlock* Text_ItemName_Des;
	UPROPERTY(meta = (BindWidgetOptional))
	UTextBlock* Text_AccessDate_Des;
	UPROPERTY(meta = (BindWidgetOptional))
	UTextBlock* Text_CreationDate_Des;
	UPROPERTY(meta = (BindWidgetOptional))
	UTextBlock* Text_ModificationDate_Des;
	UPROPERTY(meta = (BindWidgetOptional))
	UWidget* WShow_ReadOnly_Des;
	UPROPERTY(meta = (BindWidgetOptional))
	UTextBlock* Text_Size_Des;
};