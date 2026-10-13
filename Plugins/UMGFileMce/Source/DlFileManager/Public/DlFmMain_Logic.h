// Copyright(c) 2022 Dragon Li Software Limited. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "DlFmDialog_Base_Logic.h"
#include "Blueprint/UserWidget.h"
#include "Components/ComboBoxString.h"
#include "DlFmLib.h"
#include "DlFmMain_Logic.generated.h"

class UWidgetTree;
class UEditableText;
class UTextBlock;
class UImage;
class UDlFmMain_Logic;
class UDlFmButton;
class UDlFmFilteredToggleButton;
class UVerticalBox;
class UDlUI_DialogInfo_Logic;
class UDlUI_DialogChooseOnce_Logic;
class UDlFmDialog_Base_Logic;
class UDlFm_DialogDelete_Logic;
class UDlFm_DialogPasteOverwrite_Logic;
class UDlFm_DialogNewItem_Logic;
class UComboBoxString;
class UExpandableArea;
class UListView;

UCLASS()
class DLFILEMANAGER_API UDlFmMain_Logic : public UUserWidget
{
	GENERATED_BODY()
public:
	DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnFileListSelectConfirmed, const TArray<FDlUI_FileExplorer_ItemInfo>&, FileNodeList);

	UPROPERTY(BlueprintAssignable)
	FOnFileListSelectConfirmed OnFileListSelectConfirmed;
	TArray<FDlUI_FileExplorer_ItemInfo> Buffer_OnFileListSelectConfirmed;

	virtual void NativeConstruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;


	UFUNCTION(BlueprintCallable, Category="File Manager API")
	void Func_BrowseForward();
	UFUNCTION(BlueprintCallable, Category="File Manager API")
	void Func_BrowseBackward();
	UFUNCTION(BlueprintCallable, Category="File Manager API")
	void Func_Refresh();

	UFUNCTION(BlueprintCallable, Category="File Manager API")
	const FString& Func_GetCurrentPathName();
	UFUNCTION(BlueprintCallable, Category="File Manager API")
	void Func_BrowseToPath(const FString& Val);


	UPROPERTY(EditAnywhere, Category="File Manager Behavior")
	EDlFm_SysPath StartPathType = EDlFm_SysPath::UE_ProjectDir;
	UPROPERTY(EditAnywhere, Category="File Manager Behavior",
		meta=(EditCondition="StartPathType==EDlUI_FileExplorer_SysPath::Custom"))
	FString Customized_StartPath;
	UPROPERTY(EditAnywhere, Category="File Manager Behavior")
	FString StartPath_Offset;
	UPROPERTY(EditAnywhere, Category="File Manager Behavior")
	int32 DirPathStack_Depth = 10;
	UPROPERTY(EditAnywhere, Category="File Manager Behavior")
	bool bUseSingleClickForConfirm=false;



	

	void Internal_Refresh();
	void Internal_RefreshWidgetMain_NaviButton() const;
	void Internal_RefreshWidgetSegment_All();
	void Internal_RefreshWidgetMain_FunctionButton() const;
	void Internal_RefreshWidgetMain_SortControl() const;
	void Internal_RefreshWidgetMain_SelectModeRelated();
