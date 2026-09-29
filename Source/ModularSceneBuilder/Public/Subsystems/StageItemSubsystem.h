// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "GameplayTagContainer.h"
#include "StageItemSubsystem.generated.h"

class UBaseItemData;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnStageItemCatalogLoaded);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnStageItemSelectionChanged, UBaseItemData*, NewItem, UBaseItemData*, PreviousItem);

/**
 * Single source of truth for the item catalog and the item the user is about to place.
 *
 * Lives on the GameInstance so the selection survives level travel, and is a subsystem rather
 * than a UGameInstance subclass so it composes with any GameInstance class. Consumers (catalog UI,
 * spawn component) never poll: they read the current state once and bind to the delegates.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageItemSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem Interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	// --- Catalog ---

	UFUNCTION(BlueprintPure, Category = "StageCraft|Catalog")
	bool IsCatalogLoaded() const { return bCatalogLoaded; }

	/** Every discovered item, sorted by display name. Empty until OnCatalogLoaded fires. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Catalog")
	const TArray<UBaseItemData*>& GetCatalog() const { return ObjectPtrDecay(Catalog); }

	/** Items whose CategoryTag matches (or is a child of) the given tag. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Catalog")
	TArray<UBaseItemData*> GetCatalogByCategory(FGameplayTag Category) const;

	/** Fires once when the catalog finishes loading. Check IsCatalogLoaded() first to avoid missing it. */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Catalog")
	FOnStageItemCatalogLoaded OnCatalogLoaded;

	// --- Selection ---

	/**
	 * Requests a new active item. The item's spawn assets ("Game" bundle) are streamed in first,
	 * so OnSelectedItemChanged only fires once the item is ready to spawn without a hitch.
	 * A newer request supersedes one that is still loading.
	 */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Selection")
	void SelectItem(UBaseItemData* Item);

	UFUNCTION(BlueprintCallable, Category = "StageCraft|Selection")
	void ClearSelection();

	UFUNCTION(BlueprintPure, Category = "StageCraft|Selection")
	UBaseItemData* GetSelectedItem() const { return SelectedItem; }

	UFUNCTION(BlueprintPure, Category = "StageCraft|Selection")
	bool HasSelection() const { return SelectedItem != nullptr; }

	/** True while a requested item's assets are still streaming in. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Selection")
	bool IsSelectionPending() const { return PendingSelection != nullptr; }

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Selection")
	FOnStageItemSelectionChanged OnSelectedItemChanged;

private:
	void LoadCatalog();
	void HandleCatalogLoaded();
	void HandleSelectionLoaded(TWeakObjectPtr<UBaseItemData> LoadedItem);
	void CommitSelection(UBaseItemData* NewItem);

	UPROPERTY(Transient)
	TArray<TObjectPtr<UBaseItemData>> Catalog;

	UPROPERTY(Transient)
	TObjectPtr<UBaseItemData> SelectedItem;

	UPROPERTY(Transient)
	TObjectPtr<UBaseItemData> PendingSelection;

	bool bCatalogLoaded = false;
};
