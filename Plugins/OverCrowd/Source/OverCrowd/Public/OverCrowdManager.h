// This file is Copyright (C) 2025 Overcrowd Development Engineering Ltd, all rights reserved

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "GameFramework/Actor.h"
#include "OverCrowdManager.generated.h"

UCLASS()
class OVERCROWD_API AOverCrowdManager : public AActor
{
	GENERATED_BODY()
	
public:	
	// Sets default values for this actor's properties
	AOverCrowdManager();

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "OverCrowd", meta = (ExposeOnSpawn))
	FGameplayTag PlacementGroup;

	
	bool bRegenerateNextTick = false;
	
	UFUNCTION(BlueprintNativeEvent, Category = "OverCrowd", BlueprintCallable, CallInEditor)
	void TriggerRegeneration();


protected:
	// Called when the game starts or when spawned
	virtual void BeginPlay() override;

public:	
	// Called every frame
	virtual void Tick(float DeltaTime) override;

};