private:
	
	UPROPERTY(meta = (BindWidgetOptional))
	UTextBlock* Text_InternalInfo;
	UPROPERTY(EditAnywhere, Category="File Manager Behavior")
	bool bAlwaysUseFullPath = true;
	UPROPERTY(meta = (BindWidgetOptional))
	UEditableText* Text_FilePath;
	UFUNCTION()
	void OnInternalCommit_FilePath(const FText& Text, ETextCommit::Type CommitMethod);

	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmFilteredToggleButton* Toggle_MultiSelect;
	UFUNCTION()
	void OnInternalChanged_MultiSelect(bool Val);
	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmFilteredToggleButton* Toggle_MultiSelect_SelectAll;
	UFUNCTION()
	void OnInternalChanged_MultiSelect_SelectAll(bool Val);

	UPROPERTY(meta = (BindWidgetOptional))
	UWidget* WShow_IsBusy;
	void Internal_Busy_Begin() const;
	void Internal_Busy_End() const;

	UPROPERTY(EditAnywhere, Category="File Manager Behavior")
	bool bSearch_AutoAppendAsterisk = true;
	UPROPERTY(meta = (BindWidgetOptional))
	UEditableText* Text_SearchContent;
	UFUNCTION()
	void OnInternalCommit_Search(const FText& Text, ETextCommit::Type CommitMethod);
	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmButton* Button_GoToSearchedFileLocation;
	UFUNCTION()
	void OnInternalClick_GoToSearchedFileLocation();

	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmButton* Button_DirGoBack;
	UFUNCTION()
	void OnInternalClick_DirGoBack();
	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmButton* Button_DirGoForward;
	UFUNCTION()
	void OnInternalClick_DirGoForward();


	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmButton* Button_SortBy_Name;
	UFUNCTION()
	void OnInternalClick_SortByName();
	UPROPERTY(meta = (BindWidgetOptional))
	UWidget* WShow_SortByName_Ascending;
	UPROPERTY(meta = (BindWidgetOptional))
	UWidget* WShow_SortByName_Descending;

	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmButton* Button_SortBy_Size;
	UFUNCTION()
	void OnInternalClick_SortBySize();
	UPROPERTY(meta = (BindWidgetOptional))
	UWidget* WShow_SortBySize_Ascending;
	UPROPERTY(meta = (BindWidgetOptional))
	UWidget* WShow_SortBySize_Descending;

	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmButton* Button_SortBy_CreationTime;
	UFUNCTION()
	void OnInternalClick_SortByCreationTime();
	UPROPERTY(meta = (BindWidgetOptional))
	UWidget* WShow_SortByCreationTime_Ascending;
	UPROPERTY(meta = (BindWidgetOptional))
	UWidget* WShow_SortByCreationTime_Descending;

	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmButton* Button_SortBy_AccessTime;
	UFUNCTION()
	void OnInternalClick_SortByAccessTime();
	UPROPERTY(meta = (BindWidgetOptional))
	UWidget* WShow_SortByAccessTime_Ascending;
	UPROPERTY(meta = (BindWidgetOptional))
	UWidget* WShow_SortByAccessTime_Descending;

	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmButton* Button_SortBy_ModifyTime;
	UFUNCTION()
	void OnInternalClick_SortByModifyTime();
	UPROPERTY(meta = (BindWidgetOptional))
	UWidget* WShow_SortByModifyTime_Ascending;
	UPROPERTY(meta = (BindWidgetOptional))
	UWidget* WShow_SortByModifyTime_Descending;

	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmButton* Button_SortBy_FileType;
	UFUNCTION()
	void OnInternalClick_SortByFileType();
	UPROPERTY(meta = (BindWidgetOptional))
	UWidget* WShow_SortByFileType_Ascending;
	UPROPERTY(meta = (BindWidgetOptional))
	UWidget* WShow_SortByFileType_Descending;

	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmFilteredToggleButton* Toggle_ShowDir;
	UFUNCTION()
	void OnInternalCheckStateChanged_ShowDir(bool Val);
	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmFilteredToggleButton* Toggle_ShowFile;
	UFUNCTION()
	void OnInternalCheckStateChanged_ShowFile(bool Val);
	UPROPERTY(meta = (BindWidget))
	UListView* Container_DirectoryContainer;
	UPROPERTY()
	TArray<UDlFm_Item_Data*> ItemPool_Directory;
	UPROPERTY(meta = (BindWidget))
	UListView* Container_FileContainer;
	UPROPERTY()
	TArray<UDlFm_Item_Data*> ItemPool_File;


public:
	UFUNCTION()
	void OnInternal_ItemClicked(const int32 Index, EDlUI_FileExplorer_ListSegment ListSegment);
	UFUNCTION() 
	void OnInternal_ItemFunctionButtonClicked(const int32 Index, EDlUI_FileExplorer_ListSegment ListSegment);
	UFUNCTION()
	void OnInternal_ItemRenameCommitted(const int32 Index, EDlUI_FileExplorer_ListSegment Segment, const FText& Name);

	const FDlUI_FileExplorer_Lib::FCore& GetCore(){return Core;}
	const FDlUI_FileExplorer_Lib::FBundleAction& GetBundleAction(){return BundleAction;}
