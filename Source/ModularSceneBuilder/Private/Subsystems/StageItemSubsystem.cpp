// Copyright Epic Games, Inc. All Rights Reserved.

#include "Subsystems/StageItemSubsystem.h"

#include "Data/BaseItemData.h"
#include "Engine/AssetManager.h"
#include "ModularSceneBuilder.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageItemSubsystem)

void UStageItemSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// In the editor the asset registry may still be scanning when PIE starts; querying early would return a partial catalog.
	UAssetManager::CallOrRegister_OnCompletedInitialScan(FSimpleMulticastDelegate::FDelegate::CreateUObject(this, &ThisClass::LoadCatalog));
}

void UStageItemSubsystem::Deinitialize()
{
	OnCatalogLoaded.Clear();
	OnSelectedItemChanged.Clear();
	Catalog.Reset();
	SelectedItem = nullptr;
	PendingSelection = nullptr;
	bCatalogLoaded = false;

	Super::Deinitialize();
}

TArray<UBaseItemData*> UStageItemSubsystem::GetCatalogByCategory(FGameplayTag Category) const
{
	TArray<UBaseItemData*> Result;
	for (UBaseItemData* Item : Catalog)
	{
		if (Item->CategoryTag.MatchesTag(Category))
		{
			Result.Add(Item);
		}
	}
	return Result;
}

void UStageItemSubsystem::SelectItem(UBaseItemData* Item)
{
	if (!Item)
	{
		ClearSelection();
		return;
	}

	if (Item == SelectedItem && !PendingSelection)
	{
		return;
	}

	PendingSelection = Item;

	UAssetManager& AssetManager = UAssetManager::Get();
	const FPrimaryAssetId ItemId = Item->GetPrimaryAssetId();

	if (!AssetManager.GetPrimaryAssetPath(ItemId).IsValid())
	{
		// Not under a scanned directory: still usable, but spawn assets will load synchronously on first spawn.
		UE_LOG(LogStageCraft, Warning, TEXT("Stage item %s is not registered with the Asset Manager; move it under /Game/StageCraft/Items."), *GetNameSafe(Item));
		HandleSelectionLoaded(Item);
		return;
	}

	// The delegate is invoked even when there is nothing to load, possibly synchronously, so PendingSelection must be set beforehand.
	AssetManager.LoadPrimaryAsset(ItemId, { UBaseItemData::GameBundle },
		FStreamableDelegate::CreateUObject(this, &ThisClass::HandleSelectionLoaded, TWeakObjectPtr<UBaseItemData>(Item)));
}

void UStageItemSubsystem::ClearSelection()
{
	PendingSelection = nullptr;
	CommitSelection(nullptr);
}

void UStageItemSubsystem::LoadCatalog()
{
	UAssetManager& AssetManager = UAssetManager::Get();

	TArray<FPrimaryAssetId> ItemIds;
	AssetManager.GetPrimaryAssetIdList(UBaseItemData::StageItemAssetType, ItemIds);

	if (ItemIds.IsEmpty())
	{
		UE_LOG(LogStageCraft, Warning, TEXT("No stage items found. Check the StageItem entry under AssetManagerSettings in DefaultGame.ini."));
	}

	// Only the UI bundle: icons and metadata for the catalog, without pulling every mesh into memory.
	AssetManager.LoadPrimaryAssets(ItemIds, { UBaseItemData::UIBundle },
		FStreamableDelegate::CreateUObject(this, &ThisClass::HandleCatalogLoaded));
}

void UStageItemSubsystem::HandleCatalogLoaded()
{
	UAssetManager& AssetManager = UAssetManager::Get();

	TArray<UObject*> LoadedObjects;
	AssetManager.GetPrimaryAssetObjectList(UBaseItemData::StageItemAssetType, LoadedObjects);

	Catalog.Reset(LoadedObjects.Num());
	for (UObject* Object : LoadedObjects)
	{
		if (UBaseItemData* Item = Cast<UBaseItemData>(Object))
		{
			Catalog.Add(Item);
		}
	}

	Catalog.Sort([](const UBaseItemData& A, const UBaseItemData& B)
	{
		return A.DisplayName.CompareTo(B.DisplayName) < 0;
	});

	bCatalogLoaded = true;
	UE_LOG(LogStageCraft, Log, TEXT("Stage item catalog loaded: %d items."), Catalog.Num());

	OnCatalogLoaded.Broadcast();
}

void UStageItemSubsystem::HandleSelectionLoaded(TWeakObjectPtr<UBaseItemData> LoadedItem)
{
	// A newer SelectItem/ClearSelection call wins over a load that finished late.
	if (!LoadedItem.IsValid() || LoadedItem.Get() != PendingSelection)
	{
		return;
	}

	PendingSelection = nullptr;
	CommitSelection(LoadedItem.Get());
}

void UStageItemSubsystem::CommitSelection(UBaseItemData* NewItem)
{
	if (NewItem == SelectedItem)
	{
		return;
	}

	UBaseItemData* PreviousItem = SelectedItem;
	SelectedItem = NewItem;
	OnSelectedItemChanged.Broadcast(SelectedItem, PreviousItem);
}
