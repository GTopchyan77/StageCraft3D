// This file is Copyright (C) 2025 Overcrowd Development Engineering Ltd, all rights reserved
#pragma once

#include "CoreMinimal.h"

#include "Layout/Visibility.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Input/SComboBox.h"
#include "GameplayTagContainer.h"
#include "OverCrowdDataAsset.h"
#include "OverCrowdThumbnail.h"
#include "Blueprint/UserWidget.h"
#include "Components/Widget.h"
#include "Components/AssetThumbnailWidget.h"

DECLARE_DELEGATE_TwoParams(FOnSelected, UVatMeshPairing*, bool /*bSelected*/);

class SVatAssetWidget : public SCompoundWidget
{
	TSharedPtr<class FAssetThumbnailPool> ThumbnailPool;
public:
	SLATE_BEGIN_ARGS(SVatAssetWidget)
		{}
		SLATE_ARGUMENT(UVatMeshPairing*, InAsset)
		SLATE_ARGUMENT(float, InScale)
		SLATE_ARGUMENT(FGameplayTag, InWardrobeID)
		SLATE_ARGUMENT(ETextJustify::Type, InJustify)
		SLATE_EVENT(FOnSelected, OnSelected)
	SLATE_END_ARGS()

	SOverCrowdThumbnailWidget ThumbnailWidget;

	FGameplayTag WardrobeID;
	
	UPROPERTY()
	UVatMeshPairing* VatObject;

	UPROPERTY()
	bool bSelected = false;

	UPROPERTY()
	bool bHighlighted = false;

	UPROPERTY()
	float ThumbnailScale = 1.0f;

	UPROPERTY()
	ETextJustify::Type Justify = ETextJustify::Center;

	FButtonStyle GotoVatAssetButtonStyle;
	FButtonStyle GotoSkelMeshButtonStyle;
	FButtonStyle GotoVatMeshButtonStyle;
	FButtonStyle GotoVatAnimButtonStyle;
	
	FSlateBrush BackgroundBrush;
	
	void Construct(const FArguments& InArgs);

	FOnSelected OnSelected;
};
