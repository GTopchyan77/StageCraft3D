// Copyright(c) 2022 Dragon Li Software Limited. All Rights Reserved.


#include "DlFmMain_Logic.h"

#include "Blueprint/WidgetTree.h"
#include "Components/EditableText.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "Components/ListView.h"
#include "Components/TextBlock.h"
#include "DlFmDialog_Logic.h"
#include "UI/Fragment/DlFmFilteredToggleButton.h"



void UDlFmMain_Logic::NativeConstruct()
{
	Super::NativeConstruct();

	if (IsValid(Text_FilePath))
	{
		Text_FilePath->OnTextCommitted.AddUniqueDynamic(this, &UDlFmMain_Logic::OnInternalCommit_FilePath);
	}
	if (IsValid(Toggle_MultiSelect))
	{
		Toggle_MultiSelect->OnCheckStateChanged.AddUniqueDynamic(
			this, &UDlFmMain_Logic::OnInternalChanged_MultiSelect);
	}

	if (IsValid(Toggle_MultiSelect_SelectAll))
	{
		Toggle_MultiSelect_SelectAll->OnCheckStateChanged.AddUniqueDynamic(
			this, &UDlFmMain_Logic::OnInternalChanged_MultiSelect_SelectAll);
	}

	if (IsValid(Button_DirGoBack))
	{
		Button_DirGoBack->OnFilteredClicked.
		                  AddUniqueDynamic(this, &UDlFmMain_Logic::OnInternalClick_DirGoBack);
	}

	if (IsValid(Button_DirGoForward))
	{
		Button_DirGoForward->OnFilteredClicked.AddUniqueDynamic(
			this, &UDlFmMain_Logic::OnInternalClick_DirGoForward);
	}

	if (IsValid(Text_SearchContent))
	{
		Text_SearchContent->SetText(FText::GetEmpty());
		Text_SearchContent->OnTextCommitted.AddUniqueDynamic(this, &UDlFmMain_Logic::OnInternalCommit_Search);
	}
	if (IsValid(Button_GoToSearchedFileLocation))
	{
		Button_GoToSearchedFileLocation->OnFilteredClicked.AddUniqueDynamic(
			this, &UDlFmMain_Logic::OnInternalClick_GoToSearchedFileLocation);
	}

	if (IsValid(Button_SortBy_Name))
	{
		Button_SortBy_Name->OnFilteredClicked.AddUniqueDynamic(
			this, &UDlFmMain_Logic::OnInternalClick_SortByName);
	}

	if (IsValid(Button_SortBy_Size))
	{
		Button_SortBy_Size->OnFilteredClicked.AddUniqueDynamic(
			this, &UDlFmMain_Logic::OnInternalClick_SortBySize);
	}

	if (IsValid(Button_SortBy_CreationTime))
	{
		Button_SortBy_CreationTime->OnFilteredClicked.AddUniqueDynamic(
			this, &UDlFmMain_Logic::OnInternalClick_SortByCreationTime);
	}

	if (IsValid(Button_SortBy_AccessTime))
	{
		Button_SortBy_AccessTime->OnFilteredClicked.AddUniqueDynamic(
			this, &UDlFmMain_Logic::OnInternalClick_SortByAccessTime);
	}

	if (IsValid(Button_SortBy_ModifyTime))
	{
		Button_SortBy_ModifyTime->OnFilteredClicked.AddUniqueDynamic(
			this, &UDlFmMain_Logic::OnInternalClick_SortByModifyTime);
	}

	if (IsValid(Button_SortBy_FileType))
	{
		Button_SortBy_FileType->OnFilteredClicked.AddUniqueDynamic(
			this, &UDlFmMain_Logic::OnInternalClick_SortByFileType);
	}

	if (IsValid(Toggle_ShowDir))
	{
		Toggle_ShowDir->OnCheckStateChanged.AddUniqueDynamic(
			this, &UDlFmMain_Logic::OnInternalCheckStateChanged_ShowDir);
		OnInternalCheckStateChanged_ShowDir(Toggle_ShowDir->IsChecked());
	}
	if (IsValid(Toggle_ShowFile))
	{
		Toggle_ShowFile->OnCheckStateChanged.AddUniqueDynamic(
			this, &UDlFmMain_Logic::OnInternalCheckStateChanged_ShowFile);
		OnInternalCheckStateChanged_ShowFile(Toggle_ShowFile->IsChecked());
	}
	Container_DirectoryContainer->SetSelectionMode(ESelectionMode::None);
	Container_FileContainer->SetSelectionMode(ESelectionMode::None);


	if (IsValid(Button_Delete))
	{
		Button_Delete->OnFilteredClicked.AddUniqueDynamic(this, &UDlFmMain_Logic::OnInternalClicked_Delete);
	}

	if (IsValid(Button_Rename))
	{
		Button_Rename->OnFilteredClicked.AddUniqueDynamic(this, &UDlFmMain_Logic::OnInternalClicked_Rename);
	}

	if (IsValid(Button_Cut))
	{
		Button_Cut->OnFilteredClicked.AddUniqueDynamic(this, &UDlFmMain_Logic::OnInternalClicked_Cut);
	}

	if (IsValid(Button_Copy))
	{
		Button_Copy->OnFilteredClicked.AddUniqueDynamic(this, &UDlFmMain_Logic::OnInternalClicked_Copy);
	}

	if (IsValid(Button_Paste))
	{
		Button_Paste->OnFilteredClicked.AddUniqueDynamic(this, &UDlFmMain_Logic::OnInternalClicked_Paste);
	}

	if (IsValid(Button_Refresh))
	{
		Button_Refresh->OnFilteredClicked.AddUniqueDynamic(this, &UDlFmMain_Logic::OnInternalClicked_Refresh);
	}

	if (IsValid(Button_BrowseTo_ParentDir))
	{
		Button_BrowseTo_ParentDir->OnFilteredClicked.AddUniqueDynamic(
			this, &UDlFmMain_Logic::OnInternalClicked_BrowseParentDir);
	}

	if (IsValid(Button_MakeDir))
	{
		Button_MakeDir->OnFilteredClicked.AddUniqueDynamic(this, &UDlFmMain_Logic::OnInternalClicked_MakeDir);

		if (IsValid(Dialog_MakeDir))
		{
			Dialog_MakeDir->OnDialogClicked.AddUniqueDynamic(this, &UDlFmMain_Logic::OnInternalDialog_MakeDir);
			Dialog_MakeDir->SetVisibility(ESlateVisibility::Hidden);
		}
		else
		{
			//Button_MakeDir is useless without Dialog_MakeDir
			Button_MakeDir->SetVisibility(ESlateVisibility::Hidden);
		}
	}
	if (IsValid(Button_CreateFile))
	{
		Button_CreateFile->OnFilteredClicked.AddUniqueDynamic(
			this, &UDlFmMain_Logic::OnInternalClicked_CreateFile);

		if (IsValid(Dialog_CreateFile))
		{
			Dialog_CreateFile->OnDialogClicked.AddUniqueDynamic(
				this, &UDlFmMain_Logic::OnInternalDialog_CreateFile);
			Dialog_CreateFile->SetVisibility(ESlateVisibility::Hidden);
		}
		else
		{
			//Button_CreateFile is useless without Dialog_MakeDir
			Button_CreateFile->SetVisibility(ESlateVisibility::Hidden);
		}
	}

	if (IsValid(Dialog_DeleteConfirm))
	{
		Dialog_DeleteConfirm->OnDialogClicked.AddUniqueDynamic(
			this, &UDlFmMain_Logic::OnInternalDialog_DeleteConfirm);
		Dialog_DeleteConfirm->SetVisibility(ESlateVisibility::Hidden);
	}
	if (IsValid(Dialog_PasteTargetAlreadyExist))
	{
		Dialog_PasteTargetAlreadyExist->OnDialogClicked.AddUniqueDynamic(
			this, &UDlFmMain_Logic::OnInternalDialog_PasteTargetAlreadyExist);
		Dialog_PasteTargetAlreadyExist->SetVisibility(ESlateVisibility::Hidden);
	}
	if (IsValid(Combo_ExtensionFilter))
	{
		Combo_ExtensionFilter->OnSelectionChanged.AddUniqueDynamic(
			this, &UDlFmMain_Logic::OnInternalSelectionChanged_ExtensionFilter);
	}
	if (IsValid(Button_SelectConfirm))
	{
		Button_SelectConfirm->OnFilteredClicked.AddUniqueDynamic(
			this, &UDlFmMain_Logic::OnInternalClicked_SelectConfirm);
	}

	Core.Init(this, &Segment_List, Toggle_MultiSelect, Toggle_MultiSelect_SelectAll, Container_FunctionButton,&BundleAction);
	Iterator_ReadDir.Init_ForRead(&Segment_List);
	Segment_List.SetNum(static_cast<int32>(EDlUI_FileExplorer_ListSegment::All_Num));
	Segment_List[static_cast<uint8>(EDlUI_FileExplorer_ListSegment::Directory)].InitClear(
		Container_DirectoryContainer, &ItemPool_Directory, this);
	Segment_List[static_cast<uint8>(EDlUI_FileExplorer_ListSegment::File)].InitClear(
		Container_FileContainer, &ItemPool_File,this);
	CurDirPath_Stack.Init(this, bAlwaysUseFullPath);
	BundleAction.Init(this,&Segment_List);

	Core.SetSelectMode(FDlUI_FileExplorer_Lib::ESelectMode::Single);
	switch (StartPathType)
	{
	case EDlFm_SysPath::Custom:
		CurDirPath_Stack.InitStack(Customized_StartPath + StartPath_Offset, DirPathStack_Depth);
		break;
	case EDlFm_SysPath::UE_RootDir:
		CurDirPath_Stack.InitStack(FPaths::RootDir() + StartPath_Offset, DirPathStack_Depth);
		break;
	case EDlFm_SysPath::UE_ProjectDir:
		CurDirPath_Stack.InitStack(FPaths::ProjectDir() + StartPath_Offset, DirPathStack_Depth);
		break;
	case EDlFm_SysPath::UE_ProjectUserDir:
		CurDirPath_Stack.InitStack(FPaths::ProjectUserDir() + StartPath_Offset, DirPathStack_Depth);
		break;
	case EDlFm_SysPath::UE_ProjectContentDir:
		CurDirPath_Stack.InitStack(FPaths::ProjectContentDir() + StartPath_Offset, DirPathStack_Depth);
		break;
	case EDlFm_SysPath::UE_ProjectConfigDir:
		CurDirPath_Stack.InitStack(FPaths::ProjectConfigDir() + StartPath_Offset, DirPathStack_Depth);
		break;
	case EDlFm_SysPath::UE_ProjectSavedDir:
		CurDirPath_Stack.InitStack(FPaths::ProjectSavedDir() + StartPath_Offset, DirPathStack_Depth);
		break;
	case EDlFm_SysPath::UE_ProjectIntermediateDir:
		CurDirPath_Stack.InitStack(FPaths::ProjectIntermediateDir() + StartPath_Offset, DirPathStack_Depth);
		break;
	case EDlFm_SysPath::UE_ShaderWorkingDir:
		CurDirPath_Stack.InitStack(FPaths::ShaderWorkingDir() + StartPath_Offset, DirPathStack_Depth);
		break;
	case EDlFm_SysPath::UE_ProjectPluginsDir:
		CurDirPath_Stack.InitStack(FPaths::ProjectPluginsDir() + StartPath_Offset, DirPathStack_Depth);
		break;
	case EDlFm_SysPath::UE_ProjectModsDir:
		CurDirPath_Stack.InitStack(FPaths::ProjectModsDir() + StartPath_Offset, DirPathStack_Depth);
		break;
	default:
		CurDirPath_Stack.InitStack(FString(TEXT("./")), DirPathStack_Depth);
		break;
	}

	if(IsValid(Combo_ExtensionFilter))
	{
		DoWork_ApplyExtensionFilter(Combo_ExtensionFilter->GetSelectedOption());
	}
	Func_Refresh();
}

