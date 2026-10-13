// Copyright DamenTools internal. Precise Transform plugin.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "WarehousePlannerSaveGame.generated.h"

/** One placed object's class + world transform, as saved by UWarehousePlannerSaveLibrary::SaveWarehouseLayout. */
USTRUCT(BlueprintType)
struct FWarehousePlacedObjectSaveData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WarehousePlanner")
	TSoftClassPtr<AActor> ObjectClass;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WarehousePlanner")
	FVector Location = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WarehousePlanner")
	FRotator Rotation = FRotator::ZeroRotator;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WarehousePlanner")
	FVector Scale = FVector::OneVector;
};

/**
 * Basic Scenario Save/Load for the Warehouse Planner: a flat snapshot of every
 * currently placed object's class + world transform. Deliberately does NOT
 * capture BP_BuildManager's grid-occupancy state (OccupancyData/ObjectsData) -
 * see WarehousePlannerSaveLibrary.h for why re-deriving that on Load safely
 * would require replicating BP_ObjectPlacer_Base_Class's placement pipeline,
 * out of scope for this basic pass.
 */
UCLASS()
class PRECISETRANSFORM_API UWarehousePlannerSaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WarehousePlanner")
	TArray<FWarehousePlacedObjectSaveData> PlacedObjects;
};
