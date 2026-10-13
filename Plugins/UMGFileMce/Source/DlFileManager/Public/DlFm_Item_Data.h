// Copyright(c) 2022 Dragon Li Software Limited. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "DlFmLib.h"
#include "DlFm_Item_Data.generated.h"

class UDlFmMain_Logic;

UCLASS()
class DLFILEMANAGER_API UDlFm_Item_Data : public UObject
{
	GENERATED_BODY()
public:

	void Init(UDlFmMain_Logic* EnvVal,
			  FDlUI_FileExplorer_ItemInfo* ItemVal,
			  const int32 FixedIndexVal);
	void UnInit();
	
	bool bInit = false;
	UDlFmMain_Logic* Env;
	FDlUI_FileExplorer_ItemInfo* Item;
	int32 FixedIndex;
	//EDlUI_FileExplorer_ListSegment ListSegment;
};