void UDlFmMain_Logic::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	/*
	const float TmpCurTime = GetOwningPlayer()->GetWorld()->GetTimeSeconds();
	switch (Core.ExploreMode)
	{
	case FDlUI_FileExplorer_Lib::EExploreMode::Browse:
		if (bAutoRefresh_ForBrowse)
		{
			if (TmpCurTime > Core.CurTickRefresh_LastTime + AutoRefreshTimeGap_ForBrowse)
			{
				Func_Refresh();
				Core.CurTickRefresh_LastTime = TmpCurTime;
			}
		}
		break;
	case FDlUI_FileExplorer_Lib::EExploreMode::Search:
		if (bAutoRefresh_ForSearch)
		{
			if (TmpCurTime > Core.CurTickRefresh_LastTime + AutoRefreshTimeGap_ForSearch)
			{
				Func_Refresh();
				Core.CurTickRefresh_LastTime = TmpCurTime;
			}
		}
		break;
	}*/
}

void UDlFmMain_Logic::Func_BrowseForward()
{
	CurDirPath_Stack.FlowPoint_GoForward();
	Func_Refresh();
}

void UDlFmMain_Logic::Func_BrowseBackward()
{
	CurDirPath_Stack.FlowPoint_GoBack();
	Func_Refresh();
}

void UDlFmMain_Logic::Func_Refresh()
{
	switch (CurDirPath_Stack.FlowPoint_CurrentExploreMode())
	{
	case FDlUI_FileExplorer_Lib::EExploreMode::Browse:
		DoWork_ReadDir();
		break;
	case FDlUI_FileExplorer_Lib::EExploreMode::Search:
		DoWork_SearchDir();
		break;
	default: break;
	}
}

const FString& UDlFmMain_Logic::Func_GetCurrentPathName()
{
	return CurDirPath_Stack.FlowPoint_CurrentPath();
}

void UDlFmMain_Logic::Func_BrowseToPath(const FString& Val)
{
	CurDirPath_Stack.Push_AtFlowPointer(Val);

	Func_Refresh();
}


void UDlFmMain_Logic::Internal_Refresh()
{
	Core.Refresh_ShouldShow();
	Internal_RefreshWidgetMain_SortControl();
	DoWork_Sort();
	Internal_RefreshWidgetSegment_All();
	
	if (IsValid(Text_InternalInfo))
	{
		Text_InternalInfo->SetText(FText::FromString(FString::Printf(
			TEXT("%d folders | %d files"),
			Segment_List[static_cast<uint8>(EDlUI_FileExplorer_ListSegment::Directory)].VisibleItem_List.Num(),
			Segment_List[static_cast<uint8>(EDlUI_FileExplorer_ListSegment::File)].VisibleItem_List.Num())));
	}
}

