// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "Data/StageItemTypes.h"
#include "BaseItemData.generated.h"

/**
 * Catalog entry for anything the user can place on stage: props, stage elements and lights.
 *
 * Adding a new item is a content-only task: create a data asset of this class under
 * /Game/StageCraft/Items and the Asset Manager picks it up (see [/Script/Engine.AssetManagerSettings]
 * in DefaultGame.ini). All heavy references are soft and split into asset bundles so the catalog UI
 * can load thousands of entries by icon ("UI") without pulling meshes into memory ("Game").
 */
UCLASS(BlueprintType, Const)
class MODULARSCENEBUILDER_API UBaseItemData : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UBaseItemData();

	/** Primary asset type used by the Asset Manager to discover every catalog entry. */
	static const FPrimaryAssetType StageItemAssetType;

	static const FName UIBundle;
	static const FName GameBundle;

	//~ Begin UObject Interface
	virtual FPrimaryAssetId GetPrimaryAssetId() const override;
#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(class FDataValidationContext& Context) const override;
#endif
	//~ End UObject Interface

	UFUNCTION(BlueprintPure, Category = "Stage Item")
	bool IsLight() const { return ItemType == EStageItemType::Light; }

	// --- Presentation ---

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Display")
	FText DisplayName;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Display", meta = (MultiLine = true))
	FText Description;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Display", meta = (AssetBundles = "UI"))
	TSoftObjectPtr<class UTexture2D> Icon;

	/** Catalog grouping for UI tabs and filters. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Display", meta = (Categories = "StageCraft.Category"))
	FGameplayTag CategoryTag;

	// --- Spawning ---

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Spawning")
	EStageItemType ItemType = EStageItemType::Prop;

	/**
	 * Actor spawned into the scene. Simple props keep the default AModularBaseActor and set Mesh;
	 * items with behaviour (e.g. lights) point at a subclass or Blueprint.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Spawning", meta = (AssetBundles = "Game"))
	TSoftClassPtr<class AModularBaseActor> ActorClass;

	/**
	 * Optional mesh override pushed onto the spawned actor. Lets many simple props share one
	 * generic actor class instead of needing a Blueprint per mesh.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Spawning", meta = (AssetBundles = "Game"))
	TSoftObjectPtr<class UStaticMesh> Mesh;

	/** Per-slot material overrides applied together with Mesh. Empty entries keep the mesh's own material. Ignored when Mesh is not set. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Spawning", meta = (AssetBundles = "Game"))
	TArray<TSoftObjectPtr<class UMaterialInterface>> MaterialOverrides;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Spawning", meta = (ShowOnlyInnerProperties))
	FStageItemPlacementRules PlacementRules;

	// --- Equipment ---

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Specs", meta = (ShowOnlyInnerProperties))
	FStageEquipmentSpecs Specs;

	// --- Economy (UStageEconomySubsystem) ---

	/** Needed to place this item or switch an instance to it. Empty = free. A UStageProductData sells it. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Economy", meta = (Categories = "StageCraft.Entitlement"))
	FGameplayTag RequiredEntitlement;

	/**
	 * Parameters of this item that need an entitlement to be edited (parameter id -> entitlement),
	 * e.g. StageCraft.Attribute.Zoom -> StageCraft.Entitlement.Feature.ZoomOptics. Unlisted parameters are free.
	 * Locked parameters still show their value; the inspector marks them read-only.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Economy", meta = (ForceInlineRow))
	TMap<FGameplayTag, FGameplayTag> ParameterEntitlements;
};
