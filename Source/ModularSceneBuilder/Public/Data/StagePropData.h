// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Data/BaseItemData.h"
#include "Data/StageSpanData.h"
#include "People/StageFigureRig.h"
#include "StagePropData.generated.h"

/** One stand-in person of a prop item: a band member at their instrument, a technician at a desk (Docs/ADR/0031 §5). */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageFigurePlacement
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Figure")
	EStageFigureClip Clip = EStageFigureClip::Stagehand;

	/** Where the figure stands in the item, in cm: its origin is between its feet and it faces +X. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Figure")
	FTransform Transform;

	/** Shirt colour (linear). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Figure")
	FLinearColor TopColor = FLinearColor(0.03f, 0.03f, 0.035f);

	/** 0 light to 1 dark. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Figure", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float SkinTone = 0.3f;

	/** 0 blue denim to 1 black trousers. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Figure", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float BottomShade = 1.f;

	/** Speed of the clip against its design tempo. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Figure", meta = (ClampMin = "0.1", ClampMax = "4.0"))
	float RateScale = 1.f;

	/** Where in its loop the figure starts, so two figures with one clip do not move in step. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Figure", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Phase = 0.f;
};

/**
 * Catalog entry for an item assembled from parts and stand-in people (AStagePropActor; Docs/ADR/0031 §5): backline (a drum kit
 * with its drummer, an amplifier stack), crew positions (a lighting desk with its operator), set pieces (an LED backdrop).
 *
 * Parts are meshes placed in the item, drawn as instances (one draw batch per distinct mesh and material). Figures are the
 * procedural people of UStageFigureAssetSubsystem, animated on the GPU with FigureMaterial, so a whole band costs a handful of
 * draws and no tick. Specs (weight, power) are for the whole item.
 */
UCLASS(BlueprintType, Const)
class MODULARSCENEBUILDER_API UStagePropData : public UBaseItemData
{
	GENERATED_BODY()

public:
	UStagePropData();

	//~ Begin UObject Interface
#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(class FDataValidationContext& Context) const override;
#endif
	//~ End UObject Interface

	/** The item's meshes, placed relative to its origin (on the floor). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Prop", meta = (TitleProperty = "Mesh"))
	TArray<FStageSpanPart> Parts;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Prop", meta = (TitleProperty = "Clip"))
	TArray<FStageFigurePlacement> Figures;

	/** The figure material (M_StageFigure): required when there are figures. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Prop", meta = (AssetBundles = "Game"))
	TSoftObjectPtr<class UMaterialInterface> FigureMaterial;

	/** Performers and crew are few and near the stage, so they cast shadows; an item may turn that off. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Prop")
	bool bFiguresCastShadows = true;
};
