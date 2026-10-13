// This file is Copyright (C) 2025 Overcrowd Development Engineering Ltd, all rights reserved


#include "PopulateScene.h"

// Sets default values
APopulateScene::APopulateScene()
{
 	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;

}

// Called when the game starts or when spawned
void APopulateScene::BeginPlay()
{
	Super::BeginPlay();
	
}

// Called every frame
void APopulateScene::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

}

void APopulateScene::InitQuadTree(FBox2D Extent)
{
}

void APopulateScene::RemoveAllAttached()
{
	TArray<AActor*> AttachedActors;
	GetAttachedActors(AttachedActors);

	for(int32 i = 0; i < AttachedActors.Num(); i++)
	{
		AttachedActors[i]->Destroy(true);
	}
}

TArray<FVector> APopulateScene::RemoveLocationsThatIntersect(const TArray<FVector>& InLocations, float DistanceThreshold, FBox2D Extent)
{
	TArray<FVector> Out;

	float QuadTreeMinSize = 10;
	TQuadTree<FVector, 100> QuadTree(Extent, QuadTreeMinSize);
	for (FVector Location : InLocations)
	{
		TArray<FVector> Results;
		
		FBox2D Box(FVector2D(Location.X - DistanceThreshold, Location.Y - DistanceThreshold) , FVector2D(Location.X + DistanceThreshold, Location.Y + DistanceThreshold));

		QuadTree.GetElements(Box, Results);

		if (!Results.Num())
		{
			Out.Add(Location);
			QuadTree.Insert(Location, Box);
		}
	}


	return Out;
}
