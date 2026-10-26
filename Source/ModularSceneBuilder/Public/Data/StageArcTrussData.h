// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Data/BaseItemData.h"
#include "StageArcTrussData.generated.h"

/**
 * Catalog entry for a curved box truss (AStageArcTrussActor; Docs/ADR/0018 §5.3): an arc of any radius and angle within the
 * asset's ranges, up to a full circle, built along the true curve. Specs (weight) are per rental curved SECTION: an arc counts
 * ⌈arc length ÷ SectionLength⌉ sections, as a straight span counts its segments.
 */
UCLASS(BlueprintType, Const)
class MODULARSCENEBUILDER_API UStageArcTrussData : public UBaseItemData
{
	GENERATED_BODY()

public:
	UStageArcTrussData();

	//~ Begin UObject Interface
#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(class FDataValidationContext& Context) const override;
#endif
	//~ End UObject Interface

	/** The ranges after ordering Min/Max and the absolute limits StageProceduralShapes builds (50..5000 cm, 5..360 degrees). */
	float GetMinRadius() const;
	float GetMaxRadius() const;
	float GetDefaultRadius() const;
	float GetMinAngle() const;
	float GetMaxAngle() const;
	float GetDefaultAngle() const;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arc", meta = (ClampMin = "50.0", ClampMax = "5000.0", Units = "cm"))
	float DefaultRadius = 400.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arc", meta = (ClampMin = "50.0", ClampMax = "5000.0", Units = "cm"))
	float MinRadius = 100.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arc", meta = (ClampMin = "50.0", ClampMax = "5000.0", Units = "cm"))
	float MaxRadius = 2000.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arc", meta = (ClampMin = "5.0", ClampMax = "360.0", Units = "deg"))
	float DefaultAngle = 90.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arc", meta = (ClampMin = "5.0", ClampMax = "360.0", Units = "deg"))
	float MinAngle = 15.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arc", meta = (ClampMin = "5.0", ClampMax = "360.0", Units = "deg"))
	float MaxAngle = 360.f;

	/** Length of one rental curved section along the centre line: the unit the specs and the bill of materials count. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arc", meta = (ClampMin = "50.0", ClampMax = "1000.0", Units = "cm"))
	float SectionLength = 200.f;

	/** Outside width of the square profile (F34: 29 cm). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arc", meta = (ClampMin = "10.0", ClampMax = "100.0", Units = "cm"))
	float ProfileWidth = 29.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arc", meta = (ClampMin = "1.0", ClampMax = "20.0", Units = "cm"))
	float ChordDiameter = 5.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arc", meta = (ClampMin = "0.5", ClampMax = "10.0", Units = "cm"))
	float BraceDiameter = 2.f;

	/** The inspector's step for Radius. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arc", meta = (ClampMin = "1.0", Units = "cm"))
	float RadiusStep = 25.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arc", meta = (AssetBundles = "Game"))
	TSoftObjectPtr<class UMaterialInterface> TrussMaterial;
};
