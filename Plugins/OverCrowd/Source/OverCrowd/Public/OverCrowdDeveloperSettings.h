// This file is Copyright (C) 2025 Overcrowd Development Engineering Ltd, all rights reserved

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "OverCrowdDataAsset.h"
#include "OverCrowdDeveloperSettings.generated.h"

/**
 * 
 */
UCLASS(Config=Game, defaultconfig, meta = (DisplayName="Vat Crowd Settings"))
class OVERCROWD_API UOverCrowdDeveloperSettings : public UDeveloperSettings
{
	GENERATED_BODY()
public:
	UOverCrowdDeveloperSettings();

	static UOverCrowdDeveloperSettings* Get();
	
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "OverCrowd Settings", meta=(DisplayName="Auto Generated Data Assets Directory", LongPackageName))
	FDirectoryPath SaveDirectory = "/Game/OverCrowdData";

	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "OverCrowd Settings", meta=(LongPackageName))
	TSoftObjectPtr<UVatClothingMask> ClothingMask;

	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "OverCrowd Settings", meta=(LongPackageName))
	float IconScale = 0.6;

	//UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "OverCrowd Settings", meta=(LongPackageName))
	//TSoftObjectPtr<UAnimationDataAssetCollection> AnimationDataAssetCollection;
	
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "OverCrowd Settings", meta=(LongPackageName))
	TArray<TSoftObjectPtr<UWardrobeCollection> > WardrobeCollections;
	
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "OverCrowd Settings", meta=(DisplayName="Gameplay Tag for Clothing Mask", LongPackageName))
	FGameplayTag ClothingMaskTag;
	
//~ Begin UDeveloperSettings Interface
	virtual FName GetContainerName() const override;
	virtual FName GetCategoryName() const override;
	virtual FName GetSectionName() const override;

#if WITH_EDITOR
	virtual FText GetSectionText() const override;
	virtual FText GetSectionDescription() const override;

	
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;

	//void TriggerPropertyChange();
#endif
	// End of UDeveloperSettings
};
