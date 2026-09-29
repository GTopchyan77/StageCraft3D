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
}