void UDlFmMain_Logic::Internal_RefreshWidgetMain_NaviButton() const
{
	if (IsValid(Button_DirGoBack))
	{
		Button_DirGoBack->SetIsEnabled(!CurDirPath_Stack.IsFlowPointer_AtBottom());
	}
	if (IsValid(Button_DirGoForward))
	{
		Button_DirGoForward->SetIsEnabled(!CurDirPath_Stack.IsFlowPointer_AtTop());
	}
}

void UDlFmMain_Logic::Internal_RefreshWidgetSegment_All()
{
	for (uint8 TmpIndex = 0; TmpIndex < Segment_List.Num(); TmpIndex++)
	{
		Segment_List[TmpIndex].RefreshAllWidget_ModeStyle();
	}
}

void UDlFmMain_Logic::Internal_RefreshWidgetMain_FunctionButton() const
{
	//Functions are influenced by:
	//    Browse/Search(ExploreMode),
	//    whether something is currently selected (Core.SingleSelected_Valid)
	//    whether anything is being selected (Core.SelectedItem_Num)
	//    whether CopyPaste/CutPaste task is doing (BundleAction.Type)
	// So call this function whenever these factors are changed.
	switch (Core.ExploreMode)
	{
	case FDlUI_FileExplorer_Lib::EExploreMode::Browse:
		if (IsValid(Button_GoToSearchedFileLocation))
		{
			Button_GoToSearchedFileLocation->SetIsEnabled(false);
		}
		if (IsValid(Button_MakeDir) && IsValid(Dialog_MakeDir))
		{
			Button_MakeDir->SetIsEnabled(true);
		}
		if (IsValid(Button_CreateFile) && IsValid(Dialog_CreateFile))
		{
			Button_CreateFile->SetIsEnabled(true);
		}
		if(IsValid(Button_SelectConfirm))
		{
			if(bSelectCurrentPathWhenNothingSelect)
			{
				Button_SelectConfirm->SetIsEnabled(true);
			}
			else
			{
				if(Core.SelectedItem_Num>0)
				{
					Button_SelectConfirm->SetIsEnabled(true);
				}
				else
				{
					Button_SelectConfirm->SetIsEnabled(false);
				}
			}
		}
		break;
	case FDlUI_FileExplorer_Lib::EExploreMode::Search:
		if (IsValid(Button_GoToSearchedFileLocation))
		{
			if (Core.SingleSelected_Valid)
			{
				Button_GoToSearchedFileLocation->SetIsEnabled(true);
			}
			else
			{
				Button_GoToSearchedFileLocation->SetIsEnabled(false);
			}
		}
		if (IsValid(Button_MakeDir))
		{
			Button_MakeDir->SetIsEnabled(false);
		}
		if (IsValid(Button_CreateFile))
		{
			Button_CreateFile->SetIsEnabled(false);
		}
		if(IsValid(Button_SelectConfirm))
		{
			if(Core.SelectedItem_Num>0)
			{
				Button_SelectConfirm->SetIsEnabled(true);
			}
			else
			{
				Button_SelectConfirm->SetIsEnabled(false);
			}
		}
		break;
	}

	if(IsValid(Button_Cut))
	{
		if(Core.SelectedItem_Num>0)
		{
			Button_Cut->SetIsEnabled(true);
		}
		else
		{
			Button_Cut->SetIsEnabled(false);
		}
	}

	if(IsValid(Button_Copy))
	{
		if(Core.SelectedItem_Num>0)
		{
			Button_Copy->SetIsEnabled(true);
		}
		else
		{
			Button_Copy->SetIsEnabled(false);
		}
	}
	
	if (IsValid(Button_Paste))
	{
		if ((BundleAction.Type == FDlUI_FileExplorer_Lib::EBundleActionType::CopyPaste) ||
			(BundleAction.Type == FDlUI_FileExplorer_Lib::EBundleActionType::CutPaste))
		{
			Button_Paste->SetIsEnabled(true);
		}
		else
		{
			Button_Paste->SetIsEnabled(false);
		}
	}

	
	if(IsValid(Button_Rename))
	{
		if(Core.SingleSelected_Valid)
		{
			Button_Rename->SetIsEnabled(true);
		}
		else
		{
			Button_Rename->SetIsEnabled(false);
		}
	}

	if(IsValid(Button_Delete))
	{
		if(Core.SelectedItem_Num>0)
		{
			Button_Delete->SetIsEnabled(true);
		}
		else
		{
			Button_Delete->SetIsEnabled(false);
		}
	}
}

void UDlFmMain_Logic::Internal_RefreshWidgetMain_SortControl() const
{
	if (IsValid(WShow_SortByName_Ascending))
		WShow_SortByName_Ascending->SetVisibility(ESlateVisibility::Hidden);
	if (IsValid(WShow_SortByName_Descending))
		WShow_SortByName_Descending->SetVisibility(ESlateVisibility::Hidden);
	if (IsValid(WShow_SortBySize_Ascending))
		WShow_SortBySize_Ascending->SetVisibility(ESlateVisibility::Hidden);
	if (IsValid(WShow_SortBySize_Descending))
		WShow_SortBySize_Descending->SetVisibility(ESlateVisibility::Hidden);
	if (IsValid(WShow_SortByCreationTime_Ascending))
		WShow_SortByCreationTime_Ascending->SetVisibility(ESlateVisibility::Hidden);
	if (IsValid(WShow_SortByCreationTime_Descending))
		WShow_SortByCreationTime_Descending->SetVisibility(ESlateVisibility::Hidden);
	if (IsValid(WShow_SortByAccessTime_Ascending))
		WShow_SortByAccessTime_Ascending->SetVisibility(ESlateVisibility::Hidden);
	if (IsValid(WShow_SortByAccessTime_Descending))
		WShow_SortByAccessTime_Descending->SetVisibility(ESlateVisibility::Hidden);
	if (IsValid(WShow_SortByModifyTime_Ascending))
		WShow_SortByModifyTime_Ascending->SetVisibility(ESlateVisibility::Hidden);
	if (IsValid(WShow_SortByModifyTime_Descending))
		WShow_SortByModifyTime_Descending->SetVisibility(ESlateVisibility::Hidden);
	if (IsValid(WShow_SortByFileType_Ascending))
		WShow_SortByFileType_Ascending->SetVisibility(ESlateVisibility::Hidden);
	if (IsValid(WShow_SortByFileType_Descending))
		WShow_SortByFileType_Descending->SetVisibility(ESlateVisibility::Hidden);

	switch (Core.CurSortType)
	{
	case FDlUI_FileExplorer_Lib::ESortType::Name:
		if (Core.bReverseSort)
		{
			if (IsValid(WShow_SortByName_Ascending))
				WShow_SortByName_Ascending->SetVisibility(ESlateVisibility::Visible);
		}
		else
		{
			if (IsValid(WShow_SortByName_Descending))
				WShow_SortByName_Descending->SetVisibility(ESlateVisibility::Visible);
		}
		break;
	case FDlUI_FileExplorer_Lib::ESortType::Size:
		if (Core.bReverseSort)
		{
			if (IsValid(WShow_SortBySize_Ascending))
				WShow_SortBySize_Ascending->SetVisibility(ESlateVisibility::Visible);
		}
		else
		{
			if (IsValid(WShow_SortBySize_Descending))
				WShow_SortBySize_Descending->SetVisibility(ESlateVisibility::Visible);
		}
		break;
	case FDlUI_FileExplorer_Lib::ESortType::CreationTime:
		if (Core.bReverseSort)
		{
			if (IsValid(WShow_SortByCreationTime_Ascending))
				WShow_SortByCreationTime_Ascending->SetVisibility(ESlateVisibility::Visible);
		}
		else
		{
			if (IsValid(WShow_SortByCreationTime_Descending))
				WShow_SortByCreationTime_Descending->SetVisibility(ESlateVisibility::Visible);
		}
		break;
	case FDlUI_FileExplorer_Lib::ESortType::AccessTime:
		if (Core.bReverseSort)
		{
			if (IsValid(WShow_SortByAccessTime_Ascending))
				WShow_SortByAccessTime_Ascending->SetVisibility(ESlateVisibility::Visible);
		}
		else
		{
			if (IsValid(WShow_SortByAccessTime_Descending))
				WShow_SortByAccessTime_Descending->SetVisibility(ESlateVisibility::Visible);
		}
		break;
	case FDlUI_FileExplorer_Lib::ESortType::ModifyTime:
		if (Core.bReverseSort)
		{
			if (IsValid(WShow_SortByModifyTime_Ascending))
				WShow_SortByModifyTime_Ascending->SetVisibility(ESlateVisibility::Visible);
		}
		else
		{
			if (IsValid(WShow_SortByModifyTime_Descending))
				WShow_SortByModifyTime_Descending->SetVisibility(ESlateVisibility::Visible);
		}
		break;
	case FDlUI_FileExplorer_Lib::ESortType::FileType:
		if (Core.bReverseSort)
		{
			if (IsValid(WShow_SortByFileType_Ascending))
				WShow_SortByFileType_Ascending->SetVisibility(ESlateVisibility::Visible);
		}
		else
		{
			if (IsValid(WShow_SortByFileType_Descending))
				WShow_SortByFileType_Descending->SetVisibility(ESlateVisibility::Visible);
		}
		break;
	default:
		break;
	}
}