private:
	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmButton* Button_Delete;
	UFUNCTION()
	void OnInternalClicked_Delete();
	UPROPERTY(EditAnywhere, Category="File Manager Behavior")
	EDlFm_DialogEventType Type_When_DialogDeleteConfirm_Absent = EDlFm_DialogEventType::Click_Yes;
	UPROPERTY(meta = (BindWidgetOptional))
	UDlFm_DialogDelete_Logic* Dialog_DeleteConfirm;
	UFUNCTION()
	void OnInternalDialog_DeleteConfirm(EDlFm_DialogEventType Type);

	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmButton* Button_Rename;
	UFUNCTION()
	void OnInternalClicked_Rename();

	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmButton* Button_Cut;
	UFUNCTION()
	void OnInternalClicked_Cut();

	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmButton* Button_Copy;
	UFUNCTION()
	void OnInternalClicked_Copy();

	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmButton* Button_Paste;
	UFUNCTION()
	void OnInternalClicked_Paste();
	UPROPERTY(EditAnywhere, Category="File Manager Behavior")
	EDlFm_DialogEventType Type_When_DialogPasteTargetAlreadyExist_Absent = EDlFm_DialogEventType::Click_NoForAll;
	UPROPERTY(meta = (BindWidgetOptional))
	UDlFm_DialogPasteOverwrite_Logic* Dialog_PasteTargetAlreadyExist;
	int32 Dialog_PasteTargetAlreadyExist_CurrentIndex;
	UFUNCTION()
	void OnInternalDialog_PasteTargetAlreadyExist(EDlFm_DialogEventType Type);
	UPROPERTY(EditAnywhere, Category="File Manager Behavior")
	bool bTryCleanEmptyDirAfterCutPaste = true;

	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmButton* Button_Refresh;
	UFUNCTION()
	void OnInternalClicked_Refresh();

	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmButton* Button_BrowseTo_ParentDir;
	UFUNCTION()
	void OnInternalClicked_BrowseParentDir();

	UPROPERTY(EditAnywhere, Category="File Manager Behavior")
	FString NewDirDefaultName = TEXT("New Directory");
	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmButton* Button_MakeDir;
	UFUNCTION()
	void OnInternalClicked_MakeDir();
	//Make Dir cannot be done without dialog widget;
	UPROPERTY(meta = (BindWidgetOptional))
	UDlFm_DialogNewItem_Logic* Dialog_MakeDir;
	UFUNCTION()
	void OnInternalDialog_MakeDir(EDlFm_DialogEventType Type);


	UPROPERTY(EditAnywhere, Category="File Manager Behavior")
	FString NewFileDefaultName = TEXT("NewFile.txt");
	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmButton* Button_CreateFile;
	UFUNCTION()
	void OnInternalClicked_CreateFile();
	//Make Dir cannot be done without dialog widget;
	UPROPERTY(meta = (BindWidgetOptional))
	UDlFm_DialogNewItem_Logic* Dialog_CreateFile;
	UFUNCTION()
	void OnInternalDialog_CreateFile(EDlFm_DialogEventType Type);

	UPROPERTY(meta = (BindWidgetOptional))
	UComboBoxString* Combo_ExtensionFilter;
	UFUNCTION()
	void OnInternalSelectionChanged_ExtensionFilter(FString SelectedItem, ESelectInfo::Type SelectionType);

	UPROPERTY(EditAnywhere, Category="File Manager Behavior")
	bool bSelectCurrentPathWhenNothingSelect=true;
	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmButton* Button_SelectConfirm;
	UFUNCTION()
	void OnInternalClicked_SelectConfirm();
	
	UPROPERTY(meta = (BindWidgetOptional))
	UWidget* Container_FunctionButton;
	
	//UPROPERTY(meta = (BindWidgetOptional))
	//UWidget* Widget_DialogBackground_HitBlocker;

	FDlUI_FileExplorer_Lib::FBundleAction BundleAction;
	FDlUI_FileExplorer_Lib::FCore Core;
	void SetExploreMode(FDlUI_FileExplorer_Lib::EExploreMode ExploreMode);
	FDlUI_FileExplorer_Lib::FPathStack CurDirPath_Stack;
	TArray<FDlUI_FileExplorer_Lib::FSegment> Segment_List;

	FDlUI_FileExplorer_Lib::FIterator_DirItemReader Iterator_ReadDir;
	FDlUI_FileExplorer_Lib::FIterator_DirPaste Iterator_DirCopySync;


	void DoWork_ReadDir();
	void DoWork_SearchDir();
	void DoWork_Delete();
	static bool Iterator_Delete(const FDlUI_FileExplorer_ItemInfo& TargetInfo,
	                                FDlUI_FileExplorer_Lib::FBundleAction& BundleActionVal);
	void DoWork_StartMakeDir() const;
	void DoWork_MakeDir(const FString& DirName);
	void DoWork_StartCreateFile() const;
	void DoWork_CreateFile(const FString& FileName);
	void DoWork_Rename();
	void DoWork_Cut();
	void DoWork_Copy();
	void DoWork_Paste();
	static bool Iterator_Paste(const FDlUI_FileExplorer_ItemInfo& TargetInfo,
	                               FDlUI_FileExplorer_Lib::FBundleAction& BundleActionV, const FString& Param1);
	void DoWork_BrowseParentDir();
	void DoWork_CacheError_ReadSearchDirNotExist();
	void DoWork_ApplyExtensionFilter(const FString& SelectedItem);

	static void DoWork_RecursivePasteDir(const FString& DesDir, const FString& SrcDir, bool OverwriteFile,
	                                     FDlUI_FileExplorer_Lib::FBundleAction* BundleActionV);
	void DoWork_SelectConfirm();
	void DoWork_Sort();
	
	IFileManager& FileManager = IFileManager::Get();
};
