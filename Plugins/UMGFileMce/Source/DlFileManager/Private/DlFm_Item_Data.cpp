
// Copyright(c) 2022 Dragon Li Software Limited. All Rights Reserved.


#include "DlFm_Item_Data.h"


void UDlFm_Item_Data::Init(UDlFmMain_Logic* EnvVal, FDlUI_FileExplorer_ItemInfo* ItemVal, const int32 FixedIndexVal)
{
	Env=EnvVal;
	Item=ItemVal;
	FixedIndex=FixedIndexVal;
	bInit=true;
}

void UDlFm_Item_Data::UnInit()
{
	bInit=false;
}