void UDlFmMain_Logic::Internal_RefreshWidgetMain_SelectModeRelated()
{
	switch (Core.SelectMode)
	{
	case FDlUI_FileExplorer_Lib::ESelectMode::Multiply:
		{
			if (IsValid(Toggle_MultiSelect))
			{
				Toggle_MultiSelect->SetIsChecked_NoBroadcast(true);
			}
			if (IsValid(Toggle_MultiSelect_SelectAll))
			{
				Toggle_MultiSelect_SelectAll->SetVisibility(ESlateVisibility::Visible);
				Toggle_MultiSelect_SelectAll->SetIsChecked_NoBroadcast(false);
			}
			if(IsValid(Container_FunctionButton))
			{
				Container_FunctionButton->SetVisibility(ESlateVisibility::Visible);
			}
		}
		break;
	case FDlUI_FileExplorer_Lib::ESelectMode::Single:
		{
			if (IsValid(Toggle_MultiSelect))
			{
				Toggle_MultiSelect->SetIsChecked_NoBroadcast(false);
			}
			if (IsValid(Toggle_MultiSelect_SelectAll))
			{
				Toggle_MultiSelect_SelectAll->SetVisibility(ESlateVisibility::Hidden);
			}
			if(IsValid(Container_FunctionButton))
			{
				if((BundleAction.Type==FDlUI_FileExplorer_Lib::EBundleActionType::CopyPaste)||(BundleAction.Type==FDlUI_FileExplorer_Lib::EBundleActionType::CutPaste))
				{
					Container_FunctionButton->SetVisibility(ESlateVisibility::Visible);
				}
				else
				{
					Container_FunctionButton->SetVisibility(ESlateVisibility::Collapsed);
				}
			}
		}
		break;
	default:
		{
			if (IsValid(Toggle_MultiSelect))
			{
				Toggle_MultiSelect->SetIsChecked_NoBroadcast(false);
			}
			if (IsValid(Toggle_MultiSelect_SelectAll))
			{
				Toggle_MultiSelect_SelectAll->SetVisibility(ESlateVisibility::Hidden);
			}
			if(IsValid(Container_FunctionButton))
			{
				Container_FunctionButton->SetVisibility(ESlateVisibility::Collapsed);
			}
		}
		break;
	}
	Internal_RefreshWidgetSegment_All();

}

void UDlFmMain_Logic::OnInternalCommit_FilePath(const FText& Text, ETextCommit::Type CommitMethod)
{
	Func_BrowseToPath(Text.ToString());
}

void UDlFmMain_Logic::OnInternalChanged_MultiSelect(bool Val)
{
	Core.SetSelectMode(
		Val ? FDlUI_FileExplorer_Lib::ESelectMode::Multiply : FDlUI_FileExplorer_Lib::ESelectMode::Single);
}

void UDlFmMain_Logic::OnInternalChanged_MultiSelect_SelectAll(bool Val)
{
	Core.SetSelect_All(Val);
}

void UDlFmMain_Logic::Internal_Busy_Begin() const
{
	if (IsValid(WShow_IsBusy))
	{
		WShow_IsBusy->SetVisibility(ESlateVisibility::Visible);
	}
}

void UDlFmMain_Logic::Internal_Busy_End() const
{
	if (IsValid(WShow_IsBusy))
	{
		WShow_IsBusy->SetVisibility(ESlateVisibility::Hidden);
	}
}

void UDlFmMain_Logic::OnInternalCommit_Search(const FText& Text, ETextCommit::Type CommitMethod)
{
	const FString TmpSearchContent = Text.ToString();
	if (TmpSearchContent.IsEmpty()) { return; }


	CurDirPath_Stack.Push_AtFlowPointer(CurDirPath_Stack.FlowPoint_CurrentPath(), TmpSearchContent,
	                                    FDlUI_FileExplorer_Lib::EExploreMode::Search);

	DoWork_SearchDir();
}

void UDlFmMain_Logic::OnInternalClick_GoToSearchedFileLocation()
{
	if ((Core.ExploreMode == FDlUI_FileExplorer_Lib::EExploreMode::Search) && (Core.SingleSelected_Valid))
	{
		const FString& TmpSelectedName = Segment_List[static_cast<uint8>(Core.SingleSelected_Segment)].DraftInfo_List[
			Core.SingleSelected_Index].Name;
		CurDirPath_Stack.Push_AtFlowPointer(FPaths::GetPath(TmpSelectedName));
		DoWork_ReadDir();
	}
}

void UDlFmMain_Logic::OnInternalClick_DirGoBack()
{
	Func_BrowseBackward();
}

void UDlFmMain_Logic::OnInternalClick_DirGoForward()
{
	Func_BrowseForward();
}

void UDlFmMain_Logic::OnInternalClick_SortByName()
{
	if (Core.CurSortType == FDlUI_FileExplorer_Lib::ESortType::Name)
	{
		Core.bReverseSort = !Core.bReverseSort;
	}
	else
	{
		Core.CurSortType = FDlUI_FileExplorer_Lib::ESortType::Name;
		Core.bReverseSort = false;
	}
	Internal_Refresh();
}

void UDlFmMain_Logic::OnInternalClick_SortBySize()
{
	if (Core.CurSortType == FDlUI_FileExplorer_Lib::ESortType::Size)
	{
		Core.bReverseSort = !Core.bReverseSort;
	}
	else
	{
		Core.CurSortType = FDlUI_FileExplorer_Lib::ESortType::Size;
		Core.bReverseSort = false;
	}

	Internal_Refresh();
}

void UDlFmMain_Logic::OnInternalClick_SortByCreationTime()
{
	if (Core.CurSortType == FDlUI_FileExplorer_Lib::ESortType::CreationTime)
	{
		Core.bReverseSort = !Core.bReverseSort;
	}
	else
	{
		Core.CurSortType = FDlUI_FileExplorer_Lib::ESortType::CreationTime;
		Core.bReverseSort = false;
	}

	Internal_Refresh();
}

