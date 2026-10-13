// This file is Copyright (C) 2025 Overcrowd Development Engineering Ltd, all rights reserved
#pragma once

#include "CoreMinimal.h"

#include "Layout/Visibility.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Input/SComboBox.h"
#include "GameplayTagContainer.h"
#include "OverCrowdDataAsset.h"
#include "VatAnimationWidget.h"
#include "Blueprint/UserWidget.h"
#include "VatAssetArrayWidget.h"
#include "VatAssetPicker.generated.h"


class SVatAssetPicker : public SCompoundWidget
{
public:
	
SLATE_BEGIN_ARGS(SVatAssetPicker){}
	SLATE_ARGUMENT( TArray<UVatMeshPairing*>, InVatAssetArray )
	SLATE_ARGUMENT(float, InScale)
	SLATE_ARGUMENT(bool, InHideVatAnim)
	SLATE_ARGUMENT(FGameplayTag, InWardrobeID)
	SLATE_EVENT(FOnSelected, OnSelected)
SLATE_END_ARGS()

	TSharedPtr<SVerticalBox> ContentBox;
	TSharedPtr<SVatAnimationWidget> VatAnimationWidget;

	FGameplayTag WardrobeID;
	
	UPROPERTY()
	UVatAnimationDataAsset* CurrentVatAnimation = nullptr;
	
	UPROPERTY()
	UVatAnimationDataAsset* CurrentAdditiveVatAnimation = nullptr;
	
	UPROPERTY()
	TArray<TSharedPtr<SVatAssetArrayWidget> > VatAssetWidgetArray;
	
	UPROPERTY()
	TArray<UVatMeshPairing*> VatAssets;

	UPROPERTY()
	UVatMeshPairing* UserSelectedVatPairing = nullptr;

	TArray<UVatMeshPairing*> CachedSelected;

	void GetSelected(TArray<UVatMeshPairing*> &VatAssets);
	void SetSelected(const TArray<UVatMeshPairing*> &VatAssets);
	void SetSelectedWithSkeleton(USkeleton* Skeleton);
	
	float ThumbnailScale = 1.0f;
	bool bSelectedHaveSameSkel = false;
	
	void Construct(const FArguments& InArgs);

	void RefreshShowVatAnimation();
	
	void Refresh();

	FButtonStyle TransparentStyle;
	FOnSelected OnSelected;

};


DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnVatAssetSelected, UVatMeshPairing*, Asset,  bool, IsSelected);

UCLASS(Blueprintable, BlueprintType)
class OVERCROWDEDITOR_API UVatAssetPicker : public UUserWidget
{
	GENERATED_BODY()

	FGameplayTag WardrobeID;
public:
	virtual void ReleaseSlateResources(bool bReleaseChildren) override;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ExposeOnSpawn=true), Category = "Overcrowd")
	TArray<UVatMeshPairing*> VatObjects;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ExposeOnSpawn=true), Category = "Overcrowd")
	float ThumbnailScale = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ExposeOnSpawn=true), Category = "Overcrowd")
	bool HideVatAnim = false;
	
	UFUNCTION(BlueprintCallable, Category = "Overcrowd")
	void Refresh(const TArray<UVatMeshPairing*> &VatAssets, FGameplayTag WardrobeID);

	UFUNCTION(BlueprintCallable, Category = "Overcrowd")
	void GetSelected(TArray<UVatMeshPairing*> &VatAssets);

	//UFUNCTION()
	//bool IsDefaultVatAnimation();
	
	//UFUNCTION()
	//void SelectAllWithSameSkeleton();

	//UFUNCTION()
	//void SetAsDefaultSkeleton();

	//UFUNCTION()
	//void RevertToDefaultSkeleton();
	
	UFUNCTION()
	void OnPropertyEdited(FName Name);
	
	UPROPERTY(BlueprintAssignable)
	FOnVatAssetSelected OnSelected;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation")
	UVatAnimationDataAsset* VatAnimDataAsset;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation")
	UVatAnimationDataAsset* AdditiveVatAnimDataAsset;
	
	
protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	
	TSharedPtr<SVatAssetPicker> VatAssetPicker;
};
