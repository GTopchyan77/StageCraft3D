// Copyright(c) 2022 Dragon Li Software Limited. All Rights Reserved.
#include "DlFmLib.h"

#include "Blueprint/WidgetTree.h"
#include "DlFm_Item_Data.h"
#include "DlFmMain_Logic.h"
#include "Components/ExpandableArea.h"
#include "Components/ListView.h"


FString FDlUI_FileExplorer_Lib::FPathStack::FilterPath(const FString& Src) const
{
	const FString StandardVal = FPaths::CreateStandardFilename(Src);
	FString TmpRlt = bAlwaysUseFullPath ? FPaths::ConvertRelativePathToFull(StandardVal) : StandardVal;
	FPaths::NormalizeDirectoryName(TmpRlt);
	return TmpRlt;
}

void FDlUI_FileExplorer_Lib::FPathStack::Init(UDlFmMain_Logic* ParentV, bool bAlwaysUseFullPathVal)
{
	Parent = ParentV;
	bAlwaysUseFullPath = bAlwaysUseFullPathVal;
}

void FDlUI_FileExplorer_Lib::FPathStack::InitStack(const FString& InitVal, const int32 Depth)
{
	const FString InitPath = FilterPath(InitVal);
	Path_List.Init(InitPath, FMath::Max(3, Depth));
	ExploreMode_List.Init(EExploreMode::Browse, Path_List.Num());
	SearchContent_List.Init(FString(), Path_List.Num());
	StackTop = 0;
	StackBottom = 0;
	FlowPointer = 0;
	Parent->Internal_RefreshWidgetMain_NaviButton();
}

void FDlUI_FileExplorer_Lib::FPathStack::Push_AtFlowPointer(const FString& Val)
{
	const FString UsingPath = FilterPath(Val);

	if ((Path_List[FlowPointer] == UsingPath) && (ExploreMode_List[FlowPointer] == EExploreMode::Browse))
	{
		return;
	}

	const int32 NewTop = (FlowPointer + 1) % Path_List.Num();
	if (StackBottom == NewTop)
	{
		StackBottom = (StackBottom + 1) % Path_List.Num();
	}
	StackTop = NewTop;
	FlowPointer = StackTop;

	Path_List[NewTop] = UsingPath;
	ExploreMode_List[NewTop] = EExploreMode::Browse;
	SearchContent_List[NewTop] = FString();
	Parent->Internal_RefreshWidgetMain_NaviButton();
}

void FDlUI_FileExplorer_Lib::FPathStack::Push_AtFlowPointer(const FString& Val, const FString& SearchContent,
                                                            const EExploreMode ExploreMode)
{
	const FString UsingPath = FilterPath(Val);

	if ((Path_List[FlowPointer] == UsingPath) && (ExploreMode_List[FlowPointer] == ExploreMode))
	{
		if (ExploreMode == EExploreMode::Browse)
		{
			return;
		}
		else if (ExploreMode == EExploreMode::Search)
		{
			if (SearchContent_List[FlowPointer] == SearchContent)
			{
				return;
			}
		}
	}

	const int32 NewTop = (FlowPointer + 1) % Path_List.Num();
	if (StackBottom == NewTop)
	{
		StackBottom = (StackBottom + 1) % Path_List.Num();
	}
	StackTop = NewTop;
	FlowPointer = StackTop;

	Path_List[NewTop] = UsingPath;
	ExploreMode_List[NewTop] = ExploreMode;
	SearchContent_List[NewTop] = (ExploreMode_List[NewTop] == EExploreMode::Search) ? SearchContent : FString();
	Parent->Internal_RefreshWidgetMain_NaviButton();
}

bool FDlUI_FileExplorer_Lib::FPathStack::IsFlowPointer_AtTop() const
{
	return FlowPointer == StackTop;
}

bool FDlUI_FileExplorer_Lib::FPathStack::IsFlowPointer_AtBottom() const
{
	return FlowPointer == StackBottom;
}