void UDlFmMain_Logic::OnInternalClick_SortByAccessTime()
{
	if (Core.CurSortType == FDlUI_FileExplorer_Lib::ESortType::AccessTime)
	{
		Core.bReverseSort = !Core.bReverseSort;
	}
	else
	{
		Core.CurSortType = FDlUI_FileExplorer_Lib::ESortType::AccessTime;
		Core.bReverseSort = false;
	}

	Internal_Refresh();
}

void UDlFmMain_Logic::OnInternalClick_SortByModifyTime()
{
	if (Core.CurSortType == FDlUI_FileExplorer_Lib::ESortType::ModifyTime)
	{
		Core.bReverseSort = !Core.bReverseSort;
	}
	else
	{
		Core.CurSortType = FDlUI_FileExplorer_Lib::ESortType::ModifyTime;
		Core.bReverseSort = false;
	}

	Internal_Refresh();
}

void UDlFmMain_Logic::OnInternalClick_SortByFileType()
{
	if (Core.CurSortType == FDlUI_FileExplorer_Lib::ESortType::FileType)
	{
		Core.bReverseSort = !Core.bReverseSort;
	}
	else
	{
		Core.CurSortType = FDlUI_FileExplorer_Lib::ESortType::FileType;
		Core.bReverseSort = false;
	}
	Internal_Refresh();
}

