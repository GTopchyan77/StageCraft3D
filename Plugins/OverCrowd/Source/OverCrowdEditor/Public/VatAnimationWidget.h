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
#include "Components/DetailsView.h"
#include "Components/SinglePropertyView.h"

#include "VatAnimationWidget.generated.h"


class UVatAssetPicker;


UCLASS(Blueprintable, BlueprintType)
class UCustomDetailsView : public UDetailsView
{
	GENERATED_BODY()
public:
	 
	FOnPropertyValueChanged& GetPropertyChangedEvent(){ return OnPropertyChanged;}
};



UCLASS(Blueprintable, BlueprintType)
class UCustomSinglePropertyView : public USinglePropertyView
{
	GENERATED_BODY()
public:
	 
	FOnPropertyValueChanged& GetPropertyChangedEvent(){ return OnPropertyChanged;}
};

class SVatAnimationWidget : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SVatAnimationWidget)
	{}
		SLATE_ARGUMENT(FGameplayTag, InWardrobeID)
	//SLATE_ARGUMENT( UVatAnimationDataAsset*, InVatAnimation )
	SLATE_ARGUMENT(float, ThumbnailScale)
	
SLATE_END_ARGS()

	FButtonStyle AddButtonStyle;
	FButtonStyle MakeDefaultSkeletonButtonStyle;
	FButtonStyle RevertToDefaultSkeletonButtonStyle;
	FButtonStyle SelectSkeletonButtonStyle;

	FGameplayTag WardrobeID;
	
	UPROPERTY()
	UCustomSinglePropertyView* VatAnimPropertyView;

	UPROPERTY()
	UCustomSinglePropertyView* AnimToBakePropertyView;

	UPROPERTY()
	UCustomSinglePropertyView* AdditiveRootBoneNameView;

	
	void SetAssetPicker(UVatAssetPicker* VatAssetPicker);
	
	float ThumbnailScale = 1.0f;

	void SetVatAnimationDataAsset( UVatAnimationDataAsset* VatAnimation );

	void Refresh();
	
	TSharedPtr<SHorizontalBox> ContentBox;
	
	void Construct(const FArguments& InArgs);
	
protected:
	//UPROPERTY()
	//UVatAnimationContainer *VatAnimContainer;
	
	UVatAssetPicker* VatAssetPicker = nullptr;
};
