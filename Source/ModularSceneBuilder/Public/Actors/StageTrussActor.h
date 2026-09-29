// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Actors/ModularBaseActor.h"
#include "StageTrussActor.generated.h"

/**
 * Placed truss piece or rigging hardware. Truss has no per-instance tuning (each length is its
 * own catalog item), so the inspector shows its specs read-only. Rigging points and connectors
 * are exposed in world space for the upcoming snap-to-truss and load calculation features.
 */
UCLASS(Blueprintable)
class MODULARSCENEBUILDER_API AStageTrussActor : public AModularBaseActor
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintPure, Category = "StageCraft|Truss")
	class UStageTrussData* GetTrussData() const;

	UFUNCTION(BlueprintCallable, Category = "StageCraft|Truss")
	TArray<FTransform> GetRiggingPointWorldTransforms() const;

	UFUNCTION(BlueprintCallable, Category = "StageCraft|Truss")
	TArray<FTransform> GetConnectorWorldTransforms() const;

protected:
	//~ Begin AModularBaseActor Interface
	virtual void GatherParameterSections(TArray<FStageParameterSection>& OutSections) const override;
	//~ End AModularBaseActor Interface
};