void FDlUI_FileExplorer_Lib::FPathStack::FlowPoint_GoBack()
{
	if (FlowPointer != StackBottom)
	{
		const int32 TmpVal = FlowPointer - 1;
		if (TmpVal < 0)
		{
			FlowPointer = TmpVal + Path_List.Num();
		}
		else
		{
			FlowPointer = TmpVal;
		}
	}
	Parent->Internal_RefreshWidgetMain_NaviButton();
}

void FDlUI_FileExplorer_Lib::FPathStack::FlowPoint_GoForward()
{
	if (FlowPointer != StackTop)
	{
		FlowPointer = (FlowPointer + 1) % Path_List.Num();
	}
	Parent->Internal_RefreshWidgetMain_NaviButton();
}

FString& FDlUI_FileExplorer_Lib::FPathStack::FlowPoint_CurrentPath()
{
	return Path_List[FlowPointer];
}

FString FDlUI_FileExplorer_Lib::FPathStack::FlowPoint_CurrentPath_WithSlash() const
{
	return Path_List[FlowPointer] + TEXT("/");
}

FDlUI_FileExplorer_Lib::EExploreMode FDlUI_FileExplorer_Lib::FPathStack::FlowPoint_CurrentExploreMode()
{
	return ExploreMode_List[FlowPointer];
}

FString& FDlUI_FileExplorer_Lib::FPathStack::FlowPoint_SearchContent()
{
	return SearchContent_List[FlowPointer];
}

FString FDlUI_FileExplorer_Lib::FPathStack::FlowPoint_SearchContent_AutoAppendAsterisk()
{
	FString TmpResult = SearchContent_List[FlowPointer];
	if (!TmpResult.StartsWith("*"))
	{
		TmpResult = TEXT("*") + TmpResult;
	}
	if (!TmpResult.EndsWith("*"))
	{
		TmpResult = TmpResult + TEXT("*");
	}
	return TmpResult;
}

void FDlUI_FileExplorer_Lib::FIterator_DirItemReader::Init_ForRead(TArray<FSegment>* Segment_ListPtrVal)
{
	Segment_ListPtr = Segment_ListPtrVal;
	for (FSegment& OneSegment : *Segment_ListPtr)
	{
		OneSegment.ClearInfo();
	}
}

bool FDlUI_FileExplorer_Lib::FIterator_DirItemReader::Visit(const TCHAR* FilenameOrDirectory,
                                                            const FFileStatData& StatData)
{
	if ((Segment_ListPtr != nullptr))
	{
		EDlUI_FileExplorer_ListSegment TmpSegment = FDlUI_FileExplorer_ItemInfo::CalcSegment(StatData);
		(*Segment_ListPtr)[static_cast<uint8>(TmpSegment)].DraftInfo_List.Emplace(
			FDlUI_FileExplorer_ItemInfo(FString(FilenameOrDirectory), StatData));
		return true;
	}
	else
	{
		return false;
	}
}

void FDlUI_FileExplorer_Lib::FIterator_DirPaste::Init_ForCopy(FString* DesPath_WithSlashV,
                                                              FString* SrcPath_WithSlashV, FBundleAction* BundleActionV,
                                                              bool bOverwriteFileV)
{
	bOverwriteFile = bOverwriteFileV;
	DesPath_WithSlash = DesPath_WithSlashV;
	SrcPath_WithSlash = SrcPath_WithSlashV;
	BundleAction = BundleActionV;
}

