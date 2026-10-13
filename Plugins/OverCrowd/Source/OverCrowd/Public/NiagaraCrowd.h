// This file is Copyright (C) 2025 Overcrowd Development Engineering Ltd, all rights reserved

#pragma once

#include "CoreMinimal.h"
#include "OverCrowdManager.h"
#include "GameFramework/Actor.h"
#include "NiagaraComponent.h"

#include "OverCrowdDataAsset.h"
#include "NiagaraCrowd.generated.h"

UCLASS()
class OVERCROWD_API ANiagaraCrowd : public AOverCrowdManager
{
	GENERATED_BODY()
	
public:	
	// Sets default values for this actor's properties
	ANiagaraCrowd();

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "OverCrowd")
	UNiagaraComponent* CrowdEmitter;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "OverCrowd")
	UTexture2D* CrowdDataTexture;
	
	UPROPERTY(BlueprintReadWrite, VisibleAnywhere, Category = "OverCrowd")
	TArray<FGameplayTag> WardrobeIndices;

#if WITH_EDITOR
	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static void GetPackageNamesForDuplicates (const TMap<FGameplayTag, FWardrobeVatAssetPairArray> &NewSortedBodyParts, TArray<FString>& Out_PackageNames);

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static bool IsSortedBodyPartsMapEqual (const TMap<FGameplayTag, FWardrobeVatAssetPairArray> &SortedBodyPartsA, const TMap<FGameplayTag, FWardrobeVatAssetPairArray> &SortedBodyPartsB);
 
#endif
	
	UPROPERTY(BlueprintReadWrite, VisibleAnywhere, Category = "OverCrowd")
	TMap<FGameplayTag, FWardrobeVatAssetPairArray> SortedBodyParts;
	
protected:
	// Called when the game starts or when spawned
	virtual void BeginPlay() override;

public:	
	// Called every frame
	virtual void Tick(float DeltaTime) override;

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	UPARAM(DisplayName = "IsSame") bool SetNewSortedBodyParts (const TMap<FGameplayTag, FWardrobeVatAssetPairArray> &NewSortedBodyParts);

};
