// This file is Copyright (C) 2025 Overcrowd Development Engineering Ltd, all rights reserved


#include "OverCrowdManager.h"

// Sets default values
AOverCrowdManager::AOverCrowdManager()
{
 	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;

	//PlacementGroup = FGameplayTag::RequestGameplayTag(FName("OverCrowd.PlacementGroup.Default"));
}

// Called when the game starts or when spawned
void AOverCrowdManager::BeginPlay()
{
	Super::BeginPlay();
	
}


void AOverCrowdManager::TriggerRegeneration_Implementation()
{
}

// Called every frame
void AOverCrowdManager::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (bRegenerateNextTick)
	{
		bRegenerateNextTick = false;
		TriggerRegeneration();
	}

}