void UDlFmMain_Logic::OnInternalCheckStateChanged_ShowDir(bool Val)
{
	Container_DirectoryContainer->SetVisibility(Val ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
}

void UDlFmMain_Logic::OnInternalCheckStateChanged_ShowFile(bool Val)
{
	Container_FileContainer->SetVisibility(Val ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
}

void UDlFmMain_Logic::OnInternal_ItemClicked(const int32 Index, EDlUI_FileExplorer_ListSegment ListSegment)
{
	Core.NewClick(Index, ListSegment);
}

void UDlFmMain_Logic::OnInternal_ItemFunctionButtonClicked(const int32 Index,
	EDlUI_FileExplorer_ListSegment ListSegment)
{
	Core.SetSelectMode(FDlUI_FileExplorer_Lib::ESelectMode::Single);
	Core.Single_SelectChange(Index,ListSegment);
	if(IsValid( Container_FunctionButton))
	{
		Container_FunctionButton->SetVisibility(ESlateVisibility::Visible);
	}
}

void UDlFmMain_Logic::OnInternal_ItemRenameCommitted(const int32 Index, EDlUI_FileExplorer_ListSegment Segment,
													 const FText& Name)
{
	const FString& OldName = Segment_List[static_cast<int32>(Segment)].DraftInfo_List[Index].Name;
	const FString TmpPath = FPaths::GetPath(OldName);
	FString NewName = TmpPath + TEXT("/") + Name.ToString();
	FPaths::MakeStandardFilename(NewName); //Do name sanitation here
	if (!OldName.Equals(NewName))
	{
		FileManager.Move(*NewName, *OldName);
	}
	
	Segment_List[static_cast<int32>(Segment)].DraftInfo_List[Index].SpecialAction=FDlUI_FileExplorer_Lib::ESpecialAction::None;
	BundleAction.Clear();
	Func_Refresh();
}


void UDlFmMain_Logic::OnInternalClicked_Delete()
{
	BundleAction.PrepareWork(FDlUI_FileExplorer_Lib::EBundleActionType::PrepareDelete);
	if (IsValid(Dialog_DeleteConfirm))
	{
		Dialog_DeleteConfirm->SetInfo_ConfirmDelete(BundleAction);
	}
	else
	{
		OnInternalDialog_DeleteConfirm(Type_When_DialogDeleteConfirm_Absent);
	}
}

void UDlFmMain_Logic::OnInternalDialog_DeleteConfirm(const EDlFm_DialogEventType Type)
{
	switch (Type)
	{
	case EDlFm_DialogEventType::Click_Yes:
		DoWork_Delete();
		break;
	default:
		BundleAction.Clear();
		break;
	}
}

void UDlFmMain_Logic::OnInternalClicked_Rename()
{
	DoWork_Rename();
}

void UDlFmMain_Logic::OnInternalClicked_Cut()
{
	DoWork_Cut();
}

void UDlFmMain_Logic::OnInternalClicked_Copy()
{
	DoWork_Copy();
}

void UDlFmMain_Logic::OnInternalClicked_Paste()
{
	DoWork_Paste();
}

void UDlFmMain_Logic::OnInternalDialog_PasteTargetAlreadyExist(const EDlFm_DialogEventType Type)
{
	switch (Type)
	{
	case EDlFm_DialogEventType::Click_Yes:
		{
			int32& TmpIndex = Dialog_PasteTargetAlreadyExist_CurrentIndex;
			if (BundleAction.Type == FDlUI_FileExplorer_Lib::EBundleActionType::CopyPaste)
			{
				FileManager.Copy(*BundleAction.FailedBuffer_Info[TmpIndex].Name,
				                 *BundleAction.FailedBuffer[TmpIndex].Name, true);
			}
			else if (BundleAction.Type == FDlUI_FileExplorer_Lib::EBundleActionType::CutPaste)
			{
				FileManager.Move(*BundleAction.FailedBuffer_Info[TmpIndex].Name,
				                 *BundleAction.FailedBuffer[TmpIndex].Name, true);
			}
			TmpIndex++;
			if (TmpIndex < BundleAction.FailedBuffer.Num())
			{
				if (IsValid(Dialog_PasteTargetAlreadyExist))
				{
					Dialog_PasteTargetAlreadyExist->SetInfo_FileAlreadyExist(BundleAction, TmpIndex);
				}
				else
				{
					OnInternalDialog_PasteTargetAlreadyExist(Type_When_DialogPasteTargetAlreadyExist_Absent);
				}
			}
			else
			{
				BundleAction.Clear();
			}
		}
		break;
	case EDlFm_DialogEventType::Click_No:
		{
			int32& TmpIndex = Dialog_PasteTargetAlreadyExist_CurrentIndex;
			TmpIndex++;
			if (TmpIndex < BundleAction.FailedBuffer.Num())
			{
				if (IsValid(Dialog_PasteTargetAlreadyExist))
				{
					Dialog_PasteTargetAlreadyExist->SetInfo_FileAlreadyExist(BundleAction, TmpIndex);
				}
				else
				{
					OnInternalDialog_PasteTargetAlreadyExist(Type_When_DialogPasteTargetAlreadyExist_Absent);
				}
			}
			else
			{
				BundleAction.Clear();
			}
		}
		break;
	case EDlFm_DialogEventType::Click_YesForAll:
		{
			const int32& TmpIndex = Dialog_PasteTargetAlreadyExist_CurrentIndex;
			if (BundleAction.Type == FDlUI_FileExplorer_Lib::EBundleActionType::CopyPaste)
			{
				for (int32 LoopIndex = TmpIndex; LoopIndex < BundleAction.FailedBuffer.Num(); LoopIndex++)
				{
					FileManager.Copy(*BundleAction.FailedBuffer_Info[LoopIndex].Name,
					                 *BundleAction.FailedBuffer[LoopIndex].Name, true);
				}
			}
			else if (BundleAction.Type == FDlUI_FileExplorer_Lib::EBundleActionType::CutPaste)
			{
				for (int32 LoopIndex = TmpIndex; LoopIndex < BundleAction.FailedBuffer.Num(); LoopIndex++)
				{
					FileManager.Move(*BundleAction.FailedBuffer_Info[LoopIndex].Name,
					                 *BundleAction.FailedBuffer[LoopIndex].Name, true);
				}
			}
			BundleAction.Clear();
		}
		break;
	case EDlFm_DialogEventType::Click_SelectNewerModified:
		{
			int32& TmpIndex = Dialog_PasteTargetAlreadyExist_CurrentIndex;

			if (BundleAction.FailedBuffer_Info[TmpIndex].ModificationTime < BundleAction.FailedBuffer[TmpIndex].
				ModificationTime)
			{
				if (BundleAction.Type == FDlUI_FileExplorer_Lib::EBundleActionType::CopyPaste)
				{
					FileManager.Copy(*BundleAction.FailedBuffer_Info[TmpIndex].Name,
					                 *BundleAction.FailedBuffer[TmpIndex].Name, true);
				}
				else if (BundleAction.Type == FDlUI_FileExplorer_Lib::EBundleActionType::CutPaste)
				{
					FileManager.Move(*BundleAction.FailedBuffer_Info[TmpIndex].Name,
					                 *BundleAction.FailedBuffer[TmpIndex].Name, true);
				}
			}

			TmpIndex++;
			if (TmpIndex < BundleAction.FailedBuffer.Num())
			{
				if (IsValid(Dialog_PasteTargetAlreadyExist))
				{
					Dialog_PasteTargetAlreadyExist->SetInfo_FileAlreadyExist(BundleAction, TmpIndex);
				}
				else
				{
					OnInternalDialog_PasteTargetAlreadyExist(Type_When_DialogPasteTargetAlreadyExist_Absent);
				}
			}
			else
			{
				BundleAction.Clear();
			}
		}
		break;
	case EDlFm_DialogEventType::Click_SelectNewerModified_ForAll:
		{
			const int32& TmpIndex = Dialog_PasteTargetAlreadyExist_CurrentIndex;
			if (BundleAction.Type == FDlUI_FileExplorer_Lib::EBundleActionType::CopyPaste)
			{
				for (int32 LoopIndex = TmpIndex; LoopIndex < BundleAction.FailedBuffer.Num(); LoopIndex++)
				{
					if (BundleAction.FailedBuffer[LoopIndex].ModificationTime > BundleAction.FailedBuffer_Info[
						LoopIndex].ModificationTime)
					{
						FileManager.Copy(*BundleAction.FailedBuffer_Info[LoopIndex].Name,
						                 *BundleAction.FailedBuffer[LoopIndex].Name, true);
					}
				}
			}
			else if (BundleAction.Type == FDlUI_FileExplorer_Lib::EBundleActionType::CutPaste)
			{
				for (int32 LoopIndex = TmpIndex; LoopIndex < BundleAction.FailedBuffer.Num(); LoopIndex++)
				{
					if (BundleAction.FailedBuffer[LoopIndex].ModificationTime > BundleAction.FailedBuffer_Info[
						LoopIndex].ModificationTime)
					{
						FileManager.Move(*BundleAction.FailedBuffer_Info[LoopIndex].Name,
						                 *BundleAction.FailedBuffer[LoopIndex].Name, true);
					}
				}
			}
			
			BundleAction.Clear();
		}
		break;
	case EDlFm_DialogEventType::Click_SelectLarger:
		{
			int32& TmpIndex = Dialog_PasteTargetAlreadyExist_CurrentIndex;

			if (BundleAction.FailedBuffer_Info[TmpIndex].FileSize < BundleAction.FailedBuffer[TmpIndex].FileSize)
			{
				if (BundleAction.Type == FDlUI_FileExplorer_Lib::EBundleActionType::CopyPaste)
				{
					FileManager.Copy(*BundleAction.FailedBuffer_Info[TmpIndex].Name,
					                 *BundleAction.FailedBuffer[TmpIndex].Name, true);
				}
				else if (BundleAction.Type == FDlUI_FileExplorer_Lib::EBundleActionType::CutPaste)
				{
					FileManager.Move(*BundleAction.FailedBuffer_Info[TmpIndex].Name,
					                 *BundleAction.FailedBuffer[TmpIndex].Name, true);
				}
			}

			TmpIndex++;
			if (TmpIndex < BundleAction.FailedBuffer.Num())
			{
				if (IsValid(Dialog_PasteTargetAlreadyExist))
				{
					Dialog_PasteTargetAlreadyExist->SetInfo_FileAlreadyExist(BundleAction, TmpIndex);
				}
				else
				{
					OnInternalDialog_PasteTargetAlreadyExist(Type_When_DialogPasteTargetAlreadyExist_Absent);
				}
			}
			else
			{
				BundleAction.Clear();
			}
		}
		break;
	case EDlFm_DialogEventType::Click_SelectLarger_ForAll:
		{
			const int32& TmpIndex = Dialog_PasteTargetAlreadyExist_CurrentIndex;
			if (BundleAction.Type == FDlUI_FileExplorer_Lib::EBundleActionType::CopyPaste)
			{
				for (int32 LoopIndex = TmpIndex; LoopIndex < BundleAction.FailedBuffer.Num(); LoopIndex++)
				{
					if (BundleAction.FailedBuffer[LoopIndex].FileSize > BundleAction.FailedBuffer_Info[LoopIndex].
						FileSize)
					{
						FileManager.Copy(*BundleAction.FailedBuffer_Info[LoopIndex].Name,
						                 *BundleAction.FailedBuffer[LoopIndex].Name, true);
					}
				}
			}
			else if (BundleAction.Type == FDlUI_FileExplorer_Lib::EBundleActionType::CutPaste)
			{
				for (int32 LoopIndex = TmpIndex; LoopIndex < BundleAction.FailedBuffer.Num(); LoopIndex++)
				{
					if (BundleAction.FailedBuffer[LoopIndex].FileSize > BundleAction.FailedBuffer_Info[LoopIndex].
						FileSize)
					{
						FileManager.Move(*BundleAction.FailedBuffer_Info[LoopIndex].Name,
						                 *BundleAction.FailedBuffer[LoopIndex].Name, true);
					}
				}
			}
			BundleAction.Clear();
		}
		break;
	default:
		BundleAction.Clear();
		break;
	}
}

void UDlFmMain_Logic::OnInternalClicked_Refresh()
{
	Func_Refresh();
}

void UDlFmMain_Logic::OnInternalClicked_BrowseParentDir()
{
	DoWork_BrowseParentDir();
}

void UDlFmMain_Logic::OnInternalClicked_MakeDir()
{
	DoWork_StartMakeDir();
}

void UDlFmMain_Logic::OnInternalDialog_MakeDir(EDlFm_DialogEventType Type)
{
	switch (Type)
	{
	case EDlFm_DialogEventType::Click_Yes:
		DoWork_MakeDir(Dialog_MakeDir->PlannedNewName);
		break;
	default:
		break;
	}
}

void UDlFmMain_Logic::OnInternalClicked_CreateFile()
{
	DoWork_StartCreateFile();
}

void UDlFmMain_Logic::OnInternalDialog_CreateFile(EDlFm_DialogEventType Type)
{
	switch (Type)
	{
	case EDlFm_DialogEventType::Click_Yes:
		DoWork_CreateFile(Dialog_CreateFile->PlannedNewName);
		break;
	default:
		break;
	}
}

void UDlFmMain_Logic::OnInternalSelectionChanged_ExtensionFilter(FString SelectedItem,
                                                                          ESelectInfo::Type SelectionType)
{
	DoWork_ApplyExtensionFilter(SelectedItem);
	Func_Refresh();
}

void UDlFmMain_Logic::OnInternalClicked_SelectConfirm()
{
	DoWork_SelectConfirm();
}

void UDlFmMain_Logic::SetExploreMode(const FDlUI_FileExplorer_Lib::EExploreMode ExploreMode)
{
	Core.ExploreMode = ExploreMode;
	Internal_RefreshWidgetMain_FunctionButton();
}

void UDlFmMain_Logic::DoWork_ReadDir()
{
	Internal_Busy_Begin();

	Core.SetSelectMode(FDlUI_FileExplorer_Lib::ESelectMode::Single);
	if (IsValid(Text_FilePath))
	{
		if (!Text_FilePath->HasAnyUserFocus())
		{
			Text_FilePath->SetText(FText::FromString(CurDirPath_Stack.FlowPoint_CurrentPath()));
		}
	}
	if (IsValid(Text_SearchContent))
	{
		if (!Text_SearchContent->HasAnyUserFocus())
		{
			Text_SearchContent->SetText(FText::GetEmpty());
		}
	}
	
	if (!FileManager.DirectoryExists(*CurDirPath_Stack.FlowPoint_CurrentPath()))
	{
		DoWork_CacheError_ReadSearchDirNotExist();
		return;
	}
	else
	{
		for (FDlUI_FileExplorer_Lib::FSegment& OneSegment : Segment_List)
		{
			OneSegment.ClearInfo();
		}
		FileManager.IterateDirectoryStat(*CurDirPath_Stack.FlowPoint_CurrentPath(), Iterator_ReadDir);

		Core.ClearCachedSelect_NoScanSegment_NoRefreshFunctionButton();
		SetExploreMode(FDlUI_FileExplorer_Lib::EExploreMode::Browse);
		Internal_Refresh();
	}
	Internal_Busy_End();
}

void UDlFmMain_Logic::DoWork_SearchDir()
{
	Internal_Busy_Begin();

	if (IsValid(Text_FilePath))
	{
		if (!Text_FilePath->HasAnyUserFocus())
		{
			Text_FilePath->SetText(FText::FromString(CurDirPath_Stack.FlowPoint_CurrentPath()));
		}
	}
	if (IsValid(Text_SearchContent))
	{
		if (!Text_SearchContent->HasAnyUserFocus())
		{
			Text_SearchContent->SetText(FText::FromString(CurDirPath_Stack.FlowPoint_SearchContent()));
		}
	}

	if (!FileManager.DirectoryExists(*CurDirPath_Stack.FlowPoint_CurrentPath()))
	{
		DoWork_CacheError_ReadSearchDirNotExist();
		return;
	}
	else
	{
		
		TArray<FString> TmpResult;

		for (FDlUI_FileExplorer_Lib::FSegment& OneSegment : Segment_List)
		{
			OneSegment.ClearInfo();
		}

		FileManager.FindFilesRecursive(TmpResult,
									   *(CurDirPath_Stack.FlowPoint_CurrentPath()),
									   (bSearch_AutoAppendAsterisk
											? *CurDirPath_Stack.FlowPoint_SearchContent_AutoAppendAsterisk()
											: *CurDirPath_Stack.FlowPoint_SearchContent()),
									   true, true);
		for (FString& OneFindResult : TmpResult)
		{
			const FFileStatData TmpStateData = FileManager.GetStatData(*OneFindResult);
			const EDlUI_FileExplorer_ListSegment TmpSegment = FDlUI_FileExplorer_ItemInfo::CalcSegment(TmpStateData);
			Segment_List[static_cast<uint8>(TmpSegment)].DraftInfo_List.Emplace(
				FDlUI_FileExplorer_ItemInfo(OneFindResult, TmpStateData));
		}

		Core.ClearCachedSelect_NoScanSegment_NoRefreshFunctionButton();
		SetExploreMode(FDlUI_FileExplorer_Lib::EExploreMode::Search);
		Internal_Refresh();
	}
	Internal_Busy_End();
}

void UDlFmMain_Logic::DoWork_Delete()
{
	Internal_Busy_Begin();
	
	BundleAction.DoWork_IterateSelected(Iterator_Delete);
	if (BundleAction.FailedBuffer.Num() != 0)
	{
		Dialog_DeleteConfirm->SetInfo_DeleteFailed(BundleAction);
	}
	BundleAction.Clear();
	Func_Refresh();
	
	Internal_Busy_End();
}

bool UDlFmMain_Logic::Iterator_Delete(const FDlUI_FileExplorer_ItemInfo& TargetInfo,
                                                   FDlUI_FileExplorer_Lib::FBundleAction& BundleActionVal)
{
	IFileManager& TmpFileManager = IFileManager::Get();
	if (TargetInfo.bIsDirectory)
	{
		if (TmpFileManager.DeleteDirectory(*TargetInfo.Name, false, true) == false)
		{
			BundleActionVal.FailedBuffer.Emplace(TargetInfo);
		}
	}
	else
	{
		if (TmpFileManager.Delete(*TargetInfo.Name, false, true, true) == false)
		{
			BundleActionVal.FailedBuffer.Emplace(TargetInfo);
		}
	}
	return true;
}

void UDlFmMain_Logic::DoWork_StartMakeDir() const
{
	if (IsValid(Dialog_MakeDir))
	{
		Dialog_MakeDir->SetInfo_StartNewItem(NewDirDefaultName);
	}
}

void UDlFmMain_Logic::DoWork_MakeDir(const FString& DirName)
{
	FileManager.MakeDirectory(*(CurDirPath_Stack.FlowPoint_CurrentPath_WithSlash() + DirName), true);
	Func_Refresh();
}

void UDlFmMain_Logic::DoWork_StartCreateFile() const
{
	if (IsValid(Dialog_CreateFile))
	{
		Dialog_CreateFile->SetInfo_StartNewItem(NewFileDefaultName);
	}
}

void UDlFmMain_Logic::DoWork_CreateFile(const FString& FileName)
{
	FArchive* Tmp = FileManager.CreateFileWriter(*(CurDirPath_Stack.FlowPoint_CurrentPath_WithSlash() + FileName));
	Tmp->Close();
	Func_Refresh();
}

void UDlFmMain_Logic::DoWork_Rename()
{
	BundleAction.PrepareWork(FDlUI_FileExplorer_Lib::EBundleActionType::Rename);
	if (Core.SingleSelected_Valid)
	{
		Segment_List[static_cast<uint8>(Core.SingleSelected_Segment)].SetSelected_StartRename(
			Core.SingleSelected_Index);
	}
}

void UDlFmMain_Logic::DoWork_Cut()
{
	BundleAction.PrepareWork(FDlUI_FileExplorer_Lib::EBundleActionType::CutPaste);
}

void UDlFmMain_Logic::DoWork_Copy()
{
	BundleAction.PrepareWork(FDlUI_FileExplorer_Lib::EBundleActionType::CopyPaste);
}

void UDlFmMain_Logic::DoWork_Paste()
{
	if (CurDirPath_Stack.FlowPoint_CurrentExploreMode() == FDlUI_FileExplorer_Lib::EExploreMode::Browse)
	{
		const FString TmpPathWithSlash = CurDirPath_Stack.FlowPoint_CurrentPath_WithSlash();
		switch (BundleAction.Type)
		{
		case FDlUI_FileExplorer_Lib::EBundleActionType::CopyPaste:
			BundleAction.DoWork_IterateSelected_ParamString(Iterator_Paste, TmpPathWithSlash);
			if (BundleAction.FailedBuffer.Num() != 0)
			{
				if (IsValid(Dialog_PasteTargetAlreadyExist))
				{
					Dialog_PasteTargetAlreadyExist->SetInfo_FileAlreadyExist(BundleAction, 0);
				}
				else
				{
					OnInternalDialog_PasteTargetAlreadyExist(Type_When_DialogPasteTargetAlreadyExist_Absent);
					BundleAction.Clear();
				}
			}
			else
			{
				BundleAction.Clear();
			}
			Func_Refresh();
			break;
		case FDlUI_FileExplorer_Lib::EBundleActionType::CutPaste:
			BundleAction.DoWork_IterateSelected_ParamString(Iterator_Paste, TmpPathWithSlash);
			if (BundleAction.FailedBuffer.Num() != 0)
			{
				if (IsValid(Dialog_PasteTargetAlreadyExist))
				{
					Dialog_PasteTargetAlreadyExist->SetInfo_FileAlreadyExist(BundleAction, 0);
				}
				else
				{
					OnInternalDialog_PasteTargetAlreadyExist(Type_When_DialogPasteTargetAlreadyExist_Absent);
					BundleAction.Clear();
				}
			}
			else
			{
				if (bTryCleanEmptyDirAfterCutPaste)
				{
					for (FDlUI_FileExplorer_ItemInfo& OneItem : BundleAction.SelectedBuffer)
					{
						if (OneItem.bIsDirectory)
						{
							if (FileManager.DirectoryExists(*OneItem.Name))
							{
								FileManager.DeleteDirectory(*OneItem.Name, false, true);
							}
						}
					}
				}
				BundleAction.Clear();
			}
			Func_Refresh();
			break;
		default:
			break;
		}
	}
}

bool UDlFmMain_Logic::Iterator_Paste(const FDlUI_FileExplorer_ItemInfo& TargetInfo,
                                                  FDlUI_FileExplorer_Lib::FBundleAction& BundleActionV,
                                                  const FString& Param1)
{
	IFileManager& TmpFileManager = IFileManager::Get();
	const FString TmpDes = Param1 + TargetInfo.CleanName;

	if (!TmpDes.Equals(TargetInfo.Name))
	{
		if (TargetInfo.bIsDirectory)
		{
			DoWork_RecursivePasteDir(TmpDes, TargetInfo.Name, false, &BundleActionV);
			return true;
		}
		else
		{
			if (!FPaths::FileExists(TmpDes))
			{
				if (BundleActionV.Type == FDlUI_FileExplorer_Lib::EBundleActionType::CopyPaste)
				{
					TmpFileManager.Copy(*TmpDes, *TargetInfo.Name, false);
				}
				else if (BundleActionV.Type == FDlUI_FileExplorer_Lib::EBundleActionType::CutPaste)
				{
					TmpFileManager.Move(*TmpDes, *TargetInfo.Name, false);
				}
			}
			else
			{
				BundleActionV.FailedBuffer.Emplace(TargetInfo);
				BundleActionV.FailedBuffer_Info.Emplace(FDlUI_FileExplorer_ItemInfo(TmpDes));
				return true;
			}
		}
	}
	return true;
}

void UDlFmMain_Logic::DoWork_BrowseParentDir()
{
	FString TmpPath = FPaths::GetPath((CurDirPath_Stack.FlowPoint_CurrentPath()));
	if (TmpPath.EndsWith(":"))
	{
		TmpPath.AppendChar('/');
	}
	Func_BrowseToPath(TmpPath);
}

void UDlFmMain_Logic::DoWork_CacheError_ReadSearchDirNotExist()
{
	if (IsValid(Text_InternalInfo))
	{
		Text_InternalInfo->SetText(FText::FromString(TEXT("The Directory does not exist")));
	}
	for (FDlUI_FileExplorer_Lib::FSegment& OneSegment : Segment_List)
	{
		OneSegment.SetAndShowEmpty();
	}
}

void UDlFmMain_Logic::DoWork_ApplyExtensionFilter(const FString& SelectedItem)
{
	SelectedItem.ToLower().ParseIntoArray(Core.FilteredExtension_List,TEXT(","), true);
	if (Core.FilteredExtension_List.Num() != 0)
	{
		Core.bShowFilter_Extension = true;
	}
	else
	{
		Core.bShowFilter_Extension = false;
	}
}

void UDlFmMain_Logic::DoWork_RecursivePasteDir(const FString& DesDir, const FString& SrcDir,
                                                        const bool OverwriteFile,
                                                        FDlUI_FileExplorer_Lib::FBundleAction* BundleActionV)
{
	if (DesDir.Equals(SrcDir)) { return; }
	IFileManager& TmpFileManager = IFileManager::Get();
	if (!TmpFileManager.DirectoryExists(*DesDir))
	{
		if (BundleActionV->Type == FDlUI_FileExplorer_Lib::EBundleActionType::CopyPaste)
		{
			TmpFileManager.MakeDirectory(*DesDir, true);
		}
		else if (BundleActionV->Type == FDlUI_FileExplorer_Lib::EBundleActionType::CutPaste)
		{
			TmpFileManager.Move(*DesDir, *SrcDir);
			return;
		}
	}

	FString DesPathWithSlash = DesDir;
	FPaths::NormalizeDirectoryName(DesPathWithSlash);
	DesPathWithSlash = DesPathWithSlash + TEXT("/");

	FString SrcPathWithSlash = SrcDir;
	FPaths::NormalizeDirectoryName(SrcPathWithSlash);
	SrcPathWithSlash = SrcPathWithSlash + TEXT("/");

	FDlUI_FileExplorer_Lib::FIterator_DirPaste TmpIterator_Paste;
	TmpIterator_Paste.Init_ForCopy(&DesPathWithSlash, &SrcPathWithSlash, BundleActionV, OverwriteFile);

	TmpFileManager.IterateDirectoryStatRecursively(*SrcDir, TmpIterator_Paste);
}

void UDlFmMain_Logic::DoWork_SelectConfirm()
{
	TArray<FDlUI_FileExplorer_ItemInfo> TmpRlt;
	Buffer_OnFileListSelectConfirmed.SetNum(0, EAllowShrinking::No);
	for (FDlUI_FileExplorer_Lib::FSegment& OneSegment : Segment_List)
	{
		for (FDlUI_FileExplorer_ItemInfo& OneInfo : OneSegment.DraftInfo_List)
		{
			if (OneInfo.bSelected)
			{
				Buffer_OnFileListSelectConfirmed.Emplace(OneInfo);
			}
		}
	}
	if ((bSelectCurrentPathWhenNothingSelect) && (Buffer_OnFileListSelectConfirmed.Num() == 0))
	{
		Buffer_OnFileListSelectConfirmed.Emplace(FDlUI_FileExplorer_ItemInfo(CurDirPath_Stack.FlowPoint_CurrentPath()));
	}
	OnFileListSelectConfirmed.Broadcast(Buffer_OnFileListSelectConfirmed);
}

void UDlFmMain_Logic::DoWork_Sort()
{
	for (uint8 TmpIndex = 0; TmpIndex < Segment_List.Num(); TmpIndex++)
	{
		Segment_List[TmpIndex].RefreshSort(static_cast<EDlUI_FileExplorer_ListSegment>(TmpIndex), Core.CurSortType,
										   Core.bReverseSort);
	}
}