bool FDlUI_FileExplorer_Lib::FIterator_DirPaste::Visit(const TCHAR* FilenameOrDirectory,
                                                       const FFileStatData& StatData)
{
	FString TmpRelativeName = FString(FilenameOrDirectory);
	FPaths::MakePathRelativeTo(TmpRelativeName, **SrcPath_WithSlash);
	const FString TmpDesName = *DesPath_WithSlash + TmpRelativeName;

	if (StatData.bIsDirectory)
	{
		if (!FileManager.DirectoryExists(*TmpDesName))
		{
			if (BundleAction->Type == EBundleActionType::CopyPaste)
			{
				FileManager.MakeDirectory(*TmpDesName, true);
			}
			else
			{
				FileManager.Move(*TmpDesName, FilenameOrDirectory, true);
			}
		}
	}
	else
	{
		if (bOverwriteFile)
		{
			if (BundleAction->Type == EBundleActionType::CopyPaste)
			{
				FileManager.Copy(*TmpDesName, FilenameOrDirectory, true);
			}
			else if (BundleAction->Type == EBundleActionType::CutPaste)
			{
				FileManager.Move(*TmpDesName, FilenameOrDirectory, true);
			}
		}
		else
		{
			if (FileManager.FileExists(*TmpDesName))
			{
				BundleAction->FailedBuffer.Emplace(FDlUI_FileExplorer_ItemInfo(FilenameOrDirectory, StatData));
				BundleAction->FailedBuffer_Info.Emplace(FDlUI_FileExplorer_ItemInfo(TmpDesName));
			}
			else
			{
				if (BundleAction->Type == EBundleActionType::CopyPaste)
				{
					FileManager.Copy(*TmpDesName, FilenameOrDirectory, false);
				}
				else if (BundleAction->Type == EBundleActionType::CutPaste)
				{
					FileManager.Move(*TmpDesName, FilenameOrDirectory, false);
				}
			}
		}
	}
	return true;
}

void FDlUI_FileExplorer_Lib::FSegment::SetSelected(const int32 InfoIndex, const bool Val)
{
	if (InfoIndex < DraftInfo_List.Num())
	{
		DraftInfo_List[InfoIndex].bSelected = Val;
		const int32 TmpWidgetIndex = DraftSort_List.Find(InfoIndex);
		if (TmpWidgetIndex != INDEX_NONE)
		{
			Cache_Container->RegenerateAllEntries();
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("This should never happen!!!!!!"));
		}
	}
}

bool FDlUI_FileExplorer_Lib::FSegment::SetSelected_ReverseCur(int32 InfoIndex)
{
	if (InfoIndex < DraftInfo_List.Num())
	{
		DraftInfo_List[InfoIndex].bSelected = !DraftInfo_List[InfoIndex].bSelected;
		const int32 TmpWidgetIndex = DraftSort_List.Find(InfoIndex);
		if (TmpWidgetIndex != INDEX_NONE)
		{
			Cache_Container->RegenerateAllEntries();
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("This should never happen!!!!!!"));
		}
		return DraftInfo_List[InfoIndex].bSelected;
	}
	return false;
}

void FDlUI_FileExplorer_Lib::FSegment::SetSelected_StartRename(int32 InfoIndex)
{
	if (InfoIndex < DraftInfo_List.Num())
	{
		const int32 TmpWidgetIndex = DraftSort_List.Find(InfoIndex);
		if (TmpWidgetIndex != INDEX_NONE)
		{
			(*ItemPool_List)[TmpWidgetIndex]->Item->SpecialAction=ESpecialAction::Rename;
			Cache_Container->RegenerateAllEntries();
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("This should never happen!!!!!!"));
		}
	}
}

void FDlUI_FileExplorer_Lib::FSegment::SetSelected_AllVisible(const bool Val)
{
	for (const UDlFm_Item_Data* OneVisibleItem :VisibleItem_List)
	{
		OneVisibleItem->Item->bSelected=Val;
	}
	Cache_Container->RegenerateAllEntries();
}


void FDlUI_FileExplorer_Lib::FSegment::InitClear(UListView* ContainerV, TArray<UDlFm_Item_Data*>* ItemPool_ListV,
                                                 UDlFmMain_Logic* ParentVal)
{
	Cache_Container = ContainerV;
	Parent = ParentVal;
	
	ItemPool_List=ItemPool_ListV;
	
	Cache_Container->ClearListItems();
	ItemPool_List->Empty(false);
	DraftInfo_List.Empty(false);
	DraftSort_List.Empty(false);
	VisibleItem_List.Empty(false);
}

