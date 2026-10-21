// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Data/BaseItemData.h"
#include "StageDeckData.generated.h"

/**
 * Catalog entry for a stage deck or riser of any floor outline (AStageDeckActor; Docs/ADR/0015-sketch-to-stage.md §2.6): drawn as a
 * closed shape in the Sketch window, or placed from the Library with its default footprint.
 *
 * Specs (weight, power) are per PANEL: a deck counts its area divided by PanelSize, rounded up, so session limits and the bill of
 * materials count rental deck panels, as a span counts its sections.
 */
UCLASS(BlueprintType, Const)
class MODULARSCENEBUILDER_API UStageDeckData : public UBaseItemData
{
	GENERATED_BODY()

public:
	UStageDeckData();

	/** Lowest and highest deck any asset may allow, in cm. */
	static constexpr float AbsoluteMinHeight = 5.f;
	static constexpr float AbsoluteMaxHeight = 500.f;

	//~ Begin UObject Interface
#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(class FDataValidationContext& Context) const override;
#endif
	//~ End UObject Interface

	/** The height range after AbsoluteMinHeight / AbsoluteMaxHeight, so a badly authored asset still gives a usable range. */
	float GetMinHeight() const;
	float GetMaxHeight() const;

	/** DefaultHeight within the range. */
	float GetDefaultHeight() const;

	/** One panel's area in cm², at least 1. */
	double GetPanelArea() const;

	/** Footprint (X by Y) of a deck placed from the Library. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Deck", meta = (Units = "cm"))
	FVector2D DefaultSize = FVector2D(200.0, 100.0);

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Deck", meta = (ClampMin = "5.0", ClampMax = "500.0", Units = "cm"))
	float DefaultHeight = 60.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Deck", meta = (ClampMin = "5.0", ClampMax = "500.0", Units = "cm"))
	float MinHeight = 20.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Deck", meta = (ClampMin = "5.0", ClampMax = "500.0", Units = "cm"))
	float MaxHeight = 200.f;

	/** The inspector's Height step, in cm (the leg-height increments of a deck system). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Deck", meta = (ClampMin = "1.0", Units = "cm"))
	float HeightStep = 10.f;

	/** One rental panel (X by Y): the unit the specs and the bill of materials count. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Deck", meta = (Units = "cm"))
	FVector2D PanelSize = FVector2D(200.0, 100.0);

	/** The walking surface. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Deck", meta = (AssetBundles = "Game"))
	TSoftObjectPtr<class UMaterialInterface> SurfaceMaterial;

	/** The skirt around the sides. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Deck", meta = (AssetBundles = "Game"))
	TSoftObjectPtr<class UMaterialInterface> SkirtMaterial;
};
