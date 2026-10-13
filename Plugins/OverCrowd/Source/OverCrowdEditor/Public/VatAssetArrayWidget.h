// This file is Copyright (C) 2025 Overcrowd Development Engineering Ltd, all rights reserved
#pragma once

#include "CoreMinimal.h"

#include "Layout/Visibility.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Input/SComboBox.h"
#include "GameplayTagContainer.h"
#include "OverCrowdDataAsset.h"
#include "Blueprint/UserWidget.h"
#include "VatAssetWidget.h"
//#include "VatAssetArrayWidget.generated.h"
/**
 *
 */
class SVatAssetArrayWidget : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SVatAssetArrayWidget)
	{}
	SLATE_ARGUMENT( TArray<UVatMeshPairing*>, InVatAssetArray )
	SLATE_ARGUMENT( TArray<UObject*>, InObjectArray )
	SLATE_ARGUMENT( TArray<FString>, InNameArray )
	SLATE_ARGUMENT(FGameplayTag, InWardrobeID)
	SLATE_ARGUMENT(float, InScale)
	SLATE_ARGUMENT(FString, InRowName)
	SLATE_EVENT(FOnSelected, OnSelected)
	
SLATE_END_ARGS()

	float ThumbnailScale = 1.0f;
	FString RowName;
	TArray<FString> NameArray;
	TSharedPtr<SHorizontalBox> ContentBox;
	FGameplayTag WardrobeID;

	TArray<TSharedPtr<SVatAssetWidget> > VatAssetWidgets;
	TArray<TSharedPtr<SOverCrowdThumbnailWidget> > AssetThumbnailWidgets; 
	
	void Construct(const FArguments& InArgs);
	
	void AddVatAsset(UVatMeshPairing* VatAsset, int NumTotal);
	void AddNormalAsset(UObject* Asset, int NumTotal, FString Name);

	void GetSelected(TArray<UVatMeshPairing*>& OutSelected);
	void SetSelected(const TArray<UVatMeshPairing*>& Selected);
	void SetSelectedWithSkeleton(USkeleton* Skeleton);

	FOnSelected OnSelected;
};