void FDlUI_FileExplorer_Lib::FSegment::ClearInfo()
{
	DraftInfo_List.SetNum(0, EAllowShrinking::No);
}

void FDlUI_FileExplorer_Lib::FSegment::SetAndShowEmpty()
{
	DraftInfo_List.SetNum(0, EAllowShrinking::No);
	DraftSort_List.SetNum(0, EAllowShrinking::No);
	
	Cache_Container->ClearListItems();
	VisibleItem_List.SetNum(0,EAllowShrinking::No);
}

void FDlUI_FileExplorer_Lib::FSegment::RefreshAllWidget_ModeStyle()
{
	Cache_Container->RegenerateAllEntries();
}

void FDlUI_FileExplorer_Lib::FSegment::RefreshSort(const EDlUI_FileExplorer_ListSegment Segment,
                                                   const ESortType SortType, const bool bReverse)
{
	switch (SortRemapConfig(Segment, SortType))
	{
	case ESortType::Name:
		SortBy_Name(bReverse);
		break;
	case ESortType::Size:
		SortBy_Size(bReverse);
		break;
	case ESortType::AccessTime:
		SortBy_AccessTime(bReverse);
		break;
	case ESortType::CreationTime:
		SortBy_CreationTime(bReverse);
		break;
	case ESortType::FileType:
		SortBy_FileType(bReverse);
		break;
	case ESortType::ModifyTime:
		SortBy_ModifyTime(bReverse);
		break;
	default: break;
	}
}

FDlUI_FileExplorer_Lib::ESortType FDlUI_FileExplorer_Lib::FSegment::SortRemapConfig(
	const EDlUI_FileExplorer_ListSegment Segment, const ESortType SortType)
{
	if (Segment == EDlUI_FileExplorer_ListSegment::Directory)
	{
		switch (SortType)
		{
		case ESortType::Size:
			return ESortType::Name;
		case ESortType::FileType:
			return ESortType::Name;
		default: break;
		}
	}
	return SortType;
}

void FDlUI_FileExplorer_Lib::FSegment::SetItemPool_MinSize(const int32 NewNum) const
{
	if (IsValid(Cache_Container))
	{
		if (NewNum > ItemPool_List->Num())
		{
			for (int32 TmpIndex = ItemPool_List->Num(); TmpIndex < NewNum; TmpIndex++)
			{
				UDlFm_Item_Data* TmpNewItem = NewObject<UDlFm_Item_Data>(Parent);
				if (TmpNewItem != nullptr)
				{
					ItemPool_List->Emplace(TmpNewItem);
				}
			}
		}
	}
}

void FDlUI_FileExplorer_Lib::FSegment::SortBy_Name(const bool bReverse)
{
	Internal_SortPrepare();

	TArray<FDlUI_FileExplorer_ItemInfo>* TmpItemNodeList = &DraftInfo_List;
	Algo::SortBy(DraftSort_List, [TmpItemNodeList](int32 Index) -> FString
	{
		return (*TmpItemNodeList)[Index].CleanName;
	});

	Internal_SortApplyReverse(bReverse);
	Internal_SetUIAfterSort();
}

void FDlUI_FileExplorer_Lib::FSegment::SortBy_Size(const bool bReverse)
{
	Internal_SortPrepare();

	TArray<FDlUI_FileExplorer_ItemInfo>* TmpItemNodeList = &DraftInfo_List;
	Algo::SortBy(DraftSort_List, [TmpItemNodeList](int32 Index) -> int64
	{
		return (*TmpItemNodeList)[Index].FileSize;
	});

	Internal_SortApplyReverse(bReverse);
	Internal_SetUIAfterSort();
}

void FDlUI_FileExplorer_Lib::FSegment::SortBy_CreationTime(const bool bReverse)
{
	Internal_SortPrepare();

	TArray<FDlUI_FileExplorer_ItemInfo>* TmpItemNodeList = &DraftInfo_List;
	Algo::SortBy(DraftSort_List, [TmpItemNodeList](int32 Index) -> FDateTime
	{
		return (*TmpItemNodeList)[Index].CreationTime;
	});

	Internal_SortApplyReverse(bReverse);
	Internal_SetUIAfterSort();
}

