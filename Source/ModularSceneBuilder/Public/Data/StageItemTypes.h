// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "NativeGameplayTags.h"
#include "StageItemTypes.generated.h"

/**
 * Broad behavioural family of a stage item. Systems branch on this (e.g. lights expose a
 * color picker), so it stays a small closed enum. Open-ended grouping for UI filtering
 * belongs in UBaseItemData::CategoryTag instead, so designers can add categories without code.
 */
UENUM(BlueprintType)
enum class EStageItemType : uint8
{
	Prop			UMETA(DisplayName = "Prop"),
	StageElement	UMETA(DisplayName = "Stage Element"),
	Light			UMETA(DisplayName = "Light"),
	Truss			UMETA(DisplayName = "Truss / Rigging"),
	Audio			UMETA(DisplayName = "Audio"),
};

/** How the spawn system places an item while the placement input is held. */
UENUM(BlueprintType)
enum class EStageItemPlacementMode : uint8
{
	/** One instance per click. */
	Single			UMETA(DisplayName = "Single Click"),
	/** Keeps stamping instances onto free grid cells while the input is held. */
	Continuous		UMETA(DisplayName = "Continuous (Hold)"),
};

/** Grid snapping and surface placement rules consumed by the spawn system (Phase 3). */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageItemPlacementRules
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Placement")
	EStageItemPlacementMode PlacementMode = EStageItemPlacementMode::Continuous;

	/** Snap cell size in cm. Zero on an axis disables snapping on that axis. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Placement", meta = (ClampMin = "0.0", Units = "cm"))
	FVector GridSize = FVector(100.0, 100.0, 0.0);

	/** Applied after snapping, in the item's local space. Use it to lift pivots that are not at the base. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Placement", meta = (Units = "cm"))
	FVector PlacementOffset = FVector::ZeroVector;

	/** Rotate the item so its up axis follows the hit surface normal (e.g. wall-mounted screens). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Placement")
	bool bAlignToSurfaceNormal = false;
};

/**
 * Physical specs every piece of production gear shares. Weight and power feed rigging load and
 * power distribution calculations, so they live on the base catalog entry rather than per subclass.
 */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageEquipmentSpecs
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Specs")
	FText Manufacturer;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Specs", meta = (ClampMin = "0.0", Units = "kg"))
	float WeightKg = 0.f;

	/** Maximum power draw. Zero for passive gear (truss, passive speakers). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Specs", meta = (ClampMin = "0.0", DisplayName = "Power Draw (W)"))
	float PowerDrawWatts = 0.f;
};

/** Native tags so C++ can reference categories without string literals. Designers may add more in Project Settings. */
namespace StageCraftTags
{
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Category);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Category_Stage);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Category_Truss);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Category_Audio);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Category_Screen);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Category_Lighting);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Category_Prop);

	// Concert production sub-categories. Parents stay valid filters because GetCatalogByCategory matches children.
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Category_Stage_Deck);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Category_Stage_Riser);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Category_Truss_Straight);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Category_Truss_Corner);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Category_Truss_Tower);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Category_Rigging);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Category_Rigging_Motor);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Category_Lighting_MovingHead);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Category_Lighting_Par);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Category_Lighting_Strobe);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Category_Lighting_Laser);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Category_Audio_LineArray);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Category_Audio_Subwoofer);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Category_Audio_Monitor);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Category_Audio_Console);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Category_Audio_Amplifier);
}
