// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Data/BaseItemData.h"
#include "People/StageAudienceMath.h"
#include "StageAudienceData.generated.h"

/**
 * Catalog entry for an audience area (AStageAudienceActor; Docs/ADR/0031 §4): a rectangle of floor filled with an animated crowd
 * of procedural people, at a density in people per m². The ranges here bound what the inspector and saved scenes may set.
 */
UCLASS(BlueprintType, Const)
class MODULARSCENEBUILDER_API UStageAudienceData : public UBaseItemData
{
	GENERATED_BODY()

public:
	UStageAudienceData();

	//~ Begin UObject Interface
#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(class FDataValidationContext& Context) const override;
#endif
	//~ End UObject Interface

	/** The size range after sanitizing (at least 1 m, MinSize <= MaxSize), in cm. */
	double GetMinSize() const;
	double GetMaxSize() const;

	/** The density range after sanitizing, in people per m² (within 0.05 .. StageAudienceMath::MaxDensity). */
	double GetMinDensity() const;
	double GetMaxDensity() const;

	/** Width (across the stage) by depth (away from it) of an area placed from the Library, in cm. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Audience", meta = (Units = "cm"))
	FVector2D DefaultSize = FVector2D(1600.0, 1000.0);

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Audience", meta = (ClampMin = "100.0", Units = "cm"))
	double MinSize = 200.0;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Audience", meta = (ClampMin = "100.0", Units = "cm"))
	double MaxSize = 8000.0;

	/** People per m². About 1 for a relaxed crowd, 2 to 3 for a concert floor, 4 packed at the barrier. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Audience", meta = (ClampMin = "0.05", ClampMax = "5.0"))
	double DefaultDensity = 2.0;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Audience", meta = (ClampMin = "0.05", ClampMax = "5.0"))
	double MinDensity = 0.25;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Audience", meta = (ClampMin = "0.05", ClampMax = "5.0"))
	double MaxDensity = 4.0;

	/** At most this many people in one area, whatever its size and density (the GPU budget of Docs/ADR/0031 §6). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Audience", meta = (ClampMin = "1", ClampMax = "50000"))
	int32 MaxPeople = 20000;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Audience")
	EStageCrowdMood DefaultMood = EStageCrowdMood::Dancing;

	/** Shirt colours people are dressed in (linear). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Audience")
	TArray<FLinearColor> TopColors;

	/** The figure material (M_StageFigure). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Audience", meta = (AssetBundles = "Game"))
	TSoftObjectPtr<class UMaterialInterface> FigureMaterial;

	/** Past this distance from the camera, in cm, people are not drawn (0 = always). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Audience", meta = (ClampMin = "0.0", Units = "cm"))
	float CullDistance = 30000.f;
};