void FDlUI_FileExplorer_Lib::FSegment::SortBy_AccessTime(const bool bReverse)
{
	Internal_SortPrepare();

	TArray<FDlUI_FileExplorer_ItemInfo>* TmpItemNodeList = &DraftInfo_List;
	Algo::SortBy(DraftSort_List, [TmpItemNodeList](int32 Index) -> FDateTime
	{
		return (*TmpItemNodeList)[Index].AccessTime;
	});

	Internal_SortApplyReverse(bReverse);
	Internal_SetUIAfterSort();
}

void FDlUI_FileExplorer_Lib::FSegment::SortBy_ModifyTime(const bool bReverse)
{
	Internal_SortPrepare();

	TArray<FDlUI_FileExplorer_ItemInfo>* TmpItemNodeList = &DraftInfo_List;
	Algo::SortBy(DraftSort_List, [TmpItemNodeList](int32 Index) -> FDateTime
	{
		return (*TmpItemNodeList)[Index].ModificationTime;
	});

	Internal_SortApplyReverse(bReverse);
	Internal_SetUIAfterSort();
}

void FDlUI_FileExplorer_Lib::FSegment::SortBy_FileType(const bool bReverse)
{
	Internal_SortPrepare();

	TArray<FDlUI_FileExplorer_ItemInfo>* TmpItemNodeList = &DraftInfo_List;
	Algo::SortBy(DraftSort_List, [TmpItemNodeList](int32 Index) -> FString
	{
		return (*TmpItemNodeList)[Index].Extension;
	});

	Internal_SortApplyReverse(bReverse);
	Internal_SetUIAfterSort();
}

void FDlUI_FileExplorer_Lib::FSegment::Internal_SortPrepare()
{
	DraftSort_List.SetNum(DraftInfo_List.Num(), EAllowShrinking::No);
	for (int32 TmpIndex = 0; TmpIndex < DraftSort_List.Num(); TmpIndex++)
	{
		DraftSort_List[TmpIndex] = TmpIndex;
	}
}

void FDlUI_FileExplorer_Lib::FSegment::Internal_SortApplyReverse(const bool bReverse)
{
	if (bReverse)
	{
		Algo::Reverse(DraftSort_List);
	}
}

void FDlUI_FileExplorer_Lib::FSegment::Internal_SetUIAfterSort()
{
	if (IsValid(Cache_Container))
	{
		SetItemPool_MinSize(DraftInfo_List.Num());
		
		VisibleItem_List.SetNumUninitialized(0,EAllowShrinking::No);
		for (int32 TmpIndex = 0; TmpIndex < DraftSort_List.Num(); TmpIndex++)
		{
			const int32 TmpInfoIndex = DraftSort_List[TmpIndex];
			if(DraftInfo_List[TmpInfoIndex].bShouldShow)
			{
				VisibleItem_List.Emplace((*ItemPool_List)[VisibleItem_List.Num()]);
				VisibleItem_List[VisibleItem_List.Num()-1]->Init(Parent,&DraftInfo_List[TmpInfoIndex],TmpInfoIndex);
			}
		}
		Cache_Container->SetListItems(VisibleItem_List);
		Cache_Container->RegenerateAllEntries();
	}
}

void FDlUI_FileExplorer_Lib::FCore::Init(UDlFmMain_Logic* ParentVal, TArray<FSegment>* Segment_ListPtrVal,
                                         UDlFmFilteredToggleButton* Toggle_MultiSelectV,
                                         UDlFmFilteredToggleButton* Toggle_MultiSelect_SelectAllV,
                                         UWidget* Container_FunctionButtonV, FBundleAction* BundleActionV)
{
	Parent = ParentVal;
	Segment_ListPtr = Segment_ListPtrVal;
	Toggle_MultiSelect = Toggle_MultiSelectV;
	Toggle_MultiSelect_SelectAll = Toggle_MultiSelect_SelectAllV;
	Container_FunctionButton = Container_FunctionButtonV;
	BundleAction = BundleActionV;
}

