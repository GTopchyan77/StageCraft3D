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
#include "Components/Widget.h"
#include "Components/AssetThumbnailWidget.h"

//DECLARE_DELEGATE(FOnButtonClicked);


class SOverCrowdThumbnailWidget : public SCompoundWidget
{
	TSharedPtr<class FAssetThumbnailPool> ThumbnailPool;
public:
	SLATE_BEGIN_ARGS(SOverCrowdThumbnailWidget)
		{}
		SLATE_ARGUMENT(UObject*, InAsset)
		SLATE_ARGUMENT(float, InTextYOffset)
		SLATE_ARGUMENT(FText, InToolTipText)
		SLATE_ARGUMENT(float, InScale)
		SLATE_ARGUMENT(FString, InName)
		SLATE_ARGUMENT(ETextJustify::Type, InJustify)
		SLATE_EVENT(FOnClicked, OnClicked)
	SLATE_END_ARGS()

	UPROPERTY()
	FText ToolTipText;//= "(Ctrl + LeftMouse) to select multiple";
	
	UPROPERTY()
	UAssetThumbnailWidget* ThumbnailWidget;

	UPROPERTY()
	UObject* Object;

	UPROPERTY()
	float ThumbnailScale = 1.0f;

	UPROPERTY()
	float TextYOffset = 0;

	UPROPERTY()
	ETextJustify::Type Justify = ETextJustify::Center;

	UPROPERTY()
	bool bIsHovering = false;
	 
	//FSlateBrush BackgroundBrush;
	FButtonStyle TransparentButtonStyle;
	
	void Construct(const FArguments& InArgs);

	FOnClicked OnClicked;
};
