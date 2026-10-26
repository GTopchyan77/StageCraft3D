// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Data/BaseItemData.h"
#include "StageStairsData.generated.h"

/**
 * Catalog entry for procedural stage stairs (AStageStairsActor; Docs/ADR/0018 §5.2): equal steps up to a deck, any rise and
 * width within the asset's ranges, optional handrails. One stairs unit is one item for the specs and the bill of materials.
 */
UCLASS(BlueprintType, Const)
class MODULARSCENEBUILDER_API UStageStairsData : public UBaseItemData
{
	GENERATED_BODY()

public:
	UStageStairsData();

	//~ Begin UObject Interface
#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(class FDataValidationContext& Context) const override;
#endif
	//~ End UObject Interface

	/** The ranges after ordering Min/Max and the absolute limits StageProceduralShapes builds (10..300 cm rise, 40..600 cm width). */
	float GetMinRise() const;
	float GetMaxRise() const;
	float GetMinWidth() const;
	float GetMaxWidth() const;
	float GetDefaultRise() const;
	float GetDefaultWidth() const;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stairs", meta = (ClampMin = "10.0", ClampMax = "300.0", Units = "cm"))
	float DefaultRise = 60.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stairs", meta = (ClampMin = "10.0", ClampMax = "300.0", Units = "cm"))
	float MinRise = 20.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stairs", meta = (ClampMin = "10.0", ClampMax = "300.0", Units = "cm"))
	float MaxRise = 200.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stairs", meta = (ClampMin = "40.0", ClampMax = "600.0", Units = "cm"))
	float DefaultWidth = 120.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stairs", meta = (ClampMin = "40.0", ClampMax = "600.0", Units = "cm"))
	float MinWidth = 60.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stairs", meta = (ClampMin = "40.0", ClampMax = "600.0", Units = "cm"))
	float MaxWidth = 400.f;

	/** Depth of each tread. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stairs", meta = (ClampMin = "20.0", ClampMax = "60.0", Units = "cm"))
	float TreadDepth = 30.f;

	/** No step is taller than this; the rise is divided into equal steps. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stairs", meta = (ClampMin = "10.0", ClampMax = "25.0", Units = "cm"))
	float MaxStepRise = 20.f;

	/** The inspector's step for Rise and Width. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stairs", meta = (ClampMin = "1.0", Units = "cm"))
	float SizeStep = 10.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stairs")
	bool bDefaultHandrails = true;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stairs", meta = (AssetBundles = "Game"))
	TSoftObjectPtr<class UMaterialInterface> StepMaterial;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stairs", meta = (AssetBundles = "Game"))
	TSoftObjectPtr<class UMaterialInterface> HandrailMaterial;
};