void FDlUI_FileExplorer_Lib::FCore::NewClick(const int32 IndexV, const EDlUI_FileExplorer_ListSegment SegmentV)
{
	switch (SelectMode)
	{
	case ESelectMode::Single:
		if (((IndexV == SingleSelected_Index) && (SegmentV == SingleSelected_Segment) && (SingleSelected_Valid)))
		{
			//Double Click
			Single_Confirm(IndexV, SegmentV);
		}
		else
		{
			if (Parent->bUseSingleClickForConfirm)
			{
				Single_SelectChange(IndexV, SegmentV);
				Single_Confirm(IndexV, SegmentV);
			}
			else
			{
				Single_SelectChange(IndexV, SegmentV);
			}
		}
		break;
	case ESelectMode::Multiply:

		Multi_ReverseSelect(IndexV, SegmentV);

		break;
	default:
		break;
	}
}

void FDlUI_FileExplorer_Lib::FCore::SetSelect_All(const bool Val)
{
	for (FSegment& OneSegment : *Segment_ListPtr)
	{
		OneSegment.SetSelected_AllVisible(Val);
	}
}

void FDlUI_FileExplorer_Lib::FCore::SetSelectMode(const ESelectMode Val, const bool bCleanSelectInfo)
{
	//FSegment will look SelectMode to modify UMG, so set SelectMode before that
	SelectMode = Val;
	if (bCleanSelectInfo)
	{
		ClearCachedSelect();
	}
	Parent->Internal_RefreshWidgetMain_SelectModeRelated();
}

void FDlUI_FileExplorer_Lib::FCore::ClearCachedSelect_NoScanSegment_NoRefreshFunctionButton()
{
	SelectedItem_Num = 0;
	SingleSelected_Valid = false;
}

void FDlUI_FileExplorer_Lib::FCore::ClearCachedSelect()
{
	SelectedItem_Num = 0;
	SingleSelected_Valid = false;

	for (FSegment& OneSegment : *Segment_ListPtr)
	{
		OneSegment.SetSelected_AllVisible(false);
	}
	Parent->Internal_RefreshWidgetMain_FunctionButton();
}

void FDlUI_FileExplorer_Lib::FCore::Refresh_ShouldShow()
{
	if (Segment_ListPtr != nullptr)
	{
		for (FSegment& OneSegment : *Segment_ListPtr)
		{
			for (FDlUI_FileExplorer_ItemInfo& OneInfo : OneSegment.DraftInfo_List)
			{
				OneInfo.bShouldShow = true;
			}
		}

		if (bShowFilter_Extension)
		{
			for (FSegment& OneSegment : *Segment_ListPtr)
			{
				for (FDlUI_FileExplorer_ItemInfo& OneInfo : OneSegment.DraftInfo_List)
				{
					if (OneInfo.bIsDirectory == false)
					{
						OneInfo.bShouldShow &= FilteredExtension_List.Contains(OneInfo.Extension);
					}
				}
			}
		}
	}
}

void FDlUI_FileExplorer_Lib::FCore::Single_SelectChange(const int32 IndexVal, EDlUI_FileExplorer_ListSegment SegmentVal)
{
	if (SingleSelected_Valid)
	{
		(*Segment_ListPtr)[static_cast<uint8>(SingleSelected_Segment)].SetSelected(
			SingleSelected_Index, false);
		SelectedItem_Num--;

		//in single-select mode, if one item is not selected anymore, it should lost focus, and hide Container_FunctionButton
		if (IsValid(Container_FunctionButton))
		{
			if ((SelectMode == ESelectMode::Single) && (BundleAction->Type != EBundleActionType::CutPaste) && (
				BundleAction->Type != EBundleActionType::CopyPaste))
			{
				Container_FunctionButton->SetVisibility(ESlateVisibility::Collapsed);
			}
		}
	}

	(*Segment_ListPtr)[static_cast<uint8>(SegmentVal)].SetSelected(IndexVal, true);
	SelectedItem_Num++;

	SingleSelected_Index = IndexVal;
	SingleSelected_Segment = SegmentVal;
	SingleSelected_Valid = true;

	Parent->Internal_RefreshWidgetMain_FunctionButton();
}

