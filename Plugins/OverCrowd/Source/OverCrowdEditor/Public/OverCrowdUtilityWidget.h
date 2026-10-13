// This file is Copyright (C) 2025 Overcrowd Development Engineering Ltd, all rights reserved

#pragma once

#include "CoreMinimal.h"
#include "EditorUtilityWidget.h"
#include "GameplayTagContainer.h"
#include "OverCrowdUtilityWidget.generated.h"



/**
 * 
 */
UCLASS()
class OVERCROWDEDITOR_API UOverCrowdUtilityWidget : public UEditorUtilityWidget
{
	GENERATED_BODY()
public:
	
	//TSharedRef<SWindow> CurrentWardrobeWindow;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wardrobe")
	TArray<FGameplayTag> WardrobeIDs;

	UFUNCTION(BlueprintCallable, meta = (Category = "OverCrowd"))
	void AddRemoveWardrobes(FGameplayTagContainer CurrentWardrobes);

	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, CallInEditor, Category = "OverCrowd")
	void GameplayTagSelected(FGameplayTagContainer NewSelection);

	UFUNCTION(BlueprintCallable, meta = (Category = "OverCrowd"))
	static UVatAnimationDataAsset* CreateAdditveVatAnimationDataAsset(UVatMeshPairing *VatMesh);

	UFUNCTION(BlueprintCallable, meta = (Category = "OverCrowd"))
	static UAnimationsToBakeDataAsset* CreateAdditveAnimToBakeDataAsset(UVatAnimationDataAsset *VatAnimAsset);

	UFUNCTION()
	void OnPropertyEdited(FName Name);


};
