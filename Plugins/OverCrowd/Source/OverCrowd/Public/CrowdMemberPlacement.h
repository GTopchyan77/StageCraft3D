// This file is Copyright (C) 2025 Overcrowd Development Engineering Ltd, all rights reserved

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "OverCrowdDataAsset.h"
#include "CrowdMemberPlacement.generated.h"

UCLASS()
class OVERCROWD_API ACrowdMemberPlacement : public AActor
{
	GENERATED_BODY()
	
public:	
	ACrowdMemberPlacement();

	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, CallInEditor, Category = "OverCrowd")
	void OnPropertyEdited(FName PropteryName);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OverCrowd")
	FCrowdMemberData CrowdMemberData;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "OverCrowd")
	FGameplayTag AnimationState;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "OverCrowd")
	FGameplayTag AdditiveAnimationState;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "OverCrowd", meta = (ExposeOnSpawn))
	FGameplayTag PlacementGroup;// = FGameplayTag::RequestGameplayTag(FName("OverCrowd.PlacementGroup.Default"));

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "OverCrowd")
	FGuid Guid;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "OverCrowd", meta = (ExposeOnSpawn))
	FColor WardrobeColor;
	
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "OverCrowd",meta = (ExposeOnSpawn))
	bool bPlacementVisibility = false;

	/** For PCG, this gets converted to WardrobeID GameplayTag */
	UPROPERTY(BlueprintReadWrite, meta = (ExposeOnSpawn = true), Category = "Overcrowd")
	FString WardrobeOverride;
	
	UPROPERTY(BlueprintReadWrite, meta = (ExposeOnSpawn = true), Category = "Overcrowd")
	FString AnimationOverride;

protected:
	// Called when the game starts or when spawned
	virtual void BeginPlay() override;

public:	
	// Called every frame
	virtual void Tick(float DeltaTime) override;

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	void OverrideMaterialParams(UPARAM(ref) TArray<FMaterialParamData>& Master, UPARAM(ref) TArray<FMaterialParamData>& Slave, TArray<FMaterialParamData>& OverriddenParams);

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	void SetCrowdMemberData(FCrowdMemberData InCrowdMemberData);

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	void GetCrowdMemberData(FCrowdMemberData& OutCrowdMemberData);

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	void RecalculateBitMasks(UVatClothingMask* ClothingMask);
	
};