void FDlUI_FileExplorer_Lib::FCore::Single_Confirm(const int32 IndexVal, EDlUI_FileExplorer_ListSegment SegmentVal)
{
	switch (SegmentVal)
	{
	case EDlUI_FileExplorer_ListSegment::Directory:
		Parent->Func_BrowseToPath(
			(*Segment_ListPtr)[static_cast<uint8>(EDlUI_FileExplorer_ListSegment::Directory)].DraftInfo_List
			[IndexVal].Name);
		break;
	case EDlUI_FileExplorer_ListSegment::File:
		{
			const FDlUI_FileExplorer_ItemInfo& TmpInfo = (*Segment_ListPtr)[static_cast<uint8>(
				EDlUI_FileExplorer_ListSegment::File)].DraftInfo_List[IndexVal];
			Parent->Buffer_OnFileListSelectConfirmed = {TmpInfo};
			Parent->OnFileListSelectConfirmed.Broadcast(Parent->Buffer_OnFileListSelectConfirmed);

			//Double confirm on file does not change select status
			(*Segment_ListPtr)[static_cast<uint8>(SingleSelected_Segment)].SetSelected(
				SingleSelected_Index, true);
		}
		break;
	default:
		break;
	}
	Parent->Internal_RefreshWidgetMain_FunctionButton();
}

void FDlUI_FileExplorer_Lib::FCore::Multi_ReverseSelect(const int32 IndexVal, EDlUI_FileExplorer_ListSegment SegmentVal)
{
	const bool TmpB = (*Segment_ListPtr)[static_cast<uint8>(SegmentVal)].SetSelected_ReverseCur(IndexVal);
	SelectedItem_Num = (TmpB) ? (SelectedItem_Num + 1) : (SelectedItem_Num - 1);

	if (TmpB)
	{
		SingleSelected_Index = IndexVal;
		SingleSelected_Segment = SegmentVal;
		SingleSelected_Valid = true;
	}
	else
	{
		SingleSelected_Valid = false;
	}
	Parent->Internal_RefreshWidgetMain_FunctionButton();
}

void FDlUI_FileExplorer_Lib::FBundleAction::PrepareWork(const EBundleActionType TypeVal, const bool bCollectFromSegment)
{
	SelectedBuffer.SetNum(0, EAllowShrinking::No);
	FailedBuffer.SetNum(0, EAllowShrinking::No);
	FailedBuffer_Info.SetNum(0, EAllowShrinking::No);

	Type = TypeVal;
	if (bCollectFromSegment)
	{
		for (const FSegment& OneSegment : *Segment_ListPtr)
		{
			for (const FDlUI_FileExplorer_ItemInfo& OneInfo : OneSegment.DraftInfo_List)
			{
				if (OneInfo.bSelected)
				{
					SelectedBuffer.Emplace(OneInfo);
				}
			}
		}
	}
	Parent->Internal_RefreshWidgetMain_FunctionButton();
}

void FDlUI_FileExplorer_Lib::FBundleAction::DoWork_IterateSelected(
	TFunctionRef<bool(const FDlUI_FileExplorer_ItemInfo&, FBundleAction&)> FuncParam)
{
	for (auto& OneInfo : SelectedBuffer)
	{
		if (FuncParam(OneInfo, *this) != true)
		{
			break;
		}
	}
}

