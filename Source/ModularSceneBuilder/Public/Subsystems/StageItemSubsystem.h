// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "GameplayTagContainer.h"
#include "Startup/StageStartupTypes.h"
#include "StageItemSubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnStageItemCatalogLoaded);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnStageItemCatalogChanged);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnStageItemSelectionChanged, class UBaseItemData*, NewItem, class UBaseItemData*, PreviousItem);

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

	/** Load progress of the catalog in [0, 1]: 1 once loaded. For the startup loader (Docs/ADR/0024 §2). */
	float GetCatalogLoadProgress() const;

	/** Pushed while the catalog streams in, from its streamable handle. Not fired after OnCatalogLoaded. */
	FOnStageLoadProgress OnCatalogLoadProgress;

	/** Every discovered item, sorted by display name. Empty until OnCatalogLoaded fires. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Catalog")
	const TArray<class UBaseItemData*>& GetCatalog() const { return ObjectPtrDecay(Catalog); }

	/** Items whose CategoryTag matches (or is a child of) the given tag. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Catalog")
	TArray<class UBaseItemData*> GetCatalogByCategory(FGameplayTag Category) const;

	/** Fires once when the catalog finishes loading. Check IsCatalogLoaded() first to avoid missing it. */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Catalog")
	FOnStageItemCatalogLoaded OnCatalogLoaded;

	/** Fires when items are added after the initial load (a model imported at runtime). Views that list the catalog rebuild on it. */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Catalog")
	FOnStageItemCatalogChanged OnCatalogChanged;

	// --- Selection ---

	/**
	 * Requests a new active item. The item's spawn assets ("Game" bundle) are streamed in first,
	 * so OnSelectedItemChanged only fires once the item is ready to spawn without a hitch.
	 * A newer request supersedes one that is still loading.
	 */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Selection")
	void SelectItem(class UBaseItemData* Item);

	UFUNCTION(BlueprintCallable, Category = "StageCraft|Selection")
	void ClearSelection();

	UFUNCTION(BlueprintPure, Category = "StageCraft|Selection")
	class UBaseItemData* GetSelectedItem() const { return SelectedItem; }

	UFUNCTION(BlueprintPure, Category = "StageCraft|Selection")
	bool HasSelection() const { return SelectedItem != nullptr; }

	/** True while a requested item's assets are still streaming in. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Selection")
	bool IsSelectionPending() const { return PendingSelection != nullptr; }

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Selection")
	FOnStageItemSelectionChanged OnSelectedItemChanged;

private:
	// Imported models are added by the import subsystem only, after it has built and checked them.
	friend class UStageMeshImportSubsystem;
	// Device-library fixture types are added by the fixture library only, after it has parsed and built them (ADR 0033 §4.1).
	friend class UStageFixtureLibrarySubsystem;

	/**
	 * Adds an item built at runtime. It stays in the catalog for the rest of the run, also when the catalog (re)loads.
	 * Broadcasts OnCatalogChanged once the catalog has loaded; before that the item simply joins the initial load.
	 */
	void AddRuntimeItem(class UBaseItemData& Item);

	/** AddRuntimeItem for several items with one OnCatalogChanged (a fixture type's modes). Nulls and known items are skipped. */
	void AddRuntimeItems(TConstArrayView<class UBaseItemData*> Items);

	/**
	 * Takes runtime items out of the catalog (a fixture type removed or replaced), with one OnCatalogChanged. The selected item is
	 * cleared when it is one of them. Placed actors keep their item; only the catalog forgets it.
	 */
	void RemoveRuntimeItems(TConstArrayView<class UBaseItemData*> Items);
	void SortCatalog();

	void LoadCatalog();
	void HandleCatalogLoaded();
	void HandleSelectionLoaded(TWeakObjectPtr<class UBaseItemData> LoadedItem);
	void CommitSelection(class UBaseItemData* NewItem);

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UBaseItemData>> Catalog;

	/** Items added at runtime; merged into Catalog whenever it is built. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UBaseItemData>> RuntimeItems;

	UPROPERTY(Transient)
	TObjectPtr<class UBaseItemData> SelectedItem = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UBaseItemData> PendingSelection = nullptr;

	/** The catalog load in flight; reset once it completes. */
	TSharedPtr<struct FStreamableHandle> CatalogHandle;

	bool bCatalogLoaded = false;
};
