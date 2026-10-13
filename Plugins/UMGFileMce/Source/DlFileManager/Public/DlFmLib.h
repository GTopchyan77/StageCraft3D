// Copyright(c) 2022 Dragon Li Software Limited. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/FileManager.h"
#include "DlFmLib.generated.h"

namespace FDlUI_FileExplorer_Lib
{
	enum class ESpecialAction : uint8;
}

class UDlFmButton;
class UDlFmFilteredToggleButton;
class UDlFmMain_Logic;
class UDlFm_Item_Logic;
class UListView;
class UExpandableArea;
class UWidget;
class UDlFm_Item_Data;

//Namespace does not work with UENUM UCLASS, so they are put outside of FDlUI_FileExplorer_Lib
UENUM()
enum class EDlFm_SysPath:uint8
{
	Custom=0,
	UE_RootDir,
	UE_ProjectDir,
	UE_ProjectUserDir,
	UE_ProjectContentDir,
	UE_ProjectConfigDir,
	UE_ProjectSavedDir,
	UE_ProjectIntermediateDir,
	UE_ShaderWorkingDir,
	UE_ProjectPluginsDir,
	UE_ProjectModsDir,

};

UENUM()
enum class EDlUI_FileExplorer_ListSegment:uint8
{
	Directory=0,
	File=1,
	All_Num=2,
};
//FDlUI_FileExplorer_ItemInfo is used in Delegate, so it must be a USTRUCT
USTRUCT(BlueprintType)
struct FDlUI_FileExplorer_ItemInfo
{
	GENERATED_BODY()
public:
	UPROPERTY(BlueprintReadOnly, Category="Item Info")
	FString Name;
	
	UPROPERTY(BlueprintReadOnly, Category="Item Info")
	FDateTime CreationTime;
	UPROPERTY(BlueprintReadOnly, Category="Item Info")
	FDateTime AccessTime;
	UPROPERTY(BlueprintReadOnly, Category="Item Info")
	FDateTime ModificationTime;
	UPROPERTY(BlueprintReadOnly, Category="Item Info")
	int64 FileSize;
	UPROPERTY(BlueprintReadOnly, Category="Item Info")
	bool bIsDirectory;
	UPROPERTY(BlueprintReadOnly, Category="Item Info")
	bool bIsReadOnly ;
	UPROPERTY(BlueprintReadOnly, Category="Item Info")
	bool bIsValid;
	
	bool bSelected;
	bool bShouldShow;
	FDlUI_FileExplorer_Lib::ESpecialAction SpecialAction;
	FString CleanName;
	FString Extension;
	EDlUI_FileExplorer_ListSegment ListSegment=EDlUI_FileExplorer_ListSegment::File;

	FDlUI_FileExplorer_ItemInfo(const FString& NameVal, const FFileStatData& StatDataVal, const bool bSelectedV = false);

	FDlUI_FileExplorer_ItemInfo(const FString& NameVal, const bool bSelectedV = false);

	FDlUI_FileExplorer_ItemInfo();
	
	static EDlUI_FileExplorer_ListSegment CalcSegment(const FDlUI_FileExplorer_ItemInfo& Info);
	static EDlUI_FileExplorer_ListSegment CalcSegment(const FFileStatData& StatDataVal);
};
namespace FDlUI_FileExplorer_Lib
{
	enum class ESelectMode:uint8
	{
		Single,
		Multiply,
	};

	enum class EExploreMode:uint8
	{
		Browse,
		Search,
	};

	enum class ESpecialAction:uint8
	{
		None,
		Rename,
	};
	
	enum class EBundleActionType:uint8
	{
		None,
		CopyPaste,
		CutPaste,
		PrepareDelete,
		Rename,
	};
	
	struct FSegment;
	struct FCore;
	struct FBundleAction;

	class FPathStack
	{
		int32 StackTop = 0;
		int32 StackBottom = 0;
		int32 FlowPointer = 0;
		TArray<FString> Path_List;
		TArray<EExploreMode> ExploreMode_List;
		TArray<FString> SearchContent_List;
		UDlFmMain_Logic* Parent=nullptr;
		bool bAlwaysUseFullPath=true;
		FString FilterPath(const FString& Src) const;
	public:
		void Init(UDlFmMain_Logic* ParentV,bool bAlwaysUseFullPathVal);
		void InitStack(const FString& InitVal, const int32 Depth);
		void Push_AtFlowPointer(const FString& Val);
		void Push_AtFlowPointer(const FString& Val, const FString& SearchContent, const EExploreMode ExploreMode);
		bool IsFlowPointer_AtTop() const;
		bool IsFlowPointer_AtBottom() const;

		void FlowPoint_GoBack();
		void FlowPoint_GoForward();
		FString& FlowPoint_CurrentPath();
		FString FlowPoint_CurrentPath_WithSlash() const;
		EExploreMode FlowPoint_CurrentExploreMode();
		FString& FlowPoint_SearchContent(); 
		FString FlowPoint_SearchContent_AutoAppendAsterisk();
	};

	class FIterator_DirItemReader : public IPlatformFile::FDirectoryStatVisitor
	{
	public:
		//void Init(UDlUI_FileExplorer_Logic* ParentVal);
		void Init_ForRead(TArray<FSegment>* Segment_ListPtrVal);

		virtual bool Visit(const TCHAR* FilenameOrDirectory, const FFileStatData& StatData) override;
	private:
		TArray<FSegment>* Segment_ListPtr=nullptr;
	};

	class FIterator_DirPaste : public IPlatformFile::FDirectoryStatVisitor
	{
	public:
		//void Init(UDlUI_FileExplorer_Logic* ParentVal);
		void Init_ForCopy(FString* DesPath_WithSlashV,FString* SrcPath_WithSlashV,FBundleAction* BundleActionV,bool bOverwriteFileV=false);