void FDlUI_FileExplorer_Lib::FBundleAction::DoWork_IterateSelected_ParamString(
	TFunctionRef<bool(const FDlUI_FileExplorer_ItemInfo&, FBundleAction&, const FString&)> FuncParam,
	const FString& Param1)
{
	for (auto& OneInfo : SelectedBuffer)
	{
		if (FuncParam(OneInfo, *this, Param1) != true)
		{
			break;
		}
	}
}

void FDlUI_FileExplorer_Lib::FBundleAction::Clear()
{
	Type = EBundleActionType::None;
	SelectedBuffer.SetNum(0, EAllowShrinking::No);
	FailedBuffer.SetNum(0, EAllowShrinking::No);
	FailedBuffer_Info.SetNum(0, EAllowShrinking::No);
	Parent->Internal_RefreshWidgetMain_FunctionButton();
}

void FDlUI_FileExplorer_Lib::FBundleAction::Init(UDlFmMain_Logic* ParentVal,
                                                 TArray<FSegment>* Segment_ListPtrVal)
{
	Parent = ParentVal;
	Segment_ListPtr = Segment_ListPtrVal;
}

FDlUI_FileExplorer_ItemInfo::FDlUI_FileExplorer_ItemInfo(const FString& NameVal, const FFileStatData& StatDataVal,
                                                         const bool bSelectedV)
{
	Name = NameVal;
	//StatData = StatDataVal;

	CreationTime = StatDataVal.CreationTime;
	AccessTime = StatDataVal.AccessTime;
	ModificationTime = StatDataVal.ModificationTime;
	FileSize = StatDataVal.FileSize;
	bIsDirectory = StatDataVal.bIsDirectory;
	bIsReadOnly = StatDataVal.bIsReadOnly;
	bIsValid = StatDataVal.bIsValid;


	bSelected = bSelectedV;
	Extension = FPaths::GetExtension(NameVal).ToLower();
	ListSegment = CalcSegment(*this);
	CleanName = FPaths::GetCleanFilename(Name);
}

FDlUI_FileExplorer_ItemInfo::FDlUI_FileExplorer_ItemInfo(const FString& NameVal, const bool bSelectedV)
{
	Name = NameVal;
	const FFileStatData StatDataVal = IFileManager::Get().GetStatData(*Name);

	CreationTime = StatDataVal.CreationTime;
	AccessTime = StatDataVal.AccessTime;
	ModificationTime = StatDataVal.ModificationTime;
	FileSize = StatDataVal.FileSize;
	bIsDirectory = StatDataVal.bIsDirectory;
	bIsReadOnly = StatDataVal.bIsReadOnly;
	bIsValid = StatDataVal.bIsValid;

	bSelected = bSelectedV;
	Extension = FPaths::GetExtension(NameVal).ToLower();
	ListSegment = CalcSegment(*this);
	CleanName = FPaths::GetCleanFilename(Name);
	
	bShouldShow = true;
	SpecialAction=FDlUI_FileExplorer_Lib::ESpecialAction::None;
}

FDlUI_FileExplorer_ItemInfo::FDlUI_FileExplorer_ItemInfo()

{
	FileSize=0;
	bIsDirectory=false;
	bIsReadOnly=false;
	bIsValid=false;
	bSelected = false;
	bShouldShow = true;
	SpecialAction=FDlUI_FileExplorer_Lib::ESpecialAction::None;
}

EDlUI_FileExplorer_ListSegment FDlUI_FileExplorer_ItemInfo::CalcSegment(const FDlUI_FileExplorer_ItemInfo& Info)
{
	if (Info.bIsDirectory)
	{
		return EDlUI_FileExplorer_ListSegment::Directory;
	}
	else
	{
		return EDlUI_FileExplorer_ListSegment::File;
	}
}

EDlUI_FileExplorer_ListSegment FDlUI_FileExplorer_ItemInfo::CalcSegment(const FFileStatData& StatDataVal)
{
	if (StatDataVal.bIsDirectory)
	{
		return EDlUI_FileExplorer_ListSegment::Directory;
	}
	else
	{
		return EDlUI_FileExplorer_ListSegment::File;
	}
}
