// Copyright DamenTools internal. Spline Route Tool plugin.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SplineRouteObstruction.generated.h"

class UStaticMeshComponent;
class UStaticMesh;

/**
 * A placed yard-layout obstruction — a shipping container, concrete barrier, steel-part stack, etc.
 * Purely a visual, space-blocking indicator: logistics planners drop these so they can sketch
 * vehicle/vessel routes around the physical clutter of the yard. No collision/pathing logic — routes
 * are authored manually (see "Out of Scope: collision avoidance").
 */
UCLASS(BlueprintType, Blueprintable)
class SPLINEROUTETOOL_API ASplineRouteObstruction : public AActor
{
	GENERATED_BODY()

public:
	ASplineRouteObstruction();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Spline Route|Obstruction")
	TObjectPtr<UStaticMeshComponent> MeshComponent;

	/** Catalog label this obstruction was placed from (e.g. "Container", "Concrete Barrier"). For serialization. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Spline Route|Obstruction")
	FName ObstructionTypeId;

	/** Catalog index this obstruction was placed from. Persisted by the scenario serializer. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Spline Route|Obstruction")
	int32 CatalogIndex = INDEX_NONE;

	/** Apply a mesh + scale at spawn. Falls back to the engine cube when InMesh is null. */
	UFUNCTION(BlueprintCallable, Category = "Spline Route|Obstruction")
	void Configure(UStaticMesh* InMesh, FVector InScale, FName InTypeId, int32 InCatalogIndex);
};