		virtual bool Visit(const TCHAR* FilenameOrDirectory, const FFileStatData& StatData) override;
	private:
		bool bOverwriteFile=false;
		IFileManager& FileManager=IFileManager::Get();
		FString* DesPath_WithSlash=nullptr;
		FString* SrcPath_WithSlash=nullptr;
		FBundleAction* BundleAction=nullptr;
	};

	enum class ESortType:uint8
	{
		Name=0,
		Size=1,
		CreationTime=2,
		AccessTime=3,
		ModifyTime=4,
		FileType=5,
	};

	struct FSegment
	{
	public:
		TArray<FDlUI_FileExplorer_ItemInfo> DraftInfo_List;
		TArray<int32> DraftSort_List;
		TArray<UDlFm_Item_Data*> VisibleItem_List;

		void SetSelected(int32 InfoIndex, bool Val);
		bool SetSelected_ReverseCur(int32 InfoIndex);
		void SetSelected_StartRename(int32 InfoIndex);
		void SetSelected_AllVisible(bool Val);

		void InitClear(UListView* ContainerV, TArray<UDlFm_Item_Data*>* ItemPool_ListV,
												 UDlFmMain_Logic* ParentVal);
		void ClearInfo(); 
		void SetAndShowEmpty();

		void RefreshAllWidget_ModeStyle();
		void RefreshSort(const EDlUI_FileExplorer_ListSegment Segment,const ESortType SortType,const bool bReverse);
	private:
		UListView* Cache_Container = nullptr;
		UDlFmMain_Logic* Parent = nullptr;
		TArray<UDlFm_Item_Data*>* ItemPool_List=nullptr;

		static ESortType SortRemapConfig(const EDlUI_FileExplorer_ListSegment Segment,const ESortType SortType);
		void SortBy_Name(const bool bReverse);
		void SortBy_Size(const bool bReverse);
		void SortBy_CreationTime(const bool bReverse);
		void SortBy_AccessTime(const bool bReverse);
		void SortBy_ModifyTime(const bool bReverse);
		void SortBy_FileType(const bool bReverse);
		void SetItemPool_MinSize(int32 NewNum) const;
		void Internal_SortPrepare();
		void Internal_SortApplyReverse(const bool bReverse);
		void Internal_SetUIAfterSort();
	};

	struct FCore
	{
	public:
		bool SingleSelected_Valid=false;
		int32 SingleSelected_Index;
		EDlUI_FileExplorer_ListSegment SingleSelected_Segment;
		
		bool bShowFilter_Extension=false;
		TArray<FString> FilteredExtension_List;
		
		ESortType CurSortType;
		bool bReverseSort=false;
		int32 SelectedItem_Num=0;
		
		ESelectMode SelectMode = ESelectMode::Single;
		EExploreMode ExploreMode=EExploreMode::Browse;
		void Init(UDlFmMain_Logic* ParentVal, TArray<FSegment>* Segment_ListPtrVal,UDlFmFilteredToggleButton* Toggle_MultiSelectV,UDlFmFilteredToggleButton* Toggle_MultiSelect_SelectAllV,UWidget* Container_FunctionButtonV,FBundleAction* BundleActionV);
		void NewClick(const int32 IndexV, EDlUI_FileExplorer_ListSegment SegmentV);
		void SetSelect_All(bool Val);  
		void SetSelectMode(ESelectMode Val,bool bCleanSelectInfo=true); 
		void ClearCachedSelect_NoScanSegment_NoRefreshFunctionButton(); 
		void ClearCachedSelect(); 
		void Refresh_ShouldShow();
 
		void Single_SelectChange(int32 IndexVal,EDlUI_FileExplorer_ListSegment SegmentVal); 
		void Single_Confirm(int32 IndexVal,EDlUI_FileExplorer_ListSegment SegmentVal);
		void Multi_ReverseSelect(int32 IndexVal,EDlUI_FileExplorer_ListSegment SegmentVal); 
	private:
		UDlFmMain_Logic* Parent = nullptr;
		TArray<FSegment>* Segment_ListPtr=nullptr;
		UDlFmFilteredToggleButton* Toggle_MultiSelect = nullptr;
		UDlFmFilteredToggleButton* Toggle_MultiSelect_SelectAll = nullptr;
		UWidget* Container_FunctionButton=nullptr;
		FBundleAction* BundleAction=nullptr;
	};

	struct FBundleAction
	{
		EBundleActionType Type;
		TArray<FDlUI_FileExplorer_ItemInfo> SelectedBuffer;
		TArray<FDlUI_FileExplorer_ItemInfo> FailedBuffer;
		//Store additional information about failed item, e.g. already existed item during pasting.
		TArray<FDlUI_FileExplorer_ItemInfo> FailedBuffer_Info;
		void PrepareWork(EBundleActionType TypeVal,bool bCollectFromSegment=true);
		void DoWork_IterateSelected(TFunctionRef<bool (const FDlUI_FileExplorer_ItemInfo&, FBundleAction&)> FuncParam);
		void DoWork_IterateSelected_ParamString(TFunctionRef<bool (const FDlUI_FileExplorer_ItemInfo&, FBundleAction&, const FString&)> FuncParam, const FString& Param1);
		void Clear();
		void Init(UDlFmMain_Logic* ParentVal,TArray<FSegment>* Segment_ListPtrVal );
	private:
		UDlFmMain_Logic* Parent = nullptr;
		TArray<FSegment>* Segment_ListPtr=nullptr;
	};

}
