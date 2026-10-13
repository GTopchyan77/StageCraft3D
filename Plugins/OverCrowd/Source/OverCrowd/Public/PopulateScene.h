// This file is Copyright (C) 2025 Overcrowd Development Engineering Ltd, all rights reserved

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "GameFramework/Actor.h"
#include "GenericQuadTree.h"
#include "PopulateScene.generated.h"

UCLASS()
class OVERCROWD_API APopulateScene : public AActor
{
	GENERATED_BODY()
	
public:	
	// Sets default values for this actor's properties
	APopulateScene();

protected:
	// Called when the game starts or when spawned
	virtual void BeginPlay() override;

public:	
	// Called every frame
	virtual void Tick(float DeltaTime) override;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "OverCrowd", meta = (ExposeOnSpawn))
	FGameplayTag PlacementGroup = FGameplayTag::RequestGameplayTag(FName("OverCrowd.PlacementGroup.Default"));
	
	UFUNCTION(BlueprintCallable, meta = (Category = "OverCrowd"))
	TArray<FVector> RemoveLocationsThatIntersect(const TArray<FVector> &InLocations, float DistanceThreshold, FBox2D Extent);

	//UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OverCrowd")
	//TArray<FVector> Locations;

	UFUNCTION(BlueprintCallable, meta = (Category = "OverCrowd"))
	void InitQuadTree(FBox2D Extent);

	UFUNCTION(BlueprintCallable, meta = (Category = "OverCrowd"))
	void RemoveAllAttached();


};
